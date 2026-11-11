#!/usr/bin/env node
// gen-pins.js: the pins of modsettings.dll, from the reference memory dumps (one entry per known game build).
//   node gen-pins.js <dump> [<dump> ...]   -> ../src/ms_pins.h + ../src/patchhashes.h
// What it pins, per build (PE timestamp):
//   * three game functions the band writer calls, each by its first 24 bytes: FindUser (the generic server-side user lookup, 247 callers),
//     the ARRY count, the record "changed" counter bump every native int write is followed by. RVAs: the ones wsdata verified on both
//     builds (docs/WSDATA-DLL-2026-10-28.md section 10); the bytes are re-read here and must match wsdata's.
//   * the capture sites: the RPC thunks' handler calls of the flows in CAPTURE (found by the thunk shape + the response name the thunk
//     names 0xd0 bytes before its call, Press Box's method), each with its handler's first 24 bytes, the name string's RVA, and the
//     64-byte window hash patchsafe.h checks (FNV-1a over [site-32, site+32) with the site's own 5 bytes zeroed).
//   * each handler must call FindUser in its first 0x60 bytes (the generic lookup the band writer repeats with the handler's own flow / a2).
// Any miss throws: a build whose code does not have the expected shape gets no pins (the DLL refuses it).
'use strict';
const fs = require('fs'), path = require('path');
const OUT = path.join(__dirname, '..', 'src');
// the flows whose server handler the DLL wraps (none is patched by another DLL of ours: Dynasty Hooks = DepthChartFlow, Press Box =
// SeasonGameFlow, wsdata = PracticeHomeFlow; checked by a source grep 2026-11-07)
const CAPTURE = ['MetersFlow::RefreshFormResponse', 'MySchoolFlow::MySchoolRefreshFormResponse', 'RosterFlow::EnterResponse', 'TeamScheduleFlow::EnterResponse'];
// wsdata's verified RVAs (wsdata_pins.h WSD_BUILDS): FINDUSER, ARRCOUNT, DIRTY
const KNOWN = {
  1791226072: { name: 'TU 10-13', FINDUSER: 0x919d540, ARRCOUNT: 0x37f18a0, DIRTY: 0x8055870 },
  1792187070: { name: 'TU 10-29', FINDUSER: 0x91a34b0, ARRCOUNT: 0x37f40a0, DIRTY: 0x8059580 },
};
const WSD_HEADS = {   // the same 24 bytes wsdata pins (a cross-check: both tools must agree on these functions)
  FINDUSER: '40534883ec2080b9d800000000498bd87449488b49784c8b',
  ARRCOUNT: '48895c2410488974241857 4883ec20488b5910488bf14885'.replace(/ /g, ''),
  DIRTY: '488b4110ff80a0010000c3cccccccccc48895c2408574883',
};
const SIG = Buffer.from([0x48, 0x8b, 0x95, 0x20, 0x01, 0x00, 0x00, 0x4d, 0x8d, 0x46, 0x28, 0x4c, 0x8b, 0xcf, 0x48, 0x8b, 0xcb, 0xe8]);
const NAME_BACK = 0xd0;
// Layer 2 tab feed (2026-11-11): the native settings adapter (SettingsAdapterClass). Found per dump from its glue registration table
// ({glue fn, name, 0} entries; the glue names the class "SettingsAdapterClass" in r9 and its marshaller in r8), then each marshaller:
// the value holder it hands the slot (lea rdx), the adapter global (mov rcx|rax, [rip]), the Variant wrapper vtable (the one lea all five
// share), the vtable slot it calls (call [rax+N] or mov rbx, [rdx+N]; call rbx). The whole adapter vtable (every slot up to the first
// pointer that is not code) is pinned as stock RVAs; the DLL wraps only the five slots below and only for its private category.
const TAB_METHODS = [['GetListSettings', 0x30, 'arr'], ['GetToggleDetails', 0x38, 'arr'], ['GetSliderDetails', 0x40, 'obj'], ['GameAdjustToggleSetting', 0x50, 'arr'], ['GameAdjustSliderSetting', 0x58, 'arr']];
function tabPins(buf, secs, name) {
  const code = (rva) => secs.some((s) => (s.ch & 0x20000000) && rva >= s.rva && rva < s.rva + s.vsize);
  const strAt = (s) => { const pat = Buffer.from(s + '\0', 'latin1'); const out = []; let p = -1; while ((p = buf.indexOf(pat, p + 1)) !== -1) if (buf[p - 1] === 0) out.push(p); return out; };
  const q = (rva) => Number(buf.readBigUInt64LE(rva) - 0x140000000n);
  const ripT = (at, len) => at + len + buf.readInt32LE(at + len - 4);
  const cls = strAt('SettingsAdapterClass'); if (cls.length !== 1) throw new Error(`${name}: "SettingsAdapterClass" found ${cls.length} times`);
  const out = { slots: {} }; const vars = [];
  for (const [m, wantSlot, kind] of TAB_METHODS) {
    const names = strAt(m); const glues = [];
    for (const n of names) {
      const pat = Buffer.alloc(8); pat.writeBigUInt64LE(BigInt(n) + 0x140000000n); let p = -1;
      while ((p = buf.indexOf(pat, p + 1)) !== -1) {
        if (p < 8) continue; const g = Number(buf.readBigUInt64LE(p - 8) - 0x140000000n); if (!(g > 0 && g < buf.length - 64) || !code(g)) continue;
        let r9 = -1, r8 = -1; for (let i = g; i < g + 0x30; i++) { if (buf[i] === 0x4c && buf[i + 1] === 0x8d && buf[i + 2] === 0x0d) r9 = ripT(i, 7); if (buf[i] === 0x4c && buf[i + 1] === 0x8d && buf[i + 2] === 0x05) r8 = ripT(i, 7); }
        if (r9 === cls[0] && r8 > 0 && code(r8)) glues.push({ glue: g, marsh: r8 });
      }
    }
    if (glues.length !== 1) throw new Error(`${name}: ${m}: ${glues.length} SettingsAdapterClass glue(s) (want exactly 1)`);
    const M = glues[0].marsh; let holder = -1, glob = -1, slot = -1; const leas = [];
    for (let i = M; i < M + 0x1c0; i++) {
      if (buf[i] === 0x48 && buf[i + 1] === 0x8d && buf[i + 2] === 0x15) holder = ripT(i, 7);
      if (glob < 0 && buf[i] === 0x48 && buf[i + 1] === 0x8b && (buf[i + 2] === 0x0d || buf[i + 2] === 0x05)) { const t = ripT(i, 7); if (!code(t)) glob = t; }
      if (buf[i] === 0x48 && buf[i + 1] === 0x8d && [0x05, 0x1d, 0x3d].includes(buf[i + 2])) leas.push(ripT(i, 7));
      if (slot < 0 && buf[i] === 0xff && buf[i + 1] === 0x50) slot = buf[i + 2];
      if (slot < 0 && buf[i] === 0x48 && buf[i + 1] === 0x8b && buf[i + 2] === 0x5a) slot = buf[i + 3];
      if (buf[i] === 0xc3 && buf[i + 1] === 0xcc) break;
    }
    if (slot !== wantSlot) throw new Error(`${name}: ${m}: the marshaller calls slot 0x${slot.toString(16)} (want 0x${wantSlot.toString(16)})`);
    if (holder < 0 || glob < 0) throw new Error(`${name}: ${m}: holder / adapter global not found in the marshaller`);
    if (out.glob !== undefined && out.glob !== glob) throw new Error(`${name}: ${m}: another adapter global`);
    out.glob = glob; out[kind] = out[kind] === undefined ? holder : out[kind];
    if (out[kind] !== holder) throw new Error(`${name}: ${m}: a different ${kind} holder`);
    vars.push(new Set(leas.filter((t) => t !== holder && !code(t) && code(q(t)))));
    out.slots[m] = { slot, glue: glues[0].glue, marsh: M };
  }
  const shared = [...vars[0]].filter((t) => vars.every((s) => s.has(t)));
  if (shared.length !== 1) throw new Error(`${name}: the Variant wrapper vtable: ${shared.length} candidates shared by all five marshallers`);
  out.varVt = shared[0];
  out.obj0 = q(out.glob); out.vt = q(out.obj0);
  if (!(out.vt > 0) || !code(q(out.vt))) throw new Error(`${name}: the adapter object (0x${(0x140000000 + out.obj0).toString(16)}) has no code vtable`);
  out.stock = []; for (let k = 0; k < 64 && code(q(out.vt + 8 * k)); k++) out.stock.push(q(out.vt + 8 * k));
  if (out.stock.length < 0x60 / 8) throw new Error(`${name}: the adapter vtable has only ${out.stock.length} code slots`);
  out.arrVt = q(out.arr); out.objVt = q(out.obj);
  if (!code(q(out.arrVt)) || !code(q(out.objVt))) throw new Error(`${name}: a holder vtable is not code`);
  // each wrapped slot is the context forwarder (manager pick, then jmp [rax|r10 + N]) on this build: the shape the wrappers were written against
  for (const [m, s] of TAB_METHODS) { const f = out.stock[s / 8]; const b = buf.subarray(f, f + 0x70); if (!b.includes(Buffer.from([0xe8])) || !(b.includes(Buffer.from([0xff, 0x62])) || b.includes(Buffer.from([0xff, 0x60])))) throw new Error(`${name}: slot 0x${s.toString(16)} (${m}) at 0x${(0x140000000 + f).toString(16)} is not the manager forwarder`); }
  return out;
}
function fnv(buf) { let h = 0xcbf29ce484222325n; for (const b of buf) { h ^= BigInt(b); h = (h * 0x100000001b3n) & 0xffffffffffffffffn; } return h; }
function cstr(buf, off) { let e = off; while (e < buf.length && buf[e] >= 0x20 && buf[e] < 0x7f && e - off < 200) e++; return buf[e] === 0 ? buf.toString('latin1', off, e) : null; }
function pins(file) {
  const buf = fs.readFileSync(file);
  const pe = buf.readUInt32LE(0x3c); if (buf.readUInt32LE(pe) !== 0x4550) throw new Error(file + ': not a PE image dump');
  const stamp = buf.readUInt32LE(pe + 8), nsec = buf.readUInt16LE(pe + 6), optsz = buf.readUInt16LE(pe + 20);
  const k = KNOWN[stamp]; if (!k) throw new Error(`${file}: PE timestamp ${stamp} is not a build this tool knows (add it to KNOWN after verifying the three functions)`);
  const secs = []; for (let i = 0; i < nsec; i++) { const s = pe + 24 + optsz + 40 * i; secs.push({ name: buf.toString('latin1', s, s + 8).replace(/\0+$/, ''), rva: buf.readUInt32LE(s + 12), vsize: buf.readUInt32LE(s + 8), ch: buf.readUInt32LE(s + 36) }); }
  const funcs = {};
  for (const f of ['FINDUSER', 'ARRCOUNT', 'DIRTY']) {
    const head = buf.subarray(k[f], k[f] + 24);
    if (head.toString('hex') !== WSD_HEADS[f]) throw new Error(`${k.name}: ${f} at 0x${k[f].toString(16)} is not the code wsdata pinned`);
    funcs[f] = { rva: k[f], head };
  }
  const hits = [];
  for (const s of secs.filter((x) => x.ch & 0x20000000)) { let p = s.rva - 1; const end = Math.min(s.rva + s.vsize, buf.length); while ((p = buf.indexOf(SIG, p + 1)) !== -1 && p < end) hits.push(p + 17); }
  const sites = [];
  for (const name of CAPTURE) {
    const found = hits.filter((call) => { const q = call - NAME_BACK; return buf[q] === 0x4c && buf[q + 1] === 0x8d && buf[q + 2] === 0x05 && cstr(buf, q + 7 + buf.readInt32LE(q + 3)) === name; });
    if (found.length !== 1) throw new Error(`${k.name}: ${name}: ${found.length} thunk(s) (want exactly 1)`);
    const call = found[0], q = call - NAME_BACK, nameRva = q + 7 + buf.readInt32LE(q + 3);
    const handler = call + 5 + buf.readInt32LE(call + 1);
    let fu = -1; for (let i = handler; i < handler + 0x60; i++) if (buf[i] === 0xe8 && i + 5 + buf.readInt32LE(i + 1) === k.FINDUSER) { fu = i; break; }
    if (fu < 0) throw new Error(`${k.name}: ${name}: the handler 0x${handler.toString(16)} does not call FindUser in its first 0x60 bytes`);
    const w = Buffer.from(buf.subarray(call - 32, call + 32)); w.fill(0, 32, 37);
    sites.push({ name, call, handler, head: buf.subarray(handler, handler + 24), nameRva, hash: fnv(w), fuAt: fu - handler });
  }
  const tab = tabPins(buf, secs, k.name);
  return { file, stamp, name: k.name, funcs, sites, tab };
}
const hexb = (b) => [...b].map((x) => '0x' + x.toString(16).padStart(2, '0')).join(',');
const dumps = process.argv.slice(2); if (!dumps.length) { console.error('usage: node gen-pins.js <dump> [<dump> ...]'); process.exit(2); }
const B = dumps.map(pins).sort((a, b) => a.stamp - b.stamp);
let h = `/* ms_pins.h: GENERATED by tools/gen-pins.js from ${B.map((b) => path.basename(b.file)).join(', ')}. Do not edit.
 * One MsBuild per known game build (PE timestamp). modsettings.dll picks the build by the running game's timestamp and installs nothing
 * unless every pin of that build matches (ms_pins_check in modsettings.c). */
#ifndef MS_PINS_H
#define MS_PINS_H
#define MS_NFUNCS 3
#define MS_F_FINDUSER 0
#define MS_F_ARRCOUNT 1
#define MS_F_DIRTY 2
#define MS_NSITES ${CAPTURE.length}
typedef struct { const char *name; uint32_t rva; unsigned char head[24]; } MsFuncPin;
typedef struct { const char *flow; uint32_t call_rva, handler_rva, name_rva; uint64_t hash; unsigned char head[24]; } MsSitePin;
/* Layer 2 tab feed: the native settings adapter. vt_stock = every slot as an RVA (all must be stock before any slot is wrapped). */
#define MS_TAB_VT_MAX 40
typedef struct { uint32_t vt_rva, global_rva, obj_rva, arr_rva, arr_vt_rva, objh_rva, objh_vt_rva, var_vt_rva; int nvt; uint32_t vt_stock[MS_TAB_VT_MAX]; } MsTabPin;
typedef struct { const char *name; uint32_t stamp; MsFuncPin funcs[MS_NFUNCS]; MsSitePin sites[MS_NSITES]; MsTabPin tab; } MsBuild;
#define MS_NBUILDS ${B.length}
static const MsBuild MS_BUILDS[MS_NBUILDS] = {
`;
for (const b of B) {
  h += `  { "${b.name}", ${b.stamp}u,\n    {\n`;
  for (const f of ['FINDUSER', 'ARRCOUNT', 'DIRTY']) h += `      { "${f}", 0x${b.funcs[f].rva.toString(16)}u, { ${hexb(b.funcs[f].head)} } },   /* 0x${(0x140000000 + b.funcs[f].rva).toString(16)} */\n`;
  h += `    },\n    {\n`;
  for (const s of b.sites) h += `      { "${s.name}", 0x${s.call.toString(16)}u, 0x${s.handler.toString(16)}u, 0x${s.nameRva.toString(16)}u, 0x${s.hash.toString(16).padStart(16, '0')}ull,\n        { ${hexb(s.head)} } },   /* call 0x${(0x140000000 + s.call).toString(16)} -> handler 0x${(0x140000000 + s.handler).toString(16)} (FindUser at +0x${s.fuAt.toString(16)}) */\n`;
  const t = b.tab, X = (v) => `0x${v.toString(16)}u`, A = (v) => `0x${(0x140000000 + v).toString(16)}`;
  h += `    },\n    { ${X(t.vt)}, ${X(t.glob)}, ${X(t.obj0)}, ${X(t.arr)}, ${X(t.arrVt)}, ${X(t.obj)}, ${X(t.objVt)}, ${X(t.varVt)}, ${t.stock.length},\n`;
  h += `      /* settings adapter vtable ${A(t.vt)}, global ${A(t.glob)} -> object ${A(t.obj0)}; array holder ${A(t.arr)} (vt ${A(t.arrVt)}), object holder ${A(t.obj)} (vt ${A(t.objVt)}), Variant vt ${A(t.varVt)} */\n`;
  h += `      { ${t.stock.map(X).join(',')} } } },\n`;
}
h += `};\n#endif\n`;
let ph = `/* patchhashes.h: GENERATED by tools/gen-pins.js. The code sites modsettings.dll writes: one 5-byte handler call per capture flow, one row per
 * known build. patchsafe.h refuses a call patch whose window hash differs from its row (another patcher, or another build). */
#define PS_EXPECT_N ${B.length * CAPTURE.length}
static const PsExpect PS_EXPECT[${B.length * CAPTURE.length}] = {
`;
for (const b of B) for (const s of b.sites) ph += `  { ${b.stamp}u, 0x${s.call.toString(16)}u, 5, 0x${s.hash.toString(16).padStart(16, '0')}ull, "modsettings:${s.name}" },   /* ${b.name} */\n`;
ph += `};\n`;
fs.writeFileSync(path.join(OUT, 'ms_pins.h'), h); fs.writeFileSync(path.join(OUT, 'patchhashes.h'), ph);
console.log(`pins: ${B.map((b) => `${b.name} (PE ${b.stamp}): ${b.sites.length} capture sites, ${Object.keys(b.funcs).length} functions, settings adapter vtable 0x${(0x140000000 + b.tab.vt).toString(16)} (${b.tab.stock.length} slots; ${Object.entries(b.tab.slots).map(([m, x]) => m + ' 0x' + x.slot.toString(16)).join(', ')})`).join('; ')} -> src/ms_pins.h, src/patchhashes.h`);
