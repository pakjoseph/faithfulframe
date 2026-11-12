#!/usr/bin/env python3
"""Fail release checks when required notices or model files are missing."""

import argparse
from pathlib import Path
import sys


REQUIRED_FILES = {
    "LICENSE",
    "LICENSE-libvisca.txt",
    "LICENSE-dlib.txt",
    "LICENSE-dlib-models",
    "frontal_face_detector.dat",
    "mmod_human_face_detector.dat",
    "shape_predictor_5_face_landmarks.dat",
}

REQUIRED_LICENSE_MARKERS = {
    "LICENSE": b"GNU GENERAL PUBLIC LICENSE",
    "LICENSE-libvisca.txt": b"GNU LESSER GENERAL PUBLIC LICENSE",
    "LICENSE-dlib.txt": b"Boost Software License - Version 1.0",
    "LICENSE-dlib-models": b"CC0 1.0 Universal",
}

RESTRICTED_FILES = {
    "shape_predictor_68_face_landmarks.dat",
    "shape_predictor_68_face_landmarks.dat.bz2",
    "shape_predictor_68_face_landmarks_GTX.dat",
    "shape_predictor_68_face_landmarks_GTX.dat.bz2",
    "LICENSE-shape_predictor_68_face_landmarks",
}


def package_errors(root: Path) -> list[str]:
    if not root.is_dir():
        return [f"package root is not a directory: {root}"]

    files = [path for path in root.rglob("*") if path.is_file()]
    names = {path.name for path in files}
    errors = [f"missing required package file: {name}" for name in sorted(REQUIRED_FILES - names)]
    errors.extend(f"restricted model asset is packaged: {path.relative_to(root)}"
                  for path in files if path.name in RESTRICTED_FILES)
    errors.extend(f"required package file is empty: {path.relative_to(root)}"
                  for path in files if path.name in REQUIRED_FILES and path.stat().st_size == 0)
    for name, marker in REQUIRED_LICENSE_MARKERS.items():
        for path in files:
            if path.name == name and path.stat().st_size and marker not in path.read_bytes():
                errors.append(f"license notice has unexpected contents: {path.relative_to(root)}")
    return errors


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True, help="extracted install tree or release archive")
    args = parser.parse_args()

    errors = package_errors(args.root)
    if errors:
        for error in errors:
            print(error, file=sys.stderr)
        return 1

    print("Package notices and default model assets verified.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
