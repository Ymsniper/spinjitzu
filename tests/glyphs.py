#!/usr/bin/env python3
# Every character the window can show is in the fonts it carries: a missing
# one is drawn as "?" with no warning. The body subset must hold every
# character in the source's strings, the seal font every Japanese one, and
# the display face all of printable ASCII.
#
#   python3 tests/glyphs.py
import glob, os, re, sys
from fontTools.ttLib import TTFont

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FONTS = os.path.join(ROOT, "assets", "fonts")


def cmap(name):
    return set(TTFont(os.path.join(FONTS, name)).getBestCmap())


def main():
    body = cmap("NotoSerifCJKjp-Medium-subset.otf")
    seal = cmap("NotoSerifCJKjp-Black-subset.otf")
    display = cmap("Shojumaru-Regular.ttf")
    shown = set()
    for path in glob.glob(os.path.join(ROOT, "src", "**", "*.[ch]pp"), recursive=True):
        for lit in re.findall(r'"((?:[^"\\\n]|\\.)*)"', open(path, encoding="utf-8").read()):
            shown |= {ord(c) for c in lit}
    bad = []
    bad += [f"U+{c:04X} {chr(c)} is not in the body font" for c in sorted(shown) if c >= 0x20 and c not in body]
    bad += [f"U+{c:04X} {chr(c)} is not in the seal font" for c in sorted(shown) if c >= 0x3000 and c not in seal]
    bad += [f"U+{c:04X} {chr(c)} is not in the display face" for c in range(0x20, 0x7F) if c not in display]
    for b in bad:
        print("FAIL ", b)
    print(f"{len(shown)} characters in the source's strings; " + ("all good" if not bad else f"{len(bad)} missing"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
