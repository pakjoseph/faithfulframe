#!/usr/bin/env python3
"""Verify an OBS plugin load from the startup logs on macOS."""

from __future__ import annotations

import argparse
import json
import platform
from pathlib import Path
import re
import shutil
import sys
from typing import Any


def _log_files(log_directories: tuple[Path, ...]) -> list[Path]:
    files: set[Path] = set()
    for directory in log_directories:
        if directory.is_dir():
            files.update(path for path in directory.glob("*.txt") if path.is_file())
    return sorted(files, key=lambda path: (path.stat().st_mtime_ns, path.name))


def _inspect_logs(
    files: list[Path], plugin_name: str, obs_major: int, api_version: str
) -> tuple[Path | None, str | None]:
    obs_pattern = re.compile(rf"\bOBS {obs_major}(?:\.|\s)")
    plugin_pattern = re.compile(
        rf"\[{re.escape(plugin_name)}\] plugin loaded \(plugin version [^\n]*, API version "
        rf"{re.escape(api_version)}\)"
    )
    for path in reversed(files):
        try:
            contents = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        if obs_pattern.search(contents) and plugin_pattern.search(contents):
            if "Rosetta translation used: true" in contents:
                return None, f"OBS log shows the process used Rosetta translation: {path.name}"
            return path, None
    if not files:
        return None, "No OBS startup logs were found"
    return None, (
        f"No OBS {obs_major} log confirmed {plugin_name!r} loaded with API {api_version}"
    )


def _report(
    files: list[Path],
    selected: Path | None,
    error: str | None,
    plugin_name: str,
    obs_major: int,
    api_version: str,
    setup_outcome: str,
    expected_architecture: str,
) -> dict[str, Any]:
    actual_architecture = platform.machine()
    if actual_architecture != expected_architecture:
        architecture_error = (
            f"The smoke runner architecture is {actual_architecture!r}, "
            f"expected {expected_architecture!r}"
        )
        error = f"{error}; {architecture_error}" if error else architecture_error
        selected = None
    return {
        "schema_version": 1,
        "status": "plugin_load_verified" if selected and not error else "plugin_load_unverified",
        "plugin": plugin_name,
        "obs_major": obs_major,
        "plugin_api_version": api_version,
        "expected_architecture": expected_architecture,
        "observed_architecture": actual_architecture,
        "onsdriver_setup_outcome": setup_outcome,
        "evidence_log": selected.name if selected else None,
        "available_logs": [path.name for path in files],
        "error": error,
        "scope": "OBS module load only; operator workflows and sustained runtime are not measured",
    }


def _copy_logs(files: list[Path], destination: Path) -> None:
    destination.mkdir(parents=True, exist_ok=True)
    for path in files:
        shutil.copy2(path, destination / path.name)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config-root", type=Path, required=True)
    parser.add_argument("--firsttime-log-dir", type=Path, default=Path("logs"))
    parser.add_argument("--artifact-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--plugin", default="face-tracker")
    parser.add_argument("--obs-major", type=int, default=33)
    parser.add_argument("--api-version", default="33.0.0")
    parser.add_argument("--expected-architecture", default="x86_64")
    parser.add_argument("--setup-outcome", default="unknown")
    args = parser.parse_args(argv)

    config_logs = args.config_root / "logs"
    files = _log_files((config_logs, args.firsttime_log_dir))
    selected, error = _inspect_logs(files, args.plugin, args.obs_major, args.api_version)
    report = _report(
        files,
        selected,
        error,
        args.plugin,
        args.obs_major,
        args.api_version,
        args.setup_outcome,
        args.expected_architecture,
    )
    try:
        args.artifact_dir.mkdir(parents=True, exist_ok=True)
        _copy_logs(files, args.artifact_dir)
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    except OSError as write_error:
        print(f"Could not preserve OBS smoke evidence: {write_error}", file=sys.stderr)
        return 1

    print(json.dumps(report, indent=2, sort_keys=True))
    if report["status"] != "plugin_load_verified":
        if files:
            try:
                latest = files[-1].read_text(encoding="utf-8", errors="replace").splitlines()
                print("Latest OBS log tail:", file=sys.stderr)
                print("\n".join(latest[-80:]), file=sys.stderr)
            except OSError:
                pass
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
