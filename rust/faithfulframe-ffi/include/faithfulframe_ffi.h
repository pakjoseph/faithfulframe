#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FAITHFULFRAME_FFI_ABI_VERSION = 5 };

typedef enum ff_result { FF_OK = 0, FF_INVALID_ARGUMENT = 1, FF_PANIC = 2 } ff_result;

typedef enum ff_state {
	FF_STATE_FROZEN = 0,
	FF_STATE_MANUAL = 1,
	FF_STATE_SEARCHING = 2,
	FF_STATE_LOCKED = 3,
	FF_STATE_LOST = 4
} ff_state;

typedef enum ff_crop_action { FF_CROP_HOLD = 0, FF_CROP_WIDE = 1, FF_CROP_TRACK = 2, FF_CROP_NONE = 3 } ff_crop_action;

typedef enum ff_command_kind {
	FF_COMMAND_ENABLE_AUTO_FOLLOW = 0,
	FF_COMMAND_FREEZE = 1,
	FF_COMMAND_RESUME = 2,
	FF_COMMAND_MANUAL_OVERRIDE = 3,
	FF_COMMAND_SELECT_CANDIDATE = 4,
	FF_COMMAND_RESET_TO_WIDE = 5
} ff_command_kind;

typedef enum ff_lifecycle_kind {
	FF_LIFECYCLE_SCENE_INACTIVE = 0,
	FF_LIFECYCLE_SCENE_ACTIVE = 1,
	FF_LIFECYCLE_CAMERA_DISCONNECTED = 2,
	FF_LIFECYCLE_CAMERA_RESTORED = 3,
	FF_LIFECYCLE_SOURCE_REBOUND = 4,
	FF_LIFECYCLE_GEOMETRY_CHANGED = 5,
	FF_LIFECYCLE_TRACKER_CHANGED = 6,
	FF_LIFECYCLE_FRAMING_CONFIG_CHANGED = 7,
	FF_LIFECYCLE_CANDIDATE_POLICY_CHANGED = 8,
	FF_LIFECYCLE_ADAPTER_DESTROYED = 9,
	FF_LIFECYCLE_SOURCE_DESTROYED = 10,
	FF_LIFECYCLE_OBS_SHUTDOWN = 11
} ff_lifecycle_kind;

typedef enum ff_command_result {
	FF_COMMAND_NOT_APPLICABLE = 0,
	FF_COMMAND_APPLIED = 1,
	FF_COMMAND_REJECTED = 2
} ff_command_result;

typedef enum ff_rejection_reason {
	FF_REJECTION_NONE = 0,
	FF_REJECTION_STALE_COMMAND = 1,
	FF_REJECTION_INVALID_COMMAND = 2,
	FF_REJECTION_NON_MONOTONIC_EVENT = 3,
	FF_REJECTION_INVALID_CANDIDATE = 4,
	FF_REJECTION_STALE_LIFECYCLE = 5,
	FF_REJECTION_INVALID_LIFECYCLE = 6,
	FF_REJECTION_SESSION_CLOSED = 7
} ff_rejection_reason;

typedef struct ff_generations
{
	uint64_t source;
	uint64_t session;
	uint64_t stream;
	uint64_t geometry;
	uint64_t tracker;
} ff_generations;

typedef struct ff_identity
{
	uint64_t source;
	uint64_t session;
} ff_identity;

typedef struct ff_rect
{
	float x;
	float y;
	float width;
	float height;
} ff_rect;

typedef struct ff_session_config
{
	uint64_t loss_grace_ms;
	uint64_t max_evidence_age_ms;
	uint32_t max_candidates;
	/* Input and output dimensions define their respective pixel aspect ratios. */
	uint32_t source_width;
	uint32_t source_height;
	uint32_t output_width;
	uint32_t output_height;
	float target_coverage;
	float min_zoom;
	float max_zoom;
	float smoothing;
	float max_pan_step;
	float max_zoom_step;
} ff_session_config;

typedef struct ff_candidate
{
	uint64_t handle;
	uint64_t continuity_token;
	ff_identity identity;
	uint64_t observation_version;
	ff_generations generations;
	uint64_t observed_at_ms;
	uint64_t expires_at_ms;
	ff_rect rect;
} ff_candidate;

typedef struct ff_command
{
	uint32_t kind;
	ff_identity identity;
	/* Candidate selection is fenced by this preview version. Other commands
	 * validate against current state and source/session generations. */
	uint64_t status_version;
	ff_generations generations;
	uint64_t candidate_handle;
	uint64_t observation_version;
} ff_command;

typedef struct ff_lifecycle
{
	uint32_t kind;
	ff_identity identity;
	ff_generations generations;
	uint32_t source_width;
	uint32_t source_height;
} ff_lifecycle;

typedef struct ff_snapshot
{
	uint32_t state;
	uint32_t reason;
	uint32_t crop_action;
	ff_identity identity;
	uint64_t status_version;
	uint64_t observation_version;
	uint64_t selected_handle;
	uint64_t loss_deadline_ms;
	uint8_t has_selected;
	uint8_t has_loss_deadline;
	uint8_t closed;
	uint32_t command_result;
	uint32_t rejection_reason;
	/* Crop is normalized source-space geometry; viewport is normalized output-space geometry. */
	ff_rect crop;
	ff_rect viewport;
	ff_generations generations;
} ff_snapshot;

typedef struct ff_session ff_session;

uint32_t faithfulframe_ffi_abi_version(void);
ff_result ff_session_create(const ff_session_config *config, const ff_identity *identity,
			    const ff_generations *generations, ff_session **out_session);
void ff_session_destroy(ff_session *session);
ff_result ff_session_snapshot(const ff_session *session, ff_snapshot *out_snapshot);
ff_result ff_session_tick(ff_session *session, uint64_t monotonic_ms, ff_snapshot *out_snapshot);
ff_result ff_session_publish_candidate(ff_session *session, uint64_t monotonic_ms, const ff_candidate *candidate,
				       ff_snapshot *out_snapshot);
ff_result ff_session_apply_command(ff_session *session, uint64_t monotonic_ms, const ff_command *command,
				   ff_snapshot *out_snapshot);
ff_result ff_session_lifecycle(ff_session *session, uint64_t monotonic_ms, const ff_lifecycle *lifecycle,
			       ff_snapshot *out_snapshot);

#ifdef __cplusplus
}
#endif
