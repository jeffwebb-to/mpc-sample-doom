#!/usr/bin/env python3
"""Write docs/logo.svg: the title in chunky pixel letters, one SVG rect per lit cell."""
import os

GLYPHS = {
    "M": ["X...X", "XX.XX", "X.X.X", "X...X", "X...X", "X...X", "X...X"],
    "P": ["XXXX.", "X...X", "X...X", "XXXX.", "X....", "X....", "X...."],
    "C": [".XXX.", "X...X", "X....", "X....", "X....", "X...X", ".XXX."],
    "D": ["XXXX.", "X...X", "X...X", "X...X", "X...X", "X...X", "XXXX."],
    "O": [".XXX.", "X...X", "X...X", "X...X", "X...X", "X...X", ".XXX."],
    " ": ["..", "..", "..", "..", "..", "..", ".."],
}
TEXT = "MPC DOOM"
CELL = 22          # pixel size
GAP = 1            # cells between letters
MARGIN = 1         # cells around the whole word
FILL = "#d41c1c"
SHADOW = "#5a0a0a"

cols = 0
for ch in TEXT:
    cols += len(GLYPHS[ch][0]) + GAP
cols -= GAP
rows = 7
W = (cols + 2 * MARGIN) * CELL
H = (rows + 2 * MARGIN) * CELL

rects = []
x = MARGIN
for ch in TEXT:
    g = GLYPHS[ch]
    for r, line in enumerate(g):
        for c, bit in enumerate(line):
            if bit == "X":
                rects.append((x + c, MARGIN + r))
    x += len(g[0]) + GAP

out = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "logo.svg")
with open(out, "w") as f:
    f.write('<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 %d %d" width="%d" height="%d" role="img" aria-label="%s">\n' % (W, H, W, H, TEXT))
    f.write("<title>%s</title>\n" % TEXT)
    f.write('<g fill="%s">\n' % SHADOW)
    for cx, cy in rects:
        f.write('<rect x="%d" y="%d" width="%d" height="%d"/>\n' % (cx * CELL + 4, cy * CELL + 4, CELL, CELL))
    f.write("</g>\n")
    f.write('<g fill="%s">\n' % FILL)
    for cx, cy in rects:
        f.write('<rect x="%d" y="%d" width="%d" height="%d"/>\n' % (cx * CELL, cy * CELL, CELL, CELL))
    f.write("</g>\n</svg>\n")
print("wrote", os.path.normpath(out), W, "x", H, len(rects), "cells")
