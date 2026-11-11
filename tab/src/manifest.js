'use strict';
// manifest.js: Mod Settings manifests (docs/manifest-spec.md) -> the row table the page and the expression broker are built from.
//   load(files, slotsFile)  -> { mods, rows, band, request, slots, problems }   (throws on any problem: a bad manifest never builds)
//   One manifest per mod (<id>.json). Rows keep their value in one of three places ("store"):
//     "band"                    the shared settings band in the save (CrossArcData), at the row's slot in slots.json (the broker's request + #43 handler)
//     { "request": {...} }      the mod's OWN hidden request (Description "Legacy Leaderboard"), as League Settings rows read it today (Dynasty Extensions,
//                               Preseason Weeks): typeName (optional), responses (= its form count), invert (UI value = 1 - Priority; DE's On/Off requests)
//     "memory"                  page only (probe builds): the value lives while the page is open
const fs = require('fs');
const path = require('path');

const KEY0 = 31000;                       // first UI row key (stock settings keys are small enum values)
const ID_RE = /^[a-z0-9][a-z0-9-]{1,39}$/;
const MAX_ROWS_PER_MOD = 64, MAX_LABEL = 40, MAX_HELP = 400, MAX_OPT = 24;

function fail(problems, m) { problems.push(m); }
function widthOf(n) { let w = 0; while ((1 << w) < n) w++; return w; }

function loadSlots(file) {
  const s = JSON.parse(fs.readFileSync(file, 'utf8'));
  const problems = [];
  if (s.schema !== 1) fail(problems, `${file}: schema ${s.schema} (want 1)`);
  const B = s.band;
  if (!B || B.recordSize !== 1 << B.bitsPerRecord) fail(problems, 'band: recordSize must be 2^bitsPerRecord');
  if (B.base < -32768 || B.base + B.records * B.recordSize - 1 > 32767) fail(problems, 'band: outside int16');
  const used = new Map(); const prios = [];
  for (const x of s.slots) {
    const tag = `${x.mod}/${x.row}`;
    if (!(x.record >= 0 && x.record < B.records)) fail(problems, `${tag}: record ${x.record} outside the band (${B.records} records)`);
    if (!(x.width >= 1 && x.width <= 3)) fail(problems, `${tag}: width ${x.width} (1..3: 2..8 options)`);
    if (!(x.bit >= 0 && x.bit + x.width <= B.bitsPerRecord)) fail(problems, `${tag}: bits ${x.bit}..${x.bit + x.width - 1} outside the record`);
    for (let b = x.bit; b < x.bit + x.width; b++) { const k = `${x.record}:${b}`; if (used.has(k)) fail(problems, `${tag}: bit ${k} already used by ${used.get(k)}`); used.set(k, tag); }
    if (!(x.prio >= 0 && x.prio + (1 << x.width) <= 256 + (1 << x.width))) fail(problems, `${tag}: prio ${x.prio}`);
    prios.push([x.prio, tag]);
    if (s.slots.filter((y) => y.mod === x.mod && y.row === x.row).length !== 1) fail(problems, `${tag}: listed twice`);
  }
  return { s, problems };
}

function loadManifest(file) {
  const m = JSON.parse(fs.readFileSync(file, 'utf8'));
  const problems = []; const where = path.basename(file);
  if (m.schema !== 1) fail(problems, `${where}: schema ${m.schema} (want 1)`);
  if (!ID_RE.test(m.id || '')) fail(problems, `${where}: id "${m.id}" (lower case letters, digits, '-', 2-40 chars)`);
  if (path.basename(file, '.json') !== m.id) fail(problems, `${where}: the file name must be <id>.json`);
  if (!m.title || m.title.length > MAX_LABEL) fail(problems, `${where}: title missing or longer than ${MAX_LABEL}`);
  if (!['dynasty', 'global'].includes(m.scope || 'dynasty')) fail(problems, `${where}: scope "${m.scope}" (dynasty | global)`);
  if (!Array.isArray(m.rows) || !m.rows.length || m.rows.length > MAX_ROWS_PER_MOD) fail(problems, `${where}: rows: 1..${MAX_ROWS_PER_MOD}`);
  const seen = new Set();
  for (const r of m.rows || []) {
    const tag = `${m.id}/${r.id}`;
    if (!ID_RE.test(r.id || '') && !/^[a-z0-9]{1,2}$/.test(r.id || '')) fail(problems, `${tag}: row id`);
    if (seen.has(r.id)) fail(problems, `${tag}: row id twice`); seen.add(r.id);
    if (!r.label || r.label.length > MAX_LABEL) fail(problems, `${tag}: label missing or longer than ${MAX_LABEL}`);
    if (r.help && r.help.length > MAX_HELP) fail(problems, `${tag}: help longer than ${MAX_HELP}`);
    if (!Array.isArray(r.options) || r.options.length < 2 || r.options.length > 8) fail(problems, `${tag}: options: 2..8 labels`);
    else {
      if (r.options.some((o) => typeof o !== 'string' || !o.length || o.length > MAX_OPT)) fail(problems, `${tag}: option labels 1..${MAX_OPT} chars`);
      // the stock two-slot toggle lights the slot labelled Off for value 0 and On for 1 (Dynasty Extensions lab, 2026-11-03): two-option rows are Off / On, in that order
      if (r.options.length === 2 && !(r.options[0] === 'Off' && r.options[1] === 'On')) fail(problems, `${tag}: a two-option row must be ["Off", "On"] (Off = 0, On = 1)`);
    }
    if (!(Number.isInteger(r.default) && r.default >= 0 && r.default < (r.options || []).length)) fail(problems, `${tag}: default must be an option index`);
    const st = r.store === undefined ? 'band' : r.store;
    if (!(st === 'band' || st === 'memory' || (st && typeof st === 'object' && st.request))) fail(problems, `${tag}: store "band" | "memory" | {request:{...}}`);
    if (st && st.request) {
      const q = st.request;
      if (q.responses !== (r.options || []).length) fail(problems, `${tag}: request.responses must equal the option count`);
      if (q.invert && (r.options || []).length !== 2) fail(problems, `${tag}: invert only for On / Off rows`);
      if (q.typeName !== undefined && (typeof q.typeName !== 'string' || !q.typeName)) fail(problems, `${tag}: request.typeName`);
      if (q.typeName === 'Legacy Leaderboard') fail(problems, `${tag}: request.typeName "Legacy Leaderboard" is the broker's own`);
      if (q.responses === 8) fail(problems, `${tag}: 8-form requests are the broker's (pick a band slot instead)`);
    }
  }
  return { m, problems };
}

// files: manifest paths in page order. opts.memoryAll: every row is a page-only row (probe builds).
function load(files, slotsFile, opts = {}) {
  const S = loadSlots(slotsFile); const problems = [...S.problems];
  const { band, request, slots } = S.s;
  const mods = []; const rows = []; let key = KEY0;
  const ids = new Set();
  for (const f of files) {
    const L = loadManifest(f); problems.push(...L.problems);
    const m = L.m; if (ids.has(m.id)) fail(problems, `${m.id}: two manifests`); ids.add(m.id);
    const mod = { id: m.id, title: m.title, scope: m.scope || 'dynasty', rows: [] };
    mods.push(mod);
    rows.push({ kind: 'header', key: key++, label: m.title, help: '', mod: m.id });
    for (const r of m.rows || []) {
      const st = opts.memoryAll ? 'memory' : (r.store === undefined ? 'band' : r.store);
      const row = { kind: 'toggle', key: key++, mod: m.id, id: r.id, label: r.label, help: r.help || '', options: r.options, default: r.default,
        inert: r.inert || (mod.scope === 'dynasty' ? 'After your next Advance' : 'Not available'), store: st };
      if (st === 'band') {
        const sl = slots.find((x) => x.mod === m.id && x.row === r.id);
        if (!sl) { fail(problems, `${m.id}/${r.id}: no slot in slots.json (store "band" needs one)`); continue; }
        if (sl.status === 'reserved') fail(problems, `${m.id}/${r.id}: slot is reserved (not live yet)`);
        if ((1 << sl.width) < r.options.length) fail(problems, `${m.id}/${r.id}: slot width ${sl.width} holds ${1 << sl.width} values, the row has ${r.options.length}`);
        if (sl.prio + r.options.length > 256) fail(problems, `${m.id}/${r.id}: prio ${sl.prio} + ${r.options.length} options > 256 (Priority is 0..255)`);
        if (r.default >= (1 << sl.width)) fail(problems, `${m.id}/${r.id}: default does not fit the slot`);
        row.slot = { record: sl.record, bit: sl.bit, width: sl.width, prio: sl.prio,
          marker: request.markerBase + sl.prio * 512 + sl.record * 64 + sl.bit * 4 + (sl.width - 1) };
      } else if (st && st.request) row.request = { typeName: st.request.typeName, responses: st.request.responses, invert: !!st.request.invert };
      rows.push(row); mod.rows.push(row);
    }
  }
  // the broker's priority ranges are disjoint over the WHOLE slot list (a removed mod's stale request can never be read as another row)
  const pr = slots.map((x) => [x.prio, x.prio + (1 << x.width), `${x.mod}/${x.row}`]).sort((a, b) => a[0] - b[0]);
  for (let i = 1; i < pr.length; i++) if (pr[i][0] < pr[i - 1][1]) fail(problems, `prio ranges overlap: ${pr[i - 1][2]} [${pr[i - 1][0]}, ${pr[i - 1][1]}) and ${pr[i][2]} [${pr[i][0]}, ${pr[i][1]})`);
  for (const x of slots) if (x.prio + (1 << x.width) > 256) fail(problems, `${x.mod}/${x.row}: prio range ends past 255`);
  if (request.markerBase + 256 * 512 > 16777215 || request.markerSpan !== 131072) fail(problems, 'request marker range');
  if (problems.length) { const e = new Error('Mod Settings manifests REFUSED:\n  ' + problems.join('\n  ')); e.problems = problems; throw e; }
  const bandRows = rows.filter((r) => r.slot);
  const records = [...new Set(bandRows.map((r) => r.slot.record))].sort((a, b) => a - b);
  // a record's starting value: every band row's default at its bits (rows of mods not built in stay 0)
  const recordDefault = Object.fromEntries(records.map((rec) => [rec, bandRows.filter((r) => r.slot.record === rec).reduce((v, r) => v + (r.default << r.slot.bit), 0)]));
  return { mods, rows, band, request, slots, bandRows, records, recordDefault };
}

// a band record's Value <-> the bits of one row (the reader and the tests use these; the expression code does the same in int math)
const recordLo = (band, rec) => band.base + rec * band.recordSize;
function bitsOf(band, value, slot) { const v = value - recordLo(band, slot.record); return Math.floor(v / (1 << slot.bit)) % (1 << slot.width); }
function decodeMarker(request, mk) {
  const f = mk - request.markerBase; if (f < 0 || f >= request.markerSpan) return null;
  const prio = Math.floor(f / 512), s = f % 512; return { prio, record: Math.floor(s / 64), bit: Math.floor((s % 64) / 4), width: (s % 4) + 1 };
}
module.exports = { load, loadManifest, loadSlots, recordLo, bitsOf, decodeMarker, widthOf, KEY0 };
