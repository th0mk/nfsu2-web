# Experimental implementation notes

## Scope

Targets the five supplied PC asset files and the uploaded `nfsu2-web-main` source snapshot. Structural checks are not a substitute for running the game. The builder rejects unfamiliar font sets/layouts. It does not patch a PC executable or redistribute font files.

## Character mapping

`greek-character-map.json` describes the same mapping used by `build_greek.py` and the modified `runtime/language.c`. There are 69 monotonic Greek characters. They occupy extended byte values from 0xA1 upward, excluding retained characters used by the draft. ASCII remains unchanged. NBSP, copyright/registered marks, guillemets, middle dot, one-quarter and the existing capital N with tilde are preserved. Selected typographic punctuation uses the existing low extended byte conversions; an unused entry maps em dash.

Both byte-slot and true Unicode aliases are included in each font. Greek byte slots are checked not to collide with nonzero entries of the English conversion table. Text is normalized to NFC and unsupported characters are rejected, rather than replaced silently. Some non-Greek accented Latin slots are repurposed. This is not general Unicode support or support for polytonic Greek.

The runtime's existing general punctuation normalization is retained for dynamic strings. Static translated catalogue strings use the builder's punctuation slots. Original controller/control markers are not translated by the builder.

## Font and texture data

Top-level font chunk: 0x00030201. Language text chunk: 0x00039000.

In the observed font chunks, two 256-byte filename fields precede an FNTF structure. The header is 128 bytes, followed by sorted 16-byte glyph records and existing kerning/surface data. The record layout used is `<HBBHHBbbBHh`: code, width, height, x, y, page, x offset, y offset, kerning count, kerning index, advance. Layout and sorting are checked before modification.

Added raster data is placed below the original atlas. Width is retained; height grows to a power of two. Original ASCII records and original image pixels are verified unchanged. Both legacy-slot and Unicode glyph records point to the appended raster regions. Sampling scales, count and offsets are adjusted. Old kerning data is retained; new Greek kerning pairs are not generated. Mixed Greek/Latin spacing and old pairs involving repurposed extended characters need real-game testing.

FNTF versions 0x016E and 0x019E have different meanings at header offset 0x30. Only the latter's surface descriptor offset is relocated. Its dimension/size fields are adjusted. The exported-size field update is inferred from the supplied files and remains subject to game validation.

The seven definitions are `font_arial`, `conduitmditc_tt14i`, `lcd_let48`, `font_conduitmditctt38bi`, `font_arial12`, `font_impact36`, and `conduitmditc_tt21i`.

The builder reads JDLZ textures and emits uncompressed A8R8G8B8 font textures inside a standard uncompressed TPK layout. It retains texture hashes and names, preserves other textures, and updates dimensions, image sizes, offsets, log2 dimensions and format/mip metadata. The atlas changes increase download and memory size. No mipchain is generated for the replaced fonts. Texture alignment is 128 bytes. Only inputs with a first-position TPK are accepted.

The project's separate `tools/texpacks.py` reader was used to independently check generated texture payloads. That does not validate the engine's use of every field.

## Text and memory-card data

The English text hash table is read and rebuilt in sorted order. Duplicate text values share storage. The translation supplies 3,918 records; all seven extra original records remain present. Five are empty, and two are special byte strings whose semantics were not established. Their preservation is not a claim that every possible on-screen string has been translated.

The existing Greek locale is read as UTF-16, then converted to the same legacy glyph mapping and stored as the wide-character slot values expected by this translation route. The default takes all 14 records from the provided `LOCALE_GREEK.loc`. An explicit switch can use the draft JSON's save strings instead. This follows the byte-slot approach already used by the repository's Turkish-language generator; it still requires a save/load dialog playtest.

## Runtime and website integration

Greek is an English-base native language with a separate `LANGPACKS/greek` override. UTF-8 runtime strings are converted with the matching Greek map. A conditional 64 KiB reservation is added to the existing font-size table mechanism to cover extra glyph records. Its precondition checks the expected stock first entry. This modification has passed C syntax and extracted-function tests, not an actual allocator/game test.

The website registers `greek` / `el` and its translated label. The old website-only English fallback is removed. Both the native WASM runtime and frontend must be rebuilt and the separate pack served. The source overlay installer is hash-checked against the uploaded snapshot or previous translation overlay. A new upstream checkout may require a manual merge.

## Sources used

- User-supplied repository snapshot: `tools/langpacks.py`, `tools/texpacks.py`, `runtime/language.c`, `runtime/fileio.c` and the web language/settings files.
- User-supplied five assets: actual headers, glyph records, conversion tables, textures and locale strings. No source font files are included in the package.
- Reverse-engineering reference: https://yugecin.github.io/nfsu2-re/docs.html
- Reference parser structures: https://github.com/yugecin/nfsu2-re/blob/master/nfsu2-re-binfiles/main.c
- NFS custom font tooling documentation: https://github.com/xan1242/NFS_CustomFont/tree/master/Tools

The implementation is a prototype with offline evidence, not a claim of an official or fully compatible Greek release.
