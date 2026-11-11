#!/usr/bin/env node
// ms-check.js: check v2 manifests before they ship (modders; the same rules as the DLL and the page) and, for band rows, that each row's
// "band" matches its line in the public slot list (modsettings/slots.json: mod, row, record, bit, width).
//   node ms-check.js <manifest.json> [...] [--slots <slots.json>]
// Exit 0 = every manifest loads; 1 = a problem (each printed).
'use strict';
const fs = require('fs'), path = require('path');
const C = require('./ms-core.js');
function check(files, slotsFile) {
  const P = []; const slots = slotsFile ? JSON.parse(fs.readFileSync(slotsFile, 'utf8')) : null;
  const model = C.loadManifests(files.map((f) => ({ name: path.basename(f), text: fs.readFileSync(f, 'utf8') })));
  P.push(...model.problems);
  if (slots) {
    const B = slots.band || {};
    if (B.base !== C.BAND.base || B.recordSize !== C.BAND.recordSize || B.bitsPerRecord !== C.BAND.bits) P.push(`${slotsFile}: the band (base ${B.base}, ${B.recordSize} per record, ${B.bitsPerRecord} bits) is not the one this tool writes`);
    for (const m of model.mods) for (const r of m.rows.filter((x) => x.store === 'band')) {
      const s = (slots.slots || []).find((x) => x.mod === r.mod && x.row === r.id);
      if (!s) P.push(`${r.mod}/${r.id}: no line in ${path.basename(slotsFile)} (ask for a slot before you ship)`);
      else if (s.record !== r.record || s.bit !== r.bit || s.width !== r.width) P.push(`${r.mod}/${r.id}: band record ${r.record} bit ${r.bit} width ${r.width} differs from its slot line (record ${s.record} bit ${s.bit} width ${s.width})`);
    }
  }
  return { model, problems: P };
}
if (require.main === module) {
  const a = process.argv.slice(2); const si = a.indexOf('--slots'); const slots = si >= 0 ? a[si + 1] : null; const files = a.filter((x, i) => !x.startsWith('--') && (si < 0 || i !== si + 1));
  if (!files.length) { console.error('usage: ms-check.js <manifest.json> [...] [--slots <slots.json>]'); process.exit(2); }
  const r = check(files, slots);
  for (const m of r.model.mods) console.log(`ok   ${m.id}: ${m.rows.length} row(s) (${m.rows.map((x) => `${x.id}:${x.store}`).join(', ')})`);
  for (const p of r.problems) console.log(`FAIL ${p}`);
  process.exit(r.problems.length ? 1 : 0);
}
module.exports = { check };
