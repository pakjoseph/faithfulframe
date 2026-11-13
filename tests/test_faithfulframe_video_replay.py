import io
import sys
import unittest
from pathlib import Path
from unittest.mock import Mock

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
    def test_ffmpeg_command_samples_without_shell_or_upscaling(self):
        command = video_replay.ffmpeg_command("ffmpeg", Path("service recording.mov"), 7, 1440)
        self.assertEqual(command[0], "ffmpeg")
        self.assertIn("service recording.mov", command)
        self.assertIn("fps=7,scale=w='min(1440,iw)':h=-2:force_original_aspect_ratio=decrease", command)
        self.assertEqual(command[-1], "pipe:1")
        self.assertNotIn("shell", command)

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
