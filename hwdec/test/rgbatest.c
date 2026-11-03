/*
 * Decodes CLIP through waydroid-hwdec into RGBA targets from waydroid-alloc, as Android's decoder
 * does, reads every frame back through a second Vulkan device, as Android's compositor does, and
 * compares it with a software decode converted by swscale.  Run it with run.sh.
 *
 * usage: rgbatest HWDEC_SOCKET ALLOC_SOCKET CLIP
 * env:   TARGETS (13) buffers, FRAMES (300) to check, CLEAR=1 fills each buffer with magenta before
 *        its frame, BIG=1 uses 3840x2160 buffers with Android expecting 1280x720 (adaptive
 *        playback), BARS=1 prints 16 samples of the first frame's middle row, REFSPC=N forces the
 *        reference's matrix (AVColorSpace) to check that the comparison catches a wrong one.
 * A frame whose mean difference exceeds 4 of 255 counts as bad; correct frames differ by about 0.1.
 */
#define _GNU_SOURCE
#include <drm_fourcc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <vulkan/vulkan.h>
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavcodec/bsf.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>

#include "../../alloc/protocol.h"
#include "../protocol.h"

#define CK(x) do { VkResult r_ = (x); if (r_) { fprintf(stderr, "%s = %d\n", #x, r_); exit(1); } } while (0)

static int sock;

static int send_fd(int s, const void *buf, size_t len, int fd)
{
   struct iovec iov = { (void *)buf, len };
   union { char b[CMSG_SPACE(sizeof(int))]; struct cmsghdr a; } c;
   struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1 };
   if (fd >= 0) {
      memset(&c, 0, sizeof(c));
      m.msg_control = c.b; m.msg_controllen = sizeof(c.b);
      struct cmsghdr *cm = CMSG_FIRSTHDR(&m);
      cm->cmsg_level = SOL_SOCKET; cm->cmsg_type = SCM_RIGHTS; cm->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(cm), &fd, sizeof(int));
   }
   return sendmsg(s, &m, 0) == (ssize_t)len ? 0 : -1;
}

static int recv_fd(int s, void *buf, size_t len, int *fd)
{
   char cb[CMSG_SPACE(sizeof(int))];
   struct iovec iov = { buf, len };
   struct msghdr m = { .msg_iov = &iov, .msg_iovlen = 1, .msg_control = cb, .msg_controllen = sizeof(cb) };
   if (recvmsg(s, &m, MSG_WAITALL) != (ssize_t)len) return -1;
   if (fd) {
      *fd = -1;
      struct cmsghdr *c = CMSG_FIRSTHDR(&m);
      if (c && c->cmsg_type == SCM_RIGHTS) memcpy(fd, CMSG_DATA(c), sizeof(int));
   }
   return 0;
}

static int request(uint32_t type, const void *head, uint32_t hs, const void *data, uint32_t ds, int fd, struct hwdec_reply *r)
{
   size_t n = sizeof(struct hwdec_request) + hs + ds;
   uint8_t *msg = malloc(n);
   struct hwdec_request h = { type, hs + ds };
   memcpy(msg, &h, sizeof(h));
   memcpy(msg + sizeof(h), head, hs);
   if (ds) memcpy(msg + sizeof(h) + hs, data, ds);
   int err = send_fd(sock, msg, n, fd);
   free(msg);
   int shm = -1;
   if (err || recv_fd(sock, r, sizeof(*r), &shm)) return -1;
   if (shm >= 0) close(shm);
   return 0;
}

/* a buffer as Android's gralloc gets it */
static int alloc_rgba(const char *path, uint32_t w, uint32_t h, struct alloc_reply *r)
{
   int s = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un a = { .sun_family = AF_UNIX };
   strcpy(a.sun_path, path);
   if (connect(s, (void *)&a, sizeof(a))) { perror("connect alloc"); exit(1); }
   struct alloc_request req = { ALLOC_VERSION, w, h, DRM_FORMAT_ABGR8888 };
   int fd;
   if (send_fd(s, &req, sizeof(req), -1) || recv_fd(s, r, sizeof(*r), &fd) || r->status || fd < 0) { fprintf(stderr, "alloc failed %d\n", r->status); exit(1); }
   close(s);
   return fd;
}

/* the reading side, a Vulkan device of its own like Android's compositor */
static VkDevice dev;
static VkPhysicalDevice pd;
static VkQueue q;
static VkCommandPool pool;
static VkBuffer buf;
static void *map;
static VkImage imgs[64];

static void rb_device(size_t bufsize)
{
   VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .apiVersion = VK_API_VERSION_1_3 };
   VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
   VkInstance inst;
   CK(vkCreateInstance(&ici, NULL, &inst));
   uint32_t n = 8; VkPhysicalDevice pds[8];
   vkEnumeratePhysicalDevices(inst, &n, pds);
   /* the GPU the host decodes on: the first, or VK_DEVICE_INDEX */
   uint32_t index = getenv("VK_DEVICE_INDEX") ? atoi(getenv("VK_DEVICE_INDEX")) : 0;
   if (index >= n) { fprintf(stderr, "no Vulkan device %u\n", index); exit(1); }
   pd = pds[index];
   float prio = 1;
   VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueCount = 1, .pQueuePriorities = &prio };
   const char *exts[] = { "VK_KHR_external_memory_fd", "VK_EXT_external_memory_dma_buf", "VK_EXT_image_drm_format_modifier" };
   VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = 3, .ppEnabledExtensionNames = exts };
   CK(vkCreateDevice(pd, &dci, NULL, &dev));
   vkGetDeviceQueue(dev, 0, 0, &q);
   VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT };
   CK(vkCreateCommandPool(dev, &pci, NULL, &pool));
   VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = bufsize, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT };
   CK(vkCreateBuffer(dev, &bci, NULL, &buf));
   VkMemoryRequirements br; vkGetBufferMemoryRequirements(dev, buf, &br);
   VkPhysicalDeviceMemoryProperties mp; vkGetPhysicalDeviceMemoryProperties(pd, &mp);
   uint32_t t = 0;
   while (!((br.memoryTypeBits >> t) & 1) || !(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ||
          !(mp.memoryTypes[t].propertyFlags & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)) t++;
   VkMemoryAllocateInfo bai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = br.size, .memoryTypeIndex = t };
   VkDeviceMemory bmem;
   CK(vkAllocateMemory(dev, &bai, NULL, &bmem));
   CK(vkBindBufferMemory(dev, buf, bmem, 0));
   CK(vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &map));
}

static void rb_import(int k, int fd, uint32_t w, uint32_t h, uint64_t modifier, uint32_t offset, uint32_t stride)
{
   VkSubresourceLayout pl = { .offset = offset, .rowPitch = stride };
   VkImageDrmFormatModifierExplicitCreateInfoEXT mi = { .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, .drmFormatModifier = modifier, .drmFormatModifierPlaneCount = 1, .pPlaneLayouts = &pl };
   VkExternalMemoryImageCreateInfo ei = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO, .pNext = &mi, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT };
   VkImageCreateInfo ii = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = &ei, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1, .samples = 1, .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
                            .usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT };
   CK(vkCreateImage(dev, &ii, NULL, &imgs[k]));
   VkMemoryRequirements req; vkGetImageMemoryRequirements(dev, imgs[k], &req);
   PFN_vkGetMemoryFdPropertiesKHR gp = (void *)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
   VkMemoryFdPropertiesKHR fp = { .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR };
   CK(gp(dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, fd, &fp));
   VkMemoryDedicatedAllocateInfo ded = { .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, .image = imgs[k] };
   VkImportMemoryFdInfoKHR imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, .pNext = &ded, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT, .fd = dup(fd) };
   VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = req.size, .memoryTypeIndex = __builtin_ctz(req.memoryTypeBits & fp.memoryTypeBits) };
   VkDeviceMemory m; CK(vkAllocateMemory(dev, &mai, NULL, &m)); CK(vkBindImageMemory(dev, imgs[k], m, 0));
}

/* copies target K into the buffer, or with CLEAR fills it with magenta instead */
static void rb_run(int k, uint32_t w, uint32_t h, int clear)
{
   VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .commandBufferCount = 1 };
   VkCommandBuffer cmd; CK(vkAllocateCommandBuffers(dev, &cai, &cmd));
   VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
   vkBeginCommandBuffer(cmd, &bi);
   VkImageLayout l = clear ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
   VkImageMemoryBarrier b = { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, .dstAccessMask = clear ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT,
                              .oldLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_GENERAL, .newLayout = l,
                              .srcQueueFamilyIndex = clear ? VK_QUEUE_FAMILY_IGNORED : VK_QUEUE_FAMILY_EXTERNAL, .dstQueueFamilyIndex = clear ? VK_QUEUE_FAMILY_IGNORED : 0,
                              .image = imgs[k], .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
   vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
   if (clear) {
      VkClearColorValue c = { .float32 = { 1, 0, 1, 1 } };
      VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
      vkCmdClearColorImage(cmd, imgs[k], l, &c, 1, &r);
      VkImageMemoryBarrier b2 = b; b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; b2.oldLayout = l; b2.newLayout = VK_IMAGE_LAYOUT_GENERAL; b2.srcQueueFamilyIndex = 0; b2.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
   } else {
      VkBufferImageCopy r = { .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, .imageExtent = { w, h, 1 } };
      vkCmdCopyImageToBuffer(cmd, imgs[k], l, buf, 1, &r);
   }
   vkEndCommandBuffer(cmd);
   VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cmd };
   CK(vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE));
   vkQueueWaitIdle(q);
   vkFreeCommandBuffers(dev, pool, 1, &cmd);
}

/* the reference: a software decoder and swscale */
static AVCodecContext *ref;
static AVFrame *ref_frame;
static struct SwsContext *sws;
static uint8_t *ref_rgba;

static void ref_open(AVCodecParameters *par)
{
   const AVCodec *c = par->codec_id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d") : avcodec_find_decoder(par->codec_id);
   ref = avcodec_alloc_context3(c);
   avcodec_parameters_to_context(ref, par);
   ref->pkt_timebase = (AVRational){ 1, 1000000 };
   if (avcodec_open2(ref, c, NULL)) { puts("reference decoder failed"); exit(1); }
   ref_frame = av_frame_alloc();
   ref_rgba = malloc((size_t)par->width * par->height * 4);
}

/* converts the reference frame like the host should: the frame's matrix, Android's defaults
 * without one, full-range output */
static void ref_convert(uint32_t w, uint32_t h)
{
   AVFrame *f = ref_frame;
   int spc = f->colorspace;
   if (spc == AVCOL_SPC_UNSPECIFIED || spc == AVCOL_SPC_RESERVED || spc == AVCOL_SPC_RGB)
      spc = (w >= 3840 || h >= 3840 || (int64_t)w * h >= 3840 * 1634) ? AVCOL_SPC_BT2020_NCL
            : ((w <= 720 && h <= 576) || (h <= 720 && w <= 576)) ? AVCOL_SPC_SMPTE170M : AVCOL_SPC_BT709;
   if (getenv("REFSPC")) spc = atoi(getenv("REFSPC"));
   sws = sws_getCachedContext(sws, w, h, f->format, w, h, AV_PIX_FMT_RGBA, SWS_BILINEAR | SWS_FULL_CHR_H_INT | SWS_ACCURATE_RND, NULL, NULL, NULL);
   const int *coef = sws_getCoefficients(spc);
   sws_setColorspaceDetails(sws, coef, f->color_range == AVCOL_RANGE_JPEG, coef, 1, 0, 1 << 16, 1 << 16);
   uint8_t *dst[4] = { ref_rgba }; int ds[4] = { (int)w * 4 };
   sws_scale(sws, (const uint8_t *const *)f->data, f->linesize, 0, h, dst, ds);
}

int main(int argc, char **argv)
{
   if (argc < 4) { fprintf(stderr, "usage: rgbatest HWDEC_SOCKET ALLOC_SOCKET CLIP\n"); return 2; }
   int ntargets = getenv("TARGETS") ? atoi(getenv("TARGETS")) : 13;
   int max_frames = getenv("FRAMES") ? atoi(getenv("FRAMES")) : 300;
   bool clear = getenv("CLEAR");

   AVFormatContext *fmt = NULL;
   if (avformat_open_input(&fmt, argv[3], NULL, NULL) < 0 || avformat_find_stream_info(fmt, NULL) < 0) return 1;
   int vs = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, NULL, 0);
   AVCodecParameters *par = fmt->streams[vs]->codecpar;
   AVRational tb = fmt->streams[vs]->time_base;
   uint32_t codec = par->codec_id == AV_CODEC_ID_AV1 ? HWDEC_CODEC_AV1 : par->codec_id == AV_CODEC_ID_VP9 ? HWDEC_CODEC_VP9 : par->codec_id == AV_CODEC_ID_HEVC ? HWDEC_CODEC_HEVC : HWDEC_CODEC_H264;
   uint32_t w = par->width, h = par->height;
   ref_open(par);

   sock = socket(AF_UNIX, SOCK_STREAM, 0);
   struct sockaddr_un a = { .sun_family = AF_UNIX };
   strcpy(a.sun_path, argv[1]);
   if (connect(sock, (void *)&a, sizeof(a))) { perror("connect hwdec"); return 1; }
   struct hwdec_reply r;
   struct hwdec_open o = { HWDEC_VERSION, codec };
   if (request(HWDEC_REQUEST_OPEN, &o, sizeof(o), NULL, 0, -1, &r) || r.type != HWDEC_REPLY_OK) { puts("open failed"); return 1; }
   printf("%s %ux%u: open flags 0x%x\n", avcodec_get_name(par->codec_id), w, h, r.flags);
   if (!(r.flags & HWDEC_OPEN_RGBA_TARGETS)) { puts("the host cannot write RGBA targets"); return 1; }

   /* BIG: adaptive-playback buffers of 3840x2160, with Android still expecting 1280x720 */
   bool big = getenv("BIG");
   uint32_t tw = big ? 3840 : w, th = big ? 2160 : h, ow = big ? 1280 : w, oh = big ? 720 : h;
   rb_device((size_t)w * h * 4);
   for (int k = 1; k <= ntargets; k++) {
      struct alloc_reply ar;
      int fd = alloc_rgba(argv[2], tw, th, &ar);
      struct hwdec_target t = { .id = k, .format = HWDEC_FORMAT_RGBA_GPU, .width = tw, .height = th, .offset = { ar.offset }, .stride = { ar.stride },
                                .modifier_lo = (uint32_t)ar.modifier, .modifier_hi = ar.modifier >> 32 };
      if (request(HWDEC_REQUEST_REGISTER, &t, sizeof(t), NULL, 0, fd, &r) || r.type != HWDEC_REPLY_OK) { printf("register %d failed\n", k); return 1; }
      if (k == 1) printf("targets: modifier 0x%016llx stride %u size %llu\n", (unsigned long long)ar.modifier, ar.stride, (unsigned long long)ar.size);
      rb_import(k - 1, fd, tw, th, ar.modifier, ar.offset, ar.stride);
      close(fd);
   }

   AVBSFContext *bsf = NULL;
   if (codec == HWDEC_CODEC_H264 || codec == HWDEC_CODEC_HEVC) {
      av_bsf_alloc(av_bsf_get_by_name(codec == HWDEC_CODEC_H264 ? "h264_mp4toannexb" : "hevc_mp4toannexb"), &bsf);
      avcodec_parameters_copy(bsf->par_in, par);
      bsf->time_base_in = tb;
      av_bsf_init(bsf);
   }
   AVPacket *pkt = av_packet_alloc(), *rpkt = av_packet_alloc();
   int frames = 0, in_target = 0, bad = 0, unmatched = 0, next = 0, per_bad[65] = { 0 };
   double sum_diff = 0, worst = 0, host_ms = 0;
   int worst_frame = 0;
   while (frames < max_frames && av_read_frame(fmt, pkt) >= 0) {
      if (pkt->stream_index != vs) { av_packet_unref(pkt); continue; }
      int64_t ts = av_rescale_q(pkt->pts, tb, (AVRational){ 1, 1000000 });
      av_packet_ref(rpkt, pkt);
      rpkt->pts = ts; rpkt->dts = AV_NOPTS_VALUE;
      avcodec_send_packet(ref, rpkt);
      av_packet_unref(rpkt);
      if (bsf) { av_bsf_send_packet(bsf, pkt); if (av_bsf_receive_packet(bsf, pkt) < 0) continue; }
      int tgt = 1 + next++ % ntargets;
      if (clear) rb_run(tgt - 1, w, h, 1);
      struct hwdec_packet ph = { .timestamp = ts, .output = { tgt, ow, oh } };
      struct timespec t0, t1; clock_gettime(CLOCK_MONOTONIC, &t0);
      if (request(HWDEC_REQUEST_DECODE, &ph, sizeof(ph), pkt->data, pkt->size, -1, &r)) { puts("io error"); return 1; }
      clock_gettime(CLOCK_MONOTONIC, &t1); host_ms += (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
      av_packet_unref(pkt);
      for (;;) {
         if (r.type == HWDEC_REPLY_FRAME) {
            frames++;
            if (!(r.flags & HWDEC_FRAME_IN_TARGET) || r.format != HWDEC_FORMAT_RGBA_GPU) { if (frames < 5) printf("frame %d not in an RGBA target\n", frames); }
            else {
               in_target++;
               /* the reference frame with the same timestamp */
               bool found = false;
               while (!found && avcodec_receive_frame(ref, ref_frame) == 0) found = ref_frame->pts == r.timestamp;
               if (!found) { unmatched++; }
               else {
                  ref_convert(w, h);
                  rb_run(tgt - 1, w, h, 0);
                  const uint8_t *p = map;
                  uint64_t d = 0, n = 0;
                  for (uint32_t y = 0; y < h; y += 3)
                     for (uint32_t x = 0; x < w; x += 3)
                        for (int c = 0; c < 3; c++) { size_t i = ((size_t)y * w + x) * 4 + c; d += abs(p[i] - ref_rgba[i]); n++; }
                  double md = (double)d / n;
                  sum_diff += md;
                  if (md > worst) { worst = md; worst_frame = frames; }
                  if (md > 4) { bad++; per_bad[tgt]++; if (bad <= 5) printf("frame %d target %d: mean difference %.2f\n", frames, tgt, md); }
                  if (getenv("BARS") && frames == 1) { printf("bars:"); for (int k = 0; k < 16; k++) printf(" %d", p[((size_t)(h / 2) * w + (size_t)((k + 0.5) * w / 16)) * 4]); printf("\n"); }
                  if (frames == 1 && r.full_range != 1) printf("frame 1 is not marked full range\n");
               }
            }
         } else if (r.type == HWDEC_REPLY_ERROR) { puts("decode error"); return 1; }
         if (!r.pending) break;
         tgt = 1 + next++ % ntargets;
         if (clear) rb_run(tgt - 1, w, h, 1);
         struct hwdec_output out = { tgt, ow, oh };
         request(HWDEC_REQUEST_RECEIVE, &out, sizeof(out), NULL, 0, -1, &r);
      }
   }
   printf("%d frames, %d in RGBA targets, %d without a reference; mean difference %.2f, worst %.2f (frame %d); %d frames over 4",
          frames, in_target, unmatched, in_target - unmatched ? sum_diff / (in_target - unmatched) : 0, worst, worst_frame, bad);
   if (bad) { printf(" in targets"); for (int k = 1; k <= ntargets; k++) if (per_bad[k]) printf(" %d", k); }
   printf("; %.2f ms per decode request\n", host_ms / next);
   return 0;
}
