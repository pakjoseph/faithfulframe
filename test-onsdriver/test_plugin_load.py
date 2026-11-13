'''
Test plugin is loaded
'''

import glob
import os
import shutil
import subprocess
import tempfile
import time
import uuid
import unittest
from onsdriver import obstest

PLUGIN_NAME = 'face-tracker'


def firsttime_setup_was_force_stopped(logsdir):
    firsttime_logs = glob.glob(os.path.join(logsdir, 'firsttime-*.txt'))
    for logfile in firsttime_logs:
        try:
            with open(logfile, 'r', encoding='utf-8') as fr:
                was_force_stopped = '[shutdown-plugin] force exit.' in fr.read()
        except OSError:
            continue
        if was_force_stopped:
            return True
    return False


def clear_inherited_setup_shutdown_sentinels(config_path, logsdir):
    """Clear OBS shutdown markers only when onsdriver records its forced exit."""
    safe_mode_sentinel = os.path.join(config_path, 'safe_mode')
    crash_sentinel_dir = os.path.join(config_path, '.sentinel')
    crash_sentinels = []
    if os.path.isdir(crash_sentinel_dir):
        crash_sentinels = [
            os.path.join(crash_sentinel_dir, name)
            for name in os.listdir(crash_sentinel_dir)
            if name.startswith('run_')
            and os.path.isfile(os.path.join(crash_sentinel_dir, name))
        ]

    has_inherited_sentinel = os.path.exists(safe_mode_sentinel) or bool(crash_sentinels)
    setup_was_force_stopped = firsttime_setup_was_force_stopped(logsdir)
    if not has_inherited_sentinel:
        return setup_was_force_stopped
    if setup_was_force_stopped:
        for sentinel in [safe_mode_sentinel, *crash_sentinels]:
            if not os.path.exists(sentinel):
                continue
            try:
                os.remove(sentinel)
            except OSError as error:
                raise RuntimeError(
                    f'Unable to remove inherited OBS shutdown marker: {sentinel}'
                ) from error
        return True
    raise RuntimeError(
        'OBS setup profile has an unclean-shutdown sentinel '
        'without an onsdriver force-exit record'
    )


class SetupShutdownSentinelTest(unittest.TestCase):
    def test_absent_sentinel_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)

            self.assertFalse(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))

    def test_forced_setup_shutdown_clears_only_the_inherited_sentinel(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)
            sentinel = os.path.join(config_path, 'safe_mode')
            with open(sentinel, 'w', encoding='utf-8'):
                pass
            with open(os.path.join(logsdir, 'firsttime-setup.txt'), 'w', encoding='utf-8') as fw:
                fw.write('[shutdown-plugin] force exit.\n')

            self.assertTrue(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))
            self.assertFalse(os.path.exists(sentinel))

    def test_unexplained_sentinel_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)
            sentinel = os.path.join(config_path, 'safe_mode')
            with open(sentinel, 'w', encoding='utf-8'):
                pass

            with self.assertRaisesRegex(RuntimeError, 'without an onsdriver force-exit record'):
                clear_inherited_setup_shutdown_sentinels(config_path, logsdir)
            self.assertTrue(os.path.exists(sentinel))

    def test_force_stopped_setup_clears_obs_32_crash_sentinels(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            sentinel_dir = os.path.join(config_path, '.sentinel')
            os.makedirs(sentinel_dir)
            os.makedirs(logsdir)
            sentinels = [os.path.join(sentinel_dir, f'run_{index}') for index in range(2)]
            for sentinel in sentinels:
                with open(sentinel, 'w', encoding='utf-8'):
                    pass
            unrelated_marker = os.path.join(sentinel_dir, 'keep.txt')
            with open(unrelated_marker, 'w', encoding='utf-8'):
                pass
            with open(os.path.join(logsdir, 'firsttime-setup.txt'), 'w', encoding='utf-8') as fw:
                fw.write('[shutdown-plugin] force exit.\n')

            self.assertTrue(clear_inherited_setup_shutdown_sentinels(config_path, logsdir))
            self.assertFalse(any(os.path.exists(sentinel) for sentinel in sentinels))
            self.assertTrue(os.path.exists(unrelated_marker))

    def test_unexplained_obs_32_crash_sentinel_fails_closed(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            sentinel_dir = os.path.join(config_path, '.sentinel')
            os.makedirs(sentinel_dir)
            os.makedirs(logsdir)
            sentinel = os.path.join(sentinel_dir, 'run_inherited')
            with open(sentinel, 'w', encoding='utf-8'):
                pass

            with self.assertRaisesRegex(RuntimeError, 'without an onsdriver force-exit record'):
                clear_inherited_setup_shutdown_sentinels(config_path, logsdir)
            self.assertTrue(os.path.exists(sentinel))


class PluginLoadTest(obstest.OBSTest):
    'Test the plugin is loaded by OBS Studio'

    @staticmethod
    def _input_names(client):
        response = client.send('GetInputList', {})
        return {item['inputName'] for item in response.inputs}

    def _wait_for_input_presence(self, client, input_name, expected, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (input_name in self._input_names(client)) == expected:
                return
            time.sleep(0.1)
        expectation = 'appear' if expected else 'be removed'
        self.fail(f'Timed out waiting for OBS input {input_name!r} to {expectation}')

    def _screenshot(self, client, source_name):
        response = client.send('GetSourceScreenshot', {
            'sourceName': source_name,
            'imageFormat': 'png',
            'imageWidth': 16,
            'imageHeight': 16,
        })
        return response.image_data

    def _wait_for_screenshot(self, client, source_name, predicate, timeout=5.0):
        deadline = time.monotonic() + timeout
        last_image = None
        last_error = None
        while time.monotonic() < deadline:
            try:
                last_image = self._screenshot(client, source_name)
                last_error = None
                if predicate(last_image):
                    return last_image
            except Exception as error:  # OBS may need a frame after a scene or source change.
                last_error = error
            time.sleep(0.1)
        if last_error:
            self.fail(f'Unable to capture {source_name!r}: {last_error}')
        self.fail(f'Timed out waiting for the expected frame from {source_name!r}')

    def setUp(self):
        # Keep the OBSExec instance even if startup checking times out, so the
        # failed launch's log and stderr can be saved and its process cleaned up.
        super().setUp(run=False)
        self._startup_succeeded = False
        self.addCleanup(self._preserve_final_log)
        # onsdriver's first-time setup can force-stop OBS after its shutdown
        # timeout, leaving a version-specific sentinel that blocks the next
        # launch. Clear inherited markers only when its log confirms the force
        # exit.
        try:
            setup_was_force_stopped = clear_inherited_setup_shutdown_sentinels(
                self.obs.config.path, os.environ.get('ONSDRIVER_LOGS', 'logs')
            )
        except RuntimeError as error:
            self.fail(str(error))
        if setup_was_force_stopped:
            print('Cleared inherited OBS shutdown marker after onsdriver force-stopped setup')
        self.obs.run()
        self._startup_succeeded = True

    def _preserve_final_log(self):
        # OBSTest checks the leak summary in tearDown before moving the log.
        # Preserve the final OBS log even when that assertion fails so CI
        # artifacts contain the shutdown diagnostics.
        process = getattr(self.obs, 'proc_obs', None)
        if process and process.poll() is None:
            try:
                process.terminate()
            except OSError:
                pass
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                except OSError:
                    pass
                process.wait()

        logsdir = os.environ.get('ONSDRIVER_LOGS', 'logs')
        os.makedirs(logsdir, exist_ok=True)
        logfile = None
        try:
            logfile = self.obs.get_logfile()
        except OSError:
            pass

        if logfile and os.path.isfile(logfile):
            filename = f'diagnostic-{self.name.replace(os.sep, "_")}.log'
            destination = os.path.join(logsdir, filename)
            if os.path.abspath(logfile) != os.path.abspath(destination):
                shutil.copy2(logfile, destination)
        elif not self._startup_succeeded:
            filename = f'diagnostic-{self.name.replace(os.sep, "_")}-startup.txt'
            with open(os.path.join(logsdir, filename), 'w', encoding='utf-8') as fw:
                fw.write(f'OBS log path: {logfile!r}\n')

        stderr = getattr(self.obs, '_tmp_stderr', None)
        if stderr:
            try:
                stderr.seek(0)
                contents = stderr.read()
            except (OSError, ValueError):
                contents = b''
            if contents:
                filename = f'diagnostic-{self.name.replace(os.sep, "_")}-stderr.txt'
                with open(os.path.join(logsdir, filename), 'wb') as fw:
                    fw.write(contents)

    def tearDown(self):
        # onsdriver's shutdown() enables an unsafe force-exit after five
        # seconds. That can terminate OBS before its normal leak summary is
        # written, making plugin teardown impossible to distinguish from a
        # slow runner. Ask the same shutdown plugin to stop outputs and exit,
        # but let the CI step timeout handle a genuine hang.
        client = self.obs.get_obsws()
        response = client.send('CallVendorRequest', {
            'vendorName': 'shutdown-plugin',
            'requestType': 'shutdown',
            'requestData': {
                'reason': f'requested through onsdriver by {self.id()}',
                'support_url': 'https://github.com/noris-plugins-for-obs/onsdriver/issues',
                'force': True,
                'exit_timeout': 0.0,
            }
        })
        if response.response_data != {}:
            raise ValueError(f'shutdown request returned {response.response_data}')
        del client
        self.obs.wait()
        self.assertEqual(self.memory_leak(), 0)
        self.move_log(prefix=self.name + '-')

    def test_log(self):
        'Check log file'
        search_text = f' [{PLUGIN_NAME}] '
        loaded = False
        with open(self.obs.get_logfile(), 'r', encoding='utf-8') as fr:
            for line in fr:
                if search_text in line:
                    print(line)
                    loaded = True
                    break
        self.assertTrue(loaded)

    def test_safe_adapter_requires_explicit_rebinding_after_source_replacement(self):
        'Create a safe adapter and verify a same-name replacement stays unbound'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame lifecycle {suffix}'
        camera_name = f'FaithfulFrame synthetic camera {suffix}'
        adapter_name = f'FaithfulFrame safe adapter {suffix}'
        original_scene = None
        scene_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            input_kinds = kinds_response.input_kinds
            color_kinds = [
                kind for kind in input_kinds if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free lifecycle testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': scene_name})
            scene_created = True
            client.send('SetCurrentProgramScene', {'sceneName': scene_name})
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFFFF0000, 'width': 320, 'height': 180},
            })
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {'target_name': camera_name, 'safe_tracking_enabled': True},
            })

            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertEqual(adapter_settings.input_kind, 'face_tracker_source')
            self.assertEqual(adapter_settings.input_settings.get('target_name'), camera_name)
            self.assertTrue(adapter_settings.input_settings.get('safe_tracking_enabled'))

            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            bound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )

            client.send('RemoveInput', {'inputName': camera_name})
            self._wait_for_input_presence(client, camera_name, False)
            unavailable_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != bound_image
            )

            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF0000FF, 'width': 320, 'height': 180},
            })
            replacement_image = self._wait_for_screenshot(
                client, camera_name, lambda image: image != camera_image
            )
            self.assertNotEqual(replacement_image, camera_image)
            for _ in range(3):
                self.assertEqual(self._screenshot(client, adapter_name), unavailable_image)
                time.sleep(0.1)

            client.send('RemoveInput', {'inputName': adapter_name})
            self.assertNotIn(adapter_name, self._input_names(client))
            self.assertIn(camera_name, self._input_names(client), 'Removing the adapter must leave its camera intact')
        finally:
            # Keep this test isolated from the next OBS test even when an assertion fails.
            names = self._input_names(client)
            if adapter_name in names:
                client.send('RemoveInput', {'inputName': adapter_name})
            names = self._input_names(client)
            if camera_name in names:
                client.send('RemoveInput', {'inputName': camera_name})
            if original_scene:
                client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            if scene_created:
                client.send('RemoveScene', {'sceneName': scene_name})


if __name__ == '__main__':
    unittest.main()
