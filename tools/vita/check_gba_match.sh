#!/bin/sh
# Rebuild the GBA ROM and confirm it still matches the original (SHA-1).
# Usage: check_gba_match.sh [us|eu] (default us).
# Works around host issues: m4a_1.s uses the modern assembler and the final
# link uses arm-none-eabi-ld (the legacy 2.10 ld cannot link the runtime libs).
set -e
cd "$(dirname "$0")/../.."
export PATH="$PWD/.venv/bin:$PATH"
v=${1:-us}
case "$v" in
us) code=B8CE; want=10729bd884f8fdca7a310b6d606c52e46657aa48 ;;
eu) code=B8CP; want=8db73586cdb11b3795907edebf43228dbcd3e6b2 ;;
*) echo "unknown version $v" >&2; exit 2 ;;
esac
python3 configure.py --version "$v" >/dev/null
V="$v" python3 - <<'PY'
import os, re
v = os.environ["V"]
p = "build.ninja"
s = open(p).read()
s = re.sub(r"(build build/" + v + r"/asm/m4a_1\.o: as [^\n]*(?:\n    [^\n]*)*)\n  as = \$legacy_as\n  asflags = \$legacy_asflags",
           r"\1\n  as = arm-none-eabi-as\n  asflags = -mcpu=arm7tdmi -mthumb-interwork -I . -I include", s)
open(p, "w").write(s)
PY
objs=$(sed -n "/^build build\/$v\/com_$v.elf: ld/,/|/p" build.ninja | tr -d '$\n' | sed "s/build build\/$v\/com_$v.elf: ld//; s/|.*//")
ninja $objs
arm-none-eabi-ld --no-warn-mismatch -T build/$v/ldscript.ld -Map build/$v/com_$v.map -o build/$v/com_$v.elf $objs 2>/dev/null
arm-none-eabi-objcopy -O binary --only-section=.text --pad-to=0x0A000000 --gap-fill=0xFF build/$v/com_$v.elf build/$v/com_$v.gba
python3 tools/gbafix.py build/$v/com_$v.gba KINGDOMHEART $code GD >/dev/null
sum=$(shasum build/$v/com_$v.gba | cut -d' ' -f1)
if [ "$sum" = "$want" ]; then
    echo "OK: build/$v/com_$v.gba matches"
else
    echo "MISMATCH: build/$v/com_$v.gba ($sum)"
    exit 1
fi
