#!/bin/sh
# ./build.sh            Switch 1, blacklist-tool.nro (devkitpro/devkita64)
# ./build.sh switch2    Switch 2, blacklist-tool-switch2.nro (ounce-devkit)
# Anything after that goes to make, e.g. ./build.sh switch2 clean
set -e
cd "$(dirname "$0")"
platform=switch
image=devkitpro/devkita64:latest
case "${1:-}" in
switch) shift ;;
switch2)
    shift
    platform=switch2
    image=ounce-devkit
    if ! docker image inspect "$image" >/dev/null 2>&1; then
	git clone https://github.com/ouncebrew/ounce-devkit
        docker build -t "$image" ounce-devkit/
    fi
    ;;
esac
docker run --rm -u "$(id -u):$(id -g)" -v "$PWD:/build/blacklist-tool" -w /build/blacklist-tool \
    "$image" make PLATFORM="$platform" "$@"
