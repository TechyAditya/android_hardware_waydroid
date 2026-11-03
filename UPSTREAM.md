# Upstream pull requests and issues

This file holds the drafts for every pull request and issue that this fork plans to send upstream. Only [waydroid/android_hardware_waydroid#92](https://github.com/waydroid/android_hardware_waydroid/pull/92) is open. Nothing else has been submitted.

The head branches are on `TechyAditya/android_hardware_waydroid` unless a row says otherwise. Each `pr/` branch starts from upstream `lineage-20` and was compiled alone with `stack/build.sh`. The changes ran together in the `all-fixes` build, not branch by branch.

| Draft | Target | Head | Base | Ready |
| --- | --- | --- | --- | --- |
| [Rate-limit resize hotplugs](#rate-limit-resize-hotplugs-open-as-92) | `waydroid/android_hardware_waydroid` | `resize-interval` | `lineage-20` | Open as #92. Its description needs the update below. |
| [Wait for the acquire fence before a wl_shm copy](#wait-for-the-acquire-fence-before-a-wl_shm-copy) | `waydroid/android_hardware_waydroid` | `shm-copy-fence` | `lineage-20` | Yes |
| [Single-window display size, lifetime, and focus](#single-window-display-size-lifetime-and-focus) | `waydroid/android_hardware_waydroid` | `pr/single-window` | `lineage-20` | Yes |
| [Close only the full UI's tasks](#close-only-the-full-uis-tasks) | `waydroid/android_hardware_waydroid` | `pr/full-ui-close` | `lineage-20` | After the single-window PR merges |
| [A display config per monitor refresh rate](#a-display-config-per-monitor-refresh-rate) | `waydroid/android_hardware_waydroid` | `pr/refresh-rates` | `lineage-20` | Yes |
| [HDR video on HDR outputs](#hdr-video-on-hdr-outputs) | `waydroid/android_hardware_waydroid` and `waydroid/android_vendor_waydroid` | `pr/hdr-output` and `TechyAditya/android_vendor_waydroid` `hdr` | `lineage-20` | Yes, as two linked PRs |
| [Audio buffer and period by duration](#audio-buffer-and-period-by-duration) | `waydroid/android_hardware_waydroid` | `audio-rate-buffer` | `lineage-20` | After #91 merges |
| [Present GPU-memory buffers as dma-bufs](#present-gpu-memory-buffers-as-dma-bufs) | `waydroid/android_hardware_waydroid` | `pr/gpu-memory-dmabuf` | `lineage-20` | No. Upstream has no gralloc that sets the flag. |
| [NVIDIA misreads two-plane NV12 imports](#nvidia-misreads-two-plane-nv12-imports) | NVIDIA developer forums, Linux | none | none | Yes |
| [SDR video looks washed out](#sdr-video-looks-washed-out) | `waydroid/waydroid` | none | none | No. Check stock Waydroid first. |

To open a pull request from a draft, copy its title and body:

```sh
gh pr create -R waydroid/android_hardware_waydroid -B lineage-20 -H TechyAditya:pr/single-window \
	--title "TITLE" --body-file BODY.md
```

## Rate-limit resize hotplugs (open as #92)

The branch has one commit that the description does not cover yet: `hwcomposer: scale the wl_pointer cursor like the other layers`. Replace the last sentence of change 2 in the description, "The cursor keeps the output scale, and nothing is stretched without `wp_viewporter`.", with:

```markdown
The cursor surface and its hotspot use the same content scale as the other layers. Nothing is stretched without `wp_viewporter`.
```

Then add the commit as change 3:

```markdown
3. `hwcomposer: scale the wl_pointer cursor like the other layers` gives the cursor the stretched content scale from `effective_scale()` while a resize hotplug is pending. Before, the cursor and its hotspot kept the display scale and drifted from the stretched frame.
```

## Wait for the acquire fence before a wl_shm copy

Title: `hwcomposer: wait for a layer's acquire fence before copying it to wl_shm`

```markdown
## Problem

`get_wl_buffer()` copies a layer into its wl_shm buffer before `apply_hwc_layer_to_surface_context()` waits on the layer's acquire fence. The copy can read a buffer that the GPU is still drawing. With `gralloc.default`, which Waydroid uses on NVIDIA, the client target always takes this path. Drawing starts with a clear to black, so the full UI showed partly black frames during animations.

## Changes

1. `hwcomposer: wait for a layer's acquire fence before copying it to wl_shm` waits on the fence before the copy.
2. `hwcomposer: skip a layer that isn't ready instead of copying it` skips the layer for that frame when the fence does not signal within 100 ms. The compositor keeps showing the previous frame. Before, the wait only logged the timeout and copied anyway.

## Testing

I recorded the full UI window at full frame rate while swiping between launcher pages 16 times, on Hyprland 0.57, CachyOS, an NVIDIA RTX 3060 (driver 615), and LineageOS 20 images.

| Build | Frames recorded | Frames more than 20% black |
| --- | --- | --- |
| Before | 740 | 608 |
| After | 822 | 0 |

Frame pacing averaged 17 ms with the change. I built the module outside the AOSP tree against the Android 13 VNDK v33 headers, so it has not been through a Soong build.
```

## Single-window display size, lifetime, and focus

Title: `hwcomposer: fix display size, window lifetime, and focus in single-window mode`

```markdown
## Problem

With `persist.waydroid.multi_windows=false`, every app window shows the same Android display. Three things go wrong when there is more than one window:

- Each `xdg_toplevel` configure resizes the display. When focus moves between two windows of different sizes, the configure that arrives last wins, often the one for the window that loses focus. The focused app renders at the wrong size and is cropped.
- A window stays open with a stale snapshot after Android leaves its task, for example after Back on the app's root activity.
- Wayland calls listeners without a lock, so `hwc_set()` can destroy a window while one of its handlers still reads it.

## Changes

1. `hwcomposer: size the display from the focused window in single-window mode` remembers each window's configured size. Only the window with `XDG_TOPLEVEL_STATE_ACTIVATED` sets the display size. Windows that lose focus keep their last frame at their own size.
2. `hwcomposer: close a single-window app window when Android leaves its task` closes the window of the task Android left when it returns to a task that has a window, as multi-window mode does. A switch that `waydroid app launch` or a pending `setFocusedTask()` caused keeps both windows.
3. `hwcomposer: fix window lifetime and focus races in single-window mode` gives listeners a window ID that is never reused. Each handler looks the window up under `liveWindowsMutex` and uses it only while it holds the lock. The commit message lists four focus fixes that come with it.

Multi-window mode is unchanged.

## Testing

These commits ran as part of a larger build, on Hyprland 0.57, CachyOS, an NVIDIA RTX 3060 (driver 615), and LineageOS 20 images. This branch alone was compiled, not run. I built the module outside the AOSP tree against the Android 13 VNDK v33 headers, so it has not been through a Soong build.
```

## Close only the full UI's tasks

Open this after the single-window PR merges. The branch contains that PR's three commits, and the new commits are the last two.

Title: `hwcomposer: close only the full UI's tasks with its window`

```markdown
## Problem

Closing the full UI window calls `removeAllVisibleRecentTasks()`. That also closes apps that other displays show in windows of their own.

## Changes

1. `hwcomposer: close only the full UI's tasks with its window` records the tasks that have a layer in the primary display's latest frame and removes only those. Removing the home task this way is harmless, because Android recreates it right away.
2. `hwcomposer: parse task ids without std::stoi` parses the task IDs from layer names without exceptions. The module is built without exceptions, so `std::stoi` on a malformed ID would abort it.

Apps left in the background of the full UI now stay running after it closes.

## Testing

On Hyprland 0.57 with LineageOS 20 images:

- An app shown on another display, in its own window, kept running when the full UI window closed.
- Settings, open in the full UI, closed with it.
```

## A display config per monitor refresh rate

Title: `hwcomposer: offer a display config for each monitor refresh rate`

```markdown
## Problem

The primary display has one config. Its vsync period is fixed at boot from the highest mode that any `wl_output` advertises, rounded to whole Hz. Android cannot follow a window to a monitor with another rate, and a 165.018 Hz monitor gets a 165 Hz period.

## Changes

The composer tracks each output's current mode and offers one config per refresh rate seen since it started, with the exact period (165.018 Hz is 6059945 ns). `setActiveConfig` switches the vsync thread's period, so Android changes rate with a normal mode switch that `DisplayModeDirector` picks. Apps and the `min_refresh_rate` and `peak_refresh_rate` settings choose among the configs.

A rate not seen before is added with a primary display hotplug. The list never shrinks, so a hotplug happens only when a monitor with a new rate appears. Switching rates by hotplug instead stalled apps on virtual displays when it coincided with a density change.

## Testing

I used this as part of a larger build on Hyprland 0.57 with a 165 Hz laptop panel and LineageOS 20 images. This branch alone was compiled, not run, and it has not been through a Soong build.
```

## HDR video on HDR outputs

This is two pull requests that need each other. Open the vendor one first, then link each from the other.

`waydroid/android_vendor_waydroid`, head `TechyAditya:hdr`, title: `waydroid-patches: hwc2on1: pass HDR capabilities and layer dataspaces`

`waydroid/android_hardware_waydroid`, head `TechyAditya:pr/hdr-output`, title: `hwcomposer: show HDR video on HDR outputs`

```markdown
## Problem

The composer HAL is 2.1 behind `HWC2On1Adapter`, so Android sees no HDR display and SurfaceFlinger tone-maps HDR video to SDR.

## Changes

- The adapter patch in android_vendor_waydroid fetches the HDR capabilities and each layer's dataspace through two functions that this module exports, `waydroid_hwc_get_hdr_capabilities` and `waydroid_hwc_set_layer_dataspaces`. Without the adapter patch, nothing changes.
- The composer reads the host output's image description through `wp_color_management_v1`. When the output uses PQ or HLG, it reports HDR10 and HLG with the output's luminance.
- With HDR reported, an opaque, untransformed P010 layer becomes an overlay. The composer converts it from Y'CbCr to R'G'B', keeping its PQ or HLG encoding, into a 10-bit `XBGR2101010` wl_shm buffer on a subsurface below the client target, and tags it with a BT.2020 PQ or HLG image description. The compositor maps it to the output.
- `color-management-v1.xml` comes from wayland-protocols 1.50, which is newer than `external/wayland-protocols`.
- `persist.waydroid.no_hdr` turns it off.

Converting a 4K frame shown at 2560x1440 takes about 11 ms on 8 threads. A frame already on screen is not converted again.

## Testing

I tested on one HDR panel with Hyprland's HDR mode on, Hyprland 0.57, and LineageOS 20 images. Outputs in SDR mode report no HDR, so nothing changes there. This branch alone was compiled, not run.
```

## Audio buffer and period by duration

Open this after [#91](https://github.com/waydroid/android_hardware_waydroid/pull/91) merges, or offer it on #91 as a follow-up. Its first commit restores the 4-period buffer from #91.

Title: `audio: size the playback buffer and period by duration`

```markdown
## Problem

`out_get_buffer_size()` reports `PLAYBACK_PERIOD_SIZE` frames at every sample rate. That is 21 ms at 48 kHz but 5.3 ms at 192 kHz. With a HAL period that much shorter than its normal mix, AudioFlinger adds a fast mixer, and a normal mix that runs late plays as a gap of silence. At 192 kHz that was a crackle every few minutes.

## Changes

1. `audio: size the playback buffer by duration, not frames` keeps the 4-period buffer from #91 at about 85 ms at every rate.
2. `audio: scale the period with the sample rate, not only the buffer` keeps the period at its 48 kHz duration, rounded up to 16 frames.

At 48 kHz and below, nothing changes.

## Testing

TESTING: fill in from a 30-minute 192 kHz run on each build, with the crackle count and the fast mixer's late and early cycles from `dumpsys media.audio_flinger`.
```

Before you post it, replace the `TESTING:` line with measured results. No before-and-after counts are recorded yet.

## Present GPU-memory buffers as dma-bufs

Hold this one. The composer change sends buffers flagged `PRIV_FLAGS_GPU_MEMORY` to the compositor as dma-bufs. Upstream `gralloc.default` never sets that flag, so the change does nothing there until a gralloc that allocates in GPU memory, such as the one in `waydroid-venus`, is upstream too.

## NVIDIA misreads two-plane NV12 imports

Post this in the [NVIDIA developer forums, Linux category](https://forums.developer.nvidia.com/c/gpu-graphics/linux/148). Attach the log that `sudo nvidia-bug-report.sh` writes.

Title: `Vulkan: some NV12 dma-bufs imported as two-plane images read back wrong (615.78.08)`

````markdown
Some NV12 dma-bufs that GBM allocates read back wrong when Vulkan imports them as `VK_FORMAT_G8_B8R8_2PLANE_420_UNORM` images with their DRM format modifier. Which buffers fail depends on the resolution and on the import order, and the same buffers fail every run. The same buffers imported as one R8 image and one R8G8 image per plane read back correctly.

### System

RTX 3060 Laptop GPU, driver 615.78.08, Linux 7.3, Arch Linux (CachyOS). GBM gives the buffers the modifier `0x0300000000606014`.

### Reproducer

[nv12-import.c](https://github.com/TechyAditya/waydroid-venus/blob/master/hwdec/test/nv12-import.c) allocates 40 NV12 buffers with GBM, imports each into one Vulkan device, copies a pattern into both planes, copies it back, and compares.

```sh
cc -O2 -o nv12-import nv12-import.c $(pkg-config --cflags --libs gbm libdrm vulkan)
./nv12-import 1920 1080 40
./nv12-import 2560 1440 40
SPLIT=1 ./nv12-import 1920 1080 40
```

### Result

| Size | Two-plane import, buffers wrong | One image per plane |
| --- | --- | --- |
| 1280x720 | none | none |
| 1920x1080 | 6 and 38 (luma and chroma), 32 (chroma only) | none |
| 2560x1440 | 1, 17, and 33 (all chroma), 11 and 27 (86% of chroma) | none |

### What changes which buffers fail

These leave the failing buffers where they are: dedicated or shared memory, importing the whole dma-buf size, binding each plane to its own memory (`VK_IMAGE_CREATE_DISJOINT_BIT`), letting the driver choose the plane layout, sampling instead of copying, and extra allocations or imports before the test.

These move them: the resolution, the allocation method, and the import order. With linear NV12 buffers allocated through Vulkan, 1920x1080 reads back correctly, but at 2560x1440 buffers 10 and 26 lose 75% of their chroma. The failures repeat every 16 buffers at 2560x1440 and every 32 at 1920x1080. If each import takes a power-of-two block of GPU address space, both periods are 128 MB.

### Impact

Waydroid's compositor reads Android's video buffers this way through Venus, so some frames show as solid green or without colour.
````

## SDR video looks washed out

Hold this until it is confirmed on stock Waydroid. In this fork's build, Android lifted the shadows of SDR video. A grey bar at 17 of 255 reached the screen at 31, and one at 34 at 50. The lift followed the layer's SMPTE 170M dataspace, not the pixels: the same frames tagged as sRGB showed correctly. Setting `debug.sf.treat_170m_as_sRGB` did not change it. This fork works around it in its host decoder, which tags SDR video as sRGB, as Chromium does on Android.

To check stock Waydroid, play a video in an Android player with the stock decoders, then run `waydroid-venus/hwdec/test/ramp-check.sh`. If the dark bars read high, write the issue with those numbers. Name the composition path the video layer takes (`dumpsys SurfaceFlinger`, the layer's `composition type`), because the lift happens after the decoder.
