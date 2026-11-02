#!/usr/bin/env bash
# Builds and runs all tests (Linux). Needs g++; the C#<->C++ wire test also needs the .NET SDK.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$(mktemp -d)"
trap 'rm -rf "$OUT"' EXIT
P="$ROOT/mumble-plugin"

echo "== building plugin"
g++ -std=c++17 -O2 -Wall -Wextra -fPIC -shared -fvisibility=hidden -I"$P/include" -I"$P/src" \
    "$P/src/plugin.cpp" "$P/src/RadioFx.cpp" "$P/src/Settings.cpp" -o "$OUT/libvalheim_radio.so" -pthread

echo "== plugin end-to-end (fake Mumble + fake Valheim server)"
g++ -std=c++17 -O1 -I"$P/include" -I"$P/src" "$ROOT/tests/plugin_harness.cpp" "$P/src/Settings.cpp" \
    -o "$OUT/harness" -ldl -pthread
mkdir -p "$OUT/xdg/Mumble"
XDG_CONFIG_HOME="$OUT/xdg" "$OUT/harness" "$OUT/libvalheim_radio.so"

echo "== radio effect render"
g++ -std=c++17 -O2 -I"$P/src" "$P/tools/radio_demo.cpp" "$P/src/RadioFx.cpp" -o "$OUT/radio_demo"
"$OUT/radio_demo" "$OUT/demo.wav"

if command -v dotnet >/dev/null; then
    echo "== C# (server mod) <-> C++ (plugin) wire format"
    X="$ROOT/tests/crosstest"
    g++ -std=c++17 -Wall -I"$P/src" "$X/crosstest.cpp" -o "$OUT/crosstest"
    dotnet build "$X/CrossTest.csproj" -c Release -o "$OUT/cs" -v q \
        --property:BaseIntermediateOutputPath="$OUT/obj/" >/dev/null
    dotnet "$OUT/cs/CrossTest.dll" snapshot "$OUT/snap.bin"
    "$OUT/crosstest" snapshot "$OUT/snap.bin"
    "$OUT/crosstest" hello "$OUT/hello.bin" "s3cret"
    [ "$(dotnet "$OUT/cs/CrossTest.dll" hello "$OUT/hello.bin" s3cret)" = "partial=NeedMoreData full=Ok" ]
    [ "$(dotnet "$OUT/cs/CrossTest.dll" hello "$OUT/hello.bin" wrong)" = "partial=NeedMoreData full=WrongKey" ]
    echo "hello OK"
else
    echo "== skipping C# wire test (no dotnet)"
fi
echo "== all tests passed"
