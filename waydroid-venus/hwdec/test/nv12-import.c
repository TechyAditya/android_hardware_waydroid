/*
 * Reproduces NVIDIA misreading two-plane NV12 dma-bufs.  Allocates COUNT NV12 buffers of WIDTH x
 * HEIGHT with GBM, imports each into one Vulkan device as a VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
 * image with its DRM format modifier, copies a pattern into both planes, copies it back, and
 * prints the buffers whose planes do not come back as written.  Needs no Waydroid.
 *
 * build: cc -O2 -o nv12-import nv12-import.c $(pkg-config --cflags --libs gbm libdrm vulkan)
 * usage: nv12-import [WIDTH HEIGHT COUNT]   (default 1920 1080 40)
 * env:   SPLIT=1 imports each plane as an image of its own instead, R8 and R8G8
 *
 * On NVIDIA 615 at 1920x1080, buffers 6, 32 and 38 of 40 read back wrong; at 2560x1440, 1, 11,
 * 17, 27 and 33.  The same buffers imported as one R8 and one R8G8 image per plane read back
 * correctly.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#define CHECK(x)                                                                                 \
   do {                                                                                          \
      VkResult r_ = (x);                                                                         \
      if (r_ != VK_SUCCESS) {                                                                    \
         fprintf(stderr, "%s failed: %d\n", #x, r_);                                             \
         exit(1);                                                                                \
      }                                                                                          \
   } while (0)

static VkDevice dev;
static VkQueue queue;
static VkCommandPool pool;

/* runs one command buffer that RECORD fills, and waits for it */
static void
submit(void (*record)(VkCommandBuffer, void *), void *data)
{
   const VkCommandBufferAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
      .commandPool = pool,
      .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
      .commandBufferCount = 1,
   };
   VkCommandBuffer cmd;
   CHECK(vkAllocateCommandBuffers(dev, &alloc, &cmd));
   const VkCommandBufferBeginInfo begin = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   CHECK(vkBeginCommandBuffer(cmd, &begin));
   record(cmd, data);
   CHECK(vkEndCommandBuffer(cmd));
   const VkSubmitInfo info = {
      .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
      .commandBufferCount = 1,
      .pCommandBuffers = &cmd,
   };
   CHECK(vkQueueSubmit(queue, 1, &info, VK_NULL_HANDLE));
   CHECK(vkQueueWaitIdle(queue));
   vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

struct copy {
   /* one two-plane image, or with image[1] an image per plane */
   VkImage image[2];
   VkBuffer buffer;
   uint32_t width, height;
   bool upload;
};

static void
record_copy(VkCommandBuffer cmd, void *data)
{
   const struct copy *c = data;
   VkImageLayout layout =
      c->upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   int images = c->image[1] ? 2 : 1;
   VkImageMemoryBarrier barriers[2];
   for (int i = 0; i < images; i++) {
      barriers[i] = (VkImageMemoryBarrier){
         .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
         .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
         .dstAccessMask = c->upload ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
         .oldLayout = c->upload ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
         .newLayout = layout,
         .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
         .image = c->image[i],
         .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
      };
   }
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                        NULL, 0, NULL, images, barriers);
   /* the luma plane, then the chroma plane at half size */
   VkBufferImageCopy regions[2] = {
      {
         .bufferOffset = 0,
         .imageSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 },
         .imageExtent = { c->width, c->height, 1 },
      },
      {
         .bufferOffset = (VkDeviceSize)c->width * c->height,
         .imageSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 },
         .imageExtent = { c->width / 2, c->height / 2, 1 },
      },
   };
   for (int i = 0; i < images; i++) {
      VkImage image = c->image[images == 2 ? i : 0];
      const VkBufferImageCopy *r = images == 2 ? &regions[i] : regions;
      if (images == 2)
         regions[i].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      if (c->upload)
         vkCmdCopyBufferToImage(cmd, c->buffer, image, layout, 3 - images, r);
      else
         vkCmdCopyImageToBuffer(cmd, image, layout, c->buffer, 3 - images, r);
   }
}

/* Imports BO as a two-plane NV12 image laid out as GBM reports, or with PLANE >= 0 that plane
 * alone as an R8 or R8G8 image. */
static VkImage
import_nv12(struct gbm_bo *bo, uint32_t width, uint32_t height, int plane)
{
   const VkSubresourceLayout planes[2] = {
      { .offset = gbm_bo_get_offset(bo, 0), .rowPitch = gbm_bo_get_stride_for_plane(bo, 0) },
      { .offset = gbm_bo_get_offset(bo, 1), .rowPitch = gbm_bo_get_stride_for_plane(bo, 1) },
   };
   const VkImageDrmFormatModifierExplicitCreateInfoEXT modifier = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT,
      .drmFormatModifier = gbm_bo_get_modifier(bo),
      .drmFormatModifierPlaneCount = plane < 0 ? 2 : 1,
      .pPlaneLayouts = plane < 0 ? planes : &planes[plane],
   };
   const VkExternalMemoryImageCreateInfo external = {
      .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
      .pNext = &modifier,
      .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
   };
   const VkImageCreateInfo info = {
      .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
      .pNext = &external,
      .imageType = VK_IMAGE_TYPE_2D,
      .format = plane < 0   ? VK_FORMAT_G8_B8R8_2PLANE_420_UNORM
                : plane == 0 ? VK_FORMAT_R8_UNORM
                             : VK_FORMAT_R8G8_UNORM,
      .extent = { plane > 0 ? width / 2 : width, plane > 0 ? height / 2 : height, 1 },
      .mipLevels = 1,
      .arrayLayers = 1,
      .samples = VK_SAMPLE_COUNT_1_BIT,
      .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
      .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               VK_IMAGE_USAGE_SAMPLED_BIT,
   };
   VkImage image;
   CHECK(vkCreateImage(dev, &info, NULL, &image));

   int fd = gbm_bo_get_fd(bo);
   VkMemoryRequirements req;
   vkGetImageMemoryRequirements(dev, image, &req);
   PFN_vkGetMemoryFdPropertiesKHR get_fd_props =
      (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
   VkMemoryFdPropertiesKHR fd_props = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
   CHECK(get_fd_props(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fd_props));
   const VkMemoryDedicatedAllocateInfo dedicated = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
      .image = image,
   };
   const VkImportMemoryFdInfoKHR import = {
      .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
      .pNext = &dedicated,
      .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
      .fd = fd,
   };
   /* a plane imported alone starts at its offset, so the import covers the whole dma-buf */
   off_t fd_size = lseek(fd, 0, SEEK_END);
   const VkMemoryAllocateInfo alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .pNext = &import,
      .allocationSize = (VkDeviceSize)fd_size > req.size ? (VkDeviceSize)fd_size : req.size,
      .memoryTypeIndex = __builtin_ctz(req.memoryTypeBits & fd_props.memoryTypeBits),
   };
   VkDeviceMemory memory;
   CHECK(vkAllocateMemory(dev, &alloc, NULL, &memory));
   CHECK(vkBindImageMemory(dev, image, memory, 0));
   return image;
}

int
main(int argc, char **argv)
{
   uint32_t width = argc > 2 ? atoi(argv[1]) : 1920, height = argc > 2 ? atoi(argv[2]) : 1080;
   int count = argc > 3 ? atoi(argv[3]) : 40;

   const VkApplicationInfo app = {
      .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
      .apiVersion = VK_API_VERSION_1_3,
   };
   const VkInstanceCreateInfo instance_info = {
      .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
      .pApplicationInfo = &app,
   };
   VkInstance instance;
   CHECK(vkCreateInstance(&instance_info, NULL, &instance));
   uint32_t n = 1;
   VkPhysicalDevice pd;
   vkEnumeratePhysicalDevices(instance, &n, &pd);
   VkPhysicalDeviceProperties props;
   vkGetPhysicalDeviceProperties(pd, &props);
   uint32_t v = props.driverVersion;
   if (props.vendorID == 0x10de)
      printf("%s, driver %u.%u.%02u", props.deviceName, v >> 22, (v >> 14) & 0xff, (v >> 6) & 0xff);
   else
      printf("%s, driver 0x%x", props.deviceName, v);
   printf(", %ux%u, %d buffers imported as %s\n", width, height, count,
          getenv("SPLIT") ? "one image per plane" : "two-plane images");

   const float priority = 1;
   const VkDeviceQueueCreateInfo queue_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
      .queueCount = 1,
      .pQueuePriorities = &priority,
   };
   const char *extensions[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf",
                                "VK_EXT_image_drm_format_modifier" };
   const VkDeviceCreateInfo device_info = {
      .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
      .queueCreateInfoCount = 1,
      .pQueueCreateInfos = &queue_info,
      .enabledExtensionCount = 3,
      .ppEnabledExtensionNames = extensions,
   };
   CHECK(vkCreateDevice(pd, &device_info, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &queue);
   const VkCommandPoolCreateInfo pool_info = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
   CHECK(vkCreateCommandPool(dev, &pool_info, NULL, &pool));

   /* a host-visible buffer for both planes */
   VkDeviceSize size = (VkDeviceSize)width * height * 3 / 2;
   const VkBufferCreateInfo buffer_info = {
      .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
      .size = size,
      .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
   };
   VkBuffer buffer;
   CHECK(vkCreateBuffer(dev, &buffer_info, NULL, &buffer));
   VkMemoryRequirements req;
   vkGetBufferMemoryRequirements(dev, buffer, &req);
   VkPhysicalDeviceMemoryProperties mem_props;
   vkGetPhysicalDeviceMemoryProperties(pd, &mem_props);
   uint32_t type = 0;
   while (!(req.memoryTypeBits & (1u << type)) ||
          !(mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
          !(mem_props.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
      type++;
   const VkMemoryAllocateInfo buffer_alloc = {
      .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
      .allocationSize = req.size,
      .memoryTypeIndex = type,
   };
   VkDeviceMemory buffer_memory;
   CHECK(vkAllocateMemory(dev, &buffer_alloc, NULL, &buffer_memory));
   CHECK(vkBindBufferMemory(dev, buffer, buffer_memory, 0));
   uint8_t *map;
   CHECK(vkMapMemory(dev, buffer_memory, 0, VK_WHOLE_SIZE, 0, (void **)&map));

   int dri = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
   struct gbm_device *gbm = dri >= 0 ? gbm_create_device(dri) : NULL;
   if (!gbm) {
      fprintf(stderr, "no GBM device on /dev/dri/renderD128\n");
      return 1;
   }

   int bad = 0;
   for (int i = 0; i < count; i++) {
      struct gbm_bo *bo = gbm_bo_create(gbm, width, height, DRM_FORMAT_NV12, GBM_BO_USE_RENDERING);
      if (!bo || gbm_bo_get_plane_count(bo) != 2) {
         fprintf(stderr, "GBM cannot allocate a two-plane NV12 buffer\n");
         return 1;
      }
      struct copy c = { { 0 }, buffer, width, height, true };
      if (getenv("SPLIT")) {
         c.image[0] = import_nv12(bo, width, height, 0);
         c.image[1] = import_nv12(bo, width, height, 1);
      } else {
         c.image[0] = import_nv12(bo, width, height, -1);
      }
      /* luma 0x22, chroma 0x55 */
      memset(map, 0x22, (size_t)width * height);
      memset(map + (size_t)width * height, 0x55, (size_t)width * height / 2);
      submit(record_copy, &c);
      memset(map, 0, size);
      c.upload = false;
      submit(record_copy, &c);

      size_t luma_wrong = 0, chroma_wrong = 0;
      for (size_t k = 0; k < (size_t)width * height; k++)
         luma_wrong += map[k] != 0x22;
      for (size_t k = (size_t)width * height; k < size; k++)
         chroma_wrong += map[k] != 0x55;
      if (luma_wrong || chroma_wrong) {
         printf("buffer %d (modifier 0x%016llx): %.0f%% of luma and %.0f%% of chroma wrong\n",
                i + 1, (unsigned long long)gbm_bo_get_modifier(bo),
                100.0 * luma_wrong / ((double)width * height),
                100.0 * chroma_wrong / ((double)width * height / 2));
         bad++;
      }
   }
   printf("%d of %d buffers read back wrong\n", bad, count);
   return bad ? 2 : 0;
}
