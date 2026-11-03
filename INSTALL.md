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
	python-packaging libepoxy libdrm mesa ffmpeg vulkan-headers vulkan-icd-loader expat alsa-lib
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

## Run

- To open an app in its own window, run `waydroid app launch PACKAGE`, for example `waydroid app launch com.google.android.youtube`. Waydroid's desktop entries run the same command, so app launchers open separate windows too.
- To open the whole Android screen in one window, run `waydroid show-full-ui`.

The composer follows the refresh rate of the monitor that shows each window, so moving a window to another monitor changes Android's frame rate.

By default, the composer resizes Android's display on every resize event of its window. To resize it at most once every 500 milliseconds while you drag the window edge, set this property and restart Android:

```sh
sudo waydroid prop set persist.waydroid.resize_interval_ms 500
```

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

## Update

To update, pull the branch, build again, and install again:

```sh
git pull
./waydroid-venus/build.sh && ./stack/build.sh && ./waydroid-app-displays/build.sh
sudo ./waydroid-venus/install.sh && sudo ./stack/install.sh && sudo ./waydroid-app-displays/install.sh
waydroid session stop
```

After a Waydroid image update, run `stack/build.sh` and `sudo ./stack/install.sh` again, because the composer links against the image's libraries.

## Uninstall

Run the uninstallers in the reverse order, then restart Android:

```sh
sudo ./waydroid-app-displays/uninstall.sh
sudo ./stack/uninstall.sh
sudo ./waydroid-venus/uninstall.sh
waydroid session stop
```

## Known problems

- Closing the `waydroid show-full-ui` window also closes the per-app windows.
- 10-bit (HDR) video and VP8 video are decoded on the host, but the frames still pass through system memory, so they use more PCIe bandwidth than 8-bit H.264, VP9, and AV1 video.
- The stack was tested only on an NVIDIA GPU.

## Where each fix lives

Each fix also has its own branch, for review or for upstream pull requests:

| Fix | Repository and branch |
| --- | --- |
| Refresh rates, GPU-memory buffers, HDR output, single-window focus | `TechyAditya/android_hardware_waydroid` `single-window-focus` |
| Rate-limited resize hotplugs | `TechyAditya/android_hardware_waydroid` `resize-interval` ([waydroid/android_hardware_waydroid#92](https://github.com/waydroid/android_hardware_waydroid/pull/92)) |
| Audio HAL sample rate and float output | `TechyAditya/android_hardware_waydroid` `audio-rate-buffer` |
| HDR passthrough in the HWC2-on-HWC1 adapter | `TechyAditya/android_vendor_waydroid` `hdr` |
| Venus, the allocator, and the video decoder | `TechyAditya/waydroid-venus` `master` |
