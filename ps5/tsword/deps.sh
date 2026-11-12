#!/usr/bin/env bash
# ps5/tsword/deps.sh <payload SDK> <prefix> - the console builds of what TotK Recomp's runtime links (its
# tools/build_game.py LINK_LIBS on Linux: SDL3, opus, vpx, zstd, and VMA's header), into <prefix>/{include,lib}:
#   SDL3 3.4.16          headers only: the title implements the subset the runtime calls (WSword PS5's sdl_ps5.c)
#   Vulkan-Headers       1.4.350.1; the console's driver is RADV (PS5_Vulkan)
#   VMA 3.4.0            vk_mem_alloc.h, the version and hash tools/build_game.py pins (VMA_FETCH)
#   opus 1.5.2           static: the game's voice and music (no stack protector: its __stack_chk_guard is a data
#                        import from libkernel that WSword never had - TSword v0.2.1 closed at launch with it)
#   libvpx 1.15.2        static, the VP9 decoder only: the game's movies (runtime/ov_movie.c)
#   zstd 1.5.7           static: RomFS files and the patch sets (runtime/ov_zstd.c, resolution.c)
# Each part is skipped when its output exists (delete the prefix to redo).
set -euo pipefail
sdk=$(cd -- "${1:?usage: deps.sh <payload SDK> <prefix>}" && pwd)
mkdir -p "${2:?prefix}"
P=$(cd -- "$2" && pwd)
S=$P/src
W=$P/build
mkdir -p "$S" "$W" "$P/include" "$P/lib"
JOBS=$(nproc)
CC=$sdk/bin/prospero-clang
TC=$sdk/toolchain/prospero.cmake

fetch() {  # url sha256 file
    [[ -f $S/$3 ]] || { curl -fsSL --retry 3 -o "$S/$3.part" "$1" && mv "$S/$3.part" "$S/$3"; }
    echo "$2  $S/$3" | sha256sum -c --quiet
}
unpack() { rm -rf "$W/$2" && mkdir -p "$W/$2" && tar -xf "$S/$1" -C "$W/$2" --strip-components 1; }

if [[ ! -f $P/include/SDL3/SDL.h ]]; then
    fetch https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz \
        7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68 SDL3-3.4.16.tar.gz
    unpack SDL3-3.4.16.tar.gz sdl
    cp -R "$W/sdl/include/SDL3" "$P/include/"
fi

if [[ ! -f $P/include/vulkan/vulkan.h ]]; then
    fetch https://github.com/KhronosGroup/Vulkan-Headers/archive/refs/tags/vulkan-sdk-1.4.350.1.tar.gz \
        6d1bb65e49520344cc0a48af3dc02e993781efff14c7ebdcb8ae9fa23ddf7e83 Vulkan-Headers-1.4.350.1.tar.gz
    unpack Vulkan-Headers-1.4.350.1.tar.gz vulkan-headers
    cp -R "$W/vulkan-headers/include/vulkan" "$W/vulkan-headers/include/vk_video" "$P/include/"
fi

if [[ ! -f $P/include/vk_mem_alloc.h ]]; then
    fetch https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator/v3.4.0/include/vk_mem_alloc.h \
        8487b7995ad3b263eb73bc5b9a77d71aa69b6bef5d58a715c02d2663afd81f1a vk_mem_alloc.h
    cp "$S/vk_mem_alloc.h" "$P/include/"
fi

if [[ ! -f $P/lib/libopus.a ]]; then
    fetch https://github.com/xiph/opus/releases/download/v1.5.2/opus-1.5.2.tar.gz \
        65c1d2f78b9f2fb20082c38cbe47c951ad5839345876e46941612ee87f9a7ce1 opus-1.5.2.tar.gz
    unpack opus-1.5.2.tar.gz opus
    cmake -S "$W/opus" -B "$W/opus/build" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-march=znver2 -fno-stack-protector" -DCMAKE_INSTALL_PREFIX="$P" -DBUILD_SHARED_LIBS=OFF -DOPUS_BUILD_TESTING=OFF \
        -DOPUS_STACK_PROTECTOR=OFF -DOPUS_FORTIFY_SOURCE=OFF \
        -DOPUS_BUILD_PROGRAMS=OFF -DOPUS_INSTALL_PKG_CONFIG_MODULE=OFF -DOPUS_INSTALL_CMAKE_CONFIG_MODULE=OFF \
        > "$W/opus.configure.log" 2>&1 || { tail -40 "$W/opus.configure.log"; exit 1; }
    cmake --build "$W/opus/build" -j"$JOBS" > "$W/opus.build.log" 2>&1 || { grep -E "error|FAILED" "$W/opus.build.log" | head -30; exit 1; }
    cmake --install "$W/opus/build" > /dev/null
fi

if [[ ! -f $P/lib/libzstd.a ]]; then
    fetch https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz \
        eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3 zstd-1.5.7.tar.gz
    unpack zstd-1.5.7.tar.gz zstd
    # (ZSTD_TRACE=0: no weak tracing hooks, which the title's link would bind to null)
    cmake -S "$W/zstd/build/cmake" -B "$W/zstd/out" -G Ninja -DCMAKE_TOOLCHAIN_FILE="$TC" -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_C_FLAGS="-march=znver2 -DZSTD_TRACE=0" -DCMAKE_INSTALL_PREFIX="$P" -DZSTD_BUILD_SHARED=OFF -DZSTD_BUILD_STATIC=ON \
        -DZSTD_BUILD_PROGRAMS=OFF -DZSTD_BUILD_TESTS=OFF -DZSTD_MULTITHREAD_SUPPORT=ON \
        > "$W/zstd.configure.log" 2>&1 || { tail -40 "$W/zstd.configure.log"; exit 1; }
    cmake --build "$W/zstd/out" -j"$JOBS" > "$W/zstd.build.log" 2>&1 || { grep -E "error|FAILED" "$W/zstd.build.log" | head -30; exit 1; }
    cmake --install "$W/zstd/out" > /dev/null
fi

if [[ ! -f $P/lib/libvpx.a ]]; then
    if [[ ! -d $W/libvpx/.git ]]; then
        rm -rf "$W/libvpx"
        git -c advice.detachedHead=false clone -q --depth 1 --branch v1.15.2 https://github.com/webmproject/libvpx "$W/libvpx"
    fi
    [[ $(git -C "$W/libvpx" rev-parse HEAD) == d168454ecd099805c675d4a98c66f4891373302a ]] ||
        { echo "deps: libvpx v1.15.2 is not the pinned commit"; exit 1; }
    # (generic x86-64 Linux as far as libvpx can tell, the SDK's clang bringing the console's target and libc; its
    # assembly built with nasm, the VP9 decoder only)
    mkdir -p "$W/libvpx/out"
    (cd "$W/libvpx/out" && CC="$CC" CXX="$sdk/bin/prospero-clang++" AR="$sdk/bin/prospero-ar" \
        LD="$CC" STRIP="$sdk/bin/prospero-strip" AS=nasm \
        ../configure --target=x86_64-linux-gcc --prefix="$P" --enable-static --disable-shared --enable-pic \
        --disable-examples --disable-tools --disable-docs --disable-unit-tests --disable-install-bins \
        --disable-vp8 --disable-vp9-encoder --enable-vp9-decoder --disable-webm-io --disable-libyuv \
        --as=nasm --extra-cflags="-march=znver2" > "$W/libvpx.configure.log" 2>&1 ||
        { tail -40 "$W/libvpx.configure.log"; exit 1; }
     make -j"$JOBS" > "$W/libvpx.build.log" 2>&1 || { grep -E "error|Error" "$W/libvpx.build.log" | head -30; exit 1; }
     make install > /dev/null)
fi
echo "deps: $(cd "$P/lib" && ls *.a | tr '\n' ' ')"
