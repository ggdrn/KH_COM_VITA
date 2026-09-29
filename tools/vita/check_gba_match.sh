#!/bin/sh
# Rebuild the GBA ROM and confirm it still matches the original (SHA-1).
# Works around host issues: m4a_1.s uses the modern assembler and the final
# link uses arm-none-eabi-ld (the legacy 2.10 ld cannot link the runtime libs).
set -e
cd "$(dirname "$0")/../.."
export PATH="$PWD/.venv/bin:$PATH"
python3 configure.py >/dev/null
python3 - <<'PY'
import re
p = "build.ninja"
s = open(p).read()
s = re.sub(r"(build build/us/asm/m4a_1\.o: as [^\n]*(?:\n    [^\n]*)*)\n  as = \$legacy_as\n  asflags = \$legacy_asflags",
           r"\1\n  as = arm-none-eabi-as\n  asflags = -mcpu=arm7tdmi -mthumb-interwork -I . -I include", s)
open(p, "w").write(s)
PY
objs=$(sed -n '/^build build\/us\/com_us.elf: ld/,/|/p' build.ninja | tr -d '$\n' | sed 's/build build\/us\/com_us.elf: ld//; s/|.*//')
ninja $objs
arm-none-eabi-ld --no-warn-mismatch -T build/us/ldscript.ld -Map build/us/com_us.map -o build/us/com_us.elf $objs 2>/dev/null
arm-none-eabi-objcopy -O binary --only-section=.text --pad-to=0x0A000000 --gap-fill=0xFF build/us/com_us.elf build/us/com_us.gba
python3 tools/gbafix.py build/us/com_us.gba KINGDOMHEART B8CE GD >/dev/null
sum=$(shasum build/us/com_us.gba | cut -d' ' -f1)
if [ "$sum" = "10729bd884f8fdca7a310b6d606c52e46657aa48" ]; then
    echo "OK: build/us/com_us.gba matches"
else
    echo "MISMATCH: build/us/com_us.gba ($sum)"
    exit 1
fi
