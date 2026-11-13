import argparse
import csv
import io
import subprocess
import sys
from pathlib import Path


def frame_bytes():
    # An all-black synthetic frame keeps this test independent of recordings,
    # faces, and biometric data while still exercising the production detector.
    return b"P6\n80 80\n255\n" + bytes(80 * 80 * 3)


def run(binary, model, payload):
    return subprocess.run(
        [binary, "--model", model, "--ppm-stdin", "--fps", "5"],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--model", required=True)
    args = parser.parse_args()

    executable_size = Path(args.binary).stat().st_size
    overwrite = subprocess.run(
        [args.binary, "--model", args.model, "--ppm-stdin", "--csv", args.binary],
        input=b"",
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    if (
        overwrite.returncode != 2
        or b"must not replace the replay executable" not in overwrite.stderr
        or Path(args.binary).stat().st_size != executable_size
    ):
        raise SystemExit(
            f"replay accepted or damaged its executable as CSV output: "
            f"status={overwrite.returncode}, stderr={overwrite.stderr!r}"
        )

    frame = frame_bytes()
    replay = run(args.binary, args.model, frame + frame)
    if replay.returncode != 0:
        sys.stderr.write(replay.stderr.decode(errors="replace"))
        raise SystemExit(f"valid PPM stream failed with status {replay.returncode}")
    rows = list(csv.reader(io.StringIO(replay.stdout.decode("utf-8"))))
    frame_times = {(row[0], row[1]) for row in rows[1:]}
    if not {("0", "0"), ("1", "200")}.issubset(frame_times):
        raise SystemExit(f"replay did not emit both synthetic frames: {frame_times}")
    if b"frames=2" not in replay.stderr:
        raise SystemExit(f"replay summary did not report two frames: {replay.stderr!r}")
    if b"detector_percentile_samples=2 detector_percentiles_exact=1" not in replay.stderr:
        raise SystemExit(f"short replay did not report exact latency percentiles: {replay.stderr!r}")

    malformed = run(args.binary, args.model, frame + b"invalid")
    if malformed.returncode != 2 or b"PPM" not in malformed.stderr:
        raise SystemExit(
            f"malformed PPM stream did not fail closed: status={malformed.returncode}, "
            f"stderr={malformed.stderr!r}"
        )

    empty = run(args.binary, args.model, b"")
    if empty.returncode != 2 or b"contains no frames" not in empty.stderr:
        raise SystemExit(
            f"empty PPM stream did not fail closed: status={empty.returncode}, stderr={empty.stderr!r}"
        )


if __name__ == "__main__":
    main()
