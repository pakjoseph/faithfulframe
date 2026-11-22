#!/usr/bin/env python3
"""Exercise video-frame geometry events through production dlib and Rust policy."""

from __future__ import annotations

import argparse
import json
import struct
import subprocess
import tempfile
import unittest
from pathlib import Path


def write_black_ppm(path: Path, width: int, height: int) -> None:
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + bytes(width * height * 3))


def as_float32(value: float) -> float:
    return struct.unpack("=f", struct.pack("=f", value))[0]


def float32_sum(left: float, right: float) -> float:
    """Add values with the same f32 rounding used by the C ABI geometry."""
    return as_float32(as_float32(left) + as_float32(right))


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
    write_cartoon_faces_scene(path, portrait_path, (320, 1280))


def write_cartoon_faces_scene(
    path: Path, portrait_path: Path, x_positions: tuple[int, ...]
) -> None:
    portrait_width, portrait_height, portrait = read_synthetic_portrait(portrait_path)

    width, height = 1920, 1080
    pixels = bytearray([32, 38, 42]) * (width * height)
    # Duplicate the procedurally drawn, non-identifiable cartoon in a simple
    # stage scene. This exercises detector candidate enumeration without
    # storing real-person imagery or biometric data.
    for x in x_positions:
        if x < 0 or x + portrait_width > width:
            raise ValueError("synthetic portrait must fit inside the stage frame")
        y = (height - portrait_height) // 2
        for row in range(portrait_height):
            source_start = row * portrait_width * 3
            target_start = ((y + row) * width + x) * 3
            pixels[target_start : target_start + portrait_width * 3] = portrait[
                source_start : source_start + portrait_width * 3
            ]
    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def write_single_speaker_scene(
    path: Path,
    portrait_path: Path,
    scale: int,
    left: int = 800,
    top: int = 360,
) -> None:
    if scale not in (1, 2):
        raise ValueError("the geometry comparison uses 1080p and exact 2x 4K scenes")

    portrait_width, portrait_height, portrait = read_synthetic_portrait(portrait_path)
    width, height = 1920 * scale, 1080 * scale
    pixels = bytearray([32, 38, 42]) * (width * height)
    left, top = left * scale, top * scale

    # Duplicate each RGB pixel exactly at 4K. The 4K replay's stride-2 detector
    # input must therefore be byte-for-byte equivalent to the 1080p input.
    for row in range(portrait_height):
        source_start = row * portrait_width * 3
        source_row = portrait[source_start : source_start + portrait_width * 3]
        scaled_row = bytearray()
        for x in range(portrait_width):
            pixel_start = x * 3
            scaled_row.extend(source_row[pixel_start : pixel_start + 3] * scale)
        for y in range(scale):
            target_start = ((top + row * scale + y) * width + left) * 3
            pixels[target_start : target_start + len(scaled_row)] = scaled_row

    path.write_bytes(f"P6\n{width} {height}\n255\n".encode("ascii") + pixels)


def write_stage_projection_scene(
    path: Path,
    portrait_path: Path,
    include_stage_speaker: bool,
    speaker_brightness: float = 1.0,
    include_band_member: bool = False,
) -> None:
    portrait_width, portrait_height, portrait = read_synthetic_portrait(portrait_path)
    width, height = 1920, 1080
    pixels = bytearray([32, 38, 42]) * (width * height)

    # The projection screen is deliberately a bright, high-contrast region
    # containing the same synthetic face as the stage. The policy receives
    # ordinary face candidates and must rely on the volunteer's selected
    # continuity, never infer that a screen face is the speaker.
    screen = (1080, 280, 720, 520)
    screen_x, screen_y, screen_width, screen_height = screen
    screen_color = bytes([132, 138, 142]) * screen_width
    border_color = bytes([12, 14, 16]) * screen_width
    for row in range(screen_height):
        row_start = ((screen_y + row) * width + screen_x) * 3
        pixels[row_start : row_start + screen_width * 3] = (
            border_color if row < 8 or row >= screen_height - 8 else screen_color
        )
    for row in range(screen_height):
        for x in range(8):
            offset = ((screen_y + row) * width + screen_x + x) * 3
            pixels[offset : offset + 3] = bytes([12, 14, 16])
            offset = ((screen_y + row) * width + screen_x + screen_width - 1 - x) * 3
            pixels[offset : offset + 3] = bytes([12, 14, 16])

    def paste_portrait(x: int, brightness: float = 1.0) -> None:
        y = (height - portrait_height) // 2
        for row in range(portrait_height):
            source_start = row * portrait_width * 3
            target_start = ((y + row) * width + x) * 3
            row_pixels = portrait[source_start : source_start + portrait_width * 3]
            if brightness != 1.0:
                row_pixels = bytes(round(value * brightness) for value in row_pixels)
            pixels[target_start : target_start + portrait_width * 3] = row_pixels

    if include_stage_speaker:
        paste_portrait(320, speaker_brightness)
    if include_band_member:
        # Keep a separate synthetic candidate onstage while the main speaker
        # is dimmed or absent. It represents a band member behind the speaker,
        # not a face shown on the projection screen.
        paste_portrait(720)
    paste_portrait(1280)
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
    def test_4k_input_scale_is_validated_and_recorded_for_1080p_output(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        self.assertTrue(portrait.is_file(), f"missing synthetic person fixture: {portrait}")

        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            runs = {}
            for scale, label in ((1, "1080p"), (2, "4k")):
                frame = temporary / f"single-speaker-{label}.ppm"
                manifest = temporary / f"{label}-frames.tsv"
                trace = temporary / f"{label}-trace.jsonl"
                write_single_speaker_scene(frame, portrait, scale)
                manifest.write_text(
                    "monotonic_ms\tframe_path\n"
                    # Seed the detector scan quickly, then retain a 25 fps
                    # update cadence. Leave enough frames for the asynchronous
                    # detector and correlation tracker to publish a selectable
                    # candidate even on slower or heavily loaded CI runners.
                    + "".join(
                        f"{100 + frame_index * 40}\t{frame.name}\n" for frame_index in range(16)
                    ),
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
                        "--fps",
                        "1",
                        "--detection-scale",
                        str(scale),
                        "--operator-select-first-candidate-at-ms",
                        "380",
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                failure_context = result.stderr
                if result.returncode != 0 and trace.is_file():
                    diagnostics = [
                        json.loads(line)
                        for line in trace.read_text(encoding="utf-8").splitlines()
                        if '"kind":"tracker_diagnostics"' in line
                    ]
                    failure_context += f"\nDetector/tracker diagnostics: {diagnostics}"
                self.assertEqual(result.returncode, 0, f"{label} replay failed: {failure_context}")

                events = [
                    json.loads(line) for line in trace.read_text(encoding="utf-8").splitlines()
                ]
                diagnostics = [event for event in events if event["kind"] == "tracker_diagnostics"]
                candidates = [event for event in events if event["kind"] == "candidate"]
                commands = [event for event in events if event["kind"] == "command"]
                summary = next(event for event in events if event["kind"] == "summary")
                self.assertTrue(diagnostics)
                self.assertEqual(
                    {
                        (event["source_width"], event["source_height"])
                        for event in diagnostics
                    },
                    {(1920 * scale, 1080 * scale)},
                )
                self.assertEqual(
                    {
                        (event["detector_input_width"], event["detector_input_height"])
                        for event in diagnostics
                    },
                    {(1920, 1080)},
                )
                self.assertTrue(all(event["detection_scale"] == scale for event in diagnostics))
                self.assertTrue(all(event["scaling_mode"] == "rgb_stride" for event in diagnostics))
                self.assertGreater(max(event["detector_work_count"] for event in diagnostics), 0)
                self.assertGreater(max(event["detection_count"] for event in diagnostics), 0)
                self.assertTrue(candidates, f"{label} run published no tracking candidate")
                self.assertEqual(len(commands), 1)
                self.assertTrue(summary["operator_selection_applied"])
                self.assertEqual(
                    commands[0]["snapshot"]["selected_handle"], commands[0]["candidate_handle"]
                )
                selected = next(
                    event["candidate"]
                    for event in candidates
                    if event["frame_index"] == commands[0]["frame_index"]
                    and event["candidate"]["handle"] == commands[0]["candidate_handle"]
                )
                runs[label] = {
                    "selected_rect": selected["rect"],
                    "crop": commands[0]["snapshot"]["source_crop"],
                    "initial_tick": next(event for event in events if event["kind"] == "tick"),
                }

            for resolution, run in runs.items():
                x, y, width, height = run["selected_rect"]
                self.assertAlmostEqual(x + width / 2, 0.5, delta=0.08, msg=resolution)
                self.assertAlmostEqual(y + height / 2, 0.5, delta=0.08, msg=resolution)
            for actual, expected in zip(runs["4k"]["selected_rect"], runs["1080p"]["selected_rect"]):
                self.assertAlmostEqual(actual, expected, delta=0.005)
            for actual, expected in zip(runs["4k"]["crop"], runs["1080p"]["crop"]):
                self.assertAlmostEqual(actual, expected, delta=0.005)
            self.assertEqual(runs["4k"]["initial_tick"]["snapshot"]["source_crop"], [0, 0, 1, 1])
            self.assertEqual(
                runs["4k"]["initial_tick"]["snapshot"]["output_viewport"], [0, 0, 1, 1]
            )

            invalid_scale = subprocess.run(
                [
                    str(self.binary),
                    "--model",
                    str(self.model),
                    "--manifest",
                    str(temporary / "4k-frames.tsv"),
                    "--detection-scale",
                    "0",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(invalid_scale.returncode, 2)
            self.assertIn("integer from 1 through 16", invalid_scale.stderr)

            too_small_frame = temporary / "too-small-after-scale.ppm"
            too_small_manifest = temporary / "too-small-frames.tsv"
            write_black_ppm(too_small_frame, 160, 80)
            too_small_manifest.write_text(
                "monotonic_ms\tframe_path\n0\ttoo-small-after-scale.ppm\n",
                encoding="utf-8",
            )
            too_small = subprocess.run(
                [
                    str(self.binary),
                    "--model",
                    str(self.model),
                    "--manifest",
                    str(too_small_manifest),
                    "--detection-scale",
                    "2",
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(too_small.returncode, 3)
            self.assertIn("below 80x80 pixels", too_small.stderr)

    def test_4k_edge_targets_keep_selected_crop_inside_source_bounds(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        self.assertTrue(portrait.is_file(), f"missing synthetic person fixture: {portrait}")

        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            edge_scenes = (
                ("top-left", 0, 0),
                ("top-right", 1600, 0),
                ("bottom-left", 0, 720),
                ("bottom-right", 1600, 720),
            )
            for label, left, top in edge_scenes:
                with self.subTest(corner=label):
                    frame = temporary / f"{label}-edge-4k.ppm"
                    manifest = temporary / f"{label}-edge-frames.tsv"
                    trace = temporary / f"{label}-edge-trace.jsonl"
                    write_single_speaker_scene(frame, portrait, scale=2, left=left, top=top)
                    manifest.write_text(
                        "monotonic_ms\tframe_path\n"
                        + "".join(
                            f"{100 + frame_index * 40}\t{frame.name}\n"
                            for frame_index in range(8)
                        ),
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
                            "--fps",
                            "1",
                            "--detection-scale",
                            "2",
                            "--operator-select-first-candidate-at-ms",
                            "380",
                        ],
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    events = [
                        json.loads(line)
                        for line in trace.read_text(encoding="utf-8").splitlines()
                    ]
                    candidates = [event for event in events if event["kind"] == "candidate"]
                    commands = [event for event in events if event["kind"] == "command"]
                    self.assertTrue(candidates, f"{label}-edge replay published no candidate")
                    self.assertEqual(len(commands), 1)
                    self.assertEqual(commands[0]["snapshot"]["state"], "locked")
                    selected = next(
                        event["candidate"]
                        for event in candidates
                        if event["frame_index"] == commands[0]["frame_index"]
                        and event["candidate"]["handle"] == commands[0]["candidate_handle"]
                    )
                    x, y, width, height = selected["rect"]
                    crop_x, crop_y, crop_width, crop_height = commands[0]["snapshot"][
                        "source_crop"
                    ]
                    self.assertGreater(width, 0.0)
                    self.assertGreater(height, 0.0)
                    self.assertGreaterEqual(x, 0.0)
                    self.assertGreaterEqual(y, 0.0)
                    self.assertLessEqual(float32_sum(x, width), 1.0)
                    self.assertLessEqual(float32_sum(y, height), 1.0)
                    self.assertGreater(crop_width, 0.0)
                    self.assertGreater(crop_height, 0.0)
                    self.assertGreaterEqual(crop_x, 0.0)
                    self.assertGreaterEqual(crop_y, 0.0)
                    # Trace decimals round-trip through f64. Check their
                    # original f32 ABI arithmetic at the source boundary.
                    self.assertLessEqual(float32_sum(crop_x, crop_width), 1.0)
                    self.assertLessEqual(float32_sum(crop_y, crop_height), 1.0)
                    self.assertLessEqual(crop_x, x + 0.00001)
                    self.assertLessEqual(crop_y, y + 0.00001)
                    self.assertGreaterEqual(
                        float32_sum(crop_x, crop_width), float32_sum(x, width)
                    )
                    self.assertGreaterEqual(
                        float32_sum(crop_y, crop_height), float32_sum(y, height)
                    )
                    if left == 0:
                        self.assertLessEqual(x, 0.1)
                    else:
                        self.assertGreaterEqual(float32_sum(x, width), 0.9)
                    if top == 0:
                        self.assertLessEqual(y, 0.1)
                    else:
                        self.assertGreaterEqual(float32_sum(y, height), 0.9)

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
                    "--detection-scale",
                    "2",
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
            self.assertTrue(
                all(
                    (
                        event["source_width"],
                        event["source_height"],
                        event["detector_input_width"],
                        event["detector_input_height"],
                        event["detection_scale"],
                    )
                    == (320, 360, 160, 180, 2)
                    for event in diagnostics
                )
            )
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

    def test_low_light_speaker_band_member_and_projection_do_not_cause_target_switch(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            both = temporary / "speaker-band-and-projection.ppm"
            low_light = temporary / "low-light-speaker-band-and-projection.ppm"
            right_only = temporary / "band-and-projection-only.ppm"
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            write_stage_projection_scene(
                both, portrait, include_stage_speaker=True, include_band_member=True
            )
            write_stage_projection_scene(
                low_light,
                portrait,
                include_stage_speaker=True,
                speaker_brightness=0.08,
                include_band_member=True,
            )
            write_stage_projection_scene(
                right_only, portrait, include_stage_speaker=False, include_band_member=True
            )

            rows = ["monotonic_ms\tframe_path\n"]
            for frame_index in range(230):
                if frame_index < 100 or frame_index >= 180:
                    frame = both
                elif frame_index < 140:
                    frame = low_light
                elif frame_index < 180:
                    frame = right_only
                else:
                    frame = both
                rows.append(f"{frame_index * 40}\t{frame}\n")
            manifest.write_text("".join(rows), encoding="utf-8")

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
                    "--operator-select-leftmost-candidate-at-ms",
                    "2200",
                    "--operator-min-candidate-count",
                    "3",
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
                candidate_events = [
                    json.loads(line)
                    for line in trace.read_text(encoding="utf-8").splitlines()
                    if '"kind":"candidate"' in line
                ]
                command_events = [
                    json.loads(line)
                    for line in trace.read_text(encoding="utf-8").splitlines()
                    if '"kind":"command"' in line
                ]
                selection_frames = {event["frame_index"] for event in command_events}
                selection_candidates = [
                    event
                    for event in candidate_events
                    if event["frame_index"] in selection_frames
                ]
                failure_context += (
                    f"\nDetector/tracker diagnostics: {diagnostic_events}"
                    f"\nCandidate evidence: {candidate_events[-12:]}"
                    f"\nOperator command trace: {command_events}"
                    f"\nCandidates at selection: {selection_candidates}"
                )
            self.assertEqual(result.returncode, 0, failure_context)

            trace_text = trace.read_text(encoding="utf-8")
            events = [json.loads(line) for line in trace_text.splitlines()]
            candidates = [event for event in events if event["kind"] == "candidate"]
            commands = [event for event in events if event["kind"] == "command"]
            self.assertEqual(len(commands), 1)
            selected_handle = commands[0]["candidate_handle"]
            selected_candidate = next(
                event["candidate"]
                for event in candidates
                if event["candidate"]["handle"] == selected_handle
            )
            self.assertLess(
                selected_candidate["rect"][0] + selected_candidate["rect"][2] / 2,
                0.35,
                "the explicit selection must resolve to the stage speaker, not the band member; "
                f"selected candidate: {selected_candidate!r}",
            )
            selected_continuity = selected_candidate["continuity_token"]
            selected_center_x = (
                selected_candidate["rect"][0] + selected_candidate["rect"][2] / 2
            )
            selected_track = [
                event
                for event in candidates
                if event["candidate"]["continuity_token"] == selected_continuity
            ]
            for event in selected_track:
                if event["candidate"]["detector_confirmed_at_ms"] == 0:
                    continue
                center_x = (
                    event["candidate"]["rect"][0]
                    + event["candidate"]["rect"][2] / 2
                )
                if abs(center_x - selected_center_x) < 0.12:
                    continue
                event_index = events.index(event)
                previous_snapshot = next(
                    prior["snapshot"]
                    for prior in reversed(events[:event_index])
                    if "snapshot" in prior
                )
                self.assertEqual(
                    event["snapshot"]["state"],
                    "lost",
                    "a detector-confirmed face far from the last selected position must not retain the lock",
                )
                self.assertIsNone(
                    event["snapshot"]["selected_handle"],
                    "spatially ambiguous evidence must require operator selection",
                )
                self.assertEqual(event["snapshot"]["crop_action"], "hold")
                self.assertEqual(
                    event["snapshot"]["source_crop"],
                    previous_snapshot["source_crop"],
                    "spatially ambiguous evidence changed the held framing",
                )
            detector_misses = [
                event
                for event in selected_track
                if 100 <= event["frame_index"] < 180
                if event["candidate"]["detector_confirmed_at_ms"] == 0
            ]
            projected_face_candidates = [
                event
                for event in candidates
                if 100 <= event["frame_index"] < 160
                and event["candidate"]["detector_confirmed_at_ms"] != 0
                and event["candidate"]["rect"][0] + event["candidate"]["rect"][2] / 2 > 0.6
            ]
            band_member_candidates = [
                event
                for event in candidates
                if 140 <= event["frame_index"] < 180
                and event["candidate"]["detector_confirmed_at_ms"] != 0
                and 0.35
                < event["candidate"]["rect"][0] + event["candidate"]["rect"][2] / 2
                < 0.6
            ]
            self.assertTrue(
                projected_face_candidates,
                "the face remaining on the projection screen should be visible as an unselected decoy",
            )
            self.assertTrue(
                band_member_candidates,
                "the band member should remain a detected, unselected stage face while the speaker is absent",
            )
            if detector_misses:
                first_hold = detector_misses[0]
                self.assertEqual(first_hold["snapshot"]["reason"], "target_detector_unconfirmed")
            else:
                # The production detector and correlation trackers run on
                # worker threads. Some platforms retire a tracker after an
                # occlusion before it can publish an unconfirmed candidate;
                # in that case the session's evidence-age timer must still
                # move to SEARCHING and hold the crop. The deterministic Rust
                # policy test exercises the explicit unconfirmed-candidate
                # path without depending on detector scheduling.
                stale_holds = [
                    event
                    for event in events
                    if 100 <= event.get("frame_index", -1) < 180
                    and event.get("kind") in ("tick", "candidate")
                    and event["snapshot"]["state"] == "searching"
                    and event["snapshot"]["reason"] == "target_evidence_stale"
                    and event["snapshot"]["crop_action"] == "hold"
                ]
                if not stale_holds:
                    tick_timeline = [
                        (
                            event["frame_index"],
                            event["snapshot"]["state"],
                            event["snapshot"]["reason"],
                            event["snapshot"]["crop_action"],
                            event["snapshot"]["selected_handle"],
                        )
                        for event in events
                        if 100 <= event.get("frame_index", -1) < 180
                        and event.get("kind") == "tick"
                    ]
                    self.fail(
                        "a retired selected tracker must age into SEARCHING and hold framing during "
                        f"occlusion; state timeline: {tick_timeline!r}"
                    )
                first_hold = stale_holds[0]

            self.assertGreaterEqual(first_hold["frame_index"], 100)
            self.assertLess(first_hold["frame_index"], 180)
            self.assertEqual(first_hold["snapshot"]["state"], "searching")
            self.assertEqual(first_hold["snapshot"]["crop_action"], "hold")
            handle_continuity = {
                event["candidate"]["handle"]: event["candidate"]["continuity_token"]
                for event in candidates
            }

            # After framing is held, only detector-confirmed evidence for the
            # selected continuity may move it again. An unrelated face cannot
            # change the crop or become selected, even when snapshots retain
            # the previous crop-action label.
            first_hold_index = events.index(first_hold)
            for index, event in enumerate(events[first_hold_index + 1 :], first_hold_index + 1):
                if "snapshot" not in event:
                    continue
                previous_snapshot = next(
                    prior["snapshot"]
                    for prior in reversed(events[:index])
                    if "snapshot" in prior
                )
                candidate = event.get("candidate")
                confirmed_same_target = (
                    event.get("kind") == "candidate"
                    and candidate["continuity_token"] == selected_continuity
                    and candidate["detector_confirmed_at_ms"] != 0
                )
                center_x = (
                    candidate["rect"][0] + candidate["rect"][2] / 2
                    if confirmed_same_target
                    else selected_center_x
                )
                spatially_ambiguous = confirmed_same_target and abs(center_x - selected_center_x) >= 0.12
                if not confirmed_same_target or spatially_ambiguous:
                    self.assertEqual(
                        event["snapshot"]["source_crop"],
                        previous_snapshot["source_crop"],
                        "unconfirmed, unrelated, or spatially ambiguous evidence changed the held crop",
                    )
                if spatially_ambiguous:
                    self.assertEqual(event["snapshot"]["state"], "lost")
                    self.assertIsNone(event["snapshot"]["selected_handle"])
                selected_after_event = event["snapshot"]["selected_handle"]
                if selected_after_event is not None:
                    self.assertEqual(
                        handle_continuity[selected_after_event],
                        selected_continuity,
                        "the session switched to an unrelated face",
                    )

            reappeared_left = [
                event
                for event in candidates
                if event["at_ms"] >= 6400
                and event["candidate"]["detector_confirmed_at_ms"] != 0
                and event["candidate"]["rect"][0] + event["candidate"]["rect"][2] / 2 < 0.5
                and event["candidate"]["continuity_token"] != selected_continuity
            ]
            for event in reappeared_left:
                self.assertNotEqual(event["snapshot"]["selected_handle"], event["candidate"]["handle"])
                previous_snapshot = next(
                    prior["snapshot"]
                    for prior in reversed(events[: events.index(event)])
                    if "snapshot" in prior
                )
                self.assertEqual(
                    event["snapshot"]["source_crop"],
                    previous_snapshot["source_crop"],
                    "a returning face with new continuity changed the selected crop",
                )
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
                    "--detection-scale",
                    "2",
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
            self.assertLess(min(center_x), 0.4)
            self.assertGreater(max(center_x), 0.6)
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
            crop_failure_details = [
                {
                    "frame_index": event["frame_index"],
                    "at_ms": event["at_ms"],
                    "target_center_x": round(
                        event["candidate"]["rect"][0]
                        + event["candidate"]["rect"][2] / 2,
                        4,
                    ),
                    "detector_confirmed_at_ms": event["candidate"][
                        "detector_confirmed_at_ms"
                    ],
                    "state": event["snapshot"]["state"],
                    "reason": event["snapshot"]["reason"],
                    "crop_action": event["snapshot"]["crop_action"],
                    "source_crop": event["snapshot"]["source_crop"],
                }
                for index, event in enumerate(selected_candidates)
                if index % 10 == 0 or index == len(selected_candidates) - 1
            ]
            self.assertGreater(
                crop_positions[-1],
                crop_positions[0],
                json.dumps(
                    {
                        "summary": summary["snapshot"],
                        "sampled_selected_history": crop_failure_details,
                    },
                    sort_keys=True,
                ),
            )
            self.assertIn(
                summary["snapshot"]["state"],
                ("locked", "searching"),
                {
                    "summary": summary,
                    "selected_history": [
                        (
                            event["frame_index"],
                            event["candidate"]["rect"],
                            event["candidate"]["detector_confirmed_at_ms"],
                            event["snapshot"]["state"],
                            event["snapshot"]["reason"],
                            event["snapshot"]["selected_handle"],
                        )
                        for event in selected_candidates
                        if event["snapshot"]["state"] != "locked"
                    ],
                },
            )
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

    def test_crossing_faces_invalidate_ambiguous_selected_tracker(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        with tempfile.TemporaryDirectory() as directory:
            temporary = Path(directory)
            manifest = temporary / "frames.tsv"
            trace = temporary / "trace.jsonl"
            stationary_member_x = 840
            frame_for_moving_x = {}
            manifest_rows = ["monotonic_ms\tframe_path\n"]
            for frame_index in range(210):
                if frame_index < 70:
                    moving_speaker_x = 320
                else:
                    moving_speaker_x = min(
                        1200, 320 + ((frame_index - 70) // 3 + 1) * 40
                    )

                frame = frame_for_moving_x.get(moving_speaker_x)
                if frame is None:
                    frame = temporary / f"crossing-x-{moving_speaker_x}.ppm"
                    write_cartoon_faces_scene(
                        frame,
                        portrait,
                        (moving_speaker_x, stationary_member_x),
                    )
                    frame_for_moving_x[moving_speaker_x] = frame
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
                    "--detection-scale",
                    "2",
                    "--operator-select-leftmost-candidate-at-ms",
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
            selected_handle = commands[0]["candidate_handle"]
            selected_candidate = next(
                event["candidate"]
                for event in candidates
                if event["candidate"]["handle"] == selected_handle
            )
            selected_continuity = selected_candidate["continuity_token"]
            self.assertLess(
                selected_candidate["rect"][0] + selected_candidate["rect"][2] / 2,
                0.5,
                "the volunteer should initially select the moving left speaker",
            )

            handle_continuity = {
                event["candidate"]["handle"]: event["candidate"]["continuity_token"]
                for event in candidates
            }
            for event in events:
                if "snapshot" not in event:
                    continue
                selected_after_event = event["snapshot"]["selected_handle"]
                if selected_after_event is not None:
                    self.assertEqual(
                        handle_continuity[selected_after_event],
                        selected_continuity,
                        "the session must never switch continuity to the stationary stage member",
                    )

            # Both cartoon faces are deliberately identical. Once they overlap,
            # image evidence cannot prove which physical person owns a tracker.
            # The safe result is to hold framing and wait for a fresh volunteer
            # selection, even if this misses continued following of the speaker.
            self.assertIn(
                summary["snapshot"]["state"],
                ("searching", "lost"),
                "ambiguous overlapping faces must invalidate the selected lock",
            )
            self.assertEqual(summary["snapshot"]["crop_action"], "hold", summary)
            safety_entry = next(
                (index, event)
                for index, event in enumerate(events)
                if event.get("frame_index", -1) >= 100
                and event.get("snapshot", {}).get("state") in ("searching", "lost")
            )
            held_crop = safety_entry[1]["snapshot"]["source_crop"]
            for event in events[safety_entry[0] :]:
                if "snapshot" in event:
                    self.assertEqual(
                        event["snapshot"]["source_crop"],
                        held_crop,
                        "framing must remain held after ambiguous tracker retirement",
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
            self.assertIn("No detector-confirmed preview candidate was available", result.stderr)
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
