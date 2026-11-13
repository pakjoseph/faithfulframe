#!/usr/bin/env python3
"""Reject unaudited third-party crates in FaithfulFrame's production Rust graph."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess
import sys


EXPECTED_RUNTIME_PACKAGES = {"faithfulframe-ffi", "faithfulframe-core"}


def dependency_errors(metadata: dict) -> list[str]:
    packages = {package["id"]: package for package in metadata.get("packages", [])}
    resolution = metadata.get("resolve") or {}
    reachable = {node["id"] for node in resolution.get("nodes", [])}
    if not reachable:
        return ["Cargo did not provide a resolved production dependency graph"]

    errors = []
    reachable_packages = []
    for package_id in sorted(reachable):
        package = packages.get(package_id)
        if package is None:
            errors.append(f"resolved package is missing metadata: {package_id}")
            continue
        reachable_packages.append(package)
        if package.get("name") not in EXPECTED_RUNTIME_PACKAGES or package.get("source") is not None:
            errors.append(
                "unaudited Rust production dependency: "
                f"{package.get('name', '<unknown>')} ({package.get('source') or package_id})"
            )

    names = {package["name"] for package in reachable_packages}
    if names != EXPECTED_RUNTIME_PACKAGES:
        errors.append(
            "Rust production graph must contain exactly faithfulframe-ffi and faithfulframe-core; "
            f"found {', '.join(sorted(names))}"
        )
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--manifest", type=Path, required=True, help="production Rust FFI Cargo.toml")
    args = parser.parse_args()
    command = [
        "cargo",
        "metadata",
        "--format-version",
        "1",
        "--locked",
        "--offline",
        "--all-features",
        "--manifest-path",
        str(args.manifest),
    ]
    try:
        result = subprocess.run(command, check=True, capture_output=True, text=True)
        metadata = json.loads(result.stdout)
    except (OSError, subprocess.CalledProcessError, json.JSONDecodeError) as error:
        print(f"could not inspect Rust production dependencies: {error}", file=sys.stderr)
        return 2

    errors = dependency_errors(metadata)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        print(
            "Audit the new crate's license and release obligations, then update "
            "the explicit production dependency allowlist.",
            file=sys.stderr,
        )
        return 1

    print("Rust production dependency graph contains only the audited local core.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
