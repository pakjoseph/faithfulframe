'''
Test plugin is loaded
'''

import os
import shutil
import unittest
from onsdriver import obstest

PLUGIN_NAME = 'face-tracker'


class PluginLoadTest(obstest.OBSTest):
    'Test the plugin is loaded by OBS Studio'

    def setUp(self):
        super().setUp()
        self.addCleanup(self._preserve_final_log)

    def _preserve_final_log(self):
        # OBSTest checks the leak summary in tearDown before moving the log.
        # Preserve the final OBS log even when that assertion fails so CI
        # artifacts contain the shutdown diagnostics.
        logfile = self.obs.get_logfile()
        if not logfile or not os.path.isfile(logfile):
            return

        logsdir = os.environ.get('ONSDRIVER_LOGS', 'logs')
        os.makedirs(logsdir, exist_ok=True)
        filename = f'diagnostic-{self.name.replace(os.sep, "_")}.log'
        destination = os.path.join(logsdir, filename)
        if os.path.abspath(logfile) != os.path.abspath(destination):
            shutil.copy2(logfile, destination)

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
