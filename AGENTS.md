# AGENTS.md

Waydroid (LineageOS 20, Android 13) on an NVIDIA host: composer, audio HAL, Venus Vulkan, a GPU memory allocator, and a host video decoder. [INSTALL.md](INSTALL.md) covers building, installing, and checking. Read it before you build.

## Branches

`all-fixes` integrates everything. Every change also lives on one upstream-shaped branch, and the change lands there first:

- `pr/*`, `shm-copy-fence`, `audio-rate-buffer` and `resize-interval` each start from upstream `lineage-20` (remote `origin`) and hold one change. `resize-interval` is the head of upstream PR #92, so a push to it updates the PR.
- `pr/multi-window` is the upstream branch for the single-window, full UI close and refresh rate commits. `single-window-focus` combines those single-window and refresh rate commits with `pr/hdr-output` and `pr/gpu-memory-dmabuf`.
- SurfaceFlinger changes are patch files in `TechyAditya/android_vendor_waydroid`, under `waydroid-patches/base-patches-33/frameworks/native/`. Make one with `git format-patch` on LineageOS `android_frameworks_native` `lineage-20.0`, after `git am` of the existing patches, so it applies in order. `stack/surfaceflinger.sh` ships the resize-crash fix as a binary patch until an image has it.
- `waydroid-venus/` and `waydroid-app-displays/` are `git subtree`s of `TechyAditya/waydroid-venus` and `TechyAditya/waydroid-app-displays` `master`, checked out under `~/Code`. Commit there, then run `git subtree pull --prefix=NAME ~/Code/NAME master` here. Commit or stash local edits first, because the pull refuses a dirty tree.
- Keep the name `waydroid-app-displays` out of `pr/*` commits, comments, and drafts.
- `stack/settings.py` holds the `waydroid.cfg` settings that users get, and they match the author's machine. When a setting on this machine changes for good, change it there and in the settings table in `INSTALL.md`.

Push to the remote `fork`. [UPSTREAM.md](UPSTREAM.md) holds the draft pull requests and issues. The user submits them, so leave them as drafts.

## Test loop

Each check is _red_ on its bug and green when fixed:

- `waydroid-venus/hwdec/test/run.sh CLIP` checks that the host decoder writes every frame correctly. It needs no Android. Green is `0 frames over 4`. Add `TARGETS=40 CLEAR=1` to cover more buffer slots and skipped writes.
- `waydroid-venus/hwdec/test/ramp-check.sh` measures the grey bars that Android shows. Green is `shown` within 1 of `expected`. It pushes its clip once, so leave the clip on the device.
- `waydroid-venus/hwdec/test/nv12-import.c` reproduces the NVIDIA driver bug. It stays red on driver 615, so use it to check a new driver, not your change.
- `stack/build.sh` compiles the composer. To compile a `pr/` branch alone, copy `stack/` and `hwcomposer/color-management-v1.xml` from `all-fixes` into its worktree, set `WORK` to a copy of `stack/build` so nothing downloads again, and delete both copies before you commit.

## Deploying

- Files in `/var/lib/waydroid/overlay` load only after `waydroid session stop` and a new `waydroid session start`. Copying a file in while Android runs changes nothing that Android sees.
- A session restart closes every Android app. Tell the user before you restart.
- Restarting `waydroid-hwdec` breaks the decoders apps hold. Reopen the video, or force-stop the app, before you measure.
- Killing `surfaceflinger` reboots the whole container and loses runtime `setprop` values.

## Gotchas

- Run `waydroid prop set` as the session user. Under `sudo` it finds no session and changes nothing.
- Waydroid reads `waydroid.cfg` with configparser, which lowercases keys. A mixed-case property there becomes a duplicate key, and Waydroid refuses to start. Put mixed-case properties in `/var/lib/waydroid/waydroid_base.prop`. `waydroid upgrade` regenerates that file.
- Gallery opens shared storage only through a `content://media/...` URI. A `file://` path fails with "Can't play this video", and the media ID changes, so look it up by `_data` each time as `ramp-check.sh` does.
- YouTube resumes at the last position, which can be the end. Open videos with `&t=1`. ExoPlayer can also keep one decoder across videos, so a property that a decoder reads at start applies after the app restarts.
- SurfaceFlinger reads `debug.sf.treat_170m_as_sRGB` once at start, with `atoi()`. Set it to `1`; `true` reads as off. The host decoder already tags SDR video as sRGB, so this stack works without it.
- Video thumbnails, from Gallery or the file picker, crash `virgl_render_server` and then `mediaserver`. Use Just Player (`com.brouken.player`) for video checks. It loads files from its own picker, not from an intent, and the media play key (keyevent 126) starts the last file.
- adb reaches Android at `192.168.240.112:5555`, and `adb shell` runs as `shell`, not root. Use `sudo waydroid shell` for root.
