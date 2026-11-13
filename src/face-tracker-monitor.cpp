#include <obs-module.h>
#include <obs.hpp>
#include <atomic>
#include <memory>
#include <new>
#include <string>
#include <utility>
#include "plugin-macros.generated.h"
#include "helper.hpp"
#include "faithfulframe-source-identity.hpp"

#define MAX_ERROR 2

struct face_tracker_monitor_binding
{
	std::string source_uuid;
	std::string filter_uuid;

	obs_weak_source_t *source_ref = nullptr;
	obs_weak_source_t *filter_ref = nullptr;

	~face_tracker_monitor_binding()
	{
		obs_weak_source_release(filter_ref);
		obs_weak_source_release(source_ref);
	}
};

struct face_tracker_monitor
{
	std::shared_ptr<const face_tracker_monitor_binding> binding;
	std::atomic<bool> notrack{false};
	std::atomic<bool> nosource{false};
	std::atomic<bool> landmark_only{false};
	std::atomic<int> n_error{0};
};

struct filter_uuid_search
{
	const char *uuid;
	obs_source_t *match;
};

static void find_filter_by_uuid(obs_source_t *, obs_source_t *filter, void *param)
{
	auto *lookup = static_cast<filter_uuid_search *>(param);
	if (!lookup->match && faithfulframe::source_identity_matches(lookup->uuid, obs_source_get_uuid(filter)))
		lookup->match = obs_source_get_ref(filter);
}

static obs_source_t *resolve_filter_by_uuid(obs_source_t *source, const char *uuid)
{
	if (!source || !uuid || !*uuid)
		return nullptr;
	filter_uuid_search lookup{uuid, nullptr};
	obs_source_enum_filters(source, find_filter_by_uuid, &lookup);
	return lookup.match;
}

static obs_source_t *get_source(const face_tracker_monitor_binding &binding)
{
	if (!binding.source_ref || binding.source_uuid.empty())
		return nullptr;
	obs_source_t *source = obs_weak_source_get_source(binding.source_ref);
	if (!source)
		return nullptr;
	if (!faithfulframe::source_identity_matches(binding.source_uuid.c_str(), obs_source_get_uuid(source))) {
		obs_source_release(source);
		return nullptr;
	}
	return source;
}

static obs_source_t *get_filter(const face_tracker_monitor_binding &binding)
{
	if (!binding.filter_ref || binding.filter_uuid.empty())
		return nullptr;
	obs_source_t *source = get_source(binding);
	if (!source)
		return nullptr;
	obs_source_t *filter = obs_weak_source_get_source(binding.filter_ref);
	if (!filter ||
	    !faithfulframe::source_identity_matches(binding.filter_uuid.c_str(), obs_source_get_uuid(filter))) {
		if (filter)
			obs_source_release(filter);
		obs_source_release(source);
		return nullptr;
	}
	obs_source_release(source);
	return filter;
}

static obs_source_t *get_target(const face_tracker_monitor_binding &binding)
{
	return binding.filter_uuid.empty() ? get_source(binding) : get_filter(binding);
}

static std::shared_ptr<const face_tracker_monitor_binding>
make_binding(const char *source_uuid, const char *filter_uuid,
	     const std::shared_ptr<const face_tracker_monitor_binding> &previous)
{
	const std::string requested_source = source_uuid ? source_uuid : "";
	const std::string requested_filter = filter_uuid ? filter_uuid : "";
	if (previous && previous->source_uuid == requested_source && previous->filter_uuid == requested_filter)
		return previous;

	auto binding = std::make_shared<face_tracker_monitor_binding>();
	binding->source_uuid = requested_source;
	binding->filter_uuid = requested_filter;
	const bool same_source = previous && previous->source_uuid == requested_source;
	if (!requested_source.empty()) {
		if (same_source) {
			if (previous->source_ref) {
				binding->source_ref = previous->source_ref;
				obs_weak_source_addref(binding->source_ref);
			}
		} else {
			obs_source_t *source = obs_get_source_by_uuid(requested_source.c_str());
			if (source) {
				binding->source_ref = obs_source_get_weak_source(source);
				obs_source_release(source);
			}
		}
	}

	if (!requested_filter.empty() && binding->source_ref) {
		obs_source_t *source = obs_weak_source_get_source(binding->source_ref);
		if (source) {
			if (faithfulframe::source_identity_matches(requested_source.c_str(),
								   obs_source_get_uuid(source))) {
				obs_source_t *filter = resolve_filter_by_uuid(source, requested_filter.c_str());
				if (filter) {
					binding->filter_ref = obs_source_get_weak_source(filter);
					obs_source_release(filter);
				}
			}
			obs_source_release(source);
		}
	}
	return binding;
}

static std::shared_ptr<const face_tracker_monitor_binding> load_binding(const face_tracker_monitor *s)
{
	return std::atomic_load_explicit(&s->binding, std::memory_order_acquire);
}

static bool get_safe_output_dimensions(const face_tracker_monitor_binding &binding, uint32_t &width, uint32_t &height)
{
	obs_source_t *target = get_target(binding);
	if (!target)
		return false;

	proc_handler_t *handler = obs_source_get_proc_handler(target);
	if (!handler) {
		obs_source_release(target);
		return false;
	}

	CALLDATA_FIXED_DECL(cd, 128);
	calldata_set_bool(&cd, "safe_output", true);
	long long reported_width = 0;
	long long reported_height = 0;
	bool safe_enabled = false;
	const bool available = proc_handler_call(handler, "get_target_size", &cd) &&
			       calldata_get_bool(&cd, "safe_enabled", &safe_enabled) && safe_enabled &&
			       calldata_get_int(&cd, "width", &reported_width) &&
			       calldata_get_int(&cd, "height", &reported_height) && reported_width > 0 &&
			       reported_height > 0;
	obs_source_release(target);
	if (!available)
		return false;

	width = static_cast<uint32_t>(reported_width);
	height = static_cast<uint32_t>(reported_height);
	return true;
}

static const char *ftmon_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return obs_module_text("FaithfulFrame Monitor");
}

static void *ftmon_create(obs_data_t *settings, obs_source_t *context)
{
	auto *s = new (std::nothrow) face_tracker_monitor();
	if (!s)
		return nullptr;
	obs_source_update(context, settings);
	return s;
}

static void ftmon_destroy(void *data)
{
	auto *s = (struct face_tracker_monitor *)data;
	delete s;
}

static void ftmon_update(void *data, obs_data_t *settings)
{
	auto *s = (struct face_tracker_monitor *)data;
	const auto previous = load_binding(s);
	const char *source_uuid = obs_data_has_user_value(settings, "source_uuid")
					  ? obs_data_get_string(settings, "source_uuid")
					  : nullptr;
	const char *filter_uuid = obs_data_has_user_value(settings, "filter_uuid")
					  ? obs_data_get_string(settings, "filter_uuid")
					  : nullptr;
	const std::string requested_source = source_uuid ? source_uuid : (previous ? previous->source_uuid : "");
	const std::string requested_filter = filter_uuid ? filter_uuid : (previous ? previous->filter_uuid : "");
	const bool binding_changed = !previous || previous->source_uuid != requested_source ||
				     previous->filter_uuid != requested_filter;
	try {
		auto next = make_binding(requested_source.c_str(), requested_filter.c_str(), previous);
		std::atomic_store_explicit(&s->binding, std::move(next), std::memory_order_release);
	} catch (...) {
		std::atomic_store_explicit(&s->binding, std::shared_ptr<const face_tracker_monitor_binding>(),
					   std::memory_order_release);
		blog(LOG_ERROR, "FaithfulFrame preview monitor could not publish its source binding");
	}
	if (binding_changed)
		s->n_error.store(0, std::memory_order_release);
	if (obs_data_has_user_value(settings, "notrack"))
		s->notrack.store(obs_data_get_bool(settings, "notrack"), std::memory_order_release);
	if (obs_data_has_user_value(settings, "nosource"))
		s->nosource.store(obs_data_get_bool(settings, "nosource"), std::memory_order_release);
	if (obs_data_has_user_value(settings, "landmark_only"))
		s->landmark_only.store(obs_data_get_bool(settings, "landmark_only"), std::memory_order_release);
}

static obs_properties_t *ftmon_properties(void *)
{
	obs_properties_t *props;
	props = obs_properties_create();

	// TODO: use obs_properties_add_list
	obs_properties_add_text(props, "source_name", obs_module_text("Source name"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "filter_name", obs_module_text("Filter name"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "source_uuid", obs_module_text("Source UUID"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(props, "filter_uuid", obs_module_text("Filter UUID"), OBS_TEXT_DEFAULT);

	obs_properties_add_bool(props, "notrack", "Display original source");

	obs_properties_add_bool(props, "nosource", "Overlay only");

	obs_properties_add_bool(props, "landmark_only", "Display landmark information only");

	return props;
}

static void ftmon_get_defaults(obs_data_t *) {}

static void ftmon_tick(void *data, float)
{
	auto *s = (struct face_tracker_monitor *)data;
	const auto binding = load_binding(s);
	const bool source_specified = binding && !binding->source_uuid.empty();
	const bool filter_specified = binding && !binding->filter_uuid.empty();
	bool source_available = false;
	if (source_specified) {
		obs_source_t *source = get_source(*binding);
		source_available = source != nullptr;
		if (source)
			obs_source_release(source);
	}
	bool filter_available = false;
	if (filter_specified && source_available) {
		obs_source_t *filter = get_filter(*binding);
		filter_available = filter != nullptr;
		if (filter)
			obs_source_release(filter);
	}

	if (source_specified && !source_available) {
		const int errors = s->n_error.fetch_add(1, std::memory_order_acq_rel);
		if (errors < MAX_ERROR)
			blog(LOG_INFO, "failed to resolve the configured source");
	} else if (filter_specified && !filter_available) {
		const int errors = s->n_error.fetch_add(1, std::memory_order_acq_rel);
		if (errors < MAX_ERROR)
			blog(LOG_INFO, "failed to resolve the configured filter");
	} else {
		s->n_error.store(0, std::memory_order_release);
	}
}

static uint32_t ftmon_get_width(void *data)
{
	auto *s = (struct face_tracker_monitor *)data;
	const auto binding = load_binding(s);
	if (!binding)
		return 0;

	if (s->notrack.load(std::memory_order_acquire)) {
		obs_source_t *target = get_target(*binding);
		if (!target)
			return 0;
		proc_handler_t *ph = obs_source_get_proc_handler(target);
		if (!ph) {
			obs_source_release(target);
			return 0;
		}

		CALLDATA_FIXED_DECL(cd, 128);
		if (proc_handler_call(ph, "get_target_size", &cd)) {
			long long ret;
			if (calldata_get_int(&cd, "width", &ret)) {
				obs_source_release(target);
				return (uint32_t)ret;
			}
		}
		obs_source_release(target);
	}

	if (!s->notrack.load(std::memory_order_acquire)) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (get_safe_output_dimensions(*binding, width, height))
			return width;
	}

	obs_source_t *source = get_source(*binding);
	if (!source)
		return 0;
	const uint32_t width = obs_source_get_width(source);
	obs_source_release(source);
	return width;
}

static uint32_t ftmon_get_height(void *data)
{
	auto *s = (struct face_tracker_monitor *)data;
	const auto binding = load_binding(s);
	if (!binding)
		return 0;

	if (s->notrack.load(std::memory_order_acquire)) {
		obs_source_t *target = get_target(*binding);
		if (!target)
			return 0;
		proc_handler_t *ph = obs_source_get_proc_handler(target);
		if (!ph) {
			obs_source_release(target);
			return 0;
		}

		CALLDATA_FIXED_DECL(cd, 128);
		if (proc_handler_call(ph, "get_target_size", &cd)) {
			long long ret;
			if (calldata_get_int(&cd, "height", &ret)) {
				obs_source_release(target);
				return (uint32_t)ret;
			}
		}
		obs_source_release(target);
	}

	if (!s->notrack.load(std::memory_order_acquire)) {
		uint32_t width = 0;
		uint32_t height = 0;
		if (get_safe_output_dimensions(*binding, width, height))
			return height;
	}

	obs_source_t *source = get_source(*binding);
	if (!source)
		return 0;
	const uint32_t height = obs_source_get_height(source);
	obs_source_release(source);
	return height;
}

static void ftmon_video_render(void *data, gs_effect_t *)
{
	auto *s = (struct face_tracker_monitor *)data;
	const auto binding = load_binding(s);
	if (!binding)
		return;

	obs_source_t *target = get_target(*binding);
	if (!target)
		return;

	proc_handler_t *ph = obs_source_get_proc_handler(target);
	if (!ph) {
		obs_source_release(target);
		return;
	}

	CALLDATA_FIXED_DECL(cd, 128);
	calldata_set_bool(&cd, "notrack", s->notrack.load(std::memory_order_acquire));

	if (!s->nosource.load(std::memory_order_acquire)) {
		if (!proc_handler_call(ph, "render_frame", &cd)) {
			obs_source_t *source = get_source(*binding);
			if (source) {
				obs_source_video_render(source);
				obs_source_release(source);
			}
		}
	}

	if (s->landmark_only.load(std::memory_order_acquire))
		calldata_set_bool(&cd, "landmark_only", true);

	proc_handler_call(ph, "render_info", &cd);
	obs_source_release(target);
}

extern "C" void register_face_tracker_monitor(bool hide_monitor)
{
	struct obs_source_info info = {};
	info.id = "face_tracker_monitor";
	info.type = OBS_SOURCE_TYPE_INPUT;
	info.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_DO_NOT_DUPLICATE;
	if (hide_monitor)
		info.output_flags |= OBS_SOURCE_CAP_DISABLED;
	info.get_name = ftmon_get_name;
	info.create = ftmon_create;
	info.destroy = ftmon_destroy;
	info.update = ftmon_update;
	info.get_properties = ftmon_properties;
	info.get_defaults = ftmon_get_defaults;
	info.get_width = ftmon_get_width;
	info.get_height = ftmon_get_height;
	info.video_tick = ftmon_tick;
	info.video_render = ftmon_video_render;
	obs_register_source(&info);
}
