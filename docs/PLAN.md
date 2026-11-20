# Plan: full WebAssembly port of Age of Empires: Rise of Rome

> This is the working plan and progress log, kept as written during development. Some paths and pages it
> mentions belong to the earlier layout: `src/` is now `formats/`, `tools/build.py` is `formats/build.py`,
> `tools/prepare.py` is `tools/prepare_test_data.py`, and the probe-based prototype pages (`menu.html`,
> `play.html`, `units.html`, `resources.html`) were removed. See the README for the current layout.

Source: [dcostap/aoe1-decomp](https://github.com/dcostap/aoe1-decomp) at `6d6d8b14023aa74e6ac30a6c88229d5b818fb805`
(checked 2026-11-18: this is still upstream `HEAD`; nothing newer exists). The source is vendored in
`engine/upstream`.

## Where things stand

- All 225 recovered C++ files compile to WASM. The original assembly drawer was converted to C++ and checked
  byte-for-byte against the supplied `EMPIRESX.EXE`.
- **The original `WinMain` and `RGE_Base_Game::run()` loop never run.** JavaScript creates individual native
  screens (main menu, multiplayer lobby, in-game screen) through test entry points (`engine/diagnostics/*`).
  Multiplayer uses a custom HTTP lockstep (`tools/multiplayer.py`) and JS validation instead of the game's
  DirectPlay networking.
- 123 Windows functions are not implemented yet (`engine/diagnostics/unsupported.json`): window/message
  handling, GDI, files, registry, DirectSound, DirectPlay, MIDI/MCI, clipboard, WinHelp, IME. Reaching one
  aborts with an explicit error.

That approach can't reach "full" without hand-writing every screen. **Core change: run the real `WinMain`
and game loop, and implement the Windows functions the game calls on top of the browser.** Every screen and
feature then runs through the game's own code.

## Phase 0: Groundwork
- Git repository for per-phase review (no commits unless requested).
- A second build target, `game`, linking the real `main.cpp` (`WinMain`), next to the existing probe
  build, which keeps working until the new path replaces it.
- Browser loop: the game's `while(1){ PeekMessage … handle_idle }` never returns. Emscripten Asyncify pauses
  it at `PeekMessage`/`GetMessage`/`Sleep` and hands control back to the browser each frame. The recovered
  source stays unmodified.
- Headless Node harness running the real `WinMain`; it reports the first unimplemented Windows call.
- **Done when:** the real `WinMain` starts in a Web Worker, its logs reach the browser console, and an
  unimplemented call ends it with a clear report (input for Phase 1).

## Phase 1: Windows layer (boot to the main menu)
- Windows and messages: a message queue fed by browser events, so mouse, keyboard and text input arrive as
  `WM_*` messages with correct `wParam`/`lParam`. Window procedures, `DefWindowProc`, timers, key-state
  queries, cursor and window-position calls, system metrics.
- Graphics: the game's own windowed DIB mode (`DrawType 1`, already used by the probe build), so no
  DirectDraw emulation is needed. The 8-bit frame and palette are presented to a canvas.
- Text: canvas fonts stand in for Windows fonts (font fidelity was waived earlier).
- Files: game data in an in-memory filesystem. `savegame/`, `scenario/`, `campaign/`, player profiles and
  registry values live in persistent browser storage (IndexedDB).
- Language DLLs: strings, bitmaps and icons extracted from `language.dll`/`languagex.dll` at prepare time.
- CD check: report the supplied folder as the game CD.
- **Done when:** the real executable boots to the main menu, and the menus on the multiplayer path that
  don't need a network connection (main menu, Multiplayer Connection) work by mouse and keyboard without
  hitting an unimplemented call. The lobby and game-settings screens open only through a DirectPlay
  connection, so they are verified in Phase 4.

## Phase 2: Single player (deferred: scope is multiplayer only for now)
- Random map against the AI, scenarios, campaigns, save/load, victory/defeat, timeline/achievements screens,
  and the Scenario Builder.
- **Done when:** a 30-minute game against the AI runs without errors, save/load round-trips, a campaign
  mission can be started and won, and the editor can save a scenario and play it.

## Phase 3: Sound, music, video, help
- Sound effects: DirectSound emulated on Web Audio, using the original WAVs; in-game volume controls work.
- Music: `music.cpp` plays `.MID` through MCI. In the browser, a General MIDI synth with a freely licensed
  soundfont renders the game's own `.MID` files.
- Intro videos (`Logo1.avi`, `Introx.avi`): converted with ffmpeg at prepare time if it supports their codec;
  otherwise skipped the way the game's `NOSTARTUP` option does.
- Help: the `.HLP` file converted to HTML, or a no-op.

## Phase 4: Multiplayer with the original networking code
- Emulate the `IDirectPlay3`/lobby interfaces the game uses. A "Browser" service provider relays packets
  through a WebSocket server added to `tools/serve.py`. The game's own session list, lobby, chat, sync,
  speed control and drop handling run unmodified, for up to 8 players. This also covers diplomacy, chat
  recipients and the missing menu sub-screens.
- An option to listen on the local network, not just localhost.
- Remove the HTTP lockstep, JS validation and custom screens once the original path matches them.
- **Done when:** 2-, 4- and 8-player sessions in separate tabs, and across two machines on the LAN, play a
  long game with no desync stop, and dropping a player is handled by the game itself.

## Phase 5: Matching the retail EXE, and polish
- Beta vs retail: the recovered source is a 1998-07-17 beta, not the supplied retail `EMPIRESX.EXE`
  (SHA-256 `a8d8cfb2…`). The folder name suggests a build with cheat commands enabled, so
  start there. Compare behavior against the EXE with the existing Unicorn tools and port the differences
  that matter.
- Save and scenario compatibility with files made by the Windows game.
- Performance: an optimized build without debug info (about 10 MB now), frame pacing, game speed against
  original timing.
- Out of scope: mobile, and playing against the Windows EXE over the network.

## Checks for each phase
- The existing tests keep passing.
- A headless Node harness running the real `WinMain` with scripted input, failing on any unimplemented call.
- Browser runs in Chrome after each phase, with screenshots.
- A multi-tab multiplayer soak test.
- Work stops after each phase for review.

## Risks
- Asyncify makes the WASM bigger and slower. If that's noticeable, restrict it to the paths that pause
  (`ASYNCIFY_ONLY`), or use JSPI.
- Beta-vs-retail differences may only show up during play.
- The emulated DirectPlay/DirectSound interfaces must match the recovered code's expected memory layout exactly.
- Text won't look identical to Windows.
- The package includes the user's game files, so it stays on the local machine/LAN, not on a public site.

## Decisions
1. Scope: **multiplayer only for now** (user, 2026-11-18). Single player, campaigns and the Scenario Builder
   are deferred; Phase 2 is skipped. The real `WinMain` path still runs the main menu and the screens and
   game code multiplayer uses.
2. Multiplayer: **the game's original networking** (DirectPlay emulated over WebSockets; user,
   2026-11-18). Codex's HTTP lockstep stays usable until Phase 4 replaces it, then gets removed.
3. Media (user, 2026-11-18):
   - Intro videos (`Logo1.avi`: MS Video 1; `Introx.avi`: Indeo 5): **convert the game's own files with
     local ffmpeg** at prepare time.
   - Music: **a free General MIDI soundfont** plays the game's `.MID` files. None is installed locally, so it
     gets downloaded in Phase 3. Chosen: GeneralUser GS v2.0.3 with SpessaSynth (download approved
     2026-11-18).
   - Everything else (graphics, sound effects, data, strings) comes from the supplied game folder.
4. Git: `git init` in this folder; approved by starting Phase 0 (2026-11-18). No commits without a request.

## Progress
- [x] Phase 0: groundwork (2026-11-18)
  - `git init`; Codex's files are staged as the review baseline (no commit).
  - `python3 engine/build.py --sdk ../../../work/emsdk --game [--probe] [--web] [--relink]` links
    `engine/build/game.{mjs,wasm}`: upstream `WinMain` plus `platform/win32_loop.cpp` (`main()`, message
    queue, `PeekMessageA`/`GetMessageA`/`PostQuitMessage` yielding through Asyncify). Debug build 14.5 MB
    (probe 10.3 MB).
  - Hooks Codex inserted into upstream (`TTextPanel::append_line(const char*,long)`, `wasm_lobby_role`) moved
    from the probe into `platform/port_glue.cpp`; `PostMessageA` moved from `diagnostics/host.cpp` into the
    shared queue. Both builds link them.
  - `tests/test_native_boot.mjs` → `reports/native-boot.json`: message-queue self-test 5/5, `WinMain`
    starts, and the run stops at the first unported call: **`CreateFileA`** (from `Res_file.cpp`, opening
    the DRS archives). That is where Phase 1 starts.
  - `web/native.html` runs the same build in a Web Worker; logs reach the page console (checked in Chrome).
  - All earlier suites pass against the rebuilt probe.
- [x] Phase 1: Windows layer (2026-11-18)
  - Run it: `python3 tools/prepare_native.py '<game folder>'` once, then `python3 tools/serve.py` and open
    `http://127.0.0.1:8788/native.html`. Command line: `NODXCHECK NOSOUND NOSTARTUP` (`?cmd=` overrides);
    sound and intro videos are Phase 3.
  - The game's own full-screen DirectDraw path runs unmodified on software surfaces
    (`platform/win32_ddraw.cpp`). Its software mode (`WING`) is refused by this build's `TDrawSystem::Init`,
    so it was never the shipping path.
  - Windows layer for the game build only: `win32_kernel.cpp` (files, mapping, find, mutex, version, CD
    drive "AOE" from the supplied folder, PE resources from the game's own EXE/DLLs, registry saved to
    `/aoe-registry.dat`), `win32_user.cpp` (windows with 16-bit handles, messages, focus/capture, timers,
    paint regions, edit control, key state, `TranslateMessage` using the browser's characters),
    `win32_gdi.cpp` (DCs on 8-bit surfaces, palette-index color mapping, browser fonts rasterized to
    1 bit), `win32_inflate.cpp` (replaces `Inf32.asm`), the original `Deflate.c`, and placeholders in
    `win32_media.cpp`/`win32_dplay.cpp` until Phases 3–4.
  - Recovered code calls POSIX `open`/`unlink`/`chdir` with Windows paths; the game link wraps them
    (`--wrap`) through the same path normalization.
  - Shutdown: `~TPanelSystem` frees list nodes that `~TPanel` already freed (double free unnoticed by the
    Windows CRT at exit). The game build uses `EXIT_RUNTIME=0`, so global destructors don't run after
    `WinMain` returns; the game's own shutdown (profile and `scenario.inf` saves) still runs.
  - Browser: `web/native.html` + `native-worker.mjs` (OffscreenCanvas, Windows virtual keys and scan
    codes, the game's own software cursor). Files the game writes persist in IndexedDB
    (`native-storage.mjs`). A hidden page deactivates the game (`WM_ACTIVATEAPP`); losing focus only
    releases held keys.
  - Tests: `tests/test_native_boot.mjs` (boot, frame PNG) and `tests/test_native_menu.mjs` (12 checks: main
    menu, Multiplayer Connection, edit typing/Backspace/Home/Delete, Escape, Down+Enter, Cancel, Exit with
    WinMain returning 0, no unported calls). Checked by hand in Chrome as well.
  - Still open: the connection list is empty until Phase 4 adds the browser connection type; Help opens
    nothing (WinHelp); text uses browser fonts.
- [ ] Phase 2: single player (deferred)
- [x] Phase 3: sound, music, video, help (2026-11-18, awaiting review)
  - `engine/platform/win32_media.cpp`: DirectSound (`IDirectSound`/`IDirectSoundBuffer` on plain memory:
    Lock/Unlock, Play/Stop, volume, pan, frequency, status, duplicates), the mixer (wave and synthesizer
    lines behind the game's sound and music sliders), MCI (no CD audio, so the game picks its MIDI files; a
    sequencer device with play/pause/seek/status and `MM_MCINOTIFY` at the end of each file; the MIDI length
    comes from the file's own tempo map), MCIWnd (intro video window: mode, play/pause/stop, click to
    skip). Timing stays in the game; the browser only renders. Dsutil.c's WAV streaming (the `STREAMMUSIC`
    option) reports "not available": the supplied game has no WAV music.
  - Browser: `web/native-media.mjs` (worker forwards to the page; the page plays PCM through Web Audio
    with the game's volume/pan/pitch, mixer gains per line, and the videos over the canvas). `native.html`
    starts WinMain on a click so the browser allows sound from the first intro frame (`?autostart` skips
    it). Default command line is now `NODXCHECK` (sound and intro videos on).
  - Videos: `tools/prepare_native.py` converts `avi/*.avi` with local ffmpeg to WebM (VP9/Opus) in
    `web/game/video`. The game plays `logo1`, skips the missing `logo2`, plays `introx` at double size in
    its 640x480 video mode, then the main menu. (This folder's `Logo1.avi` is the repacker's splash.)
  - Tests: `tests/test_native_media.mjs` (16 checks: MIDI menu music via MCI, button sound buffers, PCM
    format, mixer lines, intro video sequence with sizes and click-to-skip). Checked in Chrome: intro
    videos with sound, main menu, audio context running, click sounds playing, music volume at 50%.
  - MIDI music (user approved the download, 2026-11-18): `python3 tools/fetch_vendor.py` installs
    SpessaSynth (`spessasynth_lib` 4.4.3 + `spessasynth_core` 4.4.5, Apache-2.0, npm sha512 checked) and the
    GeneralUser GS v2.0.3 soundfont (32 MB, pinned commit, sha256 checked) into `web/vendor` (git-ignored).
    `web/native-midi.mjs` plays each file the game's MCI sequencer starts, from the game's position, never
    looping by itself. `spessasynth_core` imports `fflate`/`stb-vorbis` only for compressed banks and SF3;
    small stand-ins in `web/shims` take their place (an import map in `native.html`). Checked in Chrome: the
    menu music plays through the music volume line (signal peak 0.41 at the game's 50% music volume).
  - Help (2026-11-18): `tools/convert_help.py` (own WinHelp reader, no download) converts `EMPIRES.HLP`
    during `prepare_native.py` into `web/game/help`: 221 topics, 241 keywords, 179 pictures as BMP with
    their hotspots as image maps, jumps/popups, fonts, tables, and the help's own buttons (Back, Search,
    Print, Game). `WinHelpA` forwards to the page, which shows it in an overlay: Help buttons open their
    keyword topic, Contents opens the contents topic, GAME/Close return to the game.
    `tests/test_help_convert.py` (8 checks, including the context hash against the file's stored hashes).
  - Graphics fix (2026-11-18): unexplored map showed old terrain after scrolling. `TDrawArea::PtrClear`
    (Codex's replacement for the assembly) skipped unlocked surfaces, but `RGE_View::draw` clears the newly
    exposed strips of its terrain buffer while it is unlocked. It now clears through the row pointers once
    the surface has been locked. `test_native_multiplayer.mjs` checks that scrolling away shows black and H
    returns to the base; its lockstep comparison now records each machine's state per world time.
- [ ] Phase 4: original multiplayer (in progress, 2026-11-18)
  - Done: `engine/platform/win32_dplay.cpp` is a DirectPlay service provider, "Browser Connection For
    DirectPlay" (`IDirectPlay` v1 + `IDirectPlay3A`): EnumSessions, Open (create/join), CreatePlayer,
    DestroyPlayer, Send, Receive (with peek), GetMessageCount, player data/names, session description,
    EnableNewPlayers, caps. System messages (`DPSYS_CREATEPLAYERORGROUP`, `DESTROYPLAYERORGROUP`,
    `SETPLAYERORGROUPDATA`, `SETPLAYERORGROUPNAME`, `SETSESSIONDESC`, `HOST`, `SESSIONLOST`) are laid out as
    DirectPlay delivered them. Calls that waited on the network block by yielding (Asyncify).
  - Relay: `tools/dplay_relay.py` (sessions, players, routing, player data, new-player gating, host
    migration / session loss), WebSocket endpoint `/api/dplay` in `tools/serve.py` (standard library only).
    `--host 0.0.0.0` listens on the local network. Browser transport: `web/native-dplay.mjs`.
  - Fixes found on the way: C: drive mapping (`fopen("c:\\aoeWVlog.txt")` failed, then `fprintf(NULL)`),
    WebSocket accept GUID, and one `__EMSCRIPTEN__` change in `RGE_View::Update_Render_Pointers`
    (`view.cpp`): in terrain mode the render area is unlocked (`Bits == 0`), so the system-memory surface's
    last mapped address (`LastBits`) is used; the first game frame crashed otherwise.
  - Checked in Chrome: connection list, Multiplayer Games (Create / Show Games / Join), lobby with both
    players, chat, ready, Start Game, in-game HUD, training (H, C), selecting and moving units, the
    in-game Menu, Game Settings and Diplomacy dialogs.
  - Tests: `tests/test_dplay_relay.py` (protocol), `tests/test_native_multiplayer.mjs` (`AOE_PLAYERS=2|4`):
    2 players 24 checks, 4 players 47 checks: create/list/join, lobby chat, ready, start, a host command
    applied on every machine, identical player state at equal world times, the game's own out-of-sync
    flag never set, and a dropped player (2 players: the remaining player wins and the match ends;
    4 players: the other three continue in sync).
  - 8 players: 72 checks, 43 lockstep comparisons; after a drop the other seven continue in sync. The
    test now has every player give commands in turn during play.
  - Removed Codex's HTTP multiplayer (user, 2026-11-18): `tools/multiplayer.py`, the `/api/mp/` endpoint,
    `multiplayer.html`/`.mjs`, its worker and client, `tests/test_room_transport.py`, the room dialog in
    `menu.html` (its Multiplayer button now opens `native.html`); `index.html` opens `native.html`. The
    probe's two-engine tests (`test_multiplayer`, `test_game_ui`, `test_battlefield`) stay: they test the
    probe build, not networking.
  - Long game: 4 players for 20 minutes of real time (about 22 minutes of game time), every player giving
    commands in turn: 460 checks, 241 lockstep comparisons, no desync, no heap growth.
  - Cursor fixes (2026-11-18): menus and the lobby use the Windows cursor (`SetCursor`), which is now shown
    as the matching browser cursor; in a match the game draws its own pointer, which failed every frame
    because `GetClipper` returned the wrong error for "no clipper" (now `DDERR_NOCLIPPERATTACHED`).
    `test_native_multiplayer.mjs` checks both.
  - Remaining: two machines on a LAN (not testable here). A hidden browser tab is throttled by Chrome,
    so for local testing open each player in its own window.
- [ ] Phase 5: retail parity and polish (in progress, 2026-11-18)
  - Second edition from the user: the AOEHD.net "333HD enhanced" repack (2009).
    `tools/compare_versions.py A B` → `reports/version-compare.json`.
  - Both EMPIRESX.EXE files are the same retail 1.0a build (1998-09-01, file version 00.04.2.0901) with
    small patches; neither is pristine. 11 patch sites, all identified:
    - The original folder (this port's data): `_stricmp` returns "equal" for every comparison (a blanket
      no-CD crack). Side effect: the five `"none"` checks in panel loading always match, so that EXE skips
      800x600+ screen backgrounds and optional panel pictures. The port runs the unpatched code.
    - 333HD: its own no-CD crack (CD label check); cheat commands in multiplayer redirected to the
      save-game case (cheats never apply); commands also sent to non-human player slots; and six hooks
      into `ExWE.dll` (an online-lobby add-on replacing the DirectPlay import: lobby control, remembered
      settings, game name). Not ported: they are platform/anti-cheat policy, not retail behavior.
  - 333HD content: taunts 26-50 (`sound/taunt026-050.wav`) need no code: the game plays `Taunt<N>.wav`
    for any number 1-999 when the file exists. 14 extra multiplayer scenarios; a modified `sounds.drs`;
    help sounds/pictures for the help file's AoEHlp.dll macros; no Rise of Rome music (`xmusic*.mid`).
  - `tools/prepare_native.py --out game-333hd '<folder>'` prepares a second edition beside the first;
    `native.html?data=game-333hd` plays it. Third-party add-ons (DirectPlay installer, lobby/windowing
    wrappers, patch programs) are not copied. A scenario with a Chinese file name is skipped (non-ASCII).
  - Tests: `test_native_multiplayer.mjs` takes `AOE_DATA` and checks taunts (1 everywhere, 26 on 333HD:
    the host plays the matching WAV). `test_native_scenarios.mjs`: every retail scenario loads through
    the game's header and information readers (23 + 26 files, internal version 1.15).
  - Release build: `engine/build.py --game --release [--web]` → `engine/build-release` (git-ignored):
    `-O2` without debug info, no runtime assertions. `game.wasm` 2.9 MB (0.9 MB gzipped) instead of
    17.6 MB (6.3 MB). `web/engine` now serves it; the debug build stays in `engine/build` for the tests'
    default and for diagnosis (`AOE_BUILD=build-release` runs the native tests on the release build).
    `--names` keeps function names for stack traces.
  - `com_hand.cpp` stays at `-O1` in the release build (`RELEASE_O1` in build.py): at `-O2` a match
    crashes later in sprite drawing (memory overwritten). Found by bisecting objects (every other file is
    fine at `-O2`); `-fno-strict-return`/`-fno-delete-null-pointer-checks` don't help; the compiler shows no
    warnings; an AddressSanitizer build of that file didn't reach the match (the lobby times out). The
    undefined behavior inside it is not pinned down.
  - Measured (headless, 2-player 40 s match): both builds pass; CPU over the run 9.3 s debug vs 8.6 s
    release (the game is mostly idle between paced frames); boot to the first menu frame ~0.59 s vs
    ~0.55 s. The gain is the download size.
  - Rendering fixes (checked in Chrome on the release build):
    - The recovered `PtrClear`, `PtrSpanCopy` and `OverlayMemCopy` assembly lock the DirectDraw surface
      themselves (`PtrClear` and `OverlayMemCopy` unlock it again; `PtrSpanCopy` leaves the render area
      locked for the sprites, and `view_function_terrain` unlocks it). The C versions in
      `software_draw.cpp` now do the same.
    - Before this fix, `PtrClear` skipped surfaces that had never been locked. The text panels (messages,
      help line, game clock, population) clear their image buffers to colour key 1 and copy them keyed,
      so their buffers stayed black (0). The result was black boxes behind text and black rectangles over
      the map, with old text left under new text.
    - The earlier `LastBits` workarounds in `PtrClear` and `RGE_View::Update_Render_Pointers` are gone;
      `view.cpp` matches the reference source there again.
    - Black tile-shaped holes in the map (2026-11-19). These appeared after a scroll that clears two strips
      of the terrain buffer: diagonal scrolling, or H / a minimap click less than a screen away.
      - `view_function_terrain` adds the spans of a tile that touches both strips, then narrows the span
        window to strip 2. `shape_draw` passes that window (`Max_Span_Px`) to `ASMDraw_Sprite`.
      - Retail `_ASMDraw_Sprite` uses `min_span`/`max_span` only to reject a sprite lying wholly outside
        them; rows clip to the span list alone.
      - The C version also cut every row at `max_x`, so when strip 2 lay left of strip 1, the tile's part
        in strip 1 stayed black in the cache. The screen showed the holes wherever it copied from the
        cache, for example along the paths of moving units.
      - The clamp is removed. `test_drawing_oracle.py` adds cases whose spans extend past the window
        (sprite plain and mirrored, and `HLine`, which does clamp): 3320 cases, and the old drawer fails
        the first new case.
  - Input fixes:
    - Pointer events carry no click count (the page tested `detail === 2`, which is always 0).
      `win32_user.cpp` now makes `WM_xBUTTONDBLCLK` the way Windows does: same button and window, within
      500 ms (`GetDoubleClickTime`) and 4x4 px (`SM_CX/CYDOUBLECLK`), and only for `CS_DBLCLKS` classes.
      It uses the browser event's time. Double-clicking a unit selects that unit type on screen; lists
      take double-clicks too.
    - The retail EXE also adds the clicked unit three times to `sel_list` (0x50C12A; `select_one_object`
      at 0x45EB70 has no duplicate check), so `sel_count` is the unit count + 2, as in the original.
    - Key state is split as on Windows. `GetAsyncKeyState` reads the physical state.
      `GetKeyState`/`GetKeyboardState`/`TranslateMessage` read the thread state, which changes when the
      game takes the input message from its queue. Arrow-key scrolling polls `GetKeyboardState`.
    - A button pressed while another is held (`pointermove` with `button` set) is forwarded.
  - Widescreen (2026-11-19). Not in the retail game; `native.html?wide=0` keeps the 4:3 game screen.
    - The page sends the shape of the space it has for the game (`aoe_set_display_aspect`). The game screen
      keeps the height of the chosen size (640x480, 800x600 or 1024x768) and widens to that shape, in whole
      8 px, up to 2.4:1. Menus and other full-screen screens stay at the chosen 4:3 size: their backgrounds
      and layouts are 4:3 only. 1280x1024 is not widened.
    - `RGE_Base_Game::fit_screen` sets the display size: from `TScreenPanel::setup` (screens are laid out
      at the menu size), `TScreenPanel::set_focus` (a screen shown; the game screen widens) and the game
      screen's `handle_idle` (the page was resized). It uses the game's own `set_screen_size`, the path the
      in-game Options dialog takes to change resolution. That dialog's size choice is now the menu size,
      and the game screen widens at its height. The registry keeps the menu size.
    - `TRIBE_Screen_Game::handle_size` widens the map view and moves the right-hand parts (minimap, the
      top-right and minimap buttons) to the right edge. The interface art keeps its left part in place,
      draws the rest shifted right, and fills the gap with copies of the relief/top-bar texture. The
      copy width per culture and size (`draw_game_screen_art`) was measured from the retail art so that
      copies join without a seam where the art repeats (most cultures). East Asian and Roman bottom panels
      and a few top bars show a faint join.
    - A wider view shows more of the map: in multiplayer, players with wider pages see more.
  - Tests:
    - `test_native_multiplayer.mjs`: the host's page is 16:9 (lobby 800x600, game screen 1064x600), the
      guest's 4:3; the lockstep checks pass with the two view sizes.
    - `test_native_menu.mjs`: thread vs physical key state, modifier correction, `PtrClear` on a
      never-locked DirectDraw area.
    - `test_native_multiplayer.mjs`: one click vs double-click on a villager (all villagers on screen
      selected; retail selection count).
  - Map-view input handlers compared with the retail EXE (`objdump` of `.text`):
    - Method: the `TRIBE_Main_View` virtual table is at 0x54B6D8, and its slots follow `panel.h`'s
      declaration order (slot 37 = `mouse_left_dbl_click_action`).
    - Compared case by case (game-mode jump tables, constants, calls):
      - `RGE_Main_View`: `handle_mouse_up` 0x51B890, `mouse_move_action` 0x51B8E0,
        `mouse_left_down_action` 0x51B930, `mouse_left_up_action` 0x51BF30 (click and box selection,
        Ctrl/Shift-click, left-click orders, Shift waypoints).
      - `TRIBE_Main_View`: `mouse_left_up_action` 0x50B8E0, `mouse_right_up_action` 0x50BC50 (attack
        ground, convert, heal, attack, repair, unload).
      - All match the source.
    - One difference: retail `mouse_left_dbl_click_action` (0x50C120) returns 0 in the Scenario Editor
      (`prog_mode` 7). Restored in `tvw_main.cpp`.
    - Layout notes: retail `RGE_Base_Game` keeps `prog_mode`/`game_mode` at 0x1B0/0x1B4, as in the source.
      Retail `RGE_Player` has 4 more bytes before `selected_obj` (0x134 vs 0x130; `sel_count` 0x1A0 vs
      0x19C). The retail view vtable has one slot more than the source between slots 55 and 57.
    - Not yet compared: `mouse_left_move_action` 0x51BE10, `mouse_right_down_action` 0x51C880,
      `mouse_right_move_action` 0x51C930, `handle_idle` 0x51B1B0, keyboard hotkeys.
  - Remaining: the com_hand.cpp root cause; behavior differences between the beta source and the retail build beyond
    what the existing RNG/UI/drawing checks and the scenario loader cover; scenario files with non-ASCII
    names.
