/*
 * Writes decoded frames into Android's gralloc buffers in GPU memory.
 *
 * Android's buffers are dma-bufs in GPU memory, allocated by waydroid-alloc
 * in a layout the GPU names by a DRM format modifier.  An NV12 buffer gets
 * each plane of the decoder's image copied in; each plane is imported into
 * FFmpeg's Vulkan device as an image of its own, R8 for luma and R8G8 for
 * chroma, because NVIDIA (615) loses the chroma writes to some buffers
 * imported as one two-plane image.  An RGBA buffer gets the frame converted
 * to RGB by a compute shader (nv12_to_rgba.comp), so Android never imports
 * an NV12 buffer: NVIDIA also misreads some of those when Android's
 * compositor samples them.
 *
 * Each frame's submission waits on and signals the frame's timeline
 * semaphore like FFmpeg's own submissions, and the reply to Android waits
 * for its fence, because Android gets no fence for the buffer.
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

/* a copy or conversion of a 4K frame takes well under a millisecond */
#define COPY_TIMEOUT_NS 1000000000ull

/* nv12_to_rgba.comp, compiled by build.sh */
static const uint32_t convert_spirv[] =
#include "nv12_to_rgba.spv.h"
   ;

/* nv12_to_rgba.comp's push constants */
struct convert_params {
   float rows[3][4];
   float chroma_origin[2];
   uint32_t size[2];
};

struct gpu_ctx {
   AVHWDeviceContext *dev_ctx;
   AVVulkanDeviceContext *hw;
   VkDevice dev;
   uint32_t family;
   VkQueue queue;
   VkCommandPool pool;
   VkCommandBuffer cmd;
   VkFence fence;
   /* NVIDIA, which misreads some NV12 imports; see gpu_target.h */
   bool nv12_unreliable;

   /* the conversion for RGBA buffers; no pipeline if the queue cannot
    * compute */
   VkSampler sampler;
   VkDescriptorSetLayout set_layout;
   VkPipelineLayout pipeline_layout;
   VkPipeline pipeline;
   VkDescriptorPool desc_pool;
   VkDescriptorSet set;

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
   PFN_vkCreateImageView CreateImageView;
   PFN_vkDestroyImageView DestroyImageView;
   PFN_vkCreateSampler CreateSampler;
   PFN_vkDestroySampler DestroySampler;
   PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout;
   PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout;
   PFN_vkCreatePipelineLayout CreatePipelineLayout;
   PFN_vkDestroyPipelineLayout DestroyPipelineLayout;
   PFN_vkCreateShaderModule CreateShaderModule;
   PFN_vkDestroyShaderModule DestroyShaderModule;
   PFN_vkCreateComputePipelines CreateComputePipelines;
   PFN_vkDestroyPipeline DestroyPipeline;
   PFN_vkCreateDescriptorPool CreateDescriptorPool;
   PFN_vkDestroyDescriptorPool DestroyDescriptorPool;
   PFN_vkAllocateDescriptorSets AllocateDescriptorSets;
   PFN_vkUpdateDescriptorSets UpdateDescriptorSets;
   PFN_vkCmdBindPipeline CmdBindPipeline;
   PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets;
   PFN_vkCmdPushConstants CmdPushConstants;
   PFN_vkCmdDispatch CmdDispatch;
};

/* an NV12 buffer's luma and chroma plane, or an RGBA buffer and its view */
struct gpu_image {
   VkImage image[2];
   VkDeviceMemory mem[2];
   VkImageView view;
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

static void
destroy_converter(struct gpu_ctx *g)
{
   if (g->pipeline)
      g->DestroyPipeline(g->dev, g->pipeline, NULL);
   if (g->pipeline_layout)
      g->DestroyPipelineLayout(g->dev, g->pipeline_layout, NULL);
   if (g->desc_pool)
      g->DestroyDescriptorPool(g->dev, g->desc_pool, NULL);
   if (g->set_layout)
      g->DestroyDescriptorSetLayout(g->dev, g->set_layout, NULL);
   if (g->sampler)
      g->DestroySampler(g->dev, g->sampler, NULL);
   g->pipeline = VK_NULL_HANDLE;
   g->pipeline_layout = VK_NULL_HANDLE;
   g->desc_pool = VK_NULL_HANDLE;
   g->set_layout = VK_NULL_HANDLE;
   g->sampler = VK_NULL_HANDLE;
}

static bool
create_converter(struct gpu_ctx *g)
{
   /* unnormalized coordinates, so the shader needs no image sizes: FFmpeg's
    * images may be larger than the frame */
   const VkSamplerCreateInfo sampler_info = {
      .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
      .magFilter = VK_FILTER_LINEAR,
      .minFilter = VK_FILTER_LINEAR,
      .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
      .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
      .unnormalizedCoordinates = VK_TRUE,
   };
   if (g->CreateSampler(g->dev, &sampler_info, NULL, &g->sampler) != VK_SUCCESS)
      goto fail;

   const VkDescriptorSetLayoutBinding bindings[3] = {
      { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g->sampler },
      { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, &g->sampler },
      { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
   };
   const VkDescriptorSetLayoutCreateInfo set_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
      .bindingCount = 3,
      .pBindings = bindings,
   };
   if (g->CreateDescriptorSetLayout(g->dev, &set_info, NULL, &g->set_layout) != VK_SUCCESS)
      goto fail;

   /* one set, rewritten for every frame once the last one has finished */
   const VkDescriptorPoolSize sizes[2] = {
      { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2 },
      { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 },
   };
   const VkDescriptorPoolCreateInfo pool_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
      .maxSets = 1,
      .poolSizeCount = 2,
      .pPoolSizes = sizes,
   };
   if (g->CreateDescriptorPool(g->dev, &pool_info, NULL, &g->desc_pool) != VK_SUCCESS)
      goto fail;
   const VkDescriptorSetAllocateInfo alloc_info = {
      .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
      .descriptorPool = g->desc_pool,
      .descriptorSetCount = 1,
      .pSetLayouts = &g->set_layout,
   };
   if (g->AllocateDescriptorSets(g->dev, &alloc_info, &g->set) != VK_SUCCESS)
      goto fail;

   const VkPushConstantRange range = {
      VK_SHADER_STAGE_COMPUTE_BIT,
      0,
      sizeof(struct convert_params),
   };
   const VkPipelineLayoutCreateInfo layout_info = {
      .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
      .setLayoutCount = 1,
      .pSetLayouts = &g->set_layout,
      .pushConstantRangeCount = 1,
      .pPushConstantRanges = &range,
   };
   if (g->CreatePipelineLayout(g->dev, &layout_info, NULL, &g->pipeline_layout) != VK_SUCCESS)
      goto fail;

   const VkShaderModuleCreateInfo module_info = {
      .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
      .codeSize = sizeof(convert_spirv),
      .pCode = convert_spirv,
   };
   VkShaderModule module;
   if (g->CreateShaderModule(g->dev, &module_info, NULL, &module) != VK_SUCCESS)
      goto fail;
   const VkComputePipelineCreateInfo pipeline_info = {
      .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
      .stage = {
         .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
         .stage = VK_SHADER_STAGE_COMPUTE_BIT,
         .module = module,
         .pName = "main",
      },
      .layout = g->pipeline_layout,
   };
   VkResult result = g->CreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &pipeline_info, NULL,
                                               &g->pipeline);
   g->DestroyShaderModule(g->dev, module, NULL);
   if (result != VK_SUCCESS) {
      g->pipeline = VK_NULL_HANDLE;
      goto fail;
   }
   return true;

fail:
   destroy_converter(g);
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

   /* any queue copies images, and one that computes also converts them;
    * FFmpeg's frames are shared by all its queue families */
   int qf = -1;
   for (int i = 0; i < hw->nb_qf; i++) {
      if (hw->qf[i].flags & VK_QUEUE_COMPUTE_BIT) {
         qf = i;
         break;
      }
      if (qf < 0 && (hw->qf[i].flags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT)))
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
   LOAD(CreateImageView);
   LOAD(DestroyImageView);
   LOAD(CreateSampler);
   LOAD(DestroySampler);
   LOAD(CreateDescriptorSetLayout);
   LOAD(DestroyDescriptorSetLayout);
   LOAD(CreatePipelineLayout);
   LOAD(DestroyPipelineLayout);
   LOAD(CreateShaderModule);
   LOAD(DestroyShaderModule);
   LOAD(CreateComputePipelines);
   LOAD(DestroyPipeline);
   LOAD(CreateDescriptorPool);
   LOAD(DestroyDescriptorPool);
   LOAD(AllocateDescriptorSets);
   LOAD(UpdateDescriptorSets);
   LOAD(CmdBindPipeline);
   LOAD(CmdBindDescriptorSets);
   LOAD(CmdPushConstants);
   LOAD(CmdDispatch);
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
   if (hw->qf[qf].flags & VK_QUEUE_COMPUTE_BIT)
      create_converter(g);

   PFN_vkGetPhysicalDeviceProperties get_props =
      (PFN_vkGetPhysicalDeviceProperties)hw->get_proc_addr(hw->inst,
                                                           "vkGetPhysicalDeviceProperties");
   if (get_props) {
      VkPhysicalDeviceProperties props;
      get_props(hw->phys_dev, &props);
      g->nv12_unreliable = props.vendorID == 0x10de;
   }
   return g;
}

void
gpu_destroy(struct gpu_ctx *g)
{
   if (!g)
      return;
   destroy_converter(g);
   g->DestroyFence(g->dev, g->fence, NULL);
   g->DestroyCommandPool(g->dev, g->pool, NULL);
   free(g);
}

bool
gpu_can_convert(const struct gpu_ctx *g)
{
   return g && g->pipeline;
}

bool
gpu_nv12_unreliable(const struct gpu_ctx *g)
{
   return g && g->nv12_unreliable;
}

/* Imports the plane of FD at OFFSET as a WIDTH x HEIGHT image of FORMAT. */
static bool
import_plane(struct gpu_ctx *g, int fd, VkFormat format, VkImageUsageFlags usage, uint32_t width,
             uint32_t height, uint64_t modifier, uint32_t offset, uint32_t stride, VkImage *image,
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
      .usage = usage,
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
   if (!import_plane(g, fd, VK_FORMAT_R8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT, width, height,
                     modifier, offset[0], stride, &img->image[0], &img->mem[0]) ||
       !import_plane(g, fd, VK_FORMAT_R8G8_UNORM, VK_IMAGE_USAGE_TRANSFER_DST_BIT, width / 2,
                     height / 2, modifier, offset[1], stride, &img->image[1], &img->mem[1])) {
      gpu_release(g, img);
      return NULL;
   }
   return img;
}

struct gpu_image *
gpu_import_rgba(struct gpu_ctx *g, int fd, uint32_t width, uint32_t height, uint64_t modifier,
                uint32_t offset, uint32_t stride)
{
   if (!gpu_can_convert(g))
      return NULL;
   struct gpu_image *img = calloc(1, sizeof(*img));
   if (!img)
      return NULL;
   /* DRM_FORMAT_ABGR8888 is R, G, B, A in memory */
   if (!import_plane(g, fd, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_STORAGE_BIT, width, height,
                     modifier, offset, stride, &img->image[0], &img->mem[0])) {
      gpu_release(g, img);
      return NULL;
   }
   const VkImageViewCreateInfo view_info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
      .image = img->image[0],
      .viewType = VK_IMAGE_VIEW_TYPE_2D,
      .format = VK_FORMAT_R8G8B8A8_UNORM,
      .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
   };
   if (g->CreateImageView(g->dev, &view_info, NULL, &img->view) != VK_SUCCESS) {
      img->view = VK_NULL_HANDLE;
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
   if (img->view)
      g->DestroyImageView(g->dev, img->view, NULL);
   for (int i = 0; i < 2; i++) {
      if (img->image[i])
         g->DestroyImage(g->dev, img->image[i], NULL);
      if (img->mem[i])
         g->FreeMemory(g->dev, img->mem[i], NULL);
   }
   free(img);
}

/* Kr and Kb of FRAME's matrix.  Without one, Android picks BT.601, BT.709
 * or BT.2020 by the frame's size (ColorUtils::
 * setDefaultCodecColorAspectsIfNeeded), and so does this. */
static void
luma_coefficients(const AVFrame *frame, float *kr, float *kb)
{
   switch (frame->colorspace) {
   case AVCOL_SPC_BT709:
      *kr = 0.2126f, *kb = 0.0722f;
      return;
   case AVCOL_SPC_FCC:
      *kr = 0.30f, *kb = 0.11f;
      return;
   case AVCOL_SPC_BT470BG:
   case AVCOL_SPC_SMPTE170M:
      *kr = 0.299f, *kb = 0.114f;
      return;
   case AVCOL_SPC_SMPTE240M:
      *kr = 0.212f, *kb = 0.087f;
      return;
   case AVCOL_SPC_BT2020_NCL:
   case AVCOL_SPC_BT2020_CL:
      *kr = 0.2627f, *kb = 0.0593f;
      return;
   default:
      break;
   }
   int64_t w = frame->width, h = frame->height;
   if (w >= 3840 || h >= 3840 || w * h >= 3840 * 1634)
      *kr = 0.2627f, *kb = 0.0593f;
   else if ((w <= 720 && h <= 576) || (h <= 720 && w <= 576))
      *kr = 0.299f, *kb = 0.114f;
   else
      *kr = 0.2126f, *kb = 0.0722f;
}

/* The conversion of FRAME's samples to full-range RGB. */
static void
convert_params(const AVFrame *frame, uint32_t width, uint32_t height, struct convert_params *p)
{
   float kr, kb;
   luma_coefficients(frame, &kr, &kb);
   float kg = 1.0f - kr - kb;
   /* samples come in as 8-bit values / 255; Android treats an unspecified
    * range as limited */
   bool full = frame->color_range == AVCOL_RANGE_JPEG;
   float ys = full ? 1.0f : 255.0f / 219.0f, yo = full ? 0.0f : -16.0f / 219.0f;
   float cs = full ? 1.0f : 255.0f / 224.0f, co = full ? -128.0f / 255.0f : -128.0f / 224.0f;
   float rv = 2.0f * (1.0f - kr), bu = 2.0f * (1.0f - kb);
   float gu = -2.0f * kb * (1.0f - kb) / kg, gv = -2.0f * kr * (1.0f - kr) / kg;
   const float rows[3][4] = {
      { ys, 0.0f, rv * cs, yo + rv * co },
      { ys, gu * cs, gv * cs, yo + (gu + gv) * co },
      { ys, bu * cs, 0.0f, yo + bu * co },
   };
   memcpy(p->rows, rows, sizeof(rows));

   /* Chroma texel k covers luma pixels 2k and 2k + 1, with its sample at
    * the left one (MPEG-2, and what decoders assume without a location) or
    * between them; vertically at the top one, between, or the bottom. */
   enum AVChromaLocation loc = frame->chroma_location;
   bool centred = loc == AVCHROMA_LOC_CENTER || loc == AVCHROMA_LOC_TOP ||
                  loc == AVCHROMA_LOC_BOTTOM;
   p->chroma_origin[0] = centred ? 0.25f : 0.5f;
   p->chroma_origin[1] = loc == AVCHROMA_LOC_TOPLEFT || loc == AVCHROMA_LOC_TOP      ? 0.5f
                         : loc == AVCHROMA_LOC_BOTTOMLEFT || loc == AVCHROMA_LOC_BOTTOM ? 0.0f
                                                                                       : 0.25f;
   p->size[0] = width;
   p->size[1] = height;
}

/* Records copies of the planes of the decoder's image(s) SRC into IMG. */
static void
record_copy(struct gpu_ctx *g, VkImage *src, int images, struct gpu_image *img, uint32_t width,
            uint32_t height)
{
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
         .extent = { (width + 1) / 2, (height + 1) / 2, 1 },
      },
   };
   for (int i = 0; i < 2; i++) {
      if (images == 2)
         regions[i].srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      g->CmdCopyImage(g->cmd, src[images == 2 ? i : 0], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                      img->image[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &regions[i]);
   }
}

/* Records the conversion of FRAME, read through VIEWS of its luma and
 * chroma, into IMG. */
static void
record_convert(struct gpu_ctx *g, const AVFrame *frame, const VkImageView views[2],
               struct gpu_image *img, uint32_t width, uint32_t height)
{
   const VkDescriptorImageInfo sources[2] = {
      { .imageView = views[0], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
      { .imageView = views[1], .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
   };
   const VkDescriptorImageInfo target = {
      .imageView = img->view,
      .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
   };
   const VkWriteDescriptorSet writes[2] = {
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = g->set,
         .dstBinding = 0,
         .descriptorCount = 2,
         .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
         .pImageInfo = sources,
      },
      {
         .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
         .dstSet = g->set,
         .dstBinding = 2,
         .descriptorCount = 1,
         .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,
         .pImageInfo = &target,
      },
   };
   g->UpdateDescriptorSets(g->dev, 2, writes, 0, NULL);

   struct convert_params params;
   convert_params(frame, width, height, &params);
   g->CmdBindPipeline(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipeline);
   g->CmdBindDescriptorSets(g->cmd, VK_PIPELINE_BIND_POINT_COMPUTE, g->pipeline_layout, 0, 1,
                            &g->set, 0, NULL);
   g->CmdPushConstants(g->cmd, g->pipeline_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0,
                       sizeof(params), &params);
   g->CmdDispatch(g->cmd, (width + 15) / 16, (height + 15) / 16, 1);
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
   bool convert = img->view != VK_NULL_HANDLE;

   /* NV12 is one two-plane image, or an image per plane */
   int images = vkf->img[1] ? 2 : 1;
   for (int i = 0; i < images; i++) {
      if (vkf->queue_family[i] != VK_QUEUE_FAMILY_IGNORED)
         return false;
   }
   /* the shader reads a two-plane image through a view of each plane */
   if (convert && (!(vfc->usage & VK_IMAGE_USAGE_SAMPLED_BIT) ||
                   (images == 1 && !(vfc->img_flags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT))))
      return false;

   vfc->lock_frame(fc, vkf);
   bool ok = false;
   VkImageView views[2] = { VK_NULL_HANDLE, VK_NULL_HANDLE };
   if (convert) {
      for (int i = 0; i < 2; i++) {
         /* the image's decode usage is not valid for a plane's format */
         const VkImageViewUsageCreateInfo usage = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
            .usage = VK_IMAGE_USAGE_SAMPLED_BIT,
         };
         const VkImageViewCreateInfo view_info = {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = &usage,
            .image = vkf->img[images == 2 ? i : 0],
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = i ? VK_FORMAT_R8G8_UNORM : VK_FORMAT_R8_UNORM,
            .subresourceRange = {
               images == 2 ? VK_IMAGE_ASPECT_COLOR_BIT
               : i         ? VK_IMAGE_ASPECT_PLANE_1_BIT
                           : VK_IMAGE_ASPECT_PLANE_0_BIT,
               0, 1, 0, 1,
            },
         };
         if (g->CreateImageView(g->dev, &view_info, NULL, &views[i]) != VK_SUCCESS) {
            views[i] = VK_NULL_HANDLE;
            goto out;
         }
      }
   }

   VkPipelineStageFlags2 stage =
      convert ? VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT : VK_PIPELINE_STAGE_2_COPY_BIT;
   VkAccessFlags2 src_access =
      convert ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : VK_ACCESS_2_TRANSFER_READ_BIT;
   VkImageLayout src_layout =
      convert ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   int targets = convert ? 1 : 2;
   VkImageMemoryBarrier2 barriers[4];
   VkSemaphoreSubmitInfo waits[2], signals[2];
   for (int i = 0; i < images; i++) {
      barriers[i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
         .srcAccessMask = vkf->access[i],
         .dstStageMask = stage,
         .dstAccessMask = src_access,
         .oldLayout = vkf->layout[i],
         .newLayout = src_layout,
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
         .stageMask = stage,
      };
   }
   /* the old contents are not needed */
   VkImageLayout dst_layout =
      convert ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
   VkAccessFlags2 dst_access =
      convert ? VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT : VK_ACCESS_2_TRANSFER_WRITE_BIT;
   for (int i = 0; i < targets; i++) {
      barriers[images + i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = VK_PIPELINE_STAGE_2_NONE,
         .dstStageMask = stage,
         .dstAccessMask = dst_access,
         .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
         .newLayout = dst_layout,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = img->image[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
   }

   const VkCommandBufferBeginInfo begin = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
      .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
   };
   if (g->ResetCommandBuffer(g->cmd, 0) != VK_SUCCESS ||
       g->BeginCommandBuffer(g->cmd, &begin) != VK_SUCCESS)
      goto out;
   const VkDependencyInfo before = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = images + targets,
      .pImageMemoryBarriers = barriers,
   };
   g->CmdPipelineBarrier2(g->cmd, &before);
   if (convert)
      record_convert(g, frame, views, img, width, height);
   else
      record_copy(g, vkf->img, images, img, width, height);
   /* Android's Vulkan driver reads the buffer next, through another device */
   VkImageMemoryBarrier2 release[2];
   for (int i = 0; i < targets; i++) {
      release[i] = (VkImageMemoryBarrier2){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
         .srcStageMask = stage,
         .srcAccessMask = dst_access,
         .dstStageMask = VK_PIPELINE_STAGE_2_NONE,
         .oldLayout = dst_layout,
         .newLayout = VK_IMAGE_LAYOUT_GENERAL,
         .srcQueueFamilyIndex = g->family,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL,
         .image = img->image[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
   }
   const VkDependencyInfo after = {
      .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO,
      .imageMemoryBarrierCount = targets,
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
      vkf->layout[i] = src_layout;
      vkf->access[i] = src_access;
      vkf->sem_value[i]++;
   }
   ok = g->WaitForFences(g->dev, 1, &g->fence, VK_TRUE, COPY_TIMEOUT_NS) == VK_SUCCESS;

out:
   for (int i = 0; i < 2; i++) {
      if (views[i])
         g->DestroyImageView(g->dev, views[i], NULL);
   }
   vfc->unlock_frame(fc, vkf);
   return ok;
}
