# Upstream pull requests and issues

What this fork plans to send to Waydroid, in plain words. Open so far: [#92](https://github.com/waydroid/android_hardware_waydroid/pull/92), [android_vendor_waydroid#57](https://github.com/waydroid/android_vendor_waydroid/pull/57), and [#93](https://github.com/waydroid/android_hardware_waydroid/pull/93) for issue [waydroid#2470](https://github.com/waydroid/waydroid/issues/2470), with a reply posted on [#2464](https://github.com/waydroid/waydroid/issues/2464#issuecomment-6617138955). Nothing else has been posted.

1. **Smoother window resizing, without the crash.** Dragging a window edge made Android rebuild its screen about 150 times in 8 seconds, and Android's graphics crashed during the rebuilds ([#2464](https://github.com/waydroid/waydroid/issues/2464)). [#92](https://github.com/waydroid/android_hardware_waydroid/pull/92) cuts the rebuilds to about 15 and stretches the old frame meanwhile. It is open. [android_vendor_waydroid#57](https://github.com/waydroid/android_vendor_waydroid/pull/57), also open, adds the missing null check that SurfaceFlinger crashed on. After both are merged, ask the reporter to confirm that the SurfaceFlinger crash and the composer deaths are both gone, then close #2464. [Drafts](#1-resizing-and-the-surfaceflinger-crash)
2. **No more black flashes in the full Waydroid window.** The window was shown before Android finished drawing it. One PR. [Draft](#2-black-flashes-in-the-full-ui)
3. **Several app windows behave.** Each window keeps its own size and closes with its app. Closing the full Waydroid window closes only its own apps. Android follows the monitor's refresh rate. Open as issue [waydroid#2470](https://github.com/waydroid/waydroid/issues/2470) and PR [#93](https://github.com/waydroid/android_hardware_waydroid/pull/93). [Drafts](#3-several-app-windows)
4. **HDR video shows as HDR** when the compositor runs the monitor in HDR mode. One issue and two PRs, one in each repository. [Drafts](#4-hdr-video)
5. **No audio crackles at high sample rates.** A comment on [#91](https://github.com/waydroid/android_hardware_waydroid/pull/91), someone else's open audio PR, which already has half of the fix. [Draft](#5-audio-at-high-sample-rates)
6. **Video buffers in GPU memory go straight to the compositor.** On hold: upstream has nothing that creates those buffers yet. [Note](#6-gpu-memory-buffers-as-dma-bufs)
7. **Bug report to NVIDIA.** Their driver misreads some video buffers, which shows as green or colourless frames. Ready, with a small program that shows the bug. [Draft](#7-nvidia-misreads-two-plane-nv12-imports)
8. **Washed-out SDR video.** Android brightens the dark parts of ordinary video. On hold until it is confirmed on stock Waydroid. [Note](#8-washed-out-sdr-video)

Every head branch below starts from upstream `lineage-20` and is on `TechyAditya/android_hardware_waydroid` unless it says otherwise. Each one compiles alone with `stack/build.sh`. The changes ran together in the `all-fixes` build, not branch by branch. To open a pull request from a draft, save the body to a file and run:

```sh
gh pr create -R waydroid/REPO -B lineage-20 -H TechyAditya:BRANCH --title "TITLE" --body-file BODY.md
```

## 1. Resizing and the SurfaceFlinger crash

[#92](https://github.com/waydroid/android_hardware_waydroid/pull/92) is open. Its description is current, and every review thread is resolved. It mitigates #2464 but does not fix it: the option is off by default, and a launch still reconnects the display once.

#2464 can close once both PRs are merged and the reporter confirms that both crashes are gone: the SurfaceFlinger SIGSEGV, and the composer deaths (SIGABRT, signal 13) after which SurfaceFlinger aborts. The SurfaceFlinger patch fixes the first. The composer deaths have no confirmed cause; #92 made them stop on this setup. So neither PR says "Fixes", which would close the issue on merge. Close it by hand after the reporter replies.

### PR: SurfaceFlinger null check (open as android_vendor_waydroid#57)

`waydroid/android_vendor_waydroid`, head `TechyAditya:sf-default-display`. The branch adds `waydroid-patches/base-patches-33/frameworks/native/0016-surfaceflinger-Check-for-a-missing-default-display-i.patch`. All 16 patches apply to LineageOS `android_frameworks_native` `lineage-20.0` (`915ad642`). It has not been built in an AOSP tree. The 5-byte binary patch that `stack/surfaceflinger.sh` installs does the same thing and has run since 2026-11-02.

Title: `waydroid-patches: surfaceflinger: Check for a missing default display`

```markdown
Addresses waydroid/waydroid#2464, together with waydroid/android_hardware_waydroid#92.

## Problem

`SurfaceFlinger::getPhysicalDisplayIdsLocked()` calls `getDefaultDisplayDeviceLocked()->getPhysicalId()` without a check. Resizing a Waydroid window reconnects the primary display, and while it reconnects there is no default display. A `getPhysicalDisplayIds()` call in that gap, which DisplayManager makes after every reconnect, crashes SurfaceFlinger in `DisplayDevice::getId()`. That is the SIGSEGV in waydroid/waydroid#2464, at `surfaceflinger + 0x1579f0`. AOSP has the same code up to `main`, because phones do not disconnect their primary display.

## Change

`0016-surfaceflinger-Check-for-a-missing-default-display-i.patch` returns the other physical displays when there is no default display. With a default display, the result is unchanged.

waydroid/android_hardware_waydroid#92 makes the gap rarer by rate-limiting the reconnects during a drag. This patch removes the crash, including the reconnect at launch.

## Testing

The patch applies after patches 0001 to 0015 on LineageOS `android_frameworks_native` `lineage-20.0`. I have not built it in an AOSP tree. Since 2026-11-02 I have run the same check as a 5-byte change to the `20260927` x86_64 SurfaceFlinger binary, on Hyprland 0.57 with LineageOS 20 images. Before it, slow window drags crashed SurfaceFlinger in `getPhysicalDisplayIds`. Since then, systemd-coredump has recorded no SurfaceFlinger crash.
```

### Comment: answer the reporter on #2464

Posted on 2026-11-04 as [this comment](https://github.com/waydroid/waydroid/issues/2464#issuecomment-6617138955). The reporter had asked on 2026-11-03 whether to switch away from the Android 13 image.

```markdown
@CodeWithKhan07 No need to change images. The crash is in Android 13's SurfaceFlinger, and the LineageOS 20 images are the current x86_64 builds. Your report has two kinds of crash, and there is now a fix for each to try:

1. The SurfaceFlinger SIGSEGV in `getPhysicalDisplayIds`. waydroid/android_vendor_waydroid#57 adds the missing null check. Until an image has it, step 3 of my comment installs the same check as a binary patch. That patch matches only the `20260927` system image, and yours is that image: `system_datetime = 1790512621` is 2026-09-27.
2. The composer deaths (SIGABRT, and signal 13 at 10:37:16), after which SurfaceFlinger aborts. I could not find their cause, but they stopped for me with waydroid/android_hardware_waydroid#92 and `persist.waydroid.resize_interval_ms = 500` (step 2). Your Intel GPU uses `gralloc=gbm`, and the composer from that PR works with it.

Could you try both and say whether launching and resizing still crash? If either crash comes back, `coredumpctl list` and `waydroid logcat` around it would help. I only tested on Hyprland 0.57 with an NVIDIA GPU.
```

## 2. Black flashes in the full UI

`waydroid/android_hardware_waydroid`, head `TechyAditya:shm-copy-fence`.

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

## 3. Several app windows

Posted on 2026-11-04 as issue [waydroid/waydroid#2470](https://github.com/waydroid/waydroid/issues/2470) and PR [waydroid/android_hardware_waydroid#93](https://github.com/waydroid/android_hardware_waydroid/pull/93), head `TechyAditya:pr/multi-window`. The branch has the three single-window commits, the two full UI commits, and the refresh rate commit.

### Issue

Title: `[BUG] App windows: multi-window mode can't resize apps, and single-window mode mixes up sizes, keeps stale windows, closes every app with the full UI, and fixes the refresh rate at boot`

```markdown
**Describe the bug**

In multi-window mode (`persist.waydroid.multi_windows=true`), every app window is a window on one shared Android display. On Hyprland:

1. Resizing an app window resizes the whole Android display, not the app. Windows keep the size they started with or clip: an app started in a half-screen window stays half-size when maximized, and an app started full-size clips when tiled to half.
2. Android draws its own window shadows on that display, and they show around the host windows.
3. The on-screen keyboard opens as a new host window. When the compositor tiles it, the display resizes, and SurfaceFlinger can crash in `getPhysicalDisplayIds` (#2464).

#2426 and #2436 report other problems in this mode: focus stealing and a hotplug loop on app launch.

Single-window mode (`persist.waydroid.multi_windows=false`) avoids those, because the display always matches the focused window. With more than one app window, though:

4. Every app window shows the same Android display, and every `xdg_toplevel` configure resizes it. When focus moves between two windows of different sizes, the configure that arrives last wins, often the one for the window that loses focus. The focused app renders at the wrong size and is cropped.
5. A window stays open with its last frame after Android leaves its task, for example after Back on the app's root activity.
6. The composer's Wayland handlers can read a window after `hwc_set()` destroyed it, because Wayland calls listeners without a lock. I found this in review and did not see a crash from it.

In both modes:

7. Closing the full UI window (`waydroid show-full-ui`) calls `removeAllVisibleRecentTasks()`, which also closes apps shown in windows of their own on other displays.
8. The primary display has one config. Its vsync period is fixed at boot from the highest mode any output advertises, rounded to whole Hz. Android cannot follow a window to a monitor with another rate, and a 165.018 Hz panel gets a 165 Hz period. #973 reports the same fixed rate with two monitors.

**To reproduce**

1. Multi-window mode: open an app, then resize or tile its window. The app keeps its first size.
2. Single-window mode: open two apps in windows of different sizes and click between them. The focused app is cropped to the other window's size.
3. Single-window mode: press Back on an app's root activity. Its window stays open.
4. Open an app in its own window, then open and close `waydroid show-full-ui`. The app closes too.

**Expected behavior**

Each window shows its app at its own size and closes with its app. Closing the full UI closes only the apps it shows. Android runs at the refresh rate of the monitor that shows it.

**General information**

- Waydroid 1.6.3, LineageOS 20 images (system 20260927, vendor 20261022)
- Hyprland 0.57 on CachyOS, NVIDIA RTX 3060 Laptop GPU (driver 615), 165 Hz panel

waydroid/android_hardware_waydroid#93 fixes items 4 to 8.
```

### PR

Title: `hwcomposer: fix window size, lifetime and focus, full UI close, and refresh rate with several windows`

```markdown
Fixes items 4 to 8 of waydroid/waydroid#2470. Items 1 to 3 are multi-window mode problems that this PR does not change. For the resize crash, see waydroid/waydroid#2464 and #92.

## Changes

Single-window mode:

1. `hwcomposer: size the display from the focused window in single-window mode` remembers each window's configured size. Only the window with `XDG_TOPLEVEL_STATE_ACTIVATED` sets the display size, and windows that lose focus keep their last frame at their own size.
2. `hwcomposer: close a single-window app window when Android leaves its task` closes the window of the task Android left when it returns to a task that has a window, as multi-window mode does. A switch that `waydroid app launch` or a pending `setFocusedTask()` caused keeps both windows.
3. `hwcomposer: fix window lifetime and focus races in single-window mode` gives listeners a window ID that is never reused. Each handler looks the window up under `liveWindowsMutex` and uses it only while it holds the lock. The commit message lists four focus fixes that come with it.

Full UI:

4. `hwcomposer: close only the full UI's tasks with its window` records the tasks with a layer in the primary display's latest frame and removes only those. Removing the home task this way is harmless, because Android recreates it right away.
5. `hwcomposer: parse task ids without std::stoi` parses the task IDs from layer names without exceptions. The module is built without exceptions, so `std::stoi` on a malformed ID would abort it.

Refresh rate:

6. `hwcomposer: offer a display config for each monitor refresh rate` tracks each output's current mode and offers one config per rate seen since the composer started, with the exact period (165.018 Hz is 6059945 ns). `setActiveConfig` switches the vsync period, so Android changes rate with a normal mode switch. A new rate is added with a hotplug, and the list never shrinks.

Multi-window mode is unchanged, apart from commit 6, which applies to every mode.

## Testing

These commits ran together in my build on Hyprland 0.57, CachyOS, an NVIDIA RTX 3060 (driver 615), and LineageOS 20 images:

- An app shown in its own window keeps running when the full UI window closes, and Settings opened in the full UI closes with it.
- `dumpsys SurfaceFlinger` shows `refresh-rate: 165.02 Hz` on the 165 Hz panel.

This branch alone was compiled, not run. I built the module outside the AOSP tree against the Android 13 VNDK v33 headers, so it has not been through a Soong build. I tested one monitor, not the two-monitor case in waydroid/waydroid#973.
```

## 4. HDR video

One issue in `waydroid/waydroid` and two PRs: `waydroid/android_vendor_waydroid` from `TechyAditya:hdr`, and `waydroid/android_hardware_waydroid` from `TechyAditya:pr/hdr-output`. Open the vendor PR first, then link each PR from the other.

### Issue

Title: `[Feature] Show HDR video as HDR on HDR outputs`

```markdown
**Describe the bug**

When the Wayland compositor runs the output in HDR mode, for example Hyprland with HDR on, Waydroid still shows HDR10 and HLG video as SDR. The composer HAL is 2.1, behind `HWC2On1Adapter`, so Android sees no HDR display and SurfaceFlinger tone-maps every HDR video to SDR. This HAL can neither output PQ nor pass HDR metadata through.

**Expected behavior**

On an HDR output, Android reports HDR10 and HLG, and HDR video reaches the compositor with its PQ or HLG encoding, so the compositor maps it to the output.

**General information**

- Waydroid 1.6.3, LineageOS 20 images (system 20260927, vendor 20261022)
- Hyprland 0.57 with HDR mode on, CachyOS

waydroid/android_vendor_waydroid#PR1 and waydroid/android_hardware_waydroid#PR2 implement it.
```

### PR 1: the adapter

Title: `waydroid-patches: hwc2on1: pass HDR capabilities and layer dataspaces`

```markdown
Part of waydroid/waydroid#ISSUE. Needs waydroid/android_hardware_waydroid#PR2; without it, this patch changes nothing.

`HWC2On1Adapter` gives SurfaceFlinger no HDR capabilities and the composer no layer dataspaces. The patch fetches both through two functions the Waydroid composer exports, `waydroid_hwc_get_hdr_capabilities` and `waydroid_hwc_set_layer_dataspaces`. It looks them up with `dlsym` on the HWC1 module, so a composer without them keeps today's behavior.
```

### PR 2: the composer

Title: `hwcomposer: show HDR video on HDR outputs`

```markdown
Fixes waydroid/waydroid#ISSUE, together with waydroid/android_vendor_waydroid#PR1.

## Changes

- The composer reads the host output's image description through `wp_color_management_v1`. When the output uses PQ or HLG, it reports HDR10 and HLG with the output's luminance, through the two functions the adapter patch calls.
- With HDR reported, an opaque, untransformed P010 layer becomes an overlay. The composer converts it from Y'CbCr to R'G'B', keeping its PQ or HLG encoding, into a 10-bit `XBGR2101010` wl_shm buffer on a subsurface below the client target, and tags it with a BT.2020 PQ or HLG image description. The compositor maps it to the output.
- `color-management-v1.xml` comes from wayland-protocols 1.50, which is newer than `external/wayland-protocols`.
- `persist.waydroid.no_hdr` turns it off.

Converting a 4K frame shown at 2560x1440 takes about 11 ms on 8 threads. A frame already on screen is not converted again.

## Testing

Tested on one HDR panel with Hyprland's HDR mode on, Hyprland 0.57, and LineageOS 20 images. Outputs in SDR mode report no HDR, so nothing changes there. This branch alone was compiled, not run.
```

## 5. Audio at high sample rates

A comment on [#91](https://github.com/waydroid/android_hardware_waydroid/pull/91), not a new PR. #91 already scales the ALSA buffer with the sample rate, which its author adopted from an earlier comment of mine. The remaining piece is the period that `out_get_buffer_size()` reports, from `audio-rate-buffer`. Before posting, record a 192 kHz run with and without the change. No counts are recorded yet.

````markdown
Thanks for adding the scaled buffer. One more piece is needed at high rates. `out_get_buffer_size()` still reports `PLAYBACK_PERIOD_SIZE` frames at every rate: 21 ms at 48 kHz, but 5.3 ms at 192 kHz. With a HAL period that much shorter than its normal mix, AudioFlinger adds a fast mixer, and a normal mix that runs late then plays as a gap. At 192 kHz I heard a crackle every few minutes, and the fast mixer counted tens of thousands of late and early cycles.

Scaling the period the same way keeps it at its 48 kHz duration, and the buffer stays `period_count` periods:

```c
static snd_pcm_uframes_t playback_period_frames(const struct alsa_stream_out *out)
{
    uint64_t frames = out->config.period_size;
    if (out->config.rate > PLAYBACK_CODEC_SAMPLING_RATE)
        frames = (frames * out->config.rate + PLAYBACK_CODEC_SAMPLING_RATE - 1) / PLAYBACK_CODEC_SAMPLING_RATE;
    return (frames + 15) / 16 * 16;
}

static snd_pcm_uframes_t playback_buffer_frames(const struct alsa_stream_out *out)
{
    return playback_period_frames(out) * out->config.period_count;
}
```

```c
static size_t out_get_buffer_size(const struct audio_stream *stream)
{
    struct alsa_stream_out *out = (struct alsa_stream_out *)stream;
    size_t size = playback_period_frames(out) * audio_stream_out_frame_size((struct audio_stream_out *)stream);
    ALOGV("out_get_buffer_size: %zu", size);
    return size;
}
```

The rounding to 16 frames is the one `out_get_buffer_size()` already does, because AudioFlinger expects multiples of 16. At 48 kHz and below, nothing changes.
````

## 6. GPU-memory buffers as dma-bufs

On hold. `pr/gpu-memory-dmabuf` sends buffers flagged `PRIV_FLAGS_GPU_MEMORY` to the compositor as dma-bufs. Upstream `gralloc.default` never sets that flag, so the change does nothing there until a gralloc that allocates in GPU memory, such as the one in `waydroid-venus`, is upstream too.

## 7. NVIDIA misreads two-plane NV12 imports

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

## 8. Washed-out SDR video

On hold until it is confirmed on stock Waydroid. In this fork's build, Android lifted the shadows of SDR video. A grey bar at 17 of 255 reached the screen at 31, and one at 34 at 50. The lift followed the layer's SMPTE 170M dataspace, not the pixels: the same frames tagged as sRGB showed correctly. Setting `debug.sf.treat_170m_as_sRGB` did not change it. This fork works around it in its host decoder, which tags SDR video as sRGB, as Chromium does on Android.

To check stock Waydroid, play a video in an Android player with the stock decoders, then run `waydroid-venus/hwdec/test/ramp-check.sh`. If the dark bars read high, write the issue with those numbers. Name the composition path the video layer takes (`dumpsys SurfaceFlinger`, the layer's `composition type`), because the lift happens after the decoder.
