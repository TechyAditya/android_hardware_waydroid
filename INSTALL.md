# Install the all-fixes Waydroid stack

This branch combines every fix from this fork into one checkout. It builds and installs four parts:

- `waydroid-venus/` runs Android's Vulkan on the host GPU through Venus. It also has a GPU memory allocator for gralloc and a host video decoder that writes frames straight into GPU memory.
- `stack/` builds the Wayland composer from `hwcomposer/`, the HWC2-on-HWC1 adapter, and the audio HAL from `audio/`.
- `waydroid-app-displays/` opens each Android app in its own host window, on an Android display of its own. It also has a `waydroid-app-displays` command for hidden and fixed-size displays, and it shows Android media players in the desktop's media controls.
- `hwcomposer/` and `audio/` are the composer and audio HAL sources that `stack/build.sh` compiles.

The install also sets the Waydroid settings that the stack is tested with, so you get the same setup as its author. [Settings the install sets](#settings-the-install-sets) lists them.

The composer changes are per-monitor refresh rates up to the display's own rate, rate-limited resize hotplugs, single-window focus fixes, GPU-memory dma-buf presentation, and HDR output. The audio HAL plays at the device's native sample rate with float output.

## Requirements

- An x86_64 Linux host with a Wayland compositor. The stack was tested on Arch Linux (CachyOS) with Hyprland and an NVIDIA RTX 3060 on driver 615.
- Waydroid initialized with the LineageOS 20 (Android 13) x86_64 images: `sudo waydroid init`.
- These Arch packages, or the same libraries on another distribution:

```sh
sudo pacman -S --needed base-devel git curl clang lld llvm meson ninja python-mako python-yaml \
	python-packaging libepoxy libdrm mesa ffmpeg vulkan-headers vulkan-icd-loader shaderc expat alsa-lib \
	jre-openjdk-headless
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

`waydroid-venus/install.sh` restarts a running Android container, and `stack/install.sh` does too when it writes new settings. A restart closes every open Android app.

1. Install the composer, the adapter, and the audio HAL into `/var/lib/waydroid/overlay/vendor`. This also writes the settings in [Settings the install sets](#settings-the-install-sets) to `/var/lib/waydroid/waydroid.cfg`:

   ```sh
   sudo ./stack/install.sh
   ```

   On the `20260927` x86_64 system image, this also installs a SurfaceFlinger with a null check for the resize crash in [waydroid/waydroid#2464](https://github.com/waydroid/waydroid/issues/2464). The patch changes 5 bytes and fits only that image, so on other images the script skips it and says so.

   On the same image, it also installs a `services.jar` in which Back can leave an app's first screen in a per-app window. Waydroid's framework ignores Back on a task's first screen unless its full window is active. The patched jar keeps that rule on Android's main display only. The script builds the jar from your `system.img` with smali, which it downloads into `stack/build/tools`, and checks the result against a fixed hash. Android then runs `services.jar` without its precompiled code, so it starts a little slower.

2. Install the Venus services, gralloc, and the video decoders. This also sets the `ro.hardware.egl`, `ro.hardware.vulkan`, and codec properties in `waydroid.cfg`:

   ```sh
   sudo ./waydroid-venus/install.sh
   ```

   The same script installs the built `waydroid-app-displays/` next to it: the per-app window service, the `waydroid app launch` hook in `/usr/local/bin/waydroid`, the `waydroid-app-displays` command, and the `waydroid-app-displays-mpris` user service.

3. Restart Android:

   ```sh
   waydroid session stop
   waydroid session start &
   ```

The installers keep the files and properties they replace. Each part's `uninstall.sh` puts them back.

## Settings the install sets

`stack/install.sh` writes these values to `/var/lib/waydroid/waydroid.cfg` on the first install. `waydroid upgrade` and later installs keep them, including any you change.

| Setting | Value | Waydroid's default | Effect |
| --- | --- | --- | --- |
| `[waydroid] suspend_action` | `none` | `freeze` | Android keeps running when its screen turns off, so apps in their own windows and the media controls stay live. |
| `persist.waydroid.multi_windows` | `false` | `false` | The full Android window shows one app at a time and follows its window size. The install writes it so that an earlier `true` doesn't stay. |
| `persist.waydroid.resize_interval_ms` | `150` | no limit: upstream's composer hotplugs on every resize | While you drag a window edge, Android's display resizes at most once every 150 milliseconds. The last frame stretches to fill the window in between. |
| `debug.stagefright.ccodec` | `4` | `0` on NVIDIA, unset on other GPUs | Android's Codec2 codecs are on. Screen recording and scrcpy encode with them. Waydroid turns them off when it falls back to software rendering, which it does on NVIDIA. |

`waydroid-venus/install.sh` writes the graphics and codec properties: `ro.hardware.egl=angle`, `ro.hardware.vulkan=virtio`, `mesa.vn.debug=vtest`, `mesa.vtest.socket.name=/data/venus/vtest.sock`, and `ro.media.xml_variant.codecs=_waydroid`.

To change one of these settings, edit its line in `waydroid.cfg`, then apply the file. The upgrade restarts a running Android container:

```sh
sudo waydroid upgrade --offline
```

These Android properties are not in `waydroid.cfg`, and their defaults are already the tested values:

| Property | Default | Effect |
| --- | --- | --- |
| `persist.waydroid.app_displays` | `true` | Each app opens in its own window. With `false`, `waydroid app launch` uses Waydroid's own window modes. |
| `persist.waydroid.app_media` | `true` | Android media players show in the desktop's media controls. |
| `persist.waydroid.video_sdr_output` | `auto` | Picks how 8-bit video reaches Android. [Choose how 8-bit video reaches Android](#choose-how-8-bit-video-reaches-android) explains the values. |

To change one of them, run `waydroid prop set PROPERTY VALUE` as the user that runs the session, then restart Android.

## Optional: play hi-res audio at its own sample rate

The audio HAL opens each stream at the rate the app asks for, up to 192 kHz. PipeWire still resamples everything to 48 kHz unless you allow other rates. To let PipeWire follow the stream's rate, create `~/.config/pipewire/pipewire.conf.d/10-allowed-rates.conf`:

```text
context.properties = {
    default.clock.allowed-rates = [ 44100 48000 88200 96000 176400 192000 ]
}
```

Then restart PipeWire:

```sh
systemctl --user restart pipewire
```

The setting applies to every app on the desktop. If a filter chain, such as a speaker equalizer, is tuned for 48 kHz, add `node.force-rate = 48000` to that chain's properties.

## Optional: give every app window a title bar with Back

With `persist.waydroid.app_titlebar` at `auto`, the default, a per-app window asks the compositor for a title bar through the `xdg-decoration` protocol. Where the compositor doesn't offer the protocol, or asks the window to decorate itself, the window draws its own. It has a Back button on the left, then the window buttons of your desktop's button layout, in your light or dark style. GNOME, for example, offers no `xdg-decoration`, so its windows draw their own. KWin draws the title bar with your decoration theme. Hyprland always answers that it decorates windows itself, and draws no title bars, so windows there have none. Everywhere, Esc and the mouse's back button also act as Back.

To give windows their own title bar with the Back button where the compositor would draw one, as on KDE:

```sh
waydroid prop set persist.waydroid.app_titlebar always
```

Windows that open after this get it. Hyprland still gets none, because it turns the request down. On KDE, the buttons follow the order set in System Settings, and the colours follow whether your colour scheme's title bar is light or dark. The bar has no KDE-specific buttons, such as keep above. Right-click the title for KWin's window menu instead.

To go back to KWin's title bars, set the property to `auto`, the default.

## Optional: keep Waydroid windows opaque on Hyprland

While an app lays itself out again after a resize, Android leaves parts of its window transparent, and Hyprland's blur shows through. This window rule, for Hyprland 0.57's Lua config, makes the full Android window and the per-app windows opaque:

```lua
hl.window_rule({ match = { class = "^(Waydroid|waydroid\\..*)$" }, opaque = true, opacity = "1.0 override" })
```

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

Android media players, such as Apple Music or YouTube, show in the desktop's media controls through MPRIS, like a browser or Spotify does. They show the title, the cover, and the position, and they take play, pause, skip, and seek.

To give an app a display of a fixed size and density, or a display with no window, use `waydroid-app-displays launch`. This is useful for testing an app at phone size, or for an AI agent that drives an app:

```sh
waydroid-app-displays launch org.cromite.cromite --hidden --size 720x1280 --dpi 320
```

The command prints the display id and the commands to take screenshots, record, tap, swipe, type, and close the app. Without `--hidden`, the app opens in a floating window of that size. A later `waydroid app launch` of the same app, or a start from the app launcher, shows it in a normal window again at the desktop's size and density. Run `waydroid-app-displays --help` for every command.

If you use an AI coding agent, you can add the author's Waydroid testing instructions to your own `AGENTS.md`. [AGENT-PREFERENCES.md](AGENT-PREFERENCES.md) has the section to copy, with the Cromite browser it uses.

Run `waydroid prop set` and `waydroid-app-displays` as the user that runs the session. With `sudo`, `waydroid prop set` looks for a session of root and changes nothing.

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

1. Check that the four host services are running:

   ```sh
   systemctl is-active waydroid-venus waydroid-alloc waydroid-hwdec
   systemctl --user is-active waydroid-app-displays-mpris
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
sudo ./stack/install.sh && sudo ./waydroid-venus/install.sh
waydroid session stop
```

An update keeps the settings you changed in `waydroid.cfg`. `stack/install.sh` writes each setting only on the first install that has it, and prints each one it writes.

After a Waydroid image update, run `stack/build.sh` and `sudo ./stack/install.sh` again, because the composer links against the image's libraries. The install also removes the patched SurfaceFlinger when the new system image no longer matches it, so the old binary cannot replace the new one.

## Uninstall

Run the uninstallers in the reverse order, then restart Android. Skip the Widevine line if you did not install it:

```sh
sudo ./waydroid-venus/uninstall-widevine.sh
sudo ./waydroid-app-displays/uninstall.sh
sudo ./waydroid-venus/uninstall.sh
sudo ./stack/uninstall.sh
waydroid session stop
```

`stack/uninstall.sh` also puts back the `waydroid.cfg` settings that the install replaced.

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

Five parts live in other repositories:

| Fix | Repository and branch |
| --- | --- |
| HDR passthrough in the HWC2-on-HWC1 adapter | `TechyAditya/android_vendor_waydroid` `hdr` |
| SurfaceFlinger null check for the resize crash ([waydroid/waydroid#2464](https://github.com/waydroid/waydroid/issues/2464)) | `TechyAditya/android_vendor_waydroid` `sf-default-display` |
| Back on a task's first screen on displays other than the default one | `TechyAditya/android_vendor_waydroid` `back-other-displays` |
| Venus, the allocator, and the video decoder | `TechyAditya/waydroid-venus` `master` |
| Per-app windows, the `waydroid-app-displays` command, and the media controls | `TechyAditya/waydroid-app-displays` `master` |
