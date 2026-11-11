#include <obs-module.h>
#include <util/platform.h>
#include <util/threading.h>
#include <graphics/vec2.h>
#include <graphics/graphics.h>
#include "plugin-macros.generated.h"
#include "texture-object.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <string>
#include <thread>
#include <vector>
#include <graphics/matrix4.h>
#include "helper.hpp"
#include "face-tracker.hpp"
#include "face-tracker-preset.h"
#include "face-tracker-manager.hpp"
#include "source_list.h"
#ifdef ENABLE_RUST_CORE_BRIDGE
#include "faithfulframe-core-bridge.hpp"
#endif

static inline void scale_texture(struct face_tracker_filter *s, float scale);
static inline int stage_to_surface(struct face_tracker_filter *s, float scale);
static inline std::shared_ptr<texture_object> surface_to_cvtex(struct face_tracker_filter *s, float scale);

class ft_manager_for_ftf : public face_tracker_manager {
public:
	struct face_tracker_filter *ctx;

public:
	ft_manager_for_ftf(struct face_tracker_filter *ctx_) { ctx = ctx_; }

	~ft_manager_for_ftf() { release_cvtex(); }

	inline void release_cvtex() {}

	std::shared_ptr<texture_object> get_cvtex() override
	{
		if (scale < 1.0f)
			scale = 1.0f;
		scale_texture(ctx, scale);
		if (stage_to_surface(ctx, scale))
			return NULL;
		return surface_to_cvtex(ctx, scale);
	};
};

#ifdef ENABLE_RUST_CORE_BRIDGE
struct faithfulframe_source_binding
{
	struct preview_payload
	{
		ff_identity identity{};
		ff_generations generations{};
		uint64_t observation_version = 0;
		uint64_t published_at_ms = 0;
		size_t candidate_count = 0;
		std::array<ff_candidate, faithfulframe_session_executor::max_candidates> candidates{};
	};

	obs_weak_source_t *target = nullptr;
	std::string target_name;
	std::shared_ptr<faithfulframe_session_executor> executor;
	ff_identity identity{};
	ff_generations generations{1, 1, 1, 1, 1};
	uint64_t observation_version = 0;
	std::atomic<uint32_t> source_width{0};
	std::atomic<uint32_t> source_height{0};
	std::atomic<uint32_t> output_width{0};
	std::atomic<uint32_t> output_height{0};
	std::atomic<uint64_t> detector_last_work_us{0};
	std::atomic<uint64_t> detector_max_work_us{0};
	std::atomic<uint64_t> detector_work_count{0};
	std::atomic<bool> signal_connected{false};
	std::atomic<bool> source_active{false};
	std::atomic<bool> closed{false};
	std::atomic<bool> transition_pending{false};
	std::mutex preview_mutex;
	preview_payload preview;

	~faithfulframe_source_binding()
	{
		auto session = std::atomic_exchange_explicit(
			&executor, std::shared_ptr<faithfulframe_session_executor>{}, std::memory_order_acq_rel);
		if (session) {
			session->shutdown();
		}
		if (target)
			obs_weak_source_release(target);
	}
};

struct faithfulframe_source_runtime
{
	std::atomic<bool> enabled{false};
	std::atomic<bool> reconfiguration_pending{false};
	std::mutex configuration_mutex;
	std::string requested_target_name;
	std::atomic<uint64_t> configuration_version{0};
	std::atomic<uint64_t> tracker_configuration_version{0};
	std::atomic<uint32_t> last_output_width{0};
	std::atomic<uint32_t> last_output_height{0};
	std::mutex pending_settings_mutex;
	obs_data_t *pending_settings = nullptr;
	uint64_t processed_configuration_version = 0;
	uint64_t processed_tracker_configuration_version = 0;
	std::atomic<faithfulframe_source_binding *> active{nullptr};
	std::vector<std::unique_ptr<faithfulframe_source_binding>> bindings;

	~faithfulframe_source_runtime()
	{
		if (pending_settings)
			obs_data_release(pending_settings);
	}
};

static std::atomic<uint64_t> faithfulframe_next_identity{1};

static uint64_t allocate_faithfulframe_identity()
{
	uint64_t value = faithfulframe_next_identity.load(std::memory_order_relaxed);
	while (value < static_cast<uint64_t>(std::numeric_limits<int64_t>::max())) {
		if (faithfulframe_next_identity.compare_exchange_weak(value, value + 1, std::memory_order_relaxed))
			return value;
	}
	return 0;
}
#endif

static const char *ftf_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("Face Tracker");
}

static inline void get_aspect_from_str(struct face_tracker_filter *s, const char *str)
{
	if (sscanf(str, "%d:%d", &s->aspect_x, &s->aspect_y) == 2)
		return;
	if (sscanf(str, "%dx%d", &s->aspect_x, &s->aspect_y) == 2)
		return;
	s->aspect_x = 0;
	s->aspect_y = 0;
}

static void ftf_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_filter *)data;

	s->ftm->update(settings);
	s->track_z = obs_data_get_double(settings, "track_z");
	s->track_x = obs_data_get_double(settings, "track_x");
	s->track_y = obs_data_get_double(settings, "track_y");
	s->scale_max = obs_data_get_double(settings, "scale_max");

	double kp = obs_data_get_double(settings, "Kp");
	float ki = (float)obs_data_get_double(settings, "Ki");
	double td = obs_data_get_double(settings, "Td");
	double att2 = from_dB(obs_data_get_double(settings, "att2_dB"));
	s->kp.v[0] = s->kp.v[1] = (float)kp;
	s->kp.v[2] = (float)(att2 * kp);
	s->ki = ki;
	s->klpf = s->kp * td;
	s->tlpf.v[0] = s->tlpf.v[1] = (float)obs_data_get_double(settings, "Tdlpf");
	s->tlpf.v[2] = (float)obs_data_get_double(settings, "Tdlpf_z");
	s->e_deadband.v[0] = (float)obs_data_get_double(settings, "e_deadband_x") * 1e-2;
	s->e_deadband.v[1] = (float)obs_data_get_double(settings, "e_deadband_y") * 1e-2;
	s->e_deadband.v[2] = (float)obs_data_get_double(settings, "e_deadband_z") * 1e-2;
	s->e_nonlinear.v[0] = (float)obs_data_get_double(settings, "e_nonlinear_x") * 1e-2;
	s->e_nonlinear.v[1] = (float)obs_data_get_double(settings, "e_nonlinear_y") * 1e-2;
	s->e_nonlinear.v[2] = (float)obs_data_get_double(settings, "e_nonlinear_z") * 1e-2;

	get_aspect_from_str(s, obs_data_get_string(settings, "aspect"));

	s->inactive_reset = obs_data_get_bool(settings, "inactive_reset");

	s->debug_faces = obs_data_get_bool(settings, "debug_faces");
	s->debug_notrack = obs_data_get_bool(settings, "debug_notrack");
	s->debug_always_show = obs_data_get_bool(settings, "debug_always_show");

	debug_data_open(&s->debug_data_tracker, &s->debug_data_tracker_last, settings, "debug_data_tracker");
	debug_data_open(&s->debug_data_error, &s->debug_data_error_last, settings, "debug_data_error");
	debug_data_open(&s->debug_data_control, &s->debug_data_control_last, settings, "debug_data_control");
}

static void fts_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_filter *)data;
	bool defer_tracker_update = false;

	const char *target_name = obs_data_get_string(settings, "target_name");
	const std::string requested_name = target_name ? target_name : "";
#ifdef ENABLE_RUST_CORE_BRIDGE
	bool binding_changed = false;
	if (s->safe_runtime) {
		{
			std::lock_guard<std::mutex> lock(s->safe_runtime->configuration_mutex);
			binding_changed = s->safe_runtime->requested_target_name != requested_name;
			s->safe_runtime->requested_target_name = requested_name;
		}
		const bool enabled = obs_data_get_bool(settings, "safe_tracking_enabled");
		const bool was_enabled = s->safe_runtime->enabled.exchange(enabled, std::memory_order_acq_rel);
		defer_tracker_update = enabled || was_enabled;
		if (binding_changed || was_enabled != enabled) {
			s->safe_runtime->reconfiguration_pending.store(true, std::memory_order_release);
			s->safe_runtime->configuration_version.fetch_add(1, std::memory_order_release);
		} else {
			s->safe_runtime->tracker_configuration_version.fetch_add(1, std::memory_order_release);
			if (auto *binding = s->safe_runtime->active.load(std::memory_order_acquire))
				binding->transition_pending.store(true, std::memory_order_release);
		}
		if (defer_tracker_update) {
			obs_data_addref(settings);
			obs_data_t *previous = nullptr;
			{
				std::lock_guard<std::mutex> lock(s->safe_runtime->pending_settings_mutex);
				previous = s->safe_runtime->pending_settings;
				s->safe_runtime->pending_settings = settings;
			}
			if (previous)
				obs_data_release(previous);
		}
	}
#endif
	if (!defer_tracker_update)
		ftf_update(data, settings);
	const char *current_name = s->target_name ? s->target_name : "";
	if (strcmp(current_name, requested_name.c_str()) != 0) {
		bfree(s->target_name);
		s->target_name = requested_name.empty() ? nullptr : bstrdup(requested_name.c_str());
	}
}

static void cb_render_frame(void *data, calldata_t *cd);
static void cb_render_info(void *data, calldata_t *cd);
static void cb_get_target_size(void *data, calldata_t *cd);
static void cb_get_state(void *data, calldata_t *cd);
static void cb_set_state(void *data, calldata_t *cd);
#ifdef ENABLE_RUST_CORE_BRIDGE
static void cb_get_faithfulframe_preview(void *data, calldata_t *cd);
static void cb_faithfulframe_command(void *data, calldata_t *cd);
static bool enqueue_safe_operator_command(struct face_tracker_filter *s, uint32_t kind);
#endif
static const char *ftptz_signals[] = {"void state_changed()", NULL};
static void emit_state_changed(struct face_tracker_filter *);

static void *ftf_create_internal(obs_data_t *settings, obs_source_t *context, bool source_adapter)
{
	auto *s = (struct face_tracker_filter *)bzalloc(sizeof(struct face_tracker_filter));
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (source_adapter) {
		s->safe_runtime = new (std::nothrow) faithfulframe_source_runtime();
		if (!s->safe_runtime) {
			bfree(s);
			return nullptr;
		}
	}
#else
	UNUSED_PARAMETER(source_adapter);
#endif
	s->ftm = new ft_manager_for_ftf(s);
	s->ftm->crop_cur.x1 = s->ftm->crop_cur.y1 = -2;
	s->context = context;
	s->ftm->scale = 2.0f;
	s->hotkey_pause = OBS_INVALID_HOTKEY_PAIR_ID;
	s->hotkey_reset = OBS_INVALID_HOTKEY_ID;

	obs_source_update(context, settings);

	proc_handler_t *ph = obs_source_get_proc_handler(context);
	proc_handler_add(ph, "void render_frame(bool notrack)", cb_render_frame, s);
	proc_handler_add(ph, "void render_info(bool notrack)", cb_render_info, s);
	proc_handler_add(ph, "void get_target_size(out int width, out int height)", cb_get_target_size, s);
	proc_handler_add(ph, "void get_state()", cb_get_state, s);
	proc_handler_add(ph, "void set_state()", cb_set_state, s);
#ifdef ENABLE_RUST_CORE_BRIDGE
	proc_handler_add(ph, "void get_faithfulframe_preview()", cb_get_faithfulframe_preview, s);
	proc_handler_add(
		ph,
		"void faithfulframe_command(int kind, int source_id, int session_id, int status_version, "
		"int source_generation, int session_generation, int stream_generation, int geometry_generation, "
		"int tracker_generation, int candidate_handle, int observation_version)",
		cb_faithfulframe_command, s);
#endif

	signal_handler_t *sh = obs_source_get_signal_handler(context);
	signal_handler_add_array(sh, ftptz_signals);

	return s;
}

static void *ftf_create(obs_data_t *settings, obs_source_t *context)
{
	return ftf_create_internal(settings, context, false);
}

static void register_hotkeys(struct face_tracker_filter *s, obs_source_t *target);

static void *fts_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = (struct face_tracker_filter *)ftf_create_internal(settings, context, true);
	if (!s)
		return nullptr;

	register_hotkeys(s, context);

	return s;
}

static void ftf_destroy(void *data)
{
	auto *s = (struct face_tracker_filter *)data;

#ifdef ENABLE_RUST_CORE_BRIDGE
	delete s->safe_runtime;
	s->safe_runtime = nullptr;
#endif

	if (s->hotkey_pause != OBS_INVALID_HOTKEY_PAIR_ID)
		obs_hotkey_pair_unregister(s->hotkey_pause);
	if (s->hotkey_reset != OBS_INVALID_HOTKEY_ID)
		obs_hotkey_unregister(s->hotkey_reset);

	obs_enter_graphics();
	gs_texrender_destroy(s->texrender);
	s->texrender = NULL;
	gs_texrender_destroy(s->texrender_scaled);
	s->texrender_scaled = NULL;
	gs_stagesurface_destroy(s->stagesurface);
	s->stagesurface = NULL;
	obs_leave_graphics();

	delete s->ftm;

	bfree(s->target_name);
	obs_weak_source_release(s->target_ref);
	if (s->debug_data_tracker)
		fclose(s->debug_data_tracker);
	if (s->debug_data_error)
		fclose(s->debug_data_error);
	if (s->debug_data_control)
		fclose(s->debug_data_control);
	bfree(s->debug_data_tracker_last);
	bfree(s->debug_data_error_last);
	bfree(s->debug_data_control_last);

	bfree(s);
}

static bool ftf_reset_tracking(obs_properties_t *, obs_property_t *, void *data)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire))
		return true;
#endif

	float w = s->known_width;
	float h = s->known_height;
	float z = sqrtf(s->width_with_aspect * s->height_with_aspect);
	s->detect_err = f3(0, 0, 0);
	s->filter_int_out = f3(w * 0.5f, h * 0.5f, z);
	s->filter_int = f3(0, 0, 0);
	s->filter_lpf = f3(0, 0, 0);
	s->ftm->reset_requested = true;

	return true;
}

static obs_properties_t *ftf_properties(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	obs_properties_t *props;
	props = obs_properties_create();

	obs_properties_add_button(props, "ftf_reset_tracking", obs_module_text("Reset tracking"), ftf_reset_tracking);

	{
		obs_properties_t *pp = obs_properties_create();
		obs_property_t *p = obs_properties_add_list(pp, "preset_name", obs_module_text("Preset"),
							    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		obs_data_t *settings = obs_source_get_settings(s->context);
		if (settings) {
			ftf_preset_item_to_list(p, settings);
			obs_data_release(settings);
		}
		obs_properties_add_button(pp, "preset_load", obs_module_text("Load preset"), ftf_preset_load);
		obs_properties_add_button(pp, "preset_save", obs_module_text("Save preset"), ftf_preset_save);
		obs_properties_add_button(pp, "preset_delete", obs_module_text("Delete preset"), ftf_preset_delete);
		obs_properties_add_bool(pp, "preset_mask_track", obs_module_text("Save and load tracking parameters"));
		obs_properties_add_bool(pp, "preset_mask_control", obs_module_text("Save and load control parameters"));
		obs_properties_add_group(props, "preset_grp", obs_module_text("Preset"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		face_tracker_manager::get_properties(pp);
		obs_properties_add_group(props, "ftm", obs_module_text("Face detection options"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "track_z", obs_module_text("Zoom"), 0.1, 2.0, 0.05);
		obs_properties_add_float(pp, "track_x", obs_module_text("X"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "track_y", obs_module_text("Y"), -1.0, +1.0, 0.05);
		obs_properties_add_float(pp, "scale_max", obs_module_text("Scale max"), 1.0, 20.0, 1.0);
		obs_properties_add_group(props, "track", obs_module_text("Tracking target location"), OBS_GROUP_NORMAL,
					 pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_float(pp, "Kp", "Track Kp", 0.01, 10.0, 0.1);
		obs_properties_add_float(pp, "Ki", "Track Ki", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Td", "Track Td", 0.0, 5.0, 0.01);
		obs_properties_add_float(pp, "Tdlpf", "Track LPF for Td (X, Y)", 0.0, 10.0, 0.1);
		obs_properties_add_float(pp, "Tdlpf_z", "Track LPF for Td (Z)", 0.0, 10.0, 0.1);
		obs_properties_add_float(pp, "att2_dB", "Attenuation (Z)", -60.0, 10.0, 1.0);
		obs_properties_add_float(pp, "e_deadband_x", "Dead band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_y", "Dead band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_deadband_z", "Dead band (Z)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_x", "Nonlinear band (X)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_y", "Nonlinear band (Y)", 0.0, 50, 0.1);
		obs_properties_add_float(pp, "e_nonlinear_z", "Nonlinear band (Z)", 0.0, 50, 0.1);
		obs_properties_add_group(props, "ctrl", obs_module_text("Tracking response"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_property_t *p = obs_properties_add_list(pp, "aspect", obs_module_text("Aspect"),
							    OBS_COMBO_TYPE_EDITABLE, OBS_COMBO_FORMAT_STRING);
		const char *aspects[] = {"16:9", "4:3", "1:1", "3:4", "9:16", NULL};
		obs_property_list_add_string(p, obs_module_text("same as the source"), "");
		for (int i = 0; aspects[i]; i++)
			obs_property_list_add_string(p, aspects[i], aspects[i]);
		obs_properties_add_group(props, "output", obs_module_text("Output"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "inactive_reset", obs_module_text("Prop.Automation.InactiveReset"));
		obs_properties_add_group(props, "automation", obs_module_text("Automation"), OBS_GROUP_NORMAL, pp);
	}

	{
		obs_properties_t *pp = obs_properties_create();
		obs_properties_add_bool(pp, "debug_faces", "Show face detection results");
		obs_properties_add_bool(pp, "debug_notrack", "Stop tracking faces");
		obs_properties_add_bool(pp, "debug_always_show", "Always show information (useful for demo)");
#ifdef ENABLE_DEBUG_DATA
		obs_properties_add_path(pp, "debug_data_tracker", "Save correlation tracker data to file",
					OBS_PATH_FILE_SAVE, DEBUG_DATA_PATH_FILTER, NULL);
		obs_properties_add_path(pp, "debug_data_error", "Save calculated error data to file",
					OBS_PATH_FILE_SAVE, DEBUG_DATA_PATH_FILTER, NULL);
		obs_properties_add_path(pp, "debug_data_control", "Save control data to file", OBS_PATH_FILE_SAVE,
					DEBUG_DATA_PATH_FILTER, NULL);
#endif
		obs_properties_add_group(props, "debug", obs_module_text("Debugging"), OBS_GROUP_NORMAL, pp);
	}

	return props;
}

static obs_properties_t *fts_properties(void *data)
{
	auto *s = (struct face_tracker_filter *)data;

	obs_properties_t *props = ftf_properties(data);

	obs_properties_t *pp = obs_properties_create();
	obs_property_t *p = obs_properties_add_list(pp, "target_name", obs_module_text("Source"), OBS_COMBO_TYPE_LIST,
						    OBS_COMBO_FORMAT_STRING);
	property_list_add_sources(p, s ? s->context : NULL);

	obs_properties_add_group(props, "input", obs_module_text("Input"), OBS_GROUP_NORMAL, pp);

#ifdef ENABLE_RUST_CORE_BRIDGE
	obs_properties_add_bool(props, "safe_tracking_enabled", "Use FaithfulFrame safe tracking (experimental)");
#endif

	return props;
}

static void ftf_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "preset_mask_track", true);
	obs_data_set_default_bool(settings, "preset_mask_control", true);
	face_tracker_manager::get_defaults(settings);
	obs_data_set_default_double(settings, "track_z", 0.70);  //  1.00  0.50  0.35
	obs_data_set_default_double(settings, "track_y", +0.00); // +0.00 +0.10 +0.30
	obs_data_set_default_double(settings, "scale_max", 10.0);

	obs_data_set_default_double(settings, "Kp", 0.95);
	obs_data_set_default_double(settings, "Ki", 0.3);
	obs_data_set_default_double(settings, "Td", 0.42);
	obs_data_set_default_double(settings, "Tdlpf", 2.0);
	obs_data_set_default_double(settings, "Tdlpf_z", 6.0);
	obs_data_set_default_double(settings, "att2_dB", -10);

	obs_data_t *presets = obs_data_create();
	obs_data_set_default_obj(settings, "presets", presets);
	obs_data_release(presets);
}

static void fts_get_defaults(obs_data_t *settings)
{
	ftf_get_defaults(settings);
#ifdef ENABLE_RUST_CORE_BRIDGE
	obs_data_set_default_bool(settings, "safe_tracking_enabled", false);
#endif
}

static void tick_filter(struct face_tracker_filter *s, float second)
{
	const float srwh = sqrtf((float)s->known_width * s->known_height);

	f3 e = s->detect_err;
	f3 e_int = e;
	for (int i = 0; i < 3; i++) {
		float x = e.v[i];
		float d = srwh * s->e_deadband.v[i];
		float n = srwh * s->e_nonlinear.v[i];
		if (std::abs(x) <= d)
			x = 0.0f;
		else if (std::abs(x) < (d + n)) {
			if (x > 0)
				x = +sqf(x - d) / (2.0f * n);
			else
				x = -sqf(x - d) / (2.0f * n);
		} else if (x > 0)
			x -= d + n * 0.5f;
		else
			x += d + n * 0.5f;
		if (second * s->ki > 1.0e-10) {
			if (s->filter_int.v[i] < 0.0f && e.v[i] > 0.0f)
				e_int.v[i] = std::min(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else if (s->filter_int.v[i] > 0.0f && e.v[i] < 0.0f)
				e_int.v[i] = std::max(e.v[i], -s->filter_int.v[i] / (second * s->ki));
			else
				e_int.v[i] = x;
		}
		e.v[i] = x;
	}

	s->filter_int_out += (e + s->filter_int).hp(s->kp * second);
	s->filter_int += e_int * (second * s->ki);
	for (int i = 0; i < 3; i++)
		s->filter_lpf.v[i] = (s->filter_lpf.v[i] * s->tlpf.v[i] + e.v[i] * second) / (s->tlpf.v[i] + second);

	f3 u = s->filter_int_out + s->filter_lpf.hp(s->klpf);

	for (int i = 0; i < 3; i++) {
		if (isnan(u.v[i]))
			u.v[i] = (s->range_max.v[i] + s->range_min_out.v[i]) * 0.5f;
		else if (u.v[i] < s->range_min_out.v[i]) {
			u.v[i] = s->range_min_out.v[i];
			if (s->filter_int_out.v[i] < s->range_min_out.v[i])
				s->filter_int_out.v[i] = s->range_min_out.v[i];
		} else if (u.v[i] > s->range_max.v[i]) {
			u.v[i] = s->range_max.v[i];
			if (s->filter_int_out.v[i] > s->range_max.v[i])
				s->filter_int_out.v[i] = s->range_max.v[i];
		}
	}

	s->u_last = u;

	if (s->debug_data_control) {
		fprintf(s->debug_data_control, "%f\t%f\t%f\t%f\n", os_gettime_ns() * 1e-9, u.v[0], u.v[1], u.v[2]);
	}

	s->ftm->crop_cur = f3_to_rectf(u, s->width_with_aspect, s->height_with_aspect);
}

static void ftf_activate(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	s->is_active = true;
}

static void ftf_deactivate(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
	s->is_active = false;

#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire))
		return;
#endif
	if (s->inactive_reset) {
		ftf_reset_tracking(nullptr, nullptr, data);
		s->ftm->crop_cur = f3_to_rectf(s->filter_int_out, s->width_with_aspect, s->height_with_aspect);
	}
}

static inline void calculate_error(struct face_tracker_filter *s);

static void calculate_aspect(struct face_tracker_filter *s)
{
	if (s->aspect_y <= 0 || s->aspect_x <= 0) {
		s->width_with_aspect = s->known_width;
		s->height_with_aspect = s->known_height;
	} else if (s->known_width * s->aspect_y >= s->known_height * s->aspect_x) {
		s->height_with_aspect = s->known_height;
		s->width_with_aspect = s->aspect_x * s->known_height / s->aspect_y;
	} else {
		s->width_with_aspect = s->known_width;
		s->height_with_aspect = s->known_width * s->aspect_y / s->aspect_x;
	}
}

static bool hotkey_cb_pause(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	auto *s = (struct face_tracker_filter *)data;
	if (!pressed)
		return false;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		enqueue_safe_operator_command(s, FF_COMMAND_FREEZE);
		return true;
	}
#endif
	if (s->is_paused)
		return false;
	s->is_paused = true;
	emit_state_changed(s);
	return true;
}

static bool hotkey_cb_pause_resume(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	auto *s = (struct face_tracker_filter *)data;
	if (!pressed)
		return false;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		enqueue_safe_operator_command(s, FF_COMMAND_RESUME);
		return true;
	}
#endif
	if (!s->is_paused)
		return false;
	s->is_paused = false;
	emit_state_changed(s);
	return true;
}

static void hotkey_cb_reset(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	UNUSED_PARAMETER(id);
	UNUSED_PARAMETER(hotkey);
	if (pressed) {
#ifdef ENABLE_RUST_CORE_BRIDGE
		auto *s = static_cast<face_tracker_filter *>(data);
		if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
			enqueue_safe_operator_command(s, FF_COMMAND_RESET_TO_WIDE);
			return;
		}
#endif
		ftf_reset_tracking(NULL, NULL, data);
	}
}

static void register_hotkeys(struct face_tracker_filter *s, obs_source_t *target)
{
	if (!target)
		return;

	if (s->hotkey_pause == OBS_INVALID_HOTKEY_PAIR_ID) {
		s->hotkey_pause = obs_hotkey_pair_register_source(target, "face-tracker.pause",
								  obs_module_text("Pause Face Tracker"),
								  "face-tracker.pause_resume",
								  obs_module_text("Resume Face Tracker"),
								  hotkey_cb_pause, hotkey_cb_pause_resume, s, s);
	}

	if (s->hotkey_reset == OBS_INVALID_HOTKEY_ID) {
		s->hotkey_reset = obs_hotkey_register_source(target, "face-tracker.reset",
							     obs_module_text("Reset Face Tracker"), hotkey_cb_reset, s);
	}
}

inline static bool is_running(struct face_tracker_filter *s)
{
	if (s->is_paused)
		return false;

	if (s->inactive_reset && !s->is_active)
		return false;

	return true;
}

static void ft_tick_internal(struct face_tracker_filter *s, float second, bool was_rendered)
{
	if (s->known_width <= 0 || s->known_height <= 0) {
		return;
	}

	calculate_aspect(s);

	if (s->ftm->crop_cur.x1 < -1 || s->ftm->crop_cur.y1 < -1) {
		ftf_reset_tracking(NULL, NULL, s);
		s->ftm->crop_cur = f3_to_rectf(s->filter_int_out, s->width_with_aspect, s->height_with_aspect);
	} else if (was_rendered && !s->is_paused) {
		s->range_min.v[0] = get_width(s->ftm->crop_cur) * 0.5f;
		s->range_max.v[0] = s->known_width - get_width(s->ftm->crop_cur) * 0.5f;
		s->range_min.v[1] = get_height(s->ftm->crop_cur) * 0.5f;
		s->range_max.v[1] = s->known_height - get_height(s->ftm->crop_cur) * 0.5f;
		s->range_min.v[2] = sqrtf(s->known_width * s->known_height) / s->scale_max;
		s->range_max.v[2] = sqrtf(s->width_with_aspect * s->height_with_aspect);
		s->range_min_out = s->range_min;
		s->range_min_out.v[2] = std::max(std::min(s->range_min.v[2], s->u_last.v[2]), 1.0f);
		calculate_error(s);
		tick_filter(s, second);
	}

	s->target_valid = true;
}

static void ftf_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool was_rendered = s->rendered;
	s->rendered = false;
	s->target_valid = false;

	s->ftm->tick(second);

	obs_source_t *target = obs_filter_get_target(s->context);
	if (!target)
		return;

	if (s->hotkey_pause == OBS_INVALID_HOTKEY_PAIR_ID || s->hotkey_reset == OBS_INVALID_HOTKEY_ID)
		register_hotkeys(s, obs_filter_get_parent(s->context));

	s->known_width = obs_source_get_base_width(target);
	s->known_height = obs_source_get_base_height(target);

	ft_tick_internal(s, second, was_rendered);
}

#ifdef ENABLE_RUST_CORE_BRIDGE
static uint64_t source_monotonic_millis()
{
	return os_gettime_ns() / 1000000ULL;
}

static bool advance_source_generations(uint32_t kind, const ff_generations &current, ff_generations &next)
{
	next = current;
	auto increment = [](uint64_t &value) {
		if (value == std::numeric_limits<uint64_t>::max())
			return false;
		++value;
		return true;
	};
	switch (kind) {
	case FF_LIFECYCLE_SCENE_INACTIVE:
	case FF_LIFECYCLE_SCENE_ACTIVE:
		return increment(next.session) && increment(next.tracker);
	case FF_LIFECYCLE_CAMERA_DISCONNECTED:
	case FF_LIFECYCLE_CAMERA_RESTORED:
		return increment(next.stream) && increment(next.tracker);
	case FF_LIFECYCLE_SOURCE_REBOUND:
		return increment(next.source) && increment(next.session) && increment(next.stream) &&
		       increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_GEOMETRY_CHANGED:
		return increment(next.stream) && increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_TRACKER_CHANGED:
		return increment(next.tracker);
	case FF_LIFECYCLE_FRAMING_CONFIG_CHANGED:
		return increment(next.session) && increment(next.geometry) && increment(next.tracker);
	case FF_LIFECYCLE_CANDIDATE_POLICY_CHANGED:
	case FF_LIFECYCLE_ADAPTER_DESTROYED:
		return increment(next.session) && increment(next.tracker);
	case FF_LIFECYCLE_SOURCE_DESTROYED:
	case FF_LIFECYCLE_OBS_SHUTDOWN:
		return increment(next.source) && increment(next.session) && increment(next.stream) &&
		       increment(next.tracker);
	default:
		return false;
	}
}

static void find_legacy_tracker_filter(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *found = static_cast<bool *>(param);
	const char *id = obs_source_get_id(filter);
	if (obs_source_enabled(filter) && id && (!strcmp(id, "face_tracker_filter") || !strcmp(id, "face_tracker_ptz")))
		*found = true;
}

static bool enqueue_source_lifecycle(faithfulframe_source_binding *binding, uint32_t kind, uint64_t at,
				     uint32_t width = 0, uint32_t height = 0)
{
	if (!binding)
		return false;
	{
		std::lock_guard<std::mutex> lock(binding->preview_mutex);
		binding->preview.candidate_count = 0;
	}
	auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
	if (!executor || binding->closed.load(std::memory_order_acquire))
		return false;
	ff_lifecycle event{};
	event.kind = kind;
	event.identity = binding->identity;
	event.source_width = width;
	event.source_height = height;
	if (!advance_source_generations(kind, binding->generations, event.generations)) {
		binding->transition_pending.store(true, std::memory_order_release);
		executor->request_shutdown(FF_LIFECYCLE_SOURCE_DESTROYED);
		binding->closed.store(true, std::memory_order_release);
		return false;
	}
	binding->transition_pending.store(true, std::memory_order_release);
	if (!executor->enqueue_lifecycle(at, event)) {
		executor->request_shutdown(FF_LIFECYCLE_SOURCE_DESTROYED);
		binding->closed.store(true, std::memory_order_release);
		return false;
	}
	binding->generations = event.generations;
	if (kind == FF_LIFECYCLE_SOURCE_DESTROYED || kind == FF_LIFECYCLE_SOURCE_REBOUND ||
	    kind == FF_LIFECYCLE_ADAPTER_DESTROYED || kind == FF_LIFECYCLE_OBS_SHUTDOWN)
		binding->closed.store(true, std::memory_order_release);
	return true;
}

static bool initialize_source_session(face_tracker_filter *s, faithfulframe_source_binding *binding,
				      uint32_t source_width, uint32_t source_height)
{
	if (!source_width || !source_height || !binding || !binding->target)
		return false;
	s->known_width = source_width;
	s->known_height = source_height;
	calculate_aspect(s);
	if (!s->width_with_aspect || !s->height_with_aspect)
		return false;

	ff_session_config config{};
	config.loss_grace_ms = 5000;
	config.max_evidence_age_ms = 500;
	config.max_candidates = faithfulframe_session_executor::max_candidates;
	config.source_width = source_width;
	config.source_height = source_height;
	config.output_width = s->width_with_aspect;
	config.output_height = s->height_with_aspect;
	config.target_coverage = 0.45f;
	config.min_zoom = 1.0f;
	config.max_zoom = 4.0f;
	config.smoothing = 0.25f;
	config.max_pan_step = 0.08f;
	config.max_zoom_step = 0.08f;

	uint64_t source_id = allocate_faithfulframe_identity();
	uint64_t session_id = allocate_faithfulframe_identity();
	if (!source_id || !session_id)
		return false;
	binding->identity = ff_identity{source_id, session_id};
	const ff_generations initial{1, 1, 1, 1, 1};
	try {
		auto executor = std::make_shared<faithfulframe_session_executor>(config, binding->identity, initial);
		std::atomic_store_explicit(&binding->executor, std::move(executor), std::memory_order_release);
	} catch (...) {
		return false;
	}
	binding->generations = initial;
	binding->source_width.store(source_width, std::memory_order_release);
	binding->source_height.store(source_height, std::memory_order_release);
	binding->output_width.store(config.output_width, std::memory_order_release);
	binding->output_height.store(config.output_height, std::memory_order_release);
	binding->signal_connected.store(true, std::memory_order_release);
	return true;
}

static void apply_safe_source_configuration(face_tracker_filter *s, faithfulframe_source_runtime *runtime)
{
	const uint64_t revision = runtime->configuration_version.load(std::memory_order_acquire);
	if (revision == runtime->processed_configuration_version)
		return;

	faithfulframe_source_binding *previous = runtime->active.exchange(nullptr, std::memory_order_acq_rel);
	const bool enabled = runtime->enabled.load(std::memory_order_acquire);
	if (previous) {
		auto executor = std::atomic_load_explicit(&previous->executor, std::memory_order_acquire);
		if (executor && !previous->closed.load(std::memory_order_acquire)) {
			previous->transition_pending.store(true, std::memory_order_release);
			executor->request_shutdown(enabled ? FF_LIFECYCLE_SOURCE_REBOUND
							   : FF_LIFECYCLE_ADAPTER_DESTROYED);
			previous->closed.store(true, std::memory_order_release);
		}
	}
	s->ftm->reset_requested = true;

	runtime->processed_configuration_version = revision;
	if (!enabled) {
		runtime->reconfiguration_pending.store(false, std::memory_order_release);
		return;
	}

	std::string target_name;
	{
		std::lock_guard<std::mutex> lock(runtime->configuration_mutex);
		target_name = runtime->requested_target_name;
	}
	if (target_name.empty()) {
		blog(LOG_WARNING, "FaithfulFrame safe tracking has no bound video source");
		return;
	}

	obs_source_t *target = obs_get_source_by_name(target_name.c_str());
	bool has_legacy_tracker = false;
	if (target)
		obs_source_enum_filters(target, find_legacy_tracker_filter, &has_legacy_tracker);
	const char *target_id = target ? obs_source_get_id(target) : nullptr;
	if (!target || target == s->context || obs_source_is_scene(target) ||
	    obs_source_get_type(target) != OBS_SOURCE_TYPE_INPUT ||
	    !(obs_source_get_output_flags(target) & OBS_SOURCE_VIDEO) || !target_id ||
	    !strcmp(target_id, "face_tracker_source") || has_legacy_tracker) {
		blog(LOG_WARNING, "FaithfulFrame rejected invalid, filtered, or unavailable source binding '%s'",
		     target_name.c_str());
		if (target)
			obs_source_release(target);
		return;
	}
	// Grow the owning container before starting an executor. If allocation fails,
	// no worker exists that the local binding destructor could join on this tick.
	try {
		runtime->bindings.reserve(runtime->bindings.size() + 1);
	} catch (...) {
		blog(LOG_ERROR, "FaithfulFrame could not reserve a source binding for '%s'", target_name.c_str());
		obs_source_release(target);
		return;
	}

	std::unique_ptr<faithfulframe_source_binding> binding(new (std::nothrow) faithfulframe_source_binding());
	if (!binding) {
		obs_source_release(target);
		return;
	}
	binding->target_name = target_name;
	binding->target = obs_source_get_weak_source(target);
	const uint32_t source_width = obs_source_get_width(target);
	const uint32_t source_height = obs_source_get_height(target);
	const bool signal_connected = source_width > 0 && source_height > 0;
	binding->source_width.store(source_width, std::memory_order_release);
	binding->source_height.store(source_height, std::memory_order_release);
	binding->signal_connected.store(signal_connected, std::memory_order_release);
	binding->source_active.store(obs_source_active(target), std::memory_order_release);
	if (signal_connected)
		initialize_source_session(s, binding.get(), source_width, source_height);
	if (!std::atomic_load_explicit(&binding->executor, std::memory_order_acquire) && signal_connected)
		blog(LOG_WARNING, "FaithfulFrame failed to create a safe tracking session for '%s'",
		     target_name.c_str());
	faithfulframe_source_binding *published = binding.get();
	try {
		runtime->bindings.push_back(std::move(binding));
	} catch (...) {
		obs_source_release(target);
		return;
	}
	runtime->active.store(published, std::memory_order_release);
	runtime->last_output_width.store(published->output_width.load(std::memory_order_acquire),
					 std::memory_order_release);
	runtime->last_output_height.store(published->output_height.load(std::memory_order_acquire),
					  std::memory_order_release);
	runtime->reconfiguration_pending.store(false, std::memory_order_release);
	runtime->processed_tracker_configuration_version =
		runtime->tracker_configuration_version.load(std::memory_order_acquire);
	s->ftm->crop_cur = rectf_s{0.0f, 0.0f, (float)published->source_width.load(std::memory_order_acquire),
				   (float)published->source_height.load(std::memory_order_acquire)};
	if (target)
		obs_source_release(target);

	if (std::atomic_load_explicit(&published->executor, std::memory_order_acquire) &&
	    !published->source_active.load(std::memory_order_acquire))
		enqueue_source_lifecycle(published, FF_LIFECYCLE_SCENE_INACTIVE, source_monotonic_millis());
}

static void publish_source_candidates(face_tracker_filter *s, faithfulframe_source_binding *binding, uint64_t now)
{
	if (!binding || binding->closed.load(std::memory_order_acquire) ||
	    !binding->signal_connected.load(std::memory_order_acquire) ||
	    !binding->source_active.load(std::memory_order_acquire))
		return;
	auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
	if (!executor)
		return;
	if (binding->observation_version >= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		return;
	const uint32_t source_width = binding->source_width.load(std::memory_order_acquire);
	const uint32_t source_height = binding->source_height.load(std::memory_order_acquire);
	if (!source_width || !source_height)
		return;

	std::array<ff_candidate, faithfulframe_session_executor::max_candidates> candidates{};
	size_t count = 0;
	for (const auto &tracker : s->ftm->tracker_rects) {
		if (count == candidates.size() || !tracker.candidate_handle || !tracker.continuity_token)
			continue;
		const float x0 = std::max(0.0f, (float)tracker.rect.x0 / source_width);
		const float y0 = std::max(0.0f, (float)tracker.rect.y0 / source_height);
		const float x1 = std::min(1.0f, (float)tracker.rect.x1 / source_width);
		const float y1 = std::min(1.0f, (float)tracker.rect.y1 / source_height);
		if (!(x1 > x0 && y1 > y0))
			continue;
		auto &candidate = candidates[count++];
		candidate.handle = tracker.candidate_handle;
		candidate.continuity_token = tracker.continuity_token;
		candidate.identity = binding->identity;
		candidate.observation_version = binding->observation_version + 1;
		candidate.generations = binding->generations;
		candidate.observed_at_ms = now;
		candidate.expires_at_ms = now > std::numeric_limits<uint64_t>::max() - 500
						  ? std::numeric_limits<uint64_t>::max()
						  : now + 500;
		candidate.rect = ff_rect{x0, y0, x1 - x0, y1 - y0};
	}
	if (count) {
		const uint64_t version = binding->observation_version + 1;
		if (executor->enqueue_candidates(now, candidates.data(), count)) {
			binding->observation_version = version;
			std::lock_guard<std::mutex> lock(binding->preview_mutex);
			binding->preview.identity = binding->identity;
			binding->preview.generations = binding->generations;
			binding->preview.observation_version = version;
			binding->preview.published_at_ms = now;
			binding->preview.candidate_count = count;
			std::copy_n(candidates.begin(), count, binding->preview.candidates.begin());
		}
	} else {
		std::lock_guard<std::mutex> lock(binding->preview_mutex);
		binding->preview.identity = binding->identity;
		binding->preview.generations = binding->generations;
		binding->preview.observation_version = binding->observation_version;
		binding->preview.published_at_ms = now;
		binding->preview.candidate_count = 0;
	}
	executor->enqueue_tick(now);
}

static void settle_source_transition(faithfulframe_source_binding *binding)
{
	if (!binding || !binding->transition_pending.load(std::memory_order_acquire))
		return;
	auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
	if (!executor)
		return;
	ff_snapshot snapshot{};
	if (!executor->try_snapshot(snapshot) || snapshot.identity.source != binding->identity.source ||
	    snapshot.identity.session != binding->identity.session ||
	    snapshot.generations.source != binding->generations.source ||
	    snapshot.generations.session != binding->generations.session ||
	    snapshot.generations.stream != binding->generations.stream ||
	    snapshot.generations.geometry != binding->generations.geometry ||
	    snapshot.generations.tracker != binding->generations.tracker)
		return;
	binding->transition_pending.store(false, std::memory_order_release);
}

static void tick_safe_source(face_tracker_filter *s, float second)
{
	faithfulframe_source_runtime *runtime = s->safe_runtime;
	apply_safe_source_configuration(s, runtime);
	faithfulframe_source_binding *binding = runtime->active.load(std::memory_order_acquire);
	if (!binding) {
		s->target_valid = false;
		return;
	}

	obs_source_t *target = obs_weak_source_get_source(binding->target);
	if (!target) {
		auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
		if (executor && !binding->closed.load(std::memory_order_acquire)) {
			binding->transition_pending.store(true, std::memory_order_release);
			executor->request_shutdown(FF_LIFECYCLE_SOURCE_DESTROYED);
			binding->closed.store(true, std::memory_order_release);
		}
		s->target_valid = binding->output_width.load(std::memory_order_acquire) > 0 &&
				  binding->output_height.load(std::memory_order_acquire) > 0;
		return;
	}

	const uint32_t width = obs_source_get_width(target);
	const uint32_t height = obs_source_get_height(target);
	const bool connected = width > 0 && height > 0;
	const bool active = obs_source_active(target);
	const uint64_t now = source_monotonic_millis();
	auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);

	if (connected && !executor) {
		if (!initialize_source_session(s, binding, width, height)) {
			obs_source_release(target);
			s->target_valid = false;
			return;
		}
		binding->source_width.store(width, std::memory_order_release);
		binding->source_height.store(height, std::memory_order_release);
		binding->signal_connected.store(true, std::memory_order_release);
		binding->source_active.store(active, std::memory_order_release);
		executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
		s->ftm->reset_requested = true;
		if (!active)
			enqueue_source_lifecycle(binding, FF_LIFECYCLE_SCENE_INACTIVE, now);
	} else if (executor && connected != binding->signal_connected.load(std::memory_order_acquire)) {
		const uint32_t lifecycle = connected ? FF_LIFECYCLE_CAMERA_RESTORED : FF_LIFECYCLE_CAMERA_DISCONNECTED;
		if (enqueue_source_lifecycle(binding, lifecycle, now)) {
			binding->signal_connected.store(connected, std::memory_order_release);
			s->ftm->reset_requested = true;
		}
	}

	const uint32_t prior_source_width = binding->source_width.load(std::memory_order_acquire);
	const uint32_t prior_source_height = binding->source_height.load(std::memory_order_acquire);
	if (executor && connected && binding->signal_connected.load(std::memory_order_acquire) &&
	    (width != prior_source_width || height != prior_source_height)) {
		if (enqueue_source_lifecycle(binding, FF_LIFECYCLE_GEOMETRY_CHANGED, now, width, height)) {
			binding->source_width.store(width, std::memory_order_release);
			binding->source_height.store(height, std::memory_order_release);
			s->ftm->reset_requested = true;
		}
	}
	if (executor && active != binding->source_active.load(std::memory_order_acquire)) {
		const uint32_t lifecycle = active ? FF_LIFECYCLE_SCENE_ACTIVE : FF_LIFECYCLE_SCENE_INACTIVE;
		if (enqueue_source_lifecycle(binding, lifecycle, now)) {
			binding->source_active.store(active, std::memory_order_release);
			s->ftm->reset_requested = true;
		}
	}

	if (executor && !binding->closed.load(std::memory_order_acquire)) {
		const uint64_t tracker_revision =
			runtime->tracker_configuration_version.load(std::memory_order_acquire);
		if (tracker_revision != runtime->processed_tracker_configuration_version) {
			enqueue_source_lifecycle(binding, FF_LIFECYCLE_TRACKER_CHANGED, now);
			runtime->processed_tracker_configuration_version = tracker_revision;
			s->ftm->reset_requested = true;
		}
		const uint32_t source_width = binding->source_width.load(std::memory_order_acquire);
		const uint32_t source_height = binding->source_height.load(std::memory_order_acquire);
		s->known_width = source_width;
		s->known_height = source_height;
		s->width_with_aspect = binding->output_width.load(std::memory_order_acquire);
		s->height_with_aspect = binding->output_height.load(std::memory_order_acquire);
		s->ftm->crop_cur = rectf_s{0.0f, 0.0f, (float)source_width, (float)source_height};
		s->ftm->tick(second);
		if (s->ftm->detect) {
			binding->detector_last_work_us.store(s->ftm->detect->last_work_duration_us(),
							     std::memory_order_relaxed);
			binding->detector_max_work_us.store(s->ftm->detect->max_work_duration_us(),
							    std::memory_order_relaxed);
			binding->detector_work_count.store(s->ftm->detect->completed_work_count(),
							   std::memory_order_relaxed);
		} else {
			binding->detector_last_work_us.store(0, std::memory_order_relaxed);
			binding->detector_max_work_us.store(0, std::memory_order_relaxed);
			binding->detector_work_count.store(0, std::memory_order_relaxed);
		}
		publish_source_candidates(s, binding, now);
	} else {
		s->known_width = binding->source_width.load(std::memory_order_acquire);
		s->known_height = binding->source_height.load(std::memory_order_acquire);
		s->width_with_aspect = binding->output_width.load(std::memory_order_acquire);
		s->height_with_aspect = binding->output_height.load(std::memory_order_acquire);
		s->ftm->tracker_rects.clear();
	}
	settle_source_transition(binding);
	s->target_valid = binding->output_width.load(std::memory_order_acquire) > 0 &&
			  binding->output_height.load(std::memory_order_acquire) > 0;
	obs_source_release(target);
}
#endif

static void fts_tick(void *data, float second)
{
	auto *s = (struct face_tracker_filter *)data;
	const bool was_rendered = s->rendered;
	s->rendered = false;
	s->target_valid = false;

#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime) {
		obs_data_t *settings = nullptr;
		{
			std::lock_guard<std::mutex> lock(s->safe_runtime->pending_settings_mutex);
			settings = s->safe_runtime->pending_settings;
			s->safe_runtime->pending_settings = nullptr;
		}
		if (settings) {
			ftf_update(s, settings);
			obs_data_release(settings);
		}
		apply_safe_source_configuration(s, s->safe_runtime);
	}
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		tick_safe_source(s, second);
		return;
	}
#endif

	s->ftm->tick(second);

	std::string configured_target_name;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime) {
		std::lock_guard<std::mutex> lock(s->safe_runtime->configuration_mutex);
		configured_target_name = s->safe_runtime->requested_target_name;
	} else
#endif
		configured_target_name = s->target_name ? s->target_name : "";

	obs_source_t *target = s->target_ref ? obs_weak_source_get_source(s->target_ref) : nullptr;
	const char *name = target ? obs_source_get_name(target) : nullptr;

	if (!configured_target_name.empty() && (!target || !name || strcmp(name, configured_target_name.c_str()))) {
		obs_source_release(target);
		obs_weak_source_release(s->target_ref);
		target = obs_get_source_by_name(configured_target_name.c_str());
		s->target_ref = target ? obs_source_get_weak_source(target) : nullptr;
		blog(LOG_INFO, "fts_tick: target=%p", target);
	} else if (configured_target_name.empty() && s->target_ref) {
		obs_source_release(target);
		obs_weak_source_release(s->target_ref);
		s->target_ref = nullptr;
		target = nullptr;
	}
	if (!target)
		return;

	s->known_width = obs_source_get_width(target);
	s->known_height = obs_source_get_height(target);

	ft_tick_internal(s, second, was_rendered);

	obs_source_release(target);
}

static inline void render_target(struct face_tracker_filter *s, obs_source_t *target, obs_source_t *parent)
{
	if (!s->texrender)
		s->texrender = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	const uint32_t cx = s->known_width, cy = s->known_height;
	gs_texrender_reset(s->texrender);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);

	if (gs_texrender_begin(s->texrender, cx, cy)) {
		uint32_t parent_flags = obs_source_get_output_flags(target);
		bool custom_draw = (parent_flags & OBS_SOURCE_CUSTOM_DRAW) != 0;
		bool async = (parent_flags & OBS_SOURCE_ASYNC) != 0;
		struct vec4 clear_color;

		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 0.0f, 0);
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);

		if (target == parent && !custom_draw && !async)
			obs_source_default_render(target);
		else
			obs_source_video_render(target);

		gs_texrender_end(s->texrender);
	}

	gs_blend_state_pop();

	s->ftm->release_cvtex();
}

static inline f3 ensure_range(f3 u, const struct face_tracker_filter *s)
{
	if (isnan(u.v[2]))
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] < s->range_min.v[2])
		u.v[2] = s->range_min.v[2];
	else if (u.v[2] > s->range_max.v[2])
		u.v[2] = s->range_max.v[2];

	rectf_s r = f3_to_rectf(u, s->width_with_aspect, s->height_with_aspect);

	if (r.x0 < 0)
		u.v[0] += -r.x0;
	else if (r.x1 > s->known_width)
		u.v[0] -= r.x1 - s->known_width;

	if (r.y0 < 0)
		u.v[1] += -r.y0;
	else if (r.y1 > s->known_height)
		u.v[1] -= r.y1 - s->known_height;

	return u;
}

static inline void calculate_error(struct face_tracker_filter *s)
{
	f3 e_tot(0.0f, 0.0f, 0.0f);
	float sc_tot = 0.0f;
	bool found = false;
	auto &tracker_rects = s->ftm->tracker_rects;
	for (size_t i = 0; i < tracker_rects.size(); i++) {
		f3 r(tracker_rects[i].rect);
		float score = tracker_rects[i].rect.score;

		if (s->ftm->landmark_detection_data) {
			pointf_s center = landmark_center(tracker_rects[i].landmark);
			float area = landmark_area(tracker_rects[i].landmark);
			if (area <= 0.0f)
				continue;

			r.v[0] = center.x;
			r.v[1] = center.y;
			r.v[2] = sqrtf(area * (float)(4.0f / M_PI));
		}

		if (s->debug_data_tracker) {
			fprintf(s->debug_data_tracker, "%f\t%f\t%f\t%f\t%f\n", os_gettime_ns() * 1e-9, r.v[0], r.v[1],
				r.v[2], score);
		}

		r.v[0] -= get_width(tracker_rects[i].crop_rect) * s->track_x;
		r.v[1] += get_height(tracker_rects[i].crop_rect) * s->track_y;
		r.v[2] /= s->track_z;
		r = ensure_range(r, s);
		f3 w(tracker_rects[i].crop_rect);

		f3 e = (r - w) * score;
		if (score > 0.0f && !isnan(e)) {
			e_tot += e;
			sc_tot += score;
			found = true;
		}
	}

	if (found)
		s->detect_err = e_tot * (1.0f / sc_tot);
	else
		s->detect_err = f3(0, 0, 0);

	if (s->debug_data_error) {
		fprintf(s->debug_data_error, "%f\t%f\t%f\t%f\n", os_gettime_ns() * 1e-9, s->detect_err.v[0],
			s->detect_err.v[1], s->detect_err.v[2]);
	}
}

static inline void draw_sprite_crop(float width, float height, float x0, float y0, float x1, float y1);

static inline void scale_texture(struct face_tracker_filter *s, float scale)
{
	if (!s->texrender_scaled)
		s->texrender_scaled = gs_texrender_create(GS_BGRA, GS_ZS_NONE);
	const uint32_t cx = s->known_width / scale, cy = s->known_height / scale;
	gs_texrender_reset(s->texrender_scaled);
	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	if (gs_texrender_begin(s->texrender_scaled, cx, cy)) {
		gs_ortho(0.0f, (float)cx, 0.0f, (float)cy, -100.0f, 100.0f);
		gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
		auto effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		if (tex && effect) {
			gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
			gs_effect_set_texture(image, tex);
			while (gs_effect_loop(effect, "Draw"))
				draw_sprite_crop(cx, cy, 0, 0, 1, 1);
		}
		gs_texrender_end(s->texrender_scaled);
	}
	gs_blend_state_pop();
}

static inline int stage_to_surface(struct face_tracker_filter *s, float scale)
{
	uint32_t width = s->known_width / scale;
	uint32_t height = s->known_height / scale;
	if (width <= 0 || height <= 0)
		return 1;

	gs_texture_t *tex = gs_texrender_get_texture(s->texrender_scaled);
	if (!tex)
		return 2;

	if (!s->stagesurface || width != gs_stagesurface_get_width(s->stagesurface) ||
	    height != gs_stagesurface_get_height(s->stagesurface)) {
		gs_stagesurface_destroy(s->stagesurface);
		s->stagesurface = gs_stagesurface_create(width, height, GS_BGRA);
	}

	gs_stage_texture(s->stagesurface, tex);

	return 0;
}

static inline std::shared_ptr<texture_object> surface_to_cvtex(struct face_tracker_filter *s, float scale)
{
	uint8_t *video_data = NULL;
	uint32_t video_linesize;
	if (!gs_stagesurface_map(s->stagesurface, &video_data, &video_linesize))
		return NULL;

	uint32_t width = gs_stagesurface_get_width(s->stagesurface);
	uint32_t height = gs_stagesurface_get_height(s->stagesurface);

	std::shared_ptr<texture_object> cvtex(new texture_object);
	cvtex->scale = scale;
	cvtex->tick = s->ftm->tick_cnt;

	struct obs_source_frame frame;
	memset(&frame, 0, sizeof(frame));
	frame.data[0] = video_data;
	frame.linesize[0] = video_linesize;
	frame.width = width;
	frame.height = height;
	frame.format = VIDEO_FORMAT_BGRA;
	cvtex->set_texture_obsframe(&frame, 1);

	gs_stagesurface_unmap(s->stagesurface);

	return cvtex;
}

static inline void draw_sprite_crop(float width, float height, float x0, float y0, float x1, float y1)
{
	gs_render_start(false);
	gs_vertex2f(0.0f, 0.0f);
	gs_vertex2f(width, 0.0f);
	gs_vertex2f(0.0f, height);
	gs_vertex2f(width, height);
	struct vec2 tv;
	vec2_set(&tv, x0, y0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x1, y0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x0, y1);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, x1, y1);
	gs_texcoord2v(&tv, 0);
	gs_render_stop(GS_TRISTRIP);
}

static inline void draw_sprite_crop_to_rect(float x0, float y0, float x1, float y1, float u0, float v0, float u1,
					    float v1)
{
	gs_render_start(false);
	gs_vertex2f(x0, y0);
	gs_vertex2f(x1, y0);
	gs_vertex2f(x0, y1);
	gs_vertex2f(x1, y1);
	struct vec2 tv;
	vec2_set(&tv, u0, v0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, u1, v0);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, u0, v1);
	gs_texcoord2v(&tv, 0);
	vec2_set(&tv, u1, v1);
	gs_texcoord2v(&tv, 0);
	gs_render_stop(GS_TRISTRIP);
}

#ifdef ENABLE_RUST_CORE_BRIDGE
static bool valid_normalized_rect(const ff_rect &rect)
{
	return std::isfinite(rect.x) && std::isfinite(rect.y) && std::isfinite(rect.width) &&
	       std::isfinite(rect.height) && rect.x >= 0.0f && rect.y >= 0.0f && rect.width > 0.0f &&
	       rect.height > 0.0f && rect.x + rect.width <= 1.0f && rect.y + rect.height <= 1.0f;
}

static ff_rect full_source_viewport(uint32_t source_width, uint32_t source_height, uint32_t output_width,
				    uint32_t output_height)
{
	if (!source_width || !source_height || !output_width || !output_height)
		return ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
	const float source_aspect = (float)source_width / source_height;
	const float output_aspect = (float)output_width / output_height;
	if (source_aspect > output_aspect) {
		const float height = output_aspect / source_aspect;
		return ff_rect{0.0f, (1.0f - height) * 0.5f, 1.0f, height};
	}
	if (source_aspect < output_aspect) {
		const float width = source_aspect / output_aspect;
		return ff_rect{(1.0f - width) * 0.5f, 0.0f, width, 1.0f};
	}
	return ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
}

static void draw_safe_source_frame(face_tracker_filter *s, faithfulframe_source_binding *binding,
				   faithfulframe_source_runtime *runtime = nullptr, bool force_black = false)
{
	const uint32_t output_width = binding   ? binding->output_width.load(std::memory_order_acquire)
				      : runtime ? runtime->last_output_width.load(std::memory_order_acquire)
						: s->width_with_aspect;
	const uint32_t output_height = binding   ? binding->output_height.load(std::memory_order_acquire)
				       : runtime ? runtime->last_output_height.load(std::memory_order_acquire)
						 : s->height_with_aspect;
	const uint32_t source_width = force_black ? 0
				      : binding   ? binding->source_width.load(std::memory_order_acquire)
						  : s->known_width;
	const uint32_t source_height = force_black ? 0
				       : binding   ? binding->source_height.load(std::memory_order_acquire)
						   : s->known_height;
	if (!output_width || !output_height)
		return;

	ff_rect crop{0.0f, 0.0f, 1.0f, 1.0f};
	ff_rect viewport = full_source_viewport(source_width, source_height, output_width, output_height);
	if (!force_black && binding && !binding->closed.load(std::memory_order_acquire) &&
	    !binding->transition_pending.load(std::memory_order_acquire)) {
		auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire);
		ff_snapshot snapshot{};
		uint64_t snapshot_time = 0;
		const uint64_t now = source_monotonic_millis();
		if (executor && executor->try_snapshot(snapshot, &snapshot_time) && snapshot_time <= now &&
		    now - snapshot_time <= 500 && snapshot.identity.source == binding->identity.source &&
		    snapshot.identity.session == binding->identity.session && !snapshot.closed) {
			const bool selected_crop = snapshot.crop_action != FF_CROP_TRACK ||
						   (snapshot.state == FF_STATE_LOCKED && snapshot.has_selected);
			if (selected_crop && valid_normalized_rect(snapshot.crop) &&
			    valid_normalized_rect(snapshot.viewport)) {
				crop = snapshot.crop;
				viewport = snapshot.viewport;
			}
		}
	}
	if (!valid_normalized_rect(crop) || !valid_normalized_rect(viewport)) {
		crop = ff_rect{0.0f, 0.0f, 1.0f, 1.0f};
		viewport = full_source_viewport(source_width, source_height, output_width, output_height);
	}

	gs_blend_state_push();
	gs_blend_function(GS_BLEND_ONE, GS_BLEND_ZERO);
	gs_effect_t *solid = obs_get_base_effect(OBS_EFFECT_SOLID);
	if (solid) {
		gs_effect_set_color(gs_effect_get_param_by_name(solid, "color"), 0x000000FF);
		while (gs_effect_loop(solid, "Solid")) {
			gs_render_start(false);
			gs_vertex2f(0.0f, 0.0f);
			gs_vertex2f((float)output_width, 0.0f);
			gs_vertex2f(0.0f, (float)output_height);
			gs_vertex2f((float)output_width, (float)output_height);
			gs_render_stop(GS_TRISTRIP);
		}
	}

	const bool source_available = !force_black && binding && !binding->closed.load(std::memory_order_acquire) &&
				      binding->signal_connected.load(std::memory_order_acquire) &&
				      binding->source_active.load(std::memory_order_acquire);
	gs_texture_t *texture = source_available && s->texrender ? gs_texrender_get_texture(s->texrender) : nullptr;
	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	if (texture && effect && source_width && source_height) {
		gs_effect_set_texture(gs_effect_get_param_by_name(effect, "image"), texture);
		const float dx0 = viewport.x * output_width;
		const float dy0 = viewport.y * output_height;
		const float dx1 = (viewport.x + viewport.width) * output_width;
		const float dy1 = (viewport.y + viewport.height) * output_height;
		const float u0 = crop.x;
		const float v0 = crop.y;
		const float u1 = crop.x + crop.width;
		const float v1 = crop.y + crop.height;
		while (gs_effect_loop(effect, "Draw"))
			draw_sprite_crop_to_rect(dx0, dy0, dx1, dy1, u0, v0, u1, v1);
	}
	gs_blend_state_pop();
}
#endif

static inline void draw_frame_texture(struct face_tracker_filter *s, bool debug_notrack)
{
	uint32_t width = s->width_with_aspect;
	uint32_t height = s->height_with_aspect;
	const rectf_s &crop_cur = s->ftm->crop_cur;

	// TODO: linear_srgb, 27 only?

	if (width <= 0 || height <= 0)
		return;

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *image = gs_effect_get_param_by_name(effect, "image");
	gs_texture_t *tex = gs_texrender_get_texture(s->texrender);
	if (!tex)
		return;
	gs_effect_set_texture(image, tex);

	while (gs_effect_loop(effect, "Draw")) {
		if (debug_notrack)
			gs_draw_sprite(tex, 0, s->known_width, s->known_height);
		else
			draw_sprite_crop(width, height, crop_cur.x0 / s->known_width, crop_cur.y0 / s->known_height,
					 crop_cur.x1 / s->known_width, crop_cur.y1 / s->known_height);
	}
}

static inline void draw_frame_info(struct face_tracker_filter *s, bool debug_notrack, bool landmark_only = false)
{
	const rectf_s &crop_cur = s->ftm->crop_cur;
	bool draw_det = !landmark_only;
	bool draw_trk = !landmark_only;
	bool draw_lmk = true;
	bool draw_ref = !landmark_only;

	if (!debug_notrack) {
		uint32_t width = s->width_with_aspect;
		uint32_t height = s->height_with_aspect;
		const float scale =
			sqrtf((float)(width * height) / ((crop_cur.x1 - crop_cur.x0) * (crop_cur.y1 - crop_cur.y0)));

		gs_matrix_push();
		struct matrix4 tr;
		matrix4_identity(&tr);
		matrix4_translate3f(&tr, &tr, -(crop_cur.x0 + crop_cur.x1) * 0.5f, -(crop_cur.y0 + crop_cur.y1) * 0.5f,
				    0.0f);
		matrix4_scale3f(&tr, &tr, scale, scale, 1.0f);
		matrix4_translate3f(&tr, &tr, width / 2, height / 2, 0.0f);
		gs_matrix_mul(&tr);
	}

	gs_effect_t *effect = obs_get_base_effect(OBS_EFFECT_SOLID);
	while (gs_effect_loop(effect, "Solid")) {
		if (draw_det) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF0000FF);
			for (size_t i = 0; i < s->ftm->detect_rects.size(); i++)
				draw_rect_upsize(s->ftm->detect_rects[i], s->ftm->upsize_l, s->ftm->upsize_r,
						 s->ftm->upsize_t, s->ftm->upsize_b);
		}

		gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFF00FF00);
		for (size_t i = 0; i < s->ftm->tracker_rects.size(); i++) {
			const auto &tr = s->ftm->tracker_rects[i];
			if (draw_trk)
				draw_rect_upsize(tr.rect);
			if (draw_lmk && tr.landmark.size())
				draw_landmark(tr.landmark);
		}
		if (debug_notrack && draw_ref) {
			gs_effect_set_color(gs_effect_get_param_by_name(effect, "color"), 0xFFFFFF00); // amber
			const rectf_s &r = s->ftm->crop_cur;
			gs_render_start(false);
			gs_vertex2f(r.x0, r.y0);
			gs_vertex2f(r.x0, r.y1);
			gs_vertex2f(r.x0, r.y1);
			gs_vertex2f(r.x1, r.y1);
			gs_vertex2f(r.x1, r.y1);
			gs_vertex2f(r.x1, r.y0);
			gs_vertex2f(r.x1, r.y0);
			gs_vertex2f(r.x0, r.y0);
			const float srwhr2 = sqrtf((r.x1 - r.x0) * (r.y1 - r.y0)) * 0.5f;
			const float rcx = (r.x0 + r.x1) * 0.5f + (r.x1 - r.x0) * s->track_x;
			const float rcy = (r.y0 + r.y1) * 0.5f - (r.y1 - r.y0) * s->track_y;
			gs_vertex2f(rcx - srwhr2 * s->track_z, rcy);
			gs_vertex2f(rcx + srwhr2 * s->track_z, rcy);
			gs_vertex2f(rcx, rcy - srwhr2 * s->track_z);
			gs_vertex2f(rcx, rcy + srwhr2 * s->track_z);
			gs_render_stop(GS_LINES);

			if (s->ftm->landmark_detection_data) {
				gs_render_start(false);
				float r = srwhr2 * s->track_z;
				for (int i = 0; i <= 32; i++)
					gs_vertex2f(rcx + r * sinf(M_PI * i / 8), rcy + r * cosf(M_PI * i / 8));
				gs_render_stop(GS_LINESTRIP);
			}
		}
	}

	if (!debug_notrack)
		gs_matrix_pop();
}

static inline void draw_frame(struct face_tracker_filter *s)
{
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);

	draw_frame_texture(s, debug_notrack);

	if (s->debug_faces && (!s->is_active || s->debug_always_show))
		draw_frame_info(s, debug_notrack);
}

static void ftf_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter *)data;
	if (!s->target_valid) {
		obs_source_skip_video_filter(s->context);
		return;
	}
	obs_source_t *target = obs_filter_get_target(s->context);
	obs_source_t *parent = obs_filter_get_parent(s->context);

	if (!target || !parent) {
		obs_source_skip_video_filter(s->context);
		return;
	}

	if (!s->rendered) {
		render_target(s, target, parent);
		if (is_running(s))
			s->ftm->post_render();
		s->rendered = true;
	}

	draw_frame(s);
}

static void fts_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		if (s->safe_runtime->reconfiguration_pending.load(std::memory_order_acquire)) {
			draw_safe_source_frame(s, s->safe_runtime->active.load(std::memory_order_acquire),
					       s->safe_runtime, true);
			return;
		}
		faithfulframe_source_binding *binding = s->safe_runtime->active.load(std::memory_order_acquire);
		if (!binding)
			return;
		obs_source_t *safe_target = binding && binding->target ? obs_weak_source_get_source(binding->target)
								       : nullptr;
		if (!s->rendered) {
			s->rendered = true;
			if (safe_target && binding && binding->signal_connected.load(std::memory_order_acquire) &&
			    binding->source_active.load(std::memory_order_acquire)) {
				render_target(s, safe_target, NULL);
				s->ftm->post_render();
			}
		}
		draw_safe_source_frame(s, binding, s->safe_runtime);
		obs_source_release(safe_target);
		return;
	}
#endif
	if (!s->target_valid)
		return;

	obs_source_t *target = obs_weak_source_get_source(s->target_ref);

	if (!target)
		return;

	if (!s->rendered) {
		s->rendered = true;
		render_target(s, target, NULL);
		if (is_running(s))
			s->ftm->post_render();
	}

	draw_frame(s);

	obs_source_release(target);
}

static uint32_t ftf_width(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		auto *binding = s->safe_runtime->active.load(std::memory_order_acquire);
		const uint32_t width = binding ? binding->output_width.load(std::memory_order_acquire)
					       : s->safe_runtime->last_output_width.load(std::memory_order_acquire);
		return width;
	}
#endif
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);
	return debug_notrack ? s->known_width : s->width_with_aspect;
}

static uint32_t ftf_height(void *data)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		auto *binding = s->safe_runtime->active.load(std::memory_order_acquire);
		const uint32_t height = binding ? binding->output_height.load(std::memory_order_acquire)
						: s->safe_runtime->last_output_height.load(std::memory_order_acquire);
		return height;
	}
#endif
	const bool debug_notrack = s->debug_notrack && (!s->is_active || s->debug_always_show);
	return debug_notrack ? s->known_height : s->height_with_aspect;
}

static void cb_render_frame(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;

	bool debug_notrack = false;
	calldata_get_bool(cd, "notrack", &debug_notrack);

	draw_frame_texture(s, debug_notrack);
}

static void cb_render_info(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;

	bool debug_notrack = false;
	calldata_get_bool(cd, "notrack", &debug_notrack);

	bool landmark_only = false;
	calldata_get_bool(cd, "landmark_only", &landmark_only);

	draw_frame_info(s, debug_notrack, landmark_only);
}

static void cb_get_target_size(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
	calldata_set_int(cd, "width", (int)s->known_width);
	calldata_set_int(cd, "height", (int)s->known_height);
}

static void cb_get_state(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire)) {
		bool paused = false;
		if (auto *binding = s->safe_runtime->active.load(std::memory_order_acquire)) {
			if (auto executor = std::atomic_load_explicit(&binding->executor, std::memory_order_acquire)) {
				ff_snapshot snapshot{};
				if (executor->try_snapshot(snapshot))
					paused = snapshot.state == FF_STATE_FROZEN;
			}
		}
		calldata_set_bool(cd, "paused", paused);
		return;
	}
#endif
	calldata_set_bool(cd, "paused", s->is_paused);
}

static void cb_set_state(void *data, calldata_t *cd)
{
	auto *s = (struct face_tracker_filter *)data;
#ifdef ENABLE_RUST_CORE_BRIDGE
	// Legacy callers do not carry session identity or version tokens. Once the
	// safety policy is enabled, those commands must not mutate its state.
	if (s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire))
		return;
#endif
	bool is_paused = s->is_paused;
	calldata_get_bool(cd, "paused", &is_paused);
	if (is_paused != s->is_paused) {
		s->is_paused = is_paused;
		emit_state_changed(s);
	}

	bool reset = false;
	calldata_get_bool(cd, "reset", &reset);
	if (reset)
		ftf_reset_tracking(NULL, NULL, s);
}

#ifdef ENABLE_RUST_CORE_BRIDGE
static long long ffi_integer(uint64_t value)
{
	return value <= static_cast<uint64_t>(std::numeric_limits<long long>::max()) ? static_cast<long long>(value)
										     : 0;
}

static void set_generation_fields(calldata_t *cd, const ff_generations &generations, const char *prefix)
{
	const std::string key_prefix = prefix ? prefix : "";
	calldata_set_int(cd, (key_prefix + "source_generation").c_str(), ffi_integer(generations.source));
	calldata_set_int(cd, (key_prefix + "session_generation").c_str(), ffi_integer(generations.session));
	calldata_set_int(cd, (key_prefix + "stream_generation").c_str(), ffi_integer(generations.stream));
	calldata_set_int(cd, (key_prefix + "geometry_generation").c_str(), ffi_integer(generations.geometry));
	calldata_set_int(cd, (key_prefix + "tracker_generation").c_str(), ffi_integer(generations.tracker));
}

static void cb_get_faithfulframe_preview(void *data, calldata_t *cd)
{
	auto *s = static_cast<face_tracker_filter *>(data);
	const bool enabled = s->safe_runtime && s->safe_runtime->enabled.load(std::memory_order_acquire);
	calldata_set_bool(cd, "safe_enabled", enabled);
	calldata_set_bool(cd, "available", false);
	calldata_set_int(cd, "candidate_count", 0);
	calldata_set_int(cd, "status_snapshot_age_ms", -1);
	if (!enabled)
		return;
	if (s->safe_runtime->reconfiguration_pending.load(std::memory_order_acquire))
		return;

	auto *binding = s->safe_runtime->active.load(std::memory_order_acquire);
	auto executor = binding ? std::atomic_load_explicit(&binding->executor, std::memory_order_acquire)
				: std::shared_ptr<faithfulframe_session_executor>{};
	if (!binding || !executor || binding->closed.load(std::memory_order_acquire) ||
	    binding->transition_pending.load(std::memory_order_acquire))
		return;
	const ff_executor_diagnostics diagnostics = executor->diagnostics();
	calldata_set_int(cd, "executor_queue_depth", ffi_integer(diagnostics.queue_depth));
	calldata_set_int(cd, "executor_queue_high_water", ffi_integer(diagnostics.queue_high_water));
	calldata_set_int(cd, "executor_accepted_work", ffi_integer(diagnostics.accepted_work));
	calldata_set_int(cd, "executor_rejected_work", ffi_integer(diagnostics.rejected_work));
	calldata_set_int(cd, "executor_coalesced_work", ffi_integer(diagnostics.coalesced_work));
	calldata_set_int(cd, "executor_last_queue_wait_us", ffi_integer(diagnostics.last_queue_wait_us));
	calldata_set_int(cd, "executor_max_queue_wait_us", ffi_integer(diagnostics.max_queue_wait_us));
	calldata_set_int(cd, "executor_last_processing_us", ffi_integer(diagnostics.last_processing_us));
	calldata_set_int(cd, "executor_max_processing_us", ffi_integer(diagnostics.max_processing_us));
	calldata_set_int(cd, "detector_last_work_us",
			 ffi_integer(binding->detector_last_work_us.load(std::memory_order_relaxed)));
	calldata_set_int(cd, "detector_max_work_us",
			 ffi_integer(binding->detector_max_work_us.load(std::memory_order_relaxed)));
	calldata_set_int(cd, "detector_work_count",
			 ffi_integer(binding->detector_work_count.load(std::memory_order_relaxed)));

	ff_snapshot snapshot{};
	uint64_t snapshot_at = 0;
	const uint64_t now = source_monotonic_millis();
	const bool has_snapshot = executor->try_snapshot(snapshot, &snapshot_at);
	if (has_snapshot && snapshot_at <= now)
		calldata_set_int(cd, "status_snapshot_age_ms", ffi_integer(now - snapshot_at));
	if (!has_snapshot || snapshot.closed || snapshot_at > now || now - snapshot_at > 1000 ||
	    snapshot.identity.source != binding->identity.source ||
	    snapshot.identity.session != binding->identity.session || !valid_normalized_rect(snapshot.crop) ||
	    !valid_normalized_rect(snapshot.viewport))
		return;

	faithfulframe_source_binding::preview_payload preview;
	{
		std::lock_guard<std::mutex> lock(binding->preview_mutex);
		preview = binding->preview;
	}
	const bool candidates_current = preview.identity.source == snapshot.identity.source &&
					preview.identity.session == snapshot.identity.session &&
					preview.generations.source == snapshot.generations.source &&
					preview.generations.session == snapshot.generations.session &&
					preview.generations.stream == snapshot.generations.stream &&
					preview.generations.geometry == snapshot.generations.geometry &&
					preview.generations.tracker == snapshot.generations.tracker &&
					preview.observation_version == snapshot.observation_version &&
					preview.published_at_ms <= now && now - preview.published_at_ms <= 500;

	calldata_set_bool(cd, "available", true);
	calldata_set_int(cd, "source_id", ffi_integer(snapshot.identity.source));
	calldata_set_int(cd, "session_id", ffi_integer(snapshot.identity.session));
	calldata_set_int(cd, "status_version", ffi_integer(snapshot.status_version));
	calldata_set_int(cd, "observation_version", ffi_integer(snapshot.observation_version));
	set_generation_fields(cd, snapshot.generations, "");
	calldata_set_int(cd, "state", snapshot.state);
	calldata_set_int(cd, "reason", snapshot.reason);
	calldata_set_int(cd, "crop_action", snapshot.crop_action);
	calldata_set_int(cd, "command_result", snapshot.command_result);
	calldata_set_int(cd, "rejection_reason", snapshot.rejection_reason);
	calldata_set_int(cd, "selected_handle", ffi_integer(snapshot.selected_handle));
	calldata_set_int(cd, "source_width", binding->source_width.load(std::memory_order_acquire));
	calldata_set_int(cd, "source_height", binding->source_height.load(std::memory_order_acquire));
	calldata_set_int(cd, "output_width", binding->output_width.load(std::memory_order_acquire));
	calldata_set_int(cd, "output_height", binding->output_height.load(std::memory_order_acquire));
	calldata_set_float(cd, "crop_x", snapshot.crop.x);
	calldata_set_float(cd, "crop_y", snapshot.crop.y);
	calldata_set_float(cd, "crop_width", snapshot.crop.width);
	calldata_set_float(cd, "crop_height", snapshot.crop.height);
	calldata_set_float(cd, "viewport_x", snapshot.viewport.x);
	calldata_set_float(cd, "viewport_y", snapshot.viewport.y);
	calldata_set_float(cd, "viewport_width", snapshot.viewport.width);
	calldata_set_float(cd, "viewport_height", snapshot.viewport.height);
	calldata_set_string(cd, "target_name", binding->target_name.c_str());
	calldata_set_bool(cd, "target_active", binding->source_active.load(std::memory_order_acquire));
	calldata_set_bool(cd, "signal_connected", binding->signal_connected.load(std::memory_order_acquire));

	if (!candidates_current)
		return;
	const size_t count = std::min(preview.candidate_count, preview.candidates.size());
	calldata_set_int(cd, "candidate_count", static_cast<long long>(count));
	calldata_set_int(cd, "candidate_source_id", ffi_integer(preview.identity.source));
	calldata_set_int(cd, "candidate_session_id", ffi_integer(preview.identity.session));
	calldata_set_int(cd, "candidate_observation_version", ffi_integer(preview.observation_version));
	set_generation_fields(cd, preview.generations, "candidate_");
	for (size_t i = 0; i < count; ++i) {
		const ff_candidate &candidate = preview.candidates[i];
		if (candidate.expires_at_ms < now || !valid_normalized_rect(candidate.rect))
			continue;
		const std::string suffix = std::to_string(i);
		calldata_set_int(cd, ("candidate_handle_" + suffix).c_str(), ffi_integer(candidate.handle));
		calldata_set_int(cd, ("candidate_expires_" + suffix).c_str(), ffi_integer(candidate.expires_at_ms));
		calldata_set_float(cd, ("candidate_x_" + suffix).c_str(), candidate.rect.x);
		calldata_set_float(cd, ("candidate_y_" + suffix).c_str(), candidate.rect.y);
		calldata_set_float(cd, ("candidate_width_" + suffix).c_str(), candidate.rect.width);
		calldata_set_float(cd, ("candidate_height_" + suffix).c_str(), candidate.rect.height);
	}
}

static bool get_unsigned_field(calldata_t *cd, const char *name, uint64_t &value)
{
	long long raw = 0;
	if (!calldata_get_int(cd, name, &raw) || raw < 0)
		return false;
	value = static_cast<uint64_t>(raw);
	return true;
}

static bool enqueue_safe_operator_command(face_tracker_filter *s, uint32_t kind)
{
	if (!s || !s->safe_runtime || !s->safe_runtime->enabled.load(std::memory_order_acquire) ||
	    s->safe_runtime->reconfiguration_pending.load(std::memory_order_acquire) || kind > FF_COMMAND_RESET_TO_WIDE)
		return false;
	auto *binding = s->safe_runtime->active.load(std::memory_order_acquire);
	auto executor = binding ? std::atomic_load_explicit(&binding->executor, std::memory_order_acquire)
				: std::shared_ptr<faithfulframe_session_executor>{};
	if (!binding || !executor || binding->closed.load(std::memory_order_acquire))
		return false;
	ff_snapshot snapshot{};
	if (!executor->try_snapshot(snapshot) || snapshot.closed ||
	    snapshot.identity.source != binding->identity.source ||
	    snapshot.identity.session != binding->identity.session)
		return false;
	ff_command command{};
	command.kind = kind;
	command.identity = snapshot.identity;
	command.status_version = snapshot.status_version;
	command.generations = snapshot.generations;
	command.observation_version = snapshot.observation_version;
	return executor->enqueue_command(source_monotonic_millis(), command);
}

static void cb_faithfulframe_command(void *data, calldata_t *cd)
{
	auto *s = static_cast<face_tracker_filter *>(data);
	calldata_set_bool(cd, "queued", false);
	if (!s->safe_runtime || !s->safe_runtime->enabled.load(std::memory_order_acquire) ||
	    s->safe_runtime->reconfiguration_pending.load(std::memory_order_acquire))
		return;
	auto *binding = s->safe_runtime->active.load(std::memory_order_acquire);
	auto executor = binding ? std::atomic_load_explicit(&binding->executor, std::memory_order_acquire)
				: std::shared_ptr<faithfulframe_session_executor>{};
	if (!binding || !executor || binding->closed.load(std::memory_order_acquire))
		return;

	uint64_t kind = 0, source_id = 0, session_id = 0, status_version = 0;
	uint64_t source_generation = 0, session_generation = 0, stream_generation = 0;
	uint64_t geometry_generation = 0, tracker_generation = 0, candidate_handle = 0;
	uint64_t observation_version = 0;
	if (!get_unsigned_field(cd, "kind", kind) || !get_unsigned_field(cd, "source_id", source_id) ||
	    !get_unsigned_field(cd, "session_id", session_id) ||
	    !get_unsigned_field(cd, "status_version", status_version) ||
	    !get_unsigned_field(cd, "source_generation", source_generation) ||
	    !get_unsigned_field(cd, "session_generation", session_generation) ||
	    !get_unsigned_field(cd, "stream_generation", stream_generation) ||
	    !get_unsigned_field(cd, "geometry_generation", geometry_generation) ||
	    !get_unsigned_field(cd, "tracker_generation", tracker_generation) ||
	    !get_unsigned_field(cd, "candidate_handle", candidate_handle) ||
	    !get_unsigned_field(cd, "observation_version", observation_version) || kind > FF_COMMAND_RESET_TO_WIDE ||
	    !source_id || !session_id || !status_version)
		return;

	ff_command command{};
	command.kind = static_cast<uint32_t>(kind);
	command.identity = ff_identity{source_id, session_id};
	command.status_version = status_version;
	command.generations = ff_generations{source_generation, session_generation, stream_generation,
					     geometry_generation, tracker_generation};
	command.candidate_handle = candidate_handle;
	command.observation_version = observation_version;
	if (executor->enqueue_command(source_monotonic_millis(), command))
		calldata_set_bool(cd, "queued", true);
}
#endif

static void emit_state_changed(struct face_tracker_filter *s)
{
	struct calldata cd;
	uint8_t stack[128];

	calldata_init_fixed(&cd, stack, sizeof(stack));
	calldata_set_ptr(&cd, "source", s->context);
	cb_get_state(s, &cd);

	signal_handler_t *sh = obs_source_get_signal_handler(s->context);
	signal_handler_signal(sh, "state_changed", &cd);
}

extern "C" void register_face_tracker_filter(bool hide_filter, bool hide_source)
{
	struct obs_source_info info = {};
	info.id = "face_tracker_filter";
	info.type = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	if (hide_filter)
		info.output_flags |= OBS_SOURCE_CAP_DISABLED;
	info.get_name = ftf_get_name;
	info.create = ftf_create;
	info.destroy = ftf_destroy;
	info.update = ftf_update;
	info.get_properties = ftf_properties;
	info.get_defaults = ftf_get_defaults;
	info.activate = ftf_activate;
	info.deactivate = ftf_deactivate;
	info.video_tick = ftf_tick;
	info.video_render = ftf_render;
	info.get_width = ftf_width;
	info.get_height = ftf_height;
	obs_register_source(&info);

	info.id = "face_tracker_source";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
	if (hide_source)
		info.output_flags |= OBS_SOURCE_CAP_DISABLED;
	info.create = fts_create;
	info.update = fts_update;
	info.get_properties = fts_properties;
	info.get_defaults = fts_get_defaults;
	info.video_tick = fts_tick;
	info.video_render = fts_render;
	obs_register_source(&info);
}
