#!/usr/bin/env python3
"""Stream sampled video frames into FaithfulFrame's dlib HOG replay diagnostic."""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Callable, Sequence


def ffmpeg_command(
    ffmpeg_bin: str, video: Path, fps: int, max_width: int, max_height: int = 1080
) -> list[str]:
    video_filter = (
        f"fps={fps},scale=w='min({max_width},iw)':h='min({max_height},ih)':"
        "force_original_aspect_ratio=decrease"
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


def same_file_or_path(left: Path, right: Path) -> bool:
    try:
        if left.exists() and right.exists() and os.path.samefile(left, right):
            return True
    except OSError:
        pass
    return left.resolve() == right.resolve()


def executable_path(command: str) -> Path | None:
    candidate = Path(command)
    if candidate.is_file():
        return candidate
    located = shutil.which(command)
    return Path(located) if located else None


def stop_process(process: subprocess.Popen | None) -> None:
    """Stop a child process left behind when the pipeline exits early."""
    if process is None or process.poll() is not None:
        return
    try:
        process.terminate()
    except OSError:
        # The child may have exited between poll() and terminate().
        pass
    try:
        process.wait(timeout=5)
    except subprocess.TimeoutExpired:
        try:
            process.kill()
        except OSError:
            pass
        try:
            process.wait()
        except OSError:
            pass
    except OSError:
        pass


def run_pipeline(
    *,
    ffmpeg_bin: str,
    video: Path,
    replay_bin: str,
    model: Path,
    fps: int,
    max_width: int,
    csv: Path | None,
    max_height: int = 1080,
    popen: Callable[..., subprocess.Popen] = subprocess.Popen,
) -> int:
    ffmpeg_process = None
    replay_process = None
    try:
        ffmpeg_process = popen(
            ffmpeg_command(ffmpeg_bin, video, fps, max_width, max_height), stdout=subprocess.PIPE
        )
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
            stop_process(ffmpeg_process)
        ffmpeg_status = ffmpeg_process.wait()
        if replay_status != 0:
            return replay_status if replay_status > 0 else 1
        return ffmpeg_status if ffmpeg_status > 0 else (1 if ffmpeg_status < 0 else 0)
    except OSError as error:
        print(f"Could not start video replay pipeline: {error}", file=sys.stderr)
        return 2
    finally:
        # Also clean up on interruption (for example, Ctrl+C during a long replay).
        stop_process(replay_process)
        if ffmpeg_process is not None and ffmpeg_process.stdout is not None:
            ffmpeg_process.stdout.close()
        stop_process(ffmpeg_process)


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
    parser.add_argument("--max-height", type=int, default=1080, help="maximum sampled frame height (default: 1080)")
    parser.add_argument("--csv", type=Path, help="write detector results to this CSV file")
    args = parser.parse_args(argv)
    if not 1 <= args.fps <= 120:
        parser.error("--fps must be between 1 and 120")
    if args.max_width < 80:
        parser.error("--max-width must be at least 80 pixels")
    if args.max_height < 80:
        parser.error("--max-height must be at least 80 pixels")
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
        protected_paths = [args.video, args.model, Path(__file__)]
        for command in (args.ffmpeg_bin, args.replay_bin):
            resolved = executable_path(command)
            if resolved is not None:
                protected_paths.append(resolved)
        if any(same_file_or_path(args.csv, path) for path in protected_paths):
            print("CSV output must not replace an input, executable, or replay script", file=sys.stderr)
            return 2
    return run_pipeline(
        ffmpeg_bin=args.ffmpeg_bin,
        video=args.video,
        replay_bin=args.replay_bin,
        model=args.model,
        fps=args.fps,
        max_width=args.max_width,
        csv=args.csv,
        max_height=args.max_height,
    )


if __name__ == "__main__":
    raise SystemExit(main())
