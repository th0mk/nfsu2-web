# Lithuanian font pack builder

This tool builds the Lithuanian NFSU2 Web language pack from the game's own files (`LANGUAGES/English.bin`,
`LANGUAGES/LanguageTextures.bin`, `GLOBAL/GLOBALA.BUN` and `memcard/LOCALE_ENGLISH.loc`) and `tools/lang/lithuanian.json`'s
texts. Game files, fonts and built packs are not stored in this repository.

The Lithuanian page catalogue is `web/src/app/lang/lithuanian.json`; the in-game catalogue is `tools/lang/lithuanian.json`.

## The letters

The game's fonts have no Lithuanian letters, and only one (`conduitmditc_tt21i`) has Š, š, Ž and ž. Each font gets
Ą Č Ę Ė Į Š Ų Ū Ž ą č ę ė į š ų ū ž put together from its own letters and marks, so Lithuanian words keep the font's
shapes:

| Mark | Taken from, in this order |
| --- | --- |
| caron (č š ž) | the font's ˇ; its â's (Â's) circumflex turned over; the TTF's ˇ |
| dot (ė) | the font's ˙; the dot of its i |
| macron (ū) | the font's ˉ; its ¯; the TTF's ˉ |
| ogonek (ą ę į ų) | the font's ˛; the TTF's ˛ |

A mark above goes where the font's â has its circumflex (in the italic fonts, that follows the slant); an ogonek under
the letter's right foot. Only `font_arial12` and `font_impact36`, which have nothing but ASCII, take marks from the
TTFs: Windows Arial and Impact, the faces those fonts are. „ “ ” ’ … – — are made from the font's comma, period and
hyphen; ™ is drawn from the TTF where the font has none.

The text writes the letters as bytes 0x8E-0x9F (`LT_BYTES`), which the game's character table had free; the builder
maps them to the letters' Unicode there, and each font has a glyph at both the byte and the Unicode. Latin-1 (0xA0-0xFF)
and the game's own punctuation bytes stay as they are. The memory-card texts (`LOCALE_ENGLISH.loc`) are written in
Unicode, as the game's own Polish and Czech ones are: its save screens read 0x80-0x9F as Windows-1252. `runtime/language.c` (`LT`) writes the port's own texts with the
same bytes, and gives the language pool the 64 KB Greek's pack gets (Lithuanian's fonts are no bigger than Greek's).

The font and texture formats are `tools/greekfont`'s (`FORMAT_NOTES.md`): this builder uses its readers and writers and
`GameFont.add_glyphs`.

## Build a Lithuanian pack

Install the Python dependencies in `requirements.txt`, then run (Windows fonts, e.g. `/mnt/c/Windows/Fonts` in WSL):

```sh
python tools/lithuanianfont/build_lithuanian.py --input "/path/to/Need for Speed Underground 2" \
  --font arial.ttf --bold-font arialbd.ttf --italic-font ariali.ttf --bold-italic-font arialbi.ttf \
  --impact-font impact.ttf --output /tmp/nfsu2-lithuanian
python tools/langpacks.py "/path/to/Need for Speed Underground 2" --lt /tmp/nfsu2-lithuanian/LANGPACKS/lithuanian
```

The output is `LANGPACKS/lithuanian` (about 3.5 MB), a build report, and `preview-<font>.png`: a sample line drawn
from each built font's own records, to look at before playing. The builder never overwrites its input.
`python tools/lithuanianfont/test_builder.py` runs its tests (no game files needed).
