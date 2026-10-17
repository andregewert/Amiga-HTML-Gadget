#!/usr/bin/env python3
# mkicons.py - Workbench icons of the html.gadget distribution
#
# Copyright (c) 2026 André Gewert <agewert@ubergeek.de>, MIT License
#
# The icon writer (classic 4 colour icons, GlowIcons, NewIcons) and the
# motifs drawer, readme, license, install and tiles come from
# amiga-devkit/tools/amigaicons.py (git submodule). This file adds the
# motif of HTMLDemo and registers it there.
#
# Run directly (or "make icons") to write sample icons of all styles to
# icons/<Style>/ and a preview picture icons/preview.png.

import os, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'amiga-devkit', 'tools'))
import amigaicons as ai  # noqa: E402

from amigaicons import STYLES, WBDRAWER, WBTOOL, WBPROJECT, write_icon  # noqa: E402,F401

WHITE, TEXT, BLUE, GREY, PAPER, RED = ai.WHITE, ai.TEXT, ai.BLUE, ai.GREY, ai.PAPER, ai.RED


def pic_demo():
    c = ai.Canvas(40, 24)
    c.rect(1, 1, 38, 22, 2)
    c.frame(1, 1, 38, 22, 1, 1)
    c.rect(2, 2, 37, 5, 3)
    c.rect(4, 8, 20, 9, 1)                 # heading
    for y in (12, 15, 18):
        c.rect(4, y, 33 if y != 18 else 24, y, 1)
    c.rect(26, 8, 33, 9, 3)                # "link"
    return c


def m_demo(s, c):
    c.rect(2, 4, 39, 34, s.line(GREY[1]))
    c.rect(3, 5, 38, 8, s.ramp(*BLUE)(3, 5, 38, 8))
    c.rect(4, 6, 6, 7, WHITE)
    c.rect(3, 9, 38, 33, s.ramp(WHITE, PAPER[1])(3, 9, 38, 33))
    c.rect(6, 12, 19, 13, s.line(GREY[1]))
    for y, x1 in ((17, 22), (20, 20), (23, 22)):
        c.line(6, y, x1, y, TEXT)
    c.line(6, 27, 15, 27, BLUE[1])
    c.line(6, 28, 15, 28, BLUE[1])

    def boing(x, y):
        return RED[1] if ((x - 25) // 3 + (y - 16) // 3) % 2 else WHITE
    c.disc(31, 22, 6, boing, s.line(RED[1]))


def n_demo(c, sel):
    """ selected: the ball has bounced on """
    def boing(c, box, u0, u1, t0, t1, sel):
        u = (u0 + u1) // 2 + (2 if sel else -2)          # radius 3: stays on the glass
        cx, cy = box.x0 + u, box.ymid(u) + (t0 + t1) // 2 + (2 if sel else 0)
        c.disc(cx, cy, 3, lambda x, y: ai.N_RED if ((x - cx + 9) // 2 + (y - cy + 9) // 2) % 2 else WHITE)
    ai.n_monitor(c, boing, sel)


ai.register('demo', pic_demo, m_demo, n_demo)


ROLES = ('drawer', 'readme', 'license', 'demo', 'install', 'tiles')

SAMPLES = (('Drawer', 'drawer', WBDRAWER, {}),
           ('ReadMe', 'readme', WBPROJECT, {'default_tool': 'SYS:Utilities/MultiView'}),
           ('License', 'license', WBPROJECT, {'default_tool': 'SYS:Utilities/MultiView'}),
           ('HTMLDemo', 'demo', WBTOOL, {'stack': 16384}),
           ('Install', 'install', WBPROJECT, {'default_tool': 'Installer'}),
           ('Script', 'tiles', WBPROJECT, {'default_tool': 'C:IconX'}))


if __name__ == '__main__':
    ai.write_samples(os.path.join(ROOT, 'icons'), SAMPLES, ROLES)
