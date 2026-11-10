#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum { FAITHFULFRAME_FFI_ABI_VERSION = 1 };
enum { FF_OK = 0, FF_INVALID_ARGUMENT = 1, FF_INVALID_STATE = 2, FF_PANIC = 3 };

typedef struct ff_generations { uint64_t source, session, stream, geometry, tracker; } ff_generations;
typedef struct ff_rect { float x, y, width, height; } ff_rect;
typedef struct ff_session_config { uint64_t loss_grace_ms, max_evidence_age_ms; uint32_t max_candidates; ff_rect safe_wide; } ff_session_config;
typedef struct ff_snapshot { uint32_t state, reason, crop_action; uint64_t status_version, observation_version, loss_deadline_ms; uint8_t has_selected, has_loss_deadline; ff_rect crop; } ff_snapshot;
typedef struct ff_session ff_session;

uint32_t faithfulframe_ffi_abi_version(void);
int32_t ff_session_create(const ff_session_config *config, const ff_generations *generations, ff_session **out_session);
void ff_session_destroy(ff_session *session);
int32_t ff_session_snapshot(const ff_session *session, ff_snapshot *out_snapshot);
int32_t ff_session_tick(ff_session *session, uint64_t monotonic_ms, ff_snapshot *out_snapshot);

#ifdef __cplusplus
}
#endif
