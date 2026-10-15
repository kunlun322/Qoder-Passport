#!/usr/bin/env python3
"""Regenerate the QCA companion LVGL fonts (app_font_cjk_16 / app_font_status_28).

Not part of the repository gate. Requirements (run outside the gate):
  * fontTools (pip install fonttools==4.60.1) for variable-font instancing/subsetting;
  * Node.js + lv_font_conv 1.5.3 (npx -y lv_font_conv@1.5.3);
  * Noto Sans SC variable font (SIL OFL 1.1), e.g. C:/Windows/Fonts/NotoSansSC-VF.ttf.

Usage:
  python assets/fonts/build_fonts.py <NotoSansSC-VF.ttf> <work_dir>

Charset of the 16 px font: printable ASCII + every GB2312 code point that the
source font contains (symbols rows 1-9 and all 6763 level-1/level-2 hanzi).
The 28 px font only contains the fixed status words in status_28_chars.txt.
"""
import os
import subprocess
import sys
from pathlib import Path

from fontTools.ttLib import TTFont
from fontTools.varLib import instancer
from fontTools import subset

HERE = Path(__file__).resolve().parent
WEIGHT = 500  # Medium: the VF default instance is wght=100 (Thin), unreadable at 16 px.


def gb2312_codepoints():
    cps = set()
    for hi in range(0xA1, 0xF8):
        for lo in range(0xA1, 0xFF):
            try:
                ch = bytes([hi, lo]).decode("gb2312")
            except UnicodeDecodeError:
                continue
            cps.add(ord(ch))
    return cps


def ranges(cps):
    out, run = [], None
    for cp in sorted(cps):
        if run and cp == run[1] + 1:
            run[1] = cp
        else:
            if run:
                out.append(run)
            run = [cp, cp]
    if run:
        out.append(run)
    return ",".join(f"0x{a:X}-0x{b:X}" if a != b else f"0x{a:X}" for a, b in out)


def static_subset(src, dst, cps):
    font = TTFont(src)
    font = instancer.instantiateVariableFont(font, {"wght": WEIGHT})
    cmap = font.getBestCmap()
    keep = sorted(cp for cp in cps if cp in cmap)
    opts = subset.Options()
    opts.layout_features = []
    opts.hinting = False
    sub = subset.Subsetter(opts)
    sub.populate(unicodes=keep)
    sub.subset(font)
    font.save(dst)
    return keep


def lv_font_conv(ttf, cps, size, name):
    out = HERE / f"{name}.c"
    cmd = ["npx", "-y", "lv_font_conv@1.5.3", "--font", str(ttf), "--range", "0x20-0x7E,0xA0-0xFFEF",  # TTF already subset; keeps argv short for cmd.exe
           
           "--size", str(size), "--bpp", "4", "--format", "lvgl", "--no-compress",
           "--lv-font-name", name, "--lv-include", "lvgl.h", "-o", str(out)]
    subprocess.run(cmd, check=True, shell=(os.name == "nt"))
    (HERE / f"{name}.codepoints.txt").write_text(
        "".join(f"U+{cp:04X}\n" for cp in cps), encoding="utf-8")


def main():
    src, work = Path(sys.argv[1]), Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    base = set(range(0x20, 0x7F))
    cjk = base | gb2312_codepoints()
    keep16 = static_subset(src, work / "cjk16.ttf", cjk)
    lv_font_conv(work / "cjk16.ttf", keep16, 16, "app_font_cjk_16")
    words = (HERE / "status_28_chars.txt").read_text(encoding="utf-8")
    status = {ord(c) for c in words if not c.isspace()} | set(range(0x30, 0x3A))
    keep28 = static_subset(src, work / "status28.ttf", status)
    lv_font_conv(work / "status28.ttf", keep28, 28, "app_font_status_28")
    print(f"app_font_cjk_16: {len(keep16)} glyphs; app_font_status_28: {len(keep28)} glyphs")


if __name__ == "__main__":
    main()
