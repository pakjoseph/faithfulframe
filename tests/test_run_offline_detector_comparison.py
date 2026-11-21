#!/usr/bin/env python3
"""Test paired synthetic dlib backend measurement preparation."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "run_offline_detector_comparison.py"
SPEC = importlib.util.spec_from_file_location("run_offline_detector_comparison", SCRIPT)
comparison = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(comparison)


class OfflineDetectorComparisonTests(unittest.TestCase):
    def test_4k_scene_is_derived_from_the_non_identifiable_fixture(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        with tempfile.TemporaryDirectory() as temporary:
            scene = Path(temporary) / "scene.ppm"
            comparison._write_4k_scene(portrait, scene)
            magic, dimensions, maximum, pixels = scene.read_bytes().split(b"\n", 3)

        self.assertEqual(magic, b"P6")
        self.assertEqual(dimensions, b"3840 2160")
        self.assertEqual(maximum, b"255")
        self.assertEqual(len(pixels), 3840 * 2160 * 3)
        self.assertEqual(pixels[:3], bytes((32, 38, 42)))
        self.assertGreater(len(set(pixels)), 3)

    def test_manifest_repeats_one_4k_frame_with_monotonic_timestamps(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            scene = directory / "scene.ppm"
            scene.write_bytes(b"synthetic")
            manifest = directory / "manifest.tsv"
            comparison._write_manifest(scene, manifest)
            rows = manifest.read_text(encoding="utf-8").splitlines()

        self.assertEqual(rows[0], "monotonic_ms\tframe_path")
        self.assertEqual(len(rows) - 1, comparison.FRAME_COUNT)
        timestamps = [int(row.split("\t", 1)[0]) for row in rows[1:]]
        self.assertEqual(timestamps[0], 0)
        self.assertEqual(timestamps[-1], (comparison.FRAME_COUNT - 1) * comparison.FRAME_STEP_MS)
        self.assertTrue(all(right > left for left, right in zip(timestamps, timestamps[1:])))
        self.assertTrue(all(row.endswith(str(scene.resolve())) for row in rows[1:]))

    def test_paired_reports_require_identical_host_and_workload(self):
        shared = {
            "host": {"cpu_architecture": "x86_64"},
            "benchmark_tool_source_revision": {"commit": "abc123", "working_tree_dirty": False},
            "replay_binary": {"filename": "replay", "sha256": "same"},
            "manifest": {"sha256": "same"},
            "workload": {
                "source_resolution": [3840, 2160],
                "output_resolution": [1920, 1080],
                "fps": 25,
                "label": "synthetic",
            },
        }
        def report(engine: str) -> dict:
            return {
                **shared,
                "detector_engine": engine,
                "production_acceptance_eligible": False,
                "model": {"sha256": engine},
                "scales": [{"detection_scale": 2, "summary": {"detector_work_median_us": 1}}],
            }

        paired = comparison._paired_summary(report("dlib_hog"), report("dlib_cnn"))
        self.assertFalse(paired["production_acceptance_eligible"])
        self.assertEqual(paired["results"]["dlib_hog"][0]["detection_scale"], 2)
        self.assertIn("unmodified upstream", paired["not_measured"][-1])

        mismatched = report("dlib_cnn")
        mismatched["host"] = {"cpu_architecture": "arm64"}
        with self.assertRaisesRegex(comparison.ComparisonError, "shared host"):
            comparison._paired_summary(report("dlib_hog"), mismatched)

        mismatched = report("dlib_cnn")
        mismatched["manifest"] = {"sha256": "different"}
        with self.assertRaisesRegex(comparison.ComparisonError, "shared manifest"):
            comparison._paired_summary(report("dlib_hog"), mismatched)


if __name__ == "__main__":
    unittest.main()
