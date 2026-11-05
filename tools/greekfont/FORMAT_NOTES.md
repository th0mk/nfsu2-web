# Implementation notes

## Scope

Targets the North American PC game's `LANGUAGES/English.bin`, `LANGUAGES/LanguageTextures.bin`, `GLOBAL/GLOBALA.BUN` and `memcard/LOCALE_ENGLISH.loc`. The builder rejects unfamiliar font sets/layouts. It does not patch the executable or redistribute font files.

## Character mapping

`greek-character-map.json` (written next to the pack) describes the mapping `build_greek.py` uses; `runtime/language.c` (`greek_byte`) follows the same rule. There are 69 monotonic Greek characters. They occupy extended byte values from 0xA1 upward, excluding retained characters used by the draft. ASCII remains unchanged. NBSP, copyright/registered marks, guillemets, middle dot, one-quarter and the existing capital N with tilde are preserved. Selected typographic punctuation uses the existing low extended byte conversions; an unused entry maps em dash.

Both byte-slot and true Unicode aliases are included in each font. Greek byte slots are checked not to collide with nonzero entries of the English conversion table. Text is normalized to NFC and unsupported characters are rejected, rather than replaced silently. Some non-Greek accented Latin slots are repurposed. This is not general Unicode support or support for polytonic Greek.

The runtime's existing general punctuation normalization is retained for dynamic strings. Static translated catalogue strings use the builder's punctuation slots. Original controller/control markers are not translated by the builder.

## Font and texture data

Top-level font chunk: 0x00030201. Language text chunk: 0x00039000.

In the observed font chunks, two 256-byte filename fields precede an FNTF structure. The header is 128 bytes, followed by sorted 16-byte glyph records and existing kerning/surface data. The record layout used is `<HBBHHBbbBHh`: code, width, height, x, y, page, x offset, y offset, kerning count, kerning index, advance. Layout and sorting are checked before modification.

Added raster data is placed below the original atlas. Width is retained; height grows to a power of two. Original ASCII records and original image pixels are verified unchanged. Both legacy-slot and Unicode glyph records point to the appended raster regions. Sampling scales, count and offsets are adjusted. Old kerning data is retained; new Greek kerning pairs are not generated. Mixed Greek/Latin spacing and old pairs involving repurposed extended characters need real-game testing.

FNTF versions 0x016E and 0x019E have different meanings at header offset 0x30. Only the latter's surface descriptor offset is relocated. Its dimension/size fields are adjusted. The exported-size field update is inferred from the supplied files and remains subject to game validation.

The seven definitions are `font_arial`, `conduitmditc_tt14i`, `lcd_let48`, `font_conduitmditctt38bi`, `font_arial12`, `font_impact36`, and `conduitmditc_tt21i`.

The builder reads JDLZ textures and keeps each texture's format: DXT3 stays DXT3 (the original 4x4 blocks kept byte for byte, only the added rows encoded), ARGB stays ARGB; one level, no mipchain. `lcd_let48` (the dashboard's digits) is left as it is. Fonts are drawn from Fira Sans: Regular for Arial, Italic for the italic Conduit fonts, Bold Italic for the bold italic one, Bold for Impact.

`LanguageTextures.bin` must stay a compressed pack (a 0x33310003 table of blobs): with an uncompressed one the game draws nothing in those fonts. Unchanged textures keep their JDLZ blobs and offsets; the changed ones are stored (RAWW, as the Polish translation's pack has all of them) at offsets past the others' end. JDLZ of our own, though it reads back here, crashed the game now and then. `GLOBALA.BUN` (already uncompressed) is written uncompressed.

## Text and memory-card data

The English text hash table is read and rebuilt in sorted order. Duplicate text values share storage. The translation supplies 3,918 records; all seven extra original records remain present. Five are empty, and two are special byte strings whose semantics were not established. Their preservation is not a claim that every possible on-screen string has been translated.

The existing Greek locale is read as UTF-16, then converted to the same legacy glyph mapping and stored as the wide-character slot values expected by this translation route. The default takes all 14 records from the provided `LOCALE_GREEK.loc`. An explicit switch can use the draft JSON's save strings instead. This follows the byte-slot approach already used by the repository's Turkish-language generator; it still requires a save/load dialog playtest.

## Runtime

Greek plays as English, from `LANGPACKS/greek` (`tools/langpacks.py --el`). UTF-8 texts of the port's own are converted with the same Greek map (`language_game_letters`). With the pack, the language pool gets 64 KiB more through the font-size table, after checking its stock first entry.

## Sources used

- Reverse-engineering reference: https://yugecin.github.io/nfsu2-re/docs.html
- Reference parser structures: https://github.com/yugecin/nfsu2-re/blob/master/nfsu2-re-binfiles/main.c
- NFS custom font tooling documentation: https://github.com/xan1242/NFS_CustomFont/tree/master/Tools
