#!/usr/bin/env python3
# mkdist.py - builds the Aminet archive html_gadget.lha from the built tree
#
# Copyright (c) 2026 Andre Gewert <agewert@ubergeek.de>, MIT License
#
# Creates dist/html_gadget/ with icons (classic 4 colour icons, plus complete
# GlowIcons and NewIcons sets in Icons/, see mkicons.py), the Installer
# script, docs, developer files and sources, then packs it with lha.
# Run "make dist".

import os, shutil, subprocess, glob, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FT = os.environ.get('FT', os.path.expanduser('~/AmiLib/freetype-2.3.8'))
DIST = os.path.join(ROOT, 'dist')
PKG = os.path.join(DIST, 'html_gadget')

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkicons import STYLES, WBDRAWER, WBTOOL, WBPROJECT, write_icon  # noqa: E402

# ------------------------------------------------------------- content ---


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(src, dst)


def copytree(src, dst, ignore=()):
    shutil.copytree(src, dst, ignore=shutil.ignore_patterns(*ignore), dirs_exist_ok=True)


def to_latin1_text(md):
    table = {'„': '"', '“': '"', '”': '"', '–': '-', '—': '-',
             '…': '...', '→': '->', '≤': '<=', '≥': '>=', '’': "'"}
    return ''.join(table.get(ch, ch) for ch in md).encode('latin-1', 'replace')


def main():
    b = lambda *p: os.path.join(ROOT, *p)
    shutil.rmtree(DIST, ignore_errors=True)
    os.makedirs(PKG)

    copy(b('package', 'html_gadget.readme'), os.path.join(PKG, 'html_gadget.readme'))
    copy(b('package', 'Install'), os.path.join(PKG, 'Install'))
    copy(b('LICENSE'), os.path.join(PKG, 'LICENSE'))
    copy(os.path.join(FT, 'docs', 'FTL.TXT'), os.path.join(PKG, 'Licenses', 'FTL.TXT'))
    copy(b('demo', 'fonts', 'Vera-COPYRIGHT.TXT'), os.path.join(PKG, 'Licenses', 'Vera-COPYRIGHT.TXT'))

    for g in ('html.gadget', 'htmlttf.gadget'):
        copy(b('bin', g), os.path.join(PKG, 'Classes', 'Gadgets', g))

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
    os.makedirs(docs)
    with open(b('README.de.md'), encoding='utf-8') as f:
        text = to_latin1_text(f.read())
    with open(os.path.join(docs, 'LiesMich.txt'), 'wb') as f:
        f.write(text)

    src = os.path.join(PKG, 'Source')
    for d in ('src', 'ttf', 'include', 'sfd', 'doc', 'package', 'tools'):
        copytree(b(d), os.path.join(src, d), ignore=('__pycache__',))
    copytree(b('demo'), os.path.join(src, 'demo'), ignore=('fonts',))
    for f in glob.glob(b('test', '*.c')):
        copy(f, os.path.join(src, 'test', os.path.basename(f)))
    for f in ('Makefile', 'README.md', 'README.de.md', 'LICENSE'):
        copy(b(f), os.path.join(src, f))

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
    write_icon(PKG, 'Classic', 'drawer', WBDRAWER)
    for name, role, kind, kw in icons:
        write_icon(os.path.join(PKG, name), 'Classic', role, kind, **kw)
    for style in STYLES:
        sets = os.path.join(PKG, 'Icons', style)
        write_icon(os.path.join(sets, 'html_gadget'), style, 'drawer', WBDRAWER)
        for name, role, kind, kw in icons:
            write_icon(os.path.join(sets, name), style, role, kind, **kw)
        script = os.path.join(PKG, 'Icons', 'Use' + style)
        with open(script, 'w', encoding='latin-1', newline='\n') as f:
            f.write('; gives the files of html_gadget the %s icons\n'
                    '; (double click, IconX runs it in this drawer)\n'
                    'Copy %s/~(html_gadget.info) / ALL CLONE QUIET\n'
                    'Copy %s/html_gadget.info // CLONE QUIET\n'
                    'Echo "%s icons copied. Close and reopen the drawers to see them."\n'
                    % (style, style, style, style))
        write_icon(script, style, 'tiles', WBPROJECT, default_tool='C:IconX')

    # archive
    arc = os.path.join(DIST, 'html_gadget.lha')
    files = ['html_gadget.info']
    for dirpath, dirnames, filenames in os.walk(PKG):
        dirnames.sort()
        for fn in sorted(filenames):
            files.append(os.path.relpath(os.path.join(dirpath, fn), DIST))
    # an LhA that can create archives: jlha (Debian: jlha-utils) or lha for UNIX
    tool = shutil.which('jlha') or 'lha'
    subprocess.run([tool, 'ao5q', arc] + files, cwd=DIST, check=True)
    shutil.copy2(os.path.join(PKG, 'html_gadget.readme'), os.path.join(DIST, 'html_gadget.readme'))
    print('created', arc, os.path.getsize(arc), 'bytes')


if __name__ == '__main__':
    main()
