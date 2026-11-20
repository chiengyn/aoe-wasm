# CLAUDE.md

This file guides Claude Code when it works in this repository.

## What this is

Age of Empires: Rise of Rome compiled to WebAssembly. The recovered C++ source (`engine/upstream`, from
dcostap/aoe1-decomp at a pinned commit) runs unmodified through its real `WinMain` and game loop. The
Windows API it calls is implemented in `engine/platform/` on top of browser APIs. The current scope is
**multiplayer**. Single player, campaigns and the Scenario Builder are deferred. `docs/PLAN.md` holds the
phase plan, the decisions made so far and a detailed log of fixes and findings. Read it before changing
engine or platform behavior.

## Commands

```sh
# Build (emsdk 7.0.0 tested; pass its root with --sdk)
python3 engine/build.py --sdk <emsdk> --jobs 8 --game --probe       # debug game + probe -> engine/build/
python3 engine/build.py --sdk <emsdk> --jobs 8 --game --release --web  # optimized game -> web/engine/
python3 engine/build.py --sdk <emsdk> --game --relink                # relink only if all hashes match
python3 formats/build.py                                             # formats/build/{assets,dat}.wasm (clang)

# Data (reads the user's game folder, never modifies it; output is git-ignored)
python3 tools/prepare_native.py '<game folder>'      # web/game/native, video, help, native.json
python3 tools/prepare_test_data.py '<game folder>'   # web/game/ data for the probe/format tests
python3 tools/fetch_vendor.py                        # web/vendor: SpessaSynth + soundfont (music)

# Run
python3 tools/serve.py        # http://127.0.0.1:8788/ ; --host 0.0.0.0 for LAN

# Tests (headless Node; each writes reports/<name>.json)
node tests/test_native_menu.mjs
AOE_PLAYERS=2 node tests/test_native_multiplayer.mjs   # AOE_BUILD=build-release, AOE_DATA, AOE_PLAY_SECONDS
python3 tests/test_dplay_relay.py
python3 tests/test_web_static.py --built
```

The README has the full test list. After a change to `engine/platform/` or `engine/upstream/`, rebuild with
`--game` and run at least `test_native_boot`, `test_native_menu` and `test_native_multiplayer`. After a
change to the drawing code (`platform/software_draw.cpp`), also run `test_drawing.mjs` against the oracle
fixtures (see README).

## Layout

- `engine/upstream/`: vendored recovered source. Keep changes minimal and guard them with
  `__EMSCRIPTEN__` where possible. Every change must stay recorded: regenerate `engine/port.patch` and
  `engine/port-changes.json` with `tools/report_engine.py --reference <pristine aoe1-decomp checkout>`.
- `engine/platform/`: the port's own code. `engine/build.py` lists the files each build links (`GAME_LOCAL`
  for the game, and the list in `link_probe`). `port_prefix.h` is force-included into every upstream file.
- `engine/diagnostics/`: probe entry points used only by the Node tests. `unsupported.json` lists the Windows
  APIs that are still unported (they abort with their name when reached).
- `web/`: the page (`native.html`) and the modules it loads. The game runs in a Web Worker
  (`native-worker.mjs`). `web/engine/`, `web/game*/` and `web/vendor/` are generated.
- `tools/`: Python tools, standard library only (except the Unicorn-based oracle in `tests/`).
- `formats/`: standalone DRS/SLP/DAT readers in C. The tests use them to cross-check the engine; the
  game doesn't use them.
- `tests/lib/`: JS wrappers that the probe and format tests share.

## Things to know

- The game loop never returns. Emscripten Asyncify suspends it in `PeekMessage`/`GetMessage`/`Sleep` and
  in blocking DirectPlay calls, so code reachable from those calls must stay Asyncify-compatible.
- The recovered source is a 1998 **beta**. The target is the retail 1.0a `EMPIRESX.EXE`
  (SHA-256 `a8d8cfb241041f756416a74faa0fa77d894418a8c1bac2d7d06f2aeefa8b4b72`). Retail behavior is
  checked against the EXE's machine code (objdump/Unicorn) before it is ported. Don't assume the beta
  matches retail.
- The struct layouts must match the original MSVC x86 ABI. The upstream size/offset assertions stay enabled.
- `com_hand.cpp` is built at `-O1` even in the release build (`RELEASE_O1` in `engine/build.py`). At `-O2`
  a match crashes later in sprite drawing. The root cause is unknown.
- In local multiplayer tests, open each player in its own browser window. Chrome throttles hidden tabs.
- Text uses browser canvas fonts in place of Windows fonts. Font fidelity is intentionally out of scope.
- **Never commit game data**: anything from the user's installation or derived from it, such as `web/game*/`
  and `reports/`. `.gitignore` covers the generated folders. The only exception is the few screenshots in
  `docs/images/` that the README uses.
- Everything in this repository (code, comments, docs, commit messages) is written in English.
