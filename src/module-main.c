#include <obs-module.h>
#include <util/config-file.h>
#include <obs-frontend-api.h>
#include "plugin-macros.generated.h"
#include "faithfulframe-worker-log.hpp"
#ifdef WITH_DOCK
#include "../ui/face-tracker-dock.hpp"
#endif // WITH_DOCK

#define CONFIG_SECTION_NAME "face-tracker"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

void register_face_tracker_filter(bool hide_filter, bool hide_source);
void register_face_tracker_ptz(bool hide_ptz);
void register_face_tracker_monitor(bool hide_monitor);
#ifdef ENABLE_RUST_CORE_BRIDGE
int faithfulframe_executor_reaper_start(void);
void faithfulframe_teardown_begin_module_unload(void);
#endif

const char *obs_module_name(void)
{
	return obs_module_text("Module.Name");
}

bool obs_module_load(void)
{
	faithfulframe_worker_log_start();
#ifdef ENABLE_RUST_CORE_BRIDGE
	if (!faithfulframe_executor_reaper_start()) {
		blog(LOG_ERROR, "could not start FaithfulFrame teardown worker; refusing to load safely");
		faithfulframe_worker_log_shutdown();
		return false;
	}
#endif
	blog(LOG_INFO, "registering face_tracker_filter_info (version %s)", PLUGIN_VERSION);

#if LIBOBS_API_VER < MAKE_SEMANTIC_VERSION(31, 0, 0)
	config_t *cfg = obs_frontend_get_global_config();
#else
	config_t *cfg = obs_frontend_get_app_config();
#endif

	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "ShowFilter", true);
	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "ShowSource", true);
	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "ShowPTZ", true);
#ifdef ENABLE_MONITOR_USER
	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "ShowMonitor", true);
#else
	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "ShowMonitor", false);
#endif

	bool show_filter = config_get_bool(cfg, CONFIG_SECTION_NAME, "ShowFilter");
	bool show_source = config_get_bool(cfg, CONFIG_SECTION_NAME, "ShowSource");
	bool show_ptz = config_get_bool(cfg, CONFIG_SECTION_NAME, "ShowPTZ");
	bool show_monitor = config_get_bool(cfg, CONFIG_SECTION_NAME, "ShowMonitor");

	register_face_tracker_filter(!show_filter, !show_source);
	register_face_tracker_ptz(!show_ptz);
	register_face_tracker_monitor(!show_monitor);

#ifdef WITH_DOCK
	config_set_default_bool(cfg, CONFIG_SECTION_NAME, "LoadDock", true);
	bool load_dock = config_get_bool(cfg, CONFIG_SECTION_NAME, "LoadDock");
	if (load_dock)
		ft_docks_init();
#endif // WITH_DOCK

	blog(LOG_INFO, "plugin loaded (plugin version %s, API version %d.%d.%d)", PLUGIN_VERSION, LIBOBS_API_MAJOR_VER,
	     LIBOBS_API_MINOR_VER, LIBOBS_API_PATCH_VER);
	return true;
}

void obs_module_unload()
{
	blog(LOG_INFO, "FaithfulFrame module unload started");
	// Keep teardown active until libobs destroys remaining source/filter
	// instances after this function returns.
#ifdef ENABLE_RUST_CORE_BRIDGE
	blog(LOG_INFO, "FaithfulFrame module unload: stopping session executor reaper");
	faithfulframe_teardown_begin_module_unload();
	blog(LOG_INFO, "FaithfulFrame module unload: session executor reaper stopped");
#endif
#ifdef WITH_DOCK
	blog(LOG_INFO, "FaithfulFrame module unload: releasing Qt docks");
	ft_docks_release();
	blog(LOG_INFO, "FaithfulFrame module unload: Qt docks released");
#endif
	// Remaining source and filter destroy callbacks run after this function
	// returns. Prevent their workers from calling OBS logging after module unload.
	blog(LOG_INFO, "FaithfulFrame module unload: stopping detector log gate");
	faithfulframe_worker_log_shutdown();
	blog(LOG_INFO, "FaithfulFrame module unload complete");
}
