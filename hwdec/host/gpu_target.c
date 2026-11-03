/*
 * Copies decoded frames into Android's gralloc buffers in GPU memory.
 *
 * Android's buffers are NV12 dma-bufs in GPU memory, allocated by
 * waydroid-alloc in a layout the GPU names by a DRM format modifier.  Each
 * plane is imported into FFmpeg's Vulkan device as an image of its own, R8
 * for luma and R8G8 for chroma, and each frame is a copy of each plane from
 * the decoder's image, in GPU memory on both sides.  NVIDIA (615) loses the
 * chroma writes to some buffers imported as one two-plane image.  The copy waits on and signals the frame's timeline semaphore like
 * FFmpeg's own submissions, and the reply to Android waits for its fence,
 * because Android gets no fence for the buffer.
 */
#define _GNU_SOURCE
#include "gpu_target.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <vulkan/vulkan.h>

#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>

/* a copy of a 4K frame takes well under a millisecond */
#define COPY_TIMEOUT_NS 1000000000ull

struct gpu_ctx {
   AVHWDeviceContext *dev_ctx;
   AVVulkanDeviceContext *hw;
   VkDevice dev;
   uint32_t family;
   VkQueue queue;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;

   PFN_vkGetDeviceQueue2 GetDeviceQueue2;
   PFN_vkCreateCommandPool CreateCommandPool;
   PFN_vkDestroyCommandPool DestroyCommandPool;
   PFN_vkAllocateCommandBuffers AllocateCommandBuffers;
   PFN_vkBeginCommandBuffer BeginCommandBuffer;
   PFN_vkEndCommandBuffer EndCommandBuffer;
   PFN_vkResetCommandBuffer ResetCommandBuffer;
   PFN_vkCreateFence CreateFence;
   PFN_vkDestroyFence DestroyFence;
   PFN_vkResetFences ResetFences;
   PFN_vkWaitForFences WaitForFences;
   PFN_vkCreateImage CreateImage;
   PFN_vkDestroyImage DestroyImage;
   PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements;
   PFN_vkAllocateMemory AllocateMemory;
   PFN_vkFreeMemory FreeMemory;
   PFN_vkBindImageMemory BindImageMemory;
   PFN_vkGetMemoryFdPropertiesKHR GetMemoryFdPropertiesKHR;
   PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2;
   PFN_vkCmdCopyImage CmdCopyImage;
   PFN_vkQueueSubmit2 QueueSubmit2;
};

/* the luma and the chroma plane of a buffer */
struct gpu_image {
   VkImage image[2];
   VkDeviceMemory mem[2];
};

static bool
has_extension(const AVVulkanDeviceContext *hw, const char *name)
{
   for (int i = 0; i < hw->nb_enabled_dev_extensions; i++) {
      if (!strcmp(hw->enabled_dev_extensions[i], name))
         return true;
   }
   return false;
}

struct gpu_ctx *
gpu_create(AVBufferRef *device)
{
   if (!device)
      return NULL;
   AVHWDeviceContext *dev_ctx = (AVHWDeviceContext *)device->data;
   if (dev_ctx->type != AV_HWDEVICE_TYPE_VULKAN)
      return NULL;
   AVVulkanDeviceContext *hw = dev_ctx->hwctx;
   if (!has_extension(hw, VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME) ||
       !has_extension(hw, VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME) ||
       !has_extension(hw, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME))
      return NULL;

   /* any queue copies images; FFmpeg's frames are shared by all its queue
    * families */
   int qf = -1;
   for (int i = 0; i < hw->nb_qf && qf < 0; i++) {
      if (hw->qf[i].flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT))
         qf = i;
   }
   if (qf < 0)
      return NULL;

   struct gpu_ctx *g = calloc(1, sizeof(*g));
   if (!g)
      return NULL;
   g->dev_ctx = dev_ctx;
   g->hw = hw;
   g->dev = hw->act_dev;
   g->family = hw->qf[qf].idx;

   PFN_vkGetDeviceProcAddr gdpa =
      (PFN_vkGetDeviceProcAddr)hw->get_proc_addr(hw->inst, "vkGetDeviceProcAddr");
   bool ok = gdpa != NULL;
#define LOAD(name)                                                              \
   do {                                                                         \
      if (ok) {                                                                 \
         g->name = (PFN_vk##name)gdpa(g->dev, "vk" #name);                      \
         ok = g->name != NULL;                                                  \
      }                                                                         \
   } while (0)
   LOAD(GetDeviceQueue2);
   LOAD(CreateCommandPool);
   LOAD(DestroyCommandPool);
   LOAD(AllocateCommandBuffers);
   LOAD(BeginCommandBuffer);
   LOAD(EndCommandBuffer);
   LOAD(ResetCommandBuffer);
   LOAD(CreateFence);
   LOAD(DestroyFence);
   LOAD(ResetFences);
   LOAD(WaitForFences);
   LOAD(CreateImage);
   LOAD(DestroyImage);
   LOAD(GetImageMemoryRequirements);
   LOAD(AllocateMemory);
   LOAD(FreeMemory);
   LOAD(BindImageMemory);
   LOAD(GetMemoryFdPropertiesKHR);
   LOAD(CmdPipelineBarrier2);
   LOAD(CmdCopyImage);
   LOAD(QueueSubmit2);
#undef LOAD
   if (!ok) {
      free(g);
      return NULL;
   }

   /* FFmpeg may create its queues internally synchronized */
   const VkDeviceQueueInfo2 queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_INFO_2,
      .flags = hw->queue_flags,
      .queueFamilyIndex = g->family,
   };
   g->GetDeviceQueue2(g->dev, &queue_info, &g->queue);
   const VkCommandPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
      .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
      .queueFamilyIndex = g->family,
   };
   const VkFenceCreateInfo fence_info = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
   if (g->CreateCommandPool(g->dev, &pool_info, NULL, &g->pool) != VK_SUCCESS) {
      free(g);
      return NULL;
   }
   const VkCommandBufferAllocateInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = g->pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   if (g->AllocateCommandBuffers(g->dev, &cmd_info, &g->cmd) != VK_SUCCESS ||
       g->CreateFence(g->dev, &fence_info, NULL, &g->fence) != VK_SUCCESS) {
      g->DestroyCommandPool(g->dev, g->pool, NULL);
      free(g);
      return NULL;
   }
   return g;
}

void
gpu_destroy(struct gpu_ctx *g)
{
   if (!g)
      return;
   g->DestroyFence(g->dev, g->fence, NULL);
   g->DestroyCommandPool(g->dev, g->pool, NULL);
   free(g);
}

/* Imports the plane of FD at OFFSET as a WIDTH x HEIGHT image of FORMAT. */
static bool
import_plane(struct gpu_ctx *g, int fd, VkFormat format, uint32_t width, uint32_t height,
             uint64_t modifier, uint32_t offset, uint32_t stride, VkImage *image,
             VkDeviceMemory *mem)
{
   const VkSubresourceLayout plane = { .offset = offset, .rowPitch = stride };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT mod_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = modifier,
      .drmFormatModifierPlaneCount = 1,
      .pPlaneLayouts = &plane,
   };
   const VkExternalMemoryImageCreateInfo ext_info = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &mod_info,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo image_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &ext_info,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = format,
      .extent = { width, height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT,
      .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
      .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
   };
   if (g->CreateImage(g->dev, &image_info, NULL, image) != VK_SUCCESS)
      return false;

   VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
   VkMemoryRequirements req;
   g->GetImageMemoryRequirements(g->dev, *image, &req);
   if (g->GetMemoryFdPropertiesKHR(g->dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd,
                                   &fd_props) != VK_SUCCESS)
      return false;
   uint32_t types = req.memoryTypeBits & fd_props.memoryTypeBits;
   off_t size = lseek(fd, 0, SEEK_END);
   if (!types || size < (off_t)req.size)
      return false;

   /* a successful import takes the fd */
   int own = fcntl(fd, F_DUPFD_CLOEXEC, 0);
   if (own < 0)
      return false;
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = *image,
   };
   const VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = own,
   };
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = size,
      .memoryTypeIndex = __builtin_ctz(types),
   };
   if (g->AllocateMemory(g->dev, &alloc, NULL, mem) != VK_SUCCESS) {
      close(own);
      return false;
   }
   return g->BindImageMemory(g->dev, *image, *mem, 0) == VK_SUCCESS;
}

struct gpu_image *
gpu_import(struct gpu_ctx *g, int fd, uint32_t width, uint32_t height, uint64_t modifier,
           const uint32_t offset[2], uint32_t stride)
{
   struct gpu_image *img = calloc(1, sizeof(*img));
   if (!img)
      return NULL;
   if (!import_plane(g, fd, VK_FORMAT_R8_UNORM, width, height, modifier, offset[0], stride,
                     &img->image[0], &img->mem[0]) ||
       !import_plane(g, fd, VK_FORMAT_R8G8_UNORM, width / 2, height / 2, modifier, offset[1],
                     stride, &img->image[1], &img->mem[1])) {
      gpu_release(g, img);
      return NULL;
   }
   return img;
}

void
gpu_release(struct gpu_ctx *g, struct gpu_image *img)
{
   if (!img)
      return;
   for (int i = 0; i < 2; i++) {
      if (img->image[i])
         g->DestroyImage(g->dev, img->image[i], NULL);
      if (img->mem[i])
         g->FreeMemory(g->dev, img->mem[i], NULL);
   }
   free(img);
}

bool
gpu_copy(struct gpu_ctx *g, const AVFrame *frame, struct gpu_image *img, uint32_t width,
         uint32_t height)
{
   if (frame->format != AV_PIX_FMT_VULKAN || !frame->hw_frames_ctx)
      return false;
   AVHWFramesContext *fc = (AVHWFramesContext *)frame->hw_frames_ctx->data;
   if (fc->device_ctx != g->dev_ctx || fc->sw_format != AV_PIX_FMT_NV12)
      return false;
   AVVulkanFramesContext *vfc = fc->hwctx;
   AVVkFrame *vkf = (AVVkFrame *)frame->data[0];

   /* NV12 is one two-plane image, or an image per plane */
   int images = vkf->img[1] ? 2 : 1;
   for (int i = 0; i < images; i++) {
      if (vkf->queue_family[i] != VK_QUEUE_FAMILY_IGNORED)
         return false;
   }
   uint32_t cw = (width + 1) / 2, ch = (height + 1) / 2;

   vfc->lock_frame(fc, vkf);
   bool ok = false;
   VkImageMemoryBarrier2 barriers[4];
   VkSemaphoreSubmitInfo waits[2], signals[2];
   for (int i = 0; i < images; i++) {
      barriers[i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = vkf->access[i],
         .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT,
         .oldLayout = vkf->layout[i],
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = vkf->img[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
      waits[i] = (VkSemaphoreSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = vkf->sem[i],
         .value = vkf->sem_value[i],
         .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
      };
      signals[i] = (VkSemaphoreSubmitInfo){
         .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO,
         .semaphore = vkf->sem[i],
         .value = vkf->sem_value[i] + 1,
         .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
      };
   }
   /* the old contents are not needed */
   for (int i = 0; i < 2; i++) {
      barriers[images + i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
         .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = img->image[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
   }

   /* R8 and R8G8 are the formats of NV12's planes, so they copy as is */
   VkImageCopy regions[2] = {
      {
         .srcSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 },
         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
         .extent = { width, height, 1 },
      },
      {
         .srcSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 },
         .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
         .extent = { cw, ch, 1 },
      },
   };

   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (g->ResetCommandBuffer(g->cmd, 0) != VK_SUCCESS ||
       g->BeginCommandBuffer(g->cmd, &begin) != VK_SUCCESS)
      goto out;
   const VkDependencyInfo before = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = images + 2,
      .pImageMemoryBarriers = barriers,
   };
   g->CmdPipelineBarrier2(g->cmd, &before);
   for (int i = 0; i < 2; i++) {
      if (images == 2)
         regions[i].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      g->CmdCopyImage(g->cmd, vkf->img[images == 2 ? i : 0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      img->image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[i]);
   }
   /* Android's Vulkan driver reads the buffer next, through another device */
   VkImageMemoryBarrier2 release[2];
   for (int i = 0; i < 2; i++) {
      release[i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT,
         .srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
         .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
         .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = g->family,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
         .image = img->image[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
   }
   const VkDependencyInfo after = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = 2,
      .pImageMemoryBarriers = release,
   };
   g->CmdPipelineBarrier2(g->cmd, &after);
   if (g->EndCommandBuffer(g->cmd) != VK_SUCCESS)
      goto out;

   const VkCommandBufferSubmitInfo cmd_info = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO,
      .commandBuffer = g->cmd,
   };
   const VkSubmitInfo2 submit = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2,
      .waitSemaphoreInfoCount = images,
      .pWaitSemaphoreInfos = waits,
      .commandBufferInfoCount = 1,
      .pCommandBufferInfos = &cmd_info,
      .signalSemaphoreInfoCount = images,
      .pSignalSemaphoreInfos = signals,
   };
   if (g->ResetFences(g->dev, 1, &g->fence) != VK_SUCCESS)
      goto out;
   /* FFmpeg submits to the same queues under these locks unless the driver
    * synchronizes its queues itself */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
   g->hw->lock_queue(g->dev_ctx, g->family, 0);
   VkResult result = g->QueueSubmit2(g->queue, 1, &submit, g->fence);
   g->hw->unlock_queue(g->dev_ctx, g->family, 0);
#pragma GCC diagnostic pop
   if (result != VK_SUCCESS)
      goto out;
   for (int i = 0; i < images; i++) {
      vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
      vkf->access[i] = VK_ACCESS_2_TRANSFER_READ_BIT;
      vkf->sem_value[i]++;
   }
   ok = g->WaitForFences(g->dev, 1, &g->fence, VK_TRUE, COPY_TIMEOUT_NS) == VK_SUCCESS;

out:
   vfc->unlock_frame(fc, vkf);
   return ok;
}
