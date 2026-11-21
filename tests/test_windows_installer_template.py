"""Guard the branded Windows installer name from OBS path compatibility."""

from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]


class WindowsInstallerTemplateTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        cls.template = (ROOT / "installer/installer-Windows.iss.in").read_text(
            encoding="utf-8"
        )

    def test_installer_uses_product_display_name(self):
        self.assertRegex(
            self.cmake,
            r'(?m)^set\(PLUGIN_DISPLAY_NAME "FaithfulFrame"\)$',
        )
        self.assertIn('#define MyAppDisplayName "@PLUGIN_DISPLAY_NAME@"', self.template)
        self.assertIn("AppName={#MyAppDisplayName}", self.template)
        self.assertIn("DefaultGroupName={#MyAppDisplayName}", self.template)

    def test_obs_paths_and_legacy_migration_keep_internal_plugin_name(self):
        self.assertRegex(
            self.cmake,
            r"(?m)^project\(face-tracker VERSION [^)]+\)$",
        )
        self.assertIn('#define MyAppName "@PROJECT_NAME@"', self.template)
        self.assertIn(
            r"DefaultDirName={commonappdata}\obs-studio\plugins\{#MyAppName}",
            self.template,
        )
        self.assertIn(
            r'Source: "..\release\{#MyAppName}\*"',
            self.template,
        )
        self.assertIn("OutputBaseFilename={#MyAppName}-", self.template)
        self.assertIn("GetLegacyDLLPath|{#MyAppName}.dll", self.template)
        self.assertIn("GetLegacyDLLPath|obs-{#MyAppName}.dll", self.template)
        self.assertNotIn(
            r"DefaultDirName={commonappdata}\obs-studio\plugins\{#MyAppDisplayName}",
            self.template,
        )


if __name__ == "__main__":
    unittest.main()
