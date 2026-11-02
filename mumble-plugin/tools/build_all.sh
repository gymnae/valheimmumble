#!/usr/bin/env bash
# Cross-compile the plugin for every OS/architecture Mumble ships on and bundle them into one
# .mumble_plugin file. Needs zig (`pip install ziglang`) and python3.
#
#   tools/build_all.sh [output.mumble_plugin]
set -euo pipefail
cd "$(dirname "$0")/.."
OUT="${1:-../dist/valheim_radio.mumble_plugin}"
ZIG="${ZIG:-python3 -m ziglang}"
SRC="src/plugin.cpp src/RadioFx.cpp src/Settings.cpp"
FLAGS="-std=c++17 -O2 -shared -fvisibility=hidden -Iinclude -Isrc"
B=build/cross
rm -rf "$B"

build() { # zig-target output-file extra-flags...
	local target=$1 out=$2; shift 2
	mkdir -p "$(dirname "$out")"
	echo "== $target"
	# shellcheck disable=SC2086
	$ZIG c++ -target "$target" $FLAGS "$@" $SRC -o "$out"
}

build x86_64-windows-gnu  $B/windows-x64/valheim_radio.dll   -s -lws2_32
build x86-windows-gnu     $B/windows-x86/valheim_radio.dll   -s -lws2_32
build aarch64-windows-gnu $B/windows-arm64/valheim_radio.dll -s -lws2_32

build x86_64-linux-gnu.2.28           $B/linux-x64/libvalheim_radio.so   -s -fPIC
build x86-linux-gnu.2.28              $B/linux-x86/libvalheim_radio.so   -s -fPIC
build aarch64-linux-gnu.2.28          $B/linux-arm64/libvalheim_radio.so -s -fPIC
build arm-linux-gnueabihf.2.28        $B/linux-armv7/libvalheim_radio.so -s -fPIC -mcpu=generic+v7a

build x86_64-macos.11.0  $B/macos-x64/libvalheim_radio.dylib   -fPIC
build aarch64-macos.11.0 $B/macos-arm64/libvalheim_radio.dylib -fPIC
mkdir -p $B/macos-universal
python3 tools/lipo.py $B/macos-universal/libvalheim_radio.dylib \
	$B/macos-x64/libvalheim_radio.dylib $B/macos-arm64/libvalheim_radio.dylib

python3 tools/package.py "$OUT" \
	--lib windows:x64:$B/windows-x64/valheim_radio.dll \
	--lib windows:x86:$B/windows-x86/valheim_radio.dll \
	--lib windows:arm64:$B/windows-arm64/valheim_radio.dll \
	--lib linux:x64:$B/linux-x64/libvalheim_radio.so \
	--lib linux:x86:$B/linux-x86/libvalheim_radio.so \
	--lib linux:arm64:$B/linux-arm64/libvalheim_radio.so \
	--lib linux:armv7,arm:$B/linux-armv7/libvalheim_radio.so \
	--lib macos:universal,x64,arm64:$B/macos-universal/libvalheim_radio.dylib
