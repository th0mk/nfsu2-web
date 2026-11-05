#!/usr/bin/env python3
"""Read back a locally generated Greek pack. Does not launch the game.

Usage: python verify_greek.py --input Languages.zip --build Greek_Build
Add --repository /path/to/nfsu2-web-main for an independent upstream texture check.
"""
from __future__ import annotations
import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import struct
import sys
import unicodedata

HERE = Path(__file__).resolve().parent

def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    if spec is None or spec.loader is None:
        raise ValueError('Could not load ' + str(path))
    m = importlib.util.module_from_spec(spec)
    sys.modules[name] = m
    spec.loader.exec_module(m)
    return m

def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--input', type=Path, required=True)
    ap.add_argument('--build', type=Path, required=True)
    ap.add_argument('--repository', type=Path)
    ap.add_argument('--report', type=Path)
    a = ap.parse_args()
    g = load('greek_builder_verify', HERE/'build_greek.py')
    src = g.read_assets(a.input)
    mapping = {'english.bin':'LANGUAGES/English.bin',
               'languagetextures.bin':'LANGUAGES/LanguageTextures.bin',
               'globala.bun':'GLOBAL/GLOBALA.BUN',
               'locale_english.loc':'memcard/LOCALE_ENGLISH.loc'}
    out = {key:(a.build/'LANGPACKS/greek'/rel).read_bytes() for key,rel in mapping.items()}
    build_report = json.loads((a.build/'greek-build-report.json').read_text(encoding='utf-8'))
    for key, rel in mapping.items():
        g.check(hashlib.sha256(out[key]).hexdigest() == build_report['generated_files'][rel]['sha256'],
                'Generated file changed: ' + rel)
    def parse(data):
        fonts, textures = {}, {}
        for key in ['english.bin','languagetextures.bin','globala.bun']:
            for cid,s,e in g.chunks(data[key]):
                if cid == 0x30201:
                    f = g.GameFont.read(data[key][s:e]); fonts[f.name] = f
                elif cid == 0xb3300000:
                    pack = g.TPK.read(data[key],s,e)
                    textures.update({t.name.lower():t for t in pack.textures})
        return fonts, textures
    old_fonts, old_tex = parse(src)
    new_fonts, new_tex = parse(out)
    g.check(set(new_fonts) == g.FONTS, 'Font set mismatch')
    checks = []
    for name,f in new_fonts.items():
        if name in g.KEEP_FONTS:
            g.check(f.records == old_fonts[name].records, name+': changed, though kept'); continue
        original,tex = old_fonts[name],new_tex[name]
        old_image,new_image = old_tex[name].rgba(),tex.rgba()
        g.check(new_image.crop((0,0,*old_image.size)).tobytes() == old_image.tobytes(),
                name+': original texture region changed')
        for code,rec in original.records.items():
            if 32 <= code < 127:
                g.check(f.records[code] == rec, name+': original ASCII changed')
        for ch in g.GREEK:
            for code in [ord(ch), g.GREEK_BYTES[ch]]:
                g.check(code in f.records, name+': missing Greek glyph')
                _c,w,h,x,y,*rest = g.GLYPH.unpack(f.records[code])
                g.check(x+w <= new_image.width and y+h <= new_image.height, 'Glyph exceeds image')
                g.check(new_image.getchannel('A').crop((x,y,x+w,y+h)).getbbox() is not None, 'Empty glyph')
        checks.append({'font':name,'greek_characters':len(g.GREEK),'ascii_records_unchanged':True,
                       'original_texture_region_unchanged':True})
    catalogue = json.loads((HERE.parent/'lang/greek.json').read_text(encoding='utf-8'))
    def text_records(data):
        parts = [(s,e) for cid,s,e in g.chunks(data) if cid==0x39000]
        g.check(len(parts)==1,'Expected one text chunk')
        s,e=parts[0]; return g.read_text_chunk(data[s:e])[2]
    originals,translated=text_records(src['english.bin']),text_records(out['english.bin'])
    translated_hashes=set()
    for label,value in catalogue['texts'].items():
        key=int(label,16) if label.startswith('0x') else g.label_hash(label)
        translated_hashes.add(key)
        g.check(g.decode(translated[key]) == unicodedata.normalize('NFC',value), 'Text mismatch '+label)
    extra=originals.keys()-translated_hashes
    for key in extra:g.check(translated[key]==originals[key],'Unmatched original record changed')
    save_source=build_report['save_text_source']
    expected=g.read_loc(src[save_source]) if save_source in src else catalogue['memcard']
    actual=[g.decode(s.encode('latin1')) for s in g.read_loc(out['locale_english.loc'])]
    g.check(actual==expected,'Save locale mismatch')
    report={'result':'PASS - OFFLINE CHECKS ONLY','fonts':checks,'translated_records':len(translated_hashes),
            'additional_original_records_preserved':len(extra),'save_strings_checked':len(expected),
            'in_game_tested':False,'independent_upstream_texture_check':None}
    if a.repository:
        upstream=load('upstream_texpacks_verify',a.repository/'tools/texpacks.py')
        parsed={}
        for key in ['languagetextures.bin','globala.bun']:
            before={r[0]:r for r in upstream.textures(src[key])}
            after={r[0]:r for r in upstream.textures(out[key])}
            g.check(set(before)==set(after),'Texture hash set changed')
            modified={g.u32(t.info,36) for name,t in new_tex.items() if name in g.FONTS}
            unchanged=0
            for nh,row in after.items():
                g.check(row[4] is not None,'Upstream parser could not read texture')
                if nh not in modified:
                    g.check(row==before[nh],'Non-font texture changed')
                    unchanged+=1
            parsed[key]={'readable_textures':len(after),'non_font_textures_unchanged':unchanged}
        report['independent_upstream_texture_check']=parsed
    text=json.dumps(report,indent=2)
    if a.report:a.report.write_text(text+'\n',encoding='utf-8')
    print(text)
    return 0

if __name__=='__main__':
    try:raise SystemExit(main())
    except (OSError,ValueError,KeyError,struct.error) as e:
        print('ERROR:',e,file=sys.stderr);raise SystemExit(1)
