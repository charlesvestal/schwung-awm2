#!/usr/bin/env bash
# Build the AWM2 (MU100) module for the Ableton Move (aarch64 Linux, glibc 2.35).
#
# Cross-compiles MAME's mu100 single-driver subtarget + links a self-contained,
# SDL-free dsp.so. Uses Docker automatically unless already inside it (or
# CROSS_PREFIX is set). See scripts/Dockerfile.
#
# MAME source resolution:
#   MAME_SRC=<dir>  use an existing (patched) MAME tree, e.g. ../mame-mame0288
#   otherwise       clone MAME 0.288 into build/mame-src and apply patches/*.patch
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
IMAGE_NAME="move-anything-awm2-builder"

# --- Docker bootstrap --------------------------------------------------------
if [ -z "${CROSS_PREFIX:-}" ] && [ ! -f "/.dockerenv" ]; then
  echo "=== AWM2 Module Build (via Docker) ==="
  if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Building Docker image (first time only)..."
    docker build -t "$IMAGE_NAME" -f "$SCRIPT_DIR/Dockerfile" "$REPO_ROOT"
  fi
  # Mount a local patched MAME tree if the caller points at one (fast path).
  MOUNT_ARGS=()
  if [ -n "${MAME_SRC:-}" ]; then
    MAME_SRC_ABS="$(cd "$MAME_SRC" && pwd)"
    MOUNT_ARGS=(-v "$MAME_SRC_ABS:/mame:ro" -e MAME_SRC=/mame_rw)
  fi
  docker run --rm \
    -v "$REPO_ROOT:/build" \
    "${MOUNT_ARGS[@]}" \
    -w /build \
    "$IMAGE_NAME" \
    ./scripts/build.sh
  echo "=== Done ==="
  exit 0
fi

# === Actual build (inside Docker / cross env) ================================
cd "$REPO_ROOT"
CROSS_PREFIX="${CROSS_PREFIX:-aarch64-linux-gnu-}"
# Cap parallelism: heavy MAME C++ TUs can each use ~1-2 GB, so -j == ncpu can OOM
# a memory-limited Docker VM. Default to 4; override with BUILD_JOBS.
NPROC="${BUILD_JOBS:-4}"
mkdir -p build dist/awm2

# --- 1. MAME source ----------------------------------------------------------
if [ -n "${MAME_SRC:-}" ] && [ -d /mame ]; then
  # Read-only mounted tree -> copy the source (not build artifacts) so we can
  # build into it. Patches are assumed already applied in the mounted tree.
  MAME="$REPO_ROOT/build/mame-src"
  if [ ! -f "$MAME/.awm2-copied" ]; then
    echo "Copying mounted MAME source (excluding build/ and .git)..."
    mkdir -p "$MAME"
    (cd /mame && tar cf - --exclude=./build --exclude=./.git . ) | (cd "$MAME" && tar xf -)
    touch "$MAME/.awm2-copied"
  fi
  PATCHED=1
else
  MAME="$REPO_ROOT/build/mame-src"
  if [ ! -d "$MAME/.git" ] && [ ! -d "$MAME/src" ]; then
    echo "Cloning MAME 0.288..."
    git clone --depth 1 -b mame0288 https://github.com/mamedev/mame.git "$MAME"
  fi
  PATCHED=0
fi

# --- 2. apply patches (swp30 fix + headless SDL-free OSD) ---------------------
if [ "${PATCHED:-0}" != "1" ] && [ ! -f "$MAME/.awm2-patched" ]; then
  echo "Applying patches..."
  for p in "$REPO_ROOT"/patches/*.patch; do
    echo "  $p"
    (cd "$MAME" && patch -p1 < "$p")
  done
  touch "$MAME/.awm2-patched"
fi

# --- 3. cross-build the mu100 subtarget static libs --------------------------
echo "Cross-building MAME mu100 subtarget for aarch64 (this is long the first time)..."
# Use the `linux` phony target -> config=release (-std=c++20, no -m32/-m64). The
# bare default builds config=debug32 (-m32, wrong arch, breaks C++20); linux_x64
# uses config=release64 which forces -m64 that aarch64-gcc rejects. Plain release
# carries only -march=armv8-a -mtune=cortex-a72 (set via ARCHOPTS) — correct for
# aarch64. PTR64=1 still sets the 64-bit-pointer defines.
# The `linux` target also links the full `mame` executable, which FAILS under our
# headless SDL-free OSD (no SDL main / stripped modules). That's expected and
# irrelevant — we only need the static libraries it archives first. Tolerate the
# exe-link failure, then verify the libs exist.
make -C "$MAME" linux \
  CROSS_BUILD=1 \
  OVERRIDE_CC="${CROSS_PREFIX}gcc" \
  OVERRIDE_CXX="${CROSS_PREFIX}g++" \
  OVERRIDE_LD="${CROSS_PREFIX}g++" \
  TARGETOS=linux PTR64=1 NOWERROR=1 \
  NO_X11=1 NO_OPENGL=1 NO_USE_XINPUT=1 USE_BGFX=0 \
  NO_USE_PORTAUDIO=1 NO_USE_PULSEAUDIO=1 NO_USE_PIPEWIRE=1 \
  ARCHOPTS="-march=armv8-a -mtune=cortex-a72 -fPIC" \
  PYTHON_EXECUTABLE=python3 \
  SOURCES=src/mame/yamaha/ymmu100.cpp \
  -j"$NPROC" || echo "(mame exe link failed as expected; using the static libs)"

echo "Locating built libraries + objects..."
LIBEMU="$(find "$MAME" -name libemu.a | head -1)"
[ -n "$LIBEMU" ] || { echo "error: MAME static libs not built (libemu.a missing)" >&2; exit 1; }
OBJV="$(find "$MAME" -path '*obj/Release/generated/version.o' | head -1)"
[ -n "$OBJV" ] || { echo "error: MAME objects not built (version.o missing)" >&2; exit 1; }
OBJ="${OBJV%/generated/version.o}"
GENLAYOUT="$(dirname "$(find "$MAME" -path '*generated/mame/layout/*.h' | head -1)")"
echo "  libs near: $(dirname "$LIBEMU")"
echo "  objs:      $OBJ"

# --- 4. compile + link dsp.so (SDL-free) -------------------------------------
echo "Compiling module glue..."
CXXFLAGS=(
  -std=c++20 -O2 -fPIC -fno-strict-aliasing
  -march=armv8-a -mtune=cortex-a72
  -DNDEBUG -DCRLF=2 -DLSB_FIRST -DFLAC__NO_DLL -DPUGIXML_HEADER_ONLY
  -DASMJIT_STATIC
  -I"$REPO_ROOT/host"
  -I"$MAME/src" -I"$MAME/src/osd" -I"$MAME/src/emu" -I"$MAME/src/devices"
  -I"$MAME/src/mame" -I"$MAME/src/lib" -I"$MAME/src/lib/util" -I"$MAME/3rdparty"
  -I"$GENLAYOUT" -I"$MAME/3rdparty/zlib" -I"$MAME/3rdparty/flac/include"
)
"${CROSS_PREFIX}g++" "${CXXFLAGS[@]}" -c src/dsp/awm2_plugin.cpp -o build/awm2_plugin.o
"${CROSS_PREFIX}g++" "${CXXFLAGS[@]}" -c host/osd_stubs.cpp      -o build/osd_stubs.o

echo "Linking dsp.so..."
# Whole MAME static-lib set (SDL/bgfx libs intentionally excluded). --start-group
# resolves the heavy circular deps among the MAME archives.
MAME_OBJS=( "$OBJ/src/mame/mame.o" "$OBJ/generated/mame/mame/drivlist.o" "$OBJ/generated/version.o" )
ARLIBS=$(find "$MAME/scripts" -name '*.a' ! -name 'libbgfx.a' ! -name 'libbimg.a' ! -name 'libbx.a' \
         ! -name 'libqtdbg*' ! -name 'libprecompile.a' | tr '\n' ' ')
"${CROSS_PREFIX}g++" -shared -o dist/awm2/dsp.so \
  build/awm2_plugin.o build/osd_stubs.o "${MAME_OBJS[@]}" \
  -Wl,--start-group $ARLIBS -Wl,--end-group \
  -lpthread -lm -ldl -static-libgcc -static-libstdc++

# --- 5. package --------------------------------------------------------------
cp src/module.json dist/awm2/module.json
mkdir -p dist/awm2/roms
( cd dist && tar -czf awm2-module.tar.gz awm2/ )
echo ""
file dist/awm2/dsp.so
echo "=== Build complete: dist/awm2/dsp.so ==="
