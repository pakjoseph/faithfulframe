import argparse
from contextlib import redirect_stderr
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import faithfulframe_video_replay as video_replay  # noqa: E402


class FakeProcess:
    def __init__(self, status=0, wait_error=None):
        self.stdout = io.BytesIO()
        self.status = status
        self.terminated = False
        self.killed = False
        self.waited = False
        self.wait_error = wait_error

    def poll(self):
        return self.status if self.waited else None

    def wait(self, timeout=None):
        del timeout
        if self.wait_error is not None:
            error = self.wait_error
            self.wait_error = None
            raise error
        self.waited = True
        return self.status

    def terminate(self):
        self.terminated = True

    def kill(self):
        self.killed = True


class VideoReplayTests(unittest.TestCase):
    def test_output_collision_detects_hard_link_aliases(self):
        with tempfile.TemporaryDirectory() as directory:
            original = Path(directory) / "video.mp4"
            output = Path(directory) / "results.csv"
            original.write_bytes(b"input")
            try:
                os.link(original, output)
            except OSError as error:
                self.skipTest(f"hard links are unavailable: {error}")

            self.assertTrue(video_replay.same_file_or_path(output, original))
            normalized = original.parent / "." / "video.mp4"
            self.assertTrue(video_replay.same_file_or_path(original, normalized))

    def test_resolves_executable_from_path_or_explicit_file(self):
        self.assertEqual(video_replay.executable_path(sys.executable), Path(sys.executable))

    def test_main_rejects_csv_that_would_replace_replay_executable(self):
        with tempfile.TemporaryDirectory() as directory:
            video = Path(directory) / "input.mp4"
            model = Path(directory) / "model.dat"
            video.touch()
            model.touch()
            args = argparse.Namespace(
                video=video,
                model=model,
                replay_bin=sys.executable,
                ffmpeg_bin="ffmpeg",
                fps=5,
                max_width=1920,
                max_height=1080,
                csv=Path(sys.executable),
                trace=None,
                session_policy=False,
                session_replay_bin="faithfulframe-offline-session-replay",
                operator_select_first_candidate_at_ms=None,
            )
            run = Mock()
            with patch.object(video_replay, "parse_args", return_value=args), patch.object(
                video_replay, "run_pipeline", run
            ), redirect_stderr(io.StringIO()):
                status = video_replay.main([])

            self.assertEqual(status, 2)
            run.assert_not_called()

    def test_ffmpeg_command_samples_without_shell_or_upscaling(self):
        command = video_replay.ffmpeg_command("ffmpeg", Path("service recording.mov"), 7, 1440)
        self.assertEqual(command[0], "ffmpeg")
        self.assertIn("service recording.mov", command)
        self.assertIn(
            "fps=7,scale=w='min(1440,iw)':h='min(1080,ih)':force_original_aspect_ratio=decrease",
            command,
        )
        self.assertEqual(command[-1], "pipe:1")
        self.assertNotIn("shell", command)

    def test_ffmpeg_command_caps_portrait_height_without_upscaling(self):
        command = video_replay.ffmpeg_command("ffmpeg", Path("portrait.mov"), 5, 1920, 1080)
        filter_arg = command[command.index("-vf") + 1]
        self.assertIn("scale=w='min(1920,iw)':h='min(1080,ih)'", filter_arg)

    def test_cli_requires_minimum_sampled_height(self):
        args = video_replay.parse_args(
            ["--video", "input.mp4", "--model", "model.dat", "--max-height", "720"]
        )
        self.assertEqual(args.max_height, 720)
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            video_replay.parse_args(
                ["--video", "input.mp4", "--model", "model.dat", "--max-height", "79"]
            )

    def test_cli_uses_production_like_session_sampling_rate_by_default(self):
        detector = video_replay.parse_args(["--video", "input.mp4", "--model", "model.dat"])
        session = video_replay.parse_args(
            ["--video", "input.mp4", "--model", "model.dat", "--session-policy"]
        )
        self.assertEqual(detector.fps, 5)
        self.assertEqual(session.fps, 30)

    def test_replay_command_passes_stream_mode_fps_and_optional_csv(self):
        command = video_replay.replay_command("replay", Path("model.dat"), 5, Path("results.csv"))
        self.assertEqual(
            command,
            ["replay", "--model", "model.dat", "--ppm-stdin", "--fps", "5", "--csv", "results.csv"],
        )
        self.assertEqual(
            video_replay.replay_command("replay", Path("model.dat"), 5, None)[-3:],
            ["--ppm-stdin", "--fps", "5"],
        )

    def test_session_replay_command_passes_dimensions_trace_and_explicit_selection(self):
        command = video_replay.session_replay_command(
            "session-replay", Path("model.dat"), 7, Path("trace.jsonl"), 1280, 720, 1400
        )
        self.assertEqual(
            command,
            [
                "session-replay",
                "--model",
                "model.dat",
                "--ppm-stdin",
                "--fps",
                "7",
                "--output-width",
                "1280",
                "--output-height",
                "720",
                "--trace",
                "trace.jsonl",
                "--operator-select-first-candidate-at-ms",
                "1400",
            ],
        )

    def test_cli_requires_session_mode_for_operator_selection_and_trace(self):
        common = ["--video", "input.mp4", "--model", "model.dat"]
        for extra in (
            ["--operator-select-first-candidate-at-ms", "0"],
            ["--trace", "trace.jsonl"],
        ):
            with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
                video_replay.parse_args(common + extra)

    def test_cli_rejects_detector_csv_in_session_mode(self):
        with redirect_stderr(io.StringIO()), self.assertRaises(SystemExit):
            video_replay.parse_args(
                [
                    "--video",
                    "input.mp4",
                    "--model",
                    "model.dat",
                    "--session-policy",
                    "--csv",
                    "detector.csv",
                ]
            )

    def test_pipeline_connects_and_closes_parent_pipe_then_waits_for_both(self):
        ffmpeg = FakeProcess()
        replay = FakeProcess()
        popen = Mock(side_effect=[ffmpeg, replay])
        status = video_replay.run_pipeline(
            ffmpeg_bin="ffmpeg",
            video=Path("input.mp4"),
            replay_bin="replay",
            model=Path("model.dat"),
            fps=5,
            max_width=1920,
            csv=None,
            popen=popen,
        )
        self.assertEqual(status, 0)
        self.assertEqual(popen.call_count, 2)
        self.assertIs(popen.call_args_list[1].kwargs["stdin"], ffmpeg.stdout)
        self.assertTrue(ffmpeg.stdout.closed)
        self.assertTrue(replay.waited)
        self.assertTrue(ffmpeg.waited)

    def test_session_policy_pipeline_selects_policy_replay_binary(self):
        ffmpeg = FakeProcess()
        replay = FakeProcess()
        popen = Mock(side_effect=[ffmpeg, replay])
        status = video_replay.run_pipeline(
            ffmpeg_bin="ffmpeg",
            video=Path("input.mp4"),
            replay_bin="detector-replay",
            model=Path("model.dat"),
            fps=5,
            max_width=1920,
            csv=None,
            session_policy=True,
            session_replay_bin="session-replay",
            trace=Path("trace.jsonl"),
            operator_select_first_candidate_at_ms=3000,
            popen=popen,
        )
        self.assertEqual(status, 0)
        session_command = popen.call_args_list[1].args[0]
        self.assertEqual(session_command[0], "session-replay")
        self.assertIn("--operator-select-first-candidate-at-ms", session_command)

    def test_replay_failure_stops_ffmpeg_and_preserves_failure_status(self):
        ffmpeg = FakeProcess()
        replay = FakeProcess(status=3)
        popen = Mock(side_effect=[ffmpeg, replay])
        status = video_replay.run_pipeline(
            ffmpeg_bin="ffmpeg",
            video=Path("input.mp4"),
            replay_bin="replay",
            model=Path("model.dat"),
            fps=5,
            max_width=1920,
            csv=None,
            popen=popen,
        )
        self.assertEqual(status, 3)
        self.assertTrue(ffmpeg.terminated)

    def test_replay_launch_failure_closes_and_stops_ffmpeg(self):
        ffmpeg = FakeProcess()
        popen = Mock(side_effect=[ffmpeg, FileNotFoundError("missing replay")])
        status = video_replay.run_pipeline(
            ffmpeg_bin="ffmpeg",
            video=Path("input.mp4"),
            replay_bin="replay",
            model=Path("model.dat"),
            fps=5,
            max_width=1920,
            csv=None,
            popen=popen,
        )
        self.assertEqual(status, 2)
        self.assertTrue(ffmpeg.stdout.closed)
        self.assertTrue(ffmpeg.terminated)
        self.assertTrue(ffmpeg.waited)

    def test_keyboard_interrupt_stops_both_child_processes(self):
        ffmpeg = FakeProcess()
        replay = FakeProcess(wait_error=KeyboardInterrupt())
        popen = Mock(side_effect=[ffmpeg, replay])

        with self.assertRaises(KeyboardInterrupt):
            video_replay.run_pipeline(
                ffmpeg_bin="ffmpeg",
                video=Path("input.mp4"),
                replay_bin="replay",
                model=Path("model.dat"),
                fps=5,
                max_width=1920,
                csv=None,
                popen=popen,
            )

        self.assertTrue(ffmpeg.stdout.closed)
        self.assertTrue(replay.terminated)
        self.assertTrue(replay.waited)
        self.assertTrue(ffmpeg.terminated)
        self.assertTrue(ffmpeg.waited)

    def test_stalled_child_is_killed_after_grace_period(self):
        class StalledProcess(FakeProcess):
            def wait(self, timeout=None):
                if timeout is not None:
                    raise video_replay.subprocess.TimeoutExpired("child", timeout)
                self.waited = True
                return self.status

        process = StalledProcess()
        video_replay.stop_process(process)

        self.assertTrue(process.terminated)
        self.assertTrue(process.killed)
        self.assertTrue(process.waited)


if __name__ == "__main__":
    unittest.main()
