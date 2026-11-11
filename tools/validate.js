#!/usr/bin/env node
// validate.js - checks a Mod Settings manifest (schema 3) and explains every problem in plain words.
// usage: node tools/validate.js <manifest.json | text-file-with-markers> [more files...] [--json]
//        node tools/validate.js --selftest
// A text file holding a [[MODSETTINGS ... MODSETTINGS]] block (a copied .fbmod description) is read too.
// Exit code 0 = no errors (warnings are fine), 1 = errors, 2 = could not read.
// Spec: docs/manifest-spec.md
'use strict';
const fs = require('fs');
const path = require('path');

const ROW_TYPES = ['toggle', 'list', 'slider', 'header', 'info'];
const BIND_TYPES = ['tuning', 'api', 'dynasty'];
const COPIES = ['simulation', 'base', 'both'];
const KNOWN = {
  top: ['schema', 'modid', 'title', 'version', 'rows', 'author', 'url', 'section', 'closed'],
  row: ['type', 'key', 'label', 'description', 'default', 'bind', 'options', 'min', 'max', 'step', 'unit'],
  tuning: ['type', 'asset', 'field', 'instance', 'copy', 'values', 'scale'],
  api: ['type'],
  dynasty: ['type', 'slot', 'values'],
};
const MARK_OPEN = '[[MODSETTINGS';
const MARK_CLOSE = 'MODSETTINGS]]';

function lev(a, b) {
  const d = Array.from({ length: a.length + 1 }, (_, i) => [i]);
  for (let j = 1; j <= b.length; j++) d[0][j] = j;
  for (let i = 1; i <= a.length; i++) for (let j = 1; j <= b.length; j++)
    d[i][j] = Math.min(d[i - 1][j] + 1, d[i][j - 1] + 1, d[i - 1][j - 1] + (a[i - 1] === b[j - 1] ? 0 : 1));
  return d[a.length][b.length];
}
function didYouMean(word, list) {
  let best = null, bd = 99;
  for (const w of list) { const x = lev(String(word).toLowerCase(), w); if (x < bd) { bd = x; best = w; } }
  return bd <= 2 ? ` Did you mean "${best}"?` : '';
}

// Pull the manifest text out of a file: raw JSON, or the marker block of a description.
function extract(text) {
  const a = text.indexOf(MARK_OPEN);
  if (a < 0) return { json: text, from: 'file' };
  const b = text.indexOf(MARK_CLOSE, a);
  if (b < 0) return { error: `Found "${MARK_OPEN}" but no closing "${MARK_CLOSE}". Add the closing marker on its own line after the JSON.` };
  return { json: text.slice(a + MARK_OPEN.length, b), from: 'description block' };
}

// spec v3 "Names, duplicates and clashes": modid normalisation (lower case; space _ . -> "-"; other characters dropped)
function normId(x) { return String(x).toLowerCase().replace(/[ _.]/g, '-').replace(/[^a-z0-9-]/g, ''); }
function normTitle(x) { return String(x || '').toLowerCase().replace(/[ \t]/g, ''); }
function choiceKeys(m) { return Array.isArray(m && m.rows) ? m.rows.filter((r) => r && typeof r.key === 'string' && r.type !== 'header' && r.type !== 'info').map((r) => r.key) : []; }
function verCmp(a, b) {
  const A = String(a).match(/\d+|\D+/g) || [], B = String(b).match(/\d+|\D+/g) || [];
  for (let i = 0; i < Math.max(A.length, B.length); i++) {
    const x = A[i], y = B[i]; if (x === undefined) return -1; if (y === undefined) return 1;
    if (/^\d/.test(x) && /^\d/.test(y)) { if (+x !== +y) return +x < +y ? -1 : 1; } else if (x !== y) return x < y ? -1 : 1;
  }
  return 0;
}

function validate(m) {
  const errors = [], warnings = [];
  const E = (where, msg) => errors.push(`${where}: ${msg}`);
  const W = (where, msg) => warnings.push(`${where}: ${msg}`);
  const isStr = (v, lo, hi) => typeof v === 'string' && v.length >= lo && v.length <= hi;
  const unknown = (obj, list, where) => {
    for (const k of Object.keys(obj)) if (!list.includes(k)) W(where, `"${k}" is not a field Mod Settings knows, so it is ignored.${didYouMean(k, list)}`);
  };

  if (m === null || typeof m !== 'object' || Array.isArray(m)) { E('manifest', 'the file must be one JSON object { ... } at the top level.'); return { errors, warnings }; }
  unknown(m, KNOWN.top, 'manifest');
  if (m.schema !== 3) E('schema', `must be 3 (this file says ${JSON.stringify(m.schema)}). Older manifests are skipped by the game with a warning, never a crash.`);
  if (typeof m.modid !== 'string' || !/^[a-z0-9][a-z0-9-]{1,39}$/.test(normId(m.modid)))
    E('modid', 'use 2-40 characters: lower case letters, digits and "-" (for example "pocket-presence"; a real mod: "yourname-modname"). It is your mod\'s permanent name; changing it later loses players\' saved choices.');
  else if (normId(m.modid) !== m.modid) W('modid', `"${m.modid}" is read as "${normId(m.modid)}". Write it that way so you see the id players' choices are saved under.`);
  if (m.section !== undefined) {
    const sv = typeof m.section === 'string' ? m.section : null; const t = sv === null ? '' : normId(sv[0] === '@' ? sv.slice(1) : sv);
    if (sv === null || !/^[a-z0-9][a-z0-9-]{1,39}$/.test(t)) E('section', 'is another mod\'s modid (your rows show under its header) or a shared "@name" such as "@pass-rush".');
    else if (sv[0] !== '@' && t === normId(m.modid || '')) W('section', 'is your own modid, so it changes nothing; leave it out.');
    else if (m.closed === true) W('closed', '"closed" is about YOUR section, but this manifest puts its rows in another section; other mods could not join anyway.');
  }
  if (m.closed !== undefined && typeof m.closed !== 'boolean') E('closed', 'is true (other mods may not add rows to your section) or false.');
  if (!isStr(m.title, 1, 40)) E('title', 'needs 1-40 characters. It is the section heading players see.');
  if (!isStr(m.version, 1, 20)) E('version', 'needs a version text such as "1.0.0" (1-20 characters).');
  if (!Array.isArray(m.rows) || m.rows.length < 1) { E('rows', 'needs a list with at least one row.'); return { errors, warnings }; }
  if (m.rows.length > 120) E('rows', `has ${m.rows.length} rows; the most one mod can show is 120. Split big mods with header rows or trim it.`);

  const keys = new Set();
  let live = 0;
  m.rows.forEach((r, i) => {
    const w = `rows[${i}]` + (r && r.key ? ` ("${r.key}")` : r && r.label ? ` ("${r.label}")` : '');
    if (r === null || typeof r !== 'object' || Array.isArray(r)) { E(w, 'a row must be an object { ... }.'); return; }
    if (!ROW_TYPES.includes(r.type)) { E(w, `"type" must be one of ${ROW_TYPES.join(', ')}.${r.type ? didYouMean(r.type, ROW_TYPES) : ' It is missing.'}`); return; }
    unknown(r, KNOWN.row, w);
    if (!isStr(r.label, 1, r.type === 'info' ? 200 : 40)) E(w, `"label" needs 1-${r.type === 'info' ? 200 : 40} characters.`);
    if (r.description !== undefined && !isStr(r.description, 1, 400)) E(w, '"description" needs 1-400 characters (or leave it out).');
    if (r.type === 'header' || r.type === 'info') {
      for (const f of ['key', 'default', 'bind', 'options', 'min', 'max', 'step'])
        if (r[f] !== undefined) W(w, `"${f}" does nothing on a ${r.type} row.`);
      return;
    }
    live++;
    if (typeof r.key !== 'string' || !/^[a-z0-9][a-z0-9_-]{0,31}$/.test(r.key)) E(w, '"key" needs 1-32 characters: lower case letters, digits, "-" or "_". It is the row\'s permanent id.');
    else if (keys.has(r.key)) E(w, `"key" "${r.key}" is used twice. Every row needs its own key.`);
    else keys.add(r.key);
    if (r.description === undefined) W(w, 'no "description". Players read it as the help line; one sentence helps a lot.');

    // choices and default
    let n = 0; // number of distinct values the row can hold
    if (r.type === 'toggle') {
      n = 2;
      if (!(typeof r.default === 'boolean' || r.default === 0 || r.default === 1)) E(w, '"default" must be true or false for a toggle.');
    } else if (r.type === 'list') {
      if (!Array.isArray(r.options) || r.options.length < 2 || r.options.length > 12) E(w, '"options" needs 2-12 labels, like ["Off", "Low", "High"].');
      else {
        n = r.options.length;
        r.options.forEach((o, j) => { if (!isStr(o, 1, 24)) E(w, `options[${j}] needs 1-24 characters of text.`); });
        if (new Set(r.options).size !== r.options.length) E(w, 'two options have the same label.');
        if (!Number.isInteger(r.default) || r.default < 0 || r.default >= n) E(w, `"default" is the option's position, from 0 to ${n - 1}. You wrote ${JSON.stringify(r.default)}.`);
      }
    } else if (r.type === 'slider') {
      const ok = ['min', 'max', 'step', 'default'].every(f => typeof r[f] === 'number' && isFinite(r[f]));
      if (!ok) E(w, 'a slider needs numbers for "min", "max", "step" and "default".');
      else if (r.max <= r.min) E(w, `"max" (${r.max}) must be bigger than "min" (${r.min}).`);
      else if (r.step <= 0) E(w, '"step" must be above 0.');
      else {
        n = Math.floor((r.max - r.min) / r.step + 1e-9) + 1;
        if (n > 1001) E(w, `this slider has ${n} stops; the most is 1001. Use a bigger "step".`);
        if (((r.max - r.min) / r.step) % 1 > 1e-9 && ((r.max - r.min) / r.step) % 1 < 1 - 1e-9) W(w, `"max" is not reached by whole steps from "min"; the last stop will be ${r.min + (n - 1) * r.step}.`);
        if (r.default < r.min || r.default > r.max) E(w, `"default" (${r.default}) is outside ${r.min}..${r.max}.`);
        else if (Math.abs(((r.default - r.min) / r.step) % 1) > 1e-9 && Math.abs(((r.default - r.min) / r.step) % 1) < 1 - 1e-9) W(w, `"default" (${r.default}) is between steps; it will snap to the nearest stop.`);
      }
      if (r.unit !== undefined && !isStr(r.unit, 1, 8)) E(w, '"unit" needs 1-8 characters (for example "%").');
    }

    // binding
    const b = r.bind;
    if (b === undefined) { E(w, 'needs a "bind" saying where the value goes: {"type": "tuning", ...}, {"type": "api"} or {"type": "dynasty", ...}.'); return; }
    if (b === null || typeof b !== 'object' || !BIND_TYPES.includes(b.type)) { E(w + '.bind', `"type" must be one of ${BIND_TYPES.join(', ')}.${b && b.type ? didYouMean(b.type, BIND_TYPES) : ''}`); return; }
    const bw = w + '.bind';
    unknown(b, KNOWN[b.type], bw);
    if (b.type === 'tuning') {
      if (!isStr(b.asset, 1, 200)) E(bw, '"asset" is the tuning asset\'s path, for example "blocking/BlockingTuning_Main".');
      if (!isStr(b.field, 1, 200)) E(bw, '"field" is the name of the field to change inside that asset.');
      if (b.instance !== undefined && !isStr(b.instance, 1, 200)) E(bw, '"instance" is a path to one value inside the field, for example "Curve.Points[2].y".');
      if (!COPIES.includes(b.copy)) E(bw, `"copy" is required and must be "simulation", "base" or "both". The game keeps a Simulation copy and a base copy of tuning data; only you know which one your mod changes, so name it. (You wrote ${JSON.stringify(b.copy)}.)`);
      if (r.type === 'slider') {
        const s = b.scale;
        if (!s || typeof s['at-min'] !== 'number' || typeof s['at-max'] !== 'number') E(bw, 'a slider needs "scale": {"at-min": <field value at the left end>, "at-max": <field value at the right end>}.');
        if (b.values !== undefined) W(bw, '"values" is for toggles and lists; a slider uses "scale".');
      } else if (r.type === 'toggle' || r.type === 'list') {
        const need = r.type === 'toggle' ? 2 : n;
        if (!Array.isArray(b.values) || (need && b.values.length !== need)) E(bw, `"values" needs exactly ${need} entries, one field value for each ${r.type === 'toggle' ? 'of Off then On' : 'option, in order'}.`);
        else b.values.forEach((v, j) => { if (!['number', 'boolean', 'string'].includes(typeof v)) E(bw, `values[${j}] must be a number, true/false, or text.`); });
        if (b.scale !== undefined) W(bw, '"scale" is for sliders; a toggle or list uses "values".');
      }
    } else if (b.type === 'dynasty') {
      if (typeof b.slot !== 'string' || !/^[a-z0-9][a-z0-9-]{1,39}$/.test(b.slot)) E(bw, '"slot" names your line in the shared save slot list (ask us for one before you ship), for example "my-mod-fast".');
      if (n > 8) E(bw, `a dynasty row can hold at most 8 values; this row has ${n}. Use fewer options, a bigger slider step, or bind it with "api".`);
    }
  });
  if (live === 0) W('rows', 'there are no toggle, list or slider rows, only headers and info. Nothing is adjustable.');
  return { errors, warnings };
}

// several manifests together: what the game will do with clashing ids / titles / sections. list = [{ file, m }]; returns warnings
function crossCheck(list) {
  const out = []; const ok = list.filter((x) => x.m && typeof x.m === 'object' && typeof x.m.modid === 'string');
  const share = (a, b) => { const ka = choiceKeys(a), kb = choiceKeys(b); return (!ka.length && !kb.length) || ka.some((k) => kb.includes(k)); };
  ok.forEach((x, i) => ok.slice(i + 1).forEach((y) => {
    const ix = normId(x.m.modid), iy = normId(y.m.modid);
    if (ix === iy) {
      if (normTitle(x.m.title) === normTitle(y.m.title) && share(x.m, y.m)) {
        const c = verCmp(x.m.version, y.m.version); const keep = c >= 0 ? x : y, drop = c >= 0 ? y : x;
        out.push(`${drop.file}: the same mod as ${keep.file} (modid "${ix}"): the game uses only the highest version (${keep.m.version}) and ignores this copy (${drop.m.version}).`);
      } else out.push(`${y.file}: uses the modid "${ix}" of a different mod (${x.file}). Both are shown, but this one's settings are stored as "${ix}@<its file name>" and its header says "duplicate id". Pick your own modid ("yourname-modname").`);
    } else if (normTitle(x.m.title) && normTitle(x.m.title) === normTitle(y.m.title)) out.push(`${x.file} / ${y.file}: the same visible title "${x.m.title}" on two mods: each header shows its source file. A more distinct title reads better.`);
  }));
  for (const x of ok) {
    const sv = typeof x.m.section === 'string' ? x.m.section : null; if (!sv || sv[0] === '@') continue;
    const owner = ok.find((y) => y !== x && normId(y.m.modid) === normId(sv));
    if (owner && owner.m.closed === true) out.push(`${x.file}: "section" "${sv}" is closed by ${owner.file}: these rows show in this mod's own section, with a note.`);
    else if (owner && owner.m.section !== undefined) out.push(`${x.file}: "section" "${sv}" (${owner.file}) puts its own rows elsewhere: these rows stay in this mod's own section.`);
  }
  return out;
}

function checkText(text, label) {
  const ex = extract(text);
  if (ex.error) return { errors: [`${label}: ${ex.error}`], warnings: [] };
  let m;
  try { m = JSON.parse(ex.json); }
  catch (e) {
    const pos = /position (\d+)/.exec(e.message);
    let where = '';
    if (pos) { const upTo = ex.json.slice(0, +pos[1]); const line = upTo.split('\n').length; where = ` Look near line ${line} of the ${ex.from}: a missing comma, a stray comma before } or ], or a quote that is not closed.`; }
    return { errors: [`${label}: this is not valid JSON.${where} (${e.message})`], warnings: [] };
  }
  const res = validate(m);
  return res;
}

function selftest() {
  let pass = 0, fail = 0;
  const t = (name, cond) => { if (cond) pass++; else { fail++; console.log('FAIL: ' + name); } };
  const base = () => ({ schema: 3, modid: 'demo-mod', title: 'Demo', version: '1.0.0', rows: [
    { type: 'toggle', key: 'a', label: 'A', description: 'x', default: true, bind: { type: 'api' } }] });
  const errs = (m) => validate(m).errors;
  const warns = (m) => validate(m).warnings;

  t('minimal ok', errs(base()).length === 0);
  let m = base(); m.schema = 2; t('schema 2 rejected', errs(m).some(e => e.startsWith('schema')));
  m = base(); m.modid = 'Bad Id'; t('modid with a space: read as bad-id (warning, not error)', errs(m).length === 0 && warns(m).some(w => /read as "bad-id"/.test(w)));
  m = base(); m.title = ''; t('empty title', errs(m).some(e => e.startsWith('title')));
  m = base(); m.rows = []; t('no rows', errs(m).some(e => e.startsWith('rows')));
  m = base(); m.rows.push({ ...m.rows[0] }); t('duplicate key', errs(m).some(e => /used twice/.test(e)));
  m = base(); delete m.rows[0].bind; t('missing bind', errs(m).some(e => /needs a "bind"/.test(e)));
  m = base(); m.rows[0].bind = { type: 'tunning' }; t('bind typo hint', errs(m).some(e => /Did you mean "tuning"/.test(e)));
  m = base(); m.rows[0].lable = 'x'; t('unknown field warns with hint', warns(m).some(w => /Did you mean "label"/.test(w)));
  m = base(); m.rows[0].bind = { type: 'tuning', asset: 'a/b', field: 'F', values: [false, true] };
  t('tuning without copy', errs(m).some(e => /"copy" is required/.test(e)));
  m.rows[0].bind.copy = 'simulation'; t('tuning ok', errs(m).length === 0);
  m.rows[0].bind.values = [true]; t('tuning values count', errs(m).some(e => /exactly 2/.test(e)));
  m = base(); m.rows.push({ type: 'list', key: 'l', label: 'L', description: 'd', options: ['A', 'B', 'C'], default: 3, bind: { type: 'api' } });
  t('list default range', errs(m).some(e => /position, from 0 to 2/.test(e)));
  m = base(); m.rows.push({ type: 'slider', key: 's', label: 'S', description: 'd', min: 0, max: 100, step: 10, default: 55, bind: { type: 'api' } });
  t('slider off-step warns', warns(m).some(w => /between steps/.test(w)) && errs(m).length === 0);
  m.rows[1].default = 500; t('slider default out of range', errs(m).some(e => /outside 0..100/.test(e)));
  m = base(); m.rows.push({ type: 'slider', key: 's', label: 'S', description: 'd', min: 0, max: 1, step: 0.1, default: 0.5, bind: { type: 'tuning', asset: 'a', field: 'f', copy: 'base' } });
  t('slider needs scale', errs(m).some(e => /"scale"/.test(e)));
  m.rows[1].bind.scale = { 'at-min': 0, 'at-max': 2 }; t('slider scale ok', errs(m).length === 0);
  m = base(); m.rows.push({ type: 'slider', key: 's', label: 'S', description: 'd', min: 0, max: 100, step: 5, default: 5, bind: { type: 'dynasty', slot: 'demo-s' } });
  t('dynasty > 8 values', errs(m).some(e => /at most 8/.test(e)));
  m = base(); m.rows.unshift({ type: 'header', label: 'H' }); t('header ok', errs(m).length === 0);
  m.rows[0].default = 1; t('header extra field warns', warns(m).some(w => /does nothing/.test(w)));
  m = base(); m.rows[0].type = 'togle'; t('row type hint', errs(m).some(e => /Did you mean "toggle"/.test(e)));
  t('non-object', validate([]).errors.length === 1);

  // text handling
  let r = checkText('{ "schema": 3, ', 'x.json'); t('bad json message', r.errors.length === 1 && /not valid JSON/.test(r.errors[0]));
  r = checkText('Cool mod.\n[[MODSETTINGS\n' + JSON.stringify(base()) + '\nMODSETTINGS]]\nMore text', 'd.txt'); t('marker block read', r.errors.length === 0);
  r = checkText('[[MODSETTINGS {}', 'd.txt'); t('unclosed marker', /no closing/.test(r.errors[0]));

  // names, duplicates and clashes
  m = base(); m.modid = 'My_Mod.Two'; t('modid normalised: no error, a warning naming the id', errs(m).length === 0 && warns(m).some(w => /read as "my-mod-two"/.test(w)));
  m = base(); m.modid = '!?'; t('modid that normalises to nothing: error', errs(m).some(e => e.startsWith('modid')));
  m = base(); m.rows.push({ ...m.rows[0], label: 'Second' }); t('duplicate key inside one mod: error', errs(m).some(e => /used twice/.test(e)));
  const M = (modid, title, version, keys, extra = {}) => ({ schema: 3, modid, title, version, ...extra, rows: keys.map((k) => ({ type: 'toggle', key: k, label: k, description: 'd', default: true, bind: { type: 'api' } })) });
  let cw = crossCheck([{ file: 'a.json', m: M('same', 'Same Mod', '1.2.0', ['x']) }, { file: 'b.json', m: M('same', 'same mod', '1.10.0', ['x', 'y']) }]);
  t('same modid + same mod: the older copy is named, the highest version kept', cw.length === 1 && /a\.json: the same mod as b\.json .*highest version \(1\.10\.0\).*\(1\.2\.0\)/.test(cw[0]));
  cw = crossCheck([{ file: 'a.json', m: M('clash', 'Alpha', '1', ['a']) }, { file: 'b.json', m: M('Clash', 'Beta', '1', ['b']) }]);
  t('same modid + different mods: stored as modid@file, duplicate id', cw.length === 1 && /b\.json: uses the modid "clash".*"clash@<its file name>".*duplicate id/.test(cw[0]));
  cw = crossCheck([{ file: 'a.json', m: M('one', 'Penalties', '1', ['a']) }, { file: 'b.json', m: M('two', 'PENAL TIES', '1', ['b']) }]);
  t('matching titles, different modids: each header shows its source', cw.length === 1 && /same visible title/.test(cw[0]));
  t('no clash: no warning', crossCheck([{ file: 'a.json', m: M('one', 'One', '1', ['a']) }, { file: 'b.json', m: M('two', 'Two', '1', ['b']) }]).length === 0);
  t('version order: digit runs as numbers', verCmp('1.10.0', '1.2.0') > 0 && verCmp('1.2', '1.2.1') < 0 && verCmp('2.0.0', '2.0.0') === 0);

  // adding rows to another mod's section
  m = base(); m.section = 'other-mod'; t('section = another modid: ok', errs(m).length === 0);
  m = base(); m.section = '@pass-rush'; t('section = a shared @name: ok', errs(m).length === 0);
  m = base(); m.section = 5; t('section not text: error', errs(m).some(e => e.startsWith('section')));
  m = base(); m.section = '@!'; t('section @name empty after normalising: error', errs(m).some(e => e.startsWith('section')));
  m = base(); m.section = 'demo-mod'; t('section = own modid: warning', warns(m).some(w => /your own modid/.test(w)));
  m = base(); m.closed = 'yes'; t('closed not true / false: error', errs(m).some(e => e.startsWith('closed')));
  m = base(); m.closed = true; t('closed true on an owner: ok', errs(m).length === 0 && !warns(m).some(w => w.startsWith('closed')));
  m = base(); m.closed = true; m.section = '@kicking'; t('closed + section: warning', warns(m).some(w => w.startsWith('closed')));
  cw = crossCheck([{ file: 'owner.json', m: M('owner', 'Owner', '1', ['o'], { closed: true }) }, { file: 'addon.json', m: M('addon', 'Addon', '1', ['late-hits'], { section: 'Owner' }) }]);
  t('section into a closed owner: warned (own section + note)', cw.length === 1 && /addon\.json: "section" "Owner" is closed by owner\.json/.test(cw[0]));
  cw = crossCheck([{ file: 'owner.json', m: M('owner', 'Owner', '1', ['late-hits']) }, { file: 'addon.json', m: M('addon', 'Addon', '1', ['late-hits'], { section: 'owner' }) }]);
  t('section into an open owner, same key: no warning (stored under each modid)', cw.length === 0);

  // the shipped samples
  const dir = path.join(__dirname, '..', 'modsettings-dll', 'samples');
  if (fs.existsSync(dir)) for (const f of fs.readdirSync(dir).filter(x => x.endsWith('.json'))) {
    const rr = checkText(fs.readFileSync(path.join(dir, f), 'utf8'), f);
    t('sample ' + f + ' valid', rr.errors.length === 0);
  }
  console.log(`modsettings-validate selftest: ${pass} passed, ${fail} failed`);
  return fail === 0;
}

function main() {
  const args = process.argv.slice(2);
  if (args.includes('--selftest')) process.exit(selftest() ? 0 : 1);
  const files = args.filter(a => !a.startsWith('--'));
  if (!files.length) { console.log('usage: node tools/validate.js <manifest.json> [...]  |  --selftest  [--json]'); process.exit(2); }
  const asJson = args.includes('--json');
  let bad = 0, unread = 0; const out = [];
  for (const f of files) {
    let text;
    try { text = fs.readFileSync(f, 'utf8').replace(/^﻿/, ''); } catch (e) { console.log(`${f}: cannot read the file (${e.code || e.message}).`); unread++; continue; }
    const r = checkText(text, path.basename(f));
    out.push({ file: f, ...r });
    if (!asJson) {
      for (const e of r.errors) console.log(`ERROR   ${path.basename(f)}  ${e}`);
      for (const w of r.warnings) console.log(`warning ${path.basename(f)}  ${w}`);
      console.log(`${path.basename(f)}: ${r.errors.length ? 'NOT OK, ' + r.errors.length + ' error(s); the game will skip this manifest until fixed' : 'OK'}${r.warnings.length ? ', ' + r.warnings.length + ' warning(s)' : ''}`);
    }
    if (r.errors.length) bad++;
  }
  if (out.length > 1) {   // several manifests: how the game treats clashing ids, titles and sections
    const list = out.map((o) => { try { return { file: path.basename(o.file), m: JSON.parse(extract(fs.readFileSync(o.file, 'utf8').replace(/^\uFEFF/, '')).json) }; } catch (e) { return { file: o.file, m: null }; } });
    const cw = crossCheck(list); if (asJson) out.push({ cross: cw }); else for (const w of cw) console.log(`warning ${w}`);
  }
  if (asJson) console.log(JSON.stringify(out, null, 2));
  process.exit(unread ? 2 : bad ? 1 : 0);
}
if (require.main === module) main();
module.exports = { validate, checkText, extract, crossCheck, normId, verCmp };
