import contextlib
import importlib.util
import io
import pathlib
import plistlib
import tempfile
import unittest
from types import SimpleNamespace
from unittest.mock import patch


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


class MachOMinimumVersionTests(unittest.TestCase):
    def make_bundle(self, minimum_version):
        temporary_directory = tempfile.TemporaryDirectory()
        self.addCleanup(temporary_directory.cleanup)
        info_path = pathlib.Path(temporary_directory.name) / "Contents" / "Info.plist"
        info_path.parent.mkdir(parents=True)
        with info_path.open("wb") as info_file:
            plistlib.dump({"LSMinimumSystemVersion": minimum_version}, info_file)
        return pathlib.Path(temporary_directory.name)

    def test_reads_each_modern_build_version_slice(self):
        load_commands = """\
Load command 0
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.0
      sdk 26.0
   ntools 1
     tool LD
  version 1.0
Load command 1
      cmd LC_BUILD_VERSION
  cmdsize 32
 platform 1
    minos 13.2
      sdk 26.0
   ntools 1
     tool LD
  version 1.0
"""
        with patch.object(
            VERIFY.subprocess,
            "run",
            side_effect=[
                SimpleNamespace(stdout="Mach-O universal binary with 2 architectures"),
                SimpleNamespace(stdout=load_commands),
            ],
        ) as run:
            versions = VERIFY.macho_minimum_versions(pathlib.Path("plugin.dylib"))

        self.assertEqual(versions, [("LC_BUILD_VERSION", "13.0"), ("LC_BUILD_VERSION", "13.2")])
        self.assertEqual(run.call_args_list[1].args[0][:3], ["otool", "-arch", "all"])

    def test_reads_legacy_minimum_version_command(self):
        load_commands = """\
Load command 0
      cmd LC_VERSION_MIN_MACOSX
  cmdsize 16
  version 10.13
      sdk 14.0
"""
        with patch.object(
            VERIFY.subprocess,
            "run",
            side_effect=[
                SimpleNamespace(stdout="Mach-O 64-bit dynamically linked shared library x86_64"),
                SimpleNamespace(stdout=load_commands),
            ],
        ):
            versions = VERIFY.macho_minimum_versions(pathlib.Path("legacy.dylib"))

        self.assertEqual(versions, [("LC_VERSION_MIN_MACOSX", "10.13")])

    def test_macho_without_recognized_minimum_version_is_not_accepted(self):
        load_commands = """\
Load command 0
      cmd LC_LOAD_DYLIB
  cmdsize 56
"""
        with patch.object(
            VERIFY.subprocess,
            "run",
            side_effect=[
                SimpleNamespace(stdout="Mach-O 64-bit dynamically linked shared library arm64"),
                SimpleNamespace(stdout=load_commands),
            ],
        ):
            versions = VERIFY.macho_minimum_versions(pathlib.Path("unknown.dylib"))

        self.assertEqual(versions, [])

    def test_non_macho_file_skips_otool(self):
        with patch.object(
            VERIFY.subprocess,
            "run",
            return_value=SimpleNamespace(stdout="ASCII text"),
        ) as run:
            versions = VERIFY.macho_minimum_versions(pathlib.Path("readme.txt"))

        self.assertIsNone(versions)
        run.assert_called_once()

    def run_package_check(self, minimum_versions):
        bundle = self.make_bundle("13.0")
        binary = bundle / "Contents" / "MacOS" / "FaithfulFrame"
        binary.parent.mkdir(parents=True, exist_ok=True)
        binary.write_bytes(b"Mach-O fixture")
        cache = bundle.parent / "CMakeCache.txt"
        cache.write_text("CMAKE_OSX_DEPLOYMENT_TARGET:STRING=13.0\n", encoding="utf-8")
        stderr = io.StringIO()
        stdout = io.StringIO()
        with (
            patch.object(
                VERIFY.sys,
                "argv",
                [str(SCRIPT_PATH), "--root", str(bundle), "--cmake-cache", str(cache)],
            ),
            patch.object(
                VERIFY,
                "macho_minimum_versions",
                side_effect=lambda path: minimum_versions if path == binary else None,
            ),
            contextlib.redirect_stdout(stdout),
            contextlib.redirect_stderr(stderr),
        ):
            result = VERIFY.main()
        return result, stdout.getvalue(), stderr.getvalue(), binary

    def test_package_gate_accepts_matching_binary_minimum(self):
        result, output, error, _ = self.run_package_check([("LC_BUILD_VERSION", "13.0")])

        self.assertEqual(result, 0)
        self.assertIn("Verified 1 Mach-O files", output)
        self.assertEqual(error, "")

    def test_package_gate_rejects_binary_above_configured_minimum(self):
        result, _, error, binary = self.run_package_check([("LC_BUILD_VERSION", "13.1")])

        self.assertEqual(result, 1)
        self.assertIn(f"{binary}: LC_BUILD_VERSION requires macOS 13.1 (target 13.0)", error)

    def test_package_gate_rejects_macho_without_minimum_metadata(self):
        result, _, error, binary = self.run_package_check([])

        self.assertEqual(result, 1)
        self.assertIn(f"{binary}: no macOS minimum-version load command found", error)


if __name__ == "__main__":
    unittest.main()
