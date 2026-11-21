#!/usr/bin/env python3
"""Verify one selected production dlib backend can replay a synthetic trace."""

import argparse
import json
from pathlib import Path
import subprocess
import sys
import tempfile


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True, type=Path)
    parser.add_argument("--model", required=True, type=Path)
    parser.add_argument("--manifest", required=True, type=Path)
    parser.add_argument("--detector-engine", required=True, choices=("dlib_hog", "dlib_cnn"))
    args = parser.parse_args()

    with tempfile.TemporaryDirectory(prefix="faithfulframe-detector-backend-") as temporary:
        trace = Path(temporary) / "trace.jsonl"
        command = [
            str(args.binary.resolve()),
            "--model",
            str(args.model.resolve()),
            "--manifest",
            str(args.manifest.resolve()),
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
