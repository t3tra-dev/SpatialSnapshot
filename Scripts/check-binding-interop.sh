#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
cmake --build build
destination="$PWD/build/binding-interop"
mkdir -p "$destination"
cp Fixtures/bindings/interop/source.heic Fixtures/bindings/interop/source.mov \
   Fixtures/bindings/interop/still.ssps Fixtures/bindings/interop/video.ssps \
   Fixtures/bindings/interop/grid.heic Fixtures/bindings/interop/grid.ssps "$destination/"
build/ss_binding_tests --bind heif "$destination/source.heic" "$destination/still.ssps" "$destination/bound.heic"
build/ss_binding_tests --bind quicktime "$destination/source.mov" "$destination/video.ssps" "$destination/bound.mov"
build/ss_binding_tests --strip heif "$destination/bound.heic" "$destination/stripped.heic"
build/ss_binding_tests --strip quicktime "$destination/bound.mov" "$destination/stripped.mov"
build/ss_binding_tests --bind heif "$destination/grid.heic" "$destination/grid.ssps" "$destination/grid-bound.heic"
build/ss_binding_tests --strip heif "$destination/grid-bound.heic" "$destination/grid-stripped.heic"
if command -v ffmpeg >/dev/null 2>&1; then
    ffmpeg -v error -i "$destination/source.mov" -map 0:v:0 -f framemd5 "$destination/source.framemd5" -y
    ffmpeg -v error -i "$destination/bound.mov" -map 0:v:0 -f framemd5 "$destination/bound.framemd5" -y
    ffmpeg -v error -i "$destination/stripped.mov" -map 0:v:0 -f framemd5 "$destination/stripped.framemd5" -y
    cmp "$destination/source.framemd5" "$destination/bound.framemd5"
    cmp "$destination/source.framemd5" "$destination/stripped.framemd5"
fi
if command -v heif-convert >/dev/null 2>&1; then
    heif-convert "$destination/source.heic" "$destination/original.png"
    heif-convert "$destination/bound.heic" "$destination/decoded.png"
    heif-convert "$destination/stripped.heic" "$destination/stripped.png"
    cmp "$destination/original.png" "$destination/decoded.png"
    cmp "$destination/decoded.png" "$destination/stripped.png"
    heif-convert "$destination/grid.heic" "$destination/grid-original.png"
    heif-convert "$destination/grid-bound.heic" "$destination/grid-decoded.png"
    heif-convert "$destination/grid-stripped.heic" "$destination/grid-stripped.png"
    cmp "$destination/grid-original.png" "$destination/grid-decoded.png"
    cmp "$destination/grid-decoded.png" "$destination/grid-stripped.png"
fi
if [ "$(uname)" = Darwin ]; then
    swiftc -parse-as-library -module-cache-path "$PWD/.build/module-cache" \
        Tests/Bindings/AppleInterop.swift -o "$destination/apple-interop"
    "$destination/apple-interop" "$destination"
fi
