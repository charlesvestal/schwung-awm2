#!/usr/bin/env bash
# Build the headless MU100 MAME host (macOS dev/verification harness).
#
# Links our awm2_host.cpp against the static libs produced by the mu100
# single-driver subtarget build in ../../mame-mame0288. Build those first with:
#   cd ../../mame-mame0288 && make SOURCES=src/mame/yamaha/ymmu100.cpp -j8
#
# Output: host/build/awm2_host
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
MAME_ROOT="${MAME_ROOT:-$(cd "$HERE/../../mame-mame0288" && pwd)}"
REL="$MAME_ROOT/build/osx_clang/bin/x64/Release"
MAME_OBJ="$MAME_ROOT/build/osx_clang/obj/x64/Release"
OUT="$HERE/build"
mkdir -p "$OUT"

if [ ! -d "$REL" ]; then
  echo "error: MAME release libs not found at $REL" >&2
  echo "build the mu100 subtarget first (see header of this script)" >&2
  exit 1
fi

# --- compile flags (mirrors MAME's own per-object flags) ---------------------
CXXFLAGS=(
  -std=c++20 -O3 -g -m64 -arch arm64 -pipe -fno-strict-aliasing
  -DNDEBUG -DCRLF=2 -DLSB_FIRST -DFLAC__NO_DLL -DPUGIXML_HEADER_ONLY
  -DASMJIT_STATIC -DHAVE_IMMINTRIN_H=0 -DSDL_DISABLE_IMMINTRIN_H=1 -DHAVE_SSE=0
  -Wno-unknown-pragmas -Wno-unused-value
  -I"$MAME_ROOT/src"
  -I"$MAME_ROOT/src/osd"
  -I"$MAME_ROOT/src/emu"
  -I"$MAME_ROOT/src/devices"
  -I"$MAME_ROOT/src/mame"
  -I"$MAME_ROOT/src/lib"
  -I"$MAME_ROOT/src/lib/util"
  -I"$MAME_ROOT/3rdparty"
  -I"$MAME_ROOT/build/generated/mame/layout"
  -I"$MAME_ROOT/3rdparty/zlib"
  -I"$MAME_ROOT/3rdparty/flac/include"
)

echo "[1/2] compiling awm2_host.cpp + osd_stubs.cpp"
clang++ "${CXXFLAGS[@]}" -c "$HERE/awm2_host.cpp" -o "$OUT/awm2_host.o"
clang++ "${CXXFLAGS[@]}" -c "$HERE/osd_stubs.cpp" -o "$OUT/osd_stubs.o"

# --- link (mirrors MAME's executable link line, minus its main object) -------
# emulator_info implementations + the driver list come from these MAME objects.
MAME_MAIN_OBJS=(
  "$MAME_OBJ/src/mame/mame.o"
  "$MAME_OBJ/generated/mame/mame/drivlist.o"
  "$MAME_OBJ/generated/version.o"
)

# Library order matters (matches MAME's link line).
LIBS=(
  "$REL/mame_mame/libmame_mame.a"
  "$REL/libfrontend.a"
  "$REL/mame_mame/liboptional.a"
  "$REL/libemu.a"
  "$REL/libosd_sdl3.a"
  "$REL/mame_mame/libformats.a"
  "$REL/mame_mame/libdasm.a"
  "$REL/libutils.a"
  "$REL/libexpat.a"
  "$REL/libsoftfloat3.a"
  "$REL/libwdlfft.a"
  "$REL/libymfm.a"
  "$REL/libjpeg.a"
  "$REL/lib7z.a"
  "$REL/libasmjit.a"
  "$REL/liblua.a"
  "$REL/liblualibs.a"
  "$REL/liblinenoise.a"
  "$REL/libzlib.a"
  "$REL/libzstd.a"
  "$REL/libflac.a"
  "$REL/libutf8proc.a"
  "$REL/libsqlite3.a"
  "$REL/libportaudio.a"
  "$REL/libportmidi.a"
  "$REL/libocore_sdl3.a"
)

# SDL-free: with the minimal register_options() override no SDL/bgfx modules are
# pulled, so libSDL3/libbgfx/libbimg/libbx/libqtdbg and the GUI frameworks drop
# out. This mirrors the self-contained aarch64 dsp.so.
echo "[2/2] linking awm2_host (SDL-free)"
clang++ -o "$OUT/awm2_host" \
  "$OUT/awm2_host.o" "$OUT/osd_stubs.o" \
  "${MAME_MAIN_OBJS[@]}" \
  -L"$REL" -L"$REL/mame_mame" \
  -m64 -arch arm64 \
  "${LIBS[@]}" \
  -Wl,-framework,CoreMedia -Wl,-framework,CoreVideo \
  -Wl,-framework,Foundation -Wl,-framework,CoreAudio -Wl,-framework,AudioToolbox \
  -framework CoreMIDI -framework CoreServices -framework IOKit \
  -framework ApplicationServices \
  -lpthread -lm

echo "built: $OUT/awm2_host"
