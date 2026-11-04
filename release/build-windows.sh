#!/bin/sh
# Builds dist/Quarantine-<version>-Windows-x64.zip (Quarantine.exe + SDL3.dll) by cross-compiling in
# Docker. Needs Docker and the generated code in gen/ (see README, "Building from source").
set -eu
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
SDL_VER=3.4.20
CACHE=release/cache
mkdir -p "$CACHE" dist
if [ ! -d "$CACHE/SDL3-$SDL_VER" ]; then
    curl -fL -o "$CACHE/SDL3-devel-$SDL_VER-mingw.tar.gz" \
        "https://github.com/libsdl-org/SDL/releases/download/release-$SDL_VER/SDL3-devel-$SDL_VER-mingw.tar.gz"
    tar -xzf "$CACHE/SDL3-devel-$SDL_VER-mingw.tar.gz" -C "$CACHE"
fi
docker build -q -t quarantine-mingw -f release/Dockerfile.windows release >/dev/null
SDL=/src/$CACHE/SDL3-$SDL_VER/x86_64-w64-mingw32
docker run --rm --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src quarantine-mingw sh -c "
    set -e
    make -j8 OPT=-O2 CC=x86_64-w64-mingw32-gcc-posix BUILD=build/windows OUT=build/windows/Quarantine.exe \
        ARCH_FLAGS='-D_FILE_OFFSET_BITS=64' \
        SDL_CFLAGS='-I$SDL/include' \
        SDL_LIBS='-L$SDL/lib -lSDL3 -Wl,-Bstatic -lpthread -Wl,-Bdynamic' \
        EXTRA_LDLIBS='-static-libgcc -mwindows'
    rm -rf dist/windows && mkdir -p dist/windows
    x86_64-w64-mingw32-strip -o dist/windows/Quarantine.exe build/windows/Quarantine.exe
    cp $SDL/bin/SDL3.dll dist/windows/
    sed 's/\$/\r/' release/README-player.txt > dist/windows/README.txt
    rm -f dist/Quarantine-$VERSION-Windows-x64.zip
    cd dist/windows && zip -q ../Quarantine-$VERSION-Windows-x64.zip Quarantine.exe SDL3.dll README.txt
"
echo "dist/Quarantine-$VERSION-Windows-x64.zip"
