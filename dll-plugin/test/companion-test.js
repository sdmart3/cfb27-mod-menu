#!/usr/bin/env node
// companion-test.js: the page's rules (ms-core inside companion/mod-settings.html), ms-set.js, ms-check.js, and PARITY with modsettings.dll
// (the DLL's own reading of the same folder: `modsettings-test.exe load <dir>`). Offline; writes only under dll/out/t-companion.
//   node test/companion-test.js            (after node dll/build.js, which builds modsettings-test.exe)
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process');
const REPO = path.join(__dirname, '..'), OUT = path.join(REPO, 'dll', 'out', 't-companion'), EXE = path.join(REPO, 'dll', 'out', 'modsettings-test.exe');
const C = require('../tools/ms-core.js'); const SET = require('../tools/ms-set.js'); const CHK = require('../tools/ms-check.js');
const SLOTS = path.join(REPO, '..', 'modsettings', 'slots.json');
let ok = 0, fail = 0; const T = (c, m) => { if (c) { ok++; console.log('  ok: ' + m); } else { fail++; console.log('  FAIL: ' + m); } };
fs.rmSync(OUT, { recursive: true, force: true }); fs.mkdirSync(path.join(OUT, 'mods'), { recursive: true });
const put = (n, o) => fs.writeFileSync(path.join(OUT, n), typeof o === 'string' ? o : JSON.stringify(o, null, 2));
const demo = JSON.parse(fs.readFileSync(path.join(REPO, 'examples', 'mods', 'modsettings-demo.json'), 'utf8'));
const other = { schema: 2, id: 'other', title: 'Other Mod', rows: [
  { id: 'x', label: 'X', options: ['Off', 'On'], default: 0, store: 'band', band: { record: 1, bit: 0, width: 1 } },
  { id: 'speed', label: 'Speed', options: ['Slow', 'Normal', 'Fast'], default: 1, store: 'global', ini: { file: 'Other\\other.ini', key: 'speed', values: ['0', '1', '2'] } }] };
const row1 = (extra) => Object.assign({ id: 'a', label: 'A', options: ['Off', 'On'], default: 0, store: 'global' }, extra);
const bad = {
  'p-schema1.json': { schema: 1, id: 'p-schema1', title: 'T', rows: [row1()] },
  'p-name.json': { schema: 2, id: 'p-other-name', title: 'T', rows: [row1()] },
  'p-onoff.json': { schema: 2, id: 'p-onoff', title: 'T', rows: [row1({ options: ['On', 'Off'] })] },
  'p-overlap.json': { schema: 2, id: 'p-overlap', title: 'T', rows: [row1({ store: 'band', band: { record: 0, bit: 6, width: 1 } })] },
  'p-dotdot.json': { schema: 2, id: 'p-dotdot', title: 'T', rows: [row1({ ini: { file: '..\\x.ini', key: 'k', values: ['0', '1'] } })] },
  'p-default.json': { schema: 2, id: 'p-default', title: 'T', rows: [row1({ default: 2 })] },
  'p-width.json': { schema: 2, id: 'p-width', title: 'T', rows: [row1({ options: ['L', 'M', 'H'], store: 'band', band: { record: 1, bit: 4, width: 1 } })] },
  'p-store.json': { schema: 2, id: 'p-store', title: 'T', rows: [row1({ store: undefined })] },
  'p-record.json': { schema: 2, id: 'p-record', title: 'T', rows: [row1({ store: 'band', band: { record: 2, bit: 0, width: 1 } })] },
  'p-bandini.json': { schema: 2, id: 'p-bandini', title: 'T', rows: [row1({ store: 'band', band: { record: 1, bit: 8, width: 1 }, ini: { file: 'A\\a.ini', key: 'k', values: ['0', '1'] } })] },
  'p-longlabel.json': { schema: 2, id: 'p-longlabel', title: 'T', rows: [row1({ label: 'L'.repeat(41) })] },
  'p-inikey.json': { schema: 2, id: 'p-inikey', title: 'T', rows: [row1({ ini: { file: 'A\\a.ini', key: 'bad key', values: ['0', '1'] } })] },
};
put('mods/modsettings-demo.json', demo); put('mods/other.json', other); for (const [n, m] of Object.entries(bad)) put('mods/' + n, m); put('mods/p-json.json', '{ "schema": 2, ');
const files = fs.readdirSync(path.join(OUT, 'mods')).map((name) => ({ name, text: fs.readFileSync(path.join(OUT, 'mods', name), 'utf8') }));
console.log('--- ms-core: manifests');
const model = C.loadManifests(files);
T(model.mods.map((m) => m.id).join(',') === 'modsettings-demo,other', 'loads the two good manifests, in the DLL\'s order');
T(model.problems.length >= 13 && Object.keys(bad).every((n) => model.problems.some((p) => p.includes(n.replace('.json', '')) || p.startsWith(n))), `refuses every bad manifest (${model.problems.length} problem lines)`);
T(model.problems.some((p) => p.includes('p-overlap/a: band record 0 bits 6-6 overlap modsettings-demo/beta')), 'a band bit clash names both rows');
console.log('--- parity with modsettings.dll (modsettings-test.exe load)');
function dllLoad() { const r = cp.spawnSync(EXE, ['load', OUT], { encoding: 'utf8' }); if (r.status !== 0) throw new Error('modsettings-test.exe load failed: ' + r.stdout + r.stderr); const L = r.stdout.split(/\r?\n/);
  return { loaded: (L.find((l) => l.startsWith('LOADED ')) || 'LOADED ').slice(7), rows: L.filter((l) => l.startsWith('ROW ')), problems: L.filter((l) => l.startsWith('PROBLEM ')) }; }
let D = dllLoad();
T(D.loaded === 'modsettings-demo,other', `the DLL loads the same two (${D.loaded})`);
T(D.rows.length === model.mods.reduce((n, m) => n + m.rows.length, 0), `the same row count (${D.rows.length})`);
for (const n of Object.keys(bad).concat(['p-json.json'])) { const id = n.replace('.json', ''); T(D.problems.some((p) => p.includes(id)) && model.problems.some((p) => p.includes(id)), `both refuse ${n}`); }
console.log('--- ms-set.js -> values.json -> the DLL reads it');
const ms = (...a) => SET.main([OUT, ...a]);
T(/whole game/.test(ms('set', 'other/speed', 'Fast').join('')), 'set a global row (other/speed = Fast)');
T(/seq 1/.test(ms('set', 'modsettings-demo/alpha', 'Off').join('')), 'set a dynasty row: seq 1');
T(/seq 2/.test(ms('set', 'modsettings-demo/beta', '2').join('')), 'set by index: seq 2');
T(/seq 3/.test(ms('set', 'modsettings-demo/gamma', 'On').join('')), 'seq 3');
let threw = false; try { ms('set', 'modsettings-demo/beta', 'Turbo'); } catch (e) { threw = /no option "Turbo"/.test(e.message); } T(threw, 'an unknown option is refused');
threw = false; try { ms('set', 'nope/x', 'On'); } catch (e) { threw = /no row nope\/x/.test(e.message); } T(threw, 'an unknown row is refused');
D = dllLoad();
const rowOf = (k) => D.rows.find((l) => l.startsWith(`ROW ${k} `)) || '';
T(/global gval 2 gset 1/.test(rowOf('other/speed')), `DLL: other/speed = 2 from values.json (${rowOf('other/speed')})`);
T(/want 0 seq 1/.test(rowOf('modsettings-demo/alpha')) && /want 2 seq 2/.test(rowOf('modsettings-demo/beta')) && /want 1 seq 3/.test(rowOf('modsettings-demo/gamma')), 'DLL: the three dynasty changes with seqs 1 2 3');
T(!D.problems.some((p) => p.includes('values.json: ')), 'DLL: no values.json problem');
console.log('--- the page model against a status file');
let values = C.parseValues(fs.readFileSync(path.join(OUT, 'values.json'), 'utf8'));
const alpha = model.mods[0].rows[0], beta = model.mods[0].rows[1], x = model.mods[1].rows[0];
const status1 = { rows: [{ mod: 'modsettings-demo', row: 'alpha', from: 'dynasty', value: 1, applied_seq: 0 }, { mod: 'modsettings-demo', row: 'beta', from: 'dynasty', value: 2, applied_seq: 2 }, { mod: 'other', row: 'x', from: 'record not there yet', value: -1, applied_seq: 0 }] };
let v = C.view(alpha, values, status1); T(v.value === 1 && v.from === 'this dynasty' && v.pending === 0, 'alpha: On in this dynasty, Off waiting to land');
v = C.view(beta, values, status1); T(v.value === 2 && v.pending === null, 'beta: landed (applied seq 2 >= 2): nothing waiting');
v = C.view(x, values, status1); T(v.from === 'not set up in this dynasty yet' && /first Advance/.test(v.note), 'other/x: record not there yet -> the first-Advance note');
v = C.view(alpha, values, null); T(v.from === 'no dynasty loaded' && v.value === 1 && v.pending === 0, 'no status file: default shown, the change waits');
const v2 = C.choose(values, x, 1, { rows: [{ mod: 'a', row: 'b', applied_seq: 9 }] }); T(v2.dynasty.other.x.seq === 10, 'a new change takes a seq above every applied seq in the status (10)');
const v3 = C.cancel(v2, x); T(!v3.dynasty.other, 'cancel drops the change (and the empty mod)');
T(C.bitsOf(-8000, beta) === 2 && C.bitsOf(-8000, alpha) === 0 && C.bitsOf(-7679, x) === 1, 'bitsOf matches the DLL (record 0 -8000: alpha 0, beta 2; record 1 -7679: x 1)');
T(C.parseValues('garbage').broken === true && C.parseValues(null).schema === 2, 'a broken / missing values.json -> an empty one');
console.log('--- ms-check.js against modsettings/slots.json');
let r = CHK.check([path.join(REPO, 'examples', 'mods', 'modsettings-demo.json')], SLOTS); T(r.problems.length === 0, 'the demo manifest matches its slots.json lines');
put('mods-check-other.json', other); fs.mkdirSync(path.join(OUT, 'chk'), { recursive: true }); fs.writeFileSync(path.join(OUT, 'chk', 'other.json'), JSON.stringify(other));
r = CHK.check([path.join(OUT, 'chk', 'other.json')], SLOTS); T(r.problems.some((p) => /other\/x: no line in slots.json/.test(p)), 'a band row without a slot line is refused');
const moved = JSON.parse(JSON.stringify(demo)); moved.rows[0].band.bit = 8; fs.writeFileSync(path.join(OUT, 'chk', 'modsettings-demo.json'), JSON.stringify(moved));
r = CHK.check([path.join(OUT, 'chk', 'modsettings-demo.json')], SLOTS); T(r.problems.some((p) => /differs from its slot line/.test(p)), 'a band row that moved off its slot is refused');
console.log('--- the page itself');
const html = fs.readFileSync(path.join(REPO, 'companion', 'mod-settings.html'), 'utf8');
const scripts = [...html.matchAll(/<script(?: id="[^"]*")?>([\s\S]*?)<\/script>/g)].map((m) => m[1]);
let parsed = 0; for (const s of scripts) { try { new (require('vm').Script)(s); parsed++; } catch (e) { console.log('    ' + e.message); } }
T(scripts.length === 2 && parsed === 2, `both inline scripts compile (${parsed} of ${scripts.length})`);
T(!/<script[^>]+src=|<link[^>]+href=|https?:\/\/|fetch\(|XMLHttpRequest|WebSocket/.test(html), 'no external script, stylesheet, URL or network call: the page reads and writes the picked folder only');
T(/getFileHandle\('values\.json', \{ create: true \}\)/.test(html) && !/getFileHandle\('(?!values\.json)[^']+', \{ create: true \}\)/.test(html), 'the page creates / writes values.json and nothing else');
console.log(`companion: ${ok} ok, ${fail} failure(s)`);
process.exit(fail ? 1 : 0);
