#!/bin/bash
# Builds the Android-side parts of this branch for x86_64 Android 13 (Waydroid LineageOS 20 images):
#   vendor/lib64/hw/hwcomposer.waydroid.so      Wayland composer (refresh rates, resize, audio-safe hotplug, HDR)
#   vendor/lib64/libhwc2on1adapter.so           HWC2-on-HWC1 adapter with Waydroid's patches and HDR passthrough
#   vendor/lib{,64}/hw/audio.primary.waydroid.so audio HAL (native sample rate, float output)
#   vendor/etc/audio_policy_configuration.xml
# The result is laid out like /var/lib/waydroid/overlay in stack/build/dist. Install it with stack/install.sh.
#
# Needs: clang, lld, llvm-ar, a host C compiler, curl, git, expat and alsa-lib headers.
# Links against the libraries of the installed image, so a Waydroid session must be running
# (waydroid session start) the first time this runs.
set -euo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(dirname "$HERE")
W=${WORK:-$HERE/build}
R=${WAYDROID_ROOTFS:-/var/lib/waydroid/rootfs}
I=$W/inc V=$W/vndk33 S=$W/src O=$W/out L=$W/libs A=$W/adapter D=$W/dist/vendor

AOSP_BRANCH=android13-release
VNDK_BRANCH=android14-release
LIBDRM_SHA=6bfcfc725fbe0ece0918535556d61ee567b1ffff
MINIGBM_SHA=e5a7769a4892cd1b6c61c56a8bedfc9b93584704
INTERFACES_SHA=2f83e1bda9536aaf406352c868a8192cc3526a69
WAYLAND_PROTOCOLS_TAG=1.50

for tool in clang clang++ ld.lld llvm-ar cc curl git tar base64; do
    command -v $tool >/dev/null || { echo "Missing $tool." >&2; exit 1; }
done
[[ -e /usr/include/expat.h ]] || { echo "Missing expat headers (expat package)." >&2; exit 1; }
[[ -e /usr/include/alsa/asoundlib.h ]] || { echo "Missing alsa-lib headers (alsa-lib package)." >&2; exit 1; }

gs() { # repo path dest: fetch a directory of an AOSP repository
    mkdir -p "$3"
    curl -fsSL "https://android.googlesource.com/$1/+archive/refs/heads/$AOSP_BRANCH${2:+/$2}.tar.gz" | tar -xz -C "$3"
}
gh_tar() { # owner/repo sha dest
    mkdir -p "$3"
    curl -fsSL "https://codeload.github.com/$1/tar.gz/$2" | tar -xz --strip-components=1 -C "$3"
}

# --- Sources (fetched once) ---
if [[ ! -e $W/.fetched ]]; then
    echo "Fetching sources into $W"
    rm -rf "$I" "$V" "$S" "$W/extra" "$A"
    gs platform/bionic libc/include "$I/bionic/include"
    gs platform/bionic libc/kernel/uapi "$I/bionic/uapi"
    gs platform/bionic libc/kernel/android/uapi "$I/bionic/android-uapi"
    gs platform/system/logging liblog/include "$I/liblog"
    gs platform/system/core libcutils/include "$I/cutils"
    gs platform/system/core libsystem/include "$I/libsystem"
    gs platform/system/media audio/include "$I/audio"
    gs platform/system/media audio_utils/include "$I/audio_utils"
    gs platform/system/media audio_effects/include "$I/audio_effects"
    gs platform/hardware/libhardware include "$I/libhardware"
    mkdir -p "$I/alsa-host" && ln -sfn /usr/include/alsa "$I/alsa-host/alsa"

    mkdir -p "$V"
    curl -fsSL "https://android.googlesource.com/platform/prebuilts/vndk/v33/+archive/refs/heads/$VNDK_BRANCH/x86_64/include.tar.gz" \
        | tar -xz -C "$V"

    for r in wayland libffi libxkbcommon wayland-protocols; do gs platform/external/$r "" "$S/$r"; done
    gh_tar waydroid/android_external_libdrm $LIBDRM_SHA "$S/android_external_libdrm"
    gh_tar waydroid/android_external_minigbm $MINIGBM_SHA "$S/android_external_minigbm"
    cp "$HERE/inputs/wayland-config.h" "$S/wayland/config.h"
    cp -r "$HERE/inputs/libxkbcommon-config" "$S/libxkbcommon/config"
    curl -fsSL -o "$S/fractional-scale-v1.xml" \
        "https://gitlab.freedesktop.org/wayland/wayland-protocols/-/raw/$WAYLAND_PROTOCOLS_TAG/staging/fractional-scale/fractional-scale-v1.xml"

    mkdir -p "$W/extra/libsync"
    curl -fsSL "https://android.googlesource.com/platform/system/core/+/refs/heads/$AOSP_BRANCH/libsync/sw_sync.h?format=TEXT" \
        | base64 -d > "$W/extra/libsync/sw_sync.h"

    for f in Android.bp HWC2On1Adapter.cpp MiniFence.cpp include/hwc2on1adapter/HWC2On1Adapter.h include/hwc2on1adapter/MiniFence.h; do
        curl -fsSL --create-dirs -o "$A/$f" \
            "https://raw.githubusercontent.com/LineageOS/android_hardware_interfaces/$INTERFACES_SHA/graphics/composer/2.1/utils/hwc2on1adapter/$f"
    done
    # Its own repository, or git apply resolves paths against this checkout and skips every file.
    git init -q "$A"
    for p in "$HERE"/patches/hwc2on1adapter/*.patch; do git -C "$A" apply -p6 "$p"; done
    grep -q waydroid_hwc_get_hdr_capabilities "$A/HWC2On1Adapter.cpp" || { echo "Adapter patches did not apply." >&2; exit 1; }
    touch "$W/.fetched"
fi

# --- Image libraries ---
if [[ ! -e $R/system/lib64/liblog.so ]]; then
    echo "$R is not mounted. Start Waydroid (waydroid session start) and run this again." >&2
    exit 1
fi
mkdir -p "$L"
VNDK=$R/system/apex/com.android.vndk.current/lib64
BIONIC=$R/system/apex/com.android.runtime/lib64/bionic
cp -f $R/system/lib64/{liblog,libsync,libEGL,libGLESv2}.so $VNDK/{libutils,libcutils,libhardware,libhidlbase,libui,libc++}.so \
    $R/vendor/lib64/libdrm.so $R/vendor/lib64/vendor.waydroid.*.so $BIONIC/{libc,libm,libdl}.so "$L/"

mkdir -p $O/obj/{ffi,wayland,xkb,proto,hwc/modes,adapter} $O/proto $O/empty $D/lib64/hw $D/lib/hw $D/etc

# --- Wayland scanner (from the same wayland release; newer scanners emit wl_proxy_marshal_flags) ---
(cd $S/wayland && cc -O2 -I. -Isrc -include config.h src/scanner.c src/wayland-util.c -lexpat -o $O/wayland-scanner)

# --- Static libraries and protocol code ---
CC=(clang --target=x86_64-linux-android33 -fPIC -O2 -g0 -nostdinc
    -isystem "$(clang -print-resource-dir)/include"
    -isystem $I/bionic/include -isystem $I/bionic/uapi -isystem $I/bionic/uapi/asm-x86 -isystem $I/bionic/android-uapi
    -DANDROID -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ -ffunction-sections -fdata-sections)

compile() { # dir src... (extra flags in EXTRA)
    local dir=$1; shift
    for f in "$@"; do
        "${CC[@]}" "${EXTRA[@]}" -c "$f" -o "$O/obj/$dir/$(basename "${f%.*}").o"
    done
}

AP=$S/wayland-protocols/freedesktop.org
declare -A XML=(
    [wayland]=$S/wayland/protocol/wayland.xml
    [wayland-android]=$ROOT/hwcomposer/wayland-android.xml
    [color-management-v1]=$ROOT/hwcomposer/color-management-v1.xml
    [linux-dmabuf-unstable-v1]=$AP/unstable/linux-dmabuf/linux-dmabuf-unstable-v1.xml
    [viewporter]=$AP/stable/viewporter/viewporter.xml
    [presentation-time]=$AP/stable/presentation-time/presentation-time.xml
    [xdg-shell]=$AP/stable/xdg-shell/xdg-shell.xml
    [tablet-unstable-v2]=$AP/unstable/tablet/tablet-unstable-v2.xml
    [pointer-constraints-unstable-v1]=$AP/unstable/pointer-constraints/pointer-constraints-unstable-v1.xml
    [relative-pointer-unstable-v1]=$AP/unstable/relative-pointer/relative-pointer-unstable-v1.xml
    [idle-inhibit-unstable-v1]=$AP/unstable/idle-inhibit/idle-inhibit-unstable-v1.xml
    [fractional-scale-v1]=$S/fractional-scale-v1.xml
)
for name in "${!XML[@]}"; do
    $O/wayland-scanner client-header < "${XML[$name]}" > "$O/proto/$name-client-protocol.h"
    $O/wayland-scanner private-code < "${XML[$name]}" > "$O/proto/$name-protocol.c"
done

bash $S/libffi/gen_ffi_header.sh < $S/libffi/include/ffi.h.in > $O/proto/ffi_gen.h
EXTRA=(-I$O/proto -I$S/libffi/include -I$S/libffi/linux-x86_64 -DHAVE_AS_X86_PCREL -DHAVE_AS_ASCII_PSEUDO_OP -Wno-everything)
compile ffi $S/libffi/src/{closures,debug,java_raw_api,prep_cif,raw_api,types}.c \
    $S/libffi/src/x86/{ffi64.c,ffiw64.c,unix64.S,win64.S}

EXTRA=(-I$S/wayland -I$S/wayland/src -I$O/proto -I$S/libffi/include -I$S/libffi/linux-x86_64 -include $S/wayland/config.h
       -fvisibility=hidden -Wno-everything)
compile wayland $S/wayland/src/{connection,wayland-client,wayland-os,wayland-util}.c
"${CC[@]}" "${EXTRA[@]}" -c $O/proto/wayland-protocol.c -o $O/obj/wayland/wayland-protocol.o

# registry.c needs libxml2 and the composer does not use it
X=$S/libxkbcommon
EXTRA=(-std=c11 -fno-strict-aliasing -D_GNU_SOURCE -I$X/src -I$X/config -I$X/config/libxkbcommon.so.0.0.0.p -I$X/include -Wno-everything)
for f in $X/config/libxkbcommon.so.0.0.0.p/parser.c $X/src/compose/{parser,paths,state,table}.c \
         $X/src/xkbcomp/{action,ast-build,compat,expr,include,keycodes,keymap,keymap-dump,keywords,rules,scanner,symbols,types,vmod,xkbcomp}.c \
         $X/src/{atom,context,context-priv,keymap,keymap-priv,keysym,keysym-utf,state,text,utf8,util-list,utils}.c; do
    "${CC[@]}" "${EXTRA[@]}" -c "$f" -o "$O/obj/xkb/$(echo "${f#$X/}" | tr / _).o"
done

EXTRA=(-I$S/wayland/src -I$O/proto -Wno-everything)
for name in "${!XML[@]}"; do
    [[ $name == wayland ]] && continue
    "${CC[@]}" "${EXTRA[@]}" -c "$O/proto/$name-protocol.c" -o "$O/obj/proto/$name-protocol.o"
done

rm -f $O/libffi.a $O/libwayland_client_static.a $O/libxkbcommon.a
llvm-ar rcs $O/libffi.a $O/obj/ffi/*.o
llvm-ar rcs $O/libwayland_client_static.a $O/obj/wayland/*.o
llvm-ar rcs $O/libxkbcommon.a $O/obj/xkb/*.o

# --- Composer ---
GEN_INCLUDES=()
while IFS= read -r d; do GEN_INCLUDES+=(-isystem "$d"); done < <(find $V/generated-headers -type d \( -path '*genc++_headers/gen' -o -path '*-ndk-source/gen/include' \) | sort)

COMMON=(--target=x86_64-linux-android33 -fPIC -O2 -g0 -nostdinc
    -DANDROID -D__ANDROID_VNDK__ -D__ANDROID_VENDOR__ '-DLOG_TAG="hwcomposer"' -DNDEBUG -D_FORTIFY_SOURCE=2
    -ffunction-sections -fdata-sections)
SYS_INCLUDES=(
    -isystem "$(clang -print-resource-dir)/include"
    -isystem $I/bionic/include -isystem $I/bionic/uapi -isystem $I/bionic/uapi/asm-x86 -isystem $I/bionic/android-uapi
    -isystem $V/system/logging/liblog/include_vndk
    -isystem $V/system/core/libcutils/include
    -isystem $V/system/core/libutils/include
    -isystem $V/system/core/libsystem/include
    -isystem $V/system/core/libsync/include
    -isystem $V/system/libbase/include
    -isystem $V/system/libhidl/base/include
    -isystem $V/system/libhidl/transport/include
    -isystem $V/system/libfmq/include
    -isystem $V/system/libfmq/base
    -isystem $V/system/libhwbinder/include
    -isystem $V/hardware/libhardware/include
    -isystem $V/frameworks/native/libs/ui/include_vndk
    -isystem $V/frameworks/native/libs/ui/include_types
    -isystem $V/frameworks/native/libs/nativebase/include
    -isystem $V/frameworks/native/libs/nativewindow/include
    -isystem $V/frameworks/native/libs/arect/include
    -isystem $V/frameworks/native/libs/math/include
    -isystem $V/frameworks/native/opengl/include
    -isystem $V/frameworks/native/libs/binder/ndk/include_cpp
    -isystem $V/frameworks/native/libs/binder/ndk/include_ndk
    -isystem $V/frameworks/native/libs/binder/ndk/include_platform
    "${GEN_INCLUDES[@]}"
    -isystem $HERE/inputs/hidl-headers
    -isystem $W/extra
    -isystem $S/android_external_libdrm/include/drm
    -isystem $S/android_external_libdrm/android
    -isystem $S/android_external_libdrm
    -isystem $S/android_external_minigbm
    -isystem $S/wayland/src
    -isystem $S/libxkbcommon/include
    -isystem $O/proto
)
CXXFLAGS=(-std=gnu++17 -nostdinc++ -fno-exceptions -fno-rtti
    -isystem $V/external/libcxx/include -isystem $V/external/libcxxabi/include)
WARN=(-Wall -Wno-unused-parameter -Wno-deprecated-declarations -Wno-unknown-warning-option)
BUILTINS=$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-x86_64.a

echo '__attribute__((visibility("hidden"))) void *__dso_handle = &__dso_handle;' > $O/dso_handle.c
clang "${COMMON[@]}" -c $O/dso_handle.c -o $O/obj/hwc/dso_handle.o

SRC=$ROOT/hwcomposer
objs=()
for f in "$SRC"/*.cpp "$SRC"/modes/*.cpp; do
    rel=${f#$SRC/}
    out=$O/obj/hwc/${rel%.cpp}.o
    clang++ "${COMMON[@]}" "${CXXFLAGS[@]}" "${SYS_INCLUDES[@]}" -I"$SRC" "${WARN[@]}" -c "$f" -o "$out"
    objs+=("$out")
done
objs+=($O/obj/hwc/dso_handle.o)

clang++ --target=x86_64-linux-android33 --sysroot=$O/empty -fuse-ld=lld -shared -nostdlib \
    -Wl,-soname,hwcomposer.waydroid.so -Wl,--no-undefined -Wl,-z,noexecstack -Wl,--gc-sections \
    -Wl,--exclude-libs,ALL -Wl,--hash-style=gnu \
    "${objs[@]}" $O/obj/proto/*.o \
    $O/libwayland_client_static.a $O/libffi.a $O/libxkbcommon.a "$BUILTINS" \
    $L/liblog.so $L/libutils.so $L/libcutils.so $L/libhardware.so $L/libhidlbase.so \
    $L/libsync.so $L/libui.so $L/libdrm.so $L/libEGL.so $L/libGLESv2.so \
    $L/vendor.waydroid.{clipboard@1.0,display@1.0,display@1.1,display@1.2,task@1.0,window@1.0,window@1.1,window@1.2}.so \
    $L/libc++.so $L/libc.so $L/libm.so $L/libdl.so \
    -o $D/lib64/hw/hwcomposer.waydroid.so

# --- HWC2-on-HWC1 adapter ---
objs=()
for f in $A/HWC2On1Adapter.cpp $A/MiniFence.cpp; do
    out=$O/obj/adapter/$(basename ${f%.cpp}).o
    clang++ "${COMMON[@]}" '-ULOG_TAG' '-DLOG_TAG="HWC2On1Adapter"' "${CXXFLAGS[@]}" "${SYS_INCLUDES[@]}" -I$A/include \
        -Wall -Werror -Wextra -Wunused -Wunreachable-code -Wno-sign-compare -Wno-unused-parameter \
        -c "$f" -o "$out"
    objs+=("$out")
done
objs+=($O/obj/hwc/dso_handle.o)

clang++ --target=x86_64-linux-android33 --sysroot=$O/empty -fuse-ld=lld -shared -nostdlib \
    -Wl,-soname,libhwc2on1adapter.so -Wl,--no-undefined -Wl,-z,noexecstack -Wl,--gc-sections \
    -Wl,--hash-style=gnu \
    "${objs[@]}" "$BUILTINS" \
    $L/libutils.so $L/libcutils.so $L/liblog.so $L/libhardware.so \
    $L/libc++.so $L/libc.so $L/libm.so $L/libdl.so \
    -o $D/lib64/libhwc2on1adapter.so

# --- Audio HAL (the audio service is 32-bit, so both builds are needed) ---
CF=(-fPIC -O2 -nostdinc -isystem "$(clang -print-resource-dir)/include"
    -isystem $I/bionic/include -isystem $I/bionic/uapi -isystem $I/bionic/uapi/asm-x86 -isystem $I/bionic/android-uapi
    -I$I/liblog -I$I/cutils -I$I/libsystem -I$I/audio -I$I/audio_utils -I$I/audio_effects -I$I/libhardware -I$I/alsa-host
    -Wno-unused-parameter)
LD=(-fuse-ld=lld -shared -nostdlib --sysroot=$O/empty -Wl,-soname,audio.primary.waydroid.so -Wl,--no-undefined -Wl,-z,noexecstack)

clang --target=x86_64-linux-android33 "${CF[@]}" -c $ROOT/audio/audio_hw.c -o $O/audio_hw64.o
B=$R/system/apex/com.android.runtime/lib64/bionic
clang --target=x86_64-linux-android33 "${LD[@]}" $O/audio_hw64.o \
    $R/system/lib64/liblog.so $R/system/lib64/libcutils.so $R/vendor/lib64/libasound.so $B/libc.so $B/libm.so $B/libdl.so \
    -o $D/lib64/hw/audio.primary.waydroid.so

clang --target=i686-linux-android33 "${CF[@]}" -c $ROOT/audio/audio_hw.c -o $O/audio_hw32.o
B=$R/system/apex/com.android.runtime/lib/bionic
clang --target=i686-linux-android33 "${LD[@]}" -Wl,--exclude-libs,ALL $O/audio_hw32.o \
    "$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-i386.a" \
    $R/system/lib/liblog.so $R/system/lib/libcutils.so $R/vendor/lib/libasound.so $B/libc.so $B/libm.so $B/libdl.so \
    -o $D/lib/hw/audio.primary.waydroid.so

cp $ROOT/audio/audio_policy_configuration.xml $D/etc/audio_policy_configuration.xml

echo "Built into $W/dist:"
(cd $W/dist && find . -type f | sort)
