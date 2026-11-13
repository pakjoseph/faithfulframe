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

    @staticmethod
    def _input_uuid(client, input_name):
        response = client.send('GetInputList', {})
        for item in response.inputs:
            if item['inputName'] == input_name:
                source_uuid = item.get('inputUuid')
                if source_uuid:
                    return source_uuid
                break
        raise AssertionError(f'OBS did not report a UUID for input {input_name!r}')

    def _wait_for_input_presence(self, client, input_name, expected, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (input_name in self._input_names(client)) == expected:
                return
            time.sleep(0.1)
        expectation = 'appear' if expected else 'be removed'
        self.fail(f'Timed out waiting for OBS input {input_name!r} to {expectation}')

    def _wait_for_log_text(self, text, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            logfile = self.obs.get_logfile()
            try:
                with open(logfile, 'r', encoding='utf-8', errors='replace') as fr:
                    if text in fr.read():
                        return
            except OSError:
                pass
            time.sleep(0.1)
        self.fail(f'Timed out waiting for OBS log message {text!r}')

    def _screenshot(self, client, source_name):
        response = client.send('GetSourceScreenshot', {
            'sourceName': source_name,
            'imageFormat': 'png',
            'imageWidth': 16,
            'imageHeight': 16,
        })
        image_data = response.image_data
        if not image_data:
            raise AssertionError(f'OBS returned an empty screenshot for source {source_name!r}')
        return image_data

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

    def test_safe_adapter_scene_switch_holds_source_isolation(self):
        'Switch away from and back to a bound camera without changing either source'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        tracked_scene = f'FaithfulFrame scene switch {suffix}'
        fallback_scene = f'FaithfulFrame fallback {suffix}'
        camera_name = f'FaithfulFrame tracked camera {suffix}'
        fallback_name = f'FaithfulFrame fallback camera {suffix}'
        adapter_name = f'FaithfulFrame scene adapter {suffix}'
        original_scene = None
        created_scenes = []
        created_inputs = []

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
            ]
            self.assertTrue(color_kinds, 'OBS color source is required for hardware-free lifecycle testing')
            color_kind = max(
                color_kinds,
                key=lambda kind: int(kind.rsplit('_v', 1)[1]) if '_v' in kind else 0,
            )

            client.send('CreateScene', {'sceneName': tracked_scene})
            created_scenes.append(tracked_scene)
            client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFFFF0000, 'width': 320, 'height': 180},
            })
            created_inputs.append(camera_name)
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': tracked_scene,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            created_inputs.append(adapter_name)

            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            tracked_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: bool(image) and image == camera_image
            )
            self.assertEqual(tracked_image, camera_image)

            client.send('CreateScene', {'sceneName': fallback_scene})
            created_scenes.append(fallback_scene)
            client.send('CreateInput', {
                'sceneName': fallback_scene,
                'inputName': fallback_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF00FF00, 'width': 320, 'height': 180},
            })
            created_inputs.append(fallback_name)
            client.send('SetCurrentProgramScene', {'sceneName': fallback_scene})
            fallback_image = self._wait_for_screenshot(client, fallback_name, bool)

            for cycle in range(3):
                inactive_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: bool(image) and image != camera_image
                )
                self.assertNotEqual(
                    inactive_image,
                    camera_image,
                    'An inactive scene must not keep rendering the bound camera through its adapter',
                )
                self.assertEqual(
                    self._screenshot(client, fallback_name),
                    fallback_image,
                    'Scene switching must leave the fallback camera unchanged',
                )

                client.send('SetCurrentProgramScene', {'sceneName': tracked_scene})
                restored_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: bool(image) and image == camera_image
                )
                self.assertEqual(
                    restored_image,
                    camera_image,
                    'Returning to the bound scene must restore only its original camera view',
                )
                self.assertEqual(self._screenshot(client, camera_name), camera_image)
                if cycle < 2:
                    client.send('SetCurrentProgramScene', {'sceneName': fallback_scene})

            self.assertEqual(self._screenshot(client, fallback_name), fallback_image)
        finally:
            try:
                if original_scene:
                    client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            except Exception:
                pass

            try:
                names = self._input_names(client)
                for input_name in created_inputs:
                    if input_name in names:
                        client.send('RemoveInput', {'inputName': input_name})
            except Exception:
                pass

            for scene_name in reversed(created_scenes):
                try:
                    client.send('RemoveScene', {'sceneName': scene_name})
                except Exception:
                    pass

    def test_safe_adapter_requires_explicit_rebinding_after_source_replacement(self):
        'Create a safe adapter and verify a same-name replacement stays unbound'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame lifecycle {suffix}'
        camera_name = f'FaithfulFrame synthetic camera {suffix}'
        other_camera_name = f'FaithfulFrame unrelated camera {suffix}'
        adapter_name = f'FaithfulFrame safe adapter {suffix}'
        other_adapter_name = f'FaithfulFrame competing adapter {suffix}'
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
                'inputName': other_camera_name,
                'inputKind': color_kind,
                'inputSettings': {'color': 0xFF00FF00, 'width': 320, 'height': 180},
            })
            camera_uuid = self._input_uuid(client, camera_name)
            other_camera_uuid = self._input_uuid(client, other_camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {'target_name': camera_name, 'target_uuid': camera_uuid},
            })
            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertEqual(adapter_settings.input_kind, 'face_tracker_source')
            self.assertEqual(adapter_settings.input_settings.get('target_name'), camera_name)
            self.assertEqual(adapter_settings.input_settings.get('target_uuid'), camera_uuid)
            self.assertFalse(adapter_settings.input_settings.get('safe_tracking_enabled', False))

            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            other_camera_image = self._wait_for_screenshot(client, other_camera_name, bool)
            legacy_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )
            self.assertEqual(legacy_image, camera_image)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            adapter_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertTrue(adapter_settings.input_settings.get('safe_tracking_enabled'))
            self.assertEqual(adapter_settings.input_settings.get('target_uuid'), camera_uuid)
            bound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )

            # Establish the first adapter's lease before attempting a competing
            # one, so this assertion does not depend on OBS tick ordering.
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': other_adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': other_camera_name,
                    'target_uuid': other_camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            self._wait_for_log_text(
                'FaithfulFrame allows safe tracking on one source adapter at a time'
            )
            self._wait_for_screenshot(
                client, other_adapter_name, lambda image: image != other_camera_image
            )
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, other_adapter_name),
                    other_camera_image,
                    'A competing safe adapter must not acquire the other camera',
                )
                retained_image = self._wait_for_screenshot(
                    client, adapter_name, lambda image: image == bound_image
                )
                self.assertEqual(retained_image, bound_image)
                time.sleep(0.1)

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
            replacement_uuid = self._input_uuid(client, camera_name)
            self.assertNotEqual(replacement_uuid, camera_uuid)
            self.assertNotEqual(replacement_image, camera_image)
            for _ in range(3):
                self.assertEqual(self._screenshot(client, adapter_name), unavailable_image)
                self.assertEqual(self._screenshot(client, other_camera_name), other_camera_image)
                time.sleep(0.1)

            client.send('RemoveInput', {'inputName': other_adapter_name})
            self._wait_for_input_presence(client, other_adapter_name, False)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'target_name': other_camera_name, 'target_uuid': other_camera_uuid},
                'overlay': True,
            })
            other_rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == other_camera_image
            )
            self.assertEqual(other_rebound_image, other_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'target_name': camera_name, 'target_uuid': replacement_uuid},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == replacement_image
            )
            self.assertEqual(rebound_image, replacement_image)
            self.assertEqual(
                self._screenshot(client, camera_name),
                replacement_image,
                'Safe tracking must leave the original camera feed unchanged',
            )
            rebound_settings = client.send('GetInputSettings', {'inputName': adapter_name})
            self.assertTrue(rebound_settings.input_settings.get('safe_tracking_enabled'))
            self.assertEqual(rebound_settings.input_settings.get('target_name'), camera_name)

            client.send('RemoveInput', {'inputName': adapter_name})
            self._wait_for_input_presence(client, adapter_name, False)
            self.assertIn(
                camera_name,
                self._input_names(client),
                'Removing the adapter must leave its camera intact',
            )
            self.assertEqual(
                self._screenshot(client, camera_name),
                replacement_image,
                'Removing the adapter must preserve the original camera feed',
            )
            self.assertEqual(self._screenshot(client, other_camera_name), other_camera_image)
        finally:
            # Keep this test isolated from the next OBS test even when an assertion fails.
            names = self._input_names(client)
            for input_name in (adapter_name, other_adapter_name, camera_name, other_camera_name):
                if input_name in names:
                    client.send('RemoveInput', {'inputName': input_name})
                    names.remove(input_name)
            if original_scene:
                client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            if scene_created:
                client.send('RemoveScene', {'sceneName': scene_name})

    def test_safe_adapter_stops_when_legacy_tracker_filter_is_added(self):
        'Fail closed if a legacy crop filter is added to the bound camera'
        client = self.obs.get_obsws()
        suffix = uuid.uuid4().hex
        scene_name = f'FaithfulFrame filter guard {suffix}'
        camera_name = f'FaithfulFrame filter guard camera {suffix}'
        adapter_name = f'FaithfulFrame filter guard adapter {suffix}'
        legacy_filter_name = f'FaithfulFrame legacy tracker {suffix}'
        original_scene = None
        scene_created = False
        filter_created = False

        try:
            scene_response = client.send('GetCurrentProgramScene', {})
            original_scene = getattr(scene_response, 'scene_name', None) or getattr(
                scene_response, 'current_program_scene_name', None
            )
            self.assertTrue(original_scene, 'OBS must have an active program scene')

            kinds_response = client.send('GetInputKindList', {'unversioned': False})
            color_kinds = [
                kind
                for kind in kinds_response.input_kinds
                if kind == 'color_source' or kind.startswith('color_source_v')
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
                'inputSettings': {'color': 0xFF4A76C4, 'width': 320, 'height': 180},
            })
            camera_uuid = self._input_uuid(client, camera_name)
            client.send('CreateInput', {
                'sceneName': scene_name,
                'inputName': adapter_name,
                'inputKind': 'face_tracker_source',
                'inputSettings': {
                    'target_name': camera_name,
                    'target_uuid': camera_uuid,
                    'safe_tracking_enabled': True,
                },
            })
            camera_image = self._wait_for_screenshot(client, camera_name, bool)
            bound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == camera_image
            )
            self.assertEqual(bound_image, camera_image)

            client.send('CreateSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterKind': 'face_tracker_filter',
                'filterSettings': {},
            })
            filter_created = True
            camera_with_legacy_filter = self._wait_for_screenshot(client, camera_name, bool)
            self._wait_for_log_text(
                'FaithfulFrame safe tracking stopped: remove the added legacy tracker filter, then rebind the source'
            )
            closed_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != bound_image
            )
            self.assertNotEqual(
                closed_image,
                bound_image,
                'Adding a legacy crop filter must stop the protected output instead of applying its crop',
            )
            self.assertNotEqual(
                closed_image,
                camera_with_legacy_filter,
                'The stopped adapter must not pass through the newly filtered camera image',
            )

            client.send('RemoveSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            filter_created = False
            restored_camera_image = self._wait_for_screenshot(
                client, camera_name, lambda image: image == camera_image
            )
            self.assertEqual(restored_camera_image, camera_image)
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, adapter_name),
                    bound_image,
                    'Removing the filter must not silently reopen the closed binding',
                )
                time.sleep(0.1)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(rebound_image, restored_camera_image)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            # The adapter switches back to legacy rendering as soon as the
            # setting is applied; let the OBS video tick finish retiring the
            # old safe binding before testing a pre-existing filter.
            time.sleep(0.25)

            # Existing disabled filters remain incompatible: enabling safety
            # must not bind until the filter is removed and the operator retries.
            client.send('CreateSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterKind': 'face_tracker_filter',
                'filterSettings': {},
            })
            filter_created = True
            client.send('SetSourceFilterEnabled', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
                'filterEnabled': False,
            })
            filter_info = client.send('GetSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            self.assertFalse(filter_info.filter_enabled)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            self._wait_for_log_text('FaithfulFrame rejected an invalid, filtered, or unavailable source binding')
            rejected_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image != restored_camera_image
            )
            self.assertNotEqual(
                rejected_image,
                restored_camera_image,
                'A pre-existing disabled tracker filter must reject the safe source binding',
            )

            client.send('RemoveSourceFilter', {
                'sourceName': camera_name,
                'filterName': legacy_filter_name,
            })
            filter_created = False
            self.assertEqual(self._screenshot(client, camera_name), restored_camera_image)
            for _ in range(3):
                self.assertNotEqual(
                    self._screenshot(client, adapter_name),
                    restored_camera_image,
                    'Removing an existing filter must not silently create a safe binding',
                )
                time.sleep(0.1)

            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': False},
                'overlay': True,
            })
            disabled_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(disabled_image, restored_camera_image)
            client.send('SetInputSettings', {
                'inputName': adapter_name,
                'inputSettings': {'safe_tracking_enabled': True},
                'overlay': True,
            })
            rebound_image = self._wait_for_screenshot(
                client, adapter_name, lambda image: image == restored_camera_image
            )
            self.assertEqual(rebound_image, restored_camera_image)
        finally:
            names = self._input_names(client)
            if filter_created and camera_name in names:
                try:
                    client.send('RemoveSourceFilter', {
                        'sourceName': camera_name,
                        'filterName': legacy_filter_name,
                    })
                except Exception:
                    pass
            for input_name in (adapter_name, camera_name):
                if input_name in names:
                    client.send('RemoveInput', {'inputName': input_name})
                    names.remove(input_name)
            if original_scene:
                client.send('SetCurrentProgramScene', {'sceneName': original_scene})
            if scene_created:
                client.send('RemoveScene', {'sceneName': scene_name})


if __name__ == '__main__':
    unittest.main()
