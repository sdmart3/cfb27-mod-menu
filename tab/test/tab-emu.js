#!/usr/bin/env node
// tab-emu.js: offline tests of the Mod Menu tab (.fbmod side, 2026-11-11): the BUILT GenericSettings class copy with the FEED adapter
// (src/ui-build-ms.js buildPage { feed: true }) run in test/as-vm.js, against a mock of the native settings adapter that answers category
// 19795 the way modsettings.dll's slot wrappers do (dll-plugin/dll/src/mstab.h). NOT the engine.
//   A  _GetSettingsManagerData: MOD_SETTINGS -> the feed object (cached); any other category -> the native adapter
//   B  the stock FeatureManager on the feed object: Init lists the DLL's rows; ShutdownFeature -> Notify -> finished-shutdown
//   C  the five data calls go to the native adapter with category 19795 and the key / value / why unchanged; the DLL's answers pass through
//   D  no DLL: the native list for 19795 is empty / undefined -> a header + one greyed "plugin is not running" row; its details = one item;
//      a change of it never reaches the native side; no native adapter at all -> the same rows
//   E  undefined native answers -> [] / {} (the stock code iterates them)
//   F  the Settings hub tab (unchanged Step 1 splice): appended last, isDefault when the hub opens on it
//   G  the DLL's OWN rows: modsettings-dll's tab self-test status file (dll/out/t-tab/modsettings-status.json, "tab.rows") fed through the
//      page: every row arrives, keys / types / labels unchanged (skipped with a note when that file is missing: run node dll/build.js first)
//   H  a change round trip through the mock DLL: adjust -> the next details call shows it; Back + reopen (a new screen) shows it
// usage: node modsettings/test/tab-emu.js
'use strict';
const fs = require('fs'); const path = require('path');
const REPO = path.join(__dirname, '..'); const ROOT = path.join(REPO, '..');
const LP = require(path.join(ROOT, 'tools', 'legacy-pack.js'));
const UI = require('../src/ui-build-ms.js');
const { vm, classOf } = require('./as-vm.js');

let fails = 0, checks = 0; const ok = (c, m) => { checks++; console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) fails++; };
const warnings = []; const warn = (m) => warnings.push(m);
const STOCK = LP.readStock(UI.FILE).buf;
const FMAST = LP.readStock('common/ui/node_com/scriptpod.ast').buf;
const page = UI.buildPage(STOCK, null, { feed: true });
const K = classOf(page.ast, UI.CLASS);
const T = { HEADER: 0, TOGGLE: 1, SLIDER: 2 };   // the native type numbers (the DLL writes these; shared_enum must agree in game)

// a mock of the native settings adapter + modsettings.dll's wrappers: category 19795 answered from `rows` / `opts`, others = stock rows
function nativeMock(o = {}) {
  const calls = []; const vals = Object.assign({}, o.vals || {});
  const rows = o.rows || [
    { key: -1, type: T.HEADER, display: 'Auto Kick', description: 'Auto Kick 0.3.0', isEnabled: false },
    { key: 0x4d01, type: T.TOGGLE, display: 'Auto Kick', description: 'The game aims and times kicks.', isEnabled: true },
    { key: 0x4d02, type: T.TOGGLE, display: 'Kick Style', description: 'Safe takes the sure kick.', isEnabled: true },
    { key: 0x4d03, type: T.SLIDER, display: 'Fine', description: 'A slider row.', isEnabled: true },
  ];
  const opts = o.opts || { [0x4d01]: ['Off', 'On'], [0x4d02]: ['Safe', 'Balanced', 'Aggressive'] };
  return { native: true, calls, vals,
    GetListSettings: (cat) => { calls.push(['list', cat]); if (cat !== UI.FEED_CAT) return [{ key: 7, type: T.TOGGLE, display: 'Stock row' }]; return o.noDll ? o.emptyAs : rows.map((r) => ({ ...r })); },
    GetToggleDetails: (cat, key) => { calls.push(['det', cat, key]); if (o.noDll || cat !== UI.FEED_CAT) return o.detAs; const op = opts[key]; if (!op) return o.detAs; const cur = key in vals ? vals[key] : 1; return op.map((d, j) => ({ display: d, key: j, isDefault: j === cur })); },
    GetSliderDetails: (cat, key) => { calls.push(['sld', cat, key]); return o.noDll || cat !== UI.FEED_CAT ? o.sldAs : { min: 0, max: 10, step: 1, defaultValue: key in vals ? vals[key] : 3, resetValue: 3, key }; },
    GameAdjustToggleSetting: (cat, key, v, why) => { calls.push(['adjt', cat, key, v, why]); if (!o.noDll && cat === UI.FEED_CAT) vals[key] = v; return o.adjAs; },
    GameAdjustSliderSetting: (cat, key, v, why) => { calls.push(['adjs', cat, key, v, why]); if (!o.noDll && cat === UI.FEED_CAT) vals[key] = v; return o.adjAs; },
  };
}
function world(N) {
  const fm = function FeatureManagerData() { this.AlwaysSendFeatureParams = false; this.AdapterFunctionOverrides = fm.GetFeatureFunctions(); this.NotifyFinishedInitialize = 'FinishedInitializeFeature'; };
  Object.assign(fm, { ADD_NOTIFY: 0, DISCARD_FEATURE_CHANGES: 1, HIGHLIGHT_SETTING: 2, INITIALIZE_FEATURE: 3, IS_FEATURE_CHANGED: 4, IS_FEATURE_INITIALIZED: 5, IS_FEATURE_RESET_NEEDED: 6, LIST_FEATURE_INPUTS: 7, REMOVE_NOTIFY: 8, RESET_FEATURE: 9, SAVE_ALL_SETTINGS: 10, SHUTDOWN_FEATURE: 11,
    GetFeatureFunctions: () => ['AddNotifyListener', 'GameDiscardFeatureChanges', 'GameHighlightSetting', 'GameInitializeFeature', 'GetIsFeatureChanged', 'GetIsFeatureInitialized', 'GetIsFeatureResetNeeded', 'GetListFeatureSettings', 'RemoveNotifyListener', 'GameResetFeature', 'GameSaveAllSettings', 'GameShutdownFeature'] });
  const Adapters = { GetGameModeAdapter: () => ({ IsInGame: () => false }), GetProfileAdapter: () => ({ GetActiveProfileController: () => 0 }), GetInGameAdapter: () => ({ GetControllerId: () => 0 }) };
  if (N !== null) Adapters.GetSettingsAdapter = () => N;
  return {
    madden: { Adapters },
    logic_com: { struct: { FeatureManagerData: fm }, utility: { FeatureManager: { TRACE_STRING: '[fm]', CLASS_STRING: 'fm', LOCK_WAITING_ON_CODE: 'lock' }, FeatureManagerEvent: { EVENT_FEATURE_FINISHED_INIT: 'init', EVENT_FEATURE_FINISHED_SHUTDOWN: 'shut', CreateEventFeatureFinishedInit: () => ({ t: 'init' }), CreateEventFeatureFinishedShutdown: () => ({ t: 'shut' }) } } },
    shared_enum: { Settings: { SETTING_TYPE_TOGGLE: T.TOGGLE, SETTING_TYPE_HEADER: T.HEADER, SETTING_TYPE_SLIDER: T.SLIDER, SETTING_TYPE_TEXTINPUT: 3, SETTING_SETTINGS_HUB: 'HUB' } },
    cafe: { utility: { ObjectUtil: { MergeInto: (d, s) => Object.assign(d, s), Trace: () => {} }, Delegate: { Create: (o, f) => f } }, Global: { Cafe: { GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) } } },
    node_com: { Global: { Game: { Assert: (m, c) => { if (!c) warn('Assert ' + m); } } } },
    Trace: { Print: () => {} },
  };
}
function open(N, model = {}) {
  const G = world(N); const V = vm(K.c, K.data, G, warn);
  const m = Object.assign({ mSettingsCategory: UI.MS_CAT }, model);
  const data = V.method('_GetSettingsManagerData').call(m);
  return { G, V, m, data, a: m.mFeatureAdapter };
}

console.log(`page: ${page.report}`);
// ================================================================================================ A
console.log('\n== A: _GetSettingsManagerData');
{
  const N = nativeMock(); const x = open(N);
  ok(x.data.OverrideAdapter === x.a && x.a && !x.a.native && typeof x.a.GetListSettings === 'function' && x.m.mMsA === x.a, 'MOD_SETTINGS: OverrideAdapter = mFeatureAdapter = the feed object (cached in mMsA)');
  const needs = ['AddNotifyListener', 'RemoveNotifyListener', 'GameInitializeSettingCategory', 'GameShutdownSettingCategory', 'GetIsFeatureInitialized', 'GetIsFeatureChanged', 'GetIsFeatureResetNeeded', 'GameSaveAllSettings', 'GameDiscardFeatureChanges', 'GameResetFeature', 'GameHighlightSetting', 'GetListSettings', 'GetToggleDetails', 'GetSliderDetails', 'GameAdjustToggleSetting', 'GameAdjustSliderSetting'];
  ok(needs.every((n) => typeof x.a[n] === 'function'), `the feed object has every method GenericSettings / FeatureManager call by name (${needs.length})`);
  ok(x.data.KeyParam && x.data.KeyParam.key === UI.MS_CAT, 'KeyParam.key = MOD_SETTINGS (the stock code kept)');
  x.m.mSettingsCategory = 1; const d3 = x.V.method('_GetSettingsManagerData').call(x.m);
  ok(d3.OverrideAdapter === N && x.m.mFeatureAdapter === N, 'any other category: the native settings adapter (stock behaviour)');
  ok(x.a.GetIsFeatureInitialized(UI.MS_CAT) === true && x.a.GameSaveAllSettings() === true && x.a.GetIsFeatureChanged() === false, 'initialized true, save true, changed false');
}
// ================================================================================================ B
console.log('\n== B: the stock FeatureManager on the feed object');
{
  const N = nativeMock(); const x = open(N);
  const FK = classOf(FMAST, 'logic_com.utility.FeatureManager'); const FV = vm(FK.c, FK.data, x.G, warn);
  const fm = { dispatched: [] };
  for (const n of ['Init', '_SetupAdapters', '_OnFeatureInitialized', 'Notify', 'GetListInputs', 'ShutdownFeature', 'IsFeatureInitialized', 'IsFeatureShuttingDown', '_LockInput', '_UnlockInput', 'Shutdown', 'SetAbstractStageData']) fm[n] = FV.method(n);
  fm.GetCafe = () => ({ Assert: (m, c) => { if (!c) warn('fm Assert ' + m); }, GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) });
  fm.DispatchEvent = (e) => fm.dispatched.push(e.t);
  x.G.logic_com.utility.FeatureManager.prototype = {};
  fm.Init(x.data);
  ok(fm.mFeatureAdapter === x.a && x.a.l === fm && fm.dispatched[0] === 'init', 'Init: the feed object is the feature adapter, init finished at once');
  ok(Array.isArray(fm.mInputList) && fm.mInputList.length === 4 && fm.mInputList[1].display === 'Auto Kick' && N.calls.some((c) => c[0] === 'list' && c[1] === UI.FEED_CAT), `Init listed the DLL's ${fm.mInputList && fm.mInputList.length} rows through native GetListSettings(19795)`);
  fm.ShutdownFeature();
  ok(fm.dispatched[1] === 'shut' && x.a.l === undefined && fm.mAdapterShuttingDown === false, 'ShutdownFeature -> Notify("FinishedShutdownSettingCategory") -> finished, listener removed');
}
// ================================================================================================ C
console.log('\n== C: the data calls go to the native adapter with category 19795');
{
  const N = nativeMock(); const x = open(N);
  const L = x.a.GetListSettings(UI.MS_CAT);
  ok(L.length === 4 && L[0].type === T.HEADER && L[0].display === 'Auto Kick' && L[2].key === 0x4d02 && x.a.f === false, 'list: the DLL rows pass through unchanged (4 rows; header first)');
  ok(N.calls.filter((c) => c[0] === 'list').every((c) => c[1] === UI.FEED_CAT), 'list: asked with category 19795 (never the string MOD_SETTINGS)');
  const D = x.a.GetToggleDetails(UI.MS_CAT, 0x4d02);
  ok(D.length === 3 && D[1].isDefault === true && N.calls.some((c) => c[0] === 'det' && c[1] === UI.FEED_CAT && c[2] === 0x4d02), 'details: forwarded (19795, key); the answer passes through');
  const S = x.a.GetSliderDetails(UI.MS_CAT, 0x4d03);
  ok(S.max === 10 && S.defaultValue === 3 && N.calls.some((c) => c[0] === 'sld' && c[1] === UI.FEED_CAT && c[2] === 0x4d03), 'slider details: forwarded (19795, key)');
  x.a.GameAdjustToggleSetting(UI.MS_CAT, 0x4d02, 2, 'Update Changed Generic Setting');
  ok(N.calls.some((c) => c[0] === 'adjt' && c[1] === UI.FEED_CAT && c[2] === 0x4d02 && c[3] === 2 && c[4] === 'Update Changed Generic Setting'), 'toggle change: forwarded (19795, key, value, why)');
  x.a.GameAdjustSliderSetting(UI.MS_CAT, 0x4d03, 7, 'why');
  ok(N.calls.some((c) => c[0] === 'adjs' && c[1] === UI.FEED_CAT && c[2] === 0x4d03 && c[3] === 7), 'slider change: forwarded (19795, key, value)');
}
// ================================================================================================ D
console.log('\n== D: no DLL / no native adapter: the fallback rows');
for (const [what, N] of [['no DLL (empty list)', nativeMock({ noDll: true, emptyAs: [] })], ['no DLL (undefined list)', nativeMock({ noDll: true, emptyAs: undefined })], ['no native adapter', null]]) {
  const x = open(N); const L = x.a.GetListSettings(UI.MS_CAT);
  ok(L.length === 2 && L[0].type === T.HEADER && L[0].display === 'Mod Settings' && L[1].key === UI.FEED_FALLBACK && L[1].type === T.TOGGLE && L[1].isEnabled === false && /not running/.test(L[1].display) && x.a.f === true, `${what}: a header + one greyed "${L[1] && L[1].display}" row`);
  const D = x.a.GetToggleDetails(UI.MS_CAT, UI.FEED_FALLBACK);
  ok(D.length === 1 && D[0].isDefault === true, `${what}: the fallback row's details = one item (greyed by the stock code)`);
  const before = N ? N.calls.length : 0; const R = x.a.GameAdjustToggleSetting(UI.MS_CAT, UI.FEED_FALLBACK, 1, 'w');
  ok(Array.isArray(R) && R.length === 0 && (N ? N.calls.length === before : true), `${what}: a change of the fallback row is ignored (the native side is not called)`);
}
// ================================================================================================ E
console.log('\n== E: undefined native answers');
{
  const N = nativeMock({ detAs: undefined, sldAs: undefined, adjAs: undefined }); const x = open(N);
  ok(Array.isArray(x.a.GetToggleDetails(UI.MS_CAT, 0x4999)) && x.a.GetToggleDetails(UI.MS_CAT, 0x4999).length === 0, 'details undefined -> []');
  const s = x.a.GetSliderDetails(UI.MS_CAT, 0x4999); ok(s && typeof s === 'object' && !Array.isArray(s), 'slider details undefined -> {}');
  const r = x.a.GameAdjustToggleSetting(UI.MS_CAT, 0x4d01, 0, 'w'); ok(Array.isArray(r) && r.length === 0, 'toggle change answer undefined -> []');
  const r2 = x.a.GameAdjustSliderSetting(UI.MS_CAT, 0x4d03, 1, 'w'); ok(Array.isArray(r2) && r2.length === 0, 'slider change answer undefined -> []');
  const x2 = open(null); ok(Array.isArray(x2.a.GetToggleDetails(UI.MS_CAT, 0x4d01)) && x2.a.GameAdjustSliderSetting(UI.MS_CAT, 1, 1, 'w').length === 0, 'no native adapter: details [] and changes [] (no throw)');
}
// ================================================================================================ F
console.log('\n== F: the Settings hub tab');
for (const startOn of ['AUD', UI.MS_CAT]) {
  const N = nativeMock(); const G = world(N); const V = vm(K.c, K.data, G, warn);
  let handed = null; const mgr = { RemoveEventListener: () => {}, Shutdown: () => {}, AddEventListener: () => {}, Init: (d) => { mgr.data = d; } };
  const tabs = [{ key: 'BTN_GAME', display: 'GAME OPTIONS', icon: 'g', isEnabled: true }, { key: 'BTN_AUD', display: 'ACCESSIBILITY', icon: 'a', isEnabled: true }];
  const m = { mSettingsCategory: 'HUB', mFeatureAdapter: { GetListSettings: () => tabs }, mSettingsManager: mgr,
    mTabListProvider: { ClearAllItems: () => {}, AddItemsByItemArray: (a) => { handed = a; }, GetDefaultIndex: () => handed.findIndex((t) => t.isDefault), GetItemData: (i) => handed[i] },
    _GetSettingCategory: () => startOn, _GetSettingCategoryKey: (k) => k.replace('BTN_', ''), _SettingsManagerSettingsHubInitFinish: null, _SettingsManagerInitFinish: () => {} };
  m._GetSettingsManagerData = V.method('_GetSettingsManagerData');
  V.method('_SettingsManagerSettingsHubInitFinish').call(m, {});
  ok(handed && handed.length === 3 && handed[2].key === UI.MS_CAT && handed[2].display === 'Mod Settings', `tab bar: the native tabs + "Mod Settings" last (start ${startOn})`);
  ok(handed[2].isDefault === (startOn === UI.MS_CAT) && (startOn === UI.MS_CAT ? mgr.data.OverrideAdapter === m.mMsA && m.mMsA.GetSliderDetails : mgr.data.OverrideAdapter === N), `the hub starts on ${startOn} with the ${startOn === UI.MS_CAT ? 'feed' : 'native'} adapter`);
}
// ================================================================================================ G
console.log('\n== G: the DLL\'s own rows (its tab self-test status file)');
{
  const SF = path.join(ROOT, 'modsettings-dll', 'dll', 'out', 't-tab', 'modsettings-status.json');
  if (!fs.existsSync(SF)) { console.log(`NOTE: ${SF} missing (run node dll-plugin/dll/build.js first): G skipped`); warn('G skipped: no DLL status file'); }
  else {
    const st = JSON.parse(fs.readFileSync(SF, 'utf8')); const rows = st.tab && st.tab.rows || [];
    const dllRows = rows.map((r) => ({ key: r.key, type: r.type, display: r.display, description: '', isEnabled: !!r.enabled }));
    const N = nativeMock({ rows: dllRows }); const x = open(N); const L = x.a.GetListSettings(UI.MS_CAT);
    ok(st.tab.category === UI.FEED_CAT, `the DLL's category (${st.tab.category}) = the page's (${UI.FEED_CAT})`);
    ok(L.length === rows.length && rows.length >= 4 && L.every((r, k) => r.key === rows[k].key && r.type === rows[k].type && r.display === rows[k].display), `all ${rows.length} DLL rows arrive in order, keys / types / labels unchanged`);
    const heads = L.filter((r) => r.type === T.HEADER && r.key === -1).map((r) => r.display);
    ok(['Auto Kick', 'Mod Settings Demo (Test)', 'Pocket Presence', 'Tab Test Dynasty'].every((t) => heads.includes(t)), `one header per mod (${heads.join(' | ')})`);
    ok(L.filter((r) => r.type !== T.HEADER).every((r) => r.key >= 0x4cf0 && r.key !== UI.FEED_FALLBACK), 'every DLL row key is in the private range and never the page\'s fallback key');
  }
}
// ================================================================================================ H
console.log('\n== H: a change round trip through the mock DLL; Back + reopen');
{
  const N = nativeMock(); const x = open(N);
  const cur = (a, k) => a.GetToggleDetails(UI.MS_CAT, k).findIndex((d) => d.isDefault);
  ok(cur(x.a, 0x4d02) === 1, 'Kick Style shows Balanced');
  x.a.GameAdjustToggleSetting(UI.MS_CAT, 0x4d02, 2, 'Update Changed Generic Setting');
  ok(cur(x.a, 0x4d02) === 2, 'after the change: Aggressive (the DLL keeps the value, the page asks again)');
  const y = open(N);   // Back out and reopen: a new screen, a new feed object, the same DLL
  ok(y.a !== x.a && cur(y.a, 0x4d02) === 2, 'Back + reopen: a new feed object shows Aggressive (the value lives in the DLL, not the page)');
}
console.log(`\n${checks - fails}/${checks} PASS${warnings.length ? `; warnings: ${[...new Set(warnings)].slice(0, 6).join(' | ')}` : ''}`);
process.exit(fails ? 1 : 0);
