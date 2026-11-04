# Experimental Greek font pack builder

This tool builds the Greek NFSU2 Web language assets from a user's own compatible
NFS Underground 2 game files plus a Greek-capable TTF/OTF font. Original game
assets and generated game bundles are not stored in this repository.

The Greek web catalogue is `web/src/app/lang/greek.json`; the in-game catalogue
is `tools/lang/greek.json`.

## Build a Greek pack

Install the Python dependencies in `requirements.txt`, then run:

```sh
python tools/greekfont/build_greek.py \
  --input /path/to/Languages.zip \
  --font /path/to/greek-capable-font.ttf \
  --output /tmp/nfsu2-greek
```

The output is `/tmp/nfsu2-greek/LANGPACKS/greek` and contains replacement
`English.bin`, font textures, `GLOBALA.BUN`, and the English-slot memcard locale.
The builder never overwrites the source assets.

To import that generated folder into a game tree using the project's normal
language-pack tool:

```sh
python tools/langpacks.py /path/to/NFSU2 --el /tmp/nfsu2-greek/LANGPACKS/greek
```

## Status

The binary/font structures and text encoding have offline regression tests, but
this Greek implementation still needs a real WebAssembly/gameplay test for font
rendering, spacing, memory use and menu layout. The generated assets are derived
from the user's game files and should not be committed to Git.
