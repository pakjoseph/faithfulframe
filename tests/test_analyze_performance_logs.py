import csv
from datetime import datetime, timedelta, timezone
import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "analyze_performance_logs.py"
SPEC = importlib.util.spec_from_file_location("analyze_performance_logs", SCRIPT)
assert SPEC and SPEC.loader
analyzer = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(analyzer)


def fixture_row(elapsed_ms, cpu, resident, render_frames, render_lag, plugin_version):
    row = {column: "" for column in analyzer.EXPECTED_COLUMNS}
    timestamp = (datetime(2026, 11, 12, 12, tzinfo=timezone.utc) + timedelta(milliseconds=elapsed_ms))
    utc_timestamp = timestamp.isoformat(timespec="milliseconds").replace("+00:00", "Z")
    row.update(
        {
            "schema_version": "3",
            "run_metadata_present": "0",
            "utc_timestamp": utc_timestamp,
            "elapsed_milliseconds": str(elapsed_ms),
            "source_id": "10",
            "session_id": "20",
            "source_generation": "1",
            "session_generation": "1",
            "stream_generation": "1",
            "geometry_generation": "1",
            "tracker_generation": "1",
            "status_version": str(elapsed_ms + 1),
            "state": "1",
            "reason": "2",
            "source_width": "3840",
            "source_height": "2160",
            "output_width": "1920",
            "output_height": "1080",
            "obs_video_configuration_available": "1",
            "obs_graphics_module": "libobs-opengl",
            "obs_fps_num": "30",
            "obs_fps_den": "1",
            "obs_base_width": "1920",
            "obs_base_height": "1080",
            "obs_canvas_output_width": "1280",
            "obs_canvas_output_height": "720",
            "obs_output_format": "2",
            "obs_colorspace": "2",
            "obs_range": "0",
            "obs_scale_type": "0",
            "obs_gpu_conversion": "1",
            "status_snapshot_age_milliseconds": "15",
            "process_cpu_percent": str(cpu),
            "resident_bytes": str(resident),
            "virtual_bytes": str(resident * 2),
            "active_fps": "59.9",
            "render_frames": str(render_frames),
            "render_lagged_frames": str(render_lag),
            "encode_frames": str(render_frames),
            "encode_skipped_frames": "2",
            "stream_frames": str(render_frames),
            "stream_dropped_frames": "3",
            "queue_depth": "1",
            "queue_high_water": "3",
            "queue_oldest_age_microseconds": "500",
            "accepted_work": str(elapsed_ms + 10),
            "rejected_work": "1",
            "coalesced_work": "2",
            "last_queue_wait_microseconds": "100",
            "max_queue_wait_microseconds": "900",
            "last_policy_microseconds": "40",
            "max_policy_microseconds": "200",
            "detector_work_count": "4",
            "last_detector_microseconds": "7000",
            "max_detector_microseconds": "9000",
            "last_render_thread_tracker_work_microseconds": "250",
            "max_render_thread_tracker_work_microseconds": "700",
        }
    )
    if elapsed_ms == 0:
        row.update(
            {
                "run_metadata_present": "1",
                "operating_system": "Windows 11 (10.0)",
                "cpu_architecture": "x86_64",
                "obs_version": "33.0.1",
                "plugin_version": plugin_version,
                "hardware_and_workload_notes": "Church PC A; 4K camera to 1080p; two speakers",
            }
        )
    return row


class PerformanceLogAnalysisTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)

    def tearDown(self):
        self.temp_dir.cleanup()

    def write_log(self, name, rows):
        path = self.root / name
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=analyzer.EXPECTED_COLUMNS)
            writer.writeheader()
            writer.writerows(rows)
        return path

    def test_loads_schema_v2_without_render_thread_metric(self):
        row = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        row["schema_version"] = "2"
        old_columns = analyzer.EXPECTED_COLUMNS[:-2]
        path = self.root / "schema-v2.csv"
        with path.open("w", encoding="utf-8", newline="") as stream:
            writer = csv.DictWriter(stream, fieldnames=old_columns)
            writer.writeheader()
            writer.writerow({column: row[column] for column in old_columns})

        report = analyzer.load_log(path)

        self.assertEqual(report["schema_version"], 2)
        self.assertEqual(report["metrics"]["last_render_thread_tracker_work_microseconds"]["count"], 0)

    def test_summarizes_metrics_and_counter_deltas(self):
        path = self.write_log(
            "run.csv",
            [
                fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0"),
                fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "1.0"),
            ],
        )

        report = analyzer.load_log(path)

        self.assertEqual(report["sample_count"], 2)
        self.assertEqual(report["duration_hours"], 1.0)
        self.assertTrue(report["obs_video_configuration_complete"])
        self.assertTrue(report["obs_video_configuration_stable"])
        self.assertEqual(report["metrics"]["process_cpu_percent"]["p95"], 12.0)
        self.assertEqual(report["metrics"]["last_render_thread_tracker_work_microseconds"]["p95"], 250.0)
        self.assertEqual(report["counter_deltas"]["render_frames"], 1000)
        self.assertAlmostEqual(report["frame_drop_rates_percent"]["render_lag_percent"], 1.0)
        self.assertAlmostEqual(report["memory_growth_bytes_per_hour"]["resident"], 100_000)

    def test_compares_only_matched_run_context_without_thresholds(self):
        reference_path = self.write_log(
            "reference.csv",
            [
                fixture_row(0, 10.0, 1_000_000, 1000, 5, "upstream"),
                fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "upstream"),
            ],
        )
        candidate_path = self.write_log(
            "candidate.csv",
            [
                fixture_row(0, 12.0, 1_050_000, 1000, 5, "faithfulframe"),
                fixture_row(3_600_000, 14.0, 1_200_000, 2000, 25, "faithfulframe"),
            ],
        )

        comparison = analyzer.compare_logs(
            analyzer.load_log(reference_path), analyzer.load_log(candidate_path)
        )

        self.assertTrue(comparison["matched_context"])
        self.assertFalse(comparison["thresholds_applied"])
        self.assertAlmostEqual(
            comparison["measurements"]["process_cpu_percent"]["change_percent"],
            16.666666666666664,
        )

    def test_rejects_mismatched_workload_notes(self):
        first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "upstream")
        second = fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "upstream")
        reference_path = self.write_log("reference.csv", [first, second])
        candidate_first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "faithfulframe")
        candidate_first["hardware_and_workload_notes"] = "different workload"
        candidate_second = fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "faithfulframe")
        candidate_path = self.write_log("candidate.csv", [candidate_first, candidate_second])

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "hardware_and_workload_notes"):
            analyzer.compare_logs(analyzer.load_log(reference_path), analyzer.load_log(candidate_path))

    def test_rejects_mismatched_obs_version(self):
        reference_path = self.write_log(
            "reference.csv",
            [
                fixture_row(0, 10.0, 1_000_000, 1000, 5, "upstream"),
                fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "upstream"),
            ],
        )
        candidate_first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "faithfulframe")
        candidate_first["obs_version"] = "30.2.0"
        candidate_second = fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "faithfulframe")
        candidate_second["run_metadata_present"] = "0"
        for column in analyzer.METADATA_COLUMNS:
            candidate_second[column] = ""
        candidate_path = self.write_log("candidate.csv", [candidate_first, candidate_second])

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "obs_version"):
            analyzer.compare_logs(analyzer.load_log(reference_path), analyzer.load_log(candidate_path))

    def test_rejects_mismatched_or_unstable_obs_video_configuration(self):
        reference_path = self.write_log(
            "reference.csv",
            [
                fixture_row(0, 10.0, 1_000_000, 1000, 5, "upstream"),
                fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "upstream"),
            ],
        )
        candidate_first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "faithfulframe")
        candidate_first["obs_output_format"] = "3"
        candidate_second = fixture_row(3_600_000, 12.0, 1_100_000, 2000, 15, "faithfulframe")
        candidate_second["obs_output_format"] = "3"
        candidate_second["run_metadata_present"] = "0"
        for column in analyzer.METADATA_COLUMNS:
            candidate_second[column] = ""
        candidate_path = self.write_log("candidate.csv", [candidate_first, candidate_second])

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "obs_video_configuration"):
            analyzer.compare_logs(analyzer.load_log(reference_path), analyzer.load_log(candidate_path))

        candidate_second["obs_output_format"] = "2"
        candidate_path = self.write_log("changing-settings.csv", [candidate_first, candidate_second])
        with self.assertRaisesRegex(analyzer.PerformanceLogError, "stable for every sample"):
            analyzer.compare_logs(analyzer.load_log(reference_path), analyzer.load_log(candidate_path))

        candidate_second["obs_video_configuration_available"] = "0"
        for column in ("obs_graphics_module", *analyzer.OBS_VIDEO_INTEGER_COLUMNS):
            candidate_second[column] = ""
        candidate_path = self.write_log("incomplete.csv", [candidate_first, candidate_second])
        with self.assertRaisesRegex(analyzer.PerformanceLogError, "stable for every sample"):
            analyzer.compare_logs(analyzer.load_log(reference_path), analyzer.load_log(candidate_path))

    def test_rejects_non_monotonic_elapsed_time(self):
        first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        second = fixture_row(0, 12.0, 1_100_000, 2000, 15, "1.0")
        second["run_metadata_present"] = "0"
        for column in analyzer.METADATA_COLUMNS:
            second[column] = ""
        path = self.write_log(
            "bad.csv",
            [
                first,
                second,
            ],
        )

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "monotonically"):
            analyzer.load_log(path)

    def test_rejects_metadata_repeated_after_first_sample(self):
        first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        second = fixture_row(1000, 12.0, 1_100_000, 2000, 15, "1.0")
        second["operating_system"] = first["operating_system"]
        path = self.write_log("bad.csv", [first, second])

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "repeated"):
            analyzer.load_log(path)

    def test_rejects_source_or_session_identity_changes(self):
        first = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        second = fixture_row(1000, 12.0, 1_100_000, 2000, 15, "1.0")
        second["run_metadata_present"] = "0"
        for column in analyzer.METADATA_COLUMNS:
            second[column] = ""
        second["session_id"] = "21"
        path = self.write_log("identity-change.csv", [first, second])

        with self.assertRaisesRegex(analyzer.PerformanceLogError, "identity changed"):
            analyzer.load_log(path)

    def test_rejects_invalid_dimensions_and_negative_metrics(self):
        invalid_dimension = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        invalid_dimension["source_width"] = "0"
        path = self.write_log("invalid-dimension.csv", [invalid_dimension])
        with self.assertRaisesRegex(analyzer.PerformanceLogError, "source_width"):
            analyzer.load_log(path)

        negative_metric = fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0")
        negative_metric["process_cpu_percent"] = "-1"
        path = self.write_log("negative-metric.csv", [negative_metric])
        with self.assertRaisesRegex(analyzer.PerformanceLogError, "negative process_cpu_percent"):
            analyzer.load_log(path)

    def test_counter_reset_does_not_produce_a_misleading_delta(self):
        path = self.write_log(
            "reset.csv",
            [
                fixture_row(0, 10.0, 1_000_000, 1000, 5, "1.0"),
                fixture_row(1000, 12.0, 1_100_000, 2000, 15, "1.0"),
                fixture_row(2000, 12.0, 1_100_000, 10, 0, "1.0"),
            ],
        )

        report = analyzer.load_log(path)

        self.assertIsNone(report["counter_deltas"]["render_frames"])
        self.assertIsNone(report["frame_drop_rates_percent"]["render_lag_percent"])


if __name__ == "__main__":
    unittest.main()
