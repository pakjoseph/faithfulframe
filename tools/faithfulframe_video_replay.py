#!/usr/bin/env python3
"""Stream sampled video frames into FaithfulFrame's dlib HOG replay diagnostic."""

from __future__ import annotations

import argparse
import subprocess
import sys
from pathlib import Path
from typing import Callable, Sequence


def ffmpeg_command(ffmpeg_bin: str, video: Path, fps: int, max_width: int) -> list[str]:
    video_filter = (
        f"fps={fps},scale=w='min({max_width},iw)':h=-2:force_original_aspect_ratio=decrease"
    )
    return [
        ffmpeg_bin,
        "-nostdin",
        "-hide_banner",
        "-loglevel",
        "error",
        "-i",
        str(video),
        "-an",
        "-vf",
        video_filter,
        "-f",
        "image2pipe",
        "-vcodec",
        "ppm",
        "-pix_fmt",
        "rgb24",
        "pipe:1",
    ]


def replay_command(replay_bin: str, model: Path, fps: int, csv: Path | None) -> list[str]:
    command = [replay_bin, "--model", str(model), "--ppm-stdin", "--fps", str(fps)]
    if csv is not None:
        command.extend(["--csv", str(csv)])
    return command


def run_pipeline(
    *,
    ffmpeg_bin: str,
    video: Path,
    replay_bin: str,
    model: Path,
    fps: int,
    max_width: int,
    csv: Path | None,
    popen: Callable[..., subprocess.Popen] = subprocess.Popen,
) -> int:
    ffmpeg_process = None
    replay_process = None
    try:
        ffmpeg_process = popen(ffmpeg_command(ffmpeg_bin, video, fps, max_width), stdout=subprocess.PIPE)
        try:
            replay_process = popen(
                replay_command(replay_bin, model, fps, csv),
                stdin=ffmpeg_process.stdout,
            )
        finally:
            if ffmpeg_process.stdout is not None:
                ffmpeg_process.stdout.close()

        replay_status = replay_process.wait()
        if replay_status != 0:
            ffmpeg_process.terminate()
        ffmpeg_status = ffmpeg_process.wait()
        if replay_status != 0:
            return replay_status if replay_status > 0 else 1
        return ffmpeg_status if ffmpeg_status > 0 else (1 if ffmpeg_status < 0 else 0)
    except OSError as error:
        print(f"Could not start video replay pipeline: {error}", file=sys.stderr)
        if replay_process is not None:
            replay_process.terminate()
            replay_process.wait()
        if ffmpeg_process is not None:
            if ffmpeg_process.stdout is not None:
                ffmpeg_process.stdout.close()
            ffmpeg_process.terminate()
            ffmpeg_process.wait()
        return 2


def parse_args(argv: Sequence[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description=(
            "Sample a video with FFmpeg and stream frames into the production dlib HOG detector replay. "
            "This is a detector diagnostic only; it does not validate OBS, target selection, or tracking policy."
        )
    )
    parser.add_argument("--video", required=True, type=Path, help="input video file")
    parser.add_argument("--model", required=True, type=Path, help="dlib frontal_face_detector.dat model")
    parser.add_argument("--replay-bin", default="faithfulframe-offline-replay", help="replay executable")
    parser.add_argument("--ffmpeg-bin", default="ffmpeg", help="FFmpeg executable")
    parser.add_argument("--fps", type=int, default=5, help="sample rate from 1 through 120 frames per second")
    parser.add_argument("--max-width", type=int, default=1920, help="maximum sampled frame width (default: 1920)")
    parser.add_argument("--csv", type=Path, help="write detector results to this CSV file")
    args = parser.parse_args(argv)
    if not 1 <= args.fps <= 120:
        parser.error("--fps must be between 1 and 120")
    if args.max_width < 80:
        parser.error("--max-width must be at least 80 pixels")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.video.is_file():
        print(f"Video file does not exist: {args.video}", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"dlib model does not exist: {args.model}", file=sys.stderr)
        return 2
    if args.csv is not None:
        video_path = args.video.resolve()
        model_path = args.model.resolve()
        csv_path = args.csv.resolve()
        if csv_path in (video_path, model_path):
            print("CSV output must not replace the video or detector model", file=sys.stderr)
            return 2
    return run_pipeline(
        ffmpeg_bin=args.ffmpeg_bin,
        video=args.video,
        replay_bin=args.replay_bin,
        model=args.model,
        fps=args.fps,
        max_width=args.max_width,
        csv=args.csv,
    )


if __name__ == "__main__":
    raise SystemExit(main())
