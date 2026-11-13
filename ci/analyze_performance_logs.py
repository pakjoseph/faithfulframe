#!/usr/bin/env python3
"""Summarize and compare FaithfulFrame performance CSV recordings.

This tool reports measurements only. It deliberately applies no universal
performance thresholds; calibrate limits from matched baseline evidence.
"""

from __future__ import annotations

import argparse
import csv
from datetime import datetime
import json
import math
import statistics
import sys
from pathlib import Path


SCHEMA_VERSION = 4
EXPECTED_COLUMNS = [
    "schema_version",
    "run_metadata_present",
    "utc_timestamp",
    "elapsed_milliseconds",
    "source_id",
    "session_id",
    "source_generation",
    "operating_system",
    "cpu_architecture",
    "obs_version",
    "plugin_version",
    "hardware_and_workload_notes",
    "session_generation",
    "stream_generation",
    "geometry_generation",
    "tracker_generation",
    "status_version",
    "state",
    "reason",
    "source_width",
    "source_height",
    "output_width",
    "output_height",
    "obs_video_configuration_available",
    "obs_graphics_module",
    "obs_fps_num",
    "obs_fps_den",
    "obs_base_width",
    "obs_base_height",
    "obs_canvas_output_width",
    "obs_canvas_output_height",
    "obs_output_format",
    "obs_colorspace",
    "obs_range",
    "obs_scale_type",
    "obs_gpu_conversion",
    "status_snapshot_age_milliseconds",
    "process_cpu_percent",
    "resident_bytes",
    "virtual_bytes",
    "active_fps",
    "render_frames",
    "render_lagged_frames",
    "encode_frames",
    "encode_skipped_frames",
    "stream_frames",
    "stream_dropped_frames",
    "queue_depth",
    "queue_high_water",
    "queue_oldest_age_microseconds",
    "accepted_work",
    "rejected_work",
    "coalesced_work",
    "last_queue_wait_microseconds",
    "max_queue_wait_microseconds",
    "last_policy_microseconds",
    "max_policy_microseconds",
    "detector_work_count",
    "last_detector_microseconds",
    "max_detector_microseconds",
    "last_render_thread_tracker_work_microseconds",
    "max_render_thread_tracker_work_microseconds",
    "stagesurface_map_count",
    "stagesurface_map_failure_count",
    "last_stagesurface_map_microseconds",
    "max_stagesurface_map_microseconds",
]
SUPPORTED_SCHEMAS = {
    2: EXPECTED_COLUMNS[:-6],
    3: EXPECTED_COLUMNS[:-4],
    SCHEMA_VERSION: EXPECTED_COLUMNS,
}

METRIC_COLUMNS = {
    "process_cpu_percent": "percent",
    "resident_bytes": "bytes",
    "virtual_bytes": "bytes",
    "active_fps": "frames_per_second",
    "status_snapshot_age_milliseconds": "milliseconds",
    "queue_depth": "items",
    "queue_high_water": "items",
    "queue_oldest_age_microseconds": "microseconds",
    "last_queue_wait_microseconds": "microseconds",
    "max_queue_wait_microseconds": "microseconds",
    "last_policy_microseconds": "microseconds",
    "max_policy_microseconds": "microseconds",
    "detector_work_count": "runs",
    "last_detector_microseconds": "microseconds",
    "max_detector_microseconds": "microseconds",
    "last_render_thread_tracker_work_microseconds": "microseconds",
    "max_render_thread_tracker_work_microseconds": "microseconds",
    "last_stagesurface_map_microseconds": "microseconds",
    "max_stagesurface_map_microseconds": "microseconds",
}

COUNTER_COLUMNS = (
    "render_frames",
    "render_lagged_frames",
    "encode_frames",
    "encode_skipped_frames",
    "stream_frames",
    "stream_dropped_frames",
    "accepted_work",
    "rejected_work",
    "coalesced_work",
    "detector_work_count",
    "stagesurface_map_count",
    "stagesurface_map_failure_count",
)

STATE_NAMES = {
    0: "frozen",
    1: "manual",
    2: "searching",
    3: "locked",
    4: "lost",
}

REASON_NAMES = {
    0: "startup",
    1: "operator_freeze",
    2: "manual_override",
    3: "awaiting_selection",
    4: "selected_by_operator",
    5: "continuity_recovered",
    6: "target_evidence_stale",
    7: "target_loss_deadline_expired",
    8: "reset_wide",
    9: "lifecycle_invalidated",
    10: "stale_command",
    11: "invalid_command",
    12: "non_monotonic_event",
    13: "invalid_candidate",
    14: "tracker_identity_expired",
    15: "session_closing",
    16: "session_closed",
    17: "stale_lifecycle",
    18: "invalid_lifecycle",
    19: "framing_failure",
}

METADATA_COLUMNS = (
    "operating_system",
    "cpu_architecture",
    "obs_version",
    "plugin_version",
    "hardware_and_workload_notes",
)

OBS_VIDEO_INTEGER_COLUMNS = (
    "obs_fps_num",
    "obs_fps_den",
    "obs_base_width",
    "obs_base_height",
    "obs_canvas_output_width",
    "obs_canvas_output_height",
    "obs_output_format",
    "obs_colorspace",
    "obs_range",
    "obs_scale_type",
    "obs_gpu_conversion",
)


class PerformanceLogError(ValueError):
    """Raised when a CSV does not satisfy the versioned recording contract."""


def _number(value: str | None, field: str, row_number: int, *, integer: bool = False):
    if value is None or value == "":
        return None
    try:
        parsed = int(value) if integer else float(value)
    except ValueError as error:
        raise PerformanceLogError(f"row {row_number}: invalid {field}: {value!r}") from error
    if isinstance(parsed, float) and not math.isfinite(parsed):
        raise PerformanceLogError(f"row {row_number}: non-finite {field}")
    return parsed


def _percentile(values: list[float], fraction: float) -> float | None:
    if not values:
        return None
    ordered = sorted(values)
    index = max(0, math.ceil(fraction * len(ordered)) - 1)
    return ordered[index]


def _series_summary(values: list[float]) -> dict[str, float | int | None]:
    if not values:
        return {"count": 0, "min": None, "mean": None, "p50": None, "p95": None, "p99": None, "max": None}
    return {
        "count": len(values),
        "min": min(values),
        "mean": statistics.fmean(values),
        "p50": _percentile(values, 0.50),
        "p95": _percentile(values, 0.95),
        "p99": _percentile(values, 0.99),
        "max": max(values),
    }


def _linear_growth_per_hour(samples: list[dict], column: str) -> float | None:
    points = [
        (row["elapsed_milliseconds"] / 3_600_000.0, row[column])
        for row in samples
        if row[column] is not None
    ]
    if len(points) < 2:
        return None
    x_mean = statistics.fmean(point[0] for point in points)
    y_mean = statistics.fmean(point[1] for point in points)
    denominator = sum((x - x_mean) ** 2 for x, _ in points)
    if denominator == 0:
        return None
    return sum((x - x_mean) * (y - y_mean) for x, y in points) / denominator


def _counter_delta(samples: list[dict], column: str) -> int | None:
    values = [row[column] for row in samples if row[column] is not None]
    if len(values) < 2 or any(current < previous for previous, current in zip(values, values[1:])):
        return None
    return values[-1] - values[0]


def load_log(path: str | Path) -> dict:
    path = Path(path)
    try:
        with path.open("r", encoding="utf-8-sig", newline="") as stream:
            reader = csv.DictReader(stream)
            schema_version = next(
                (version for version, columns in SUPPORTED_SCHEMAS.items() if reader.fieldnames == columns),
                None,
            )
            if schema_version is None:
                raise PerformanceLogError(
                    f"{path}: columns do not match a supported performance CSV schema"
                )
            raw_rows = list(reader)
    except OSError as error:
        raise PerformanceLogError(f"{path}: {error}") from error
    if not raw_rows:
        raise PerformanceLogError(f"{path}: recording has no samples")

    samples = []
    metadata = None
    recording_identity = None
    obs_video_configurations = []
    previous_elapsed = -1
    for row_number, raw in enumerate(raw_rows, start=2):
        if None in raw or any(value is None for value in raw.values()):
            raise PerformanceLogError(f"{path}: row {row_number} has missing columns")
        if _number(raw["schema_version"], "schema_version", row_number, integer=True) != schema_version:
            raise PerformanceLogError(f"{path}: row {row_number} has an unsupported schema version")

        metadata_flag = _number(raw["run_metadata_present"], "run_metadata_present", row_number, integer=True)
        if metadata_flag != (1 if row_number == 2 else 0):
            raise PerformanceLogError(
                f"{path}: run metadata must appear exactly once on the first sample"
            )
        if row_number == 2:
            metadata = {column: raw[column] for column in METADATA_COLUMNS}
            empty = [column for column, value in metadata.items() if not value.strip()]
            if empty:
                raise PerformanceLogError(f"{path}: first sample lacks metadata: {', '.join(empty)}")
        elif any(raw[column] for column in METADATA_COLUMNS):
            raise PerformanceLogError(f"{path}: run metadata is repeated after the first sample")

        elapsed = _number(raw["elapsed_milliseconds"], "elapsed_milliseconds", row_number, integer=True)
        if elapsed is None or elapsed < 0 or elapsed <= previous_elapsed:
            raise PerformanceLogError(f"{path}: elapsed timestamps must increase monotonically")
        previous_elapsed = elapsed

        sample = {
            "elapsed_milliseconds": elapsed,
            "utc_timestamp": raw["utc_timestamp"],
        }
        if not sample["utc_timestamp"]:
            raise PerformanceLogError(f"{path}: row {row_number} has no UTC timestamp")
        try:
            utc_time = datetime.fromisoformat(sample["utc_timestamp"].replace("Z", "+00:00"))
        except ValueError as error:
            raise PerformanceLogError(f"{path}: row {row_number} has an invalid UTC timestamp") from error
        if utc_time.utcoffset() is None or utc_time.utcoffset().total_seconds() != 0:
            raise PerformanceLogError(f"{path}: row {row_number} timestamp is not UTC")
        for column in (
            "source_id",
            "session_id",
            "source_generation",
            "session_generation",
            "stream_generation",
            "geometry_generation",
            "tracker_generation",
            "status_version",
            "state",
            "reason",
            "source_width",
            "source_height",
            "output_width",
            "output_height",
        ):
            sample[column] = _number(raw[column], column, row_number, integer=True)
        required_values = (
            "source_id",
            "session_id",
            "source_generation",
            "session_generation",
            "stream_generation",
            "geometry_generation",
            "tracker_generation",
            "status_version",
            "state",
            "reason",
            "source_width",
            "source_height",
            "output_width",
            "output_height",
        )
        if any(sample[column] is None for column in required_values):
            raise PerformanceLogError(f"{path}: row {row_number} has missing required tracking fields")
        if not sample["source_id"] or not sample["session_id"]:
            raise PerformanceLogError(f"{path}: row {row_number} has an invalid source/session identity")
        identity = (sample["source_id"], sample["session_id"])
        if recording_identity is None:
            recording_identity = identity
        elif identity != recording_identity:
            raise PerformanceLogError(f"{path}: source/session identity changed during the recording")
        for column in (
            "source_generation",
            "session_generation",
            "stream_generation",
            "geometry_generation",
            "tracker_generation",
        ):
            if sample[column] <= 0:
                raise PerformanceLogError(f"{path}: row {row_number} has an invalid {column}")
        if sample["state"] not in STATE_NAMES:
            raise PerformanceLogError(f"{path}: row {row_number} has an unknown state code")
        if sample["reason"] not in REASON_NAMES:
            raise PerformanceLogError(f"{path}: row {row_number} has an unknown reason code")
        for column in ("source_width", "source_height", "output_width", "output_height"):
            if sample[column] <= 0:
                raise PerformanceLogError(f"{path}: row {row_number} has an invalid {column}")

        video_configuration_available = _number(
            raw["obs_video_configuration_available"],
            "obs_video_configuration_available",
            row_number,
            integer=True,
        )
        if video_configuration_available not in (0, 1):
            raise PerformanceLogError(f"{path}: row {row_number} has an invalid OBS video configuration flag")
        sample["obs_video_configuration_available"] = bool(video_configuration_available)
        sample["obs_graphics_module"] = raw["obs_graphics_module"]
        for column in OBS_VIDEO_INTEGER_COLUMNS:
            sample[column] = _number(raw[column], column, row_number, integer=True)
        if video_configuration_available:
            if not sample["obs_graphics_module"].strip():
                raise PerformanceLogError(f"{path}: row {row_number} lacks an OBS graphics module")
            for column in OBS_VIDEO_INTEGER_COLUMNS:
                if sample[column] is None or sample[column] < 0:
                    raise PerformanceLogError(f"{path}: row {row_number} has an invalid {column}")
            for column in (
                "obs_fps_num",
                "obs_fps_den",
                "obs_base_width",
                "obs_base_height",
                "obs_canvas_output_width",
                "obs_canvas_output_height",
            ):
                if sample[column] == 0:
                    raise PerformanceLogError(f"{path}: row {row_number} has an invalid {column}")
            if sample["obs_gpu_conversion"] not in (0, 1):
                raise PerformanceLogError(f"{path}: row {row_number} has an invalid obs_gpu_conversion")
            obs_video_configurations.append(
                {
                    "graphics_module": sample["obs_graphics_module"],
                    **{column: sample[column] for column in OBS_VIDEO_INTEGER_COLUMNS},
                }
            )
        else:
            if sample["obs_graphics_module"] or any(
                sample[column] is not None for column in OBS_VIDEO_INTEGER_COLUMNS
            ):
                raise PerformanceLogError(
                    f"{path}: row {row_number} has OBS video settings while marked unavailable"
                )
            obs_video_configurations.append(None)
        for column in METRIC_COLUMNS:
            sample[column] = _number(raw.get(column), column, row_number)
            if sample[column] is not None and sample[column] < 0:
                raise PerformanceLogError(f"{path}: row {row_number} has a negative {column}")
        for column in COUNTER_COLUMNS:
            sample[column] = _number(raw.get(column), column, row_number, integer=True)
            if sample[column] is not None and sample[column] < 0:
                raise PerformanceLogError(f"{path}: row {row_number} has a negative {column}")
        samples.append(sample)

    if metadata is None:
        raise PerformanceLogError(f"{path}: run metadata was not found")
    known_obs_video_configurations = []
    for configuration in obs_video_configurations:
        if configuration is not None and configuration not in known_obs_video_configurations:
            known_obs_video_configurations.append(configuration)
    obs_video_configuration_stable = (
        len(obs_video_configurations) == len(samples)
        and all(configuration is not None for configuration in obs_video_configurations)
        and len(known_obs_video_configurations) == 1
    )
    elapsed_hours = (samples[-1]["elapsed_milliseconds"] - samples[0]["elapsed_milliseconds"]) / 3_600_000.0
    resolution_pairs = sorted(
        {
            (
                row["source_width"],
                row["source_height"],
                row["output_width"],
                row["output_height"],
            )
            for row in samples
        }
    )
    metric_summary = {}
    for column, unit in METRIC_COLUMNS.items():
        values = [float(row[column]) for row in samples if row[column] is not None]
        metric_summary[column] = {"unit": unit, **_series_summary(values)}

    counter_deltas = {column: _counter_delta(samples, column) for column in COUNTER_COLUMNS}
    render_delta = counter_deltas["render_frames"]
    render_lag_delta = counter_deltas["render_lagged_frames"]
    encode_delta = counter_deltas["encode_frames"]
    encode_skip_delta = counter_deltas["encode_skipped_frames"]
    stream_delta = counter_deltas["stream_frames"]
    stream_drop_delta = counter_deltas["stream_dropped_frames"]
    rates = {
        "render_lag_percent": (100.0 * render_lag_delta / render_delta)
        if render_delta and render_lag_delta is not None
        else None,
        "encode_skip_percent": (100.0 * encode_skip_delta / encode_delta)
        if encode_delta and encode_skip_delta is not None
        else None,
        "stream_drop_percent": (100.0 * stream_drop_delta / stream_delta)
        if stream_delta and stream_drop_delta is not None
        else None,
    }
    states = {}
    reasons = {}
    for row in samples:
        state_code = row["state"]
        state = f"{state_code}_{STATE_NAMES.get(state_code, 'unknown')}"
        states[state] = states.get(state, 0) + 1
        reason_code = row["reason"]
        reason = f"{reason_code}_{REASON_NAMES.get(reason_code, 'unknown')}"
        reasons[reason] = reasons.get(reason, 0) + 1

    return {
        "path": str(path),
        "schema_version": schema_version,
        "metadata": metadata,
        "obs_video_configuration_complete": all(
            configuration is not None for configuration in obs_video_configurations
        ),
        "obs_video_configuration_stable": obs_video_configuration_stable,
        "obs_video_configurations": known_obs_video_configurations,
        "sample_count": len(samples),
        "duration_hours": elapsed_hours,
        "sample_interval_milliseconds": _series_summary(
            [float(b["elapsed_milliseconds"] - a["elapsed_milliseconds"])
             for a, b in zip(samples, samples[1:])]
        ),
        "resolutions": [
            {
                "input": [source_width, source_height],
                "output": [output_width, output_height],
            }
            for source_width, source_height, output_width, output_height in resolution_pairs
        ],
        "metrics": metric_summary,
        "memory_growth_bytes_per_hour": {
            "resident": _linear_growth_per_hour(samples, "resident_bytes"),
            "virtual": _linear_growth_per_hour(samples, "virtual_bytes"),
        },
        "counter_deltas": counter_deltas,
        "frame_drop_rates_percent": rates,
        "state_sample_counts": states,
        "reason_sample_counts": reasons,
    }


def compare_logs(reference: dict, candidate: dict) -> dict:
    reference_resolution = reference["resolutions"]
    candidate_resolution = candidate["resolutions"]
    context_mismatches = []
    for field in ("operating_system", "cpu_architecture", "obs_version", "hardware_and_workload_notes"):
        if reference["metadata"][field] != candidate["metadata"][field]:
            context_mismatches.append(field)
    if not reference["obs_video_configuration_stable"] or not candidate["obs_video_configuration_stable"]:
        raise PerformanceLogError(
            "runs are not comparable; OBS video configuration must be available and stable for every sample"
        )
    if reference["obs_video_configurations"] != candidate["obs_video_configurations"]:
        context_mismatches.append("obs_video_configuration")
    if reference_resolution != candidate_resolution:
        context_mismatches.append("video_resolutions")
    if context_mismatches:
        raise PerformanceLogError(
            "runs are not comparable; mismatched context: " + ", ".join(context_mismatches)
        )

    comparisons = {}
    for metric in ("process_cpu_percent", "resident_bytes", "virtual_bytes", "active_fps"):
        before = reference["metrics"][metric]
        after = candidate["metrics"][metric]
        reference_value = before["p95"] if metric != "resident_bytes" and metric != "virtual_bytes" else before["max"]
        candidate_value = after["p95"] if metric != "resident_bytes" and metric != "virtual_bytes" else after["max"]
        difference = (
            candidate_value - reference_value
            if reference_value is not None and candidate_value is not None
            else None
        )
        change_percent = (
            difference / reference_value * 100.0
            if difference is not None and reference_value != 0
            else None
        )
        comparisons[metric] = {
            "reference_p95_or_max": reference_value,
            "candidate_p95_or_max": candidate_value,
            "difference": difference,
            "change_percent": change_percent,
        }
    for metric in ("resident", "virtual"):
        before = reference["memory_growth_bytes_per_hour"][metric]
        after = candidate["memory_growth_bytes_per_hour"][metric]
        comparisons[f"{metric}_growth_bytes_per_hour"] = {
            "reference": before,
            "candidate": after,
            "difference": after - before if before is not None and after is not None else None,
        }
    for metric in ("render_lag_percent", "encode_skip_percent", "stream_drop_percent"):
        before = reference["frame_drop_rates_percent"][metric]
        after = candidate["frame_drop_rates_percent"][metric]
        comparisons[metric] = {
            "reference": before,
            "candidate": after,
            "difference": after - before if before is not None and after is not None else None,
        }

    return {
        "reference_plugin_version": reference["metadata"]["plugin_version"],
        "candidate_plugin_version": candidate["metadata"]["plugin_version"],
        "matched_context": True,
        "reference_duration_hours": reference["duration_hours"],
        "candidate_duration_hours": candidate["duration_hours"],
        "measurements": comparisons,
        "thresholds_applied": False,
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("reference", type=Path, help="reference/baseline recording CSV")
    parser.add_argument("candidate", type=Path, nargs="?", help="candidate FaithfulFrame recording CSV")
    args = parser.parse_args(argv)
    try:
        reference = load_log(args.reference)
        result = {"reference": reference, "thresholds_applied": False}
        if args.candidate:
            candidate = load_log(args.candidate)
            result["candidate"] = candidate
            result["comparison"] = compare_logs(reference, candidate)
        print(json.dumps(result, indent=2, sort_keys=True, allow_nan=False))
    except PerformanceLogError as error:
        print(f"performance log analysis failed: {error}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
