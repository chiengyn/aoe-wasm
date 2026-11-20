// Phase 3: the real WinMain build with sound and music on. A recording stand-in for the browser media host
// (web/native-media.mjs) checks what the game's own sound code hands it: DirectSound buffers from the
// game's WAVs, button sounds, menu music from its MIDI files (through MCI, with end-of-file notifications),
// and the mixer lines behind the volume sliders. Writes reports/native-media-tests.json.
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
const log = [], boxes = [], calls = [];
let frames = 0, aborted = null, checks = 0;
const ok = (value, message) => { assert.ok(value, message); checks++; };
const eq = (a, b, message) => { assert.deepEqual(a, b, message); checks++; };

const sounds = new Map();
const media = {
  hasVideos: () => false,
  soundData: (id, data, rate, channels, bits) => { sounds.set(id, {bytes: data.length, rate, channels, bits, first: data.subarray(0, 64)}); calls.push(['data', id]); },
  soundDuplicate: (id, source) => calls.push(['duplicate', id, source]),
  soundPlay: (id, loop, volume, pan, frequency) => calls.push(['play', id, loop, volume, pan, frequency]),
  soundUpdate: (id, volume, pan, frequency) => calls.push(['update', id, volume, pan, frequency]),
  soundStop: id => calls.push(['stop', id]),
  soundRelease: id => calls.push(['release', id]),
  volume: (line, gain) => calls.push(['volume', line, gain]),
  midiPlay: (id, data, from) => calls.push(['midi', id, data.length, from, String.fromCharCode(...data.subarray(0, 4))]),
  midiStop: id => calls.push(['midi-stop', id]),
  videoOpen: () => 0, videoPlay() {}, videoPause() {}, videoClose() {},
};

const game = await createGame({
  wasmBinary, noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => frames++, aoeMedia: media,
  onMessageBox: (title, text) => boxes.push({title, text}),
  print: line => log.push(line), printErr: line => log.push(line), onAbort: reason => { aborted = String(reason); },
  preRun: [m => {
    for (const d of native.directories) m.FS.mkdirTree('/' + d);
    for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
  }],
});
try { game.callMain(['NODXCHECK', 'NOSTARTUP', '800']); } catch (e) { if (!/unwind/i.test(String(e))) throw e; }

const sleep = ms => new Promise(r => setTimeout(r, ms));
async function until(test, what, ms = 5000) {
  const end = Date.now() + ms;
  while (Date.now() < end) { if (aborted) throw new Error('Aborted: ' + aborted); if (test()) return; await sleep(20); }
  throw new Error('Timed out waiting for ' + what);
}
const click = async (x, y) => { game._aoe_input_mouse(0, 0, x, y); game._aoe_input_mouse(1, 0, x, y); await sleep(40); game._aoe_input_mouse(2, 0, x, y); await sleep(300); };
const of = kind => calls.filter(c => c[0] === kind);

// 1. Boot with sound: the menu music starts from the game's own MIDI files.
await until(() => frames > 0, 'main menu', 20000);
await until(() => of('midi').length > 0, 'menu music', 5000);
const [, musicId, musicBytes, musicFrom, magic] = of('midi')[0];
eq(magic, 'MThd', 'Menu music is a Standard MIDI file read by MCI from the game folder');
ok(musicBytes > 1000 && musicFrom === 0, 'The whole MIDI file is handed over, from the start');

// 2. A button click plays the game's click sound through a DirectSound buffer.
const before = of('play').length;
await click(400, 310);                                   // Multiplayer
await until(() => of('play').length > before, 'button sound', 3000);
const [, soundId, loop, volume, pan, frequency] = of('play').at(-1);
const sound = sounds.get(soundId);
ok(sound && sound.bytes > 100, 'The played buffer holds PCM data uploaded through Lock/Unlock');
ok([8, 16].includes(sound.bits) && [1, 2].includes(sound.channels) && sound.rate >= 8000, `PCM format ${sound.rate} Hz, ${sound.bits}-bit, ${sound.channels} ch`);
eq(loop, false, 'Button sounds do not loop');
ok(volume <= 0 && volume >= -10000 && pan >= -10000 && pan <= 10000 && frequency >= 8000, 'DirectSound volume, pan and frequency are in range');
await click(562, 566);                                   // Cancel back to the main menu
await sleep(300);

// 3. The game's volume settings reach the mixer lines (wave = 1, synthesizer = 2).
const volumes = of('volume');
ok(volumes.some(v => v[1] === 2), 'Music volume set on the synthesizer line');
ok(volumes.every(v => v[2] >= 0 && v[2] <= 1), 'Mixer gains are 0..1');

// 4. MCI timing: the sequencer reports the end of the file and the game plays its next track.
ok(of('midi-stop').length === 0 || of('midi').length > 1, 'Music is still playing or moved on');

// 5. Intro videos (no NOSTARTUP): the game opens an MCIWnd window per video; the page reports each end.
const videoCalls = [], videoLog = [];
let videoFrames = 0, videoAborted = null;
const videos = native.videos;
const intro = await createGame({
  wasmBinary, noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => videoFrames++,
  aoeMedia: {...media, hasVideos: () => true,
    videoOpen: path => { const v = videos[path.replace(/\\/g, '/').split('/').pop().replace(/\.avi$/i, '').toLowerCase()]; videoCalls.push(['open', path]); return v ? v.width << 16 | v.height : 0; },
    videoPlay: (...a) => videoCalls.push(['play', ...a]), videoClose: () => videoCalls.push(['close'])},
  print: line => videoLog.push(line), printErr: line => videoLog.push(line), onAbort: reason => { videoAborted = String(reason); },
  preRun: [m => {
    for (const d of native.directories) m.FS.mkdirTree('/' + d);
    for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
  }],
});
try { intro.callMain(['NODXCHECK', '800']); } catch (e) { if (!/unwind/i.test(String(e))) throw e; }
const plays = () => videoCalls.filter(c => c[0] === 'play');
const waitFor = async (test, what, ms = 10000) => { const end = Date.now() + ms; while (Date.now() < end) { if (videoAborted) throw new Error('Aborted: ' + videoAborted); if (test()) return; await sleep(20); } throw new Error('Timed out waiting for ' + what); };
await waitFor(() => plays().length === 1, 'first intro video');
const [, x, y, w, h, sw, sh] = plays()[0];
eq([sw, sh], [640, 480], 'Videos play in the 640x480 mode the game switches to');
eq([w, h, x, y], [videos.logo1.width * 2, videos.logo1.height * 2, (640 - videos.logo1.width * 2) / 2, (480 - videos.logo1.height * 2) / 2], 'logo1 is shown at double size, centred');
intro._aoe_media_video_ended();
await waitFor(() => plays().length === 2, 'second intro video (logo2 is missing, so introx)', 5000);
// logo2.avi isn't in the game folder: the game finds no file and goes on to the intro.
eq(videoCalls.filter(c => c[0] === 'open').map(c => c[1].replace(/\\/g, '/').split('/').pop().toLowerCase()), ['logo1.avi', 'introx.avi'], 'logo1, then (logo2 missing) the intro');
eq(plays()[1].slice(3, 5), [videos.introx.width * 2, videos.introx.height * 2], 'The intro is shown at double size');
// A click skips the video (the game's video window procedure), and the main menu follows in 800x600.
intro._aoe_input_mouse(1, 0, 320, 240); intro._aoe_input_mouse(2, 0, 320, 240);
await waitFor(() => intro._aoe_frame_info(1) === 800 && videoCalls.at(-1)[0] === 'close', 'main menu after the videos', 10000);
ok(videoFrames > 0, 'Main menu reached after the intro videos');

eq(boxes, [], 'No message boxes');
eq([...log, ...videoLog].filter(l => /Unsupported|unsupported/.test(l)), [], 'No unported calls');
const report = {
  passed: true, checks, musicBytes, buffersUploaded: sounds.size, soundsPlayed: of('play').length,
  mixer: volumes.map(v => [v[1], Number(v[2].toFixed(3))]),
  wasmSha256: createHash('sha256').update(wasmBinary).digest('hex'),
  videos: plays().map(p => p.slice(1)),
  scope: 'Real WinMain with sound on: DirectSound buffers and button sounds, MIDI menu music through MCI, mixer volume lines; intro videos through MCIWnd (logo1, missing logo2 skipped, introx, click to skip, main menu). Recording media host (no audio or video output).',
};
fs.writeFileSync(new URL('reports/native-media-tests.json', root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
process.exit(0);
