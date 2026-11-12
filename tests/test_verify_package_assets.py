import importlib.util
from pathlib import Path
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "verify-package-assets.py"
SPEC = importlib.util.spec_from_file_location("verify_package_assets", SCRIPT)
assert SPEC and SPEC.loader
verify_package_assets = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verify_package_assets)


class PackageAssetVerificationTests(unittest.TestCase):
    def setUp(self):
        self.temp_dir = tempfile.TemporaryDirectory()
        self.root = Path(self.temp_dir.name)
        for name in verify_package_assets.REQUIRED_FILES:
            content = verify_package_assets.REQUIRED_LICENSE_MARKERS.get(name, b"fixture")
            (self.root / name).write_bytes(content)

    def tearDown(self):
        self.temp_dir.cleanup()

    def test_accepts_required_notices_and_default_models(self):
        self.assertEqual(verify_package_assets.package_errors(self.root), [])

    def test_rejects_missing_required_notice_or_model(self):
        (self.root / "LICENSE-dlib-models").unlink()
        self.assertIn(
            "missing required package file: LICENSE-dlib-models",
            verify_package_assets.package_errors(self.root),
        )

    def test_rejects_restricted_landmark_asset(self):
        restricted = self.root / "nested" / "shape_predictor_68_face_landmarks.dat"
        restricted.parent.mkdir()
        restricted.write_bytes(b"restricted fixture")
        self.assertIn(
            "restricted model asset is packaged: nested/shape_predictor_68_face_landmarks.dat",
            verify_package_assets.package_errors(self.root),
        )

    def test_rejects_empty_required_files(self):
        (self.root / "LICENSE").write_bytes(b"")
        self.assertIn(
            "required package file is empty: LICENSE",
            verify_package_assets.package_errors(self.root),
        )

    def test_rejects_wrong_license_notice_under_expected_name(self):
        (self.root / "LICENSE-dlib-models").write_bytes(b"Boost Software License")
        self.assertIn(
            "license notice has unexpected contents: LICENSE-dlib-models",
            verify_package_assets.package_errors(self.root),
        )

    def test_rejects_wrong_gpl_version_under_expected_name(self):
        (self.root / "LICENSE").write_bytes(b"GNU GENERAL PUBLIC LICENSE Version 3")
        self.assertIn(
            "license notice has unexpected contents: LICENSE",
            verify_package_assets.package_errors(self.root),
        )


if __name__ == "__main__":
    unittest.main()
