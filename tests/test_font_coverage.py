#!/usr/bin/env python3
"""Every on-screen string of the QCA companion must be covered by its bundled fonts.

* Non-ASCII characters in C string literals of the app sources must exist in the
  16 px CJK font (LV_SYMBOL_* private-use glyphs come from the Montserrat fallback).
* The large status words (voice_state_word, status_28_chars.txt) must exist in the
  28 px status font.
"""
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FONTS = ROOT / "assets" / "fonts"
APP_SOURCES = [
    "main/main.c",
    "main/app_ui.c",
    "main/app_wifi.c",
    "main/app_portal.c",
    "main/qca_pat.c",
    "main/qca_api.c",
    "main/qca_voice.c",
    "main/voice_logic.c",
]
PRIVATE_USE = range(0xF000, 0xF900)  # LV_SYMBOL_* (Montserrat fallback)


def load_codepoints(name):
    cps = set()
    for line in (FONTS / name).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line:
            assert line.startswith("U+"), f"{name}: bad line {line!r}"
            cps.add(int(line[2:], 16))
    return cps


def string_literals(text):
    """Yield the contents of C string literals, skipping comments and char literals."""
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            assert j >= 0, "unterminated comment"
            i = j + 2
        elif c in "\"'":
            j, buf = i + 1, []
            while text[j] != c:
                if text[j] == "\\":
                    j += 2  # escapes are ASCII-only in these sources
                    continue
                buf.append(text[j])
                j += 1
            if c == '"':
                yield "".join(buf)
            i = j + 1
        else:
            i += 1


def function_returns(text, func):
    m = re.search(r"\b" + func + r"\s*\([^)]*\)\s*\{(.*?)\n\}", text, re.S)
    assert m, f"{func} not found"
    return re.findall(r'return\s+"([^"]*)"', m.group(1))


def main():
    cjk16 = load_codepoints("app_font_cjk_16.codepoints.txt")
    status28 = load_codepoints("app_font_status_28.codepoints.txt")
    failures = []

    checked = 0
    for rel in APP_SOURCES:
        text = (ROOT / rel).read_text(encoding="utf-8")
        for lit in string_literals(text):
            for ch in lit:
                cp = ord(ch)
                if cp < 0x80 or cp in PRIVATE_USE:
                    continue
                checked += 1
                if cp not in cjk16:
                    failures.append(f"{rel}: U+{cp:04X} {ch!r} in {lit!r} missing from 16 px font")

    words = function_returns((ROOT / "main/voice_logic.c").read_text(encoding="utf-8"),
                             "voice_state_word")
    words += [w.strip() for w in (FONTS / "status_28_chars.txt").read_text(encoding="utf-8").splitlines()]
    assert len([w for w in words if w]) >= 7
    for word in words:
        for ch in word:
            if ord(ch) not in status28:
                failures.append(f"status word {word!r}: U+{ord(ch):04X} missing from 28 px font")

    assert checked > 100, f"too few CJK characters scanned ({checked}); scanner broken?"
    if failures:
        print("\n".join(failures), file=sys.stderr)
        return 1
    print(f"font coverage: {checked} CJK characters OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
