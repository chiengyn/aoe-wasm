// Phase 5: scenario files made by the retail game load through the recovered loaders. Boots the real
// WinMain for one prepared edition (AOE_DATA, default 'game'; e.g. game-333hd) and reads every
// scenario/*.scn|*.scx with the scenario list's header reader and the full scenario information reader.
// Writes reports/native-scenarios-<edition>.json.
import fs from 'node:fs';
import assert from 'node:assert/strict';
import {metricFonts} from '../web/aoe-fonts.mjs';

const BUILD_DIR = process.env.AOE_BUILD || 'build';
const DATA = process.env.AOE_DATA || 'game';
const {default: createGame} = await import(`../engine/${BUILD_DIR}/game.mjs`);
const root = new URL('../', import.meta.url);
const read = p => fs.readFileSync(new URL(p, root));
const native = JSON.parse(read(`web/${DATA}/native.json`));
const log = [];
let frames = 0, aborted = null, checks = 0;
const ok = (value, message) => { assert.ok(value, message); checks++; };

const game = await createGame({
  wasmBinary: read(`engine/${BUILD_DIR}/game.wasm`), noInitialRun: true, aoeFonts: metricFonts(), aoePresent: () => frames++,
  print: line => log.push(line), printErr: line => log.push(line), onAbort: reason => { aborted = String(reason); },
  preRun: [m => {
    for (const d of native.directories) m.FS.mkdirTree('/' + d);
    for (const f of native.files) m.FS.writeFile('/' + f.path.toLowerCase(), read('web/' + f.url));
  }],
});
try { game.callMain(['NODXCHECK', 'NOSOUND', 'NOSTARTUP']); } catch (e) { if (!/unwind/i.test(String(e))) throw e; }
const sleep = ms => new Promise(r => setTimeout(r, ms));
for (const end = Date.now() + 20000; !frames && Date.now() < end; ) { if (aborted) throw new Error(aborted); await sleep(50); }
ok(frames > 0, 'Main menu reached');

const files = native.files.map(f => f.path).filter(p => /^scenario\/[^/]+\.sc[nx]$/i.test(p));
ok(files.length > 0, `${files.length} scenario files`);
const results = [];
for (const path of files) {
  const name = path.split('/').pop();
  const header = game.ccall('aoe_test_scenario', 'number', ['string', 'number'], [name, 0]);
  const version = game.ccall('aoe_test_scenario', 'number', ['string', 'number'], [name, 1]);
  if (aborted) throw new Error(`${name}: ${aborted}`);
  results.push({name, header: header === 1, version: version / 100});
}
for (const r of results) {
  ok(r.header, `${r.name}: header read by the scenario list`);
  ok(r.version >= 1.0 && r.version <= 1.15, `${r.name}: scenario information read (version ${r.version})`);
}
const report = {passed: true, edition: DATA, checks, scenarios: results.length,
  versions: [...new Set(results.map(r => r.version))].sort(), results};
fs.writeFileSync(new URL(`reports/native-scenarios-${DATA}.json`, root), JSON.stringify(report, null, 2) + '\n');
console.log(JSON.stringify({passed: true, edition: DATA, checks, scenarios: results.length, versions: report.versions}));
process.exit(0);
