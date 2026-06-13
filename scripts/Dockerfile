# AWM2 (MU100) Module Build Environment
# Targets: Ableton Move (aarch64 Linux, glibc 2.35)
# Ubuntu 22.04 pins glibc 2.35 to match the device; cross-compile amd64 -> aarch64
# (also what the GitHub Actions release runners use).

FROM ubuntu:22.04

RUN apt-get update && apt-get install -y \
    gcc-aarch64-linux-gnu \
    g++-aarch64-linux-gnu \
    gcc g++ \
    make \
    python3 \
    git \
    file \
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# SDL2 dev headers (arch-independent) are needed only to COMPILE MAME's SDL OSD
# .cpp files; the resulting SDL objects are excluded at link time (SDL-free .so).
# SDL_ttf is needed to compile the (unused) SDL font module.
RUN apt-get update && apt-get install -y libsdl2-dev libsdl2-ttf-dev libfontconfig1-dev \
    && rm -rf /var/lib/apt/lists/*

# MAME's OSD makefile injects `-I$(shell qmake6 -query QT_INSTALL_HEADERS)` for
# the Qt debugger. Without Qt, qmake6 is missing and the shell expands to empty,
# leaving a DANGLING `-I` that swallows the following `-std=c++20` token — so the
# OSD silently compiles as C++17 (breaks char8_t/std::span/heterogeneous lookup).
# A stub qmake6 returning a valid dir keeps the `-I` well-formed and `-std=c++20`
# intact. (We don't use the Qt debugger; it's excluded at link time.)
RUN printf '#!/bin/sh\necho /usr/include\n' > /usr/local/bin/qmake6 \
    && chmod +x /usr/local/bin/qmake6

WORKDIR /build

ENV CROSS_PREFIX=aarch64-linux-gnu-
ENV CC=aarch64-linux-gnu-gcc
ENV CXX=aarch64-linux-gnu-g++
