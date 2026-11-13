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
        restricted_directory = self.root / "nested"
        restricted_directory.mkdir()
        for name in sorted(verify_package_assets.RESTRICTED_FILES):
            with self.subTest(name=name):
                restricted = restricted_directory / name
                restricted.write_bytes(b"restricted fixture")
                self.assertIn(
                    f"restricted model asset is packaged: nested/{name}",
                    verify_package_assets.package_errors(self.root),
                )
                restricted.unlink()

    def test_rejects_unreviewed_model_assets(self):
        model_directory = self.root / "nested" / "dlib_face_recognition_model"
        model_directory.mkdir(parents=True)
        for name in (
            "unreviewed.dat",
            "unreviewed.dat.bz2",
            "unreviewed.onnx",
            "unreviewed.pt",
        ):
            with self.subTest(name=name):
                model = model_directory / name
                model.write_bytes(b"unreviewed fixture")
                self.assertIn(
                    f"unreviewed model asset is packaged: nested/dlib_face_recognition_model/{name}",
                    verify_package_assets.package_errors(self.root),
                )
                model.unlink()

    def test_dlib_face_recognition_weights_are_not_shipped_without_review(self):
        model_directory = self.root / "nested" / "dlib_face_recognition_model"
        model_directory.mkdir(parents=True)
        model = model_directory / "dlib_face_recognition_resnet_model_v1.dat"
        model.write_bytes(b"unreviewed recognition weights")
        self.assertIn(
            "unreviewed model asset is packaged: "
            "nested/dlib_face_recognition_model/dlib_face_recognition_resnet_model_v1.dat",
            verify_package_assets.package_errors(self.root),
        )

    def test_allows_reviewed_model_asset_names(self):
        model_directory = self.root / "nested" / "dlib_face_landmark_model"
        model_directory.mkdir(parents=True)
        model = model_directory / "shape_predictor_5_face_landmarks.dat"
        model.write_bytes(b"reviewed fixture")
        self.assertFalse(
            any("model asset is packaged" in error for error in verify_package_assets.package_errors(self.root))
        )

    def test_requires_each_model_asset_declared_license_notice(self):
        (self.root / "LICENSE-dlib-models").unlink()
        self.assertIn(
            "model asset from davisking/dlib-models fd81b6308a6a73d4ce08859eb2f4b628a21e27a2 "
            "is missing its declared license notice LICENSE-dlib-models",
            "\n".join(verify_package_assets.package_errors(self.root)),
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
