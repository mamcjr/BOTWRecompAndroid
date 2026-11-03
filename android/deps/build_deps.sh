#!/bin/bash
# android/deps/build_deps.sh: what the Android app links against, from pinned sources
# into android/deps/out for arm64-v8a:
#   SDL3 (shared, plus its Java glue), shaderc (one shared library, glslang and SPIRV-Tools inside), and the headers
#   Vulkan-Headers, Vulkan Memory Allocator and volk (the Vulkan functions come from the driver adrenotools loads),
#   and the DejaVu Sans font.
# FFmpeg is not here yet (docs/las/ANDROID-PORT.md, Fase 2).
# Runs in Git Bash on Windows (or any bash) with the Android SDK's NDK and CMake; each part is skipped when its output
# exists (delete out/ to redo).
set -euo pipefail
cd "$(dirname "$0")"
D=$PWD
S=$D/src
P=$D/out
W=$D/work
mkdir -p "$S" "$P/include" "$P/lib" "$W"

SDK=${ANDROID_HOME:-${LOCALAPPDATA:-$HOME}/Android/Sdk}
SDK=${SDK//\\//}
NDK=${ANDROID_NDK:-$SDK/ndk/30.0.16248370}
CMAKE_DIR=$SDK/cmake/3.30.4/bin
API=30
JOBS=$(nproc 2>/dev/null || echo 8)
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=.exe ;; *) EXE= ;; esac
CMAKE=$CMAKE_DIR/cmake$EXE
ANDROID_CMAKE=(-G Ninja -DCMAKE_MAKE_PROGRAM="$CMAKE_DIR/ninja$EXE" -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake"
               -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-$API -DANDROID_STL=c++_static -DCMAKE_BUILD_TYPE=Release)

fetch() {  # url sha256 file
    [ -f "$S/$3" ] || { curl -fsSL -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    (cd "$S" && echo "$2  $3" | sha256sum -c --quiet)
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && (cd "$S" && tar --force-local -xf "$1" -C "$W/$2" --strip-components 1); }

fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
      7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
fetch https://github.com/google/shaderc/archive/refs/tags/v2026.3.tar.gz \
      ee493ccf1b3038b4ef2fe024664c5eb2dc4bcc1f6b05b33e3909de0e19c81024 shaderc-2026.3.tar.gz
fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
fetch https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/archive/refs/tags/v3.3.0.tar.gz \
      c4f6bbe6b5a45c2eb610ca9d231158e313086d5b1a40c9922cb42b597419b14e VulkanMemoryAllocator-3.3.0.tar.gz
fetch https://github.com/zeux/volk/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
      078a9411298e4e0f60f5f5398c890783427c25a414619294ca8e69587bbd5eae volk-1.4.350.1.tar.gz
fetch https://github.com/dejavu-fonts/dejavu-fonts/releases/download/version_2_37/dejavu-fonts-ttf-2.37.tar.bz2 \
      fa9ca4d13871dd122f61258a80d01751d603b4d3ee14095d65453b4e846e17d7 dejavu-fonts-ttf-2.37.tar.bz2

# DejaVu Sans: the system font substitute when the player hasn't imported the Switch's (runtime/hle/pl.c; the one
# Linux uses - Android's Roboto lacks glyphs the game asks for)
if [ ! -f "$P/fonts/DejaVuSans.ttf" ]; then
    unpack dejavu-fonts-ttf-2.37.tar.bz2 dejavu
    mkdir -p "$P/fonts" && cp "$W/dejavu/ttf/DejaVuSans.ttf" "$P/fonts/" && cp "$W/dejavu/LICENSE" "$P/fonts/DejaVu-LICENSE.txt"
fi

# headers: Vulkan (the backend's, newer than the NDK's), VMA, volk (built into the app: volk.c next to volk.h)
if [ ! -f "$P/include/vulkan/vulkan_core.h" ]; then
    unpack Vulkan-Headers-1.4.350.1.tar.gz vkh
    cp -r "$W/vkh/include/vulkan" "$W/vkh/include/vk_video" "$P/include/"
fi
if [ ! -f "$P/include/vma/vk_mem_alloc.h" ]; then
    unpack VulkanMemoryAllocator-3.3.0.tar.gz vma
    mkdir -p "$P/include/vma" && cp "$W/vma/include/vk_mem_alloc.h" "$P/include/vma/"
fi
if [ ! -f "$P/volk/volk.c" ]; then
    unpack volk-1.4.350.1.tar.gz volk
    mkdir -p "$P/volk" && cp "$W/volk/volk.h" "$W/volk/volk.c" "$W/volk/LICENSE.md" "$P/volk/"
fi

# SDL3: the shared library and the Java side of the same version (org.libsdl.app, compiled into the app)
if [ ! -f "$P/lib/libSDL3.so" ]; then
    unpack SDL3-3.4.16.tar.gz sdl
    # (an AAudio stream disconnected while the app was in the background: reopened on resume, not left silent)
    patch -d "$W/sdl" -p1 < "$D/sdl-aaudio-resume.patch"
    "$CMAKE" -S "$W/sdl" -B "$W/sdl/build" "${ANDROID_CMAKE[@]}" -DCMAKE_INSTALL_PREFIX="$P" -DCMAKE_INSTALL_LIBDIR=lib \
        -DSDL_SHARED=ON -DSDL_STATIC=OFF -DSDL_TESTS=OFF -DSDL_EXAMPLES=OFF
    "$CMAKE" --build "$W/sdl/build" -j"$JOBS"
    "$CMAKE" --install "$W/sdl/build"
    rm -rf "$P/java" && mkdir -p "$P/java/org/libsdl"
    cp -r "$W/sdl/android-project/app/src/main/java/org/libsdl/app" "$P/java/org/libsdl/"
fi

# shaderc as one shared library (glslang, SPIRV-Tools inside), for the Android Vulkan backend; its DEPS file pins the
# glslang / SPIRV-Tools / SPIRV-Headers commits (utils/git-sync-deps clones them)
if [ ! -f "$P/lib/libshaderc_shared.so" ]; then
    unpack shaderc-2026.3.tar.gz shaderc
    PY=${PYTHON:-}
    for p in python3 python; do  # (Windows: python3 may be the Microsoft Store's stub, which can't run anything)
        [ -z "$PY" ] && command -v $p >/dev/null && $p -c 1 2>/dev/null && PY=$p
    done
    : "${PY:?no Python for the shaderc utils/git-sync-deps}"
    (cd "$W/shaderc" && "$PY" utils/git-sync-deps)
    "$CMAKE" -S "$W/shaderc" -B "$W/shaderc/build" "${ANDROID_CMAKE[@]}" -DSHADERC_SKIP_TESTS=ON \
        -DSHADERC_SKIP_EXAMPLES=ON -DSHADERC_SKIP_COPYRIGHT_CHECK=ON -DSPIRV_SKIP_TESTS=ON -DSPIRV_SKIP_EXECUTABLES=ON \
        -DENABLE_GLSLANG_BINARIES=OFF -DSPIRV_TOOLS_BUILD_STATIC=ON -DBUILD_SHARED_LIBS=OFF
    "$CMAKE" --build "$W/shaderc/build" --target shaderc_shared -j"$JOBS"
    mkdir -p "$P/include/shaderc"
    cp "$W/shaderc/libshaderc/include/shaderc/"*.h "$P/include/shaderc/"
    cp "$W/shaderc/build/libshaderc/libshaderc_shared.so" "$P/lib/"
fi

echo "deps: $P"
ls "$P/lib"
