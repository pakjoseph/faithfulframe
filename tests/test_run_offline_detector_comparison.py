#!/usr/bin/env python3
"""Test paired synthetic dlib backend measurement preparation."""

from __future__ import annotations

import hashlib
import importlib.util
import json
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
    def test_comparison_defaults_to_five_independent_repeats(self):
        args = comparison._build_parser().parse_args(
            ["--binary", "replay", "--output-dir", "reports"]
        )
        self.assertEqual(args.repeats, comparison.MIN_COMPARISON_REPEATS)

    def test_plugin_detector_default_is_explicitly_hog(self):
        manager_source_path = (
            Path(__file__).resolve().parents[1] / "src" / "face-tracker-manager.cpp"
        )
        manager_source = manager_source_path.read_text(encoding="utf-8")
        defaults_start = manager_source.index("void face_tracker_manager::get_defaults(obs_data_t *settings)")
        defaults = manager_source[defaults_start:]
        self.assertRegex(
            defaults,
            r'obs_data_set_default_int\(settings,\s*"detector_engine",\s*engine_dlib_hog\)',
        )

        manager_header_path = (
            Path(__file__).resolve().parents[1] / "src" / "face-tracker-manager.hpp"
        )
        manager_header = manager_header_path.read_text(encoding="utf-8")
        self.assertRegex(manager_header, r"engine_dlib_hog\s*=\s*0")
        self.assertRegex(manager_header, r"engine_dlib_cnn\s*=\s*1")

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

    def test_three_face_scene_spans_the_frame_and_is_deterministic(self):
        portrait = (
            Path(__file__).resolve().parent
            / "fixtures"
            / "offline-synthetic-person-320x360.ppm"
        )
        positions = comparison._face_positions(portrait, "three_face_stage")
        self.assertEqual(
            positions,
            ((0, 720), (1600, 720), (3200, 720)),
        )

        with tempfile.TemporaryDirectory() as temporary:
            first = Path(temporary) / "first.ppm"
            second = Path(temporary) / "second.ppm"
            comparison._write_4k_scene(portrait, first, positions)
            comparison._write_4k_scene(portrait, second, positions)
            first_bytes = first.read_bytes()
            second_bytes = second.read_bytes()

        self.assertEqual(first_bytes, second_bytes)
        header, pixels = first_bytes.split(b"255\n", 1)
        self.assertTrue(header.endswith(b"3840 2160\n"))
        portrait_width, portrait_height, portrait_pixels = comparison._read_portrait(portrait)
        center_source = ((portrait_height // 2) * portrait_width + portrait_width // 2) * 3
        expected = portrait_pixels[center_source : center_source + 3]
        for left, top in positions:
            center_pixel = ((top + portrait_height) * comparison.SOURCE_WIDTH + left + portrait_width) * 3
            self.assertEqual(pixels[center_pixel : center_pixel + 3], expected)

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
        self.assertEqual(timestamps[-1], 21960)
        self.assertEqual(comparison.MIN_DETECTOR_WORK_SAMPLES_PER_RUN, 10)
        self.assertEqual(comparison.MIN_COMPARISON_REPEATS, 5)

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
            artifacts = comparison._input_artifacts(
                portrait, scene, "single_face_center", ((1600, 720),)
            )

        self.assertEqual(
            artifacts["synthetic_portrait_fixture"]["sha256"],
            hashlib.sha256(b"synthetic portrait").hexdigest(),
        )
        self.assertEqual(
            artifacts["generated_source_frame"]["sha256"],
            hashlib.sha256(b"generated scene").hexdigest(),
        )
        self.assertEqual(artifacts["generated_source_frame"]["size_bytes"], len(b"generated scene"))
        self.assertEqual(artifacts["generation"]["layout"], "single_face_center")
        self.assertEqual(artifacts["generation"]["face_top_left_pixels"], [[1600, 720]])
        self.assertEqual(artifacts["generation"]["frames"], comparison.FRAME_COUNT)
        self.assertEqual(
            artifacts["generation"]["minimum_comparison_repeats"],
            comparison.MIN_COMPARISON_REPEATS,
        )
        self.assertEqual(
            artifacts["generation"]["minimum_detector_work_samples_per_run"],
            comparison.MIN_DETECTOR_WORK_SAMPLES_PER_RUN,
        )

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
                    "synthetic workload",
                )

        self.assertEqual(report, {})

    def test_main_indexes_both_synthetic_workloads(self):
        portrait_fixture = Path(__file__).resolve().parent / "fixtures" / "offline-synthetic-person-320x360.ppm"

        def fake_backend(
            _benchmark,
            _binary,
            model,
            engine,
            manifest,
            _output_dir,
            repeats,
            _timeout,
            revision,
            workload_label,
        ):
            digest = hashlib.sha256(manifest.read_bytes()).hexdigest()
            return {
                "host": {"cpu_architecture": "x86_64"},
                "benchmark_tool_source_revision": {"commit": "abc123", "tracked_working_tree_dirty": False},
                "replay_binary": {
                    "filename": "replay",
                    "sha256": "binary-sha",
                    "build_source_revision": revision,
                    "build_source_revision_status": "operator_supplied_unverified",
                },
                "manifest": {"sha256": digest},
                "workload": {
                    "source_resolution": [3840, 2160],
                    "output_resolution": [1920, 1080],
                    "fps": 25,
                    "label": workload_label,
                },
                "detector_engine": engine,
                "production_acceptance_eligible": False,
                "model": {"filename": model.name, "sha256": engine},
                "scales": [
                    {
                        "detection_scale": scale,
                        "summary": {
                            "repeats": repeats,
                            "detector_work_samples_count": 10 * repeats,
                            "simulated_duration_ms_per_run": 21960,
                            "detector_work_median_us": 100,
                            "detector_work_p95_us": 120,
                            "max_face_detections": 3,
                            "max_tracking_candidates": 3,
                            "process_cpu_percent_median": 50.0,
                            "peak_resident_bytes_max": 1000000,
                            "runs": [
                                {"detector_work_samples_us": [100 + index for index in range(10)]}
                                for _ in range(repeats)
                            ],
                        },
                    }
                    for scale in comparison.DETECTION_SCALES
                ],
            }

        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            binary = directory / "replay"
            hog_model = directory / "hog.dat"
            cnn_model = directory / "cnn.dat"
            for path in (binary, hog_model, cnn_model):
                path.write_bytes(b"placeholder")
            output = directory / "results"
            with patch.object(comparison, "_run_backend", side_effect=fake_backend):
                exit_code = comparison.main(
                    [
                        "--binary",
                        str(binary),
                        "--hog-model",
                        str(hog_model),
                        "--cnn-model",
                        str(cnn_model),
                        "--portrait",
                        str(portrait_fixture),
                        "--output-dir",
                        str(output),
                        "--repeats",
                        str(comparison.MIN_COMPARISON_REPEATS),
                        "--binary-source-revision",
                        "abc123",
                    ]
                )
            index = json.loads((output / "comparison.json").read_text(encoding="utf-8"))
            reports_exist = [
                (output / item["report"]).is_file() for item in index["workloads"]
            ]

        self.assertEqual(exit_code, 0)
        self.assertFalse(index["production_acceptance_eligible"])
        self.assertEqual(
            index["latency_sample_contract"]["minimum_detector_work_samples_per_run"], 10
        )
        self.assertEqual([item["layout"] for item in index["workloads"]], [
            "single_face_center",
            "three_face_stage",
        ])
        for item, report_exists in zip(index["workloads"], reports_exist):
            self.assertTrue(report_exists)
            self.assertEqual(item["input_artifacts"]["generation"]["layout"], item["layout"])
            self.assertEqual(len(item["results"]["dlib_hog"]), 2)
            for engine_results in item["results"].values():
                for scale_result in engine_results:
                    self.assertEqual(scale_result["detector_work_samples_count"], 50)
                    self.assertEqual(scale_result["detector_work_samples_per_run"], [10] * 5)
                    self.assertEqual(scale_result["simulated_duration_ms_per_run"], 21960)

    def test_paired_reports_require_identical_host_and_workload(self):
        shared = {
            "host": {"cpu_architecture": "x86_64"},
            "benchmark_tool_source_revision": {
                "commit": "abc123",
                "tracked_working_tree_dirty": False,
            },
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
                    {
                        "detection_scale": scale,
                        "summary": {
                            "repeats": 5,
                            "detector_work_samples_count": 50,
                            "detector_work_median_us": 1,
                            "runs": [
                                {"detector_work_samples_us": list(range(10))}
                                for _ in range(5)
                            ],
                        },
                    }
                    for scale in (2, 4)
                ],
            }

        inputs = {"generated_source_frame": {"sha256": "scene-hash"}}
        paired = comparison._paired_summary(report("dlib_hog"), report("dlib_cnn"), inputs)
        self.assertEqual(paired["schema_version"], comparison.SCHEMA_VERSION)
        self.assertFalse(paired["production_acceptance_eligible"])
        self.assertEqual(paired["input_artifacts"], inputs)
        self.assertIn("input_artifacts", paired["manifest"]["hash_scope"])
        self.assertEqual(
            [scale["detection_scale"] for scale in paired["results"]["dlib_hog"]], [2, 4]
        )
        self.assertIn("unmodified upstream", paired["not_measured"][-1])
        self.assertEqual(
            paired["latency_sample_contract"]["minimum_detector_work_samples_per_run"], 10
        )
        self.assertEqual(
            paired["latency_sample_contract"]["minimum_total_detector_work_samples"], 50
        )

        insufficient_repeats = report("dlib_hog")
        for scale in insufficient_repeats["scales"]:
            scale["summary"]["repeats"] = 4
            scale["summary"]["detector_work_samples_count"] = 40
            scale["summary"]["runs"].pop()
        with self.assertRaisesRegex(comparison.ComparisonError, "inconsistent run samples"):
            comparison._paired_summary(insufficient_repeats, report("dlib_cnn"), {})

        insufficient = report("dlib_hog")
        insufficient["scales"][0]["summary"]["runs"][1]["detector_work_samples_us"] = [1]
        insufficient["scales"][0]["summary"]["detector_work_samples_count"] = 41
        with self.assertRaisesRegex(comparison.ComparisonError, "fewer than 10 detector work samples"):
            comparison._paired_summary(insufficient, report("dlib_cnn"), {})

        inconsistent = report("dlib_hog")
        inconsistent["scales"][0]["summary"]["detector_work_samples_count"] = 49
        with self.assertRaisesRegex(comparison.ComparisonError, "aggregate sample count is inconsistent"):
            comparison._paired_summary(inconsistent, report("dlib_cnn"), {})

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
