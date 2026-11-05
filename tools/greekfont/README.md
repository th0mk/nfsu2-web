# Greek font pack builder

This tool builds the Greek NFSU2 Web language pack from the game's own files (`LANGUAGES/English.bin`,
`LANGUAGES/LanguageTextures.bin`, `GLOBAL/GLOBALA.BUN`, `memcard/LOCALE_ENGLISH.loc` and `LOCALE_GREEK.loc`), with
the Greek letters drawn in from Fira Sans (SIL Open Font License). Game files and built packs are not stored in this
repository.

The Greek page catalogue is `web/src/app/lang/greek.json`; the in-game catalogue is `tools/lang/greek.json`.

## Build a Greek pack

Install the Python dependencies in `requirements.txt`, then run:

```sh
python tools/greekfont/build_greek.py --input <folder with those five files> \
  --font FiraSans-Regular.ttf --bold-font FiraSans-Bold.ttf \
  --italic-font FiraSans-Italic.ttf --bold-italic-font FiraSans-BoldItalic.ttf \
  --output /tmp/nfsu2-greek
python tools/greekfont/verify_greek.py --input <the same folder> --build /tmp/nfsu2-greek
python tools/langpacks.py /path/to/NFSU2 --el /tmp/nfsu2-greek/LANGPACKS/greek
```

The output is `/tmp/nfsu2-greek/LANGPACKS/greek` (about 3.5 MB): `English.bin`, `LanguageTextures.bin`,
`GLOBALA.BUN` and the memory-card texts. The builder never overwrites its input. `FORMAT_NOTES.md` has the details.
