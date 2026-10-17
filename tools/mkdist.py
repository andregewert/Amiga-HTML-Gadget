#!/usr/bin/env python3
# mkdist.py - builds the Aminet archive html_gadget.lha from the built tree
#
# Copyright (c) 2026 André Gewert <agewert@ubergeek.de>, MIT License
#
# Creates dist/html_gadget/ with icons (classic 4 colour icons, plus complete
# GlowIcons and NewIcons sets in Icons/, see mkicons.py), the Installer
# script, docs, developer files and sources, then packs it with lha.
# The common parts come from amiga-devkit/tools/distlib.py. Run "make dist".

import os, shutil, glob, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkicons import ROOT, WBDRAWER, WBTOOL, WBPROJECT  # noqa: E402  (registers the motifs)
from distlib import copy, copytree, copy_devkit, latin1_copy, package_icons, pack  # noqa: E402

NAME = 'html_gadget'
FT = os.environ.get('FT', os.path.expanduser('~/AmiLib/freetype-2.3.8'))
DIST = os.path.join(ROOT, 'dist')
PKG = os.path.join(DIST, NAME)


def main():
    b = lambda *p: os.path.join(ROOT, *p)
    shutil.rmtree(DIST, ignore_errors=True)
    os.makedirs(PKG)

    copy(b('package', NAME + '.readme'), os.path.join(PKG, NAME + '.readme'))
    copy(b('package', 'Install'), os.path.join(PKG, 'Install'))
    copy(b('LICENSE'), os.path.join(PKG, 'LICENSE'))
    copy(os.path.join(FT, 'docs', 'FTL.TXT'), os.path.join(PKG, 'Licenses', 'FTL.TXT'))
    copy(b('demo', 'fonts', 'Vera-COPYRIGHT.TXT'), os.path.join(PKG, 'Licenses', 'Vera-COPYRIGHT.TXT'))

    for g in ('html.gadget', 'htmlttf.gadget'):
        copy(b('bin', g), os.path.join(PKG, 'Classes', 'Gadgets', g))
    copy(b('bin', '68020', 'htmlttf.gadget'), os.path.join(PKG, 'Classes', 'Gadgets', '68020', 'htmlttf.gadget'))

    demo = os.path.join(PKG, 'Demo')
    copy(b('bin', 'HTMLDemo'), os.path.join(demo, 'HTMLDemo'))
    for f in glob.glob(b('demo', '*.html')) + glob.glob(b('demo', '*.gif')) + glob.glob(b('demo', '*.iff')):
        copy(f, os.path.join(demo, os.path.basename(f)))
    for f in glob.glob(b('demo', 'fonts', 'Vera*')):
        copy(f, os.path.join(demo, 'fonts', os.path.basename(f)))

    dev = os.path.join(PKG, 'Developer')
    for sub in ('gadgets', 'proto', 'inline', 'clib'):
        copytree(b('include', sub), os.path.join(dev, 'Include', sub))
    copytree(b('include', 'fd'), os.path.join(dev, 'FD'))
    copytree(b('sfd'), os.path.join(dev, 'SFD'))
    copytree(b('doc'), os.path.join(dev, 'Autodocs'))

    docs = os.path.join(PKG, 'Docs')
    latin1_copy(b('README.de.md'), os.path.join(docs, 'LiesMich.txt'))
    # the READMEs show the screenshot
    copy(b('screenshot-amiga.png'), os.path.join(docs, 'screenshot-amiga.png'))

    src = os.path.join(PKG, 'Source')
    for d in ('src', 'ttf', 'include', 'sfd', 'doc', 'package', 'tools'):
        copytree(b(d), os.path.join(src, d), ignore=('__pycache__',))
    copytree(b('demo'), os.path.join(src, 'demo'), ignore=('fonts',))
    for f in glob.glob(b('test', '*.c')):
        copy(f, os.path.join(src, 'test', os.path.basename(f)))
    for f in ('Makefile', 'README.md', 'README.de.md', 'LICENSE', 'screenshot-amiga.png'):
        copy(b(f), os.path.join(src, f))
    copy(b('icons', 'preview.png'), os.path.join(src, 'icons', 'preview.png'))
    copy_devkit(os.path.join(src, 'amiga-devkit'))

    # icons: classic ones next to the files, every style also in Icons/<Style>/
    mv = 'SYS:Utilities/MultiView'
    icons = [('Install', 'install', WBPROJECT,
              dict(default_tool='Installer', tooltypes=('APPNAME=html.gadget', 'MINUSER=AVERAGE'))),
             ('html_gadget.readme', 'readme', WBPROJECT, dict(default_tool=mv)),
             ('LICENSE', 'license', WBPROJECT, dict(default_tool=mv)),
             ('Docs/LiesMich.txt', 'readme', WBPROJECT, dict(default_tool=mv)),
             ('Demo/HTMLDemo', 'demo', WBTOOL,
              dict(stack=16384, tooltypes=('(TTF)', '(FONTSET=Vera)', '(SIZE=12)',
                                           '(FILE=PROGDIR:example.html)')))]
    for d in ('Classes', 'Demo', 'Developer', 'Docs', 'Source', 'Licenses', 'Icons',
              'Developer/Autodocs'):
        icons.append((d, 'drawer', WBDRAWER, {}))
    package_icons(PKG, NAME, icons)

    pack(DIST, NAME)


if __name__ == '__main__':
    main()
