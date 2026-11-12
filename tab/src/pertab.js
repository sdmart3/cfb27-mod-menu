#!/usr/bin/env node
// pertab-probe.js: PROBE build (2026-11-12, docs/MOD-MENU-PER-MOD-TABS-2026-11-12.md section 6). NOT a release build; does not change src/.
//
// One Settings tab PER MOD, titled at runtime, with the PUBLISHED modsettings.dll (0.1 beta, 8e8a7ecb) unchanged:
//   hub init (_SettingsManagerSettingsHubInitFinish): after the stock tabs are keyed, the page asks the native settings adapter for category 19795
//     (the DLL's one list: a section header per mod, then its rows) and appends one tab per SECTION header (type header, key -1, non-empty
//     description: the DLL writes "<title> <version>" there; a mod's own header rows have an empty description). Tab key = 19796 + n, display = the
//     section title (the manifest "title"). At most MAXT tabs; when there are more sections, the last tab is "More Mods" and holds the rest.
//     No section (no DLL, no mods, DLL refused) -> one "Mod Settings" tab keyed MOD_SETTINGS, exactly as the 0.1 beta.
//   _GetSettingsManagerData: MOD_SETTINGS or a number in [19796, 19796 + MAXT) -> the feed object (cached in mMsA; .c = the category, for the
//     shutdown Notify key); any other category -> the native adapter (as 0.1).
//   feed object GetListSettings(cat): the DLL's list (19795) sliced to that tab's section(s); MOD_SETTINGS -> the whole list (0.1 behaviour).
//     Details / adjust: forwarded with 19795 and the key unchanged (row keys are unique across mods: 0x4D00 + row index).
// Proves in one lab: runtime tab titles, several of our tabs on the bar, and whether the row limit is per list build (216 rows over 4 tabs:
// 10 / 101 / 101 / 4).
// usage: node modsettings/probes/pertab-probe.js [--max 8] [--emu-only]
'use strict';
const fs = require('fs'); const path = require('path'); const crypto = require('crypto'); const cp = require('child_process');
const REPO = path.join(__dirname, '..'); const ROOT = path.join(REPO, '..'); const T = (p) => path.join(ROOT, 'tools', p);
const LP = require(T('legacy-pack.js')); const apt = require(T('apt.js')); const bgfa = require(T('bgfa.js'));
const BEM = require(T('build-expression-mod.js')); const fbf = require(T('fbmod-full.js')); const { TEST_CATEGORY } = require(T('pack-fbmod.js'));
const UI = require(path.join(REPO, 'src', 'ui-build-ms.js'));
const PB = require(path.join(ROOT, 'spex', 'probes', 'gen-pressbox-ui.js'));
const { findFn, T: TX, copyClass, must } = PB._internals;
const { link, mkAs, patchWithFunctions, FEED_CAT, FEED_FALLBACK, MS_CAT, CLASS, FILE } = UI;

const argv = process.argv.slice(2); const opt = (k, d) => (argv.includes(k) ? argv[argv.indexOf(k) + 1] : d);
const MAXT = Number(opt('--max', 8)); must(MAXT >= 2 && MAXT <= 32, '--max 2..32');
const TAB0 = 19796;
const SHELL = process.env.MS_SHELL || '';   // a packing shell .fbmod (maintainers' build; set MS_SHELL)
const OUTDIR = path.join(ROOT, 'exports', 'build-mods', 'modmenu-pertab-probe');

// ------------------------------------------------------------------ the feed object (0.1's adapterFeed + section slicing + S / SC helpers)
function feedObject(A) {
  const { C, reg: r, X, num } = A; const THIS = () => r(1);
  const TOGGLE = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_TOGGLE'), HEADER = () => A.G('shared_enum', 'Settings', 'SETTING_TYPE_HEADER');
  const isNum = (v) => [v, X('TypeOf'), C('number'), X('Equals2')];
  const fnN = A.fn(0, 4, [
    A.sto(2, A.G('madden')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.sto(2, A.m(r(2), 'Adapters')), A.IF(A.isU(r(2)), A.ret(A.U())),
    A.IF(A.not(A.isFn(A.m(r(2), 'GetSettingsAdapter'))), A.ret(A.U())),
    A.ret(A.call(r(2), 'GetSettingsAdapter')),
  ]);
  // S(row): a section header (type header, key -1, a non-empty description). r2 row, r3 d
  const fnS = A.fn(1, 5, [
    A.IF(A.isU(r(2)), A.ret(A.F())),
    A.IF(A.not(A.eq(A.m(r(2), 'type'), HEADER())), A.ret(A.F())),
    A.IF(A.not(A.eq(A.m(r(2), 'key'), num(-1))), A.ret(A.F())),
    A.sto(3, A.m(r(2), 'description')), A.IF(A.isU(r(3)), A.ret(A.F())),
    A.IF(A.eq(r(3), C('')), A.ret(A.F())),
    A.ret(A.T()),
  ]);
  // SC(list): the number of section headers. r2 list, r3 i, r4 n
  const fnSC = A.fn(1, 6, [
    A.sto(4, num(0)), A.IF(A.isU(r(2)), A.ret(r(4))), A.sto(3, num(0)),
    A.WHILE(A.lt(r(3), A.m(r(2), 'length')), [A.IF(A.call(THIS(), 'S', [A.ix(r(2), r(3))]), A.inc(4)), A.inc(3)]),
    A.ret(r(4)),
  ]);
  // GetListSettings(cat): r2 cat, r3 native, r4 list, r5 tab index, r6 out, r7 section, r8 i, r9 row, r10 take
  const fnList = A.fn(1, 12, [
    A.sto(3, A.call(THIS(), 'N')), A.sto(4, A.U()),
    A.IF(A.not(A.isU(r(3))), A.sto(4, A.call(r(3), 'GetListSettings', [num(FEED_CAT)]))),
    A.IF(A.not(A.isU(r(4))), A.IF(A.gt(A.m(r(4), 'length'), num(0)), [
      A.IF(A.eq(r(2), C(MS_CAT)), [A.set(THIS(), 'f', A.F()), A.ret(r(4))]),
      A.IF(isNum(r(2)), [
        A.sto(5, A.sub(r(2), num(TAB0))), A.sto(6, A.newArr()), A.sto(7, num(-1)), A.sto(8, num(0)),
        A.WHILE(A.lt(r(8), A.m(r(4), 'length')), [
          A.sto(9, A.ix(r(4), r(8))),
          A.IF(A.call(THIS(), 'S', [r(9)]), A.inc(7)),
          A.sto(10, A.eq(r(7), r(5))),
          A.IFALL([A.eq(r(5), num(MAXT - 1)), A.gt(r(7), r(5))], A.sto(10, A.T())),
          A.IF(r(10), A.push(r(6), r(9))),
          A.inc(8)]),
        A.IF(A.gt(A.m(r(6), 'length'), num(0)), [A.set(THIS(), 'f', A.F()), A.ret(r(6))]),
      ]),
    ])),
    A.set(THIS(), 'f', A.T()),
    A.ret(A.arr([
      A.obj([['key', num(-1)], ['type', HEADER()], ['display', C('Mod Settings')], ['description', C(' ')], ['isEnabled', A.F()]]),
      A.obj([['key', num(FEED_FALLBACK)], ['type', TOGGLE()], ['display', C('The Mod Settings plugin is not running')],
        ['description', C('Install Mod Settings (modsettings.dll, a Mod Manager plugin) to see and change your mods\' settings here. Without it every mod uses its own defaults.')], ['isEnabled', A.F()]]),
    ])),
  ]);
  const fnDetails = A.fn(2, 7, [
    A.IF(A.eq(r(3), num(FEED_FALLBACK)), A.ret(A.arr([A.obj([['display', C('Not installed')], ['key', num(0)], ['isDefault', A.T()]])]))),
    A.sto(4, A.call(THIS(), 'N')), A.IF(A.isU(r(4)), A.ret(A.newArr())),
    A.sto(5, A.call(r(4), 'GetToggleDetails', [num(FEED_CAT), r(3)])), A.IF(A.isU(r(5)), A.ret(A.newArr())),
    A.ret(r(5)),
  ]);
  const fnSlider = A.fn(2, 7, [
    A.sto(4, A.call(THIS(), 'N')), A.IF(A.isU(r(4)), A.ret([X('PushZero'), C('Object'), X('NewObject')])),
    A.sto(5, A.call(r(4), 'GetSliderDetails', [num(FEED_CAT), r(3)])), A.IF(A.isU(r(5)), A.ret([X('PushZero'), C('Object'), X('NewObject')])),
    A.ret(r(5)),
  ]);
  const adjust = (name) => A.fn(4, 9, [
    A.IF(A.eq(r(3), num(FEED_FALLBACK)), A.ret(A.newArr())),
    A.sto(6, A.call(THIS(), 'N')), A.IF(A.isU(r(6)), A.ret(A.newArr())),
    A.sto(7, A.call(r(6), name, [num(FEED_CAT), r(3), r(4), r(5)])), A.IF(A.isU(r(7)), A.ret(A.newArr())),
    A.ret(r(7)),
  ]);
  // shutdown: Notify with the category this feature manager was made for (.c; FeatureManager.Notify compares it with its mFeatureKey)
  const fnShut = A.fn(1, 5, [
    A.sto(3, A.m(THIS(), 'l')),
    A.IF(A.not(A.isU(r(3))), A.callp(r(3), 'Notify', [C('ModSettings'), C('FinishedShutdownSettingCategory'), A.m(THIS(), 'c')])),
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
    ['N', fnN], ['S', fnS], ['SC', fnSC],
    ['c', C(MS_CAT)],
  ]);
}

function buildProbe(stockAst) {
  const { data, apt1 } = apt.streamsOf(stockAst);
  const gs = copyClass(data, apt1, CLASS, []);
  const A = mkAs(gs); const { reg: r, X, C, num } = A;
  const isNum = (v) => [v, X('TypeOf'), C('number'), X('Equals2')];
  // ---- _GetSettingsManagerData (regs 6 -> 8): r7 = ours?
  const gd = findFn(gs, data, '_GetSettingsManagerData');
  must(gd.fn.raw[12] === 6, `_GetSettingsManagerData uses ${gd.fn.raw[12]} registers, expected 6`);
  const gt = gd.body.map((i) => TX(gs, data, i)); const nb = gd.body.length;
  must(gt[nb - 2] === 'PushRegister r2' && gt[nb - 1] === 'Return' && gt.includes('PushConstW "OverrideAdapter"'), '_GetSettingsManagerData shape changed');
  const retAt = gd.body[nb - 2];
  must(gs.s.ins.filter((x) => x.kind === 'branch' && x.target === retAt).length === 0, '_GetSettingsManagerData: a branch reaches the final return');
  const cat = () => A.m(r(1), 'mSettingsCategory');
  const gdIns = link([
    A.sto(7, A.eq(cat(), C(MS_CAT))),
    A.IFALL([isNum(cat()), A.not(A.lt(cat(), num(TAB0))), A.lt(cat(), num(TAB0 + MAXT))], A.sto(7, A.T())),
    A.IF(r(7), [
      A.sto(6, A.m(r(1), 'mMsA')),
      A.IF(A.isU(r(6)), [A.sto(6, feedObject(A)), A.set(r(1), 'mMsA', r(6))]),
      A.set(r(6), 'c', cat()),
      A.set(r(2), 'OverrideAdapter', r(6)),
      A.set(r(1), 'mFeatureAdapter', r(6)),
    ], A.set(r(1), 'mFeatureAdapter', A.call(A.G('madden', 'Adapters'), 'GetSettingsAdapter'))),
  ], retAt);
  // ---- _SettingsManagerSettingsHubInitFinish (regs 6 -> 12): r3 tabs, r4 start category; ours r5 n, r6 feed, r7 list, r8 i, r9 row, r10 tab, r11 total
  const hf = findFn(gs, data, '_SettingsManagerSettingsHubInitFinish');
  must(hf.fn.raw[12] === 6, `_SettingsManagerSettingsHubInitFinish uses ${hf.fn.raw[12]} registers, expected 6`);
  const ht = hf.body.map((i) => TX(gs, data, i));
  const k = ht.findIndex((x, j) => x === 'PushRegister r3' && ht[j + 1] === 'PushOne' && ht[j + 2] === 'PushRegister r1' && ht[j + 3] === 'GetNamedMember "mTabListProvider"' && ht[j + 4] === 'PushConstW "AddItemsByItemArray"');
  must(k > 0, '_SettingsManagerSettingsHubInitFinish: the tab hand-over not found');
  const tabAt = hf.body[k];
  const newTab = (key, display, desc) => [
    A.sto(10, [X('PushZero'), C('Object'), X('NewObject')]),
    A.callp(A.G('cafe', 'utility', 'ObjectUtil'), 'MergeInto', [r(10), A.ix(r(3), A.sub(A.m(r(3), 'length'), num(1))), A.T()]),
    A.set(r(10), 'key', key), A.set(r(10), 'display', display), A.set(r(10), 'description', desc),
    A.set(r(10), 'isEnabled', A.T()), A.set(r(10), 'isDefault', A.eq(A.m(r(10), 'key'), r(4))),
    A.push(r(3), r(10)),
  ];
  const tabIns = link([
    A.IF(A.not(A.isU(r(3))), A.IF(A.gt(A.m(r(3), 'length'), num(0)), [
      A.sto(6, A.m(r(1), 'mMsA')),
      A.IF(A.isU(r(6)), [A.sto(6, feedObject(A)), A.set(r(1), 'mMsA', r(6))]),
      A.sto(5, num(0)), A.sto(7, A.U()),
      A.sto(9, A.call(r(6), 'N')),
      A.IF(A.not(A.isU(r(9))), A.sto(7, A.call(r(9), 'GetListSettings', [num(FEED_CAT)]))),
      A.sto(11, A.call(r(6), 'SC', [r(7)])),
      A.IF(A.gt(r(11), num(0)), [
        A.sto(8, num(0)),
        A.WHILE(A.lt(r(8), A.m(r(7), 'length')), [
          A.sto(9, A.ix(r(7), r(8))),
          A.IFALL([A.call(r(6), 'S', [r(9)]), A.lt(r(5), num(MAXT))], [
            A.IFALL([A.eq(r(5), num(MAXT - 1)), A.gt(r(11), num(MAXT))],
              newTab(A.add(num(TAB0), r(5)), C('More Mods'), C('Settings of the mods that did not get a tab of their own.')),
              newTab(A.add(num(TAB0), r(5)), A.m(r(9), 'display'), A.m(r(9), 'description'))),
            A.inc(5),
          ]),
          A.inc(8)]),
      ]),
      A.IF(A.eq(r(5), num(0)), newTab(C(MS_CAT), C('Mod Settings'), C('Settings of your installed mods.'))),
    ])),
  ], tabAt);
  let moved = 0;
  for (const i of gs.s.ins) if (i.kind === 'branch' && i.target === tabAt) { i.target = tabIns[0]; moved++; }
  must(moved === 1, `expected one branch to the tab hand-over, found ${moved}`);
  const r0 = patchWithFunctions(stockAst, gs, A, [{ before: retAt, extra: gdIns }, { before: tabAt, extra: tabIns }],
    [{ fn: gd.fn, regs: 8 }, { fn: hf.fn, regs: 12 }], `${FILE} + per-mod tabs PROBE (max ${MAXT}, categories ${TAB0}..${TAB0 + MAXT - 1}, rows from category ${FEED_CAT})`);
  return { ast: r0.ast, report: r0.report + `; ${A.fns.length} new functions; ${A.added.length} new strings` };
}

// ------------------------------------------------------------------ offline emulator checks (modsettings/test/as-vm.js; NOT the engine)
function emu(ast) {
  const { vm, classOf } = require(path.join(REPO, 'test', 'as-vm.js'));
  const K = classOf(ast, CLASS); const FMAST = LP.readStock('common/ui/node_com/scriptpod.ast').buf;
  let fails = 0, n = 0; const ok = (c, m) => { n++; console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) fails++; }; const warns = [];
  const TY = { HEADER: 0, TOGGLE: 1, SLIDER: 2 };
  // the 0.1 DLL's list for the lab's 4 manifests: section header (desc "<title> <version>"), mod header rows (desc ""), rows
  const mk = (sections) => { const rows = []; let key = 0x4d00; for (const [t, cnt, sub] of sections) { rows.push({ key: -1, type: TY.HEADER, display: t, description: `${t} 1.0`, isEnabled: false });
    for (let j = 1; j < cnt; j++) rows.push(sub && j % 20 === 1 ? { key: -1, type: TY.HEADER, display: `${t} part ${j}`, description: '', isEnabled: false } : { key: key++, type: TY.TOGGLE, display: `${t} row ${j}`, description: 'x', isEnabled: true }); }
    return rows; };
  const LAB = [['Showcase', 10, true], ['Stress Test A', 101, true], ['Stress Test B', 101, true], ['Tuning Lab (Test)', 4]];
  const world = (N) => {
    const fm = function FeatureManagerData() { this.AlwaysSendFeatureParams = false; this.AdapterFunctionOverrides = fm.GetFeatureFunctions(); this.NotifyFinishedInitialize = 'FinishedInitializeFeature'; };
    Object.assign(fm, { ADD_NOTIFY: 0, DISCARD_FEATURE_CHANGES: 1, HIGHLIGHT_SETTING: 2, INITIALIZE_FEATURE: 3, IS_FEATURE_CHANGED: 4, IS_FEATURE_INITIALIZED: 5, IS_FEATURE_RESET_NEEDED: 6, LIST_FEATURE_INPUTS: 7, REMOVE_NOTIFY: 8, RESET_FEATURE: 9, SAVE_ALL_SETTINGS: 10, SHUTDOWN_FEATURE: 11,
      GetFeatureFunctions: () => ['AddNotifyListener', 'GameDiscardFeatureChanges', 'GameHighlightSetting', 'GameInitializeFeature', 'GetIsFeatureChanged', 'GetIsFeatureInitialized', 'GetIsFeatureResetNeeded', 'GetListFeatureSettings', 'RemoveNotifyListener', 'GameResetFeature', 'GameSaveAllSettings', 'GameShutdownFeature'] });
    const Adapters = { GetGameModeAdapter: () => ({ IsInGame: () => false }), GetProfileAdapter: () => ({ GetActiveProfileController: () => 0 }), GetInGameAdapter: () => ({ GetControllerId: () => 0 }) }; if (N) Adapters.GetSettingsAdapter = () => N;
    return { madden: { Adapters }, logic_com: { struct: { FeatureManagerData: fm }, utility: { FeatureManager: { TRACE_STRING: '[fm]', CLASS_STRING: 'fm', LOCK_WAITING_ON_CODE: 'lock' }, FeatureManagerEvent: { EVENT_FEATURE_FINISHED_INIT: 'init', EVENT_FEATURE_FINISHED_SHUTDOWN: 'shut', CreateEventFeatureFinishedInit: () => ({ t: 'init' }), CreateEventFeatureFinishedShutdown: () => ({ t: 'shut' }) } } },
      shared_enum: { Settings: { SETTING_TYPE_TOGGLE: TY.TOGGLE, SETTING_TYPE_HEADER: TY.HEADER, SETTING_TYPE_SLIDER: TY.SLIDER } },
      cafe: { utility: { ObjectUtil: { MergeInto: (d, s) => Object.assign(d, s) }, Delegate: { Create: (o, f) => f } }, Global: { Cafe: { GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) } } },
      node_com: { Global: { Game: { Assert: () => {} } } }, Trace: { Print: () => {} } };
  };
  const native = (rows) => ({ calls: [], GetListSettings(c) { this.calls.push(['list', c]); return c === FEED_CAT ? (rows ? rows.map((x) => ({ ...x })) : undefined) : [{ key: 7, type: 1, display: 'Stock row' }]; },
    GetToggleDetails(c, k) { this.calls.push(['det', c, k]); return c === FEED_CAT ? [{ display: 'Off', key: 0, isDefault: true }, { display: 'On', key: 1, isDefault: false }] : undefined; },
    GetSliderDetails(c, k) { this.calls.push(['sld', c, k]); return {}; }, GameAdjustToggleSetting(c, k, v, w) { this.calls.push(['adjt', c, k, v, w]); return []; }, GameAdjustSliderSetting(c, k, v, w) { this.calls.push(['adjs', c, k, v, w]); return []; } });
  const hub = (N, startOn) => {
    const G = world(N); const V = vm(K.c, K.data, G, (m) => warns.push(m));
    let handed = null; const mgr = { RemoveEventListener: () => {}, Shutdown: () => {}, AddEventListener: () => {}, Init: (d) => { mgr.data = d; } };
    const stock = Array.from({ length: 11 }, (_, i) => ({ key: 'BTN' + i, display: 'STOCK ' + i, icon: 'i', isEnabled: true }));
    const m = { mSettingsCategory: 'HUB', mFeatureAdapter: { GetListSettings: () => stock }, mSettingsManager: mgr,
      mTabListProvider: { ClearAllItems: () => {}, AddItemsByItemArray: (a) => { handed = a; }, GetDefaultIndex: () => { const i = handed.findIndex((t) => t.isDefault); return i < 0 ? undefined : i; }, GetItemData: (i) => handed[i] },
      _GetSettingCategory: () => startOn, _GetSettingCategoryKey: (k) => k.replace('BTN', 'CAT'), _SettingsManagerSettingsHubInitFinish: null, _SettingsManagerInitFinish: () => {} };
    m._GetSettingsManagerData = V.method('_GetSettingsManagerData');
    V.method('_SettingsManagerSettingsHubInitFinish').call(m, {});
    return { G, V, m, handed, mgr };
  };
  const ours = (h) => h.handed.slice(11);
  // 1: the lab's 4 manifests -> 4 tabs titled at runtime, each list = its section
  { const N = native(mk(LAB)); const h = hub(N, 'CAT0'); const o = ours(h);
    ok(h.handed.length === 15 && o.map((t) => t.display).join('|') === LAB.map((x) => x[0]).join('|') && o.every((t, i) => t.key === TAB0 + i && t.icon === 'i'), `hub: 11 stock tabs + one per mod, titled from the DLL list: ${o.map((t) => `${t.key}=${t.display}`).join(', ')}`);
    ok(h.mgr.data && h.mgr.data.OverrideAdapter === N, 'hub opens on a stock tab: the native adapter (unchanged)');
    const a = h.m.mMsA; const sizes = o.map((t) => a.GetListSettings(t.key).length);
    ok(sizes.join(',') === LAB.map((x) => x[1]).join(','), `each tab's list = its section only: ${sizes.join(' / ')} rows (the lab's 216 rows over 4 tabs)`);
    ok(a.GetListSettings(TAB0 + 1)[0].display === 'Stress Test A' && a.GetListSettings(TAB0 + 1).some((x) => x.description === '' && x.type === 0), 'a section keeps its own header rows (Stress A part ...) inside its tab');
    ok(a.GetListSettings(MS_CAT).length === 216, 'MOD_SETTINGS (0.1 key) still gets the whole list');
    // a tab change to tab 2: the manager data, the feed object, the category kept for the shutdown key
    h.m.mSettingsCategory = TAB0 + 2; const d = h.V.method('_GetSettingsManagerData').call(h.m);
    ok(d.OverrideAdapter === a && h.m.mFeatureAdapter === a && a.c === TAB0 + 2 && d.KeyParam.key === TAB0 + 2, `tab ${TAB0 + 2}: the feed object (cached), .c = KeyParam.key = ${a.c}`);
    h.m.mSettingsCategory = TAB0 + MAXT; const d2 = h.V.method('_GetSettingsManagerData').call(h.m); ok(d2.OverrideAdapter !== a, `category ${TAB0 + MAXT} (past the pool) is not ours`);
    h.m.mSettingsCategory = 'CAT3'; h.V.method('_GetSettingsManagerData').call(h.m); ok(h.m.mFeatureAdapter === N, 'a stock category: the native adapter again');
    // details / adjust forward with 19795 + the key
    N.calls.length = 0; a.GetToggleDetails(TAB0 + 1, 0x4d20); a.GameAdjustToggleSetting(TAB0 + 1, 0x4d20, 1, 'why');
    ok(N.calls[0][1] === FEED_CAT && N.calls[0][2] === 0x4d20 && N.calls[1][1] === FEED_CAT && N.calls[1][3] === 1 && N.calls[1][4] === 'why', 'details / changes go to category 19795 with the key, value, why unchanged');
    // the stock FeatureManager life cycle with a numeric key: init at once, the shutdown Notify carries the same key
    const FK = classOf(FMAST, 'logic_com.utility.FeatureManager'); const FV = vm(FK.c, FK.data, h.G, (m) => warns.push(m));
    const fm = { dispatched: [] }; for (const nm of ['Init', '_SetupAdapters', '_OnFeatureInitialized', 'Notify', 'GetListInputs', 'ShutdownFeature', 'IsFeatureInitialized', 'IsFeatureShuttingDown', '_LockInput', '_UnlockInput', 'Shutdown', 'SetAbstractStageData']) fm[nm] = FV.method(nm);
    fm.GetCafe = () => ({ Assert: () => {}, GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) }); fm.DispatchEvent = (e) => fm.dispatched.push(e.t);
    h.G.logic_com.utility.FeatureManager.prototype = {};
    fm.Init(d); fm.ShutdownFeature();
    ok(fm.dispatched.join(',') === 'init,shut', `stock FeatureManager with key ${TAB0 + 2}: init at once, shutdown finished (${fm.dispatched.join(',')})`);
    { const fm2 = { dispatched: [] }; for (const nm of Object.keys(fm)) if (typeof fm[nm] === 'function' && nm !== 'DispatchEvent') fm2[nm] = fm[nm]; fm2.DispatchEvent = (e) => fm2.dispatched.push(e.t);
      fm2.Init(d); a.c = TAB0 + 3; fm2.ShutdownFeature(); ok(fm2.dispatched.join(',') === 'init', `control: a wrong shutdown key (${a.c} vs ${TAB0 + 2}) is ignored by the stock Notify, so the key matters`); a.c = TAB0 + 2; }
  }
  // 2: opening Settings straight on our tab 1 (the hub's init param = a tab key)
  { const N = native(mk(LAB)); const h = hub(N, TAB0 + 1); ok(ours(h)[1].isDefault === true && h.mgr.data.OverrideAdapter === h.m.mMsA, 'start on tab 2: isDefault, the feed object runs it'); }
  // 3: more mods than tabs -> the last tab is "More Mods" and holds the rest
  { const many = Array.from({ length: MAXT + 3 }, (_, i) => [`Mod ${String.fromCharCode(65 + i)}`, 3]); const N = native(mk(many)); const h = hub(N, 'CAT0'); const o = ours(h);
    const last = h.m.mMsA.GetListSettings(TAB0 + MAXT - 1);
    ok(o.length === MAXT && o[MAXT - 1].display === 'More Mods' && o[MAXT - 2].display === many[MAXT - 2][0], `${many.length} mods: ${MAXT} tabs, the last = "More Mods"`);
    ok(last.length === 3 * 4 && last.filter((x) => x.type === 0 && x.description !== '').length === 4, `"More Mods" lists the last ${4} mods' sections (${last.length} rows)`); }
  // 4: exactly MAXT mods -> no "More Mods"
  { const many = Array.from({ length: MAXT }, (_, i) => [`Mod ${i}`, 2]); const h = hub(native(mk(many)), 'CAT0'); ok(ours(h).length === MAXT && ours(h)[MAXT - 1].display === `Mod ${MAXT - 1}`, `${MAXT} mods: ${MAXT} titled tabs, no "More Mods"`); }
  // 5: no DLL (empty / undefined list), no mods (the DLL's "Mod Settings" header has an empty description), no native adapter -> one "Mod Settings" tab (0.1)
  for (const [nm, N] of [['empty list', native([])], ['undefined list', native(undefined)], ['no mods', native([{ key: -1, type: 0, display: 'Mod Settings', description: '' }, { key: 0x4d01 + 0x7fff, type: 1, display: 'No mod with settings is installed', description: 'x' }])], ['no adapter', null]]) {
    const h = hub(N, 'CAT0'); const o = ours(h);
    ok(o.length === 1 && o[0].key === MS_CAT && o[0].display === 'Mod Settings', `${nm}: one "Mod Settings" tab (the 0.1 fallback)`);
    if (nm !== 'no mods') { const L = h.m.mMsA.GetListSettings(MS_CAT); ok(L.length === 2 && L[1].key === FEED_FALLBACK, `${nm}: its list = the "plugin is not running" row`); }
  }
  console.log(`emulator: ${n - fails}/${n}${warns.length ? `; warnings: ${[...new Set(warns)].slice(0, 5).join(' | ')}` : ''}`);
  return fails;
}

function main() {
  const stock = LP.readStock(FILE).buf;
  const r = buildProbe(stock); console.log(r.report);
  // every Apt script decodes and re-assembles; BGFA parses (build.js checkAst)
  { const { data, apt1 } = apt.streamsOf(r.ast); let bad = 0, n = 0; for (const ia of apt.initActions(data, apt1)) { const d = apt.decode(data, ia.script); n++; if (!apt.assemble(d.ins, ia.script).bytes.equals(data.subarray(ia.script, d.end))) bad++; }
    must(bad === 0 && bgfa.parse(r.ast).header, 'AST self-check'); console.log(`PASS AST: ${n} Apt scripts decode and re-assemble; BGFA parses`); }
  const fails = emu(r.ast); if (fails) { console.log(`${fails} emulator check(s) FAILED: nothing packaged`); return 1; }
  if (argv.includes('--emu-only')) return 0;
  // package (build.js tabBuild's recipe): UI file only, Test category
  const md5 = (b) => crypto.createHash('md5').update(b).digest('hex'); const sha256 = (b) => crypto.createHash('sha256').update(b).digest('hex');
  const BUILD = path.join(REPO, 'build', 'pertab-probe'); fs.mkdirSync(BUILD, { recursive: true }); fs.mkdirSync(OUTDIR, { recursive: true });
  const f = path.join(BUILD, 'pertab__fb_node_com__scriptpod.ast'); fs.writeFileSync(f, r.ast);
  const title = 'Mod Settings Per-Mod Tabs Probe (Test)'; const out = path.join(BUILD, `${title}.fbmod`);
  const b = BEM.build({ editsPath: '-', outPath: out, basePath: SHELL, standalone: true, slots: [], title, category: TEST_CATEGORY,
    description: `TEST BUILD (probe, not for release). The Mod Menu tab split into one Settings tab per mod (up to ${MAXT}; more mods share a last "More Mods" tab), each titled at runtime from the Mod Settings plugin's list. Use with the Mod Settings plugin (modsettings.dll 0.1 beta). UI file only: no save data. Replaces "Mod Settings Tab (Test)": do not load both.`,
    legacyAdd: [{ path: FILE, file: f }] });
  for (const l of b.log.filter((x) => /legacy-add/.test(x))) console.log('  ' + l);
  const node = (a) => cp.spawnSync(process.execPath, a, { cwd: ROOT, encoding: 'utf8', maxBuffer: 1 << 26 });
  let bad = 0; const say = (c, m) => { console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) bad++; };
  { const info = fbf.parse(out); const L = info.resources.filter((x) => /^legacy;(?!Collector \()/.test(x.userData || '') && x.type === 'Chunk'); say(L.length === 1, `${path.basename(out)}: carries exactly 1 file`); }
  say(LP.readFromMod(out, FILE).equals(r.ast), 'fb_node_com/scriptpod.ast reads back == the built file');
  { const st = LP.readStock(FILE); const vr = LP.verifyMod(out, { files: [{ path: st.path, collectors: st.collectors, sha1: crypto.createHash('sha1').update(r.ast).digest('hex'), chunkGuid: LP.chunkGuid('ExpressionMod', st.path) }] }); say(vr.problems.length === 0, `UI file wired${vr.problems.length ? ' -- ' + vr.problems.join('; ') : ''}`); }
  for (const t of ['audit-fbmod-metadata.js', 'verify-mod-tu.js', 'preflight.js']) { const v = node([T(t), out]); say(v.status === 0, `${t} exit ${v.status}`); }
  if (bad) { console.log(`${bad} package check(s) FAILED: nothing copied`); return 1; }
  const dst = path.join(OUTDIR, path.basename(out)); fs.copyFileSync(out, dst); const buf = fs.readFileSync(dst);
  const info = [
    `Mod Menu per-mod tabs PROBE (TEST, not for release), built ${new Date().toISOString()} by modsettings/probes/pertab-probe.js (--max ${MAXT}).`,
    `Doc: docs/MOD-MENU-PER-MOD-TABS-2026-11-12.md (section 6).`, '',
    `${path.basename(dst)}  md5 ${md5(buf)}  sha256 ${sha256(buf)}  ${buf.length} B`,
    `  carries only common/ui/fb_node_com/scriptpod.ast (ast md5 ${md5(r.ast)}). Replaces "Mod Settings Tab (Test)" (681bdc6d): load ONE of them.`,
    `  tabs: one per DLL section header, keys ${TAB0}..${TAB0 + MAXT - 1}; rows still from category ${FEED_CAT}; no DLL -> one "Mod Settings" tab (0.1 fallback).`, '',
    'Plugin: NOT included. Use the published DLL unchanged with the four lab manifests: exports/build-mods/modmenu-combined-test/Plugins',
    '  (modsettings.dll 8e8a7ecb, mods: showcase.json, stress-a.json, stress-b.json, tuning-lab.json = 216 list rows: 10 / 101 / 101 / 4 per tab).', '',
    'Lab (NOT queued; title screen, no save, runBy auto; a copy of modmenu-combined-1 with this .fbmod in place of the tab .fbmod):',
    '  1. stage the combined plugin, launch with ONLY this .fbmod, wait for "tab feed installed".',
    '  2. main menu -> Settings: shot of the tab bar. PASS = 15 tabs: the 11 stock ones, then Showcase, Stress Test A, Stress Test B, Tuning Lab (Test).',
    '  3. RB to each of our 4 tabs: a shot each. PASS = title, tab bar and button bar drawn on all 4 (Stress A / B = 101 rows each; the same 216 rows',
    '     made the whole screen vanish in ONE tab). modsettings.log "tab list" lines show the DLL asked once per tab open plus once per hub open.',
    '  4. on Stress Test B: scroll to the last row (Down x 110): shot. Change one row; Back; settings.json has it; reopen shows it.',
    '  5. LB back to a stock tab (Game Options draws), Back to the main menu; crash dumps since launch = 0; real saves md5 unchanged.',
    '  Judge by screenshots (the drivers\' OCR reads nothing on this screen).',
  ];
  fs.writeFileSync(path.join(OUTDIR, 'BUILD-INFO.txt'), info.join('\r\n') + '\r\n');
  console.log(`ALL PASS: ${path.relative(ROOT, dst)}  md5 ${md5(buf)}`);
  return 0;
}
if (require.main === module) process.exit(main());
module.exports = { buildProbe, MAXT, TAB0 };
