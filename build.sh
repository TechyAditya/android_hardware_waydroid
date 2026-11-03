#!/usr/bin/env bash
# Builds the patched render server for the host, and the patched Venus
# Vulkan driver and gralloc for Waydroid's Android 13 x86_64 image.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
build=$here/build
prefix=${PREFIX:-$HOME/.local/lib/waydroid-venus}

# Mesa 26.0.8, the release in the Waydroid vendor image, so only the
# patches differ; virglrenderer 1.3.0; AOSP android13-release.
mesa_commit=60e95b787857afbc9a00b693b91c0d9c8923a430
virgl_commit=ca50e008863837e094747a69974dde3ae148aeaa
libhardware_commit=bbd07bd2e60752a665ebc9b0915241f3c1636516
ndk_version=r28c
android_api=33

mkdir -p "$build/out"

# Checks out COMMIT of URL into DIR with PATCH applied.  An existing tree is
# used only if it is at COMMIT with exactly PATCH applied, so an interrupted
# run or an updated patch cannot build stale sources.
fetch() {
   local url=$1 commit=$2 dir=$3 patch=$4
   if [ -d "$dir" ]; then
      if [ "$(git -C "$dir" rev-parse HEAD)" != "$commit" ] ||
         ! git -C "$dir" diff HEAD | cmp -s - "$patch" ||
         [ -n "$(git -C "$dir" ls-files --others --exclude-standard)" ]; then
         echo "$dir does not match $(basename "$patch"); remove it to rebuild" >&2
         exit 1
      fi
      return
   fi

   local tmp=$dir.tmp
   rm -rf "$tmp"
   git init -q "$tmp"
   git -C "$tmp" fetch -q --depth 1 "$url" "$commit"
   git -C "$tmp" checkout -q FETCH_HEAD
   git -C "$tmp" apply "$patch"
   mv "$tmp" "$dir"
}

fetch https://gitlab.freedesktop.org/virgl/virglrenderer.git "$virgl_commit" \
   "$build/virglrenderer" "$here/patches/virglrenderer-vtest-import-fd.patch"
fetch https://gitlab.freedesktop.org/mesa/mesa.git "$mesa_commit" \
   "$build/mesa" "$here/patches/mesa-venus-vtest-android.patch"
fetch https://android.googlesource.com/platform/hardware/libhardware "$libhardware_commit" \
   "$build/libhardware" "$here/patches/gralloc-waydroid-venus.patch"

ndk=${ANDROID_NDK:-$build/android-ndk-$ndk_version}
if [ ! -f "$ndk/source.properties" ]; then
   rm -rf "$build/ndk.tmp"
   mkdir "$build/ndk.tmp"
   curl -fL --retry 5 -o "$build/ndk.tmp/ndk.zip" \
      "https://dl.google.com/android/repository/android-ndk-$ndk_version-linux.zip"
   unzip -q -d "$build/ndk.tmp" "$build/ndk.tmp/ndk.zip"
   rm -rf "$ndk"
   mv "$build/ndk.tmp/android-ndk-$ndk_version" "$ndk"
   rm -rf "$build/ndk.tmp"
fi

if [ ! -d "$build/virglrenderer/build" ]; then
   meson setup "$build/virglrenderer/build" "$build/virglrenderer" \
      --prefix="$prefix" -Dbuildtype=release -Dvenus=true -Dvideo=false -Dtests=false
fi
ninja -C "$build/virglrenderer/build" install

ndk_bin=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin
gralloc_src=$build/libhardware/modules/gralloc

# Waydroid runs 64-bit and 32-bit processes, which load /vendor/lib64 and
# /vendor/lib respectively.
for abi in x86_64 x86; do
   case $abi in
   x86_64) triple=x86_64-linux-android lib=lib64 ;;
   x86) triple=i686-linux-android lib=lib ;;
   esac

   cat > "$build/android-$abi.cross" <<EOF
[binaries]
ar = '$ndk_bin/llvm-ar'
c = ['$ndk_bin/$triple$android_api-clang']
cpp = ['$ndk_bin/$triple$android_api-clang++', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '--start-no-unused-arguments', '-static-libstdc++', '--end-no-unused-arguments']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$ndk_bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=/nonexistent', '/usr/bin/pkg-config']

[host_machine]
system = 'android'
cpu_family = '$abi'
cpu = '$abi'
endian = 'little'
EOF

   mesa_build=$build/mesa/build-android-$abi
   if [ ! -d "$mesa_build" ]; then
      meson setup "$mesa_build" "$build/mesa" \
         --cross-file "$build/android-$abi.cross" \
         -Dbuildtype=release -Db_ndebug=true \
         -Dplatforms=android -Dplatform-sdk-version=$android_api \
         -Dandroid-stub=true -Dandroid-libbacktrace=disabled \
         -Dvulkan-drivers=virtio -Dgallium-drivers= -Dvulkan-layers= -Dtools= \
         -Degl=disabled -Dgbm=disabled -Dglx=disabled -Dllvm=disabled \
         -Dzstd=disabled -Dxmlconfig=disabled -Dvideo-codecs= \
         -Dallow-fallback-for=libdrm -Dlibdrm:default_library=static
   fi
   ninja -C "$mesa_build" src/virtio/vulkan/libvulkan_virtio.so
   mkdir -p "$build/out/$lib"
   "$ndk_bin/llvm-strip" -o "$build/out/$lib/vulkan.virtio.so" \
      "$mesa_build/src/virtio/vulkan/libvulkan_virtio.so"

   # AOSP's default gralloc with NVIDIA-compatible row alignment and YUV
   # support.  It links against the stub libraries from the Mesa build;
   # Waydroid uses 4 KiB pages.
   "$ndk_bin/$triple$android_api-clang++" -shared -fPIC -O2 -std=c++17 \
      -static-libstdc++ -DPAGE_SIZE=4096 -DLOG_TAG='"gralloc"' \
      -I"$here/gralloc/include" -I"$build/mesa/include/android_stub" -I"$gralloc_src" \
      -o "$build/out/$lib/gralloc.default.so" \
      "$gralloc_src/gralloc.cpp" "$gralloc_src/mapper.cpp" "$here/gralloc/framebuffer_stub.cpp" \
      -L"$mesa_build/src/android_stub" -lcutils -llog
   "$ndk_bin/llvm-strip" "$build/out/$lib/gralloc.default.so"
done

echo "render server: $prefix/bin/virgl_test_server"
echo "android libraries: $build/out/lib64 $build/out/lib"
