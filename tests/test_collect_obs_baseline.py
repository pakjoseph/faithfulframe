import base64
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "collect_obs_baseline.py"
SPEC = importlib.util.spec_from_file_location("collect_obs_baseline", SCRIPT)
assert SPEC and SPEC.loader
collector = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(collector)
ANALYZER_SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "analyze_performance_logs.py"
ANALYZER_SPEC = importlib.util.spec_from_file_location("analyze_performance_logs_for_baseline", ANALYZER_SCRIPT)
assert ANALYZER_SPEC and ANALYZER_SPEC.loader
analyzer = importlib.util.module_from_spec(ANALYZER_SPEC)
ANALYZER_SPEC.loader.exec_module(analyzer)


def response_for(request_type, *, stream_active=True, record_active=False, record_paused=False):
    return {
        "GetVersion": {
            "obsVersion": "33.0.1", "platformDescription": "Windows 11",
            "availableRequests": ["GetStats", "GetVideoSettings", "GetStreamStatus", "GetRecordStatus"],
        },
        "GetVideoSettings": {
            "fpsNumerator": 30, "fpsDenominator": 1,
            "baseWidth": 1920, "baseHeight": 1080,
            "outputWidth": 1280, "outputHeight": 720,
        },
        "GetStats": {
            "cpuUsage": 18.5, "memoryUsage": 2048.0, "activeFps": 59.9,
            "renderTotalFrames": 10000, "renderSkippedFrames": 10,
        },
        "GetStreamStatus": {
            "outputActive": stream_active, "outputTotalFrames": 9990, "outputSkippedFrames": 5,
        },
        "GetRecordStatus": {"outputActive": record_active, "outputPaused": record_paused},
    }[request_type]


class FakeSocket:
    def __init__(self):
        salt = "test-salt"
        challenge = "test-challenge"
        self.incoming = [
            {
                "op": 0,
                "d": {
                    "rpcVersion": 1,
                    "authentication": {"salt": salt, "challenge": challenge},
                },
            }
        ]
        self.sent = []
        self.closed = False
        self.stream_active = True
        self.record_active = False
        self.record_paused = False

    def send(self, raw):
        message = json.loads(raw)
        self.sent.append(message)
        if message["op"] == 1:
            self.incoming.append({"op": 2, "d": {"negotiatedRpcVersion": 1}})
        elif message["op"] == 6:
            data = message["d"]
            self.incoming.append(
                {
                    "op": 7,
                    "d": {
                        "requestType": data["requestType"],
                        "requestId": data["requestId"],
                        "requestStatus": {"result": True, "code": 100},
                        "responseData": response_for(
                            data["requestType"],
                            stream_active=self.stream_active,
                            record_active=self.record_active,
                            record_paused=self.record_paused,
                        ),
                    },
                }
            )

    def recv(self):
        return json.dumps(self.incoming.pop(0))

    def close(self):
        self.closed = True


class BaselineCollectorTests(unittest.TestCase):
    def setUp(self):
        self.socket = FakeSocket()

    def make_client(self):
        return collector.ObsWebSocketClient(
            "localhost", 4455, "secret", socket_factory=lambda *args, **kwargs: self.socket
        )

    def test_authentication_uses_obs_challenge_contract(self):
        client = self.make_client()
        client.connect()
        identify = self.socket.sent[0]["d"]
        secret = base64.b64encode(hashlib.sha256(b"secret" + b"test-salt").digest())
        expected = base64.b64encode(hashlib.sha256(secret + b"test-challenge").digest()).decode()
        self.assertEqual(identify["authentication"], expected)
        self.assertEqual(identify["eventSubscriptions"], 0)
        client.close()
        self.assertTrue(self.socket.closed)

    def test_csv_contract_matches_shared_analyzer(self):
        self.assertEqual(list(collector.CSV_COLUMNS), analyzer.BASELINE_COLUMNS)

    def test_collects_only_read_only_metrics_and_validates_active_output(self):
        client = self.make_client()
        client.connect()
        snapshot = collector.collect_snapshot(client, (3840, 2160))
        request_types = [message["d"]["requestType"] for message in self.socket.sent if message["op"] == 6]
        self.assertEqual(
            request_types,
            ["GetVersion", "GetVideoSettings", "GetStats", "GetStreamStatus", "GetRecordStatus"],
        )
        self.assertEqual(snapshot["input_width"], 3840)
        self.assertEqual(snapshot["memory_usage_bytes"], 2 * 1024 * 1024 * 1024)
        self.assertEqual(snapshot["render_total_frames"], 10000)
        self.assertEqual(snapshot["stream_skipped_frames"], 5)
        self.assertTrue(snapshot["stream_active"])
        self.assertFalse(snapshot["record_active"])
        collector.collect_snapshot(client, (3840, 2160))
        repeated_types = [message["d"]["requestType"] for message in self.socket.sent if message["op"] == 6]
        self.assertEqual(repeated_types.count("GetVersion"), 1)

    def test_rejects_inactive_outputs(self):
        client = self.make_client()
        self.socket.stream_active = False
        self.socket.record_active = False
        client.connect()
        with self.assertRaisesRegex(collector.BaselineCollectionError, "start OBS recording or streaming"):
            collector.collect_snapshot(client, (3840, 2160))

    def test_rejects_paused_recording_without_streaming(self):
        client = self.make_client()
        self.socket.stream_active = False
        self.socket.record_active = True
        self.socket.record_paused = True
        client.connect()
        with self.assertRaisesRegex(collector.BaselineCollectionError, "start OBS recording or streaming"):
            collector.collect_snapshot(client, (3840, 2160))

    def test_rejects_invalid_output_status_types(self):
        client = self.make_client()
        self.socket.stream_active = 1
        client.connect()
        with self.assertRaisesRegex(collector.BaselineCollectionError, "invalid outputActive"):
            collector.collect_snapshot(client, (3840, 2160))

        self.socket = FakeSocket()
        client = self.make_client()
        self.socket.stream_active = False
        self.socket.record_active = True
        self.socket.record_paused = "false"
        client.connect()
        with self.assertRaisesRegex(collector.BaselineCollectionError, "invalid outputPaused"):
            collector.collect_snapshot(client, (3840, 2160))

    def test_accepts_recording_without_streaming(self):
        client = self.make_client()
        self.socket.stream_active = False
        self.socket.record_active = True
        client.connect()
        snapshot = collector.collect_snapshot(client, (3840, 2160))
        self.assertFalse(snapshot["stream_active"])
        self.assertTrue(snapshot["record_active"])
        self.assertIsNone(snapshot["stream_total_frames"])

    def test_resolution_parser_rejects_invalid_dimensions(self):
        self.assertEqual(collector.parse_resolution("3840x2160"), (3840, 2160))
        with self.assertRaises(Exception):
            collector.parse_resolution("0x1080")
        with self.assertRaises(Exception):
            collector.parse_resolution("1920p")

    def test_rejects_nonfinite_and_fractional_integer_metrics(self):
        with self.assertRaisesRegex(collector.BaselineCollectionError, "non-finite"):
            collector._number({"metric": float("nan")}, "metric")
        with self.assertRaisesRegex(collector.BaselineCollectionError, "invalid"):
            collector._number({"metric": 4.5}, "metric", integer=True)

    def test_csv_first_row_contains_metadata_and_utc_time(self):
        snapshot = {
            "operating_system": "Windows 11", "cpu_architecture": "x86_64", "obs_version": "33.0.1",
            "input_width": 3840, "input_height": 2160, "output_width": 1280, "output_height": 720,
        }
        metadata = {
            "operating_system": "Windows 11", "cpu_architecture": "x86_64", "obs_version": "33.0.1",
            "plugin_version": "upstream@abc123", "hardware_and_workload_notes": "4K sanctuary camera to 720p",
        }
        row = collector.make_csv_row(
            snapshot,
            elapsed_milliseconds=0,
            run_metadata_present=True,
            metadata=metadata,
            now=datetime(2026, 11, 13, tzinfo=timezone.utc),
        )
        self.assertEqual(row["utc_timestamp"], "2026-11-13T00:00:00.000Z")
        self.assertEqual(row["plugin_version"], "upstream@abc123")
        self.assertNotIn("password", row)


if __name__ == "__main__":
    unittest.main()
