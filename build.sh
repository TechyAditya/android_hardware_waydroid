#!/usr/bin/env bash
# Builds the patched render server for the host and the patched Venus
# Vulkan driver for Waydroid's Android 13 x86_64 image.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
build=$here/build
prefix=${PREFIX:-$HOME/.local/lib/waydroid-venus}

# Same Mesa release as the Waydroid vendor image, so only the patches differ.
mesa_tag=mesa-26.0.8
virgl_tag=1.3.0
ndk_version=r28c
android_api=33

mkdir -p "$build/out"

fetch() {
   local url=$1 tag=$2 dir=$3 patch=$4
   if [ ! -d "$dir" ]; then
      git clone -q --depth 1 --branch "$tag" "$url" "$dir"
      git -C "$dir" apply "$patch"
   fi
}

fetch https://gitlab.freedesktop.org/virgl/virglrenderer.git "$virgl_tag" \
   "$build/virglrenderer" "$here/patches/virglrenderer-vtest-import-fd.patch"
fetch https://gitlab.freedesktop.org/mesa/mesa.git "$mesa_tag" \
   "$build/mesa" "$here/patches/mesa-venus-vtest-android.patch"
fetch https://android.googlesource.com/platform/hardware/libhardware android13-release \
   "$build/libhardware" "$here/patches/gralloc-waydroid-venus.patch"

ndk=${ANDROID_NDK:-$build/android-ndk-$ndk_version}
if [ ! -d "$ndk" ]; then
   curl -fL --retry 5 -o "$build/ndk.zip" \
      "https://dl.google.com/android/repository/android-ndk-$ndk_version-linux.zip"
   unzip -q -d "$build" "$build/ndk.zip"
   rm "$build/ndk.zip"
fi

if [ ! -d "$build/virglrenderer/build" ]; then
   meson setup "$build/virglrenderer/build" "$build/virglrenderer" \
      --prefix="$prefix" -Dbuildtype=release -Dvenus=true -Dvideo=false -Dtests=false
fi
ninja -C "$build/virglrenderer/build" install

ndk_bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
cat > "$build/android-x86_64.cross" <<EOF
[binaries]
ar = '$ndk_bin/llvm-ar'
c = ['$ndk_bin/x86_64-linux-android$android_api-clang']
cpp = ['$ndk_bin/x86_64-linux-android$android_api-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$ndk_bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=/nonexistent', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = 'x86_64'
cpu = 'x86_64'
endian = 'little'
EOF

if [ ! -d "$build/mesa/build-android" ]; then
   meson setup "$build/mesa/build-android" "$build/mesa" \
      --cross-file "$build/android-x86_64.cross" \
      -Dbuildtype=release -Db_ndebug=true \
      -Dplatforms=android -Dplatform-sdk-version=$android_api \
      -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
      -Dvulkan-drivers=virtio -Dgallium-drivers= -Dvulkan-layers= -Dtools= \
      -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dllvm=disabled \
      -Dzstd=disabled -Dxmlconfig=disabled -Dvideo-codecs= \
      -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static
fi
ninja -C "$build/mesa/build-android" src/virtio/vulkan/libvulkan_virtio.so
"$ndk_bin/llvm-strip" -o "$build/out/vulkan.virtio.so" \
   "$build/mesa/build-android/src/virtio/vulkan/libvulkan_virtio.so"

# AOSP's default gralloc with NVIDIA-compatible row alignment.  It links
# against the stub libraries from the Mesa build; Waydroid uses 4 KiB pages.
gralloc_src=$build/libhardware/modules/gralloc
"$ndk_bin/x86_64-linux-android$android_api-clang++" -shared -fPIC -O2 -std=c++17 \
   -static-libstdc++ -DPAGE_SIZE=4096 -DLOG_TAG='"gralloc"' \
   -I"$here/gralloc/include" -I"$build/mesa/include/android_stub" -I"$gralloc_src" \
   -o "$build/out/gralloc.default.so" \
   "$gralloc_src/gralloc.cpp" "$gralloc_src/mapper.cpp" "$here/gralloc/framebuffer_stub.cpp" \
   -L"$build/mesa/build-android/src/android_stub" -lcutils -llog
"$ndk_bin/llvm-strip" "$build/out/gralloc.default.so"

echo "render server: $prefix/bin/virgl_test_server"
echo "android driver: $build/out/vulkan.virtio.so"
echo "android gralloc: $build/out/gralloc.default.so"
