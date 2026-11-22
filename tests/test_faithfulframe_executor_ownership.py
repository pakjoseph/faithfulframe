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

    def test_production_executor_shared_ownership_uses_reaper_aware_factory(self):
        sources = production_sources()
        production = "\n".join(sources.values())
        self.assertIn("faithfulframe_session_executor::create(", production)
        self.assertNotRegex(production, r"std::make_shared\s*<\s*faithfulframe_session_executor\s*>")

    def test_production_executor_construction_is_confined_to_bridge_factory(self):
        direct_construction = re.compile(
            r"\bfaithfulframe_session_executor\s+[A-Za-z_]\w*\s*\(|"
            r"\bnew\s+faithfulframe_session_executor\s*\(|"
            r"std::make_shared\s*<\s*faithfulframe_session_executor\s*>"
        )
        call_sites = []
        for path, source in production_sources().items():
            if path in ("src/faithfulframe-core-bridge.cpp", "src/faithfulframe-core-bridge.hpp"):
                continue
            for match in direct_construction.finditer(source):
                line_number = source.count("\n", 0, match.start()) + 1
                call_sites.append(f"{path}:{line_number}")

        self.assertEqual(call_sites, [])

    def test_single_adapter_lease_does_not_block_obs_callbacks(self):
        source = (ROOT / "src/faithfulframe-adapter-lease.hpp").read_text(encoding="utf-8")

        self.assertIn("std::atomic<const void *> owner_slot", source)
        self.assertIn("is_always_lock_free", source)
        self.assertNotRegex(source, r"std::(?:mutex|lock_guard|unique_lock|condition_variable)")

    def test_direct_session_call_outside_the_bridge_is_detected(self):
        sources = {
            SERIALIZED_BRIDGE: "ff_session_tick(session, at, &snapshot);",
            "src/face-tracker.cpp": "ff_session_apply_command\n(session, at, &command, &snapshot);",
        }

        self.assertEqual(direct_session_call_sites(sources), ["src/face-tracker.cpp:1"])

    def test_video_tick_lifecycle_reads_defer_instead_of_waiting_for_the_mutex(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        publish_start = source.index("static void publish_source_candidates(")
        settle_start = source.index("static void settle_source_transition(", publish_start)
        tick_end = source.index("static void fts_tick(", settle_start)
        publish = source[publish_start:settle_start]
        settle = source[settle_start:tick_end]

        self.assertIn(
            "std::unique_lock<std::mutex> lifecycle_lock(binding->lifecycle_mutex, std::try_to_lock);",
            publish,
        )
        self.assertIn("if (!lifecycle_lock.owns_lock())\n\t\treturn;", publish)
        self.assertNotIn("std::lock_guard<std::mutex>", publish + settle, "video-tick paths must not wait on locks")
        self.assertIn(
            "std::unique_lock<std::mutex> lock(binding->lifecycle_mutex, std::try_to_lock);",
            settle,
        )
        self.assertIn("if (!lock.owns_lock())\n\t\treturn;", settle)

    def test_video_tick_session_reads_use_the_binding_pinned_atomic_pointer(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        publish_start = source.index("static void publish_source_candidates(")
        settle_start = source.index("static void settle_source_transition(", publish_start)
        tick_safe_start = source.index("static void tick_safe_source(", settle_start)
        tick_safe_end = source.index("\n#endif\n", tick_safe_start)
        publish_and_settle = source[publish_start:tick_safe_start]
        tick_safe = source[tick_safe_start:tick_safe_end]
        steady_state_start = tick_safe.index("const uint32_t width = obs_source_get_width(target);")
        steady_state_end = tick_safe.index("if (binding->policy_faulted", steady_state_start)
        steady_state = tick_safe[steady_state_start:steady_state_end]

        self.assertIn("auto binding_lease = runtime->active.read();", tick_safe)
        self.assertIn("std::atomic<faithfulframe_session_executor *> callback_executor{nullptr};", source)
        self.assertNotIn("atomic_shared_ptr_load(&binding->executor", publish_and_settle + steady_state)
        self.assertIn("binding->callback_executor.load(std::memory_order_acquire)", publish_and_settle)
        self.assertIn("binding->callback_executor.load(std::memory_order_acquire)", steady_state)

    def test_video_tick_does_not_wait_for_pending_detector_settings(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        tick_start = source.index("static void fts_tick(void *data, float second)\n{")
        configuration_start = source.index(
            "\t\tapply_safe_source_configuration(s, s->safe_runtime);", tick_start
        )
        tick_settings = source[tick_start:configuration_start]

        self.assertIn("pending_settings_available.load(std::memory_order_acquire)", tick_settings)
        self.assertRegex(tick_settings, r"pending_settings_mutex,\s*std::try_to_lock")
        self.assertNotRegex(tick_settings, r"std::lock_guard<std::mutex>[^;]*pending_settings_mutex")
        self.assertLess(tick_settings.index("try_idle()"), tick_settings.index("pending_settings_mutex"))

    def test_fixed_target_size_proc_only_writes_declared_outputs(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        signature = re.search(
            r'proc_handler_add\(ph,\s*"([^"]+)"\s*,\s*cb_get_target_size\b', source
        )
        self.assertIsNotNone(signature)
        declared_outputs = set(re.findall(r"\bout\s+\w+\s+(\w+)", signature.group(1)))
        self.assertTrue(declared_outputs)

        callback_start = source.index("static void cb_get_target_size(void *data, calldata_t *cd)\n{")
        callback_end = source.index("\nstatic void cb_get_state(", callback_start)
        callback = source[callback_start:callback_end]
        written_fields = set(re.findall(r'calldata_set_\w+\(cd,\s*"([^"]+)"', callback))

        self.assertTrue(written_fields)
        self.assertLessEqual(
            written_fields,
            declared_outputs,
            "a fixed OBS proc callback must not write undeclared calldata outputs",
        )

    def test_fixed_render_proc_only_writes_declared_outputs(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        signature = re.search(
            r'proc_handler_add\(ph,\s*((?:"[^"]*"\s*)+),\s*cb_render_frame\b',
            source,
            re.DOTALL,
        )
        self.assertIsNotNone(signature)
        declaration = "".join(re.findall(r'"([^"]*)"', signature.group(1)))
        declared_outputs = set(re.findall(r"\bout\s+\w+\s+(\w+)", declaration))
        self.assertTrue(declared_outputs)

        callback_start = source.index("static void cb_render_frame(void *data, calldata_t *cd)\n{")
        callback_end = source.index("\nstatic void cb_render_info(", callback_start)
        callback = source[callback_start:callback_end]
        written_fields = set(re.findall(r'calldata_set_\w+\(cd,\s*"([^"]+)"', callback))

        self.assertTrue(written_fields)
        self.assertLessEqual(
            written_fields,
            declared_outputs,
            "a fixed OBS proc callback must not write undeclared calldata outputs",
        )

    def test_safe_capture_maps_previous_staging_slot_before_current_stage(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        start = source.rindex(
            "static std::shared_ptr<texture_object> capture_safe_staging_pair("
        )
        end = source.index("\n}\n#endif", start)
        callback = source[start:end]
        previous_map = callback.index("surface_to_cvtex(")
        current_stage = callback.index("stage_to_surface(")

        self.assertLess(
            previous_map,
            current_stage,
            "safe mode must map the prior frame before staging the current frame",
        )
        self.assertIn("safe_stagesurfaces[plan.previous_slot]", callback)
        self.assertIn("safe_stagesurfaces[plan.stage_slot]", callback)
        self.assertIn("safe_candidate_max_age_ms", callback)
        self.assertIn("plan.reset_surfaces", callback)

    def test_mapped_staging_surface_is_unmapped_if_frame_allocation_fails(self):
        source = (ROOT / "src/face-tracker.cpp").read_text(encoding="utf-8")
        start = source.rindex("static inline std::shared_ptr<texture_object> surface_to_cvtex(")
        end = source.index("\n#ifdef ENABLE_RUST_CORE_BRIDGE", start)
        callback = source[start:end]

        map_call = callback.index("gs_stagesurface_map(surface")
        unmap_guard = callback.index(
            "std::unique_ptr<gs_stagesurf_t, decltype(&gs_stagesurface_unmap)> mapped_surface"
        )
        allocation = callback.index("std::make_shared<texture_object>()")

        self.assertLess(map_call, unmap_guard)
        self.assertLess(unmap_guard, allocation)
        self.assertIn("&gs_stagesurface_unmap", callback)
        self.assertIn("catch (...)", callback)
        self.assertNotIn("gs_stagesurface_unmap(surface);", callback)


if __name__ == "__main__":
    unittest.main()
