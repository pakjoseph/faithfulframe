import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "faithfulframe_offline_benchmark.py"
SPEC = importlib.util.spec_from_file_location("faithfulframe_offline_benchmark", SCRIPT)
assert SPEC and SPEC.loader
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


def diagnostic(frame_index, work_count, work_us, *, width=3840, height=2160, scale=2, interval_ms=2000):
    return {
        "kind": "tracker_diagnostics",
        "frame_index": frame_index,
        "at_ms": frame_index * 40,
        "source_width": width,
        "source_height": height,
        "detector_input_width": width // scale,
        "detector_input_height": height // scale,
        "detector_engine": "dlib_hog",
        "detection_scale": scale,
        "detector_interval_ms": interval_ms,
        "scaling_mode": "rgb_stride",
        "detector_work_count": work_count,
        "detector_work_us": work_us,
        "detection_count": frame_index % 3,
        "candidate_count": frame_index % 2,
    }


def process_summary(**overrides):
    summary = {
        "detector_engine": "dlib_hog",
        "detector_interval_ms": 2000,
        "process_wall_us": 1000,
        "process_cpu_valid": True,
        "process_cpu_us": 250,
        "process_cpu_percent_valid": True,
        "process_cpu_percent": 25.0,
        "memory_valid": True,
        "resident_start_bytes": 1000,
        "resident_bytes": 1200,
        "peak_resident_bytes": 1250,
        "resident_growth_valid": True,
        "resident_growth_bytes": 200,
    }
    summary.update(overrides)
    return summary


class OfflineBenchmarkTests(unittest.TestCase):
    def write_trace(self, directory, events):
        trace = Path(directory) / "trace.jsonl"
        trace.write_text("".join(json.dumps(event) + "\n" for event in events), encoding="utf-8")
        return trace

    def test_binary_provenance_is_separate_from_benchmark_checkout(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "offline-session-replay"
            binary.write_bytes(b"replay binary fixture")

            unknown = benchmark._replay_binary_metadata(binary, None)
            supplied = benchmark._replay_binary_metadata(binary, "  abc123  ")

        self.assertIsNone(unknown["build_source_revision"])
        self.assertEqual(unknown["build_source_revision_status"], "unknown")
        self.assertEqual(supplied["build_source_revision"], "abc123")
        self.assertEqual(supplied["build_source_revision_status"], "operator_supplied_unverified")
        self.assertEqual(unknown["sha256"], supplied["sha256"])

    def test_source_provenance_ignores_untracked_build_artifacts_only(self):
        with tempfile.TemporaryDirectory() as directory:
            repository = Path(directory)

            def git(*args):
                subprocess.run(
                    ["git", "-C", str(repository), *args],
                    check=True,
                    capture_output=True,
                    text=True,
                )

            git("init", "--quiet")
            git("config", "user.name", "Test")
            git("config", "user.email", "test@example.invalid")
            tracked = repository / "tool.py"
            tracked.write_text("print('baseline')\n", encoding="utf-8")
            git("add", "tool.py")
            git("commit", "--quiet", "-m", "baseline")

            clean = benchmark._source_revision(repository)
            self.assertFalse(clean["tracked_working_tree_dirty"])

            build_output = repository / "build" / "generated.txt"
            build_output.parent.mkdir()
            build_output.write_text("generated", encoding="utf-8")
            untracked_build = benchmark._source_revision(repository)
            self.assertFalse(untracked_build["tracked_working_tree_dirty"])

            tracked.write_text("print('changed')\n", encoding="utf-8")
            modified_source = benchmark._source_revision(repository)
            self.assertTrue(modified_source["tracked_working_tree_dirty"])

    def test_benchmark_selects_existing_dlib_backend_explicitly(self):
        parser = benchmark._build_parser()
        args = parser.parse_args(
            [
                "--binary", "replay",
                "--model", "model.dat",
                "--manifest", "frames.tsv",
                "--output", "report.json",
                "--workload-label", "synthetic",
                "--detector-engine", "dlib_cnn",
            ]
        )
        self.assertEqual(args.detector_engine, "dlib_cnn")

    def test_benchmark_records_selected_detector_interval(self):
        parser = benchmark._build_parser()
        args = parser.parse_args(
            [
                "--binary", "replay",
                "--model", "model.dat",
                "--manifest", "frames.tsv",
                "--output", "report.json",
                "--workload-label", "synthetic",
                "--detector-interval-ms", "1250",
            ]
        )
        self.assertEqual(args.detector_interval_ms, 1250)

        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()):
            parser.parse_args(
                [
                    "--binary", "replay",
                    "--model", "model.dat",
                    "--manifest", "frames.tsv",
                    "--output", "report.json",
                    "--workload-label", "synthetic",
                    "--detector-interval-ms", "1125",
                ]
            )

    def test_rejects_trace_with_different_detector_interval(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0, interval_ms=2000),
                    diagnostic(1, 1, 1200, interval_ms=2000),
                    {
                        "kind": "summary",
                        "frames": 2,
                        "detection_scale": 2,
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "detector interval"):
                benchmark.summarize_trace(trace, 2, detector_interval_ms=1250)

    def test_summarizes_latency_and_numeric_tracking_diagnostics(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    {"kind": "candidate", "candidate": {"handle": 101}},
                    diagnostic(1, 1, 1200),
                    diagnostic(2, 1, 1200),
                    diagnostic(3, 2, 1800),
                    {
                        "kind": "summary",
                        "frames": 4,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            result = benchmark.summarize_trace(trace, 2)

        self.assertEqual(result["source_resolution"], [3840, 2160])
        self.assertEqual(result["detector_input_resolution"], [1920, 1080])
        self.assertEqual(result["detector_engine"], "dlib_hog")
        self.assertEqual(result["detector_interval_ms"], 2000)
        self.assertEqual(result["frame_count"], 4)
        self.assertEqual(result["simulated_duration_ms"], 120)
        self.assertEqual(result["detector_work_samples_us"], [1200, 1800])
        self.assertEqual(result["detector_work_median_us"], 1500)
        self.assertEqual(result["detector_work_p95_us"], 1800)
        self.assertEqual(result["max_face_detections"], 2)
        self.assertEqual(result["max_tracking_candidates"], 1)
        self.assertEqual(result["candidate_observation_events"], 1)
        self.assertEqual(
            result["process_metrics"],
            {
                "process_wall_time_ms": 1.0,
                "process_cpu_time_ms": 0.25,
                "process_cpu_percent": 25.0,
                "process_cpu_valid": True,
                "resident_start_bytes": 1000,
                "resident_bytes": 1200,
                "peak_resident_bytes": 1250,
                "resident_growth_bytes": 200,
                "memory_valid": True,
            },
        )

    def test_rejects_inconsistent_process_metrics(self):
        invalid = [
            process_summary(process_cpu_percent=26.0),
            process_summary(resident_growth_bytes=201),
            process_summary(peak_resident_bytes=1100),
            process_summary(memory_valid=False),
        ]
        for process_metrics in invalid:
            with self.subTest(process_metrics=process_metrics), tempfile.TemporaryDirectory() as directory:
                trace = self.write_trace(
                    directory,
                    [
                        diagnostic(0, 0, 0),
                        diagnostic(1, 1, 100),
                        {
                            "kind": "summary",
                            "frames": 2,
                            "detection_scale": 2,
                            "scaling_mode": "rgb_stride",
                            **process_metrics,
                        },
                    ],
                )
                with self.assertRaises(benchmark.BenchmarkError):
                    benchmark.summarize_trace(trace, 2)

    def test_accepts_explicitly_unavailable_process_metrics(self):
        unavailable = process_summary(
            process_cpu_valid=False,
            process_cpu_us=None,
            process_cpu_percent_valid=False,
            process_cpu_percent=None,
            memory_valid=False,
            resident_start_bytes=None,
            resident_bytes=None,
            peak_resident_bytes=None,
            resident_growth_valid=False,
            resident_growth_bytes=None,
        )
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    diagnostic(1, 1, 100),
                    {
                        "kind": "summary",
                        "frames": 2,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **unavailable,
                    },
                ],
            )
            result = benchmark.summarize_trace(trace, 2)
        self.assertIsNone(result["process_metrics"]["process_cpu_percent"])
        self.assertFalse(result["process_metrics"]["memory_valid"])

    def test_percentile_uses_nearest_rank_and_handles_empty_samples(self):
        self.assertEqual(benchmark.nearest_rank_percentile([8, 2, 5, 1], 0.95), 8)
        self.assertIsNone(benchmark.nearest_rank_percentile([], 0.95))
        with self.assertRaises(ValueError):
            benchmark.nearest_rank_percentile([1], 0.0)

    def test_rejects_inconsistent_or_incomplete_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            wrong_scale = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0, scale=1),
                    {
                        "kind": "summary",
                        "frames": 1,
                        "detection_scale": 1,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "does not match"):
                benchmark.summarize_trace(wrong_scale, 2)

            skipped = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    diagnostic(1, 2, 900),
                    {
                        "kind": "summary",
                        "frames": 2,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "skipped detector completions"):
                benchmark.summarize_trace(skipped, 2)

    def test_rejects_detector_engine_mismatch_in_trace(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    diagnostic(1, 1, 100),
                    {
                        "kind": "summary",
                        "frames": 2,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "requested engine"):
                benchmark.summarize_trace(trace, 2, "dlib_cnn")

    def test_rejects_geometry_change_within_comparison_run(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    diagnostic(1, 1, 800, width=1920, height=1080),
                    {
                        "kind": "summary",
                        "frames": 2,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "resolution changed"):
                benchmark.summarize_trace(trace, 2)

    def test_rejects_runs_that_never_invoked_the_detector(self):
        with tempfile.TemporaryDirectory() as directory:
            trace = self.write_trace(
                directory,
                [
                    diagnostic(0, 0, 0),
                    {
                        "kind": "summary",
                        "frames": 1,
                        "detection_scale": 2,
                        "scaling_mode": "rgb_stride",
                        **process_summary(),
                    },
                ],
            )
            with self.assertRaisesRegex(benchmark.BenchmarkError, "no detector work sample"):
                benchmark.summarize_trace(trace, 2)

    def test_aggregates_repeats_only_when_workload_shape_matches(self):
        common = {
            "detector_engine": "dlib_hog",
            "detector_interval_ms": 2000,
            "source_resolution": [3840, 2160],
            "detector_input_resolution": [1920, 1080],
            "frame_count": 150,
            "simulated_duration_ms": 5960,
            "detector_work_samples_us": [100, 200],
            "max_face_detections": 2,
            "max_tracking_candidates": 2,
            "candidate_observation_events": 10,
            "process_metrics": {
                "process_wall_time_ms": 1000.0,
                "process_cpu_time_ms": 250.0,
                "process_cpu_percent": 25.0,
                "process_cpu_valid": True,
                "resident_start_bytes": 1000,
                "resident_bytes": 1200,
                "peak_resident_bytes": 1250,
                "resident_growth_bytes": 200,
                "memory_valid": True,
            },
        }
        first = dict(common, replay_wall_time_ms=1000.0)
        second = dict(common, replay_wall_time_ms=1200.0)
        result = benchmark._aggregate_runs([first, second])
        self.assertEqual(result["detector_work_samples_count"], 4)
        self.assertEqual(result["detector_work_median_us"], 150)
        self.assertEqual(result["detector_work_p95_us"], 200)
        self.assertEqual(result["replay_wall_time_median_ms"], 1100.0)
        self.assertEqual(result["process_cpu_percent_median"], 25.0)
        self.assertEqual(result["resident_growth_bytes_median"], 200)
        self.assertEqual(result["peak_resident_bytes_max"], 1250)

        changed = dict(second, frame_count=149)
        with self.assertRaisesRegex(benchmark.BenchmarkError, "changed between repeats"):
            benchmark._aggregate_runs([first, changed])


if __name__ == "__main__":
    unittest.main()
