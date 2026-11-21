#!/usr/bin/env python3
"""Test paired synthetic dlib backend measurement preparation."""

from __future__ import annotations

import hashlib
import importlib.util
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
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
        self.assertTrue(all(row.split("\t", 1)[1] == scene.name for row in rows[1:]))

    def test_manifest_hash_is_stable_across_temporary_directories(self):
        with tempfile.TemporaryDirectory() as first, tempfile.TemporaryDirectory() as second:
            manifests = []
            for directory in (Path(first), Path(second)):
                scene = directory / "synthetic-4k-scene.ppm"
                scene.write_bytes(b"same deterministic frame")
                manifest = directory / "manifest.tsv"
                comparison._write_manifest(scene, manifest)
                manifests.append(manifest.read_bytes())

        self.assertEqual(manifests[0], manifests[1])

    def test_input_artifacts_capture_exact_content_digests(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            portrait = directory / "portrait.ppm"
            portrait.write_bytes(b"synthetic portrait")
            scene = directory / "scene.ppm"
            scene.write_bytes(b"generated scene")
            artifacts = comparison._input_artifacts(portrait, scene)

        self.assertEqual(
            artifacts["synthetic_portrait_fixture"]["sha256"],
            hashlib.sha256(b"synthetic portrait").hexdigest(),
        )
        self.assertEqual(
            artifacts["generated_source_frame"]["sha256"],
            hashlib.sha256(b"generated scene").hexdigest(),
        )
        self.assertEqual(artifacts["generated_source_frame"]["size_bytes"], len(b"generated scene"))

    def test_replay_source_revision_is_forwarded_to_benchmark(self):
        with tempfile.TemporaryDirectory() as temporary:
            output_dir = Path(temporary)
            report_path = output_dir / "dlib_hog.json"

            def run(command, **_kwargs):
                revision_index = command.index("--binary-source-revision")
                self.assertEqual(command[revision_index + 1], "commit-123")
                report_path.write_text("{}", encoding="utf-8")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            with patch.object(comparison.subprocess, "run", side_effect=run):
                report = comparison._run_backend(
                    Path("benchmark.py"),
                    Path("replay"),
                    Path("model.dat"),
                    "dlib_hog",
                    Path("manifest.tsv"),
                    output_dir,
                    1,
                    2.0,
                    "commit-123",
                )

        self.assertEqual(report, {})

    def test_paired_reports_require_identical_host_and_workload(self):
        shared = {
            "host": {"cpu_architecture": "x86_64"},
            "benchmark_tool_source_revision": {"commit": "abc123", "working_tree_dirty": False},
            "replay_binary": {
                "filename": "replay",
                "sha256": "same",
                "build_source_revision": "abc123",
                "build_source_revision_status": "operator_supplied_unverified",
            },
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
                "scales": [
                    {"detection_scale": 2, "summary": {"detector_work_median_us": 1}},
                    {"detection_scale": 4, "summary": {"detector_work_median_us": 1}},
                ],
            }

        inputs = {"generated_source_frame": {"sha256": "scene-hash"}}
        paired = comparison._paired_summary(report("dlib_hog"), report("dlib_cnn"), inputs)
        self.assertEqual(paired["schema_version"], comparison.SCHEMA_VERSION)
        self.assertFalse(paired["production_acceptance_eligible"])
        self.assertEqual(paired["input_artifacts"], inputs)
        self.assertEqual(
            [scale["detection_scale"] for scale in paired["results"]["dlib_hog"]], [2, 4]
        )
        self.assertIn("unmodified upstream", paired["not_measured"][-1])

        mismatched = report("dlib_cnn")
        mismatched["host"] = {"cpu_architecture": "arm64"}
        with self.assertRaisesRegex(comparison.ComparisonError, "shared host"):
            comparison._paired_summary(report("dlib_hog"), mismatched, {})

        mismatched = report("dlib_cnn")
        mismatched["manifest"] = {"sha256": "different"}
        with self.assertRaisesRegex(comparison.ComparisonError, "shared manifest"):
            comparison._paired_summary(report("dlib_hog"), mismatched, {})

        mismatched = report("dlib_cnn")
        mismatched["scales"].reverse()
        with self.assertRaisesRegex(comparison.ComparisonError, "same detector scale"):
            comparison._paired_summary(report("dlib_hog"), mismatched, {})


if __name__ == "__main__":
    unittest.main()
