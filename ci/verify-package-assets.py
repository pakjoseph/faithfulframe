#!/usr/bin/env python3
"""Fail release checks when required notices or model files are missing."""

import argparse
import hashlib
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
    "LICENSE": b"Version 2, June 1991",
    "LICENSE-libvisca.txt": b"Version 2.1, February 1999",
    "LICENSE-dlib.txt": b"Boost Software License - Version 1.0",
    "LICENSE-dlib-models": b"CC0 1.0 Universal",
}

# Model binaries are a distinct licensing/reproducibility surface. Adding a
# model is an explicit review gate: record its provenance/license here and add
# a fixture that proves it is packaged intentionally. Match by filename so
# this remains effective regardless of whether a platform installer nests the
# data directory under a plugin bundle or a package-specific prefix. Values
# record the upstream provenance, matching packaged notice, and SHA-256 of the
# exact uncompressed file.
ALLOWED_MODEL_ASSETS = {
    "frontal_face_detector.dat": (
        "norihiro/obs-face-tracker 0.7.0-hogdata",
        "LICENSE-dlib.txt",
        "f3f7aa833fb4a14a46fc48689f45b98d85ef72d6dfaf8bdc012b7d3c522a5426",
    ),
    "mmod_human_face_detector.dat": (
        "davisking/dlib-models fd81b6308a6a73d4ce08859eb2f4b628a21e27a2",
        "LICENSE-dlib-models",
        "be467b1a76f482693de3b0f6a1ff91d092319be71523d4d4b0628f6a53fcb87a",
    ),
    "shape_predictor_5_face_landmarks.dat": (
        "davisking/dlib-models fd81b6308a6a73d4ce08859eb2f4b628a21e27a2",
        "LICENSE-dlib-models",
        "c4b1e9804792707d3a405c2c16a80a20269e6675021f64a41d30fffafbc41888",
    ),
}

MODEL_FILE_SUFFIXES = {
    ".dat",
    ".onnx",
    ".pb",
    ".pt",
    ".pth",
    ".tflite",
    ".weights",
    ".engine",
    ".bin",
    ".dnn",
    ".gguf",
    ".h5",
    ".hdf5",
    ".keras",
    ".mlmodel",
    ".model",
    ".plan",
    ".safetensors",
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
    for path in files:
        relative_path = path.relative_to(root)
        # Compressed model archives are also model assets. Release packages
        # currently contain only the unpacked, allowlisted dlib files.
        is_model_asset = path.suffix.lower() in MODEL_FILE_SUFFIXES or path.name.lower().endswith(
            ".dat.bz2"
        )
        reviewed_asset = ALLOWED_MODEL_ASSETS.get(path.name)
        if is_model_asset and reviewed_asset is None and path.name not in RESTRICTED_FILES:
            errors.append(f"unreviewed model asset is packaged: {relative_path}")
        elif is_model_asset and reviewed_asset:
            provenance, required_notice, expected_sha256 = reviewed_asset
            if required_notice not in names:
                errors.append(
                    f"model asset from {provenance} is missing its declared license notice "
                    f"{required_notice}: {relative_path}"
                )
            actual_sha256 = _sha256_file(path)
            if actual_sha256 != expected_sha256:
                errors.append(
                    f"model asset checksum mismatch for {relative_path}: "
                    f"expected {expected_sha256}, got {actual_sha256}"
                )
    errors.extend(f"required package file is empty: {path.relative_to(root)}"
                  for path in files if path.name in REQUIRED_FILES and path.stat().st_size == 0)
    for name, marker in REQUIRED_LICENSE_MARKERS.items():
        for path in files:
            if path.name == name and path.stat().st_size and marker not in path.read_bytes():
                errors.append(f"license notice has unexpected contents: {path.relative_to(root)}")
    return errors


def _sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as asset:
        for chunk in iter(lambda: asset.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


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
