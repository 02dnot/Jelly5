#!/usr/bin/env python3
"""Add gfx1013 (the PS5's GPU) to LGC's supported-GPU table.

amdllpc rejects -gfxip=10.1.3 with "Invalid gfxip: gfx1013" because
lgc/state/TargetInfo.cpp's gpuNameMap lists gfx1010/1011/1012 and 1030+ but not
1013. It is not behind a build flag - AMD simply does not ship the semi-custom
part in public AMDVLK. LLVM's AMDGPU backend DOES know gfx1013 (llc -mcpu=help
lists it), so only this table stands in the way.

gfx1013 is a GFX10.1 sibling of Navi12/gfx1011 and is modelled on it here,
including the integer-dot capabilities that separate 1011 from 1010. The
workaround bits are the GFX10.1 set; if a shader ever miscompiles in a way that
smells like a missing hardware workaround, this table is the first place to
look.
"""
import re
import sys
from pathlib import Path

path = Path(sys.argv[1] if len(sys.argv) > 1
            else "/opt/amdllpc/drivers/llpc/lgc/state/TargetInfo.cpp")
src = path.read_text(encoding="utf-8")

if "gfx1013" in src:
    print("TargetInfo.cpp already knows gfx1013; nothing to do")
    sys.exit(0)

# Clone setGfx1011Info verbatim as setGfx1013Info.
match = re.search(
    r"// gfx1011\n//\n// @param \[in/out\] targetInfo : Target info\n"
    r"static void setGfx1011Info\(TargetInfo \*targetInfo\) \{.*?\n\}\n",
    src, re.DOTALL)
if not match:
    sys.exit("could not locate setGfx1011Info in TargetInfo.cpp")
clone = (match.group(0)
         .replace("// gfx1011", "// gfx1013 (PS5) - modelled on gfx1011/Navi12")
         .replace("setGfx1011Info", "setGfx1013Info"))
src = src[:match.end()] + "\n" + clone + src[match.end():]

# Register it in the name map next to its GFX10.1 siblings.
anchor = '    {"gfx1012", "Navi14", &setGfx1012Info},    // gfx1012\n'
if anchor not in src:
    sys.exit("could not locate the gfx1012 gpuNameMap entry")
src = src.replace(
    anchor,
    anchor + '    {"gfx1013", "Navi10Lite", &setGfx1013Info}, // gfx1013 (PS5)\n')

path.write_text(src, encoding="utf-8")
print("patched TargetInfo.cpp: added gfx1013 -> setGfx1013Info")
