#!/bin/bash
# Builds build/out/appdisplay.jar and build/out/libappdisplay.so for x86_64 Android 13.
# Downloads its toolchain into build/tools on first use; set TOOLS to reuse another copy.
set -euo pipefail

ROOT=$(cd "$(dirname "$0")" && pwd)
B=$ROOT/build
TOOLS=${TOOLS:-$B/tools}
OUT=$B/out
API=33

fetch() { # file url sha256
    local file=$TOOLS/$1
    if [[ ! -f $file ]]; then
        echo "downloading $1"
        curl -fSL --retry 5 -C - -o "$file.part" "$2"
        mv "$file.part" "$file"
    fi
    echo "$3  $file" | sha256sum -c --quiet
}

mkdir -p "$TOOLS" "$OUT"
fetch ecj-3.40.0.jar https://repo1.maven.org/maven2/org/eclipse/jdt/ecj/3.40.0/ecj-3.40.0.jar \
    05cc22a24e7982970f63a405fc6c820bc80b806f27f3c5a6236fc475f8f7152b
fetch r8.jar https://dl.google.com/android/maven2/com/android/tools/r8/8.5.35/r8-8.5.35.jar \
    4733945987ee0a840fafc34080b135259e01678412e07212b23f706334290294
fetch platform-33.zip https://dl.google.com/android/repository/platform-33_r02.zip \
    f851b13fe89f8510a1250df5e8593e86176b2428f4f3cbe0e304a85818c07bc8
fetch ndk.zip https://dl.google.com/android/repository/android-ndk-r27c-linux.zip \
    59c2f6dc96743b5daf5d1626684640b20a6bd2b1d85b13156b90333741bad5cc
fetch wayland-1.26.0.tar.xz https://gitlab.freedesktop.org/wayland/wayland/-/releases/1.26.0/downloads/wayland-1.26.0.tar.xz \
    64176eaa46e4969903e286f8e5ef8331affc17fdf03ac9b58381d2b23162b7a3
fetch libffi-3.4.6.tar.gz https://github.com/libffi/libffi/releases/download/v3.4.6/libffi-3.4.6.tar.gz \
    b0dea9df23c863a7a50e825440f3ebffabd65df1497108e5d437747843895a4e

ANDROID_JAR=$TOOLS/android-13/android.jar
[[ -f $ANDROID_JAR ]] || unzip -q -o "$TOOLS/platform-33.zip" 'android-13/android.jar' -d "$TOOLS"
NDK=$TOOLS/android-ndk-r27c
[[ -d $NDK ]] || unzip -q -o "$TOOLS/ndk.zip" -d "$TOOLS"
LLVM=$NDK/toolchains/llvm/prebuilt/linux-x86_64/bin
CC=$LLVM/x86_64-linux-android$API-clang
CXX=$LLVM/x86_64-linux-android$API-clang++
AR=$LLVM/llvm-ar
SCANNER=${WAYLAND_SCANNER:-wayland-scanner}
PROTOCOLS=${WAYLAND_PROTOCOLS:-/usr/share/wayland-protocols}

# ---- Java ----
rm -rf "$B/classes"
mkdir -p "$B/classes"
java -jar "$TOOLS/ecj-3.40.0.jar" -8 -proc:none -nowarn -encoding UTF-8 \
    -bootclasspath "$ANDROID_JAR" -d "$B/classes" \
    $(find "$ROOT/java" "$ROOT/java-stubs" -name '*.java')
java -cp "$TOOLS/r8.jar" com.android.tools.r8.D8 --release --min-api $API --lib "$ANDROID_JAR" \
    --output "$OUT/appdisplay.jar" $(find "$B/classes/waydroid" -name '*.class')

# ---- libffi ----
DEPS=$B/deps
if [[ ! -f $DEPS/lib/libffi.a ]]; then
    rm -rf "$B/src/libffi-3.4.6"
    mkdir -p "$B/src"
    tar -xzf "$TOOLS/libffi-3.4.6.tar.gz" -C "$B/src"
    (cd "$B/src/libffi-3.4.6" &&
        ./configure --host=x86_64-linux-android --prefix="$DEPS" --disable-shared --enable-static --with-pic \
            --disable-docs --disable-multi-os-directory CC="$CC" CFLAGS=-O2 >/dev/null &&
        make -j"$(nproc)" >/dev/null && make install >/dev/null)
fi

# ---- libwayland-client ----
GEN=$B/gen
WL=$B/src/wayland-1.26.0
if [[ ! -f $DEPS/lib/libwayland-client.a ]]; then
    rm -rf "$WL" "$B/obj/wayland"
    mkdir -p "$B/src" "$GEN" "$B/obj/wayland"
    tar -xJf "$TOOLS/wayland-1.26.0.tar.xz" -C "$B/src"
    sed -e 's/@WAYLAND_VERSION_MAJOR@/1/' -e 's/@WAYLAND_VERSION_MINOR@/26/' -e 's/@WAYLAND_VERSION_MICRO@/0/' \
        -e 's/@WAYLAND_VERSION@/1.26.0/' "$WL/src/wayland-version.h.in" > "$GEN/wayland-version.h"
    cat > "$WL/config.h" <<'EOF'
#define HAVE_ACCEPT4 1
#define HAVE_MKOSTEMP 1
#define HAVE_POSIX_FALLOCATE 1
#define HAVE_PRCTL 1
#define HAVE_MEMFD_CREATE 1
#define HAVE_MREMAP 1
#define HAVE_STRNDUP 1
#define HAVE_SYS_PRCTL_H 1
#define HAVE_BROKEN_MSG_CMSG_CLOEXEC 0
#define PACKAGE "wayland"
#define PACKAGE_VERSION "1.26.0"
EOF
    "$SCANNER" client-header "$WL/protocol/wayland.xml" "$GEN/wayland-client-protocol.h"
    "$SCANNER" private-code "$WL/protocol/wayland.xml" "$GEN/wayland-protocol.c"
    for f in connection wayland-client wayland-os wayland-util; do
        "$CC" -O2 -fPIC -fvisibility=hidden -D_GNU_SOURCE -I"$GEN" -I"$WL/src" \
            -I"$DEPS/include" -w -c "$WL/src/$f.c" -o "$B/obj/wayland/$f.o"
    done
    "$CC" -O2 -fPIC -fvisibility=hidden -I"$GEN" -I"$WL/src" -w -c "$GEN/wayland-protocol.c" \
        -o "$B/obj/wayland/wayland-protocol.o"
    rm -f "$DEPS/lib/libwayland-client.a"
    "$AR" rcs "$DEPS/lib/libwayland-client.a" "$B/obj/wayland/"*.o
fi

# ---- Protocol extensions ----
mkdir -p "$B/obj/proto"
declare -A XML=(
    [xdg-shell]=$PROTOCOLS/stable/xdg-shell/xdg-shell.xml
    [viewporter]=$PROTOCOLS/stable/viewporter/viewporter.xml
    [fractional-scale-v1]=$PROTOCOLS/staging/fractional-scale/fractional-scale-v1.xml
    [cursor-shape-v1]=$PROTOCOLS/staging/cursor-shape/cursor-shape-v1.xml
    [tablet-unstable-v2]=$PROTOCOLS/unstable/tablet/tablet-unstable-v2.xml
)
proto_objs=()
for name in "${!XML[@]}"; do
    "$SCANNER" client-header "${XML[$name]}" "$GEN/$name-client-protocol.h"
    "$SCANNER" private-code "${XML[$name]}" "$GEN/$name-protocol.c"
    "$CC" -O2 -fPIC -fvisibility=hidden -I"$GEN" -I"$WL/src" -w -c "$GEN/$name-protocol.c" -o "$B/obj/proto/$name.o"
    proto_objs+=("$B/obj/proto/$name.o")
done

# ---- libappdisplay.so ----
mkdir -p "$B/obj/native"
objs=()
for f in "$ROOT"/native/*.cpp; do
    o=$B/obj/native/$(basename "${f%.cpp}").o
    "$CXX" -std=c++17 -O2 -fPIC -fvisibility=hidden -fno-exceptions -fno-rtti -Wall -Wextra -Wno-unused-parameter \
        -Wno-missing-field-initializers -I"$GEN" -I"$WL/src" -c "$f" -o "$o"
    objs+=("$o")
done
"$CXX" -shared -fuse-ld=lld -static-libstdc++ -o "$OUT/libappdisplay.so" "${objs[@]}" "${proto_objs[@]}" \
    "$DEPS/lib/libwayland-client.a" "$DEPS/lib/libffi.a" \
    -landroid -lmediandk -lnativewindow -llog \
    -Wl,--no-undefined -Wl,--exclude-libs,ALL -Wl,--gc-sections -Wl,-z,noexecstack -Wl,-soname,libappdisplay.so
"$LLVM/llvm-strip" --strip-unneeded "$OUT/libappdisplay.so"
ls -la "$OUT"
