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


def session_replay_command(
    replay_bin: str,
    model: Path,
    fps: int,
    trace: Path | None,
    output_width: int,
    output_height: int,
    operator_select_first_candidate_at_ms: int | None,
) -> list[str]:
    command = [replay_bin, "--model", str(model), "--ppm-stdin", "--fps", str(fps)]
    command.extend(["--output-width", str(output_width), "--output-height", str(output_height)])
    if trace is not None:
        command.extend(["--trace", str(trace)])
    if operator_select_first_candidate_at_ms is not None:
        command.extend(
            [
                "--operator-select-first-candidate-at-ms",
                str(operator_select_first_candidate_at_ms),
            ]
        )
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
    session_policy: bool = False,
    session_replay_bin: str = "faithfulframe-offline-session-replay",
    trace: Path | None = None,
    output_width: int = 1920,
    output_height: int = 1080,
    operator_select_first_candidate_at_ms: int | None = None,
    popen: Callable[..., subprocess.Popen] = subprocess.Popen,
) -> int:
    ffmpeg_process = None
    replay_process = None
    try:
        ffmpeg_process = popen(
            ffmpeg_command(ffmpeg_bin, video, fps, max_width, max_height), stdout=subprocess.PIPE
        )
        try:
            if session_policy:
                command = session_replay_command(
                    session_replay_bin,
                    model,
                    fps,
                    trace,
                    output_width,
                    output_height,
                    operator_select_first_candidate_at_ms,
                )
            else:
                command = replay_command(replay_bin, model, fps, csv)
            replay_process = popen(command, stdin=ffmpeg_process.stdout)
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
            "Sample a video with FFmpeg and stream frames into either the dlib HOG detector diagnostic or "
            "the production dlib tracker plus Rust session policy replay. No video or facial imagery is saved."
        )
    )
    parser.add_argument("--video", required=True, type=Path, help="input video file")
    parser.add_argument("--model", required=True, type=Path, help="dlib frontal_face_detector.dat model")
    parser.add_argument("--replay-bin", default="faithfulframe-offline-replay", help="replay executable")
    parser.add_argument(
        "--session-policy",
        action="store_true",
        help="replay through face_tracker_manager, dlib correlation trackers, and the Rust session core",
    )
    parser.add_argument(
        "--session-replay-bin",
        default="faithfulframe-offline-session-replay",
        help="offline Rust session replay executable",
    )
    parser.add_argument("--ffmpeg-bin", default="ffmpeg", help="FFmpeg executable")
    parser.add_argument("--fps", type=int, help="sample rate from 1 through 120 frames per second")
    parser.add_argument("--max-width", type=int, default=1920, help="maximum sampled frame width (default: 1920)")
    parser.add_argument("--max-height", type=int, default=1080, help="maximum sampled frame height (default: 1080)")
    parser.add_argument("--csv", type=Path, help="write detector results to this CSV file")
    parser.add_argument("--trace", type=Path, help="write session replay decisions to this JSONL file")
    parser.add_argument("--output-width", type=int, default=1920, help="session output width in pixels")
    parser.add_argument("--output-height", type=int, default=1080, help="session output height in pixels")
    parser.add_argument(
        "--operator-select-first-candidate-at-ms",
        type=int,
        help=(
            "simulate one explicit operator target selection at or after this replay timestamp; "
            "selection never happens automatically"
        ),
    )
    args = parser.parse_args(argv)
    if args.fps is None:
        args.fps = 30 if args.session_policy else 5
    if not 1 <= args.fps <= 120:
        parser.error("--fps must be between 1 and 120")
    if args.max_width < 80:
        parser.error("--max-width must be at least 80 pixels")
    if args.max_height < 80:
        parser.error("--max-height must be at least 80 pixels")
    if args.output_width < 80 or args.output_height < 80:
        parser.error("--output-width and --output-height must be at least 80 pixels")
    if args.output_width > 16384 or args.output_height > 16384:
        parser.error("--output-width and --output-height must not exceed 16384 pixels")
    if args.csv is not None and args.session_policy:
        parser.error("--csv is for detector mode; use --trace with --session-policy")
    if args.trace is not None and not args.session_policy:
        parser.error("--trace requires --session-policy")
    if args.operator_select_first_candidate_at_ms is not None and (
        not args.session_policy or args.operator_select_first_candidate_at_ms < 0
    ):
        parser.error("operator selection requires --session-policy and a non-negative timestamp")
    return args


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(argv)
    if not args.video.is_file():
        print(f"Video file does not exist: {args.video}", file=sys.stderr)
        return 2
    if not args.model.is_file():
        print(f"dlib model does not exist: {args.model}", file=sys.stderr)
        return 2
    if args.csv is not None or args.trace is not None:
        output_path = args.trace if args.trace is not None else args.csv
        protected_paths = [args.video, args.model, Path(__file__)]
        replay_commands = [args.ffmpeg_bin]
        replay_commands.append(args.session_replay_bin if args.session_policy else args.replay_bin)
        for command in replay_commands:
            resolved = executable_path(command)
            if resolved is not None:
                protected_paths.append(resolved)
        if any(same_file_or_path(output_path, path) for path in protected_paths):
            print("Output must not replace an input, executable, or replay script", file=sys.stderr)
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
        session_policy=args.session_policy,
        session_replay_bin=args.session_replay_bin,
        trace=args.trace,
        output_width=args.output_width,
        output_height=args.output_height,
        operator_select_first_candidate_at_ms=args.operator_select_first_candidate_at_ms,
    )


if __name__ == "__main__":
    raise SystemExit(main())
