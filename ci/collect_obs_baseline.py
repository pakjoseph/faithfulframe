#!/usr/bin/env python3
"""Record aggregate OBS metrics for an unmodified-plugin baseline run.

The collector uses only read-only OBS WebSocket requests. It does not inspect
scene/source names, change OBS state, or write the WebSocket password to disk.
Install the pinned optional dependency with:

    python3 -m pip install -r ci/requirements-performance.txt

The operator is responsible for verifying the unmodified plugin build and
entering the same run metadata used by the FaithfulFrame performance recorder.
Start OBS recording or streaming yourself before running the tool. Example:

    python3 ci/collect_obs_baseline.py upstream.csv \\
      --input-resolution 3840x2160 \\
      --plugin-version obs-face-tracker@<commit> \\
      --notes "Same PC, camera, OBS profile, and speaker workload" \\
      --duration-seconds 7200

Then compare with `python3 ci/analyze_performance_logs.py upstream.csv faithfulframe.csv`.
The camera resolution is operator-declared; stop and restart the run if it changes.
"""

from __future__ import annotations

import argparse
import base64
import csv
from datetime import datetime, timezone
import getpass
import hashlib
import ipaddress
import json
import math
import os
from pathlib import Path
import platform
import sys
import time
from typing import Callable


SCHEMA_VERSION = 1
CSV_COLUMNS = (
    "baseline_schema_version",
    "run_metadata_present",
    "utc_timestamp",
    "elapsed_milliseconds",
    "operating_system",
    "cpu_architecture",
    "obs_version",
    "plugin_version",
    "hardware_and_workload_notes",
    "input_width",
    "input_height",
    "output_width",
    "output_height",
    "stream_active",
    "record_active",
    "fps_numerator",
    "fps_denominator",
    "base_width",
    "base_height",
    "cpu_usage_percent",
    "memory_usage_bytes",
    "active_fps",
    "render_total_frames",
    "render_skipped_frames",
    "stream_total_frames",
    "stream_skipped_frames",
)


class BaselineCollectionError(ValueError):
    """Raised when OBS does not provide valid, comparable baseline data."""


def _auth_string(password: str, salt: str, challenge: str) -> str:
    secret = base64.b64encode(hashlib.sha256((password + salt).encode("utf-8")).digest())
    return base64.b64encode(hashlib.sha256(secret + challenge.encode("utf-8")).digest()).decode("ascii")


def _number(response: dict, field: str, *, integer: bool = False) -> int | float:
    if field not in response or response[field] is None:
        raise BaselineCollectionError(f"OBS response is missing {field}")
    raw_value = response[field]
    if isinstance(raw_value, bool):
        raise BaselineCollectionError(f"OBS returned an invalid {field}")
    try:
        if integer and isinstance(raw_value, int):
            value = raw_value
        elif integer:
            numeric = float(raw_value)
            if not math.isfinite(numeric) or not numeric.is_integer():
                raise ValueError("not an integer")
            value = int(numeric)
        else:
            value = float(raw_value)
    except (OverflowError, TypeError, ValueError) as error:
        raise BaselineCollectionError(f"OBS returned an invalid {field}") from error
    if isinstance(value, float) and not math.isfinite(value):
        raise BaselineCollectionError(f"OBS returned a non-finite {field}")
    if value < 0:
        raise BaselineCollectionError(f"OBS returned a negative {field}")
    return value


class ObsWebSocketClient:
    """Small synchronous read-only OBS WebSocket v5 client."""

    def __init__(
        self,
        host: str,
        port: int,
        password: str,
        timeout: float = 5.0,
        socket_factory: Callable | None = None,
    ):
        self.host = host
        self.port = port
        self.password = password
        self.timeout = timeout
        self._socket_factory = socket_factory
        self._socket = None
        self._next_request_id = 1
        self._version_info = None

    def connect(self) -> None:
        if self._socket_factory is None:
            try:
                import websocket
            except ImportError as error:
                raise BaselineCollectionError(
                    "websocket-client is required; install ci/requirements-performance.txt"
                ) from error
            self._socket_factory = websocket.create_connection

        try:
            host = f"[{self.host}]" if ":" in self.host else self.host
            self._socket = self._socket_factory(
                f"ws://{host}:{self.port}", timeout=self.timeout
            )
            hello = self._receive()
            if hello.get("op") != 0 or not isinstance(hello.get("d"), dict):
                raise BaselineCollectionError("OBS did not send a WebSocket Hello message")
            hello_data = hello["d"]
            rpc_version = hello_data.get("rpcVersion")
            if not isinstance(rpc_version, int) or rpc_version < 1:
                raise BaselineCollectionError("OBS sent an invalid WebSocket RPC version")
            identify = {"rpcVersion": rpc_version, "eventSubscriptions": 0}
            authentication = hello_data.get("authentication")
            if authentication is not None:
                if not isinstance(authentication, dict):
                    raise BaselineCollectionError("OBS sent invalid authentication metadata")
                try:
                    identify["authentication"] = _auth_string(
                        self.password,
                        authentication["salt"],
                        authentication["challenge"],
                    )
                except KeyError as error:
                    raise BaselineCollectionError("OBS sent incomplete authentication metadata") from error
            self._send({"op": 1, "d": identify})
            while True:
                message = self._receive()
                if message.get("op") == 2:
                    return
                if message.get("op") == 0:
                    raise BaselineCollectionError("OBS sent a second Hello before identifying")
        except BaselineCollectionError:
            self.close()
            raise
        except Exception as error:
            self.close()
            raise BaselineCollectionError(f"could not connect to OBS WebSocket: {error}") from error

    def _send(self, message: dict) -> None:
        if self._socket is None:
            raise BaselineCollectionError("OBS WebSocket is not connected")
        self._socket.send(json.dumps(message, separators=(",", ":")))

    def _receive(self) -> dict:
        if self._socket is None:
            raise BaselineCollectionError("OBS WebSocket is not connected")
        raw = self._socket.recv()
        if isinstance(raw, bytes):
            try:
                raw = raw.decode("utf-8")
            except UnicodeDecodeError as error:
                raise BaselineCollectionError("OBS sent invalid UTF-8 WebSocket JSON") from error
        try:
            message = json.loads(raw)
        except (TypeError, json.JSONDecodeError) as error:
            raise BaselineCollectionError("OBS sent invalid WebSocket JSON") from error
        if not isinstance(message, dict) or not isinstance(message.get("op"), int):
            raise BaselineCollectionError("OBS sent an invalid WebSocket message")
        return message

    def request(self, request_type: str) -> dict:
        request_id = f"faithfulframe-baseline-{self._next_request_id}"
        self._next_request_id += 1
        try:
            self._send(
                {
                    "op": 6,
                    "d": {
                        "requestType": request_type,
                        "requestId": request_id,
                        "requestData": {},
                    },
                }
            )
            while True:
                message = self._receive()
                if message.get("op") != 7:
                    continue
                data = message.get("d")
                if not isinstance(data, dict) or data.get("requestId") != request_id:
                    continue
                status = data.get("requestStatus")
                if not isinstance(status, dict) or not status.get("result"):
                    comment = status.get("comment", "request failed") if isinstance(status, dict) else "request failed"
                    raise BaselineCollectionError(f"OBS {request_type} failed: {comment}")
                response = data.get("responseData", {})
                if not isinstance(response, dict):
                    raise BaselineCollectionError(f"OBS {request_type} returned invalid response data")
                return response
        except BaselineCollectionError:
            raise
        except Exception as error:
            raise BaselineCollectionError(f"OBS {request_type} request failed: {error}") from error

    def close(self) -> None:
        if self._socket is not None:
            try:
                self._socket.close()
            except Exception:
                pass
            finally:
                self._socket = None


def parse_resolution(value: str) -> tuple[int, int]:
    try:
        width_text, height_text = value.lower().split("x", maxsplit=1)
        width, height = int(width_text), int(height_text)
    except (ValueError, TypeError) as error:
        raise argparse.ArgumentTypeError("resolution must use WIDTHxHEIGHT, for example 3840x2160") from error
    if width <= 0 or height <= 0:
        raise argparse.ArgumentTypeError("resolution dimensions must be positive")
    return width, height


def _safe_csv_text(value: str) -> str:
    first = len(value) - len(value.lstrip())
    if first < len(value) and value[first] in "=+-@":
        return "'" + value
    return value


def collect_snapshot(client: ObsWebSocketClient, input_resolution: tuple[int, int]) -> dict:
    if client._version_info is None:
        client._version_info = client.request("GetVersion")
    version = client._version_info
    settings = client.request("GetVideoSettings")
    stats = client.request("GetStats")
    stream = client.request("GetStreamStatus")
    record = client.request("GetRecordStatus")

    video = {
        "fps_numerator": _number(settings, "fpsNumerator", integer=True),
        "fps_denominator": _number(settings, "fpsDenominator", integer=True),
        "base_width": _number(settings, "baseWidth", integer=True),
        "base_height": _number(settings, "baseHeight", integer=True),
        "output_width": _number(settings, "outputWidth", integer=True),
        "output_height": _number(settings, "outputHeight", integer=True),
    }
    if any(video[field] <= 0 for field in video):
        raise BaselineCollectionError("OBS returned zero-valued video settings")
    if not version.get("obsVersion"):
        raise BaselineCollectionError("OBS response is missing obsVersion")
    if not (stream.get("outputActive") is True or record.get("outputActive") is True):
        raise BaselineCollectionError("start OBS recording or streaming before collecting a rehearsal baseline")

    input_width, input_height = input_resolution
    return {
        **video,
        "input_width": input_width,
        "input_height": input_height,
        "operating_system": version.get("platformDescription", "") or platform.platform(),
        "cpu_architecture": platform.machine().lower().replace("amd64", "x86_64"),
        "obs_version": str(version["obsVersion"]),
        "cpu_usage_percent": _number(stats, "cpuUsage"),
        # OBS documents memoryUsage in MB. Convert to bytes for the shared analyzer.
        "memory_usage_bytes": round(_number(stats, "memoryUsage") * 1024 * 1024),
        "active_fps": _number(stats, "activeFps"),
        "render_total_frames": _number(stats, "renderTotalFrames", integer=True),
        "render_skipped_frames": _number(stats, "renderSkippedFrames", integer=True),
        "stream_total_frames": (
            _number(stream, "outputTotalFrames", integer=True) if stream.get("outputActive") else None
        ),
        "stream_skipped_frames": (
            _number(stream, "outputSkippedFrames", integer=True) if stream.get("outputActive") else None
        ),
        "stream_active": stream.get("outputActive") is True,
        "record_active": record.get("outputActive") is True and record.get("outputPaused") is not True,
    }


def make_csv_row(
    snapshot: dict,
    *,
    elapsed_milliseconds: int,
    run_metadata_present: bool,
    metadata: dict,
    now: datetime | None = None,
) -> dict:
    timestamp = now or datetime.now(timezone.utc)
    if timestamp.utcoffset() is None or timestamp.utcoffset().total_seconds() != 0:
        raise ValueError("timestamp must be UTC")
    row = {
        "baseline_schema_version": SCHEMA_VERSION,
        "run_metadata_present": 1 if run_metadata_present else 0,
        "utc_timestamp": timestamp.isoformat(timespec="milliseconds").replace("+00:00", "Z"),
        "elapsed_milliseconds": elapsed_milliseconds,
        **{key: "" for key in (
            "operating_system", "cpu_architecture", "obs_version", "plugin_version",
            "hardware_and_workload_notes",
        )},
        **snapshot,
    }
    if run_metadata_present:
        for key, value in metadata.items():
            row[key] = _safe_csv_text(str(value))
    return row


def _run_collection(args: argparse.Namespace, client: ObsWebSocketClient, output_path: Path) -> None:
    width, height = args.input_resolution
    metadata = {
        "operating_system": args.operating_system or "",
        "cpu_architecture": args.cpu_architecture or "",
        "obs_version": "",
        "plugin_version": args.plugin_version,
        "hardware_and_workload_notes": args.notes,
    }
    start_ns = time.monotonic_ns()
    deadline_ns = start_ns + round(args.duration_seconds * 1_000_000_000)
    next_sample_ns = start_ns
    previous_elapsed = -1
    sample_count = 0
    active_output_mode = None

    with output_path.open("w", encoding="utf-8", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=CSV_COLUMNS, extrasaction="raise")
        writer.writeheader()
        while True:
            snapshot = collect_snapshot(client, (width, height))
            output_mode = (snapshot["stream_active"], snapshot["record_active"])
            if active_output_mode is None:
                active_output_mode = output_mode
            elif output_mode != active_output_mode:
                raise BaselineCollectionError("recording or streaming output state changed during collection")
            if sample_count == 0:
                metadata["operating_system"] = metadata["operating_system"] or snapshot["operating_system"]
                metadata["cpu_architecture"] = metadata["cpu_architecture"] or snapshot["cpu_architecture"]
                metadata["obs_version"] = snapshot["obs_version"]
            elif snapshot["obs_version"] != metadata["obs_version"]:
                raise BaselineCollectionError("OBS version changed during baseline recording")
            elapsed = 0 if sample_count == 0 else max(
                previous_elapsed + 1, round((time.monotonic_ns() - start_ns) / 1_000_000)
            )
            previous_elapsed = elapsed
            writer.writerow(
                make_csv_row(
                    snapshot,
                    elapsed_milliseconds=elapsed,
                    run_metadata_present=(sample_count == 0),
                    metadata=metadata,
                )
            )
            stream.flush()
            sample_count += 1
            now_ns = time.monotonic_ns()
            if now_ns >= deadline_ns:
                break
            next_sample_ns += round(args.interval_seconds * 1_000_000_000)
            time.sleep(max(0.0, (next_sample_ns - time.monotonic_ns()) / 1_000_000_000))


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("output", type=Path, help="destination baseline CSV")
    parser.add_argument("--input-resolution", required=True, type=parse_resolution,
                        help="operator-declared camera resolution, e.g. 3840x2160; restart if it changes")
    parser.add_argument("--plugin-version", required=True,
                        help="upstream release/commit label; verify the loaded plugin yourself")
    parser.add_argument("--notes", required=True,
                        help="hardware and workload notes; reuse the exact text for candidate recording")
    parser.add_argument("--operating-system", help="exact OS label matching the candidate CSV metadata")
    parser.add_argument("--cpu-architecture", help="exact architecture label matching candidate metadata")
    parser.add_argument("--host", default="127.0.0.1", help="loopback OBS WebSocket host (default: 127.0.0.1)")
    parser.add_argument("--port", default=4455, type=int, help="OBS WebSocket port (default: 4455)")
    parser.add_argument("--duration-seconds", default=7200, type=float,
                        help="recording duration (default: 7200 seconds)")
    parser.add_argument("--interval-seconds", default=1.0, type=float,
                        help="sample interval; must be at least 1 second (default: 1)")
    parser.add_argument("--timeout-seconds", default=5.0, type=float,
                        help="WebSocket request timeout (default: 5)")
    return parser


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    if args.port < 1 or args.port > 65535:
        parser.error("--port must be between 1 and 65535")
    try:
        if args.host.lower() != "localhost" and not ipaddress.ip_address(args.host).is_loopback:
            parser.error("--host must be localhost or a loopback IP; this collector uses unencrypted local WebSocket")
    except ValueError:
        parser.error("--host must be localhost or a loopback IP; this collector uses unencrypted local WebSocket")
    if args.duration_seconds <= 0 or not math.isfinite(args.duration_seconds):
        parser.error("--duration-seconds must be a finite positive number")
    if args.interval_seconds < 1.0 or not math.isfinite(args.interval_seconds):
        parser.error("--interval-seconds must be finite and at least 1 second")
    if args.timeout_seconds <= 0 or not math.isfinite(args.timeout_seconds):
        parser.error("--timeout-seconds must be finite and positive")
    if not args.plugin_version.strip() or not args.notes.strip():
        parser.error("--plugin-version and --notes must not be empty")
    if args.output.exists():
        parser.error(f"refusing to overwrite existing output: {args.output}")
    partial_path = args.output.with_name(args.output.name + ".partial")
    if partial_path.exists():
        parser.error(f"refusing to reuse existing partial output: {partial_path}")

    password = os.environ.get("OBS_WEBSOCKET_PASSWORD")
    if password is None:
        password = getpass.getpass("OBS WebSocket password (leave empty if authentication is disabled): ")
    client = ObsWebSocketClient(args.host, args.port, password, timeout=args.timeout_seconds)
    partial_created = False
    try:
        client.connect()
        with partial_path.open("x", encoding="utf-8"):
            partial_created = True
        _run_collection(args, client, partial_path)
        os.replace(partial_path, args.output)
    except (BaselineCollectionError, OSError, csv.Error) as error:
        print(f"OBS baseline collection failed: {error}", file=sys.stderr)
        return 2
    except KeyboardInterrupt:
        print("OBS baseline collection cancelled; incomplete data discarded", file=sys.stderr)
        return 130
    finally:
        try:
            client.close()
        finally:
            if partial_created:
                try:
                    partial_path.unlink(missing_ok=True)
                except OSError:
                    pass
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
