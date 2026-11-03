// Replaces AOSP's framebuffer.cpp.  Waydroid has no /dev/graphics/fb0 and
// displays through its hwcomposer, so the framebuffer device is never used.
#include <errno.h>

#include <hardware/gralloc.h>
#include <log/log.h>

#include "gr.h"
#include "gralloc_priv.h"

int fb_device_open(const hw_module_t *, const char *, hw_device_t **)
{
    return -ENODEV;
}

int mapFrameBufferLocked(struct private_module_t *, int)
{
    return -ENODEV;
}
