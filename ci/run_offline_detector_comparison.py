#!/usr/bin/env python3
"""Collect paired, provisional HOG/CNN measurements from synthetic 4K scenes.

The reports characterize the existing dlib backends on identical workloads.
They do not establish camera tracking quality or production acceptance.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
from pathlib import Path
import subprocess
import sys
import tempfile
from typing import Any


SCHEMA_VERSION = 4
FRAME_COUNT = 250
FRAME_STEP_MS = 40
MIN_DETECTOR_WORK_SAMPLES_PER_RUN = 3
SOURCE_WIDTH = 3840
SOURCE_HEIGHT = 2160
OUTPUT_WIDTH = 1920
OUTPUT_HEIGHT = 1080
DETECTION_SCALE = 2
DETECTION_SCALES = (2, 4)
FPS = 25
WORKLOADS = (
    (
        "single_face_center",
        "Synthetic 4K centered cartoon face; stride-2 and stride-4 detector inputs",
    ),
    (
        "three_face_stage",
        "Synthetic 4K three cartoon faces spread across stage width; stride-2 and stride-4 detector inputs",
    ),
)


class ComparisonError(ValueError):
    """Raised when the paired backend runs are not comparable."""


def _read_portrait(path: Path) -> tuple[int, int, bytes]:
    try:
        magic, dimensions, maximum, pixels = path.read_bytes().split(b"\n", 3)
    except (OSError, ValueError) as error:
        raise ComparisonError(f"could not read the synthetic portrait fixture: {error}") from error
    if magic.strip() != b"P6" or maximum.strip() != b"255":
        raise ComparisonError("the synthetic portrait must be an 8-bit P6 PPM")
    try:
        width, height = map(int, dimensions.split())
    except ValueError as error:
        raise ComparisonError("the synthetic portrait has an invalid PPM size") from error
    if width <= 0 or height <= 0 or len(pixels) != width * height * 3:
        raise ComparisonError("the synthetic portrait has an invalid pixel payload")
    return width, height, pixels


def _face_positions(portrait_path: Path, layout: str) -> tuple[tuple[int, int], ...]:
    portrait_width, portrait_height, _ = _read_portrait(portrait_path)
    scaled_width = portrait_width * DETECTION_SCALE
    scaled_height = portrait_height * DETECTION_SCALE
    center_y = (SOURCE_HEIGHT - scaled_height) // 2
    if layout == "single_face_center":
        return (((SOURCE_WIDTH - scaled_width) // 2, center_y),)
    if layout == "three_face_stage":
        return (
            (0, center_y),
            ((SOURCE_WIDTH - scaled_width) // 2, center_y),
            (SOURCE_WIDTH - scaled_width, center_y),
        )
    raise ComparisonError(f"unsupported synthetic scene layout: {layout}")


def _write_4k_scene(
    portrait_path: Path,
    destination: Path,
    face_positions: tuple[tuple[int, int], ...] | None = None,
) -> None:
    portrait_width, portrait_height, portrait = _read_portrait(portrait_path)
    scaled_width = portrait_width * DETECTION_SCALE
    scaled_height = portrait_height * DETECTION_SCALE
    if face_positions is None:
        face_positions = _face_positions(portrait_path, "single_face_center")
    if not face_positions:
        raise ComparisonError("synthetic scene must contain at least one portrait")
    if any(
        left < 0 or top < 0 or left + scaled_width > SOURCE_WIDTH or top + scaled_height > SOURCE_HEIGHT
        for left, top in face_positions
    ):
        raise ComparisonError("synthetic portrait position is outside the 4K source bounds")
    pixels = bytearray([32, 38, 42]) * (SOURCE_WIDTH * SOURCE_HEIGHT)

    for row in range(portrait_height):
        source_start = row * portrait_width * 3
        source_row = portrait[source_start : source_start + portrait_width * 3]
        scaled_row = bytearray()
        for column in range(portrait_width):
            pixel_start = column * 3
            scaled_row.extend(source_row[pixel_start : pixel_start + 3] * DETECTION_SCALE)
        for left, top in face_positions:
            for vertical_offset in range(DETECTION_SCALE):
                target_start = (
                    (top + row * DETECTION_SCALE + vertical_offset) * SOURCE_WIDTH + left
                ) * 3
                pixels[target_start : target_start + len(scaled_row)] = scaled_row

    destination.write_bytes(
        f"P6\n{SOURCE_WIDTH} {SOURCE_HEIGHT}\n255\n".encode("ascii") + pixels
    )


def _write_manifest(scene: Path, destination: Path) -> None:
    destination.write_text(
        "monotonic_ms\tframe_path\n"
        + "".join(
            f"{frame_index * FRAME_STEP_MS}\t{scene.name}\n"
            for frame_index in range(FRAME_COUNT)
        ),
        encoding="utf-8",
    )


def _artifact_metadata(path: Path) -> dict[str, Any]:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
        size_bytes = path.stat().st_size
    except OSError as error:
        raise ComparisonError(f"could not fingerprint benchmark input {path.name}: {error}") from error
    return {"filename": path.name, "size_bytes": size_bytes, "sha256": digest.hexdigest()}


def _input_artifacts(
    portrait: Path,
    scene: Path,
    layout: str,
    face_positions: tuple[tuple[int, int], ...],
) -> dict[str, Any]:
    return {
        "synthetic_portrait_fixture": _artifact_metadata(portrait),
        "generated_source_frame": _artifact_metadata(scene),
        "generation": {
            "source_resolution": [SOURCE_WIDTH, SOURCE_HEIGHT],
            "background_rgb": [32, 38, 42],
            "portrait_scale": DETECTION_SCALE,
            "layout": layout,
            "face_top_left_pixels": [list(position) for position in face_positions],
            "frames": FRAME_COUNT,
            "frame_interval_ms": FRAME_STEP_MS,
            "minimum_detector_work_samples_per_run": MIN_DETECTOR_WORK_SAMPLES_PER_RUN,
        },
    }


def _run_backend(
    benchmark: Path,
    binary: Path,
    model: Path,
    engine: str,
    manifest: Path,
    output_dir: Path,
    repeats: int,
    timeout_seconds: float,
    binary_source_revision: str | None,
    workload_label: str,
) -> dict[str, Any]:
    report_path = output_dir / f"{engine}.json"
    trace_dir = output_dir / "traces" / engine
    command = [
        sys.executable,
        str(benchmark),
        "--binary",
        str(binary),
        "--model",
        str(model),
        "--detector-engine",
        engine,
        "--manifest",
        str(manifest),
        "--output",
        str(report_path),
        "--scales",
        ",".join(str(scale) for scale in DETECTION_SCALES),
        "--repeats",
        str(repeats),
        "--fps",
        str(FPS),
        "--output-resolution",
        f"{OUTPUT_WIDTH}x{OUTPUT_HEIGHT}",
        "--workload-label",
        workload_label,
        "--timeout-seconds",
        str(timeout_seconds),
        "--trace-directory",
        str(trace_dir),
    ]
    if binary_source_revision:
        command.extend(("--binary-source-revision", binary_source_revision))
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds * repeats + 60,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired) as error:
        raise ComparisonError(f"{engine} benchmark could not complete: {error}") from error
    if completed.stdout:
        print(completed.stdout, end="")
    if completed.returncode != 0:
        if completed.stderr:
            print(completed.stderr, file=sys.stderr, end="")
        raise ComparisonError(f"{engine} benchmark exited with status {completed.returncode}")
    try:
        report = json.loads(report_path.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError) as error:
        raise ComparisonError(f"{engine} benchmark report is invalid: {error}") from error
    if not isinstance(report, dict):
        raise ComparisonError(f"{engine} benchmark report must be a JSON object")
    return report


def _paired_summary(
    hog: dict[str, Any], cnn: dict[str, Any], input_artifacts: dict[str, Any]
) -> dict[str, Any]:
    if hog.get("detector_engine") != "dlib_hog" or cnn.get("detector_engine") != "dlib_cnn":
        raise ComparisonError("benchmark reports do not identify the requested detector engines")
    if hog.get("production_acceptance_eligible") is not False or cnn.get("production_acceptance_eligible") is not False:
        raise ComparisonError("offline reports must remain ineligible for production acceptance")
    for field in ("host", "benchmark_tool_source_revision", "replay_binary", "workload", "manifest"):
        if field not in hog or field not in cnn:
            raise ComparisonError(f"backend reports are missing shared {field} metadata")
        if hog[field] != cnn[field]:
            raise ComparisonError(f"backend reports differ in the shared {field} metadata")
    if hog.get("workload", {}).get("source_resolution") != [SOURCE_WIDTH, SOURCE_HEIGHT]:
        raise ComparisonError("benchmark workload is not the defined synthetic 4K source")
    if hog.get("workload", {}).get("output_resolution") != [OUTPUT_WIDTH, OUTPUT_HEIGHT]:
        raise ComparisonError("benchmark workload does not use the defined 1080p output")
    hog_scales = hog.get("scales", [])
    cnn_scales = cnn.get("scales", [])
    expected_scales = list(DETECTION_SCALES)
    if not isinstance(hog_scales, list) or not isinstance(cnn_scales, list):
        raise ComparisonError("backend scale summaries must be arrays")
    if [scale.get("detection_scale") for scale in hog_scales] != expected_scales or [
        scale.get("detection_scale") for scale in cnn_scales
    ] != expected_scales:
        raise ComparisonError("backend reports do not use the same detector scale")

    for engine, scales in (("dlib_hog", hog_scales), ("dlib_cnn", cnn_scales)):
        for scale in scales:
            scale_label = f"{engine} scale {scale.get('detection_scale')}"
            summary = scale.get("summary")
            runs = summary.get("runs") if isinstance(summary, dict) else None
            repeats = summary.get("repeats") if isinstance(summary, dict) else None
            if (
                not isinstance(runs, list)
                or not runs
                or isinstance(repeats, bool)
                or not isinstance(repeats, int)
                or repeats != len(runs)
            ):
                raise ComparisonError(f"{scale_label} report has inconsistent run samples")
            sample_counts: list[int] = []
            for run in runs:
                samples = run.get("detector_work_samples_us") if isinstance(run, dict) else None
                if not isinstance(samples, list):
                    raise ComparisonError(f"{scale_label} report has invalid detector work samples")
                sample_counts.append(len(samples))
            if any(count < MIN_DETECTOR_WORK_SAMPLES_PER_RUN for count in sample_counts):
                raise ComparisonError(
                    f"{scale_label} report has fewer than "
                    f"{MIN_DETECTOR_WORK_SAMPLES_PER_RUN} detector work samples in a run"
                )
            if summary.get("detector_work_samples_count") != sum(sample_counts):
                raise ComparisonError(
                    f"{scale_label} aggregate sample count is inconsistent"
                )

    manifest = dict(hog["manifest"])
    manifest["hash_scope"] = (
        "manifest file bytes; referenced frame content hashes are listed separately in input_artifacts"
    )
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "evidence_scope": "offline_development_measurement",
        "production_acceptance_eligible": False,
        "comparison": "existing dlib HOG and CNN backends on the same synthetic workload and host",
        "latency_sample_contract": {
            "minimum_detector_work_samples_per_run": MIN_DETECTOR_WORK_SAMPLES_PER_RUN,
            "percentile_method": "nearest-rank",
            "note": "A sample-count gate improves trace coverage; it does not establish production performance thresholds.",
        },
        "host": hog["host"],
        "benchmark_tool_source_revision": hog["benchmark_tool_source_revision"],
        "replay_binary": hog["replay_binary"],
        "input_artifacts": input_artifacts,
        "manifest": manifest,
        "workload": hog["workload"],
        "models": {"dlib_hog": hog["model"], "dlib_cnn": cnn["model"]},
        "results": {"dlib_hog": hog_scales, "dlib_cnn": cnn_scales},
        "not_measured": [
            "real-camera detection or tracking quality",
            "live OBS capture, rendering, or encoding",
            "Windows production hardware performance",
            "unmodified upstream OBS-plugin comparison",
        ],
    }


def _workload_index_entry(
    layout: str, workload_label: str, paired: dict[str, Any]
) -> dict[str, Any]:
    result_fields = (
        "detector_work_median_us",
        "detector_work_p95_us",
        "max_face_detections",
        "max_tracking_candidates",
        "process_cpu_percent_median",
        "peak_resident_bytes_max",
    )
    results: dict[str, list[dict[str, Any]]] = {}
    for engine, scales in paired["results"].items():
        results[engine] = [
            {
                "detection_scale": scale["detection_scale"],
                **{
                    field: scale.get("summary", {}).get(field)
                    for field in result_fields
                },
            }
            for scale in scales
        ]
    return {
        "layout": layout,
        "label": workload_label,
        "report": f"{layout}/comparison.json",
        "input_artifacts": paired["input_artifacts"],
        "results": results,
    }


def _run_workload(
    benchmark: Path,
    binary: Path,
    hog_model: Path,
    cnn_model: Path,
    portrait: Path,
    temporary_dir: Path,
    output_dir: Path,
    layout: str,
    workload_label: str,
    repeats: int,
    timeout_seconds: float,
    binary_source_revision: str | None,
) -> dict[str, Any]:
    scene = temporary_dir / f"{layout}.ppm"
    manifest = temporary_dir / f"{layout}.tsv"
    positions = _face_positions(portrait, layout)
    _write_4k_scene(portrait, scene, positions)
    _write_manifest(scene, manifest)
    input_artifacts = _input_artifacts(portrait, scene, layout, positions)
    workload_output_dir = output_dir / layout
    workload_output_dir.mkdir()
    hog = _run_backend(
        benchmark,
        binary,
        hog_model,
        "dlib_hog",
        manifest,
        workload_output_dir,
        repeats,
        timeout_seconds,
        binary_source_revision,
        workload_label,
    )
    cnn = _run_backend(
        benchmark,
        binary,
        cnn_model,
        "dlib_cnn",
        manifest,
        workload_output_dir,
        repeats,
        timeout_seconds,
        binary_source_revision,
        workload_label,
    )
    if _input_artifacts(portrait, scene, layout, positions) != input_artifacts:
        raise ComparisonError(f"synthetic benchmark inputs changed during {layout} paired runs")
    paired = _paired_summary(hog, cnn, input_artifacts)
    (workload_output_dir / "comparison.json").write_text(
        json.dumps(paired, indent=2, sort_keys=True) + "\n", encoding="utf-8"
    )
    return _workload_index_entry(layout, workload_label, paired)


def _comparison_index(workloads: list[dict[str, Any]]) -> dict[str, Any]:
    return {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "evidence_scope": "offline_development_measurement",
        "production_acceptance_eligible": False,
        "comparison": "existing dlib HOG and CNN backends on paired synthetic workloads and the same host",
        "workloads": workloads,
        "not_measured": [
            "real-camera detection or tracking quality",
            "live OBS capture, rendering, or encoding",
            "Windows production hardware performance",
            "unmodified upstream OBS-plugin comparison",
        ],
    }


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="built offline session replay executable")
    parser.add_argument(
        "--hog-model",
        type=Path,
        default=Path("data/dlib_hog_model/frontal_face_detector.dat"),
    )
    parser.add_argument(
        "--cnn-model",
        type=Path,
        default=Path("data/dlib_cnn_model/mmod_human_face_detector.dat"),
    )
    parser.add_argument(
        "--portrait",
        type=Path,
        default=Path("tests/fixtures/offline-synthetic-person-320x360.ppm"),
    )
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument(
        "--binary-source-revision",
        help="CI checkout revision used to build the replay binary; recorded as operator-supplied",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    args = _build_parser().parse_args(argv)
    if not 1 <= args.repeats <= 10:
        print("--repeats must be from 1 through 10", file=sys.stderr)
        return 2
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        print("--timeout-seconds must be finite and greater than zero", file=sys.stderr)
        return 2

    root = Path(__file__).resolve().parents[1]
    binary = args.binary.expanduser().resolve()
    hog_model = args.hog_model.expanduser().resolve()
    cnn_model = args.cnn_model.expanduser().resolve()
    portrait = args.portrait.expanduser().resolve()
    output_dir = args.output_dir.expanduser().resolve()
    benchmark = root / "tools" / "faithfulframe_offline_benchmark.py"
    for path, label in (
        (binary, "offline replay executable"),
        (hog_model, "dlib HOG model"),
        (cnn_model, "dlib CNN model"),
        (portrait, "synthetic portrait fixture"),
        (benchmark, "offline benchmark tool"),
    ):
        if not path.is_file():
            print(f"{label} does not exist: {path}", file=sys.stderr)
            return 2
    output_dir.mkdir(parents=True, exist_ok=True)
    if any(output_dir.iterdir()):
        print(f"output directory is not empty: {output_dir}", file=sys.stderr)
        return 2

    try:
        workloads: list[dict[str, Any]] = []
        with tempfile.TemporaryDirectory(prefix="faithfulframe-4k-benchmark-") as temporary:
            temporary_dir = Path(temporary)
            for layout, workload_label in WORKLOADS:
                workloads.append(
                    _run_workload(
                        benchmark,
                        binary,
                        hog_model,
                        cnn_model,
                        portrait,
                        temporary_dir,
                        output_dir,
                        layout,
                        workload_label,
                        args.repeats,
                        args.timeout_seconds,
                        args.binary_source_revision,
                    )
                )
        index = _comparison_index(workloads)
        (output_dir / "comparison.json").write_text(
            json.dumps(index, indent=2, sort_keys=True) + "\n", encoding="utf-8"
        )
    except (ComparisonError, OSError, ValueError) as error:
        print(f"offline detector comparison failed: {error}", file=sys.stderr)
        return 1

    print(f"Wrote provisional offline detector comparison: {output_dir / 'comparison.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
