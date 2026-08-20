#ifndef PS3RECOMP_RSX_BATCH_IO_H
#define PS3RECOMP_RSX_BATCH_IO_H

#include "rsx_render_batch.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RSXB_MAX_FILE_SIZE (UINT64_C(2) * 1024u * 1024u * 1024u)
#define RSXB_MAX_BATCHES 4096u
#define RSXB_MAX_OPERATIONS 1048576u
#define RSXB_MAX_BLOB_SIZE (UINT64_C(512) * 1024u * 1024u)

int rsxb_write_file(const char* path, const rsx_render_batch* batches,
                    u32 batch_count, char* error, size_t error_size);
int rsxb_read_file(const char* path, rsx_render_batch** batches,
                   u32* batch_count, char* error, size_t error_size);
void rsxb_free_batches(rsx_render_batch* batches, u32 batch_count);

#ifdef __cplusplus
}
#endif
#endif
