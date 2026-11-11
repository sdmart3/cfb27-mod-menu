#!/usr/bin/env node
// ms-set.js: the Mod Settings page from the command line (the lab and players without Edge / Chrome). Same rules as the page (ms-core).
//   node ms-set.js <Plugins\ModSettings folder> list                 every mod's rows, what is in effect, what waits to land
//   node ms-set.js <folder> set <mod>/<row> <option label | index>   choose (global rows: values.json; dynasty rows: a new change, next seq)
//   node ms-set.js <folder> cancel <mod>/<row>                       drop a dynasty change that has not landed
// Writes ONLY <folder>\values.json (atomically: a temp file, then a rename). Refuses a folder without a mods\ folder.
'use strict';
const fs = require('fs'), path = require('path');
const C = require('./ms-core.js');
function read(dir) {
  const md = path.join(dir, 'mods'); if (!fs.existsSync(md)) throw new Error(`${dir} has no mods\\ folder: not a Mod Settings folder`);
  const files = fs.readdirSync(md).filter((f) => /\.json$/i.test(f)).map((name) => ({ name, text: fs.readFileSync(path.join(md, name), 'utf8') }));
  const model = C.loadManifests(files);
  const vp = path.join(dir, 'values.json'); const values = C.parseValues(fs.existsSync(vp) ? fs.readFileSync(vp, 'utf8') : null);
  const sp = path.join(dir, 'modsettings-status.json'); let status = null; try { status = JSON.parse(fs.readFileSync(sp, 'utf8')); } catch (e) { status = null; }
  return { model, values, status };
}
function write(dir, values) { const p = path.join(dir, 'values.json'), t = p + '.ms-set.tmp'; fs.writeFileSync(t, C.serialize(values)); fs.renameSync(t, p); }
function find(model, spec) { const [mod, row] = String(spec || '').split('/'); const m = model.mods.find((x) => x.id === mod); const r = m && m.rows.find((x) => x.id === row); if (!r) throw new Error(`no row ${spec} (installed: ${model.mods.flatMap((x) => x.rows.map((y) => `${x.id}/${y.id}`)).join(', ') || 'none'})`); return r; }
function main(argv) {
  const [dir, cmd, spec, val] = argv; const out = [];
  if (!dir || !cmd) throw new Error('usage: ms-set.js <folder> list | set <mod>/<row> <option> | cancel <mod>/<row>');
  const { model, values, status } = read(dir);
  if (cmd === 'list') {
    for (const p of model.problems) out.push(`PROBLEM ${p}`);
    for (const m of model.mods) { out.push(`${m.title} (${m.id})`); for (const r of m.rows) { const v = C.view(r, values, status); out.push(`  ${m.id}/${r.id} [${r.store}] ${r.options[v.value]} (${v.from})${v.pending != null ? `; waiting to land: ${r.options[v.pending]}` : ''}`); } }
  } else if (cmd === 'set') {
    const r = find(model, spec); let i = r.options.indexOf(val); if (i < 0 && /^\d+$/.test(String(val))) i = Number(val);
    if (!(i >= 0 && i < r.options.length)) throw new Error(`${spec}: no option "${val}" (${r.options.join(' / ')})`);
    const nv = C.choose(values, r, i, status); write(dir, nv);
    out.push(`${spec} = ${r.options[i]}${r.store === 'band' ? ` (dynasty change seq ${nv.dynasty[r.mod][r.id].seq}: lands in the loaded dynasty at its next hub refresh / Roster / Schedule)` : ' (whole game)'}`);
  } else if (cmd === 'cancel') { const r = find(model, spec); write(dir, C.cancel(values, r)); out.push(`${spec}: change dropped`); }
  else throw new Error(`unknown command ${cmd}`);
  return out;
}
if (require.main === module) { try { console.log(main(process.argv.slice(2)).join('\n')); } catch (e) { console.error('ms-set: ' + e.message); process.exit(1); } }
module.exports = { main };
