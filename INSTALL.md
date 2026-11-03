# Install the all-fixes Waydroid stack

This branch combines every fix from this fork into one checkout. It builds and installs four parts:

- `waydroid-venus/` runs Android's Vulkan on the host GPU through Venus. It also has a GPU memory allocator for gralloc and a host video decoder that writes frames straight into GPU memory.
- `stack/` builds the Wayland composer from `hwcomposer/`, the HWC2-on-HWC1 adapter, and the audio HAL from `audio/`.
- `waydroid-app-displays/` opens each Android app in its own host window.
- `hwcomposer/` and `audio/` are the composer and audio HAL sources that `stack/build.sh` compiles.

The composer changes are per-monitor refresh rates up to the display's own rate, rate-limited resize hotplugs, single-window focus fixes, GPU-memory dma-buf presentation, and HDR output. The audio HAL plays at the device's native sample rate with float output.

## Requirements

- An x86_64 Linux host with a Wayland compositor. The stack was tested on Arch Linux (CachyOS) with Hyprland and an NVIDIA RTX 3060 on driver 615.
- Waydroid initialized with the LineageOS 20 (Android 13) x86_64 images: `sudo waydroid init`.
- These Arch packages, or the same libraries on another distribution:

```sh
sudo pacman -S --needed base-devel git curl clang lld llvm meson ninja python-mako python-yaml \
	python-packaging libepoxy libdrm mesa ffmpeg vulkan-headers vulkan-icd-loader shaderc expat alsa-lib
```

The first build downloads about 1.5 GB of sources and toolchains into the ignored `build/` folders of each part, and uses about 5.5 GB of disk. On an 8-core laptop it takes about 6 minutes, most of it downloading.

## Build

1. Clone the branch:

   ```sh
   git clone -b all-fixes https://github.com/TechyAditya/android_hardware_waydroid.git waydroid-all-fixes
   cd waydroid-all-fixes
   ```

2. Start Waydroid. `stack/build.sh` links against the libraries of the installed image, so the image must be mounted while it runs:

   ```sh
   waydroid session start &
   ```

3. Build the three parts:

   ```sh
   ./waydroid-venus/build.sh
   ./stack/build.sh
   ./waydroid-app-displays/build.sh
   ```

   `stack/build.sh` puts its output in `stack/build/dist/vendor`, in the same layout as Waydroid's vendor overlay.

## Install

The last step restarts Android, which closes every open Android app.

1. Install the Venus services, gralloc, and the video decoders. This also sets the `ro.hardware.egl`, `ro.hardware.vulkan`, and codec properties in `/var/lib/waydroid/waydroid.cfg`:

   ```sh
   sudo ./waydroid-venus/install.sh
   ```

2. Install the composer, the adapter, and the audio HAL into `/var/lib/waydroid/overlay/vendor`:

   ```sh
   sudo ./stack/install.sh
   ```

   On the `20260927` x86_64 system image, this also installs a SurfaceFlinger with a null check for the resize crash in [waydroid/waydroid#2464](https://github.com/waydroid/waydroid/issues/2464). The patch changes 5 bytes and fits only that image, so on other images the script skips it and says so.

3. Install the per-app window service and the `waydroid app launch` hook in `/usr/local/bin/waydroid`:

   ```sh
   sudo ./waydroid-app-displays/install.sh
   ```

4. Restart Android:

   ```sh
   waydroid session stop
   waydroid session start &
   ```

The installers keep the files and properties they replace. Each part's `uninstall.sh` puts them back.

## Optional: install Widevine L3

Waydroid's image has only the ClearKey DRM, so apps that need Widevine, such as Netflix, refuse protected video. `install-widevine.sh` downloads Google's Widevine service from the prebuilt archive that [waydroid_script](https://github.com/casualsnek/waydroid_script) uses, checks its SHA-256, and copies it into the vendor overlay. The service is a proprietary Google binary, which is why this step is separate.

Widevine here runs at security level L3, in software. Streaming services usually limit L3 devices to standard definition.

1. Install Widevine. The script restarts a running Waydroid container, which closes every open Android app:

   ```sh
   sudo ./waydroid-venus/install-widevine.sh
   ```

2. Check that Android can start the Widevine service:

   ```sh
   sudo waydroid shell -- sh -c 'start vendor.drm-widevine-hal; sleep 1; service check android.hardware.drm.IDrmFactory/widevine'
   ```

   The output ends with `found`.

To remove Widevine, run `sudo ./waydroid-venus/uninstall-widevine.sh`. It also restarts the container.

## Run

- To open an app in its own window, run `waydroid app launch PACKAGE`, for example `waydroid app launch com.google.android.youtube`. Waydroid's desktop entries run the same command, so app launchers open separate windows too.
- To open the whole Android screen in one window, run `waydroid show-full-ui`.

The composer follows the refresh rate of the monitor that shows each window, so moving a window to another monitor changes Android's frame rate.

By default, the composer resizes Android's display on every resize event of its window. To resize it at most once every 500 milliseconds while you drag the window edge, set this property and restart Android:

```sh
waydroid prop set persist.waydroid.resize_interval_ms 500
```

Run `waydroid prop set` as the user that runs the session. With `sudo`, it looks for a session of root and changes nothing.

## Choose how 8-bit video reaches Android

The host decoder writes 8-bit (SDR) video into Android's buffers in one of three formats. The `persist.waydroid.video_sdr_output` property picks the format:

| Value | Buffers | Use it when |
| --- | --- | --- |
| `auto` (default) | `rgba` on NVIDIA, `nv12` on other GPUs | Always, unless one of the rows below applies. |
| `rgba` | RGBA in GPU memory. The host converts each frame to RGB. | You want the NVIDIA behaviour on another GPU. |
| `nv12` | NV12 in GPU memory, 2.7 times smaller than RGBA. | Your GPU reads NV12 correctly and you want to save memory. On NVIDIA driver 615 it shows green or colourless frames in some videos. |
| `system` | YV12 in system memory. The host copies each frame through the CPU. | You want to compare against the GPU routes. It costs PCIe bandwidth and CPU time. |

To change the format, set the property, then close and reopen the app that plays the video. Each decoder reads the property when it starts, and YouTube can keep one decoder across videos:

```sh
waydroid prop set persist.waydroid.video_sdr_output nv12
```

If the host cannot write the format you pick, the decoder uses the next one down the table. On NVIDIA, it never falls back to `nv12`. 10-bit (HDR) video always uses P010 buffers in system memory.

## Check that it works

1. Check that the three host services are running:

   ```sh
   systemctl is-active waydroid-venus waydroid-alloc waydroid-hwdec
   ```

   Each line prints `active`.

2. Check that Android renders with Vulkan on the host GPU:

   ```sh
   sudo waydroid shell -- dumpsys SurfaceFlinger | grep -m1 GLES:
   ```

   The line names ANGLE, Venus, and your GPU, for example `ANGLE (NVIDIA, Vulkan 1.3.335 (NVIDIA Virtio-GPU Venus (NVIDIA GeForce RTX 3060 Laptop GPU) ...`.

3. Play a 1440p60 or 4K video in YouTube. During playback, `nvidia-smi dmon -s u` shows load in the `dec` column, which means the host GPU decodes the video.

4. To check the colours of SDR video, connect adb to Android and run the grey-bar check. It needs Hyprland and `grim`:

   ```sh
   ANDROID_SERIAL=192.168.240.112:5555 ./waydroid-venus/hwdec/test/ramp-check.sh
   ```

   The `shown` row matches the `expected` row to within 1. If the dark bars are brighter, for example 31 where 17 is expected, Android lifts the shadows of video.

5. To check that the host writes every frame correctly, run the decoder test on any H.264, VP9, or AV1 clip. It needs no Waydroid session:

   ```sh
   TARGETS=40 CLEAR=1 ./waydroid-venus/hwdec/test/run.sh clip.mp4
   ```

   The last line reports `0 frames over 4` when every frame matches a software decode.

## Update

To update, pull the branch, build again, and install again:

```sh
git pull
./waydroid-venus/build.sh && ./stack/build.sh && ./waydroid-app-displays/build.sh
sudo ./waydroid-venus/install.sh && sudo ./stack/install.sh && sudo ./waydroid-app-displays/install.sh
waydroid session stop
```

After a Waydroid image update, run `stack/build.sh` and `sudo ./stack/install.sh` again, because the composer links against the image's libraries. The install also removes the patched SurfaceFlinger when the new system image no longer matches it, so the old binary cannot replace the new one.

## Uninstall

Run the uninstallers in the reverse order, then restart Android. Skip the Widevine line if you did not install it:

```sh
sudo ./waydroid-venus/uninstall-widevine.sh
sudo ./waydroid-app-displays/uninstall.sh
sudo ./stack/uninstall.sh
sudo ./waydroid-venus/uninstall.sh
waydroid session stop
```

## Known problems

- 10-bit (HDR) video and VP8 video are decoded on the host, but the frames still pass through system memory, so they use more PCIe bandwidth than 8-bit H.264, VP9, and AV1 video.
- HDR video shows as HDR only while Hyprland runs the monitor in HDR mode. Otherwise it shows as SDR.
- RGBA video buffers take 2.7 times the memory of NV12. YouTube allocates 13 buffers of 3840x2160 for adaptive playback, which take about 430 MB of GPU memory as RGBA.
- The stack was tested only on an NVIDIA GPU. The `nv12` route on other GPUs is untested.
- Video thumbnails crash the Venus render server. When an app asks Android for a video thumbnail, as Gallery and the file picker do, `mediaserver` draws it on the GPU. The render server then crashes in NVIDIA's shader compiler (`libnvidia-glvkspirv`, during `vkCreateGraphicsPipelines`), and `mediaserver` aborts. Gallery stops playing, because it plays through `mediaserver`. It happens with every route in `persist.waydroid.video_sdr_output`. Not investigated yet; `coredumpctl list` shows each crash as a `virgl_render_server` dump followed by a `mediaserver` dump.

## Where each fix lives

Each fix also has its own branch on `TechyAditya/android_hardware_waydroid`, for review or for upstream pull requests. The `pr/` branches start from upstream `lineage-20` and hold one change each. [UPSTREAM.md](UPSTREAM.md) has the draft pull requests and issues.

| Fix | Branch |
| --- | --- |
| Several app windows: single-window size, lifetime, and focus, closing the full UI, and per-monitor refresh rates | `pr/multi-window` |
| HDR video on HDR outputs | `pr/hdr-output` |
| Present gralloc buffers in GPU memory as dma-bufs | `pr/gpu-memory-dmabuf` |
| Rate-limited resize hotplugs and cursor scale | `resize-interval` ([waydroid/android_hardware_waydroid#92](https://github.com/waydroid/android_hardware_waydroid/pull/92)) |
| Full UI frames copied before Android finished drawing them | `shm-copy-fence` |
| Audio HAL sample rate and float output | `audio-rate-buffer` |
| Single-window fixes, HDR, GPU-memory buffers, and refresh rates together | `single-window-focus` |

Three parts live in other repositories:

| Fix | Repository and branch |
| --- | --- |
| HDR passthrough in the HWC2-on-HWC1 adapter | `TechyAditya/android_vendor_waydroid` `hdr` |
| SurfaceFlinger null check for the resize crash ([waydroid/waydroid#2464](https://github.com/waydroid/waydroid/issues/2464)) | `TechyAditya/android_vendor_waydroid` `sf-default-display` |
| Venus, the allocator, and the video decoder | `TechyAditya/waydroid-venus` `master` |
