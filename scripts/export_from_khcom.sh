#!/bin/sh
# Brings the port's current code from a khcom working tree into this repository:
#   1. patches/khcom-vita.patch = the port's changes to the decomp (include/, src/,
#      .gitignore) against the upstream commit scripts/setup.sh checks out,
#      including changes not committed in khcom yet,
#   2. port/vita and tools/vita, copied over (build leftovers and Sony's
#      libshacccg.suprx left out).
# The inverse of scripts/setup.sh. Review with `git diff` before committing.
#
# Usage: scripts/export_from_khcom.sh [path/to/khcom]   (default: ../khcom)
set -e

REPO="$(cd "$(dirname "$0")/.." && pwd)"
KHCOM="$(cd "${1:-$REPO/../khcom}" && pwd)"
BASE="$(sed -n 's/^KHCOM_COMMIT="\(.*\)"$/\1/p' "$REPO/scripts/setup.sh")"

if [ ! -d "$KHCOM/port/vita" ] || [ ! -d "$KHCOM/.git" ]; then
    echo "error: $KHCOM is not a khcom checkout with the port" >&2
    exit 1
fi
if ! git -C "$KHCOM" cat-file -e "$BASE^{commit}" 2>/dev/null; then
    echo "error: $KHCOM does not have the upstream commit $BASE (git fetch it first)" >&2
    exit 1
fi

git -C "$KHCOM" diff --binary "$BASE" -- include src .gitignore > "$REPO/patches/khcom-vita.patch"
# New decomp files the port added that git does not track yet.
untracked="$(git -C "$KHCOM" ls-files --others --exclude-standard -- include src)"
if [ -n "$untracked" ]; then
    echo "error: untracked files in khcom that the patch would miss:" >&2
    echo "$untracked" >&2
    exit 1
fi

for d in port/vita tools/vita; do
    rm -rf "$REPO/$d"
    mkdir -p "$REPO/$(dirname "$d")"
    cp -R "$KHCOM/$d" "$REPO/$d"
done
find "$REPO/port" "$REPO/tools" \( -name __pycache__ -o -name .DS_Store -o -name '*.suprx' \) -exec rm -rf {} +

echo "Exported port $(cat "$REPO/port/vita/VERSION") from $KHCOM"
git -C "$REPO" status --short
