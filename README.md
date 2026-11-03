# Waydroid all-fixes

This is a personal fork of [waydroid/android_hardware_waydroid](https://github.com/waydroid/android_hardware_waydroid), not an official Waydroid project. The `all-fixes` branch combines every fix from the fork into one checkout that you build and install on top of a stock Waydroid with the LineageOS 20 (Android 13) images.

With it, Waydroid on an NVIDIA GPU runs Android's graphics through Vulkan on the host GPU instead of software rendering, and decodes video on the host GPU. Each app can open in its own resizable window. The branch also fixes window resizing, refresh rates, HDR video, and high sample rate audio.

It was tested on one setup only: Hyprland on Arch Linux (CachyOS) with an NVIDIA RTX 3060 on driver 615.

- [INSTALL.md](INSTALL.md) lists the requirements and explains how to build, install, check, update, and uninstall. It also lists the branch that holds each fix.
- [UPSTREAM.md](UPSTREAM.md) lists what this fork plans to send to Waydroid, with the draft pull requests and issues.
- [AGENTS.md](AGENTS.md) has notes for AI coding agents that work on this branch.
