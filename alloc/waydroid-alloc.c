/*
 * Allocates render buffers in GPU memory for Waydroid's gralloc, through the
 * GPU's GBM backend, and hands them out as dma-bufs.  Buffers in GPU memory
 * let Android render and the compositor read without crossing PCIe, which
 * system-memory gralloc buffers do on every frame.  See protocol.h.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <gbm.h>
#include <getopt.h>
#include <glob.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <drm_fourcc.h>
#include <stdbool.h>

#include "protocol.h"

static uid_t allowed_uid = 1000; /* Android's AID_SYSTEM, which runs the allocator HAL */
static struct gbm_device *gbm;

/* Layouts the GPU can both render to and sample, as EGL reports them, which
 * is also what compositors accept from clients.  Left to itself the driver
 * may pick a compressed layout that only it can read. */
#define MAX_MODIFIERS 64
static struct {
   uint32_t format;
   /* video is only sampled, which EGL calls external */
   bool external_ok;
   uint64_t modifiers[MAX_MODIFIERS];
   int count;
} layouts[3] = {
   { .format = DRM_FORMAT_ABGR8888 },
   { .format = DRM_FORMAT_XBGR8888 },
   { .format = DRM_FORMAT_NV12, .external_ok = true },
};

static bool
query_layouts(void)
{
   PFNEGLGETPLATFORMDISPLAYEXTPROC get_display =
      (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
   PFNEGLQUERYDMABUFMODIFIERSEXTPROC query =
      (PFNEGLQUERYDMABUFMODIFIERSEXTPROC)eglGetProcAddress("eglQueryDmaBufModifiersEXT");
   if (!get_display || !query)
      return false;
   EGLDisplay dpy = get_display(EGL_PLATFORM_GBM_KHR, gbm, NULL);
   if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL))
      return false;
   bool ok = true;
   for (unsigned i = 0; i < sizeof(layouts) / sizeof(layouts[0]); i++) {
      EGLuint64KHR mods[MAX_MODIFIERS];
      EGLBoolean external[MAX_MODIFIERS];
      EGLint n = 0;
      /* without NV12, video stays in system memory */
      bool needed = !layouts[i].external_ok;
      if (!query(dpy, layouts[i].format, MAX_MODIFIERS, mods, external, &n)) {
         ok = ok && !needed;
         continue;
      }
      for (EGLint j = 0; j < n; j++) {
         /* compositors leave out NVIDIA's compressed layouts */
         bool compressed = (mods[j] >> 56) == DRM_FORMAT_MOD_VENDOR_NVIDIA &&
                           ((mods[j] >> 23) & 7) != 0;
         if ((!external[j] || layouts[i].external_ok) && !compressed &&
             mods[j] != DRM_FORMAT_MOD_INVALID)
            layouts[i].modifiers[layouts[i].count++] = mods[j];
      }
      if (!layouts[i].count && needed)
         ok = false;
   }
   eglTerminate(dpy);
   return ok;
}

static void
log_msg(const char *fmt, ...)
{
   va_list ap;
   va_start(ap, fmt);
   vfprintf(stderr, fmt, ap);
   va_end(ap);
   fputc('\n', stderr);
}

static int
allocate(const struct alloc_request *req, struct alloc_reply *reply)
{
   unsigned i = 0;
   while (i < sizeof(layouts) / sizeof(layouts[0]) && layouts[i].format != req->format)
      i++;
   if (req->version != ALLOC_VERSION || !req->width || !req->height ||
       req->width > ALLOC_MAX_DIMENSION || req->height > ALLOC_MAX_DIMENSION ||
       i == sizeof(layouts) / sizeof(layouts[0]) || !layouts[i].count)
      return -EINVAL;
   bool nv12 = req->format == DRM_FORMAT_NV12;
   if (nv12 && (req->width % 2 || req->height % 2))
      return -EINVAL;

   struct gbm_bo *bo = gbm_bo_create_with_modifiers2(gbm, req->width, req->height, req->format,
                                                     layouts[i].modifiers, layouts[i].count,
                                                     GBM_BO_USE_RENDERING);
   if (!bo)
      return -errno ? -errno : -ENOMEM;
   int fd = gbm_bo_get_fd(bo);
   /* both NV12 planes in one dma-buf, with one stride as gralloc records it */
   bool layout_ok = nv12 ? gbm_bo_get_plane_count(bo) == 2 &&
                              gbm_bo_get_stride_for_plane(bo, 1) == gbm_bo_get_stride_for_plane(bo, 0)
                         : gbm_bo_get_plane_count(bo) == 1;
   if (!layout_ok || fd < 0) {
      if (fd >= 0)
         close(fd);
      gbm_bo_destroy(bo);
      return -EINVAL;
   }
   reply->stride = gbm_bo_get_stride_for_plane(bo, 0);
   reply->offset = gbm_bo_get_offset(bo, 0);
   reply->offset1 = nv12 ? gbm_bo_get_offset(bo, 1) : 0;
   reply->modifier = gbm_bo_get_modifier(bo);
   /* the dma-buf keeps the memory alive after the bo is gone */
   gbm_bo_destroy(bo);
   off_t size = lseek(fd, 0, SEEK_END);
   reply->size = size > 0 ? (uint64_t)size : 0;
   lseek(fd, 0, SEEK_SET);
   return fd;
}

static void
serve(int fd)
{
   struct ucred cred;
   socklen_t len = sizeof(cred);
   if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) < 0 || cred.uid != allowed_uid) {
      log_msg("refused a connection from uid %d", len == sizeof(cred) ? (int)cred.uid : -1);
      return;
   }
   struct timeval tv = { 1, 0 };
   setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
   setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

   struct alloc_request req;
   if (recv(fd, &req, sizeof(req), MSG_WAITALL) != sizeof(req))
      return;
   struct alloc_reply reply = { 0 };
   int buf = allocate(&req, &reply);
   reply.status = buf < 0 ? buf : 0;
   if (buf < 0)
      log_msg("%ux%u format 0x%x: %s", req.width, req.height, req.format, strerror(-buf));

   union {
      char buf[CMSG_SPACE(sizeof(int))];
      struct cmsghdr align;
   } control;
   struct iovec iov = { .iov_base = &reply, .iov_len = sizeof(reply) };
   struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1 };
   if (buf >= 0) {
      msg.msg_control = control.buf;
      msg.msg_controllen = sizeof(control.buf);
      struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
      c->cmsg_level = SOL_SOCKET;
      c->cmsg_type = SCM_RIGHTS;
      c->cmsg_len = CMSG_LEN(sizeof(int));
      memcpy(CMSG_DATA(c), &buf, sizeof(int));
   }
   if (sendmsg(fd, &msg, MSG_NOSIGNAL) != sizeof(reply))
      log_msg("sending a reply failed: %s", strerror(errno));
   if (buf >= 0)
      close(buf);
}

/* The render node of the NVIDIA GPU, which Venus renders on.  Render node
 * numbers follow the probe order, which changes with a laptop's MUX mode. */
static const char *
nvidia_render_node(void)
{
   static char node[64] = "/dev/dri/renderD128";
   glob_t g;
   if (glob("/sys/class/drm/renderD*", 0, NULL, &g) != 0)
      return node;
   for (size_t i = 0; i < g.gl_pathc; i++) {
      char link[PATH_MAX], driver[PATH_MAX];
      snprintf(link, sizeof(link), "%s/device/driver", g.gl_pathv[i]);
      if (!realpath(link, driver) || strcmp(strrchr(driver, '/') + 1, "nvidia") != 0)
         continue;
      snprintf(node, sizeof(node), "/dev/dri/%s", strrchr(g.gl_pathv[i], '/') + 1);
      break;
   }
   globfree(&g);
   return node;
}

int
main(int argc, char **argv)
{
   const char *path = NULL, *device = NULL;
   static const struct option options[] = {
      { "socket-path", required_argument, NULL, 's' },
      { "allowed-uid", required_argument, NULL, 'u' },
      { "device", required_argument, NULL, 'd' },
      { 0 },
   };
   int opt;
   while ((opt = getopt_long(argc, argv, "", options, NULL)) != -1) {
      switch (opt) {
      case 's':
         path = optarg;
         break;
      case 'u':
         allowed_uid = strtoul(optarg, NULL, 10);
         break;
      case 'd':
         device = optarg;
         break;
      default:
         return 2;
      }
   }
   struct sockaddr_un addr = { .sun_family = AF_UNIX };
   if (!path || strlen(path) >= sizeof(addr.sun_path)) {
      log_msg("usage: %s --socket-path PATH [--allowed-uid UID] [--device NODE]", argv[0]);
      return 2;
   }
   signal(SIGPIPE, SIG_IGN);

   if (!device)
      device = nvidia_render_node();
   int dri = open(device, O_RDWR | O_CLOEXEC);
   gbm = dri >= 0 ? gbm_create_device(dri) : NULL;
   if (!gbm) {
      log_msg("can't open %s for GBM: %s", device, strerror(errno));
      return 1;
   }
   if (!query_layouts()) {
      log_msg("can't query the GPU's buffer layouts through EGL");
      return 1;
   }

   int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
   strcpy(addr.sun_path, path);
   unlink(path);
   /* any local user may connect; serve() admits only allowed_uid */
   mode_t old = umask(0111);
   int rc = bind(listener, (struct sockaddr *)&addr, sizeof(addr));
   umask(old);
   if (rc < 0 || listen(listener, 16) < 0) {
      log_msg("can't listen on %s: %s", path, strerror(errno));
      return 1;
   }
   log_msg("allocating with %s on %s for uid %d at %s", gbm_device_get_backend_name(gbm), device,
           (int)allowed_uid, path);
   for (;;) {
      int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
      if (fd < 0) {
         if (errno == EINTR || errno == ECONNABORTED)
            continue;
         log_msg("accept: %s", strerror(errno));
         return 1;
      }
      serve(fd);
      close(fd);
   }
}
