# The device-call recorder (`NFSU2_DXREC`)

`runtime/comrec.c` writes every `IDirect3DDevice9` call the guest makes to a text file, so that two runs can be
compared call by call: the D3DX oracle against the effect HLE (TRACKER M5-07, M5-09), or two runs of one build (the
noise floor, L-082). `web/tools/dxrec-diff.mjs` compares two files.

## Turning it on

- `NFSU2_DXREC=<file>` records every Present; `NFSU2_DXREC=<file>,<from>` from Present `<from>` on;
  `NFSU2_DXREC=<file>,<from>,<to>` up to Present `<to>`, where the file is closed.
- In the browser the file goes to the page's save folder: `--env NFSU2_DXREC=/save/dxrec.txt` with run-suite's
  `--save-out <dir>` brings it out (`<dir>/<scenario>/dxrec.txt`).
- Off, the cost is one test per COM call (`com_call`, `com_new`).
- The file is flushed at every Present and closed at the guest's `ExitProcess` and at the host's exit.

## The lines

| Line | Meaning |
|---|---|
| `dxrec 1` | the format's version, first line |
| `F <n>` | Present `n`'s calls follow (`n` = Presents so far; `F 0` before the first) |
| `<Method> <args>[ -> <out>][ =<hr>]` | one device call, written after it ran |
| `Capture sb#3`, `Apply sb#3`, `Release sb#3` | a state block's calls (`IDirect3DStateBlock9`) |
| `E<id> <method>[ <arg>]` | a context line of the game's: Most Wanted's effect methods (`runtime/mw/d3dxfx_mw.c` for the HLE, `fxcensus_mw.c` for the oracle), `<id>` the game's effect row, `-` for none |

What differs between two equal runs is never written as it is:

- A COM object is named by its kind and creation number: `tex#12` is the 12th texture made, `surf`, `vb`, `ib`,
  `decl`, `sb`, `q`, `swap`, `fx`, `pool`; a shader by a hash of its code, `sh:1a2b3c4d`; the device is `dev`.
- Data a call points at is written instead of the pointer: constants, matrices, viewports, materials, lights, clip
  planes and rectangles as hex dwords `[3f800000,0,...]` (past 32 dwords as `#<fnv-1a>/<count>`); the vertices and
  indices of the UP draws, a shader's code, a gamma ramp and present parameters as `#<fnv-1a>/<bytes>`.
- An output pointer is written as `out`, and what the call left there after ` -> `.
- A result other than 0 follows `=`.

The calls a state block's `Apply` replays (com.c `g_sb_replay`) are not written; `Apply` is.

## Comparing: `web/tools/dxrec-diff.mjs`

```
node web/tools/dxrec-diff.mjs A.txt B.txt [--show N] [--frames LO-HI] [--quiet]
```

- **calls**: the device calls of each frame as written, in order (Myers' diff per frame); each call only one side
  made counts. Context lines are shown around a difference, never compared.
- **draws**: each file replayed into the device state com.c keeps (render, stage and sampler states, textures,
  shaders, constants, declaration, streams, transforms, lights, viewport, targets), with its state blocks recorded,
  captured and applied as com.c does (`sb_recordable`; `Capture` refreshes render, stage and sampler states,
  textures, shaders, declaration and FVF only); the state every `Draw*` call sees is compared draw by draw.
- It prints the totals, the first N differences of each kind and `result: <calls> call differences, <draws> draw
  differences`; exit code 0 when both are 0.

The replay takes a block's constants as they were when recorded, where com.c reads the guest memory the recorded
pointer names at `Apply`; that is the one place the replay can differ from the device.
