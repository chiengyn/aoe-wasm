// Room launch from the web lobby (web/index.html): real-WinMain games go from the main menu straight to a
// room's game setup screen, created or joined through the Go server's relay, with no in-game Multiplayer
// menus. Checks the lobby API (/api/rooms, /api/rooms/check), passwords (wrong, right, public), the events
// the page gets (setup, failed, left), and that no password leaves the relay. Writes reports/native-rooms-tests.json.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {spawn, execFileSync} from 'node:child_process';
import {createHash} from 'node:crypto';
// AOE_BUILD=build-release tests the optimized build (engine/build.py --release).
const BUILD_DIR = process.env.AOE_BUILD || 'build';
const {default: createGame} = await import(`../engine/${BUILD_DIR}/game.mjs`);
import {metricFonts} from '../web/aoe-fonts.mjs';
import {attachDirectPlay} from '../web/native-dplay.mjs';
import {setRoom, ROOM_ERRORS} from '../web/native-room.mjs';

const root = new URL('../', import.meta.url);
const read = p => fs.readFileSync(new URL(p, root));
const DATA = process.env.AOE_DATA || 'game';
const native = JSON.parse(read(`web/${DATA}/native.json`));
const wasmBinary = read(`engine/${BUILD_DIR}/game.wasm`);
const port = Number(process.env.AOE_TEST_PORT || 8798);
const origin = `http://127.0.0.1:${port}`;
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
await new Promise((resolve, reject) => { server.stdout.once('data', resolve); server.once('exit', code => reject(new Error('server exited ' + code))); });

const games = [];
// Every frame the relay sends to the games, to check that passwords never leave it.
const received = [];
async function boot(tag, room) {
  const g = {tag, log: [], boxes: [], frames: 0, aborted: null, events: []};
  const media = {hasVideos: () => false, soundData() {}, soundDuplicate() {}, soundPlay() {}, soundUpdate() {}, soundStop() {}, soundRelease() {},
    volume() {}, midiPlay() {}, midiStop() {}, videoOpen: () => 0, videoPlay() {}, videoPause() {}, videoClose() {}};
  g.m = await createGame({
    wasmBinary, noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => g.frames++, aoeCursor() {}, aoeMedia: media,
    onMessageBox: (title, text) => g.boxes.push(title + ': ' + text),
    print: line => g.log.push(line), printErr: line => g.log.push(line), onAbort: reason => { g.aborted = String(reason); },
    preRun: [m => {
      for (const d of native.directories) m.FS.mkdirTree('/' + d);
      for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
    }],
  });
  attachDirectPlay(g.m, `ws://127.0.0.1:${port}/api/dplay`);
  const incoming = g.m._aoe_dp_incoming;
  g.m._aoe_dp_incoming = (ptr, len) => { received.push(Buffer.from(g.m.HEAPU8.subarray(ptr, ptr + len))); return incoming(ptr, len); };
  setRoom(g.m, room, (event, code) => g.events.push([event, code]));
  g.m.callMain(['NODXCHECK', 'NOSOUND', 'NOSTARTUP', '800']);
  g.state = f => g.m._aoe_test_state(f);
  g.click = async (x, y) => { g.m._aoe_input_mouse(0, 0, x, y); g.m._aoe_input_mouse(1, 0, x, y); await sleep(40); g.m._aoe_input_mouse(2, 0, x, y); await sleep(300); };
  games.push(g);
  return g;
}
async function until(test, what, ms = 20000) {
  const end = Date.now() + ms;
  while (Date.now() < end) {
    for (const g of games) if (g.aborted) throw new Error(`${g.tag} aborted: ${g.aborted}`);
    if (test()) return;
    await sleep(50);
  }
  throw new Error('Timed out waiting for ' + what);
}
const event = (g, name) => g.events.find(e => e[0] === name);
const rooms = async () => (await (await fetch(origin + '/api/rooms')).json()).rooms;
const check = async (id, password) => (await (await fetch(origin + '/api/rooms/check', {method: 'POST', headers: {'Content-Type': 'application/json', Origin: origin},
  body: JSON.stringify({id, password})})).json()).result;

// A host creates a locked room: straight to the setup screen as player 1, listed with its name and host.
const PASSWORD = 'sésame';
const host = await boot('host', {create: true, player: 'Livia', room: 'Forum', password: PASSWORD});
await until(() => event(host, 'setup') || event(host, 'failed'), 'the host in its room', 60000);
eq(host.events, [['setup', 0]], 'Host: the room launch reaches the game setup screen');
eq(host.state(5), 1, 'Host is player 1');
let list = await rooms();
eq(list.map(({name, host, players, locked, open}) => ({name, host, players, locked, open})),
  [{name: 'Forum', host: 'Livia', players: 1, locked: true, open: true}], 'The lobby lists the locked room by its own name and its host');
const forum = list[0].id;
eq([await check(forum, PASSWORD), await check(forum, 'nope'), await check('00', '')], ['ok', 'wrong-password', 'gone'], 'Password check before the game loads');

// A wrong password fails in the game too (the relay checks again).
const intruder = await boot('intruder', {player: 'Gaius', id: forum, password: 'nope'});
await until(() => intruder.events.length, 'the wrong password to fail', 60000);
eq(intruder.events.map(([e, code]) => [e, ROOM_ERRORS[code]]), [['failed', 'wrong-password']], 'Wrong password: failed with DPERR_INVALIDPASSWORD');

// The right password joins as player 2.
const guest = await boot('guest', {player: 'Titus', id: forum, password: PASSWORD});
await until(() => guest.events.length, 'the guest in the room', 60000);
eq(guest.events, [['setup', 0]], 'Guest: the room launch reaches the game setup screen');
await until(() => guest.state(5) === 2, 'the guest as player 2');
ok(true, 'Guest joined as player 2');
eq((await rooms())[0].players, 2, 'The room shows 2 players');

// A public room: no password needed.
const host2 = await boot('host2', {create: true, player: 'Marcus', room: 'Open field'});
await until(() => host2.events.length, 'the public room', 60000);
const field = (await rooms()).find(r => r.name === 'Open field');
ok(field && !field.locked && field.host === 'Marcus', 'The public room is listed without a lock');
eq(await check(field.id, ''), 'ok', 'A public room needs no password');
const visitor = await boot('visitor', {player: 'Julia', id: field.id});
await until(() => visitor.events.length, 'the visitor in the public room', 60000);
eq(visitor.events, [['setup', 0]], 'Visitor joined the public room');
await until(() => visitor.state(5) === 2, 'the visitor as player 2');

// Leaving the room (a guest's Cancel: 330,440 240x30 in the 640x480 layout, at 800x600) reports 'left';
// the room shows one player again.
await sleep(1000);
await visitor.click(562, 569);
await until(() => event(visitor, 'left'), 'the visitor to leave');
ok(true, 'Leaving the room reports left');
await sleep(500);
eq((await rooms()).find(r => r.id === field.id).players, 1, 'The room shows 1 player after the visitor left');

// The password never reached any game: not in session lists, open replies or description updates.
const secret = Buffer.from([0x73, 0xe9, 0x73, 0x61, 0x6d, 0x65]); // "sésame" in Windows-1252
ok(received.length > 0 && !received.some(frame => frame.includes(secret)), `None of the ${received.length} relay frames carries the password`);

eq(games.flatMap(g => g.boxes), [], 'No message boxes');
eq(games.flatMap(g => g.log).filter(l => /Unsupported|unsupported/.test(l)), [], 'No unported calls');
const report = {
  passed: true, checks, rooms: await rooms(), events: Object.fromEntries(games.map(g => [g.tag, g.events])), relayFrames: received.length,
  wasmSha256: createHash('sha256').update(wasmBinary).digest('hex'), edition: DATA,
  scope: 'Room launch: real-WinMain games created and joined rooms from the main menu (no in-game Multiplayer menus): locked room with wrong/right password, public room, leaving; /api/rooms and /api/rooms/check; passwords never sent by the relay.',
};
fs.mkdirSync(new URL('reports/', root), {recursive: true});
fs.writeFileSync(new URL('reports/native-rooms-tests.json', root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
process.exit(0);
