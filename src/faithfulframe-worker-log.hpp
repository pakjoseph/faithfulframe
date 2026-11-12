#pragma once

#ifdef __cplusplus
extern "C" {
#endif

void faithfulframe_worker_log_start(void);
void faithfulframe_worker_log_shutdown(void);
void faithfulframe_worker_log(int level, const char *format, ...);

#ifdef __cplusplus
}
#endif
