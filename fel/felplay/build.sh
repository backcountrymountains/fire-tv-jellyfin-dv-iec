#!/bin/bash
# Build felplay-cli: static armv7 (musl) via zig cc.
set -e
cd "$(dirname "$0")"
ZIG=${ZIG:-zig}   # zig 0.14.x
$ZIG cc -target arm-linux-musleabihf -static -O2 -Wall -Wextra -o felplay-cli felplay_cli.c felplay.c
echo "built $(pwd)/felplay-cli"
