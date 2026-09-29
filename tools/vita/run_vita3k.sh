#!/bin/sh
# Installs build/vita/khcom_us.vpk in Vita3K, runs it for N seconds (default
# 60) and prints the emulator's errors plus the port's own log.
cd "$(dirname "$0")/../.."
LOG="${TMPDIR:-/tmp}/khcom_vita3k.log"
V3K_FS="$HOME/Library/Application Support/Vita3K/Vita3K/fs"
pkill -f Vita3K.app/Contents/MacOS/Vita3K
sleep 1
/Applications/Vita3K.app/Contents/MacOS/Vita3K build/vita/khcom_us.vpk >"$LOG" 2>&1 &
sleep "${1:-60}"
grep -E "\|[EC]\||EXC_BAD|PC: " "$LOG" | grep -v "bootimage|sysmodule|khcom.sav|Discord|preloaded module" | head -${2:-30}
echo "--- port log"
cat "$V3K_FS/ux0/data/khcom/log.txt"
