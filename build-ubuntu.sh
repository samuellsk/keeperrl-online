#!/usr/bin/env bash
#
# One-shot build script for Ubuntu 24.04 (also fine on 22.04 and Debian 12).
#
#   ./build-ubuntu.sh
#
# Installs the build dependencies, creates the directories the Makefile expects,
# compiles, and tells you how to run it. Safe to re-run: apt skips what is already
# installed and make only rebuilds what changed.
#
set -euo pipefail

cd "$(dirname "$0")"

JOBS="$(nproc 2>/dev/null || echo 2)"

PACKAGES=(
  clang                 # the compiler the Makefile expects (GCC also works, see BUILDING.md)
  make
  git
  pkg-config
  libsdl2-dev           # window, input, audio device
  libsdl2-image-dev     # PNG loading
  libgl1-mesa-dev       # OpenGL
  libopenal-dev         # sound
  libvorbis-dev         # ogg vorbis audio
  libogg-dev
  libtheora-dev         # intro video
  zlib1g-dev            # save compression
  libcurl4-openssl-dev  # HTTP(S)
  libssl-dev            # TLS + hashing for the online layer
  liblzma-dev           # dungeon blob transport compression
)

echo "==> Installing build dependencies (needs sudo)"
if command -v apt-get >/dev/null 2>&1; then
  sudo apt-get update
  sudo apt-get install -y "${PACKAGES[@]}"
else
  echo "!! Not an apt system. Install the equivalents of: ${PACKAGES[*]}" >&2
  exit 1
fi

# The Makefile writes object files into these but does NOT create them, and git cannot
# store empty directories -- so a fresh clone may not have them.
echo "==> Creating build directories"
mkdir -p obj/extern obj-opt/extern

echo "==> Building with $JOBS parallel jobs (this takes a while: ~10-25 min)"
make -j"$JOBS" NO_STEAMWORKS=true RELEASE=true OPT=true GCC=clang++

echo
echo "==> Done. Binary: $(pwd)/keeper"
echo
echo "Run it FROM THIS DIRECTORY -- the game looks for appconfig.txt, data_free/ and"
echo "mods/ in the CURRENT working directory, not next to the binary:"
echo
echo "    ./keeper"
echo
echo "No graphics? That is expected: this repository ships no artwork. The game falls"
echo "back to ASCII. To play with sprites and sound, copy the 'data' folder out of a"
echo "purchased copy of KeeperRL into this directory. See BUILDING.md."
