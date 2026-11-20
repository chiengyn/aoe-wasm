// Phase 0 harness: run the real WinMain (engine/build/game.wasm) headlessly.
// Passes when WinMain starts and the run ends either normally or at a named unported Windows call.
// The first unported call is recorded in reports/native-boot.json as the next Phase 1 target.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {createHash} from 'node:crypto';
import zlib from 'node:zlib';
// AOE_BUILD=build-release tests the optimized build (engine/build.py --release).
const BUILD_DIR = process.env.AOE_BUILD || 'build';
const {default: createGame} = await import(`../engine/${BUILD_DIR}/game.mjs`);
import {metricFonts} from '../web/aoe-fonts.mjs';

const root = new URL('../', import.meta.url);
const read = p => fs.readFileSync(new URL(p, root));
const json = p => JSON.parse(read(p));
const timeoutMs = Number(process.env.AOE_BOOT_TIMEOUT_MS || 20000);
const wasmBinary = read(`engine/${BUILD_DIR}/game.wasm`);

const log = [];
let frames = 0;
const messageBoxes = [];
let finish;
const done = new Promise(resolve => { finish = resolve; });
const timer = setTimeout(() => finish({kind: 'timeout'}), timeoutMs);

// Mirror of the supplied game folder (tools/prepare_native.py).
const native = json('web/game/native.json');
const files = native.files;
const commandLine = (process.env.AOE_CMDLINE || 'NODXCHECK NOSOUND NOSTARTUP').split(/\s+/).filter(Boolean);

const module = await createGame({
  wasmBinary,
  noInitialRun: true,
  aoeFonts: metricFonts(),
  aoePresent: () => { frames++; },
  onMessageBox: (title, text) => messageBoxes.push({title, text}),
  print: line => log.push(line),
  printErr: line => log.push(line),
  preRun: [m => {
    // Lower-case paths: the platform layer normalizes Windows paths to lower case.
    for (const dir of native.directories) m.FS.mkdirTree('/' + dir);
    for (const f of files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
  }],
  onGameExit: code => finish({kind: 'exit', code}),
  onExit: code => finish({kind: 'exit', code}),
  onAbort: reason => finish({kind: 'abort', reason: String(reason)}),
}).catch(error => { finish({kind: 'abort', reason: String(error && error.message || error)}); return null; });

// Asyncify self-test of the yielding message queue, before the real WinMain runs.
const loopSelfTest = module ? await module.ccall('aoe_loop_selftest', 'number', [], [], {async: true}) : -1;
if (module) {
  try { module.callMain([...commandLine]); } catch (error) { if (!/unwind/i.test(String(error))) finish({kind: 'abort', reason: String(error)}); }
}
const end = await done;
clearTimeout(timer);

// Save the last presented frame (8-bit indices + palette) as PNG.
function savePng(file) {
  if (!module || !module._aoe_frame_info) return null;
  const [bits, w, h, pitch, pal] = [0, 1, 2, 3, 4].map(f => module._aoe_frame_info(f));
  if (!bits || !w || !h) return null;
  const raw = Buffer.alloc((w * 3 + 1) * h);
  for (let y = 0; y < h; y++) for (let x = 0; x < w; x++) {
    const i = module.HEAPU8[bits + y * pitch + x], o = y * (w * 3 + 1) + 1 + x * 3;
    raw[o] = pal ? module.HEAPU8[pal + i * 4] : i; raw[o + 1] = pal ? module.HEAPU8[pal + i * 4 + 1] : i; raw[o + 2] = pal ? module.HEAPU8[pal + i * 4 + 2] : i;
  }
  const chunk = (type, data) => { const len = Buffer.alloc(4); len.writeUInt32BE(data.length); const td = Buffer.concat([Buffer.from(type), data]); const crc = Buffer.alloc(4); crc.writeUInt32BE(zlib.crc32(td) >>> 0); return Buffer.concat([len, td, crc]); };
  const ihdr = Buffer.alloc(13); ihdr.writeUInt32BE(w, 0); ihdr.writeUInt32BE(h, 4); ihdr[8] = 8; ihdr[9] = 2;
  fs.writeFileSync(file, Buffer.concat([Buffer.from([137, 80, 78, 71, 13, 10, 26, 10]), chunk('IHDR', ihdr), chunk('IDAT', zlib.deflateSync(raw)), chunk('IEND', Buffer.alloc(0))]));
  return {width: w, height: h};
}
const frame = savePng(new URL('reports/native-boot.png', root));

const started = log.some(line => line.startsWith('[aoe] WinMain starting'));
const unported = /Unsupported in engine diagnostic: (\w+)/.exec(end.reason || '')?.[1] ?? null;
const report = {
  // Running means the original loop kept going (no abort) and presented frames until the timeout.
  passed: loopSelfTest === 0x1f && started && (end.kind === 'exit' || unported !== null || (end.kind === 'timeout' && frames > 0)),
  loopSelfTest: {mask: loopSelfTest, expected: 0x1f, checks: ['empty queue yields then FALSE', 'PM_NOREMOVE', 'hwnd/range filter + params', 'GetMessageA', 'WM_QUIT ends GetMessageA']},
  winMainStarted: started,
  end: end.kind,
  exitCode: end.code ?? null,
  firstUnportedCall: unported,
  abortReason: end.kind === 'abort' ? end.reason.slice(0, 500) : null,
  framesPresented: frames,
  frame,
  messageBoxes,
  logTail: log.slice(-20),
  commandLine: commandLine.join(' '),
  files: files.length,
  wasmSha256: createHash('sha256').update(wasmBinary).digest('hex'),
  wasmBytes: wasmBinary.length,
  scope: 'Real WinMain under Asyncify with the mirrored game folder; no input, presentation or audio yet.',
};
fs.mkdirSync(new URL('reports/', root), {recursive: true});
fs.writeFileSync(new URL('reports/native-boot.json', root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify(report, null, 2));
assert.equal(loopSelfTest, 0x1f, 'Asyncify message queue self-test');
assert.ok(started, 'WinMain must start');
// Without a saved "Screen Size" or a size on the command line, the game starts at 1024x768.
if (!process.env.AOE_CMDLINE) assert.deepEqual(frame, {width: 1024, height: 768}, 'Default screen size is 1024x768');
assert.ok(report.passed, 'Run must end normally or at a named unported Windows call, got: ' + end.kind + ' ' + (end.reason || ''));
process.exit(0);
