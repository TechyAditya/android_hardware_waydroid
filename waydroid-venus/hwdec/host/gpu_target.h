/*
 * Android gralloc buffers in GPU memory that decoded frames are written into
 * on the GPU, so they never cross PCIe.  The decoder must run on a Vulkan
 * device; see gpu_target.c.
 */
#ifndef WAYDROID_HWDEC_GPU_TARGET_H
#define WAYDROID_HWDEC_GPU_TARGET_H

#include <stdbool.h>
#include <stdint.h>

#include <libavutil/buffer.h>
#include <libavutil/frame.h>

struct gpu_ctx;
struct gpu_image;

/* Per decoder; NULL unless DEVICE is a Vulkan device that can import
 * dma-bufs with DRM format modifiers.  Does not take DEVICE. */
struct gpu_ctx *gpu_create(AVBufferRef *device);
void gpu_destroy(struct gpu_ctx *g);

/* Whether G can write frames into RGBA buffers. */
bool gpu_can_convert(const struct gpu_ctx *g);
/* Whether G's GPU misreads some two-plane NV12 dma-bufs that another Vulkan
 * device imports, as NVIDIA (615) does for Android's compositor. */
bool gpu_nv12_unreliable(const struct gpu_ctx *g);

/* Imports the NV12 dma-buf FD (not taken) of WIDTH x HEIGHT, laid out by
 * MODIFIER, with planes at OFFSET[0] and OFFSET[1] that share STRIDE. */
struct gpu_image *gpu_import(struct gpu_ctx *g, int fd, uint32_t width, uint32_t height,
                             uint64_t modifier, const uint32_t offset[2], uint32_t stride);
/* Imports the DRM_FORMAT_ABGR8888 dma-buf FD (not taken) of WIDTH x HEIGHT,
 * laid out by MODIFIER, at OFFSET with STRIDE. */
struct gpu_image *gpu_import_rgba(struct gpu_ctx *g, int fd, uint32_t width, uint32_t height,
                                  uint64_t modifier, uint32_t offset, uint32_t stride);
void gpu_release(struct gpu_ctx *g, struct gpu_image *img);

/* Writes the top left WIDTH x HEIGHT of FRAME, an NV12 frame on g's device,
 * into IMG and waits for it to finish: as is into an NV12 buffer, or as
 * full-range RGB by the frame's matrix into an RGBA one. */
bool gpu_copy(struct gpu_ctx *g, const AVFrame *frame, struct gpu_image *img, uint32_t width,
              uint32_t height);

#endif
