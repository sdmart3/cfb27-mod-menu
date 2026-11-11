#!/usr/bin/env node
// read-ms.js: READ-ONLY report of a lab save for the Mod Settings lab tests (docs/design.md, Built section).
//   * user 0's shared-band records (slots.json band) and every slotted row's value in words (manifests in modsettings/manifests)
//   * the broker's hidden requests (CpuResponseMinDuration in the marker range): row, Priority, forms, answered, still listed; plus DE / PW's own
//     setting requests (the "request" rows of the DE-on-page variant)
//   * the record count of the 150-record CrossArcData table
//   * with --before <save>: band rows that changed
// usage: node modsettings/tools/read-ms.js <lab save> [--before <lab save>]       (lab copies only; never a real dynasty)
'use strict';
const fs = require('fs'); const path = require('path');
const REPO = path.join(__dirname, '..'); const D = require(path.join(REPO, '..', 'tools', 'dynasty-core.js'));
const MAN = require('../src/manifest.js');
const SL = JSON.parse(fs.readFileSync(path.join(REPO, 'slots.json'), 'utf8'));
const mans = fs.readdirSync(path.join(REPO, 'manifests')).filter((f) => /\.json$/.test(f)).map((f) => JSON.parse(fs.readFileSync(path.join(REPO, 'manifests', f), 'utf8')));
const rowOf = (s) => { const m = mans.find((x) => x.id === s.mod); const r = m && m.rows.find((x) => x.id === s.row); return r || null; };
async function tablesNamed(f, name) { const out = []; for (const t of f.tables.filter((x) => x.name === name)) { if (!t.isLoaded) await t.readRecords(); out.push(t); } return out; }
async function deref(f, rec, key) { const r = D.refOf(rec, key); if (!r) return null; const t = f.getTableById(r.tableId); if (!t) return null; if (!t.isLoaded) await t.readRecords(); const x = t.records[r.row]; return x && !x.isEmpty ? x : null; }
const arrKeys = (arr) => (arr ? Object.keys(arr.fields).filter((k) => /\d+$/.test(k)).sort((a, b) => Number(/\d+$/.exec(a)[0]) - Number(/\d+$/.exec(b)[0])).slice(0, Number.isFinite(arr.arraySize) ? arr.arraySize : undefined) : []);
async function arrayItems(f, arr) { const out = []; for (const k of arrKeys(arr)) out.push(await deref(f, arr, k)); return out; }
const str = (r, k) => { const v = D.safe(r, k); return v == null ? '' : String(v); };
const num = (r, k) => D.num(D.safe(r, k));
const OWN = { 16777104: 'DE Weekly Strategy On/Off', 16777105: 'DE Press Box On/Off', 16777103: 'PW Spring Game setting' };
async function read(file) {
  const f = await D.openRead(file); const si = await D.seasonInfo(f);
  const fu = (await tablesNamed(f, 'FranchiseUser')).flatMap((t) => t.records.filter((r) => r && !r.isEmpty))[0];
  const all = (fu ? await arrayItems(f, await deref(f, fu, 'CrossArcDataArray')) : []).filter(Boolean);
  const vals = all.filter((r) => str(r, 'Id') === 'PLACEHOLDER').map((r) => num(r, 'Value'));
  const listed = new Set();
  const rs = (await tablesNamed(f, 'RequestServer')).flatMap((t) => t.records.filter((r) => r && !r.isEmpty))[0];
  if (rs) { const lst = await deref(f, rs, '#1'); if (lst) for (const k of arrKeys(lst)) { const r = D.refOf(lst, k); if (r) listed.add(`${r.tableId}:${r.row}`); } }
  const reqs = [];
  for (const t of await tablesNamed(f, 'LostPracticeSquadPlayerRequest')) for (const r of t.records) {
    if (!r || r.isEmpty) continue; const mk = num(r, 'CpuResponseMinDuration'); const dm = MAN.decodeMarker(SL.request, mk); if (!dm && !OWN[mk]) continue;
    const forms = await deref(f, r, 'ResponseForms'); const keys = arrKeys(forms); const resp = D.refOf(r, 'Response');
    const ans = resp ? keys.findIndex((k) => { const x = D.refOf(forms, k); return x && x.tableId === resp.tableId && x.row === resp.row; }) : -1;
    const slot = dm ? SL.slots.find((s) => s.prio === dm.prio && s.record === dm.record && s.bit === dm.bit && s.width === dm.width) : null;
    reqs.push({ mk, dm, slot, what: slot ? `${slot.mod}/${slot.row}` : dm ? `broker (no slot ${JSON.stringify(dm)})` : OWN[mk], type: str(r, 'TypeName'), pri: num(r, 'Priority'), forms: keys.length, ans, listed: listed.has(`${t.header.tableId}:${r.index}`) });
  }
  return { si, vals, total: all.length, reqs };
}
(async () => {
  const args = process.argv.slice(2); const bi = args.indexOf('--before'); const before = bi >= 0 ? args[bi + 1] : null; const file = args.find((a, i) => !a.startsWith('--') && (bi < 0 || i !== bi + 1));
  if (!file) { console.error('usage: node read-ms.js <lab save> [--before <lab save>]'); process.exit(1); }
  const a = await read(file); const B = SL.band; const lo = (r) => B.base + r * B.recordSize;
  console.log(`${file}\n  ${a.si.year} ${a.si.stage} week ${a.si.week} (${a.si.weekType}); CrossArcData rows ${a.total} of 150`);
  for (let r = 0; r < B.records; r++) {
    const v = a.vals.filter((x) => x >= lo(r) && x < lo(r) + B.recordSize);
    console.log(`  BAND RECORD ${r}: ${v.length ? v.map((x) => `${x} (bits ${(x - lo(r)).toString(2).padStart(B.bitsPerRecord, '0')})`).join(', ') : 'none'}${v.length > 1 ? '  << MORE THAN ONE' : ''}`);
    if (v.length) for (const s of SL.slots.filter((q) => q.record === r)) { const row = rowOf(s); const val = MAN.bitsOf(B, v[0], s); console.log(`    ${s.mod}/${s.row} [bit ${s.bit}${s.width > 1 ? '..' + (s.bit + s.width - 1) : ''}${s.status === 'reserved' ? ', reserved' : ''}] = ${val}${row && row.options[val] ? ` (${row.options[val]})` : ''}`); }
  }
  console.log(`  REQUESTS (${a.reqs.length}):`); for (const r of a.reqs.sort((x, y) => x.mk - y.mk)) console.log(`    ${r.mk} ${r.what}: "${r.type}", Priority ${r.pri}${r.slot ? ` (= value ${r.pri - r.slot.prio})` : ''}, ${r.forms} forms, answered ${r.ans < 0 ? '-' : r.ans}, ${r.listed ? 'LISTED' : 'not listed'}`);
  for (const s of SL.slots.filter((q) => q.status !== 'reserved')) { const n = a.reqs.filter((r) => r.slot === s && r.listed).length; if (n !== 1) console.log(`  ${s.mod}/${s.row}: ${n} listed broker requests${n > 1 ? '  << MORE THAN ONE' : ' (none: before the first host run, dropped, or the mod is not installed)'}`); }
  if (before) {
    const b = await read(before); const inBand = (x) => x >= B.base && x < B.base + B.records * B.recordSize;
    console.log(`  vs ${before}: band rows added ${a.vals.filter((v) => inBand(v) && !b.vals.includes(v)).join(',') || '-'}; gone ${b.vals.filter((v) => inBand(v) && !a.vals.includes(v)).join(',') || '-'}; table ${b.total} -> ${a.total}`);
  }
})().catch((e) => { console.error(e.stack || e); process.exit(2); });
