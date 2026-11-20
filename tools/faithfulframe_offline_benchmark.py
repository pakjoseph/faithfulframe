#!/usr/bin/env python3
"""Run comparable detector-scale sweeps with the production offline replay.

The report is a development measurement, not a production acceptance record.
It measures dlib HOG work, replay wall time, and process CPU/RSS for the offline
replay process. It does not measure live OBS capture, rendering, encoding, or
face-tracking quality on real camera footage.
"""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import tempfile
import time
from typing import Any


SCHEMA_VERSION = 2
MAX_DETECTION_SCALE = 16


class BenchmarkError(ValueError):
    """Raised when a replay or its trace cannot support a valid comparison."""


def parse_resolution(value: str) -> tuple[int, int]:
    parts = value.lower().split("x")
    if len(parts) != 2:
        raise argparse.ArgumentTypeError("resolution must be WIDTHxHEIGHT")
    try:
        width, height = (int(part) for part in parts)
    except ValueError as error:
        raise argparse.ArgumentTypeError("resolution dimensions must be integers") from error
    if not (80 <= width <= 16384 and 80 <= height <= 16384):
        raise argparse.ArgumentTypeError("resolution dimensions must be from 80 through 16384")
    return width, height


def parse_scales(value: str) -> list[int]:
    try:
        scales = [int(part.strip()) for part in value.split(",")]
    except ValueError as error:
        raise argparse.ArgumentTypeError("scales must be comma-separated integers") from error
    if not scales or any(scale < 1 or scale > MAX_DETECTION_SCALE for scale in scales):
        raise argparse.ArgumentTypeError("scales must be integers from 1 through 16")
    if len(set(scales)) != len(scales):
        raise argparse.ArgumentTypeError("scales must not contain duplicates")
    return scales


def nearest_rank_percentile(values: list[int | float], percentile: float) -> int | float | None:
    if not values:
        return None
    if not math.isfinite(percentile) or not 0.0 < percentile <= 1.0:
        raise ValueError("percentile must be finite and in (0, 1]")
    ordered = sorted(values)
    rank = max(1, math.ceil(percentile * len(ordered)))
    return ordered[rank - 1]


def _nonnegative_integer(value: Any, field: str) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < 0:
        raise BenchmarkError(f"trace has invalid {field}")
    return value


def _boolean(value: Any, field: str) -> bool:
    if not isinstance(value, bool):
        raise BenchmarkError(f"trace has invalid {field}")
    return value


def _process_metrics(summary: dict[str, Any]) -> dict[str, Any]:
    wall_us = _nonnegative_integer(summary.get("process_wall_us"), "summary.process_wall_us")
    if wall_us == 0:
        raise BenchmarkError("trace has zero process wall duration")

    cpu_valid = _boolean(summary.get("process_cpu_valid"), "summary.process_cpu_valid")
    cpu_us = summary.get("process_cpu_us")
    cpu_percent = summary.get("process_cpu_percent")
    percent_valid = _boolean(
        summary.get("process_cpu_percent_valid"), "summary.process_cpu_percent_valid"
    )
    if cpu_valid:
        cpu_us = _nonnegative_integer(cpu_us, "summary.process_cpu_us")
        if not isinstance(cpu_percent, (int, float)) or isinstance(cpu_percent, bool):
            raise BenchmarkError("trace has invalid summary.process_cpu_percent")
        cpu_percent = float(cpu_percent)
        if not math.isfinite(cpu_percent) or cpu_percent < 0.0:
            raise BenchmarkError("trace has invalid summary.process_cpu_percent")
        expected_percent = 100.0 * cpu_us / wall_us
        if not percent_valid or not math.isclose(cpu_percent, expected_percent, rel_tol=0.01, abs_tol=0.02):
            raise BenchmarkError("trace process CPU metrics are internally inconsistent")
    elif cpu_us is not None or cpu_percent is not None or percent_valid:
        raise BenchmarkError("trace has process CPU values marked invalid")

    memory_valid = _boolean(summary.get("memory_valid"), "summary.memory_valid")
    resident_start = summary.get("resident_start_bytes")
    resident = summary.get("resident_bytes")
    peak_resident = summary.get("peak_resident_bytes")
    growth_valid = _boolean(summary.get("resident_growth_valid"), "summary.resident_growth_valid")
    growth = summary.get("resident_growth_bytes")
    if memory_valid:
        resident_start = _nonnegative_integer(resident_start, "summary.resident_start_bytes")
        resident = _nonnegative_integer(resident, "summary.resident_bytes")
        peak_resident = _nonnegative_integer(peak_resident, "summary.peak_resident_bytes")
        if peak_resident < max(resident_start, resident):
            raise BenchmarkError("trace peak resident memory is below a sampled resident value")
        if not growth_valid or isinstance(growth, bool) or not isinstance(growth, int):
            raise BenchmarkError("trace has invalid summary.resident_growth_bytes")
        if growth != resident - resident_start:
            raise BenchmarkError("trace resident growth does not match start and end samples")
    elif any(value is not None for value in (resident_start, resident, peak_resident, growth)) or growth_valid:
        raise BenchmarkError("trace has memory values marked invalid")

    return {
        "process_wall_time_ms": wall_us / 1000.0,
        "process_cpu_time_ms": cpu_us / 1000.0 if cpu_valid else None,
        "process_cpu_percent": cpu_percent if cpu_valid else None,
        "process_cpu_valid": cpu_valid,
        "resident_start_bytes": resident_start,
        "resident_bytes": resident,
        "peak_resident_bytes": peak_resident,
        "resident_growth_bytes": growth,
        "memory_valid": memory_valid,
    }


def summarize_trace(trace_path: Path, detection_scale: int) -> dict[str, Any]:
    diagnostics: list[dict[str, Any]] = []
    candidate_observations = 0
    summary_events: list[dict[str, Any]] = []
    try:
        with trace_path.open("r", encoding="utf-8") as trace_file:
            for line_number, line in enumerate(trace_file, start=1):
                try:
                    event = json.loads(line)
                except json.JSONDecodeError as error:
                    raise BenchmarkError(f"trace line {line_number} is not valid JSON") from error
                if not isinstance(event, dict):
                    raise BenchmarkError(f"trace line {line_number} is not an object")
                kind = event.get("kind")
                if kind == "tracker_diagnostics":
                    diagnostics.append(event)
                elif kind == "candidate":
                    candidate_observations += 1
                elif kind == "summary":
                    summary_events.append(event)
    except OSError as error:
        raise BenchmarkError(f"could not read replay trace: {error}") from error

    if not diagnostics:
        raise BenchmarkError("replay trace contains no detector diagnostics")
    if len(summary_events) != 1:
        raise BenchmarkError("replay trace must contain exactly one summary event")

    detector_work_us: list[int] = []
    previous_frame_index = -1
    previous_work_count = 0
    source_geometry: tuple[int, int] | None = None
    max_detection_count = 0
    max_candidate_count = 0
    first_timestamp_ms: int | None = None
    last_timestamp_ms: int | None = None

    for event in diagnostics:
        frame_index = _nonnegative_integer(event.get("frame_index"), "frame_index")
        timestamp_ms = _nonnegative_integer(event.get("at_ms"), "at_ms")
        source_width = _nonnegative_integer(event.get("source_width"), "source_width")
        source_height = _nonnegative_integer(event.get("source_height"), "source_height")
        detector_width = _nonnegative_integer(event.get("detector_input_width"), "detector_input_width")
        detector_height = _nonnegative_integer(event.get("detector_input_height"), "detector_input_height")
        event_scale = _nonnegative_integer(event.get("detection_scale"), "detection_scale")
        if frame_index <= previous_frame_index:
            raise BenchmarkError("detector diagnostics frame indexes are not strictly increasing")
        if event_scale != detection_scale:
            raise BenchmarkError("trace detection scale does not match the requested scale")
        if source_width == 0 or source_height == 0:
            raise BenchmarkError("trace contains an empty source geometry")
        if (detector_width, detector_height) != (
            source_width // detection_scale,
            source_height // detection_scale,
        ):
            raise BenchmarkError("detector input geometry does not match source geometry and scale")
        if event.get("scaling_mode") != "rgb_stride":
            raise BenchmarkError("trace was not produced by the documented RGB stride scaling path")
        geometry = (source_width, source_height)
        if source_geometry is None:
            source_geometry = geometry
        elif geometry != source_geometry:
            raise BenchmarkError("source resolution changed during the run; split geometry changes into separate runs")

        work_count = _nonnegative_integer(event.get("detector_work_count"), "detector_work_count")
        work_duration = _nonnegative_integer(event.get("detector_work_us"), "detector_work_us")
        detections = _nonnegative_integer(event.get("detection_count"), "detection_count")
        candidates = _nonnegative_integer(event.get("candidate_count"), "candidate_count")
        if work_count < previous_work_count:
            raise BenchmarkError("detector work count moved backwards")
        completed_since_previous = work_count - previous_work_count
        if completed_since_previous > 1:
            raise BenchmarkError("trace skipped detector completions; detector latency samples are incomplete")
        if completed_since_previous == 1:
            detector_work_us.append(work_duration)

        previous_frame_index = frame_index
        previous_work_count = work_count
        first_timestamp_ms = timestamp_ms if first_timestamp_ms is None else first_timestamp_ms
        last_timestamp_ms = timestamp_ms
        max_detection_count = max(max_detection_count, detections)
        max_candidate_count = max(max_candidate_count, candidates)

    final_summary = summary_events[0]
    if _nonnegative_integer(final_summary.get("frames"), "summary.frames") != len(diagnostics):
        raise BenchmarkError("summary frame count does not match detector diagnostics")
    if _nonnegative_integer(final_summary.get("detection_scale"), "summary.detection_scale") != detection_scale:
        raise BenchmarkError("summary detection scale does not match the requested scale")
    if final_summary.get("scaling_mode") != "rgb_stride":
        raise BenchmarkError("summary does not identify the RGB stride scaling mode")
    process_metrics = _process_metrics(final_summary)
    if not detector_work_us:
        raise BenchmarkError("replay produced no detector work sample; use a longer frame manifest")

    assert source_geometry is not None
    assert first_timestamp_ms is not None and last_timestamp_ms is not None
    return {
        "source_resolution": list(source_geometry),
        "detector_input_resolution": [
            source_geometry[0] // detection_scale,
            source_geometry[1] // detection_scale,
        ],
        "detection_scale": detection_scale,
        "scaling_mode": "rgb_stride",
        "frame_count": len(diagnostics),
        "simulated_duration_ms": last_timestamp_ms - first_timestamp_ms,
        "detector_work_count": previous_work_count,
        "detector_work_samples_us": detector_work_us,
        "detector_work_median_us": statistics.median(detector_work_us),
        "detector_work_p95_us": nearest_rank_percentile(detector_work_us, 0.95),
        "detector_work_max_us": max(detector_work_us),
        "max_face_detections": max_detection_count,
        "max_tracking_candidates": max_candidate_count,
        "candidate_observation_events": candidate_observations,
        "process_metrics": process_metrics,
    }


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    try:
        with path.open("rb") as source:
            for block in iter(lambda: source.read(1024 * 1024), b""):
                digest.update(block)
    except OSError as error:
        raise BenchmarkError(f"could not hash {path.name}: {error}") from error
    return digest.hexdigest()


def _cpu_description() -> str | None:
    processor = platform.processor().strip()
    generic_names = {"arm", "aarch64", "x86_64", "amd64", "i386", "i686"}
    if processor and processor.casefold() not in generic_names:
        return processor
    if sys.platform == "darwin":
        command = ["sysctl", "-n", "machdep.cpu.brand_string"]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=2, check=False)
        except (OSError, subprocess.TimeoutExpired):
            return None
        value = result.stdout.strip()
        return value or None
    if sys.platform.startswith("linux"):
        try:
            with Path("/proc/cpuinfo").open("r", encoding="utf-8", errors="replace") as cpu_info:
                for line in cpu_info:
                    if line.lower().startswith(("model name", "hardware")):
                        _, value = line.split(":", 1)
                        return value.strip() or None
        except OSError:
            pass
    return processor or None


def _os_product_version() -> str:
    if sys.platform == "darwin":
        return platform.mac_ver()[0] or platform.version()
    if sys.platform == "win32":
        return platform.win32_ver()[1] or platform.version()
    if sys.platform.startswith("linux") and hasattr(platform, "freedesktop_os_release"):
        try:
            release = platform.freedesktop_os_release()
            return release.get("PRETTY_NAME") or release.get("VERSION_ID") or platform.version()
        except OSError:
            pass
    return platform.version()


def _source_revision(repository_root: Path) -> dict[str, Any]:
    try:
        commit = subprocess.run(
            ["git", "-C", str(repository_root), "rev-parse", "HEAD"],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
        status = subprocess.run(
            ["git", "-C", str(repository_root), "status", "--porcelain"],
            capture_output=True,
            text=True,
            timeout=2,
            check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return {"commit": None, "working_tree_dirty": None}
    return {
        "commit": commit.stdout.strip() if commit.returncode == 0 else None,
        "working_tree_dirty": bool(status.stdout.strip()) if status.returncode == 0 else None,
    }


def _run_replay(
    binary: Path,
    model: Path,
    manifest: Path,
    output_resolution: tuple[int, int],
    fps: int,
    scale: int,
    trace_path: Path,
    timeout_seconds: float,
) -> dict[str, Any]:
    command = [
        str(binary),
        "--model",
        str(model),
        "--manifest",
        str(manifest),
        "--trace",
        str(trace_path),
        "--fps",
        str(fps),
        "--output-width",
        str(output_resolution[0]),
        "--output-height",
        str(output_resolution[1]),
        "--detection-scale",
        str(scale),
    ]
    started = time.perf_counter_ns()
    try:
        completed = subprocess.run(
            command,
            capture_output=True,
            text=True,
            timeout=timeout_seconds,
            check=False,
        )
    except subprocess.TimeoutExpired as error:
        raise BenchmarkError(f"replay timed out at detection scale {scale}") from error
    except OSError as error:
        raise BenchmarkError(f"could not run offline replay: {error}") from error
    wall_time_ms = (time.perf_counter_ns() - started) / 1_000_000
    if completed.returncode != 0:
        detail = completed.stderr.strip()[-3000:]
        raise BenchmarkError(
            f"replay failed at detection scale {scale} with exit code {completed.returncode}: {detail}"
        )
    metrics = summarize_trace(trace_path, scale)
    metrics["replay_wall_time_ms"] = round(wall_time_ms, 3)
    return metrics


def _aggregate_runs(runs: list[dict[str, Any]]) -> dict[str, Any]:
    latency_samples = [
        sample for run in runs for sample in run["detector_work_samples_us"]
    ]
    wall_samples = [run["replay_wall_time_ms"] for run in runs]
    first = runs[0]
    for run in runs[1:]:
        for field in ("source_resolution", "detector_input_resolution", "frame_count", "simulated_duration_ms"):
            if run[field] != first[field]:
                raise BenchmarkError(f"replay workload changed between repeats ({field})")
    process_runs = [run["process_metrics"] for run in runs]
    cpu_runs = [run for run in process_runs if run["process_cpu_valid"]]
    memory_runs = [run for run in process_runs if run["memory_valid"]]
    return {
        "repeats": len(runs),
        "source_resolution": first["source_resolution"],
        "detector_input_resolution": first["detector_input_resolution"],
        "frame_count_per_run": first["frame_count"],
        "simulated_duration_ms_per_run": first["simulated_duration_ms"],
        "detector_work_samples_count": len(latency_samples),
        "detector_work_samples_us": latency_samples,
        "detector_work_median_us": statistics.median(latency_samples),
        "detector_work_p95_us": nearest_rank_percentile(latency_samples, 0.95),
        "detector_work_max_us": max(latency_samples),
        "replay_wall_time_median_ms": statistics.median(wall_samples),
        "replay_wall_time_max_ms": max(wall_samples),
        "process_wall_time_median_ms": statistics.median(run["process_wall_time_ms"] for run in process_runs),
        "process_cpu_valid_runs": len(cpu_runs),
        "process_cpu_time_median_ms": (
            statistics.median(run["process_cpu_time_ms"] for run in cpu_runs) if cpu_runs else None
        ),
        "process_cpu_percent_median": (
            statistics.median(run["process_cpu_percent"] for run in cpu_runs) if cpu_runs else None
        ),
        "memory_valid_runs": len(memory_runs),
        "resident_start_bytes_median": (
            statistics.median(run["resident_start_bytes"] for run in memory_runs) if memory_runs else None
        ),
        "resident_bytes_median": (
            statistics.median(run["resident_bytes"] for run in memory_runs) if memory_runs else None
        ),
        "peak_resident_bytes_max": max((run["peak_resident_bytes"] for run in memory_runs), default=None),
        "resident_growth_bytes_median": (
            statistics.median(run["resident_growth_bytes"] for run in memory_runs) if memory_runs else None
        ),
        "max_face_detections": max(run["max_face_detections"] for run in runs),
        "max_tracking_candidates": max(run["max_tracking_candidates"] for run in runs),
        "candidate_observation_events_total": sum(run["candidate_observation_events"] for run in runs),
        "runs": runs,
    }


def _write_json_atomically(path: Path, report: dict[str, Any]) -> None:
    temporary_path: Path | None = None
    try:
        path.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            mode="w",
            encoding="utf-8",
            dir=path.parent,
            prefix=f".{path.name}.",
            suffix=".tmp",
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            json.dump(report, output, indent=2, sort_keys=True, allow_nan=False)
            output.write("\n")
        os.replace(temporary_path, path)
    except OSError as error:
        raise BenchmarkError(f"could not write benchmark report: {error}") from error
    finally:
        if temporary_path and temporary_path.exists():
            try:
                temporary_path.unlink()
            except OSError:
                pass


def _build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", required=True, type=Path, help="offline session replay executable")
    parser.add_argument("--model", required=True, type=Path, help="dlib frontal face detector model")
    parser.add_argument("--manifest", required=True, type=Path, help="monotonic timestamp/PPM manifest")
    parser.add_argument("--output", required=True, type=Path, help="destination JSON report")
    parser.add_argument("--scales", type=parse_scales, default=parse_scales("1,2,4"))
    parser.add_argument("--repeats", type=int, default=3, help="replay count per scale (1..100)")
    parser.add_argument("--fps", type=int, default=30, help="simulated source rate (1..120)")
    parser.add_argument(
        "--output-resolution",
        type=parse_resolution,
        default=parse_resolution("1920x1080"),
        help="output viewport dimensions (default: 1920x1080)",
    )
    parser.add_argument(
        "--workload-label",
        required=True,
        help="short description without source paths or personal information",
    )
    parser.add_argument("--timeout-seconds", type=float, default=300.0)
    parser.add_argument(
        "--trace-directory",
        type=Path,
        help="optional directory for numeric JSONL traces; reports never contain frame paths or pixels",
    )
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = _build_parser()
    args = parser.parse_args(argv)
    if not (1 <= args.repeats <= 100):
        parser.error("--repeats must be from 1 through 100")
    if not (1 <= args.fps <= 120):
        parser.error("--fps must be from 1 through 120")
    if not math.isfinite(args.timeout_seconds) or args.timeout_seconds <= 0:
        parser.error("--timeout-seconds must be finite and greater than zero")

    binary = args.binary.expanduser().resolve()
    if not binary.is_file():
        resolved = shutil.which(str(args.binary))
        if resolved:
            binary = Path(resolved).resolve()
        else:
            parser.error(f"offline replay executable does not exist: {args.binary}")
    model = args.model.expanduser().resolve()
    manifest = args.manifest.expanduser().resolve()
    output = args.output.expanduser().resolve()
    if not model.is_file():
        parser.error(f"model file does not exist: {model}")
    if not manifest.is_file():
        parser.error(f"manifest file does not exist: {manifest}")
    if output in (binary, model, manifest):
        parser.error("report output must not replace the replay executable, model, or manifest")
    if not args.workload_label.strip():
        parser.error("--workload-label must not be empty")

    trace_root = args.trace_directory.expanduser().resolve() if args.trace_directory else None
    if trace_root:
        try:
            trace_root.mkdir(parents=True, exist_ok=True)
        except OSError as error:
            parser.error(f"could not create trace directory: {error}")
        expected_traces = [
            trace_root / f"scale-{scale}-run-{repeat_index + 1}.jsonl"
            for scale in args.scales
            for repeat_index in range(args.repeats)
        ]
        if output in expected_traces:
            parser.error("report output must not replace a retained trace file")
        if any(trace.exists() for trace in expected_traces):
            parser.error("trace directory already contains a requested scale/run trace filename")

    repository_root = Path(__file__).resolve().parents[1]
    report: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "generated_at_utc": datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z"),
        "evidence_scope": "offline_development_measurement",
        "production_acceptance_eligible": False,
        "obs_runtime_version": None,
        "host": {
            "operating_system": platform.system(),
            "os_version": _os_product_version(),
            "os_kernel_release": platform.release(),
            "os_kernel_version": platform.version(),
            "cpu_architecture": platform.machine(),
            "cpu_description": _cpu_description(),
            "logical_cpu_count": os.cpu_count(),
            "python_version": platform.python_version(),
        },
        "source_revision": _source_revision(repository_root),
        "replay_binary": {"filename": binary.name, "sha256": _sha256_file(binary)},
        "model": {"filename": model.name, "sha256": _sha256_file(model)},
        "manifest": {
            "sha256": _sha256_file(manifest),
            "hash_scope": "manifest file bytes only; referenced PPM frame contents are not hashed",
        },
        "workload": {
            "label": args.workload_label.strip(),
            "fps": args.fps,
            "output_resolution": list(args.output_resolution),
            "source_resolution": None,
            "notes": "manifest frame paths and image data are not copied into this report",
        },
        "metrics_scope": {
            "measured": [
                "dlib HOG detector execution latency from the numeric replay trace",
                "offline replay wall time",
                (
                    "offline replay process CPU time and CPU percentage from before HOG model validation "
                    "through detector/tracker and Rust session teardown"
                ),
                (
                    "offline replay process resident memory and start-to-end growth over that interval; "
                    "peak resident memory is the operating-system process-lifetime high-water mark"
                ),
                "maximum face detections and tracking candidates per frame",
            ],
            "not_measured": [
                "OBS live capture, rendering, or encoding",
                "real-camera tracking quality",
                "production Windows hardware behavior",
            ],
        },
        "scales": [],
    }

    try:
        with tempfile.TemporaryDirectory(prefix="faithfulframe-offline-benchmark-") as temp_dir:
            temporary_root = Path(temp_dir)
            for scale in args.scales:
                runs: list[dict[str, Any]] = []
                for repeat_index in range(args.repeats):
                    trace_path = (
                        trace_root / f"scale-{scale}-run-{repeat_index + 1}.jsonl"
                        if trace_root
                        else temporary_root / f"scale-{scale}-run-{repeat_index + 1}.jsonl"
                    )
                    metrics = _run_replay(
                        binary,
                        model,
                        manifest,
                        args.output_resolution,
                        args.fps,
                        scale,
                        trace_path,
                        args.timeout_seconds,
                    )
                    metrics["run_index"] = repeat_index + 1
                    if report["workload"]["source_resolution"] is None:
                        report["workload"]["source_resolution"] = metrics["source_resolution"]
                    elif report["workload"]["source_resolution"] != metrics["source_resolution"]:
                        raise BenchmarkError("source resolution changed between scales")
                    runs.append(metrics)
                report["scales"].append(
                    {
                        "detection_scale": scale,
                        "scaling_mode": "rgb_stride",
                        "detector_input_resolution": runs[0]["detector_input_resolution"],
                        "summary": _aggregate_runs(runs),
                        "trace_files": (
                            [f"scale-{scale}-run-{i + 1}.jsonl" for i in range(args.repeats)]
                            if trace_root
                            else []
                        ),
                    }
                )
        _write_json_atomically(output, report)
    except BenchmarkError as error:
        print(f"offline benchmark failed: {error}", file=sys.stderr)
        return 2

    print(f"Wrote offline benchmark report: {output}")
    print("Scope: offline development measurement; not production acceptance evidence")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
