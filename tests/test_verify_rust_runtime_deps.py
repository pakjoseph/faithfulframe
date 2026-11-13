import importlib.util
from pathlib import Path
import unittest


SCRIPT = Path(__file__).resolve().parents[1] / "ci" / "verify-rust-runtime-deps.py"
SPEC = importlib.util.spec_from_file_location("verify_rust_runtime_deps", SCRIPT)
assert SPEC and SPEC.loader
verifier = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(verifier)


def package(name, source=None):
    return {"id": f"{name}-id", "name": name, "source": source}


def metadata(packages, reachable_ids):
    return {
        "packages": packages,
        "resolve": {"nodes": [{"id": package_id} for package_id in reachable_ids]},
    }


class RustRuntimeDependencyTests(unittest.TestCase):
    def test_accepts_only_the_local_ffi_and_core_packages(self):
        ffi = package("faithfulframe-ffi")
        core = package("faithfulframe-core")

        self.assertEqual(
            verifier.dependency_errors(metadata([ffi, core], [ffi["id"], core["id"]])),
            [],
        )

    def test_rejects_registry_runtime_dependencies(self):
        ffi = package("faithfulframe-ffi")
        core = package("faithfulframe-core")
        serde = package("serde", "registry+https://github.com/rust-lang/crates.io-index")

        errors = verifier.dependency_errors(
            metadata([ffi, core, serde], [ffi["id"], core["id"], serde["id"]])
        )

        self.assertTrue(any("unaudited Rust production dependency: serde" in error for error in errors))

    def test_test_only_crates_outside_the_resolved_graph_do_not_block_shipping(self):
        ffi = package("faithfulframe-ffi")
        core = package("faithfulframe-core")
        serde = package("serde", "registry+https://github.com/rust-lang/crates.io-index")

        self.assertEqual(
            verifier.dependency_errors(metadata([ffi, core, serde], [ffi["id"], core["id"]])),
            [],
        )

    def test_rejects_an_unexpected_local_runtime_crate(self):
        ffi = package("faithfulframe-ffi")
        core = package("faithfulframe-core")
        extra = package("new-local-runtime-crate")

        errors = verifier.dependency_errors(
            metadata([ffi, core, extra], [ffi["id"], core["id"], extra["id"]])
        )

        self.assertTrue(any("unaudited Rust production dependency" in error for error in errors))

    def test_rejects_missing_resolution(self):
        self.assertTrue(verifier.dependency_errors({"packages": [], "resolve": None}))


if __name__ == "__main__":
    unittest.main()
