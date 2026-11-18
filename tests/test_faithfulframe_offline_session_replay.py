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


def read_synthetic_portrait(portrait_path: Path) -> tuple[int, int, bytes]:
    magic, dimensions, maximum, portrait = portrait_path.read_bytes().split(b"\n", 3)
    # Git may check this text-like header out with CRLF on Windows while the
    # binary pixel payload remains unchanged. Treat the PPM separators as
    # whitespace instead of requiring one newline convention.
    if magic.strip() != b"P6" or maximum.strip() != b"255":
        raise ValueError("synthetic portrait fixture must be an 8-bit P6 PPM")
    portrait_width, portrait_height = map(int, dimensions.split())
    if len(portrait) != portrait_width * portrait_height * 3:
        raise ValueError("synthetic portrait fixture has an invalid pixel payload")
    return portrait_width, portrait_height, portrait


def write_two_cartoon_face_scene(path: Path, portrait_path: Path) -> None:
    portrait_width, portrait_height, portrait = read_synthetic_portrait(portrait_path)

    width, height = 1920, 1080
    pixels = bytearray([32, 38, 42]) * (width * height)
    # Duplicate the procedurally drawn, non-identifiable cartoon in a simple
    # stage scene. This exercises detector candidate enumeration without
    # storing real-person imagery or biometric data.
    for x in (320, 1280):
        y = (height - portrait_height) // 2
        for row in range(portrait_height):
            source_start = row * portrait_width * 3
            target_start = ((y + row) * width + x) * 3
            pixels[target_start : target_start + portrait_width * 3] = portrait[
                source_start : source_start + portrait_width * 3
            ]
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def write_cartoon_stage_frame(path: Path, portrait_path: Path, x: int) -> None:
    portrait_width, portrait_height, portrait = read_synthetic_portrait(portrait_path)
    width, height = 1280, 720
    if x < 0 or x + portrait_width > width or portrait_height > height:
        raise ValueError("synthetic portrait must fit inside the stage frame")

    pixels = bytearray([32, 38, 42]) * (width * height)
    y = (height - portrait_height) // 2
    for row in range(portrait_height):
        source_start = row * portrait_width * 3
        target_start = ((y + row) * width + x) * 3
        pixels[target_start : target_start + portrait_width * 3] = portrait[
            source_start : source_start + portrait_width * 3
        ]
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


class OfflineSessionReplayTests(unittest.TestCase):
    def test_synthetic_face_flows_through_detection_tracking_and_explicit_selection(self):
        # This checked-in cartoon is procedurally drawn and contains no real
        # person, recording, or biometric template.
        fixture = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        self.assertTrue(fixture.is_file(), f"missing synthetic person fixture: {fixture}")

        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            manifest.write_text(
                "monotonic_ms\tframe_path\n"
                # Keep the scene static long enough for the production detector
                # and correlation-tracker workers to publish a stable candidate
                # across slower CI hosts. Event time stays deterministic; the
                # replay does not sleep for the six simulated seconds.
                + "".join(f"{frame_index * 40}\t{fixture}\n" for frame_index in range(150)),
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
                    "--output-width",
                    "1920",
                    "--output-height",
                    "1080",
                    "--operator-select-first-candidate-at-ms",
                    "2200",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            failure_context = result.stderr
            if result.returncode != 0 and trace.is_file():
                diagnostic_events = [
                    json.loads(line)
                    for line in trace.read_text(encoding="utf-8").splitlines()
                    if '"kind":"tracker_diagnostics"' in line
                ]
                failure_context += f"\nDetector/tracker diagnostics: {diagnostic_events}"
            self.assertEqual(result.returncode, 0, failure_context)

            trace_text = trace.read_text(encoding="utf-8")
            events = [json.loads(line) for line in trace_text.splitlines()]
            diagnostics = [event for event in events if event["kind"] == "tracker_diagnostics"]
            candidates = [event for event in events if event["kind"] == "candidate"]
            commands = [event for event in events if event["kind"] == "command"]
            summary = next(event for event in events if event["kind"] == "summary")

            self.assertGreater(max(event["detector_work_count"] for event in diagnostics), 0)
            self.assertGreater(max(event["detection_count"] for event in diagnostics), 0)
            self.assertGreater(len(candidates), 0)
            self.assertEqual(len(commands), 1)
            self.assertEqual(commands[0]["origin"], "explicit_operator_flag")
            self.assertTrue(summary["operator_selection_applied"])
            self.assertEqual(commands[0]["snapshot"]["state"], "locked")
            self.assertEqual(
                commands[0]["snapshot"]["selected_handle"], commands[0]["candidate_handle"]
            )
            # Detector evidence can legitimately age out during an accelerated
            # replay. The policy must then hold the selected framing instead
            # of silently following an unconfirmed target.
            self.assertIn(summary["snapshot"]["state"], ("locked", "searching"), summary)
            self.assertEqual(summary["snapshot"]["selected_handle"], commands[0]["candidate_handle"])
            expected_crop_action = "track" if summary["snapshot"]["state"] == "locked" else "hold"
            self.assertEqual(summary["snapshot"]["crop_action"], expected_crop_action, summary)

            selection_index = events.index(commands[0])
            self.assertTrue(
                all(
                    event["snapshot"]["selected_handle"] is None
                    for event in events[:selection_index]
                    if "snapshot" in event
                ),
                "the synthetic detection must not select a target without an operator command",
            )
            for event in candidates:
                x, y, width, height = event["candidate"]["rect"]
                self.assertGreater(width, 0.0)
                self.assertGreater(height, 0.0)
                self.assertGreaterEqual(x, 0.0)
                self.assertGreaterEqual(y, 0.0)
                self.assertLessEqual(x + width, 1.00001)
                self.assertLessEqual(y + height, 1.00001)
            self.assertNotIn("pixels", trace_text)
            self.assertNotIn("frame_path", trace_text)

    def test_two_synthetic_faces_require_explicit_selection(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            scene = temporary / "two-cartoon-faces-1920x1080.ppm"
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            write_two_cartoon_face_scene(scene, portrait)
            manifest.write_text(
                "monotonic_ms\tframe_path\n"
                + "".join(f"{frame_index * 40}\t{scene}\n" for frame_index in range(150)),
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
                    "--output-width",
                    "1920",
                    "--output-height",
                    "1080",
                    "--operator-select-first-candidate-at-ms",
                    "2200",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            failure_context = result.stderr
            if result.returncode != 0 and trace.is_file():
                diagnostic_events = [
                    json.loads(line)
                    for line in trace.read_text(encoding="utf-8").splitlines()
                    if '"kind":"tracker_diagnostics"' in line
                ]
                failure_context += f"\nDetector/tracker diagnostics: {diagnostic_events}"
            self.assertEqual(result.returncode, 0, failure_context)

            trace_text = trace.read_text(encoding="utf-8")
            events = [json.loads(line) for line in trace_text.splitlines()]
            diagnostics = [event for event in events if event["kind"] == "tracker_diagnostics"]
            candidates = [event for event in events if event["kind"] == "candidate"]
            commands = [event for event in events if event["kind"] == "command"]
            summary = next(event for event in events if event["kind"] == "summary")
            candidates_by_frame = {}
            for event in candidates:
                candidates_by_frame.setdefault(event["frame_index"], []).append(event["candidate"])
            simultaneous_group = max(
                candidates_by_frame.values(),
                key=lambda group: len({candidate["handle"] for candidate in group}),
                default=[],
            )
            simultaneous_by_handle = {candidate["handle"]: candidate for candidate in simultaneous_group}

            self.assertGreaterEqual(max(event["detection_count"] for event in diagnostics), 2)
            self.assertGreaterEqual(max(event["candidate_count"] for event in diagnostics), 2)
            self.assertGreaterEqual(len(simultaneous_by_handle), 2)
            center_x = []
            for candidate in simultaneous_by_handle.values():
                x, y, width, height = candidate["rect"]
                self.assertGreater(width, 0.0)
                self.assertGreater(height, 0.0)
                self.assertGreaterEqual(x, 0.0)
                self.assertGreaterEqual(y, 0.0)
                self.assertLessEqual(x + width, 1.00001)
                self.assertLessEqual(y + height, 1.00001)
                center_x.append(x + width / 2)
            self.assertGreater(
                max(center_x) - min(center_x),
                0.25,
                "the two dlib candidates must represent spatially separate stage positions",
            )
            self.assertEqual(len(commands), 1)
            self.assertEqual(commands[0]["origin"], "explicit_operator_flag")
            self.assertTrue(summary["operator_selection_applied"])
            self.assertEqual(commands[0]["snapshot"]["state"], "locked")
            self.assertEqual(
                commands[0]["snapshot"]["selected_handle"], commands[0]["candidate_handle"]
            )
            self.assertIn(summary["snapshot"]["state"], ("locked", "searching"), summary)
            self.assertEqual(summary["snapshot"]["selected_handle"], commands[0]["candidate_handle"])
            expected_crop_action = "track" if summary["snapshot"]["state"] == "locked" else "hold"
            self.assertEqual(summary["snapshot"]["crop_action"], expected_crop_action, summary)
            selection_index = events.index(commands[0])
            self.assertTrue(
                all(
                    event["snapshot"]["selected_handle"] is None
                    for event in events[:selection_index]
                    if "snapshot" in event
                ),
                "multiple synthetic detections must not select a target without an operator command",
            )
            self.assertNotIn("pixels", trace_text)
            self.assertNotIn("frame_path", trace_text)

    def test_selected_synthetic_speaker_can_move_across_stage(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            frame_for_x = {}
            manifest_rows = ["monotonic_ms\tframe_path\n"]
            frame_count = 170
            for frame_index in range(frame_count):
                if frame_index < 70:
                    x = 300
                elif frame_index < 130:
                    # Move right in small, repeated steps after the detector
                    # has had a stable initial view and the operator can lock.
                    x = 300 + ((frame_index - 70) // 3 + 1) * 20
                else:
                    x = 700

                frame = frame_for_x.get(x)
                if frame is None:
                    frame = temporary / f"stage-x-{x}.ppm"
                    write_cartoon_stage_frame(frame, portrait, x)
                    frame_for_x[x] = frame
                manifest_rows.append(f"{frame_index * 40}\t{frame}\n")
            manifest.write_text("".join(manifest_rows), encoding="utf-8")

            result = subprocess.run(
                [
                    str(self.binary),
                    "--model",
                    str(self.model),
                    "--manifest",
                    str(manifest),
                    "--trace",
                    str(trace),
                    "--output-width",
                    "1920",
                    "--output-height",
                    "1080",
                    "--operator-select-first-candidate-at-ms",
                    "2200",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)

            trace_text = trace.read_text(encoding="utf-8")
            events = [json.loads(line) for line in trace_text.splitlines()]
            candidates = [event for event in events if event["kind"] == "candidate"]
            commands = [event for event in events if event["kind"] == "command"]
            summary = next(event for event in events if event["kind"] == "summary")
            self.assertEqual(len(commands), 1)
            command = commands[0]
            self.assertEqual(command["snapshot"]["state"], "locked")
            self.assertEqual(command["snapshot"]["selected_handle"], command["candidate_handle"])

            selected_token = next(
                event["candidate"]["continuity_token"]
                for event in candidates
                if event["candidate"]["handle"] == command["candidate_handle"]
            )
            selected_candidates = [
                event
                for event in candidates
                if event["candidate"]["continuity_token"] == selected_token
            ]
            centers = [
                event["candidate"]["rect"][0] + event["candidate"]["rect"][2] / 2
                for event in selected_candidates
            ]
            self.assertGreater(len(centers), 1)
            self.assertLess(centers[0], 0.45)
            self.assertGreater(centers[-1], 0.60)
            self.assertGreater(centers[-1] - centers[0], 0.20)

            crop_positions = [event["snapshot"]["source_crop"][0] for event in selected_candidates]
            self.assertGreater(crop_positions[-1], crop_positions[0])
            self.assertIn(summary["snapshot"]["state"], ("locked", "searching"), summary)
            self.assertEqual(summary["snapshot"]["selected_handle"], command["candidate_handle"])
            expected_crop_action = "track" if summary["snapshot"]["state"] == "locked" else "hold"
            self.assertEqual(summary["snapshot"]["crop_action"], expected_crop_action, summary)

            selection_index = events.index(command)
            self.assertTrue(
                all(
                    event["snapshot"]["selected_handle"] is None
                    for event in events[:selection_index]
                    if "snapshot" in event
                ),
                "the moving synthetic speaker must not be selected without an operator command",
            )
            self.assertNotIn("pixels", trace_text)
            self.assertNotIn("frame_path", trace_text)

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
