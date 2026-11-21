from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]
SESSION_FFI_CALL = re.compile(r"\bff_session_[A-Za-z0-9_]+\s*\(")
SERIALIZED_BRIDGE = "src/faithfulframe-core-bridge.cpp"
CPP_SUFFIXES = {".c", ".cc", ".cpp", ".cxx", ".h", ".hh", ".hpp", ".hxx"}


def direct_session_call_sites(sources):
    call_sites = []
    for path, source in sources.items():
        if path == SERIALIZED_BRIDGE:
            continue
        for match in SESSION_FFI_CALL.finditer(source):
            line_number = source.count("\n", 0, match.start()) + 1
            call_sites.append(f"{path}:{line_number}")
    return call_sites


def production_sources():
    sources = {}
    for directory in ("src", "ui"):
        for path in (ROOT / directory).rglob("*"):
            if path.is_file() and path.suffix in CPP_SUFFIXES:
                sources[path.relative_to(ROOT).as_posix()] = path.read_text(encoding="utf-8")
    return sources


class SerializedSessionOwnershipTests(unittest.TestCase):
    def test_production_session_calls_are_confined_to_the_serialized_bridge(self):
        self.assertEqual(direct_session_call_sites(production_sources()), [])
        bridge = (ROOT / SERIALIZED_BRIDGE).read_text(encoding="utf-8")
        self.assertRegex(bridge, SESSION_FFI_CALL)

    def test_direct_session_call_outside_the_bridge_is_detected(self):
        sources = {
            SERIALIZED_BRIDGE: "ff_session_tick(session, at, &snapshot);",
            "src/face-tracker.cpp": "ff_session_apply_command\n(session, at, &command, &snapshot);",
        }

        self.assertEqual(direct_session_call_sites(sources), ["src/face-tracker.cpp:1"])


if __name__ == "__main__":
    unittest.main()
