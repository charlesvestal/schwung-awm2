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

echo "[1/2] compiling awm2_host.cpp"
clang++ "${CXXFLAGS[@]}" -c "$HERE/awm2_host.cpp" -o "$OUT/awm2_host.o"

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
  "$REL/libqtdbg_sdl3.a"
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
  "$REL/libbgfx.a"
  "$REL/libbimg.a"
  "$REL/libbx.a"
  "$REL/libocore_sdl3.a"
)

echo "[2/2] linking awm2_host"
clang++ -o "$OUT/awm2_host" \
  "$OUT/awm2_host.o" \
  "${MAME_MAIN_OBJS[@]}" \
  -L"$REL" -L"$REL/mame_mame" \
  -m64 -arch arm64 \
  "${LIBS[@]}" \
  -framework QuartzCore -framework OpenGL -framework IOKit -weak_framework Metal \
  -L/opt/homebrew/lib -Wl,-rpath,/opt/homebrew/lib \
  -Wl,-framework,CoreMedia -Wl,-framework,CoreVideo -Wl,-framework,Cocoa \
  -Wl,-weak_framework,UniformTypeIdentifiers -Wl,-framework,IOKit \
  -Wl,-framework,ForceFeedback -Wl,-framework,Carbon -Wl,-framework,CoreAudio \
  -Wl,-framework,AudioToolbox -Wl,-framework,AVFoundation -Wl,-framework,Foundation \
  -Wl,-framework,GameController -Wl,-framework,Metal -Wl,-framework,QuartzCore \
  -Wl,-weak_framework,CoreHaptics \
  -framework Cocoa -lSDL3 -lpthread -lm -framework OpenGL -framework CoreMIDI \
  -framework AudioUnit -framework AudioToolbox -framework CoreAudio -framework CoreServices

echo "built: $OUT/awm2_host"
