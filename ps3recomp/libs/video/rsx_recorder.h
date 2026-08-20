#ifndef PS3RECOMP_RSX_RECORDER_H
#define PS3RECOMP_RSX_RECORDER_H

#include "rsx_commands.h"
#include "rsx_render_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Install the portable recorder as the command parser's backend. The optional
 * legacy backend is invoked inline until its renderer has completed migration
 * to submit_batch. The consumer must copy/encode all batch data before its
 * submit callback returns. */
int rsx_recorder_install(rsx_backend* legacy_backend,
                         const rsx_render_backend_ops* consumer_ops,
                         void* consumer_userdata);
void rsx_recorder_uninstall(void);
const rsx_render_batch* rsx_recorder_pending_batch(void);
int rsx_recorder_flush(u32 display_buffer_id, int allow_empty);
void rsx_recorder_arm_capture(const char* path, u32 frame_count);

#ifdef __cplusplus
}
#endif
#endif
