#!/usr/bin/env python3
"""Verify one selected production dlib backend can replay a synthetic trace."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def write_detector_cadence_manifest(source: Path, destination: Path) -> None:
    """Repeat the first fixture frame long enough to reach production scan cadence."""
    rows = [line for line in source.read_text(encoding="utf-8").splitlines() if line.strip()]
    if rows and rows[0].split("\t") == ["monotonic_ms", "frame_path"]:
        rows = rows[1:]
    if not rows:
        raise ValueError(f"manifest contains no frames: {source}")
    fields = rows[0].split("\t", 1)
    if len(fields) != 2:
        raise ValueError(f"manifest row must contain timestamp and frame path: {rows[0]!r}")
    frame_path = Path(fields[1])
    if not frame_path.is_absolute():
        frame_path = (source.parent / frame_path).resolve()

    # The production face_tracker_manager scans every two seconds. Three seconds
    # at 25 fps gives deterministic room for its first scheduled detector pass.
    destination.write_text(
        "monotonic_ms\tframe_path\n"
        + "".join(f"{frame_index * 40}\t{frame_path}\n" for frame_index in range(75)),
        encoding="utf-8",
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--detector-engine", required=True, choices=("dlib_hog", "dlib_cnn"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="faithfulframe-detector-backend-") as temporary:
        temporary_path = Path(temporary)
        trace = temporary_path / "trace.jsonl"
        manifest = temporary_path / "cadence.tsv"
        try:
            write_detector_cadence_manifest(args.manifest, manifest)
        except (OSError, ValueError) as error:
            print(f"could not prepare detector cadence manifest: {error}", file=sys.stderr)
            return 1
        command = [
            str(args.binary.resolve()),
            "--model",
            str(args.model.resolve()),
            "--manifest",
            str(manifest),
            "--fps",
            "25",
            "--trace",
            str(trace),
            "--detector-engine",
            args.detector_engine,
        ]
        try:
            result = subprocess.run(command, capture_output=True, text=True, timeout=120, check=False)
        except (OSError, subprocess.TimeoutExpired) as error:
            print(f"offline {args.detector_engine} replay failed to complete: {error}", file=sys.stderr)
            return 1
        if result.returncode != 0:
            print(result.stdout, file=sys.stderr, end="")
            print(result.stderr, file=sys.stderr, end="")
            return result.returncode or 1
        try:
            events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
        except (OSError, json.JSONDecodeError) as error:
            print(f"offline replay produced an invalid trace: {error}", file=sys.stderr)
            return 1

    diagnostics = [event for event in events if event.get("kind") == "tracker_diagnostics"]
    summaries = [event for event in events if event.get("kind") == "summary"]
    if not diagnostics or len(summaries) != 1:
        print("offline replay omitted detector diagnostics or its final summary", file=sys.stderr)
        return 1
    if any(event.get("detector_engine") != args.detector_engine for event in diagnostics + summaries):
        print("offline replay trace did not preserve the requested detector engine", file=sys.stderr)
        return 1
    if not any(event.get("detector_work_count", 0) > 0 for event in diagnostics):
        print("offline replay did not execute the selected detector", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
