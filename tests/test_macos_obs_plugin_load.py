"""Tests for the macOS OBS startup-log module-load verifier."""

from __future__ import annotations

import importlib.util
import json
import platform
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "macos" / "verify-obs-plugin-load.py"
SPEC = importlib.util.spec_from_file_location("verify_obs_plugin_load", SCRIPT)
verifier = importlib.util.module_from_spec(SPEC)
assert SPEC.loader is not None
SPEC.loader.exec_module(verifier)


def startup_log(obs_major: int = 33, api_version: str = "33.0.0") -> str:
    return "\n".join(
        (
            "CPU Name: Intel(R) Core(TM) Processor",
            "Rosetta translation used: false",
            f"OBS {obs_major}.0.2 (mac)",
            f"[face-tracker] plugin loaded (plugin version 0.9.1, API version {api_version})",
        )
    )


class MacOSPluginLoadVerifierTests(unittest.TestCase):
    def test_accepts_plugin_loaded_by_expected_obs_api(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "startup.txt"
            log.write_text(startup_log(), encoding="utf-8")

            selected, error = verifier._inspect_logs([log], "face-tracker", 33, "33.0.0")

        self.assertEqual(selected, log)
        self.assertIsNone(error)

    def test_rejects_wrong_obs_major_or_plugin_api(self):
        for contents in (startup_log(obs_major=32), startup_log(api_version="32.0.0")):
            with self.subTest(contents=contents.splitlines()[-1]):
                with tempfile.TemporaryDirectory() as temporary:
                    log = Path(temporary) / "startup.txt"
                    log.write_text(contents, encoding="utf-8")
                    selected, error = verifier._inspect_logs([log], "face-tracker", 33, "33.0.0")

                self.assertIsNone(selected)
                self.assertIn("No OBS 33 log confirmed", error)

    def test_rejects_rosetta_translated_obs(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "startup.txt"
            log.write_text(startup_log() + "\nRosetta translation used: true", encoding="utf-8")

            selected, error = verifier._inspect_logs([log], "face-tracker", 33, "33.0.0")

        self.assertIsNone(selected)
        self.assertIn("used Rosetta translation", error)

    def test_report_rejects_a_smoke_runner_on_the_wrong_architecture(self):
        with tempfile.TemporaryDirectory() as temporary:
            log = Path(temporary) / "startup.txt"
            log.write_text(startup_log(), encoding="utf-8")
            with patch.object(verifier.platform, "machine", return_value="arm64"):
                report = verifier._report(
                    [log],
                    log,
                    None,
                    "face-tracker",
                    33,
                    "33.0.0",
                    "success",
                    "x86_64",
                )

        self.assertEqual(report["status"], "plugin_load_unverified")
        self.assertIsNone(report["evidence_log"])
        self.assertIn("expected 'x86_64'", report["error"])

    def test_cli_records_setup_outcome_and_copies_evidence(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            config = root / "config"
            logs = config / "logs"
            logs.mkdir(parents=True)
            evidence = logs / "obs.txt"
            evidence.write_text(startup_log(), encoding="utf-8")
            copied_logs = root / "saved-logs"
            report = root / "result.json"

            exit_code = verifier.main(
                [
                    "--config-root",
                    str(config),
                    "--artifact-dir",
                    str(copied_logs),
                    "--report",
                    str(report),
                    "--expected-architecture",
                    platform.machine(),
                    "--setup-outcome",
                    "failure",
                ]
            )
            result = json.loads(report.read_text(encoding="utf-8"))
            copied_log_exists = (copied_logs / "obs.txt").is_file()

        self.assertEqual(exit_code, 0)
        self.assertEqual(result["status"], "plugin_load_verified")
        self.assertEqual(result["onsdriver_setup_outcome"], "failure")
        self.assertEqual(result["evidence_log"], "obs.txt")
        self.assertTrue(copied_log_exists)


if __name__ == "__main__":
    unittest.main()
