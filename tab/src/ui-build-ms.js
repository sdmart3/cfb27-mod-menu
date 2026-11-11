#!/usr/bin/env node
// ui-build-ms.js: the Mod Settings page (docs/design.md, Built section).
//
// The page is the game's OWN settings screen: fb_logic_com.model.settings.GenericSettings (common/ui/fb_node_com/scriptpod.ast, shipped by no mod
// in a 309-file survey) driving art_screen.settings.SettingsList (Accessibility, Game Options, ...). Two splices in a class copy of GenericSettings
// (the Press Box / Dynasty Extensions class-copy method: decode, insert, extend the pool, append the copy, repoint the frame item):
//
//   _SettingsManagerSettingsHubInitFinish (the Settings hub builds its tab bar from the native tab list): right before the tabs go to the tab provider,
//     one more tab is appended: a copy of the last native tab (cafe.utility.ObjectUtil.MergeInto) with key "MOD_SETTINGS", display "Mod Settings".
//     The hub's tab change sets mSettingsCategory = the tab's key and restarts the settings manager (stock code), so our tab gets category MOD_SETTINGS.
//   _GetSettingsManagerData (called for every category start, the hub's tabs included): when mSettingsCategory is MOD_SETTINGS, OverrideAdapter and
//     this.mFeatureAdapter = our adapter OBJECT (built once per screen, kept in this.mMsA); any other category: this.mFeatureAdapter = the native
//     settings adapter again (what Init sets). GenericSettings and logic_com.utility.FeatureManager call the adapter ONLY by method name
//     (adapter[name](...)), so an ActionScript object with those methods stands in for the native one (the stock GameplanAdjustments model builds such
//     an object inline when its native adapter is missing: the same pattern).
//
// The adapter object: { R: the row table (compiled in from the manifests), and the methods }:
//   AddNotifyListener / RemoveNotifyListener / GameInitializeSettingCategory (= the request scan Z) / GetIsFeatureInitialized (true) / GetListSettings
//   (one header per mod + one toggle row per manifest row) / GetToggleDetails (the options, the current one isDefault; an inert row = one item, so the
//   stock code greys it) / GameAdjustToggleSetting (answers the row's request with ActionRequestRespond; memory rows keep the value) /
//   GameShutdownSettingCategory (calls the FeatureManager's Notify(adapterName, "FinishedShutdownSettingCategory", key) at once) / the rest no-ops.
//   Values: Z() reads the open request list once (madden.ultimatecareer.Adapters.GetUcmAdapter(): GetListActionRequestIds / GetActionRequestData /
//   GetActionRequestResponseList, Description "Legacy Leaderboard"):
//     band rows      TypeName "Legacy Leaderboard", 8 forms, Priority in [prio, prio + options): value = Priority - prio   (the broker's requests)
//     request rows   the mod's own request: TypeName (when given) + its form count, value = Priority (1 - Priority for invert)   (DE / PW today)
//     newest RequestId wins; nothing found in the native list -> the FranTk request list (Playoff Customizer's protocol; DE 0.1.3 lab finding).
//   Outside a dynasty (GetSeasonInfo().seasonWeek not a number) no request is read and every request / band row shows "Open from a dynasty".
//
// New functions are anonymous DefineFunction2 records (flags 0x29 = preload this, no arguments / super, as GenericSettings' own methods), each with its
// own name string + parameter array appended to Apt Data ahead of the class copy (the loader rewrites parameter arrays in place: one array per record).
'use strict';
const fs = require('fs');
const path = require('path');
const ROOT = path.join(__dirname, '..', '..');
const apt = require(path.join(ROOT, 'tools', 'apt.js'));
const bgfa = require(path.join(ROOT, 'tools', 'bgfa.js'));
const PB = require(path.join(ROOT, 'spex', 'probes', 'gen-pressbox-ui.js'));
const { makeDsl, findFn, T, copyClass, splice, must } = PB._internals;

const FILE = 'common/ui/fb_node_com/scriptpod.ast';
const CLASS = 'fb_logic_com.model.settings.GenericSettings';
const MS_CAT = 'MOD_SETTINGS';
const D_NAME = 'Legacy Leaderboard';
const FORMS = 8;

// ------------------------------------------------------------------------------------------------ the AS helper layer
function mkAs(c) {
  const added = [];
  const name = (s) => { if (!c.pool.includes(s)) { c.pool.push(s); added.push(s); } return s; };
  const D = makeDsl(c.pool);
  const { X, reg, byte, jump, L, short } = D;
  const C = (s) => D.C(name(s));
  const get = (s) => [C(s), X('GetMember')];
  const st = (r) => [D.B.storeReg(r), X('Pop')];
  let n = 0; const nl = (p) => `${p}.${++n}`;
  const num = (v) => { must(Number.isInteger(v), `num ${v}`); return v >= -128 && v <= 127 ? byte(v) : (must(v >= -32768 && v <= 32767, `short range ${v}`), short(v)); };
  const A = {
    C, get, st, reg, X, num, nl, added, D,
    U: () => X('PushUndefined'), T: () => X('PushTrue'), F: () => X('PushFalse'),
    G: (...names) => [X('PushGlobal'), ...names.map(get)],
    m: (obj, nm) => [obj, get(nm)],
    ix: (obj, k) => [obj, k, X('GetMember')],
    call: (obj, nm, args = []) => [...args.slice().reverse(), num(args.length), obj, C(nm), X('CallMethod')],
    callp: (obj, nm, args = []) => [A.call(obj, nm, args), X('Pop')],
    set: (obj, nm, v) => [obj, C(nm), v, X('SetMember')],
    seti: (obj, k, v) => [obj, k, v, X('SetMember')],
    sto: (r, v) => [v, st(r)],
    eq: (a, b) => [a, b, X('Equals2')], lt: (a, b) => [a, b, X('Less2')], gt: (a, b) => [a, b, X('Greater')],
    add: (a, b) => [a, b, X('Add2')], sub: (a, b) => [a, b, X('Subtract')], not: (a) => [a, X('Not')],
    isU: (a) => [a, X('PushUndefined'), X('Equals2')],
    isFn: (a) => [a, X('TypeOf'), C('function'), X('Equals2')],
    // IF(cond, then, else); cond is an instruction list leaving a boolean
    IF: (cond, th, el) => { const e = nl('e'), x = nl('x'); return el ? [cond, jump('IfFalse', e), th, jump('Jump', x), L(e), el, L(x)] : [cond, jump('IfFalse', x), th, L(x)]; },
    // all conditions true -> th (else el)
    IFALL: (conds, th, el) => { const e = nl('e'), x = nl('x'); return [conds.map((cd) => [cd, jump('IfFalse', e)]), th, jump('Jump', x), L(e), el || [], L(x)]; },
    WHILE: (cond, body) => { const t = nl('w'), x = nl('wx'); return [L(t), cond, jump('IfFalse', x), body, jump('Jump', t), L(x)]; },
    ret: (v) => [v, X('Return')],
    newArr: () => [X('PushZero'), C('Array'), X('NewObject')],
    obj: (pairs) => [pairs.map(([k, v]) => [C(k), v]), num(pairs.length), X('InitObject')],
    arr: (els) => [els.slice().reverse(), num(els.length), X('InitArray')],
    push: (arrC, v) => A.callp(arrC, 'push', [v]),
    inc: (r) => [reg(r), X('Increment'), st(r)],
  };
  // an anonymous function: DefineFunction2 record + body; regs = the highest register used + 1; params land in r2.. (r1 = this)
  const fns = [];
  A.fn = (nparams, regs, body) => {
    const raw = Buffer.alloc(0x30);
    raw.writeUInt32LE(nparams, 8); raw[12] = regs; raw.writeUInt16LE(0x29, 14);
    raw.writeUInt32LE(0x98765432, 0x20); raw.writeUInt32LE(0x12345678, 0x28);   // the stock records' filler words
    const end = nl('fend');
    const rec = { op: 0x8e, kind: 'func', raw, endLabel: end, nparams };
    fns.push(rec);
    // every body ends on an explicit Return (a label at the very end must bind inside the body)
    return [rec, body, L(end), X('PushUndefined'), X('Return')];
  };
  A.fns = fns;
  return A;
}
// link with function body ends (PB's link + endLabel)
function link(list, tail) {
  const flat = []; const pending = []; const labels = new Map();
  (function walk(x) { if (Array.isArray(x)) x.forEach(walk); else flat.push(x); })(list);
  const out = [];
  for (const x of flat) { if (x.label !== undefined && x.op === undefined) { pending.push(x.label); continue; } for (const l of pending) labels.set(l, x); pending.length = 0; out.push(x); }
  for (const l of pending) labels.set(l, tail);
  for (const i of out) if (i.kind === 'branch' && i.label !== undefined) { const t = labels.get(i.label); must(t, `label ${i.label} unbound`); i.target = t; delete i.label; }
  // a function's body ends on the Return we put after its end label: bodyEnd = the instruction AFTER that Return
  for (const i of out) if (i.kind === 'func' && i.endLabel !== undefined) {
    const lab = labels.get(i.endLabel); must(lab, `function end ${i.endLabel} unbound`);
    const k = out.indexOf(lab); must(k >= 0 && out[k + 1] && out[k + 1].op === 0x3e, 'function end shape');
    i.bodyEnd = out[k + 2] || tail; must(i.bodyEnd, 'function body end'); delete i.endLabel;
  }
  return out;
}

// ------------------------------------------------------------------------------------------------ the adapter object
// table: manifest.load() output. Row fields in AS: k key, y 1 = header, d label, h help, o options, n option count, f default, b binding
// (0 memory, 1 band, 2 request), p prio + s span = 2^width (band), t typeName (request; undefined = any but the broker's), q forms (request), iv invert, it inert text.
function adapterObject(A, table) {
  const { C, get, reg: r, X, num } = A;
  const THIS = () => r(1);
  const rowObj = (row) => {
    if (row.kind === 'header') return A.obj([['k', num(row.key)], ['y', num(1)], ['d', C(row.label)], ['h', C(row.help || ' ')], ['n', num(0)], ['b', num(0)]]);
    const b = row.slot ? 1 : row.request ? 2 : 0;
    const pairs = [['k', num(row.key)], ['y', num(0)], ['d', C(row.label)], ['h', C(row.help || ' ')], ['o', A.arr(row.options.map((o) => C(o)))], ['n', num(row.options.length)],
      ['f', num(row.default)], ['b', num(b)], ['it', C(row.inert)]];
    if (b === 1) pairs.push(['p', num(row.slot.prio)], ['s', num(1 << row.slot.width)]);   // s: the slot's span (a stored value past the options shows the default)
    if (b === 2) { pairs.push(['q', num(row.request.responses)], ['iv', row.request.invert ? A.T() : A.F()]); if (row.request.typeName) pairs.push(['t', C(row.request.typeName)]); }
    return A.obj(pairs);
  };
  const LL = () => C(D_NAME);
  // ---- U(): the UCM adapter or undefined (r2 = madden.ultimatecareer.Adapters)
  const fnU = A.fn(0, 4, [
    A.sto(2, A.G('madden')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.sto(2, A.m(r(2), 'ultimatecareer')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.sto(2, A.m(r(2), 'Adapters')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.IF(A.not(A.isFn(A.m(r(2), 'GetUcmAdapter'))), A.ret(A.U())),
    A.ret(A.call(r(2), 'GetUcmAdapter')),
  ]);
  // ---- X(key): the row index of a UI key, -1 when none. r2 key, r3 i, r4 rows
  const fnX = A.fn(1, 6, [
    A.sto(4, A.m(THIS(), 'R')), A.sto(3, num(0)),
    A.WHILE(A.lt(r(3), A.m(r(4), 'length')), [A.IF(A.eq(A.m(A.ix(r(4), r(3)), 'k'), r(2)), A.ret(r(3))), A.inc(3)]),
    A.ret(num(-1)),
  ]);
  // ---- Lv(i): is row i live? memory rows always; request / band rows when a request was found. r2 i, r3 row
  const fnLv = A.fn(1, 5, [
    A.sto(3, A.ix(A.m(THIS(), 'R'), r(2))),
    A.IF(A.eq(A.m(r(3), 'b'), num(0)), A.ret(A.T())),
    A.ret(A.not(A.isU(A.ix(A.m(THIS(), 'i'), r(2))))),
  ]);
  // ---- M(u, id, tn, ds): one request against every row. r2 u, r3 id, r4 tn, r5 ds, r6 d, r7 pr, r8 resp, r9 nr, r10 i, r11 row, r12 val, r13 rows
  const fnM = A.fn(4, 15, [
    A.IF(A.not(A.eq(r(5), LL())), A.ret(A.U())),
    A.sto(6, A.call(r(2), 'GetActionRequestData', [r(3)])), A.IF(A.isU(r(6)), A.ret(A.U())),
    A.sto(7, A.m(r(6), 'priority')),
    A.sto(8, A.call(r(2), 'GetActionRequestResponseList', [r(3)])),
    A.sto(9, num(-1)), A.IF(A.not(A.isU(r(8))), A.sto(9, A.m(r(8), 'length'))),
    A.sto(13, A.m(THIS(), 'R')), A.sto(10, num(0)),
    A.WHILE(A.lt(r(10), A.m(r(13), 'length')), [
      A.sto(11, A.ix(r(13), r(10))), A.sto(12, num(-1)),
      // band rows: the broker's requests
      A.IFALL([A.eq(A.m(r(11), 'b'), num(1)), A.eq(r(4), LL()), A.eq(r(9), num(FORMS)), A.not(A.lt(r(7), A.m(r(11), 'p'))), A.lt(r(7), A.add(A.m(r(11), 'p'), A.m(r(11), 's')))],
        A.sto(12, A.sub(r(7), A.m(r(11), 'p')))),
      // request rows: the mod's own request (never the broker's TypeName)
      A.IFALL([A.eq(A.m(r(11), 'b'), num(2)), A.not(A.eq(r(4), LL())), A.eq(r(9), A.m(r(11), 'q'))],
        A.IF(A.isU(A.m(r(11), 't')), A.sto(12, r(7)), A.IF(A.eq(r(4), A.m(r(11), 't')), A.sto(12, r(7))))),
      A.IF(A.not(A.lt(r(12), num(0))), [
        A.IF(A.m(r(11), 'iv'), A.sto(12, A.sub(num(1), r(12)))),
        A.IFALL([A.not(A.lt(r(12), num(0))), A.lt(r(12), A.m(r(11), 'n'))], [], A.sto(12, A.m(r(11), 'f'))),
        A.IF(A.isU(A.ix(A.m(THIS(), 'i'), r(10))), [A.seti(A.m(THIS(), 'i'), r(10), r(3)), A.seti(A.m(THIS(), 'v'), r(10), r(12))],
          A.IF(A.gt(r(3), A.ix(A.m(THIS(), 'i'), r(10))), [A.seti(A.m(THIS(), 'i'), r(10), r(3)), A.seti(A.m(THIS(), 'v'), r(10), r(12))])),
      ]),
      A.inc(10)]),
    A.ret(A.U()),
  ]);
  // ---- Z(): the scan. r2 rows, r3 i, r4 u, r5 si, r6 ids, r7 j, r8 found, r9 fk, r10 list, r11 rec, r12 d
  const fnZ = A.fn(0, 14, [
    A.sto(2, A.m(THIS(), 'R')),
    A.IF(A.isU(A.m(THIS(), 'v')), A.set(THIS(), 'v', A.newArr())),
    A.set(THIS(), 'i', A.newArr()),
    A.sto(3, num(0)),
    A.WHILE(A.lt(r(3), A.m(r(2), 'length')), [
      A.IF(A.not(A.eq(A.m(A.ix(r(2), r(3)), 'b'), num(0))), A.seti(A.m(THIS(), 'v'), r(3), A.m(A.ix(r(2), r(3)), 'f')),
        A.IF(A.isU(A.ix(A.m(THIS(), 'v'), r(3))), A.seti(A.m(THIS(), 'v'), r(3), A.m(A.ix(r(2), r(3)), 'f')))),
      A.inc(3)]),
    A.set(THIS(), 'dy', A.F()),
    A.sto(4, A.call(THIS(), 'U')), A.IF(A.isU(r(4)), A.ret(A.U())),
    // in a dynasty? (Playoff Customizer's own test: GetSeasonInfo().seasonWeek is a number; not at the title screen or at dynasty creation)
    A.IF(A.not(A.isFn(A.m(r(4), 'GetSeasonInfo'))), A.ret(A.U())),
    A.sto(5, A.call(r(4), 'GetSeasonInfo')), A.IF(A.isU(r(5)), A.ret(A.U())),
    A.IF(A.not(A.eq([A.m(r(5), 'seasonWeek'), X('TypeOf')], C('number'))), A.ret(A.U())),
    A.set(THIS(), 'dy', A.T()),
    A.sto(8, num(0)),
    A.IF(A.isFn(A.m(r(4), 'GetListActionRequestIds')), [
      A.sto(6, A.call(r(4), 'GetListActionRequestIds')),
      A.IF(A.not(A.isU(r(6))), [A.sto(7, num(0)), A.WHILE(A.lt(r(7), A.m(r(6), 'length')), [
        A.sto(12, A.call(r(4), 'GetActionRequestData', [A.ix(r(6), r(7))])),
        A.IF(A.not(A.isU(r(12))), A.callp(THIS(), 'M', [r(4), A.ix(r(6), r(7)), A.m(r(12), 'requestTypeName'), A.m(r(12), 'description')])),
        A.inc(7)])]),
    ]),
    // found = request / band rows with a request
    A.sto(3, num(0)),
    A.WHILE(A.lt(r(3), A.m(r(2), 'length')), [A.IF(A.not(A.isU(A.ix(A.m(THIS(), 'i'), r(3)))), A.inc(8)), A.inc(3)]),
    A.IF(A.gt(r(8), num(0)), A.ret(A.U())),
    // nothing in the native list: the FranTk request list (Playoff Customizer never lists our hidden requests natively; DE 0.1.3 lab)
    A.sto(9, A.G('ION_FranTkRequest')),
    A.IF(A.isU(r(9)), [], A.IFALL([A.isFn(A.m(r(9), 'InitializeRequestList')), A.isFn(A.m(r(9), 'GetRequestList'))], [], A.sto(9, A.U()))),
    A.IF(A.isU(r(9)), [
      A.sto(9, A.G('frantk')), A.IF(A.isU(r(9)), A.ret(A.U())),
      A.sto(9, A.m(r(9), 'Adapters')), A.IF(A.isU(r(9)), A.ret(A.U())),
      A.sto(9, A.m(r(9), 'FranTkRequestAdapter')), A.IF(A.isU(r(9)), A.ret(A.U())),
      A.IFALL([A.isFn(A.m(r(9), 'InitializeRequestList')), A.isFn(A.m(r(9), 'GetRequestList'))], [], A.ret(A.U())),
    ]),
    A.callp(r(9), 'InitializeRequestList'),
    A.sto(10, A.call(r(9), 'GetRequestList')),
    A.IF(A.not(A.isU(r(10))), [A.sto(7, num(0)), A.WHILE(A.lt(r(7), A.m(r(10), 'length')), [
      A.sto(11, A.ix(r(10), r(7))),
      A.IF(A.not(A.isU(r(11))), A.callp(THIS(), 'M', [r(4), A.m(r(11), 'RequestId'), A.m(r(11), 'TypeName'), A.m(r(11), 'Description')])),
      A.inc(7)])]),
    A.IF(A.isFn(A.m(r(9), 'ShutdownRequestList')), A.callp(r(9), 'ShutdownRequestList')),
    A.ret(A.U()),
  ]);
  const TOGGLE = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_TOGGLE'), HEADER = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_HEADER');
  // ---- GetListSettings(cat): r2 cat, r3 out, r4 i, r5 rows, r6 row, r7 item
  const fnList = A.fn(1, 9, [
    A.IF(A.isU(A.m(THIS(), 'i')), A.callp(THIS(), 'Z')),
    A.sto(3, A.newArr()), A.sto(5, A.m(THIS(), 'R')), A.sto(4, num(0)),
    A.WHILE(A.lt(r(4), A.m(r(5), 'length')), [
      A.sto(6, A.ix(r(5), r(4))),
      A.IF(A.eq(A.m(r(6), 'y'), num(1)),
        A.sto(7, A.obj([['key', A.m(r(6), 'k')], ['type', HEADER()], ['display', A.m(r(6), 'd')], ['description', A.m(r(6), 'h')], ['isEnabled', A.F()]])),
        A.sto(7, A.obj([['key', A.m(r(6), 'k')], ['type', TOGGLE()], ['display', A.m(r(6), 'd')], ['description', A.m(r(6), 'h')], ['isEnabled', A.call(THIS(), 'Lv', [r(4)])]]))),
      A.push(r(3), r(7)), A.inc(4)]),
    A.ret(r(3)),
  ]);
  // ---- GetToggleDetails(cat, key): r2 cat, r3 key, r4 i, r5 out, r6 row, r7 v, r8 j
  const fnDetails = A.fn(2, 10, [
    A.sto(5, A.newArr()),
    A.sto(4, A.call(THIS(), 'X', [r(3)])), A.IF(A.lt(r(4), num(0)), A.ret(r(5))),
    A.sto(6, A.ix(A.m(THIS(), 'R'), r(4))),
    A.IF(A.not(A.call(THIS(), 'Lv', [r(4)])), [
      A.IF(A.m(THIS(), 'dy'), A.push(r(5), A.obj([['display', A.m(r(6), 'it')], ['key', num(0)], ['isDefault', A.T()]])),
        A.push(r(5), A.obj([['display', C('Open from a dynasty')], ['key', num(0)], ['isDefault', A.T()]]))),
      A.ret(r(5))]),
    A.sto(7, A.ix(A.m(THIS(), 'v'), r(4))), A.sto(8, num(0)),
    A.WHILE(A.lt(r(8), A.m(r(6), 'n')), [A.push(r(5), A.obj([['display', A.ix(A.m(r(6), 'o'), r(8))], ['key', r(8)], ['isDefault', A.eq(r(8), r(7))]])), A.inc(8)]),
    A.ret(r(5)),
  ]);
  // ---- GameAdjustToggleSetting(cat, key, value, why): r2 cat, r3 key, r4 value, r5 why, r6 i, r7 row, r8 u, r9 a
  const fnAdjust = A.fn(4, 11, [
    A.sto(6, A.call(THIS(), 'X', [r(3)])), A.IF(A.lt(r(6), num(0)), A.ret(A.newArr())),
    A.IF(A.not(A.call(THIS(), 'Lv', [r(6)])), A.ret(A.newArr())),
    A.sto(7, A.ix(A.m(THIS(), 'R'), r(6))),
    A.IFALL([A.not(A.lt(r(4), num(0))), A.lt(r(4), A.m(r(7), 'n'))], [], A.ret(A.newArr())),
    A.IF(A.not(A.eq(A.m(r(7), 'b'), num(0))), [
      A.sto(8, A.call(THIS(), 'U')),
      A.IF(A.not(A.isU(r(8))), [
        A.sto(9, r(4)), A.IF(A.m(r(7), 'iv'), A.sto(9, A.sub(num(1), r(4)))),
        A.callp(r(8), 'ActionRequestRespond', [A.ix(A.m(THIS(), 'i'), r(6)), r(9)])]),
    ]),
    A.seti(A.m(THIS(), 'v'), r(6), r(4)),
    A.ret(A.newArr()),
  ]);
  // ---- GameShutdownSettingCategory(params): r2 params, r3 l
  const fnShut = A.fn(1, 5, [
    A.sto(3, A.m(THIS(), 'l')),
    // FeatureManager.Notify(adapterName, eventName, featureKey, ...): the stock manager checks the key and the event name, then finishes the shutdown
    A.IF(A.not(A.isU(r(3))), A.callp(r(3), 'Notify', [C('ModSettings'), C('FinishedShutdownSettingCategory'), C(MS_CAT)])),
    A.ret(A.U()),
  ]);
  const nop = (np) => A.fn(np, 2 + np, [A.ret(A.U())]);
  const retv = (np, v) => A.fn(np, 2 + np, [A.ret(v)]);
  return A.obj([
    ['R', A.arr(table.rows.map(rowObj))],
    ['AddNotifyListener', A.fn(1, 4, [A.set(THIS(), 'l', r(2)), A.ret(A.U())])],
    ['RemoveNotifyListener', A.fn(1, 4, [A.set(THIS(), 'l', A.U()), A.ret(A.U())])],
    ['GameInitializeSettingCategory', A.fn(1, 4, [A.callp(THIS(), 'Z'), A.ret(A.U())])],
    ['GameShutdownSettingCategory', fnShut],
    ['GetIsFeatureInitialized', retv(1, A.T())],
    ['GetIsFeatureChanged', retv(1, A.F())],
    ['GetIsFeatureResetNeeded', retv(1, A.F())],
    ['GameSaveAllSettings', retv(1, A.T())],
    ['GameDiscardFeatureChanges', nop(1)],
    ['GameResetFeature', nop(1)],
    ['GameHighlightSetting', nop(2)],
    ['GetListSettings', fnList],
    ['GetToggleDetails', fnDetails],
    ['GameAdjustToggleSetting', fnAdjust],
    ['U', fnU], ['X', fnX], ['Lv', fnLv], ['M', fnM], ['Z', fnZ],
  ]);
}

// ------------------------------------------------------------------------------------------------ the FEED adapter object (Mod Menu layer 2, 2026-11-11)
// The tab's rows come from modsettings.dll through the game's NATIVE settings adapter, asked with a private category id (FEED_CAT = 19795 =
// 0x4D53; stock categories are 0-11, 15-17, 20, 21: PROVEN from the 10-29 dump). This object keeps the screen life cycle in ActionScript (the
// Step 1 methods, PROVEN in game by the probes: init at once, shutdown answered with Notify at once) and forwards the five data calls:
//   GetListSettings(cat)                -> native.GetListSettings(19795)                (the DLL's slot 0x30 wrapper answers; rows + keys from the DLL)
//   GetToggleDetails(cat, key)          -> native.GetToggleDetails(19795, key)          (slot 0x38)
//   GetSliderDetails(cat, key)          -> native.GetSliderDetails(19795, key)          (slot 0x40)
//   GameAdjustToggleSetting(c, k, v, w) -> native.GameAdjustToggleSetting(19795, k, v, w) (slot 0x50: the DLL stores the value, writes settings.json)
//   GameAdjustSliderSetting(c, k, v, w) -> native.GameAdjustSliderSetting(19795, k, v, w) (slot 0x58)
// Without the DLL the native managers find no category 19795 and return nothing (PROVEN statically): the list is empty, so this object shows
// a header + one greyed row "The Mod Settings plugin is not running" instead (key FEED_FALLBACK; details for it = one item, changes ignored).
// An undefined answer from the native side is turned into an empty array (the stock code iterates the results).
const FEED_CAT = 19795;
const FEED_FALLBACK = 0x4cff;
function adapterFeed(A, { noPluginLabel = 'The Mod Settings plugin is not running', noPluginHelp = 'Install Mod Settings (modsettings.dll, a Mod Manager plugin) to see and change your mods\' settings here. Without it every mod uses its own defaults.' } = {}) {
  const { C, reg: r, X, num } = A;
  const THIS = () => r(1);
  const TOGGLE = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_TOGGLE'), HEADER = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_HEADER');
  // ---- N(): the native settings adapter or undefined. r2 = madden.Adapters
  const fnN = A.fn(0, 4, [
    A.sto(2, A.G('madden')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.sto(2, A.m(r(2), 'Adapters')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.IF(A.not(A.isFn(A.m(r(2), 'GetSettingsAdapter'))), A.ret(A.U())),
    A.ret(A.call(r(2), 'GetSettingsAdapter')),
  ]);
  // ---- GetListSettings(cat): r2 cat, r3 native, r4 list
  const fnList = A.fn(1, 6, [
    A.sto(3, A.call(THIS(), 'N')), A.sto(4, A.U()),
    A.IF(A.not(A.isU(r(3))), A.sto(4, A.call(r(3), 'GetListSettings', [num(FEED_CAT)]))),
    A.IF(A.not(A.isU(r(4))), A.IF(A.gt(A.m(r(4), 'length'), num(0)), [A.set(THIS(), 'f', A.F()), A.ret(r(4))])),
    A.set(THIS(), 'f', A.T()),
    A.ret(A.arr([
      A.obj([['key', num(-1)], ['type', HEADER()], ['display', C('Mod Settings')], ['description', C(' ')], ['isEnabled', A.F()]]),
      A.obj([['key', num(FEED_FALLBACK)], ['type', TOGGLE()], ['display', C(noPluginLabel)], ['description', C(noPluginHelp)], ['isEnabled', A.F()]]),
    ])),
  ]);
  // ---- GetToggleDetails(cat, key): r2 cat, r3 key, r4 native, r5 out
  const fnDetails = A.fn(2, 7, [
    A.IF(A.eq(r(3), num(FEED_FALLBACK)), A.ret(A.arr([A.obj([['display', C('Not installed')], ['key', num(0)], ['isDefault', A.T()]])]))),
    A.sto(4, A.call(THIS(), 'N')), A.IF(A.isU(r(4)), A.ret(A.newArr())),
    A.sto(5, A.call(r(4), 'GetToggleDetails', [num(FEED_CAT), r(3)])), A.IF(A.isU(r(5)), A.ret(A.newArr())),
    A.ret(r(5)),
  ]);
  // ---- GetSliderDetails(cat, key): r2 cat, r3 key, r4 native, r5 out
  const fnSlider = A.fn(2, 7, [
    A.sto(4, A.call(THIS(), 'N')), A.IF(A.isU(r(4)), A.ret([X('PushZero'), C('Object'), X('NewObject')])),
    A.sto(5, A.call(r(4), 'GetSliderDetails', [num(FEED_CAT), r(3)])), A.IF(A.isU(r(5)), A.ret([X('PushZero'), C('Object'), X('NewObject')])),
    A.ret(r(5)),
  ]);
  // ---- GameAdjust<kind>Setting(cat, key, value, why): r2 cat, r3 key, r4 value, r5 why, r6 native, r7 out
  const adjust = (name) => A.fn(4, 9, [
    A.IF(A.eq(r(3), num(FEED_FALLBACK)), A.ret(A.newArr())),
    A.sto(6, A.call(THIS(), 'N')), A.IF(A.isU(r(6)), A.ret(A.newArr())),
    A.sto(7, A.call(r(6), name, [num(FEED_CAT), r(3), r(4), r(5)])), A.IF(A.isU(r(7)), A.ret(A.newArr())),
    A.ret(r(7)),
  ]);
  // ---- GameShutdownSettingCategory(params): the stock FeatureManager waits for this notify (Step 1's fnShut, PROVEN in game)
  const fnShut = A.fn(1, 5, [
    A.sto(3, A.m(THIS(), 'l')),
    A.IF(A.not(A.isU(r(3))), A.callp(r(3), 'Notify', [C('ModSettings'), C('FinishedShutdownSettingCategory'), C(MS_CAT)])),
    A.ret(A.U()),
  ]);
  const nop = (np) => A.fn(np, 2 + np, [A.ret(A.U())]);
  const retv = (np, v) => A.fn(np, 2 + np, [A.ret(v)]);
  return A.obj([
    ['AddNotifyListener', A.fn(1, 4, [A.set(THIS(), 'l', r(2)), A.ret(A.U())])],
    ['RemoveNotifyListener', A.fn(1, 4, [A.set(THIS(), 'l', A.U()), A.ret(A.U())])],
    ['GameInitializeSettingCategory', nop(1)],
    ['GameShutdownSettingCategory', fnShut],
    ['GetIsFeatureInitialized', retv(1, A.T())],
    ['GetIsFeatureChanged', retv(1, A.F())],
    ['GetIsFeatureResetNeeded', retv(1, A.F())],
    ['GameSaveAllSettings', retv(1, A.T())],
    ['GameDiscardFeatureChanges', nop(1)],
    ['GameResetFeature', nop(1)],
    ['GameHighlightSetting', nop(2)],
    ['GetListSettings', fnList],
    ['GetToggleDetails', fnDetails],
    ['GetSliderDetails', fnSlider],
    ['GameAdjustToggleSetting', adjust('GameAdjustToggleSetting')],
    ['GameAdjustSliderSetting', adjust('GameAdjustSliderSetting')],
    ['N', fnN],
  ]);
}

// ------------------------------------------------------------------------------------------------ the file
// opts.feed = true: the Mod Menu tab (rows from modsettings.dll, adapterFeed); else Step 1's page (rows compiled in from `table`)
function buildPage(stockAst, table, { tabLabel = 'Mod Settings', tabHelp = 'Settings of your installed mods.', feed = false, feedOpts = {} } = {}) {
  if (feed) table = { mods: [], rows: [] };
  const { data, apt1 } = apt.streamsOf(stockAst);
  const gs = copyClass(data, apt1, CLASS, []);
  const A = mkAs(gs);
  const { reg: r, X, C } = A;
  // ---- _GetSettingsManagerData: ... r2.KeyParam = {...}; return r2   -> our block before the final "PushRegister r2; Return"
  const gd = findFn(gs, data, '_GetSettingsManagerData');
  must(gd.fn.raw[12] === 6, `_GetSettingsManagerData uses ${gd.fn.raw[12]} registers, expected 6`);
  const gt = gd.body.map((i) => T(gs, data, i));
  const nb = gd.body.length;
  must(gt[nb - 2] === 'PushRegister r2' && gt[nb - 1] === 'Return' && gt.includes('PushConstW "OverrideAdapter"') && gt.includes('PushConstW "KeyParam"'), `_GetSettingsManagerData shape changed: ${gt.slice(-6).join(' | ')}`);
  const retAt = gd.body[nb - 2];
  const gdBranchesToRet = gs.s.ins.filter((x) => x.kind === 'branch' && x.target === retAt).length;
  const gdIns = link([
    A.IF(A.eq(A.m(r(1), 'mSettingsCategory'), C(MS_CAT)), [
      A.sto(6, A.m(r(1), 'mMsA')),
      A.IF(A.isU(r(6)), [A.sto(6, feed ? adapterFeed(A, feedOpts) : adapterObject(A, table)), A.set(r(1), 'mMsA', r(6))]),
      A.set(r(2), 'OverrideAdapter', r(6)),
      A.set(r(1), 'mFeatureAdapter', r(6)),
    ], A.set(r(1), 'mFeatureAdapter', A.call(A.G('madden', 'Adapters'), 'GetSettingsAdapter'))),
  ], retAt);
  // ---- _SettingsManagerSettingsHubInitFinish: before "PushRegister r3; PushOne; PushRegister r1; GetNamedMember mTabListProvider; AddItemsByItemArray"
  const hf = findFn(gs, data, '_SettingsManagerSettingsHubInitFinish');
  must(hf.fn.raw[12] === 6, `_SettingsManagerSettingsHubInitFinish uses ${hf.fn.raw[12]} registers, expected 6`);
  const ht = hf.body.map((i) => T(gs, data, i));
  const k = ht.findIndex((x, j) => x === 'PushRegister r3' && ht[j + 1] === 'PushOne' && ht[j + 2] === 'PushRegister r1' && ht[j + 3] === 'GetNamedMember "mTabListProvider"' && ht[j + 4] === 'PushConstW "AddItemsByItemArray"');
  must(k > 0 && ht.filter((x) => x === 'PushConstW "AddItemsByItemArray"').length === 1, '_SettingsManagerSettingsHubInitFinish: the tab hand-over not found');
  must(ht.slice(0, k).some((x) => x === 'PushConstW "GetListSettings"') && ht.slice(0, k).some((x) => x === 'PushConstW "_GetSettingCategoryKey"'), '_SettingsManagerSettingsHubInitFinish: the native tab list + key loop are not before the hand-over');
  const tabAt = hf.body[k];
  const tabIns = link([
    A.IF(A.not(A.isU(r(3))), A.IF(A.gt(A.m(r(3), 'length'), num0(A)), [
      A.sto(6, [X('PushZero'), C('Object'), X('NewObject')]),
      A.callp(A.G('cafe', 'utility', 'ObjectUtil'), 'MergeInto', [r(6), A.ix(r(3), A.sub(A.m(r(3), 'length'), A.num(1))), A.T()]),
      A.set(r(6), 'key', C(MS_CAT)), A.set(r(6), 'display', C(tabLabel)), A.set(r(6), 'description', C(tabHelp)),
      A.set(r(6), 'isEnabled', A.T()), A.set(r(6), 'isDefault', A.eq(r(4), C(MS_CAT))),
      A.push(r(3), r(6)),
    ])),
  ], tabAt);
  // every branch that reached the hand-over (the key loop's exit) now reaches our tab first
  let moved = 0;
  for (const i of gs.s.ins) if (i.kind === 'branch' && i.target === tabAt) { i.target = tabIns[0]; moved++; }
  must(moved === 1, `expected one branch to the tab hand-over (the key loop exit), found ${moved}`);
  must(gdBranchesToRet === 0, `_GetSettingsManagerData: ${gdBranchesToRet} branches reach the final return (our block would be skipped)`);
  const r0 = patchWithFunctions(stockAst, gs, A, [{ before: retAt, extra: gdIns }, { before: tabAt, extra: tabIns }],
    [{ fn: gd.fn, regs: 8 }, { fn: hf.fn, regs: 8 }], feed ? `${FILE} + Mod Settings tab (rows fed by modsettings.dll, category ${FEED_CAT})` : `${FILE} + Mod Settings page (${table.mods.length} mods, ${table.rows.length} rows)`);
  return { ast: r0.ast, report: r0.report + `; tab "${tabLabel}"; ${A.fns.length} new functions; ${A.added.length} new strings`, functions: A.fns.length, strings: A.added.length };
}
function num0(A) { return A.num(0); }

// PB's patchFile, plus an area ahead of the class copy that holds each new function's name string and parameter array
function patchWithFunctions(stockAst, copy, A, inserts, regs, label) {
  const { parsed, data, apt1, dataIndex, apt1Index } = apt.streamsOf(stockAst);
  const newNames = A.added.slice();
  const a1 = apt.apt1AddStrings(apt1, newNames);
  const idxOf = (nm) => a1.indices[newNames.indexOf(nm)];
  let cursor = apt.al8(data.length);
  const area = []; let at = cursor;
  for (const f of A.fns) {
    // [8-byte empty name][params: {u32 reg, u32 0, u64 name ptr} x n, or 16 zero bytes][param names "a0".. 8 bytes each]
    const n = f.nparams; const nameAt = at; const parAt = at + 8; const pnAt = parAt + Math.max(1, n) * 16;
    const b = Buffer.alloc(8 + Math.max(1, n) * 16 + n * 8);
    for (let k = 0; k < n; k++) { b.writeUInt32LE(2 + k, 8 + k * 16); b.writeBigUInt64LE(BigInt(pnAt + k * 8), 8 + k * 16 + 8); b.write(`a${k}`, 8 + Math.max(1, n) * 16 + k * 8, 'latin1'); }
    f.raw.writeBigUInt64LE(BigInt(nameAt), 0); f.raw.writeBigUInt64LE(BigInt(parAt), 0x10);
    area.push(b); at += b.length; at = apt.al8(at); if (at > nameAt + b.length) area.push(Buffer.alloc(at - nameAt - b.length));
  }
  const areaBuf = Buffer.concat(area); must(cursor + areaBuf.length === at, 'area layout');
  const parts = [data, Buffer.alloc(cursor - data.length), areaBuf];
  cursor = apt.al8(at); if (cursor > at) parts.push(Buffer.alloc(cursor - at));
  const swaps = new Map(regs.map(({ fn, regs: rg }) => { const cp = { ...fn, raw: Buffer.from(fn.raw) }; must(rg > fn.raw[12], 'registers must grow'); cp.raw[12] = rg; return [fn, cp]; }));
  const out = splice(copy.s.ins, inserts, swaps);
  const cp0 = out[0];
  const first = apt.assemble(out, cursor);
  const listAt = apt.al8(first.end);
  cp0.raw.writeUInt32LE(copy.pool.length, 0); cp0.raw.writeBigUInt64LE(BigInt(listAt), 8);
  const asm = apt.assemble(out, cursor); must(asm.end === first.end, 'layout changed between passes');
  const list = Buffer.alloc(copy.pool.length * 8);
  for (let k = 0; k < copy.ia.pool.length; k++) list.writeBigUInt64LE(data.readBigUInt64LE(copy.ia.poolPtr + k * 8), k * 8);
  for (let k = copy.ia.pool.length; k < copy.pool.length; k++) { const ix = idxOf(copy.pool[k]); must(ix !== undefined, `pool name ${copy.pool[k]} not added`); list.writeBigUInt64LE(BigInt(ix), k * 8); }
  parts.push(asm.bytes, Buffer.alloc(listAt - asm.end), list);
  const newData = Buffer.concat(parts);
  newData.writeBigUInt64LE(BigInt(cursor), copy.ia.ptrAt);
  const streams = []; streams[dataIndex] = { data: newData }; streams[apt1Index] = { data: a1.buf };
  let ast = null; let widened = false;
  try { ast = bgfa.write(parsed, streams, { level: 9 }); } catch (e) {
    if (!/does not fit/.test(e.message)) throw e;
    const w = bgfa.writeEx(parsed, streams, { level: 9, widen: true }); ast = w.buf; widened = w.widened; must(widened, 'writeEx did not widen');
  }
  verify(ast, { data, apt1, copy, inserts, newNames, at: cursor, label, fns: A.fns.length });
  return { ast, report: `${label}: GenericSettings copy 0x${cursor.toString(16)} (+${inserts.reduce((n, x) => n + x.extra.length, 0)} ins), function area ${areaBuf.length} B; Apt Data ${data.length} -> ${newData.length} B, Apt1 +${newNames.length} strings${widened ? ' (BGFA fields widened)' : ''}` };
}
const norm = (t) => t.replace(/^ConstantPool.*/, 'CP').replace(/^(Jump|IfTrue|IfFalse) .*/, '$1').replace(/ size=0x[0-9a-f]+$/, '').replace(/ regs=\d+/, '').replace(/^PushConstW /, 'PushConst ');
function verify(ast, ctx) {
  const s = apt.streamsOf(ast); const d = s.data, d0 = ctx.data;
  const ptr = ctx.copy.ia.ptrAt;
  for (let k = 0; k < d0.length; k++) if (d[k] !== d0[k]) must(k >= ptr && k < ptr + 8, `${ctx.label}: stock Apt Data byte 0x${k.toString(16)} changed`);
  const A0 = apt.parseApt1(ctx.apt1), A1 = apt.parseApt1(s.apt1);
  must(A1.count === A0.count + ctx.newNames.length, 'Apt1 count');
  for (let k = 0; k < A0.count; k++) must(A1.str(k) === A0.str(k), `Apt1 entry ${k} changed`);
  const c = ctx.copy; const ia = apt.initActions(d, s.apt1).find((x) => x.item === c.ia.item);
  must(ia && ia.script === ctx.at, 'frame item not repointed');
  must(ia.pool.join('\u0001') === c.pool.join('\u0001'), 'copy pool does not resolve');
  let got = apt.decode(d, ia.script).ins.map((i) => norm(apt.text(i, ia.pool, d)));
  const stock = c.s.ins.map((i) => norm(apt.text(i, c.ia.pool, d0)));
  for (const x of ctx.inserts) {
    const ours = x.extra.map((i) => norm(apt.text(i, c.pool, d)));
    let k0 = -1; for (let k = 0; k + ours.length <= got.length && k0 < 0; k++) if (ours.every((t, j) => got[k + j] === t)) k0 = k;
    must(k0 >= 0, 'inserted block not found in the copy');
    got = got.slice(0, k0).concat(got.slice(k0 + ours.length));
  }
  must(got.length === stock.length && got.every((t, k) => t === stock[k]), 'copy differs from the stock class outside our insertions');
  // every new function record: its parameter array is inside the new area and names a register
  const dec = apt.decode(d, ia.script); let nf = 0;
  for (const i of dec.ins) if (i.op === 0x8e && Number(i.raw.readBigUInt64LE(0x10)) >= d0.length) {
    nf++; const np = i.raw.readUInt32LE(8); const pp = Number(i.raw.readBigUInt64LE(0x10));
    for (let k = 0; k < np; k++) { const rg = d.readUInt32LE(pp + k * 16); const nmp = Number(d.readBigUInt64LE(pp + k * 16 + 8)); must(rg === 2 + k && nmp >= d0.length && nmp < ctx.at, 'parameter array'); }
    must(Number(i.raw.readBigUInt64LE(0)) >= d0.length && Number(i.raw.readBigUInt64LE(0)) < ctx.at, 'function name pointer');
  }
  must(nf === ctx.fns, `new function records: ${nf}, expected ${ctx.fns}`);
  for (const x of apt.initActions(d, s.apt1)) apt.decode(d, x.script);
}

module.exports = { buildPage, FILE, CLASS, MS_CAT, D_NAME, FORMS, FEED_CAT, FEED_FALLBACK, link, mkAs, patchWithFunctions };
