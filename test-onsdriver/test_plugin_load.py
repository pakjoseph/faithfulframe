'''
Test plugin is loaded
'''

import glob
import os
import shutil
import subprocess
import tempfile
import unittest
from onsdriver import obstest

PLUGIN_NAME = 'face-tracker'


def clear_inherited_setup_safe_mode_sentinel(config_path, logsdir):
    safe_mode_sentinel = os.path.join(config_path, 'safe_mode')
    if not os.path.exists(safe_mode_sentinel):
        return False

    firsttime_logs = glob.glob(os.path.join(logsdir, 'firsttime-*.txt'))
    for logfile in firsttime_logs:
        try:
            with open(logfile, 'r', encoding='utf-8') as fr:
                was_force_stopped = '[shutdown-plugin] force exit.' in fr.read()
        except OSError:
            continue
        if was_force_stopped:
            os.remove(safe_mode_sentinel)
            return True
    raise RuntimeError(
        'OBS setup profile has an unclean-shutdown sentinel '
        'without an onsdriver force-exit record'
    )


class SetupSafeModeSentinelTest(unittest.TestCase):
    def test_absent_sentinel_is_unchanged(self):
        with tempfile.TemporaryDirectory() as tmp:
            config_path = os.path.join(tmp, 'config')
            logsdir = os.path.join(tmp, 'logs')
            os.makedirs(config_path)
            os.makedirs(logsdir)

            self.assertFalse(clear_inherited_setup_safe_mode_sentinel(config_path, logsdir))

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

            self.assertTrue(clear_inherited_setup_safe_mode_sentinel(config_path, logsdir))
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
                clear_inherited_setup_safe_mode_sentinel(config_path, logsdir)
            self.assertTrue(os.path.exists(sentinel))


class PluginLoadTest(obstest.OBSTest):
    'Test the plugin is loaded by OBS Studio'

    def setUp(self):
        # Keep the OBSExec instance even if startup checking times out, so the
        # failed launch's log and stderr can be saved and its process cleaned up.
        super().setUp(run=False)
        self._startup_succeeded = False
        self.addCleanup(self._preserve_final_log)
        # onsdriver's first-time setup can force-stop OBS after its shutdown
        # timeout and then save that profile. OBS persists a safe_mode sentinel
        # for this condition and blocks the next launch on a recovery dialog,
        # before its plugins or websocket are available. OBSTest has copied the
        # saved profile into an isolated per-test config, so remove only this
        # inherited sentinel before exercising the plugin's fresh startup.
        try:
            inherited_sentinel_removed = clear_inherited_setup_safe_mode_sentinel(
                self.obs.config.path, os.environ.get('ONSDRIVER_LOGS', 'logs')
            )
        except RuntimeError as error:
            self.fail(str(error))
        if inherited_sentinel_removed:
            print('Removing inherited OBS safe_mode sentinel from onsdriver setup')
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


if __name__ == '__main__':
    unittest.main()
