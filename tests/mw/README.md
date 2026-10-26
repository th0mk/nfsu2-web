# Most Wanted's tests

Most Wanted's input scripts, seeds and checks, apart from Underground 2's, which stay where they are
(`tests/scripts/`, `tests/web/`, `tests/checks/`). The server started with `--game mw --dev` serves this folder
at `/tests/`, so a script here is `/tests/scripts/<name>.txt` to the test pages, as Underground 2's are on its own
server. The test tools take `--game mw`; `node web/tools/<tool>.mjs --game mw --help` says what each one has for
Most Wanted so far.

- `scripts/`: input scripts (runtime/autotest.c), one per scenario or reproduced bug.
- `seed/`: registry seeds (`registry.txt` files) that runs start from. Career saves stay outside the repository.
- `checks/`: `<scenario>.sh <log>`, a suite scenario's own check (web/tools/run-suite.mjs).

Nothing made from the game's files goes here.
