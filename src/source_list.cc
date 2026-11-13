#include <vector>
#include <string>
#include <algorithm>
#include <utility>
#include <obs-module.h>
#include <obs-frontend-api.h>
#include "plugin-macros.generated.h"
#include "source_list.h"

struct add_sources_s
{
	obs_source_t *self;
	std::vector<std::string> source_names;
};

static bool add_sources(void *data, obs_source_t *source)
{
	auto &ctx = *(add_sources_s *)data;

	if (source == ctx.self)
		return true;

	uint32_t caps = obs_source_get_output_flags(source);
	if (~caps & OBS_SOURCE_VIDEO)
		return true;

	if (obs_source_is_group(source))
		return true;

	const char *name = obs_source_get_name(source);
	ctx.source_names.push_back(name);
	return true;
}

void property_list_add_sources(obs_property_t *prop, obs_source_t *self)
{
	// scenes, same order as the scene list
	obs_frontend_source_list sceneList = {};
	obs_frontend_get_scenes(&sceneList);
	for (size_t i = 0; i < sceneList.sources.num; i++) {
		obs_source_t *source = sceneList.sources.array[i];
		const char *c_name = obs_source_get_name(source);
		std::string name = obs_module_text("Scene: ");
		name += c_name;
		obs_property_list_add_string(prop, name.c_str(), c_name);
	}
	obs_frontend_source_list_free(&sceneList);

	// sources, alphabetical order
	add_sources_s ctx;
	ctx.self = self;
	obs_enum_sources(add_sources, &ctx);

	std::sort(ctx.source_names.begin(), ctx.source_names.end());

	for (size_t i = 0; i < ctx.source_names.size(); i++) {
		const std::string name = obs_module_text("Source: ") + ctx.source_names[i];
		obs_property_list_add_string(prop, name.c_str(), ctx.source_names[i].c_str());
	}
}

struct add_video_input_sources_by_uuid_s
{
	obs_source_t *self;
	bool failed = false;
	std::vector<std::pair<std::string, std::string>> sources;
};

static bool add_video_input_sources_by_uuid(void *data, obs_source_t *source)
{
	auto &ctx = *(add_video_input_sources_by_uuid_s *)data;
	if (source == ctx.self || obs_source_get_type(source) != OBS_SOURCE_TYPE_INPUT || obs_source_is_scene(source) ||
	    obs_source_is_group(source) || !(obs_source_get_output_flags(source) & OBS_SOURCE_VIDEO))
		return true;

	const char *name = obs_source_get_name(source);
	const char *uuid = obs_source_get_uuid(source);
	if (name && *name && uuid && *uuid) {
		try {
			ctx.sources.emplace_back(name, uuid);
		} catch (...) {
			ctx.failed = true;
			return false;
		}
	}
	return true;
}

void property_list_add_video_input_sources_by_uuid(obs_property_t *prop, obs_source_t *self)
{
	try {
		obs_property_list_add_string(prop, obs_module_text("Select a video input"), "");
		add_video_input_sources_by_uuid_s ctx;
		ctx.self = self;
		obs_enum_sources(add_video_input_sources_by_uuid, &ctx);
		if (ctx.failed) {
			blog(LOG_WARNING, "FaithfulFrame could not enumerate all safety tracking sources");
			return;
		}
		std::sort(ctx.sources.begin(), ctx.sources.end(), [](const auto &left, const auto &right) {
			return left.first == right.first ? left.second < right.second : left.first < right.first;
		});
		for (const auto &source : ctx.sources) {
			const std::string label = std::string(obs_module_text("Source: ")) + source.first;
			obs_property_list_add_string(prop, label.c_str(), source.second.c_str());
		}
	} catch (...) {
		blog(LOG_WARNING, "FaithfulFrame could not populate the safety tracking source list");
	}
}
