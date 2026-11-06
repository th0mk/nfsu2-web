#!/usr/bin/env python3
"""Source-level tests; run with python test_builder.py. No game assets needed."""
from pathlib import Path
import json
import re
import sys
import unicodedata
import unittest

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(HERE))
import build_lithuanian as lt                                   # noqa: E402

TOKENS = re.compile(r'\$[A-Z0-9_]+\$|%[-0-9.$]*[sdifu]')


class BuilderTests(unittest.TestCase):
    def test_letters(self):
        self.assertEqual(len(lt.LETTERS), 18)
        self.assertEqual(sorted(lt.LT_BYTES.values()), list(range(0x8e, 0xa0)))
        self.assertEqual(lt.decode(lt.encode(lt.LETTERS)), lt.LETTERS)
        for ch in lt.LETTERS:
            base, _mark = lt.MARKS[ch]
            self.assertEqual(unicodedata.normalize('NFD', ch)[0], base)

    def test_ascii_and_latin1_unchanged(self):
        s = ''.join(chr(n) for n in range(32, 127)) + ''.join(chr(n) for n in range(0xa0, 0x100))
        self.assertEqual(lt.encode(s), s.encode('latin1'))

    def test_bytes_do_not_collide(self):
        self.assertEqual(len(set(lt.CODEC.values())), len(lt.CODEC))
        # the game's own punctuation bytes stay where its character table has them
        self.assertEqual({lt.PUNCT[c] for c in '’™–”…“„'},
                         {0x81, 0x82, 0x83, 0x84, 0x86, 0x87, 0x88})

    def test_decomposed_input(self):
        self.assertEqual(lt.encode(unicodedata.normalize('NFD', 'Žiedas')), lt.encode('Žiedas'))

    def test_unsupported_rejected(self):
        for s in ('ł', 'ő', 'Ω', '€'):
            with self.assertRaises(ValueError): lt.encode(s)

    def test_runtime_table_matches(self):
        """runtime/language.c's LT, the port's own texts in the game, puts the letters where the pack's fonts have them"""
        src = (ROOT / 'runtime' / 'language.c').read_text(encoding='utf-8')
        table = re.search(r'LT\[\] = \{(.*?)\n    \}', src, re.S).group(1)
        pairs = {int(u, 16): int(b, 16) for u, b in re.findall(r'\{ 0x([0-9a-f]+), 0x([0-9a-f]+) \}', table)}
        self.assertEqual(pairs, {ord(ch): b for ch, b in lt.LT_BYTES.items()})

    def test_catalogue(self):
        cat = json.loads((ROOT / 'tools' / 'lang' / 'lithuanian.json').read_text(encoding='utf-8'))
        greek = json.loads((ROOT / 'tools' / 'lang' / 'greek.json').read_text(encoding='utf-8'))
        self.assertEqual(set(cat['texts']), set(greek['texts']))
        self.assertEqual(len(cat['memcard']), 14)
        for k, v in cat['texts'].items():
            self.assertEqual(lt.decode(lt.encode(v)), unicodedata.normalize('NFC', v), k)
        for s in cat['memcard']: lt.encode(s)

    def test_catalogue_keeps_the_games_codes(self):
        """the controller markers and formats the game fills in, as Greek's text keeps them"""
        cat = json.loads((ROOT / 'tools' / 'lang' / 'lithuanian.json').read_text(encoding='utf-8'))['texts']
        greek = json.loads((ROOT / 'tools' / 'lang' / 'greek.json').read_text(encoding='utf-8'))['texts']
        for k, v in cat.items():
            self.assertEqual(sorted(TOKENS.findall(v)), sorted(TOKENS.findall(greek[k])), k)
            self.assertEqual([t for t in TOKENS.findall(v) if t[0] == '%'], [t for t in TOKENS.findall(greek[k]) if t[0] == '%'], k)


if __name__ == '__main__':
    unittest.main()
