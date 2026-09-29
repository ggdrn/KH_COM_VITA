#!/bin/sh
# Prepares a working tree for the PS Vita port:
#   1. clones the Kingdom Hearts: Chain of Memories decompilation
#      (https://github.com/Pheenoh/khcom) at the commit this port targets,
#   2. applies patches/khcom-vita.patch (the port's changes to the decomp),
#   3. copies the port sources (port/vita, tools/vita) into it.
#
# Usage: scripts/setup.sh [destination]   (default: ./khcom)
# Set KHCOM_URL to clone the decompilation from somewhere else (e.g. a mirror).
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEST="${1:-$REPO/khcom}"
KHCOM_URL="${KHCOM_URL:-https://github.com/Pheenoh/khcom.git}"
KHCOM_COMMIT="addf92171e56e0d774525cc5f921afb489b9afff"

if [ -e "$DEST" ]; then
    echo "error: $DEST already exists" >&2
    exit 1
fi

git clone "$KHCOM_URL" "$DEST"
git -C "$DEST" checkout -B vita-port "$KHCOM_COMMIT"
git -C "$DEST" apply "$REPO/patches/khcom-vita.patch"
mkdir -p "$DEST/port" "$DEST/tools"
cp -R "$REPO/port/vita" "$DEST/port/"
cp -R "$REPO/tools/vita" "$DEST/tools/"

echo
echo "Ready: $DEST"
echo "Next: put your own ROM dump at $DEST/roms/B8CE.gba and follow README.md."
