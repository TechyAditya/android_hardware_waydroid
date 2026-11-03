/*
 * Wire protocol between Waydroid's gralloc (client) and waydroid-alloc
 * (host).  One connection is one allocation: the client sends a request, the
 * host answers with a reply and, on success, the buffer's dma-buf attached
 * with SCM_RIGHTS.  All integers are little endian.
 */
#ifndef WAYDROID_ALLOC_PROTOCOL_H
#define WAYDROID_ALLOC_PROTOCOL_H

#include <stdint.h>

#define ALLOC_VERSION 1
#define ALLOC_SOCKET_NAME "alloc.sock"
#define ALLOC_MAX_DIMENSION 16384u

struct alloc_request {
   uint32_t version;
   uint32_t width;
   uint32_t height;
   /* DRM fourcc: DRM_FORMAT_ABGR8888 or DRM_FORMAT_XBGR8888 */
   uint32_t format;
};

struct alloc_reply {
   /* 0, or a negative errno */
   int32_t status;
   /* bytes per row and plane offset in the dma-buf */
   uint32_t stride;
   uint32_t offset;
   uint32_t reserved;
   /* DRM format modifier, in GPU memory layout */
   uint64_t modifier;
   uint64_t size;
};

#ifdef __cplusplus
#define ALLOC_ASSERT(e) static_assert(e, #e)
#else
#define ALLOC_ASSERT(e) _Static_assert(e, #e)
#endif
ALLOC_ASSERT(sizeof(struct alloc_request) == 16);
ALLOC_ASSERT(sizeof(struct alloc_reply) == 32);

#endif
