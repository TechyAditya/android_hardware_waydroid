#!/bin/bash
# Sends HDR video thumbnails through libstagefright's CPU path. For an HDR video, FrameDecoder
# draws the thumbnail with RenderEngine on the GPU, and NVIDIA's shader compiler crashes building
# that pipeline under Venus (vkCreateGraphicsPipelines in libnvidia-gpucomp), which takes
# mediaserver down with the render server. VideoFrameDecoder::initSurface() now returns null, so
# FrameDecoder logs "fallback to non-hdr" and converts the frame on the CPU, as when the GPU
# capture can't start. HDR thumbnails show without tone mapping. The patch fits only the
# libstagefright of the lineage-20.0-20260927 x86_64 system image, which it reads from system.img
# with debugfs.
#
# usage: stagefright.sh install|uninstall, as root. stack/install.sh and stack/uninstall.sh run
# it. install skips other images, and removes its own copies once the image has changed.
# uninstall removes only its own copies. Restart Waydroid afterwards: waydroid session stop.
set -euo pipefail

[[ $EUID -eq 0 ]] || { echo "Run this with sudo." >&2; exit 1; }
images=$(sed -n 's/^images_path *= *//p' /var/lib/waydroid/waydroid.cfg 2> /dev/null || true)
image=${WAYDROID_IMAGES:-${images:-/var/lib/waydroid/images}}/system.img
overlay=/var/lib/waydroid/overlay/system

# dir, original sha256, patched sha256, file offset of initSurface(), its new first bytes
targets=(
    # mov 0x4(%esp),%eax; movl $0,(%eax); ret $4: store a null sp<Surface> in the return slot
    "lib c9d36a1dcce9d7e581f27ff1b6e32e6f08f26d30b2cf34cbdb04d8e634e061a9 2cd50b5e7d68731f3d037957b072e7366ff95f2bc5d2f598d07876c25b1cab3f 0xe3540 \x8b\x44\x24\x04\xc7\x00\x00\x00\x00\x00\xc2\x04\x00"
    # movq $0,(%rdi); mov %rdi,%rax; ret
    "lib64 41c7efbfc33d2356cece65c4245899ed82cab0a2df941d13f2dbcf0bf0c19949 a40f55cebad4f28d92f852bcf1d41c0bb240f160892b008921d340566b2d4a20 0xf4160 \x48\xc7\x07\x00\x00\x00\x00\x48\x89\xf8\xc3"
)

sha() { sha256sum "$1" | cut -d' ' -f1; }

case ${1:-} in
install|uninstall) ;;
*)
    echo "usage: $0 install|uninstall" >&2
    exit 2
    ;;
esac

tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
for target in "${targets[@]}"; do
    read -r dir original patched offset bytes <<< "$target"
    installed=$overlay/$dir/libstagefright.so
    ours=false
    [[ -f $installed && $(sha "$installed") == "$patched" ]] && ours=true

    if [[ $1 == uninstall ]]; then
        if $ours; then
            rm "$installed"
            echo "removed the patched system/$dir/libstagefright.so"
        fi
        continue
    fi

    debugfs -R "dump /system/$dir/libstagefright.so $tmp/$dir.so" "$image" > /dev/null 2>&1 || true
    if [[ ! -s $tmp/$dir.so || $(sha "$tmp/$dir.so") != "$original" ]]; then
        # a copy from an older image would replace the new image's library
        if $ours; then
            rm "$installed"
            echo "removed the patched system/$dir/libstagefright.so, which does not fit $image"
        fi
        echo "skipped the HDR thumbnail CPU path for system/$dir: it fits only the 20260927 system image"
        continue
    fi
    if $ours; then
        echo "system/$dir/libstagefright.so already sends HDR thumbnails to the CPU path"
        continue
    fi
    if [[ -e $installed ]]; then
        echo "skipped the HDR thumbnail CPU path: $installed exists and is not this patch" >&2
        continue
    fi
    printf "$bytes" | dd of="$tmp/$dir.so" bs=1 seek=$((offset)) conv=notrunc status=none
    [[ $(sha "$tmp/$dir.so") == "$patched" ]] || { echo "the patched system/$dir/libstagefright.so does not match" >&2; exit 1; }
    # a partial write would load in Android, and its hash would no longer mark it as ours
    install -D -m 0644 -o root -g root "$tmp/$dir.so" "$installed.new"
    mv -f "$installed.new" "$installed"
    echo "installed system/$dir/libstagefright.so with HDR thumbnails on the CPU path"
done
