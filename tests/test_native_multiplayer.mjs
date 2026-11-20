// Phase 4: two real-WinMain games play multiplayer through the game's own DirectPlay networking, relayed by
// the Go server (server/, DirectPlay over WebSocket). Host: Multiplayer -> Browser connection -> Create; guest:
// Show Games -> Join; both Ready; host Start Game. Then the two simulations must stay identical while the
// host trains a villager. Writes reports/native-multiplayer-tests.json and frame PNGs.
import fs from 'node:fs';
import zlib from 'node:zlib';
import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
// AOE_BUILD=build-release tests the optimized build (engine/build.py --release).
const BUILD_DIR = process.env.AOE_BUILD || 'build';
const {default: createGame} = await import(`../engine/${BUILD_DIR}/game.mjs`);
import {metricFonts} from '../web/aoe-fonts.mjs';
import {attachDirectPlay} from '../web/native-dplay.mjs';

const root = new URL('../', import.meta.url);
const read = p => fs.readFileSync(new URL(p, root));
// AOE_DATA: the prepared edition under web/ (tools/prepare_native.py --out), 'game' by default.
const DATA = process.env.AOE_DATA || 'game';
const native = JSON.parse(read(`web/${DATA}/native.json`));
const wasmBinary = read(`engine/${BUILD_DIR}/game.wasm`);
const port = Number(process.env.AOE_TEST_PORT || 8797);
const PLAY_SECONDS = Number(process.env.AOE_PLAY_SECONDS || 40);
const PLAYERS = Math.max(2, Math.min(8, Number(process.env.AOE_PLAYERS || 2)));
let checks = 0;
const ok = (value, message) => { assert.ok(value, message); checks++; };
const eq = (a, b, message) => { assert.deepEqual(a, b, message); checks++; };
const sleep = ms => new Promise(r => setTimeout(r, ms));

// AOE_SERVER: a prebuilt server binary. Otherwise server/ is built first (GO: the go command to use).
let serverBin = process.env.AOE_SERVER;
if (!serverBin) {
  serverBin = new URL('server/build/aoe-server', root).pathname;
  execFileSync(process.env.GO || 'go', ['build', '-o', serverBin, '.'], {cwd: new URL('server/', root).pathname, stdio: 'inherit'});
}
const server = spawn(serverBin, ['-port', String(port), '-web', new URL('web', root).pathname], {stdio: ['ignore', 'pipe', 'inherit']});
process.on('exit', () => server.kill());
await new Promise((resolve, reject) => { server.stdout.once('data', resolve); server.once('exit', code => reject(new Error('relay exited ' + code))); });

async function boot(tag, sound = false) {
  const g = {tag, log: [], boxes: [], frames: 0, aborted: null, cursor: '', sounds: new Map(), played: []};
  // The host runs with sound to check taunts; a recording media host stands in for Web Audio.
  const media = {hasVideos: () => false, soundData: (id, data) => g.sounds.set(id, data), soundDuplicate: (id, src) => g.sounds.set(id, g.sounds.get(src)),
    soundPlay: id => g.played.push(id), soundUpdate() {}, soundStop() {}, soundRelease() {}, volume() {}, midiPlay() {}, midiStop() {},
    videoOpen: () => 0, videoPlay() {}, videoPause() {}, videoClose() {}};
  g.m = await createGame({
    wasmBinary, noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => g.frames++, aoeCursor: css => { g.cursor = css; }, aoeMedia: media,
    onMessageBox: (title, text) => g.boxes.push(title + ': ' + text),
    print: line => g.log.push(line), printErr: line => g.log.push(line), onAbort: reason => { g.aborted = String(reason); },
    preRun: [m => {
      for (const d of native.directories) m.FS.mkdirTree('/' + d);
      for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
    }],
  });
  attachDirectPlay(g.m, `ws://127.0.0.1:${port}/api/dplay`);
  g.m.callMain(sound ? ['NODXCHECK', 'NOSTARTUP', '800'] : ['NODXCHECK', 'NOSOUND', 'NOSTARTUP', '800']);
  g.state = f => g.m._aoe_test_state(f);
  g.player = (p, a) => g.m._aoe_test_player(p, a);
  g.click = async (x, y) => { g.m._aoe_input_mouse(0, 0, x, y); g.m._aoe_input_mouse(1, 0, x, y); await sleep(40); g.m._aoe_input_mouse(2, 0, x, y); await sleep(300); };
  g.key = async (vk, ch = 0) => { g.m._aoe_input_key(vk, 0, 1, 0, ch); g.m._aoe_input_key(vk, 0, 0, 0, 0); await sleep(60); };
  g.type = async s => { for (const c of s) await g.key(c === ' ' ? 0x20 : c.toUpperCase().charCodeAt(0), c.charCodeAt(0)); };
  g.edit = n => g.m.ccall('aoe_find_window', 'number', ['string', 'number'], ['edit', n]);
  g.text = h => g.m.ccall('aoe_window_text', 'string', ['number'], [h]);
  g.png = name => {
    const m = g.m, [bits, w, h, pitch, pal] = [0, 1, 2, 3, 4].map(f => m._aoe_frame_info(f));
    const raw = Buffer.alloc((w * 3 + 1) * h);
    for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) { const i = m.HEAPU8[bits + y * pitch + x], o = y * (w * 3 + 1) + 1 + x * 3; for (let c = 0; c < 3; c++) raw[o + c] = m.HEAPU8[pal + i * 4 + c]; }
    const chunk = (t, d) => { const l = Buffer.alloc(4); l.writeUInt32BE(d.length); const td = Buffer.concat([Buffer.from(t), d]); const c = Buffer.alloc(4); c.writeUInt32BE(zlib.crc32(td) >>> 0); return Buffer.concat([l, td, c]); };
    const ih = Buffer.alloc(13); ih.writeUInt32BE(w, 0); ih.writeUInt32BE(h, 4); ih[8] = 8; ih[9] = 2;
    fs.writeFileSync(new URL(`reports/${name}.png`, root), Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ih), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  };
  return g;
}
const games = [];
async function until(test, what, ms = 10000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    for (const g of games) if (g.aborted) throw new Error(`${g.tag} aborted: ${g.aborted}`);
    if (test()) return;
    await sleep(50);
  }
  throw new Error('Timed out waiting for ' + what);
}

const host = await boot('host', true);
// Widescreen: the host's page is 16:9, the guests' 4:3 (no aspect). Menus stay 800x600; the host's game screen
// widens to 1064x600. The view size is local and must not affect the lockstep simulation.
host.m._aoe_set_display_aspect(16, 9);
const guests = [];
for (let i = 1; i < PLAYERS; i++) guests.push(await boot('guest' + i));
const guest = guests[0];
games.push(host, ...guests);
await until(() => games.every(g => g.frames > 0), 'main menus', 60000);

// Multiplayer Connection: name, the "Browser" connection type (from DirectPlayEnumerateA), OK.
async function connect(g, name) {
  await g.click(400, 310);
  await until(() => g.edit(0), `${g.tag} connection screen`);
  await g.click(400, 134);
  await g.type(name);
  await g.click(225, 247);
  await g.click(235, 566);
  await until(() => g.state(0) !== -1 && !g.edit(0), `${g.tag} Multiplayer Games screen`);
}
await connect(host, 'Host');
await host.click(399, 566);                                       // Create
await until(() => host.edit(0), 'Create Game dialog');
await host.click(399, 304);
await host.type('Relay Test');
await host.click(300, 378);                                       // OK: DirectPlay Open (create) + CreatePlayer
await until(() => host.edit(0) && host.state(5) === 1, 'host lobby');   // lobby chat edit, comm player 1
ok(true, 'Host created a session through DirectPlay Open/CreatePlayer');
host.png('native-mp-host-lobby');
eq([host.m._aoe_frame_info(1), host.m._aoe_frame_info(2)], [800, 600], 'Widescreen host: the lobby keeps the 4:3 menu size');

for (const [i, g] of guests.entries()) {
  await connect(g, 'Guest' + (i + 1));
  await g.click(399, 478);                                        // Show Games: EnumSessions
  await sleep(500);
  await g.click(136, 566);                                        // Join: DirectPlay Open (join) + CreatePlayer
  await until(() => g.state(5) === i + 2, `${g.tag} joined as player ${i + 2}`);
  ok(true, `${g.tag} found the session (EnumSessions) and joined as player ${i + 2}`);
  await sleep(1000);
}
guest.png('native-mp-guest-lobby');

// Lobby chat goes through DirectPlay Send.
await guest.click(258, 522);
await guest.type('hello from guest');
await guest.key(0x0d, 13);
await sleep(800);

// Taunts: a chat message that is a number plays sound/Taunt<NNN>.wav on every machine (TTaunt). The
// 333HD edition adds taunts 26-50 as files only.
const wavData = path => { const b = read(`web/${DATA}/native/` + path); let o = 12; while (o + 8 <= b.length) { const size = b.readUInt32LE(o + 4); if (b.toString('latin1', o, o + 4) === 'data') return b.subarray(o + 8, o + 8 + size); o += 8 + size + (size & 1); } return null; };
const taunts = ['1', '26'].filter(n => native.files.some(f => f.path.toLowerCase() === `sound/taunt${n.padStart(3, '0')}.wav`));
for (const n of taunts) {
  const expected = wavData(native.files.find(f => f.path.toLowerCase() === `sound/taunt${n.padStart(3, '0')}.wav`).path);
  const before = host.played.length;
  await guest.click(258, 522); await guest.type(n); await guest.key(0x0d, 13);
  await until(() => host.played.length > before, `taunt ${n} on the host`, 5000);
  const pcm = host.sounds.get(host.played.at(-1));
  ok(pcm && expected && Buffer.compare(Buffer.from(pcm.subarray(0, 256)), expected.subarray(0, 256)) === 0, `Chat "${n}" plays Taunt${n.padStart(3, '0')}.wav on the host`);
  await sleep(500);
}
await sleep(800);

for (const g of guests) { await g.click(134, 578); await sleep(600); }   // I'm Ready! (guests)
await host.click(23, 578);                                        // I'm Ready! (host)
await sleep(800);
await host.click(367, 578);                                       // Start Game
await until(() => games.every(g => g.state(0) === 4), 'all games in progress', 90000);
ok(true, `Host started the game; all ${PLAYERS} machines reached ProgModeGame`);
await until(() => host.m._aoe_frame_info(1) === 1064, 'widescreen host game screen', 10000);
eq([host.m._aoe_frame_info(1), host.m._aoe_frame_info(2)], [1064, 600], 'Widescreen host: the game screen widens to 16:9 at the menu height');
eq(guests.map(g => [g.m._aoe_frame_info(1), g.m._aoe_frame_info(2)]), guests.map(() => [800, 600]), 'Guests without an aspect keep the 4:3 game screen');

// Cursor: menus and lobby use the Windows arrow; in the match the game hides it and draws its own pointer.
eq(guest.cursor, 'none', 'In the match the Windows cursor is hidden (the game draws its pointer)');
{
  const region = () => { const [bits, , , pitch] = [0, 1, 2, 3].map(f => host.m._aoe_frame_info(f)); let s = ''; for (let y = 200; y < 230; y++) for (let x = 300; x < 330; x++) s += host.m.HEAPU8[bits + y * pitch + x] + ','; return s; };
  host.m._aoe_input_mouse(0, 0, 600, 100); await sleep(400); const away = region();
  host.m._aoe_input_mouse(0, 0, 305, 205); await sleep(400); const here = region();
  ok(away !== here, "The game's own pointer is drawn at the mouse position in the match");
}
// Edge scrolling measures against the game window (GetWindowRect), which must grow with the display mode:
// on the 1064-wide host screen, x=900 (past the 800-wide menu screen) must not scroll; the real edge must.
{
  const view = () => [host.m._aoe_test_minimap(0), host.m._aoe_test_minimap(1)];
  host.m._aoe_input_mouse(0, 0, 900, 300); await sleep(300); const start = view(); await sleep(1000);
  eq(view(), start, 'Widescreen host: no edge scrolling short of the right edge (x=900 of 1064)');
  host.m._aoe_input_mouse(0, 0, 1063, 300); await sleep(1000); const scrolled = view();
  host.m._aoe_input_mouse(0, 0, 532, 300);
  ok(scrolled.some((v, i) => v !== start[i]), 'Widescreen host: the right edge of the screen scrolls the map');
  await host.key(0x48, 'h'.charCodeAt(0));
}
// Unexplored map is black: scroll the guest's view away from its base (arrow keys), then back (H).
// The view keeps its terrain in a scrolling buffer and must clear the newly exposed strips.
{
  const lit = () => { const [bits, w, , pitch, pal] = [0, 1, 2, 3, 4].map(f => guest.m._aoe_frame_info(f)); let n = 0;
    for (let y = 40; y < 440; y += 2) for (let x = 0; x < w; x += 2) { const i = guest.m.HEAPU8[bits + y * pitch + x]; if (guest.m.HEAPU8[pal + i * 4] + guest.m.HEAPU8[pal + i * 4 + 1] + guest.m.HEAPU8[pal + i * 4 + 2] > 30) n++; }
    return n; };
  const hold = async (vk, ms) => { guest.m._aoe_input_key(vk, 0, 1, 1, 0); await sleep(ms); guest.m._aoe_input_key(vk, 0, 0, 1, 0); await sleep(400); };
  const home = lit();
  await hold(0x25, 3000); await hold(0x26, 2500);
  const away = lit();
  ok(home > 20000 && away < home / 20, `Unexplored map is black after scrolling away (lit samples ${home} -> ${away})`);
  await guest.key(0x48, 'h'.charCodeAt(0)); await sleep(800);
  ok(lit() > home / 2, 'H returns the view to the base');
}
// Double-click a villager: selects every villager of the player shown on screen (TRIBE_Main_View::
// mouse_left_dbl_click_action). The browser gives no click count; the Win32 layer makes WM_LBUTTONDBLCLK
// from two presses within the double-click time and rectangle, as Windows does for CS_DBLCLKS classes.
{
  const shown = [];
  for (let n = 0; ; n++) { const x = guest.m._aoe_test_displayed(n, 0); if (x < 0) break; shown.push({x, y: guest.m._aoe_test_displayed(n, 1), id: guest.m._aoe_test_displayed(n, 2)}); }
  const villagers = shown.filter(o => o.id === 83);
  ok(villagers.length >= 2, `${villagers.length} villagers on screen`);
  const {x, y} = villagers[0], press = async () => { guest.m._aoe_input_mouse(1, 0, x, y); await sleep(20); guest.m._aoe_input_mouse(2, 0, x, y); };
  guest.m._aoe_input_mouse(0, 0, x, y);
  await press(); await sleep(700); await press(); await sleep(500);
  eq([guest.m._aoe_test_selection(0), guest.m._aoe_test_selection(1)], [1, 83], 'Two clicks 700 ms apart select one villager');
  await sleep(700);
  await press(); await sleep(60); await press();
  await until(() => guest.m._aoe_test_selection(2) === villagers.length, 'double-click selection', 5000);
  ok(true, `Double-click selects all ${villagers.length} villagers on screen`);
  // As in the retail EXE (0x50C12A, select_one_object at 0x45EB70): the clicked villager is added before,
  // during and after the loop, and select_one_object does not skip objects already selected.
  eq(guest.m._aoe_test_selection(0), Math.min(25, villagers.length + 2), 'Selection count as the retail game keeps it');
  await guest.key(0x48, 'h'.charCodeAt(0));
}
const players = host.state(4);
eq(games.map(g => g.state(4)), games.map(() => players), 'Same number of world players');
eq(new Set(games.map(g => g.state(6))).size, PLAYERS, 'Each machine controls its own player');
const hostPlayer = host.state(6);

// Same simulation on both machines: resources and object counts of every player agree.
const snapshot = g => Array.from({length: players}, (_, p) => [0, 1, 2, 3].map(a => g.player(p, a)).concat(g.player(p, -1)));
// Wait until the simulation is running on every machine (not just the game screen shown).
await until(() => games.every(g => g.state(1) > 3000), 'world time running', 30000);
const food0 = host.player(hostPlayer, 0);
await host.key(0x48, 'h'.charCodeAt(0));                          // H: select the Town Center
await host.key(0x43, 'c'.charCodeAt(0));                          // C: train a villager
await until(() => host.player(hostPlayer, 0) === food0 - 50000, 'villager paid for (50 food)', 10000);
ok(true, 'Host trained a villager through the original command path (food -50)');
await until(() => guests.every(g => g.player(hostPlayer, 0) === food0 - 50000), 'guest simulations see the host spending food', 10000);
ok(true, "Every guest's simulation applied the host's command");

let compared = 0, turn = 0;
const heap0 = games.map(g => g.m.HEAPU8.length);
const before = snapshot(host);
const end = Date.now() + PLAY_SECONDS * 1000;
while (Date.now() < end) {
  await sleep(2000);
  for (const g of games) if (g.aborted) throw new Error(`${g.tag} aborted: ${g.aborted}`);
  // Players take turns giving commands (select the Town Center, train a villager while food lasts).
  const actor = games[turn++ % games.length];
  await actor.key(0x48, 'h'.charCodeAt(0));
  await actor.key(0x43, 'c'.charCodeAt(0));
  eq(games.map(g => g.state(3)), games.map(() => 0), 'No out-of-sync detected by the game');
  // Lockstep: compare machines at the same world time. The simulation only changes when world time
  // advances, so a snapshot taken while a machine shows time T is its state at T. Machines may be a step
  // apart at any instant; record each machine's state per time and compare the times they share.
  const seen = new Map(games.map(g => [g, new Map()]));
  for (let tries = 0; tries < 150; tries++) {
    for (const g of games) { const t = g.state(1); if (!seen.get(g).has(t)) seen.get(g).set(t, snapshot(g)); }
    await sleep(10);
  }
  for (const g of guests) {
    const shared = [...seen.get(g).keys()].filter(t => seen.get(host).has(t));
    if (shared.length) { const t = shared.at(-1); eq(seen.get(g).get(t), seen.get(host).get(t), `${g.tag} identical to host at time ${t}`); compared++; }
  }
}
ok(compared >= 3, `Several lockstep comparisons at equal world times (${compared}; world times ${games.map(g => g.state(1)).join(', ')})`);
ok(turn >= games.length, 'Every player had a turn to give commands');
// Each player's own commands spent food on every machine (a villager costs 50 food).
const after = snapshot(host);
eq(games.map(g => after[g.state(6)][0] < before[g.state(6)][0]), games.map(() => true), 'Every player trained a villager through its own commands');
const heapGrowth = games.map((g, i) => g.m.HEAPU8.length - heap0[i]);
ok(games.every(g => g.state(1) > 10000), 'All worlds advanced');
const times = games.map(g => g.state(1));
ok(Math.max(...times) - Math.min(...times) < 3000, 'World clocks stay within the lockstep window');
host.png('native-mp-host-game');
guest.png('native-mp-guest-game');

// A player drops (its connection closes): the relay destroys its DirectPlay player, the others continue.
const dropped = guests[guests.length - 1];
dropped.m.aoeDirectPlay.close();
dropped.m._aoe_dp_disconnected();
games.splice(games.indexOf(dropped), 1);
if (games.length === 1) {
  // The host's last opponent left: the original game ends the match (the host wins).
  await until(() => host.state(0) === 5, 'game over after the only opponent left', 15000);
  ok(true, 'The match ends for the remaining player when the only opponent drops');
} else {
  const before = host.state(1);
  await sleep(8000);
  ok(host.state(1) > before + 3000, 'The remaining games keep running after a player drops');
  eq(games.map(g => g.state(3)), games.map(() => 0), 'Still no out-of-sync after the drop');
  eq(games.map(g => g.state(0)), games.map(() => 4), 'Remaining machines are still in the game');
}

eq(games.flatMap(g => g.boxes), [], 'No message boxes');
eq(games.flatMap(g => g.log).filter(l => /Unsupported|unsupported/.test(l)), [], 'No unported calls');

// Relay traffic of the whole run (lobby + match), for sizing the server.
const relayStats = await (await fetch(`http://127.0.0.1:${port}/healthz`)).json();
const report = {
  passed: true, players: PLAYERS, relay: relayStats, checks, lockstepComparisons: compared, worldTime: games.map(g => g.state(1)),
  hostPlayer: snapshot(host)[hostPlayer], frames: games.map(g => g.frames), playSeconds: PLAY_SECONDS, commandTurns: turn,
  heapGrowthBytes: heapGrowth,
  wasmSha256: createHash('sha256').update(wasmBinary).digest('hex'),
  edition: DATA, taunts,
  scope: `${PLAYERS} real-WinMain games: Browser DirectPlay provider, relay, Multiplayer Games (create/list/join), lobby (chat, ready, start), in-game lockstep with commands from every player (host widescreen, guests 4:3), no out-of-sync, a dropped player. Metric-only fonts.`,
};
fs.writeFileSync(new URL('reports/native-multiplayer-tests.json', root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
process.exit(0);
