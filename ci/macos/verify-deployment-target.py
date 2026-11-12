#!/usr/bin/env python3
"""Check that every Mach-O in a packaged plugin supports its configured macOS target."""

import argparse
import pathlib
import re
import subprocess
import sys


def version_tuple(value):
    parts = tuple(int(part) for part in value.split("."))
    return parts + (0,) * (3 - len(parts))


def configured_target(cache_path):
    text = pathlib.Path(cache_path).read_text(encoding="utf-8")
    match = re.search(r"^CMAKE_OSX_DEPLOYMENT_TARGET:[^=]+=([^\r\n]+)$", text, re.MULTILINE)
    if not match:
        raise ValueError(f"CMAKE_OSX_DEPLOYMENT_TARGET is missing from {cache_path}")
    target = match.group(1).strip()
    if not re.fullmatch(r"\d+(?:\.\d+){0,2}", target):
        raise ValueError(f"Invalid macOS deployment target {target!r}")
    return target


def macho_minimum_versions(path):
    description = subprocess.run(
        ["file", "-b", str(path)], check=True, capture_output=True, text=True
    ).stdout
    if "Mach-O" not in description:
        return None

    output = subprocess.run(
        ["otool", "-arch", "all", "-l", str(path)],
        check=True,
        capture_output=True,
        text=True,
    ).stdout
    versions = []
    for block in re.split(r"(?=^Load command \d+\s*$)", output, flags=re.MULTILINE):
        if re.search(r"^\s*cmd\s+LC_BUILD_VERSION\s*$", block, re.MULTILINE):
            match = re.search(r"^\s*minos\s+(\d+(?:\.\d+){1,2})\s*$", block, re.MULTILINE)
            if match:
                versions.append(("LC_BUILD_VERSION", match.group(1)))
        elif re.search(r"^\s*cmd\s+LC_VERSION_MIN_MACOSX\s*$", block, re.MULTILINE):
            match = re.search(r"^\s*version\s+(\d+(?:\.\d+){1,2})\s*$", block, re.MULTILINE)
            if match:
                versions.append(("LC_VERSION_MIN_MACOSX", match.group(1)))
    return versions


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", required=True, type=pathlib.Path, help="Installed plugin bundle")
    parser.add_argument("--cmake-cache", required=True, type=pathlib.Path)
    args = parser.parse_args()

    try:
        target = configured_target(args.cmake_cache)
        target_tuple = version_tuple(target)
        if not args.root.is_dir():
            raise ValueError(f"Plugin bundle does not exist: {args.root}")

        macho_count = 0
        failures = []
        for path in sorted(args.root.rglob("*")):
            if not path.is_file():
                continue
            versions = macho_minimum_versions(path)
            if versions is None:
                continue
            macho_count += 1
            if not versions:
                failures.append(f"{path}: no macOS minimum-version load command found")
                continue
            for command, version in versions:
                if version_tuple(version) > target_tuple:
                    failures.append(f"{path}: {command} requires macOS {version} (target {target})")

        if macho_count == 0:
            raise ValueError(f"No Mach-O files found under {args.root}")
        if failures:
            print("\n".join(failures), file=sys.stderr)
            return 1
        print(f"Verified {macho_count} Mach-O files support macOS {target} or earlier.")
        return 0
    except (OSError, subprocess.CalledProcessError, ValueError) as error:
        print(f"macOS deployment-target verification failed: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
