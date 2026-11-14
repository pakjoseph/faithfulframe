#!/usr/bin/env python3
"""Exercise video-frame geometry events through production dlib and Rust policy."""

from __future__ import annotations

import argparse
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


def write_black_ppm(path: Path, width: int, height: int) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + bytes(width * height * 3))


class OfflineSessionReplayTests(unittest.TestCase):
    def test_resolution_change_invalidates_geometry_and_stays_safe_wide(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            first = temporary / "first.ppm"
            second = temporary / "second.ppm"
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            write_black_ppm(first, 80, 80)
            write_black_ppm(second, 96, 80)
            manifest.write_text(
                "monotonic_ms\tframe_path\n0\tfirst.ppm\n200\tsecond.ppm\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    str(self.binary),
                    "--model",
                    str(self.model),
                    "--manifest",
                    str(manifest),
                    "--trace",
                    str(trace),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
            policy_events = [event for event in events if event["kind"] != "tracker_diagnostics"]
            diagnostics = [event for event in events if event["kind"] == "tracker_diagnostics"]
            self.assertEqual(
                [event["kind"] for event in policy_events], ["tick", "lifecycle", "tick", "summary"]
            )
            self.assertEqual(len(diagnostics), 2)
            self.assertTrue(all("detector_work_us" in event for event in diagnostics))
            geometry = policy_events[1]
            self.assertEqual(geometry["event"], "geometry_changed")
            self.assertEqual((geometry["source_width"], geometry["source_height"]), (96, 80))
            self.assertEqual(geometry["snapshot"]["generations"]["geometry"], 2)
            self.assertEqual(policy_events[-1]["snapshot"]["state"], "manual")
            self.assertIsNone(policy_events[-1]["snapshot"]["selected_handle"])
            self.assertEqual(policy_events[-1]["snapshot"]["source_crop"], [0, 0, 1, 1])
            self.assertTrue(all("pixels" not in event and "path" not in event for event in events))

    def test_selection_requires_a_candidate_and_never_happens_automatically(self):
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            frame = temporary / "black.ppm"
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            write_black_ppm(frame, 80, 80)
            manifest.write_text(
                "monotonic_ms\tframe_path\n0\tblack.ppm\n",
                encoding="utf-8",
            )

            result = subprocess.run(
                [
                    str(self.binary),
                    "--model",
                    str(self.model),
                    "--manifest",
                    str(manifest),
                    "--trace",
                    str(trace),
                    "--operator-select-first-candidate-at-ms",
                    "0",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 3)
            self.assertIn("No candidate was available", result.stderr)
            events = [json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()]
            policy_events = [event for event in events if event["kind"] != "tracker_diagnostics"]
            self.assertEqual([event["kind"] for event in policy_events], ["tick"])
            self.assertEqual(policy_events[0]["snapshot"]["state"], "manual")

    @classmethod
    def setUpClass(cls):
        parser = argparse.ArgumentParser()
        parser.add_argument("--binary", required=True, type=Path)
        parser.add_argument("--model", required=True, type=Path)
        args, _ = parser.parse_known_args()
        cls.binary = args.binary
        cls.model = args.model


if __name__ == "__main__":
    unittest.main(argv=[__file__])
