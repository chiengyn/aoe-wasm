// Phase 1: the real WinMain build driven through the multiplayer-path menus with Windows input messages.
// Main menu -> Multiplayer Connection (edit control typing, Backspace/Home/Delete) -> Escape -> keyboard
// Down+Enter -> Cancel -> Exit, which must end WinMain normally. Writes reports/native-menu-tests.json.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
// AOE_BUILD=build-release tests the optimized build (engine/build.py --release).
const BUILD_DIR = process.env.AOE_BUILD || 'build';
const {default: createGame} = await import(`../engine/${BUILD_DIR}/game.mjs`);
import {metricFonts} from '../web/aoe-fonts.mjs';

const root = new URL('../', import.meta.url);
const read = p => fs.readFileSync(new URL(p, root));
const native = JSON.parse(read('web/game/native.json'));
const wasmBinary = read(`engine/${BUILD_DIR}/game.wasm`);
const log = [], boxes = [];
let frames = 0, exitCode = null, aborted = null, checks = 0;
const ok = (value, message) => { assert.ok(value, message); checks++; };
const eq = (a, b, message) => { assert.deepEqual(a, b, message); checks++; };

const game = await createGame({
  wasmBinary, noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => frames++,
  onMessageBox: (title, text) => boxes.push({title, text}),
  print: line => log.push(line), printErr: line => log.push(line),
  onGameExit: code => { exitCode = code; }, onExit: code => { exitCode ??= code; }, onAbort: reason => { aborted = String(reason); },
  preRun: [m => {
    for (const d of native.directories) m.FS.mkdirTree('/' + d);
    for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
  }],
});
try { game.callMain(['NODXCHECK', 'NOSOUND', 'NOSTARTUP']); } catch (e) { if (!/unwind/i.test(String(e))) throw e; }

const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(test, what, ms = 5000) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (aborted) throw new Error('Aborted: ' + aborted); if (test()) return; await sleep(20); }
  throw new Error('Timed out waiting for ' + what);
}
const edit = () => game.ccall('aoe_find_window', 'number', ['string', 'number'], ['edit', 0]);
const text = h => game.ccall('aoe_window_text', 'string', ['number'], [h]);
const click = async (x, y) => { game._aoe_input_mouse(0, 0, x, y); game._aoe_input_mouse(1, 0, x, y); await sleep(40); game._aoe_input_mouse(2, 0, x, y); await sleep(150); };
const VK = {BACK: 8, RETURN: 13, ESCAPE: 27, HOME: 0x24, DELETE: 0x2e, DOWN: 0x28, SHIFT: 0x10};
const press = async (vk, ch = 0) => { game._aoe_input_key(vk, 0, 1, 0, ch); game._aoe_input_key(vk, 0, 0, 0, 0); await sleep(40); };
const type = async s => { for (const c of s) await press(c === ' ' ? 0x20 : c.toUpperCase().charCodeAt(0), c.charCodeAt(0)); };
function frameStats() {
  const [bits, w, h, pitch, pal] = [0, 1, 2, 3, 4].map(f => game._aoe_frame_info(f));
  const colors = new Set();
  let lit = 0;
  for (let y = 0; y < h; y += 2) for (let x = 0; x < w; x += 2) {
    const i = game.HEAPU8[bits + y * pitch + x];
    colors.add(i);
    if (game.HEAPU8[pal + i * 4] + game.HEAPU8[pal + i * 4 + 1] + game.HEAPU8[pal + i * 4 + 2] > 60) lit++;
  }
  return {width: w, height: h, colors: colors.size, lit};
}

// 1. Boot to the main menu.
await until(() => frames > 0, 'first frame', 20000);
const menu = frameStats();
eq([menu.width, menu.height], [800, 600], 'DirectDraw primary surface is 800x600');
ok(menu.colors > 64 && menu.lit > 10000, 'Main menu background is drawn with the game palette');
eq(game.ccall('aoe_find_window', 'number', ['string', 'number'], ['edit', 0]), 0, 'No edit control on the main menu');

// 1b. Key state as on Windows: GetAsyncKeyState follows the device at once; GetKeyState/GetKeyboardState follow
// the input messages as the game takes them from its queue (bit 0 thread state, bit 1 physical state).
const keys = vk => game._aoe_test_keys(vk);
game._aoe_input_key(VK.SHIFT, 0x2a, 1, 0, 0);
eq(keys(VK.SHIFT) & 3, 2, 'Shift press: physically down, thread state unchanged until WM_KEYDOWN is read');
await until(() => (keys(VK.SHIFT) & 3) === 3, 'thread state after WM_KEYDOWN');
game._aoe_input_key(VK.SHIFT, 0x2a, 0, 0, 0);
eq(keys(VK.SHIFT) & 3, 1, 'Shift release: physically up, thread state still down until WM_KEYUP is read');
await until(() => (keys(VK.SHIFT) & 3) === 0, 'thread state after WM_KEYUP');
game._aoe_input_modifiers(0, 1, 0, 0);
eq(keys(0x11) & 3, 3, 'Ctrl held from another window: both states corrected without a message');
game._aoe_input_modifiers(0, 0, 0, 0);
eq(keys(0x11) & 3, 0, 'Ctrl released in another window: both states corrected');
// Text panels clear their never-locked image buffers to colour key 1 with PtrClear, which locks the
// DirectDraw surface for the fill as the original assembly does.
eq(game._aoe_test_ptrclear(37, 9), 37 * 9, 'PtrClear fills a never-locked DirectDraw area and unlocks it');

// 2. Multiplayer -> Multiplayer Connection screen with its name edit control.
await click(400, 310);
await until(() => edit() && game._aoe_window_state(edit(), 0) === 1, 'name edit control');
eq([2, 3, 4, 5].map(f => game._aoe_window_state(edit(), f)), [27, 127, 745, 18], 'Name edit control geometry');

// 3. Typing through WM_KEYDOWN -> TranslateMessage -> WM_CHAR into the edit control.
await click(400, 134);
await until(() => game._aoe_window_state(edit(), 1) === 1, 'edit focus');
await type('Ab C');
await until(() => text(edit()) === 'Ab C', 'typed text');
await press(VK.BACK);
await until(() => text(edit()) === 'Ab ', 'Backspace');
await press(VK.HOME);
await press(VK.DELETE);
await until(() => text(edit()) === 'b ', 'Home + Delete');
ok(true, 'Edit control editing keys');

// 4. Escape returns to the main menu (screen and its edit control destroyed).
await press(VK.ESCAPE);
await until(() => edit() === 0, 'Escape back to main menu');
ok(true, 'Escape leaves the connection screen');

// 5. Keyboard navigation: Down moves to Multiplayer, Enter activates it.
await press(VK.DOWN);
await press(VK.RETURN);
await until(() => edit() !== 0, 'keyboard-activated Multiplayer');
ok(true, 'Down + Enter opens Multiplayer');

// 6. Cancel button.
await click(562, 566);
await until(() => edit() === 0, 'Cancel back to main menu');
ok(true, 'Cancel leaves the connection screen');

// 7. Registry values persist through the file the host stores.
const registry = new TextDecoder().decode(game.FS.readFile('/aoe-registry.dat'));
ok(/GAME FILE NUMBER/.test(registry) && /CDPATH/.test(registry), 'Registry written to /aoe-registry.dat');

// 8. Exit ends WinMain normally.
await click(400, 497);
await until(() => exitCode !== null, 'WinMain to return after Exit', 8000);
eq(exitCode, 0, 'WinMain returns 0');
eq(log.filter(l => /Unsupported|DirectDraw: unsupported/.test(l)), [], 'No unported Windows calls during the menu flows');
eq(boxes, [], 'No message boxes');

const report = {passed: true, checks, frames, mainMenu: menu, wasmSha256: createHash('sha256').update(wasmBinary).digest('hex'),
  scope: 'Real WinMain: main menu, key state (thread vs physical), PtrClear on DirectDraw areas, Multiplayer Connection screen, edit control typing/editing, Escape, keyboard navigation, Cancel, Exit. Metric-only fonts; no connection types yet (Phase 4).'};
fs.writeFileSync(new URL('reports/native-menu-tests.json', root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
process.exit(0);
