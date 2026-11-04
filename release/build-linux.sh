#!/bin/sh
# Builds dist/Quarantine-<version>-Linux-x64.tar.gz (Quarantine + lib/libSDL3.so.0) in Docker.
# Needs Docker and the generated code in gen/ (see README, "Building from source").
set -eu
cd "$(dirname "$0")/.."
VERSION=$(cat VERSION)
mkdir -p dist
docker build -q --platform linux/amd64 -t quarantine-linux -f release/Dockerfile.linux release >/dev/null
docker run --rm --platform linux/amd64 --user "$(id -u):$(id -g)" -v "$PWD:/src" -w /src quarantine-linux sh -c "
    set -e
    make -j8 OPT=-O2 CC=clang BUILD=build/linux OUT=build/linux/Quarantine \
        SDL_CFLAGS='-I/opt/sdl3/include' SDL_LIBS='-L/opt/sdl3/lib -lSDL3' \
        LDFLAGS='-Wl,-rpath,\\\$\$ORIGIN/lib'
    rm -rf dist/linux && mkdir -p dist/linux/Quarantine/lib
    strip -o dist/linux/Quarantine/Quarantine build/linux/Quarantine
    cp -L /opt/sdl3/lib/libSDL3.so.0 dist/linux/Quarantine/lib/
    cp release/README-player.txt dist/linux/Quarantine/README.txt
    readelf -d dist/linux/Quarantine/Quarantine | grep -E 'RUNPATH|RPATH'
    tar -czf dist/Quarantine-$VERSION-Linux-x64.tar.gz -C dist/linux Quarantine
"
echo "dist/Quarantine-$VERSION-Linux-x64.tar.gz"
