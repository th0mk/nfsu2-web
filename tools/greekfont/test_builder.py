#!/usr/bin/env python3
"""Source-level tests; run with python test_builder.py. No game assets needed."""
from pathlib import Path
import importlib.util
import sys
import unittest
import unicodedata

p=Path(__file__).resolve().parent/'build_greek.py'
spec=importlib.util.spec_from_file_location('greek_test',p)
g=importlib.util.module_from_spec(spec);sys.modules[spec.name]=g;spec.loader.exec_module(g)

class BuilderTests(unittest.TestCase):
    def test_greek_coverage(self):
        self.assertEqual(len(g.GREEK),69)
        self.assertEqual(len(set(g.GREEK_BYTES.values())),69)
        self.assertEqual(g.decode(g.encode(g.GREEK)),g.GREEK)
    def test_ascii_unchanged(self):
        s=''.join(chr(n) for n in range(32,127))
        self.assertEqual(g.encode(s),s.encode('ascii'))
    def test_punctuation(self):
        s=''.join(g.PUNCT)+''.join(map(chr,sorted(g.PRESERVE)))
        self.assertEqual(g.decode(g.encode(s)),s)
    def test_normalization(self):
        self.assertEqual(g.encode('\u03b1\u0301'),g.encode('\u03ac'))
    def test_reject_unsupported(self):
        with self.assertRaises(ValueError):g.encode('\U0001f600')
    def test_reject_embedded_nul(self):
        with self.assertRaises(ValueError):g.encode('abc\x00def')
    def test_truncated_chunk(self):
        with self.assertRaises(ValueError):list(g.chunks(b'abc'))
    def test_oversized_chunk(self):
        with self.assertRaises(ValueError):list(g.chunks(b'\x01\x00\x00\x00\x08\x00\x00\x00'))
    def test_padding(self):
        for pos in range(256):
            self.assertEqual((pos+len(g.padding(pos)))%128,0)
    def test_supported_text_controls(self):
        s='\t%1^<0>{name}</0>\r\n'
        self.assertEqual(g.encode(s),s.encode('ascii'))

    def test_discount_percent_order(self):
        import json
        catalogue = json.loads((Path(__file__).resolve().parents[1]/'lang/greek.json').read_text(encoding='utf-8'))
        self.assertIn('10%%', catalogue['texts']['DISCOUNT_GENERIC_TITLE'])
        self.assertIn('20%%', catalogue['texts']['DISCOUNT_MEGALOW_TITLE'])
        self.assertNotIn('%%10', catalogue['texts']['DISCOUNT_GENERIC_TITLE'])
        self.assertNotIn('%%20', catalogue['texts']['DISCOUNT_MEGALOW_TITLE'])
    def test_catalogue_encodes(self):
        import json
        catalogue = json.loads((Path(__file__).resolve().parents[1]/'lang/greek.json').read_text(encoding='utf-8'))
        for key, value in catalogue['texts'].items():
            self.assertEqual(g.decode(g.encode(value)), unicodedata.normalize('NFC',value), key)

if __name__=='__main__':unittest.main(verbosity=2)
