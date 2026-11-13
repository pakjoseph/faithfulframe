#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void property_list_add_sources(obs_property_t *prop, obs_source_t *self);
void property_list_add_video_input_sources_by_uuid(obs_property_t *prop, obs_source_t *self);

#ifdef __cplusplus
} // extern "C"
#endif
