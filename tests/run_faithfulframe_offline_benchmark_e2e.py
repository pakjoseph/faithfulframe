"""End-to-end smoke test for the offline detector benchmark CLI."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--benchmark", required=True, type=Path)
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    args = parser.parse_args()

    fixture = Path(__file__).resolve().parent / "fixtures" / "offline-synthetic-person-320x360.ppm"
    with tempfile.TemporaryDirectory(prefix="faithfulframe-benchmark-smoke-") as temp_dir:
        temp = Path(temp_dir)
        manifest = temp / "frames.tsv"
        report_path = temp / "report.json"
        manifest.write_text(
            "monotonic_ms\tframe_path\n"
            + "".join(f"{frame_index * 40}\t{fixture}\n" for frame_index in range(150)),
            encoding="utf-8",
        )
        command = [
            sys.executable,
            str(args.benchmark.resolve()),
            "--binary",
            str(args.binary.resolve()),
            "--model",
            str(args.model.resolve()),
            "--manifest",
            str(manifest),
            "--output",
            str(report_path),
            "--scales",
            "1,2",
            "--repeats",
            "1",
            "--fps",
            "25",
            "--output-resolution",
            "1920x1080",
            "--workload-label",
            "synthetic-person-ci",
            "--timeout-seconds",
            "300",
        ]
        try:
            completed = subprocess.run(command, capture_output=True, text=True, timeout=360, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"offline benchmark CLI could not complete: {error}", file=sys.stderr)
            return 1
        if completed.returncode != 0:
            print(completed.stdout, file=sys.stderr, end="")
            print(completed.stderr, file=sys.stderr, end="")
            return completed.returncode or 1

        try:
            report = json.loads(report_path.read_text(encoding="utf-8"))
        except (OSError, json.JSONDecodeError) as error:
            print(f"offline benchmark did not produce valid JSON: {error}", file=sys.stderr)
            return 1

        if report.get("evidence_scope") != "offline_development_measurement":
            print("benchmark scope was not marked as an offline development measurement", file=sys.stderr)
            return 1
        if report.get("production_acceptance_eligible") is not False or report.get("obs_runtime_version") is not None:
            print("offline benchmark was incorrectly marked as production acceptance evidence", file=sys.stderr)
            return 1
        measured = report.get("metrics_scope", {}).get("measured", [])
        if not any("process CPU" in metric for metric in measured) or not any("resident memory" in metric for metric in measured):
            print("offline benchmark did not identify its process CPU and memory measurements", file=sys.stderr)
            return 1
        if any("process CPU" in metric or "process" in metric and "memory" in metric for metric in report.get("metrics_scope", {}).get("not_measured", [])):
            print("offline benchmark still marks measured process metrics as unavailable", file=sys.stderr)
            return 1
        if report.get("workload", {}).get("source_resolution") != [320, 360]:
            print("benchmark did not preserve the fixture source geometry", file=sys.stderr)
            return 1
        scales = report.get("scales", [])
        if [scale.get("detection_scale") for scale in scales] != [1, 2]:
            print("benchmark did not report both requested detector scales", file=sys.stderr)
            return 1
        for scale in scales:
            summary = scale.get("summary", {})
            if summary.get("detector_work_samples_count", 0) <= 0:
                print("benchmark scale has no detector latency samples", file=sys.stderr)
                return 1
            if summary.get("max_face_detections", 0) <= 0:
                print("benchmark scale did not detect the synthetic person fixture", file=sys.stderr)
                return 1
            if summary.get("max_tracking_candidates", 0) <= 0:
                print("benchmark scale produced no tracking candidate for the fixture", file=sys.stderr)
                return 1
            if summary.get("process_wall_time_median_ms", 0) <= 0:
                print("benchmark scale has no process wall-time measurement", file=sys.stderr)
                return 1
            if summary.get("process_cpu_valid_runs", 0) < 0 or summary.get("memory_valid_runs", 0) < 0:
                print("benchmark scale has invalid process metric availability counts", file=sys.stderr)
                return 1
        if str(fixture) in report_path.read_text(encoding="utf-8"):
            print("benchmark report unexpectedly contains the fixture path", file=sys.stderr)
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
