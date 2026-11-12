import importlib.util
import pathlib
import plistlib
import tempfile
import unittest


SCRIPT_PATH = pathlib.Path(__file__).parents[1] / "ci" / "macos" / "verify-deployment-target.py"
SPEC = importlib.util.spec_from_file_location("verify_macos_deployment_target", SCRIPT_PATH)
VERIFY = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VERIFY)


class BundleDeploymentTargetTests(unittest.TestCase):
    def make_bundle(self, minimum_version):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        info_path = pathlib.Path(temporary_directory.name) / "Contents" / "Info.plist"
        info_path.parent.mkdir(parents=True)
        with info_path.open("wb") as info_file:
            info = {} if minimum_version is None else {"LSMinimumSystemVersion": minimum_version}
            plistlib.dump(info, info_file)
        return pathlib.Path(temporary_directory.name)

    def test_bundle_target_matches_cmake_target(self):
        bundle = self.make_bundle("13.0")
        VERIFY.validate_bundle_minimum_version(bundle, "13.0")

    def test_equivalent_version_spelling_matches(self):
        bundle = self.make_bundle("13")
        VERIFY.validate_bundle_minimum_version(bundle, "13.0")

    def test_legacy_plist_target_is_rejected(self):
        bundle = self.make_bundle("10.13")
        with self.assertRaisesRegex(ValueError, "LSMinimumSystemVersion is 10.13"):
            VERIFY.validate_bundle_minimum_version(bundle, "13.0")

    def test_missing_plist_target_is_rejected(self):
        bundle = self.make_bundle(None)
        with self.assertRaisesRegex(ValueError, "missing or invalid"):
            VERIFY.validate_bundle_minimum_version(bundle, "13.0")


if __name__ == "__main__":
    unittest.main()
