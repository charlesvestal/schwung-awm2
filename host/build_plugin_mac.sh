#!/usr/bin/env bash
# Build the module plugin (src/dsp/awm2_plugin.cpp) as a macOS .dylib for
# off-device validation, plus the test loader. This mirrors what the Docker
# aarch64 build (scripts/build.sh) will produce as dist/awm2/dsp.so.
#
# Outputs: host/build/dsp.dylib, host/build/test_plugin
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
MAME_ROOT="${MAME_ROOT:-$(cd "$HERE/../../mame-mame0288" && pwd)}"
REL="$MAME_ROOT/build/osx_clang/bin/x64/Release"
MAME_OBJ="$MAME_ROOT/build/osx_clang/obj/x64/Release"
OUT="$HERE/build"
mkdir -p "$OUT"

[ -d "$REL" ] || { echo "error: build the mu100 subtarget first ($REL missing)" >&2; exit 1; }

CXXFLAGS=(
  -std=c++20 -O2 -g -m64 -arch arm64 -pipe -fno-strict-aliasing -fPIC
  -DNDEBUG -DCRLF=2 -DLSB_FIRST -DFLAC__NO_DLL -DPUGIXML_HEADER_ONLY
  -DASMJIT_STATIC -DHAVE_IMMINTRIN_H=0 -DSDL_DISABLE_IMMINTRIN_H=1 -DHAVE_SSE=0
  -Wno-unknown-pragmas -Wno-unused-value
  -I"$HERE"
  -I"$MAME_ROOT/src" -I"$MAME_ROOT/src/osd" -I"$MAME_ROOT/src/emu"
  -I"$MAME_ROOT/src/devices" -I"$MAME_ROOT/src/mame" -I"$MAME_ROOT/src/lib"
  -I"$MAME_ROOT/src/lib/util" -I"$MAME_ROOT/3rdparty"
  -I"$MAME_ROOT/build/generated/mame/layout"
  -I"$MAME_ROOT/3rdparty/zlib" -I"$MAME_ROOT/3rdparty/flac/include"
)

MAME_MAIN_OBJS=(
  "$MAME_OBJ/src/mame/mame.o"
  "$MAME_OBJ/generated/mame/mame/drivlist.o"
  "$MAME_OBJ/generated/version.o"
)
# SDL-free (matches the aarch64 dsp.so): the patched register_options() pulls no
# SDL/bgfx modules, so those libs and the GUI frameworks drop out.
LIBS=(
  "$REL/mame_mame/libmame_mame.a" "$REL/libfrontend.a" "$REL/mame_mame/liboptional.a"
  "$REL/libemu.a" "$REL/libosd_sdl3.a"
  "$REL/mame_mame/libformats.a" "$REL/mame_mame/libdasm.a" "$REL/libutils.a"
  "$REL/libexpat.a" "$REL/libsoftfloat3.a" "$REL/libwdlfft.a" "$REL/libymfm.a"
  "$REL/libjpeg.a" "$REL/lib7z.a" "$REL/libasmjit.a" "$REL/liblua.a"
  "$REL/liblualibs.a" "$REL/liblinenoise.a" "$REL/libzlib.a" "$REL/libzstd.a"
  "$REL/libflac.a" "$REL/libutf8proc.a" "$REL/libsqlite3.a" "$REL/libportaudio.a"
  "$REL/libportmidi.a" "$REL/libocore_sdl3.a"
)
FRAMEWORKS=(
  -Wl,-framework,CoreMedia -Wl,-framework,CoreVideo -Wl,-framework,Foundation
  -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox
  -framework CoreMIDI -framework CoreServices -framework IOKit
  -framework ApplicationServices
  -lpthread -lm
)

echo "[1/3] compiling awm2_plugin.cpp + osd_stubs.cpp"
clang++ "${CXXFLAGS[@]}" -c "$ROOT/src/dsp/awm2_plugin.cpp" -o "$OUT/awm2_plugin.o"
clang++ "${CXXFLAGS[@]}" -c "$HERE/osd_stubs.cpp" -o "$OUT/osd_stubs.o"

echo "[2/3] linking dsp.dylib (SDL-free)"
clang++ -dynamiclib -o "$OUT/dsp.dylib" \
  "$OUT/awm2_plugin.o" "$OUT/osd_stubs.o" "${MAME_MAIN_OBJS[@]}" \
  -L"$REL" -L"$REL/mame_mame" -m64 -arch arm64 \
  "${LIBS[@]}" "${FRAMEWORKS[@]}"

echo "[3/3] compiling test_plugin"
clang++ -std=c++20 -O2 -g -arch arm64 "$HERE/test_plugin.cpp" -o "$OUT/test_plugin"

echo "built: $OUT/dsp.dylib  $OUT/test_plugin"
