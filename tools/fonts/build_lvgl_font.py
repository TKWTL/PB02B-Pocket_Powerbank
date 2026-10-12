#!/usr/bin/env python3
"""Create a native 1bpp LVGL 9 font from a Fusion Pixel Font BDF.

The BDF glyph bitmap is copied pixel-for-pixel (no rasterization, scaling or AA).
Requires only Python 3.9+ standard library.
"""
from __future__ import annotations
import argparse
import gzip
import re
import sys
from pathlib import Path

DEFAULT_NAME = "lv_font_menucn_12"
ASCII = ''.join(map(chr, range(32, 127)))
EXTRA = '°？'


def required_chars(source: Path | None, charset: Path | None) -> tuple[str, int]:
    if source is not None:
        code = source.read_text(encoding='utf-8-sig')
        match = re.search(r'static\s+const\s+menu_tr_t\s+menu_tr_table\s*\[\s*\]\s*=\s*\{(.*?)\n\s*\};', code, re.S)
        if match is None:
            raise ValueError(f'cannot locate menu_tr_table[] in {source}')
        translations = re.findall(r'\{\s*"(?:\\.|[^"\\])*"\s*,\s*"((?:\\.|[^"\\])*)"\s*\}', match.group(1))
        if not translations:
            raise ValueError('menu_tr_table[] has no translation entries')
        cjk = ''.join(sorted({c for s in translations for c in s if '\u3400' <= c <= '\u9fff'}))
        non_ascii = ''.join(sorted({c for s in translations for c in s if ord(c)>127 and not ('\u3400'<=c<='\u9fff')}))
        return ''.join(sorted(set(ASCII + EXTRA + cjk + non_ascii))), len(translations)
    if charset is None:
        raise ValueError('either --source or --charset is required')
    characters = charset.read_text(encoding='utf-8-sig').strip('\r\n')
    return ''.join(sorted(set(ASCII + EXTRA + characters))), 0


def open_bdf(p: Path):
    return gzip.open(p,'rt',encoding='latin1') if p.suffix.lower()=='.gz' else p.open('rt',encoding='latin1')


def parse_bdf(path: Path, required: str):
    needed = {ord(c) for c in required}
    glyphs: dict[int, dict] = {}
    font_ascent = None
    font_descent = None
    global_bbox = None
    current = None
    bitmap = False
    with open_bdf(path) as fd:
        for raw in fd:
            line = raw.strip()
            if line.startswith('FONT_ASCENT '):
                font_ascent = int(line.split()[1])
            elif line.startswith('FONT_DESCENT '):
                font_descent = int(line.split()[1])
            elif line.startswith('FONTBOUNDINGBOX '):
                global_bbox = list(map(int,line.split()[1:5]))
            elif line == 'STARTCHAR' or line.startswith('STARTCHAR '):
                current = {'rows': []}
                bitmap = False
            elif line == 'ENDCHAR':
                if current and current.get('cp') in needed:
                    glyphs[current['cp']] = current
                current = None
                bitmap = False
            elif current is not None:
                if line == 'BITMAP':
                    bitmap = True
                elif bitmap:
                    current['rows'].append(line)
                elif line.startswith('ENCODING '):
                    current['cp'] = int(line.split()[1])
                elif line.startswith('DWIDTH '):
                    current['dwidth'] = int(line.split()[1])
                elif line.startswith('BBX '):
                    current['bbx'] = list(map(int, line.split()[1:5]))
    if font_ascent is None or font_descent is None:
        if global_bbox is None:
            raise ValueError('BDF missing FONT_ASCENT/DESCENT and FONTBOUNDINGBOX')
        _, h, _, y = global_bbox
        font_descent = max(0,-y)
        font_ascent = h - font_descent
    missing = [c for c in required if ord(c) not in glyphs]
    if missing:
        raise ValueError('Missing glyphs from chosen BDF ('+str(len(missing))+'): '+''.join(missing))
    return glyphs, font_ascent, font_descent


def pack_glyph(g: dict) -> bytes:
    w,h,x,y = g['bbx']
    if not (0 <= w <= 255 and 0 <= h <= 255 and -128 <= x <= 127 and -128 <= y <= 127):
        raise ValueError(f'Invalid BBX {g["bbx"]} for U+{g["cp"]:04X}')
    if len(g['rows']) != h:
        raise ValueError(f'BITMAP row count mismatch: U+{g["cp"]:04X} h={h}, rows={len(g["rows"])}')
    bits = []
    padded_width = ((w + 7) // 8) * 8
    for line in g['rows']:
        v = int(line,16) if line else 0
        for x_idx in range(w):
            bits.append((v >> (padded_width - x_idx - 1)) & 1)
    b = bytearray((len(bits) + 7) // 8)
    for i, val in enumerate(bits):
        if val: b[i//8] |= 0x80 >> (i%8)
    return bytes(b)


def emit_c(characters: str, glyphs: dict, ascent: int, descent: int, name: str, metrics: str):
    cp_sorted = sorted(map(ord, characters))
    data = bytearray()
    descriptions = []
    for cp in cp_sorted:
        g = glyphs[cp]
        w,h,x,y = g['bbx']
        offset = len(data)
        b = pack_glyph(g)
        data.extend(b)
        adv = g['dwidth'] * 16
        if adv >= 4096 or offset >= 1048576:
            raise ValueError('glyph exceeds LV_FONT_FMT_TXT_LARGE=0 limits')
        descriptions.append((cp,offset,adv,w,h,x,y))
    # Compact vertical metrics use the occupied pixel region, with original
    # BDF glyph rows and relative-baseline offsets left completely unchanged.
    # This font has 11-pixel CJK ink and a 12-pixel tight line box.
    if metrics == "compact":
        ascent = max((g["bbx"][1] + g["bbx"][3] for g in glyphs.values()), default=0)
        descent = max((-g["bbx"][3] for g in glyphs.values()), default=0)
        ascent = max(ascent, 1)
        descent = max(descent, 0)
    first = 32
    ascii_cps = list(range(32,127))
    assert cp_sorted[:len(ascii_cps)] == ascii_cps
    others = cp_sorted[len(ascii_cps):]
    if not others:
        raise ValueError('expected at least one non-ASCII character')
    if others[-1]-others[0]>65535:
        raise ValueError('sparse cmap unicode deltas exceed uint16_t')
    lines = [
        '/* AUTO-GENERATED. DO NOT EDIT.',
        ' * Source: Fusion Pixel Font 12px Proportional zh-Hans BDF (SIL OFL 1.1).',
        ' * Native BDF pixels -> 1bpp; no anti-alias, no resize, no compression.',
        f' * Line box: {ascent+descent}px; baseline descent: {descent}px ({metrics} metrics).',
        ' * Keep upstream font license/attribution when redistributing this derivative.',
        ' */',
        '#include "lvgl.h"',
        '',
        'static const uint8_t glyph_bitmap[] = {',
    ]
    for i in range(0,len(data),16):
        lines.append('    '+', '.join(f'0x{x:02X}' for x in data[i:i+16])+',')
    lines.extend(['};','', 'static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {',
                  '    {0},  /* reserved */'])
    for cp,off,adv,w,h,x,y in descriptions:
        lines.append(f'    {{.bitmap_index={off}, .adv_w={adv}, .box_w={w}, .box_h={h}, .ofs_x={x}, .ofs_y={y}}}, /* U+{cp:04X} */')
    lines += ['};','','static const uint16_t unicode_list_cn[] = {']
    deltas = [cp-others[0] for cp in others]
    for i in range(0,len(deltas),12):
        lines.append('    '+', '.join(str(v) for v in deltas[i:i+12])+',')
    lines += ['};','', 'static const lv_font_fmt_txt_cmap_t cmaps[] = {',
              '    {.range_start=32, .range_length=95, .glyph_id_start=1,',
              '     .unicode_list=NULL, .glyph_id_ofs_list=NULL, .list_length=0,',
              '     .type=LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY},',
             f'    {{.range_start={others[0]}, .range_length={others[-1]-others[0]+1},',
              '     .glyph_id_start=96, .unicode_list=unicode_list_cn,',
             f'     .glyph_id_ofs_list=NULL, .list_length={len(others)},',
              '     .type=LV_FONT_FMT_TXT_CMAP_SPARSE_TINY},',
              '};','',
              'static const lv_font_fmt_txt_dsc_t font_dsc = {',
              '    .glyph_bitmap=glyph_bitmap, .glyph_dsc=glyph_dsc,',
              '    .cmaps=cmaps, .kern_dsc=NULL, .kern_scale=0,',
              '    .cmap_num=2, .bpp=1, .kern_classes=0,',
              '    .bitmap_format=LV_FONT_FMT_TXT_PLAIN, .stride=0,',
              '};','',
              f'const lv_font_t {name} = {{',
              '    .get_glyph_dsc=lv_font_get_glyph_dsc_fmt_txt,',
              '    .get_glyph_bitmap=lv_font_get_bitmap_fmt_txt,',
              f'    .line_height={ascent+descent},',
              f'    .base_line={descent},',
              '    .subpx=LV_FONT_SUBPX_NONE,',
              '    .underline_position=-2, .underline_thickness=1,',
              '    .static_bitmap=0, .dsc=&font_dsc,',
              '    .fallback=NULL, .user_data=NULL,',
              '};','']
    return '\n'.join(lines), len(data), len(cp_sorted), max(glyphs[cp]['bbx'][1] for cp in cp_sorted)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--bdf',required=True,type=Path,help='Fusion Pixel 12px Proportional zh-Hans .bdf or .bdf.gz')
    group=p.add_mutually_exclusive_group()
    group.add_argument('--source',type=Path,help='translation source; defaults to firmware/Applications/menu/menu_pages.c')
    group.add_argument('--charset',type=Path,help='pre-extracted required characters')
    p.add_argument('--output',type=Path,default=Path('lv_font_menucn_12.c'))
    p.add_argument('--name',default=DEFAULT_NAME)
    p.add_argument('--metrics', choices=['compact','native'], default='compact',
                   help='compact: 12px line box for current subset; native: 16px BDF line box')
    p.add_argument('--charset-output', type=Path, help='optional extracted subset snapshot text output')
    args=p.parse_args()
    source = None if args.charset else (args.source or Path('firmware/Applications/menu/menu_pages.c'))
    chars, entries = required_chars(source, args.charset)
    glyphs,asc,desc=parse_bdf(args.bdf,chars)
    code,nbytes,nglyphs,inkh=emit_c(chars,glyphs,asc,desc,args.name,args.metrics)
    if args.charset_output:
        args.charset_output.parent.mkdir(parents=True,exist_ok=True)
        args.charset_output.write_text(chars,encoding='utf-8')
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(code,encoding='utf-8')
    n_hanzi=sum(1 for ch in chars if '\u3400' <= ch <= '\u9fff')
    print(f'OK: {args.output}; translations={entries if source else "snapshot"}; Hanzi={n_hanzi}; glyphs={nglyphs}; bpp=1')
    m=re.search(r'\.line_height=(\d+),\s*\n\s*\.base_line=(\d+)',code)
    line_height,base_line = map(int,m.groups())
    print(f'    bitmap={nbytes} bytes; line_height={line_height}, base_line={base_line} ({args.metrics}); max ink height={inkh}px')
    print(f'    C source={args.output.stat().st_size} bytes (not flash occupancy)')

if __name__=='__main__':
    try: main()
    except (OSError,ValueError,KeyError,AssertionError) as exc:
        sys.exit(f'ERROR: {exc}')
