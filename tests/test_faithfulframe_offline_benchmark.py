import importlib.util
import json
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "tools" / "faithfulframe_offline_benchmark.py"
SPEC = importlib.util.spec_from_file_location("faithfulframe_offline_benchmark", SCRIPT)
assert SPEC and SPEC.loader
benchmark = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(benchmark)


def diagnostic(frame_index, work_count, work_us, *, width=3840, height=2160, scale=2):
    return {
        "kind": "tracker_diagnostics",
        "frame_index": frame_index,
        "at_ms": frame_index * 40,
        "source_width": width,
        "source_height": height,
        "detector_input_width": width // scale,
        "detector_input_height": height // scale,
        "detection_scale": scale,
        "scaling_mode": "rgb_stride",
        "detector_work_count": work_count,
        "detector_work_us": work_us,
        "detection_count": frame_index % 3,
        "candidate_count": frame_index % 2,
    }


def process_summary(**overrides):
    summary = {
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
