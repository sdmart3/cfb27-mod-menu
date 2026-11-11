#!/usr/bin/env node
// ms-emu.js: offline tests of Mod Settings Step 1 (NOT the engine). The page = the BUILT GenericSettings class copy run in test/as-vm.js; the stock
// logic_com.utility.FeatureManager (node_com/scriptpod.ast) runs against our adapter object; the broker = the GENERATED .spexasm sources run in
// Preseason Weeks' SPEX emulator (dynasty-extensions/parts/preseason-weeks/test/combo-emu.js) on JS records.
//   A  _GetSettingsManagerData: our category -> our adapter (cached); any other -> the native adapter again
//   B  the stock FeatureManager drives our adapter: Init -> list; ShutdownFeature -> Notify -> finished-shutdown event + RemoveNotifyListener
//   C  rows render from the manifests (header + toggles, keys, labels, help); inert texts in a dynasty / outside one
//   D  the broker: issue (record with defaults, one request per band row, Priority = prio + value, 8 forms), page reads them, an answer -> #43 -> bits +
//      Priority + Refresh, page shows the new value, a second issue run adds nothing, save + load (no control link) still lands, bad answers ignored
//   E  two mods share record 0 + use record 1: answers never touch the other mod's bits; every request maps to its own row only
//   F  Dynasty Extensions' own requests (request rows): On / Off invert, Spring Game 4 options, newest wins, broker requests never read as DE rows
//   G  Playoff Customizer case: the native list is empty -> the FranTk list (Initialize, read, Shutdown once)
//   H  the Settings hub tab: appended last, a copy of the last native tab, isDefault when the hub opens on it
//   I  probe pages: 40 / 120 memory rows render, a change stays while the page lives
//   J  the record table: no record and no request at 149 rows (ADD_CAP); PC 1.3.0 at 64 teams still fits (row-budget numbers + our records)
// usage: node --max-old-space-size=8192 modsettings/test/ms-emu.js [--no-budget]
'use strict';
const fs = require('fs'); const path = require('path'); const os = require('os'); const cp = require('child_process');
const REPO = path.join(__dirname, '..'); const ROOT = path.join(REPO, '..');
const LP = require(path.join(ROOT, 'tools', 'legacy-pack.js'));
const MAN = require('../src/manifest.js'); const UI = require('../src/ui-build-ms.js'); const GEN = require('../src/gen-ms.js');
const { vm, classOf } = require('./as-vm.js');
const C = require(path.join(ROOT, 'dynasty-extensions', 'parts', 'preseason-weeks', 'test', 'combo-emu.js'));
const BUILD = require('../build.js');

let fails = 0, checks = 0; const ok = (c, m) => { checks++; console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) fails++; };
const warnings = []; let SECTION = ''; const warn = (m) => warnings.push(SECTION + ': ' + m);
const SLOTS = path.join(REPO, 'slots.json'); const MF = (id) => path.join(REPO, 'manifests', `${id}.json`);
const LL = 'Legacy Leaderboard';
const STOCK = LP.readStock(UI.FILE).buf;
const FMAST = LP.readStock('common/ui/node_com/scriptpod.ast').buf;

// ------------------------------------------------------------------------------------------------ world mocks
const NATIVE = { native: true, GetListSettings: () => [{ key: 1, type: 'T', display: 'Native row' }] };
function ucmMock(o = {}) {
  const reqs = o.requests || []; const responded = [];
  return { reqs, responded,
    GetSeasonInfo: () => (o.noDynasty ? {} : { seasonWeek: 3 }),
    GetListActionRequestIds: () => (o.hideNative ? [] : reqs.map((r) => r.id)),
    GetActionRequestData: (id) => { const r = reqs.find((x) => x.id === id); return r ? { requestTypeName: r.typeName, description: r.description, priority: r.priority } : undefined; },
    GetActionRequestResponseList: (id) => { const r = reqs.find((x) => x.id === id); return r ? new Array(r.nResp).fill({}) : undefined; },
    ActionRequestRespond: (id, v) => { responded.push([id, v]); if (o.onRespond) o.onRespond(id, v); },
  };
}
function world(o = {}) {
  const fm = function FeatureManagerData() { this.AlwaysSendFeatureParams = false; this.AdapterFunctionOverrides = fm.GetFeatureFunctions(); this.NotifyFinishedInitialize = 'FinishedInitializeFeature'; };
  Object.assign(fm, { ADD_NOTIFY: 0, DISCARD_FEATURE_CHANGES: 1, HIGHLIGHT_SETTING: 2, INITIALIZE_FEATURE: 3, IS_FEATURE_CHANGED: 4, IS_FEATURE_INITIALIZED: 5, IS_FEATURE_RESET_NEEDED: 6, LIST_FEATURE_INPUTS: 7, REMOVE_NOTIFY: 8, RESET_FEATURE: 9, SAVE_ALL_SETTINGS: 10, SHUTDOWN_FEATURE: 11,
    GetFeatureFunctions: () => ['AddNotifyListener', 'GameDiscardFeatureChanges', 'GameHighlightSetting', 'GameInitializeFeature', 'GetIsFeatureChanged', 'GetIsFeatureInitialized', 'GetIsFeatureResetNeeded', 'GetListFeatureSettings', 'RemoveNotifyListener', 'GameResetFeature', 'GameSaveAllSettings', 'GameShutdownFeature'] });
  const events = [];
  const G = {
    madden: { Adapters: { GetSettingsAdapter: () => NATIVE, GetGameModeAdapter: () => ({ IsInGame: () => false }), GetProfileAdapter: () => ({ GetActiveProfileController: () => 0 }), GetInGameAdapter: () => ({ GetControllerId: () => 0 }) } },
    logic_com: { struct: { FeatureManagerData: fm }, utility: { FeatureManager: { TRACE_STRING: '[fm]', CLASS_STRING: 'fm', LOCK_WAITING_ON_CODE: 'lock' }, FeatureManagerEvent: { EVENT_FEATURE_FINISHED_INIT: 'init', EVENT_FEATURE_FINISHED_SHUTDOWN: 'shut', CreateEventFeatureFinishedInit: () => ({ t: 'init' }), CreateEventFeatureFinishedShutdown: () => ({ t: 'shut' }) } } },
    shared_enum: { Settings: { SETTING_TYPE_TOGGLE: 'T', SETTING_TYPE_HEADER: 'H', SETTING_TYPE_SLIDER: 'S', SETTING_TYPE_TEXTINPUT: 'X', SETTING_SETTINGS_HUB: 'HUB' } },
    cafe: { utility: { ObjectUtil: { MergeInto: (d, s) => Object.assign(d, s), Trace: () => {} }, Delegate: { Create: (o, f) => f } }, Global: { Cafe: { GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) } } },
    node_com: { Global: { Game: { Assert: (m, c) => { if (!c) warn('Assert ' + m); } } } },
    Trace: { Print: () => {} },
  };
  if (o.ucm) G.madden.ultimatecareer = { Adapters: { GetUcmAdapter: () => o.ucm } };
  if (o.fk) G.ION_FranTkRequest = o.fk;
  return { G, events };
}
const pageFor = (files, opts = {}) => { const T = MAN.load(files, opts.slots || SLOTS, opts); const p = UI.buildPage(STOCK, T); const k = classOf(p.ast, UI.CLASS); return { T, p, k }; };
function adapterOf(pg, w, model = {}) {
  const V = vm(pg.k.c, pg.k.data, w.G, warn);
  const m = Object.assign({ mSettingsCategory: UI.MS_CAT }, model);
  const data = V.method('_GetSettingsManagerData').call(m);
  return { V, m, data, a: m.mFeatureAdapter };
}
const list = (a) => a.GetListSettings(UI.MS_CAT);
const shown = (a, key) => { const d = a.GetToggleDetails(UI.MS_CAT, key); const s = d.filter((x) => x.isDefault); return s.length === 1 ? s[0].display : `?${JSON.stringify(d)}`; };
const keyOf = (T, mod, id) => T.rows.find((r) => r.mod === mod && r.id === id).key;

// ------------------------------------------------------------------------------------------------ the broker world (SPEX emulator)
const SRC = path.join(os.tmpdir(), `ms-emu-${process.pid}`);
function brokerSources(T, host = 46) { fs.mkdirSync(SRC, { recursive: true }); GEN.generate(SRC, T, { host }); return { issue: fs.readFileSync(path.join(SRC, 'ms-issue.spexasm'), 'utf8'), h43: fs.readFileSync(path.join(SRC, 'ms-h43.spexasm'), 'utf8') }; }
function save(o = {}) {
  const arcs = (o.rows || [30716, -6100]).map((v) => ({ __t: 'CrossArcData', Id: 0, Value: v })); arcs.__t = 'CrossArcData'; arcs.__cap = arcs.length;
  const fu = { __t: 'FranchiseUser', CrossArcDataArray: arcs };
  const list = (o.open || []).slice();
  const tips = [{ __t: 'WeeklyTip', SeasonStage: C.ENUM('SeasonStage.NFLSeason'), Week: 5, Title: 'Depth Chart' }, { __t: 'WeeklyTip', SeasonStage: C.ENUM('SeasonStage.NFLSeason'), Week: 15, Title: LL }];
  const cf = { __t: 'FranchiseServer_CareerFlow', UserRegistry: { Users: [null, fu] }, RequestServer: { '#1': list }, Franchise: { LeagueOwner: fu }, WeeklyTips: tips };
  return { fu, cf, list };
}
// 2026-11-10 the Stage.End auto-answer check (docs/HIDDEN-REQUEST-UNINSTALL-CRASH-AUDIT-2026-11-09.md 3 / 4B; tools/hidden-request-optb.js): after every issue run, no broker
// request (marker 16515072..16777099) may pass the exe walker's test (Resolution 0 and (!IsResponseRequired or DefaultResponse set)): with this mod removed that is the stock #43 on a null PlayerSigningEval
const OPTB = require(path.join(ROOT, 'tools', 'hidden-request-optb.js')); const AA = { runs: 0, exposed: 0, who: [] };
const msOurs = (r) => r && r.__t === 'LostPracticeSquadPlayerRequest' && r.CpuResponseMinDuration >= 16515072 && r.CpuResponseMinDuration <= 16777099;
const aaExposed = (S) => S.list.filter((r) => msOurs(r) && OPTB.wouldAutoAnswer(r) && OPTB.stockH43Crashes(r));
const aaProbe = (S) => { AA.runs++; const x = aaExposed(S); AA.exposed += x.length; if (x.length && AA.who.length < 3) AA.who.push(x.map((r) => r.CpuResponseMinDuration).join(',')); };
function runIssue(S, src) { const ctx = { self: { CareerFlow: S.cf }, list: S.list, violations: [] }; C.run(src.issue, ctx); aaProbe(S); return ctx; }
function answer(S, src, req, sel, { lostControl = false } = {}) {
  const form = req.ResponseForms[sel]; req.Response = lostControl ? { ...form, Control: null } : form;
  if (lostControl) { req.ResponseForms = req.ResponseForms.slice(); req.ResponseForms[sel] = req.Response; }
  if (!lostControl) form.Control.Selection = sel;
  const ctx = { self: req, list: S.list, violations: [] }; C.run(src.h43, ctx); return ctx;
}
// the page's view of the save's open list (ids 1000+)
function ucmOfSave(S, src, o = {}) {
  const u = ucmMock({ ...o, requests: [], onRespond: (id, v) => { const r = S.list[id - 1000]; if (r) answer(S, src, r, v); sync(); } });
  const sync = () => { u.reqs.length = 0; S.list.forEach((r, k) => { if (r && r.__t === 'LostPracticeSquadPlayerRequest') u.reqs.push({ id: 1000 + k, typeName: r.TypeName, description: r.Description, priority: r.Priority, nResp: (r.ResponseForms || []).length, rec: r }); }); };
  sync(); u.sync = sync; return u;
}
const recVal = (S, T, rec) => { const lo = T.band.base + rec * T.band.recordSize; const x = S.fu.CrossArcDataArray.filter((c) => c.Value >= lo && c.Value < lo + T.band.recordSize); return x.length === 1 ? x[0].Value : x.length ? 'DUP' : null; };
const bits = (S, T, r) => { const v = recVal(S, T, r.slot.record); return v === null ? null : MAN.bitsOf(T.band, v, r.slot); };

(async () => {
  // ================================================================================================ A
  console.log('\n== A: _GetSettingsManagerData');
  {
    const pg = pageFor([MF('modsettings-demo')]); const w = world({ ucm: ucmMock() });
    const x = adapterOf(pg, w);
    ok(x.data.OverrideAdapter === x.a && x.a && !x.a.native && typeof x.a.GetListSettings === 'function', 'MOD_SETTINGS: OverrideAdapter = this.mFeatureAdapter = our adapter object');
    ok(x.m.mMsA === x.a && x.data.KeyParam && x.data.KeyParam.key === UI.MS_CAT, 'adapter cached in mMsA; KeyParam.key = MOD_SETTINGS (stock code kept)');
    ok(x.data.AdapterFunctionOverrides[7] === 'GetListSettings' && x.data.AdapterFunctionOverrides[3] === 'GameInitializeSettingCategory' && x.data.AdapterFunctionOverrides[11] === 'GameShutdownSettingCategory', 'the stock function map is untouched (GetListSettings / GameInitializeSettingCategory / GameShutdownSettingCategory)');
    const d2 = x.V.method('_GetSettingsManagerData').call(x.m); ok(d2.OverrideAdapter === x.a, 'a second start (tab switch back) reuses the same adapter object');
    x.m.mSettingsCategory = 5; const d3 = x.V.method('_GetSettingsManagerData').call(x.m);
    ok(d3.OverrideAdapter === NATIVE && x.m.mFeatureAdapter === NATIVE, 'any other category: OverrideAdapter and mFeatureAdapter = the native settings adapter');
    const needs = ['AddNotifyListener', 'RemoveNotifyListener', 'GameInitializeSettingCategory', 'GameShutdownSettingCategory', 'GetIsFeatureInitialized', 'GetIsFeatureChanged', 'GetIsFeatureResetNeeded', 'GameSaveAllSettings', 'GameDiscardFeatureChanges', 'GameResetFeature', 'GameHighlightSetting', 'GetListSettings', 'GetToggleDetails', 'GameAdjustToggleSetting'];
    ok(needs.every((n) => typeof x.a[n] === 'function'), `adapter has every method FeatureManager / GenericSettings call by name (${needs.length})`);
    ok(x.a.GetIsFeatureInitialized(UI.MS_CAT) === true && x.a.GetIsFeatureChanged() === false && x.a.GetIsFeatureResetNeeded() === false && x.a.GameSaveAllSettings() === true, 'initialized true, changed / reset-needed false, save true');
  }
  // ================================================================================================ B
  console.log('\n== B: the stock FeatureManager on our adapter');
  {
    const pg = pageFor([MF('modsettings-demo')]); const w = world({ ucm: ucmMock() });
    const x = adapterOf(pg, w);
    const FK = classOf(FMAST, 'logic_com.utility.FeatureManager'); const FV = vm(FK.c, FK.data, w.G, warn);
    const fm = { dispatched: [] };
    for (const n of ['Init', '_SetupAdapters', '_OnFeatureInitialized', 'Notify', 'GetListInputs', 'ShutdownFeature', 'IsFeatureInitialized', 'IsFeatureShuttingDown', '_LockInput', '_UnlockInput', 'Shutdown', 'SetAbstractStageData']) fm[n] = FV.method(n);
    fm.GetCafe = () => ({ Assert: (m, c) => { if (!c) warn('fm Assert ' + m); }, GetInputLockManager: () => ({ LockInput: () => {}, UnlockInput: () => {} }) });
    fm.DispatchEvent = (e) => fm.dispatched.push(e.t);
    w.G.logic_com.utility.FeatureManager.prototype = {};
    // the stock FeatureManager.Shutdown ends with super.Shutdown(): r2 = the prototype chain in the engine; not modelled (warning only)
    fm.Init(x.data);
    ok(fm.mFeatureAdapter === x.a && x.a.l === fm, 'Init: our adapter is the feature adapter; AddNotifyListener stored the manager');
    ok(fm.dispatched[0] === 'init' && Array.isArray(fm.mInputList) && fm.mInputList.length === 4, `Init finished at once (GetIsFeatureInitialized = true) and listed ${fm.mInputList && fm.mInputList.length} rows through GetListSettings`);
    ok(fm.GetListInputs() === fm.mInputList, 'GetListInputs = our list');
    fm.ShutdownFeature();
    ok(fm.dispatched[1] === 'shut' && x.a.l === undefined && fm.mAdapterShuttingDown === false, 'ShutdownFeature -> GameShutdownSettingCategory -> Notify("FinishedShutdownSettingCategory") -> finished-shutdown event, RemoveNotifyListener, not shutting down any more (' + JSON.stringify([fm.dispatched, typeof x.a.l, fm.mAdapterShuttingDown]) + ')');
  }
  // ================================================================================================ C
  console.log('\n== C: rows from the manifests');
  {
    const pg = pageFor([MF('preseason-weeks'), MF('dynasty-extensions'), MF('modsettings-demo')]);
    const x = adapterOf(pg, world({ ucm: ucmMock() }));
    const L = list(x.a);
    const T = pg.T;
    ok(L.length === T.rows.length && L.length === 3 + 1 + 2 + 3, `${L.length} list items = 3 headers + 6 rows`);
    ok(L.filter((r) => r.type === 'H').map((r) => r.display).join('|') === 'Preseason Weeks|Dynasty Extensions|Mod Settings Demo (Test)', 'one header per mod, in manifest order, with the mod title');
    ok(L.every((r, k) => r.key === MAN.KEY0 + k) && L.filter((r) => r.type === 'T').map((r) => r.display).join('|') === 'Spring Game|Weekly Strategy|Press Box|Demo Switch A|Demo Level|Demo Switch B', 'toggle rows: labels in order, keys 31000+');
    ok(L.find((r) => r.display === 'Press Box').description.startsWith('On: in Scores / Schedules'), 'help text = the manifest help');
    ok(L.filter((r) => r.type === 'T').every((r) => r.isEnabled === false), 'in a dynasty with no request yet: every row disabled');
    ok(shown(x.a, keyOf(T, 'preseason-weeks', 'spring')) === 'From Preseason Wk 1' && shown(x.a, keyOf(T, 'dynasty-extensions', 'ws')) === 'After your next Advance' && x.a.GetToggleDetails(UI.MS_CAT, keyOf(T, 'modsettings-demo', 'beta')).length === 1,
      'inert rows: one item with the row\'s inert text (the stock code greys a row with one item)');
    const y = adapterOf(pg, world({ ucm: ucmMock({ noDynasty: true }) })); list(y.a);
    ok(shown(y.a, keyOf(T, 'dynasty-extensions', 'ws')) === 'Open from a dynasty' && y.a.dy === false, 'outside a dynasty (no seasonWeek): "Open from a dynasty", no request read (' + shown(y.a, keyOf(T, 'dynasty-extensions', 'ws')) + ' ' + y.a.dy + ')');
    const z = adapterOf(pg, world({})); list(z.a);
    ok(shown(z.a, keyOf(T, 'modsettings-demo', 'alpha')) === 'Open from a dynasty', 'no ultimatecareer package at all (title screen): "Open from a dynasty"');
    ok(x.a.GameAdjustToggleSetting(UI.MS_CAT, keyOf(T, 'modsettings-demo', 'alpha'), 0).length === 0 && x.a.v[keyOf(T, 'modsettings-demo', 'alpha') - MAN.KEY0] === 1, 'an inert row ignores a change');
    ok(x.a.GetToggleDetails(UI.MS_CAT, 4242).length === 0 && x.a.GameAdjustToggleSetting(UI.MS_CAT, 4242, 1).length === 0, 'an unknown key: empty details, change ignored');
  }
  // ================================================================================================ D
  console.log('\n== D: the broker round trip');
  {
    const T = MAN.load([MF('modsettings-demo')], SLOTS); const src = brokerSources(T);
    const S = save(); const c1 = runIssue(S, src);
    const [ra, rb, rg] = T.bandRows;
    ok(!c1.violations.length, `issue: no violations ${c1.violations.join('; ')}`);
    ok(recVal(S, T, 0) === -8192 + (1 << 4) + (1 << 5) && S.fu.CrossArcDataArray.length === 3, `record 0 added with the defaults: Value ${recVal(S, T, 0)} (alpha On, beta Medium, gamma Off)`);
    ok(S.list.length === 3 && S.list.every((r) => r.TypeName === LL && r.Description === LL && r.Title === LL && r.ResponseForms.length === 8 && r.RequestStyle === C.ENUM('RequestStyle.Notification') && r.IsPersistent === true && r.SeenByUser === true), '3 hidden requests: TypeName = Description = Title = "Legacy Leaderboard", 8 forms, Notification, persistent, seen');
    ok(S.list.map((r) => r.CpuResponseMinDuration).join(',') === [ra, rb, rg].map((r) => r.slot.marker).join(',') && S.list.every((r) => r.CpuResponseMaxDuration === 16777215), `markers = the slots' (${S.list.map((r) => r.CpuResponseMinDuration).join(',')})`);
    ok(S.list.map((r) => r.Priority).join(',') === '9,11,14' && S.list.every((r) => r.RequestForm === r.ResponseForms[r.Priority - [8, 10, 14][S.list.indexOf(r)]]), 'Priority = prio + value (9, 11, 14); the current form is RequestForm');
    const c2 = runIssue(S, src); ok(S.list.length === 3 && (c2.issued || 0) === 0 && S.fu.CrossArcDataArray.length === 3, 'a second run (same week, the host fires again): nothing issued, no row added');
    const u = ucmOfSave(S, src); const pg = pageFor([MF('modsettings-demo')]); const x = adapterOf(pg, world({ ucm: u }));
    const L = list(x.a);
    ok(L.filter((r) => r.type === 'T').every((r) => r.isEnabled === true), 'page: the three rows are live');
    ok(shown(x.a, ra.key) === 'On' && shown(x.a, rb.key) === 'Medium' && shown(x.a, rg.key) === 'Off', 'page shows the save\'s values (On / Medium / Off)');
    ok(x.a.GetToggleDetails(UI.MS_CAT, rb.key).map((d) => `${d.key}:${d.display}`).join(',') === '0:Low,1:Medium,2:High', 'details: one item per option, key = the option index');
    x.a.GameAdjustToggleSetting(UI.MS_CAT, rb.key, 2, 'x');
    ok(u.responded.length === 1 && u.responded[0][0] === 1001 && u.responded[0][1] === 2, 'change Demo Level -> High: ActionRequestRespond(its request, 2)');
    ok(bits(S, T, rb) === 2 && bits(S, T, ra) === 1 && bits(S, T, rg) === 0 && S.list[1].Priority === 12, `#43: bits of beta = 2, alpha / gamma kept; Priority 12 (record ${recVal(S, T, 0)})`);
    x.a.GameAdjustToggleSetting(UI.MS_CAT, ra.key, 0); x.a.GameAdjustToggleSetting(UI.MS_CAT, rg.key, 1);
    ok(bits(S, T, ra) === 0 && bits(S, T, rb) === 2 && bits(S, T, rg) === 1 && recVal(S, T, 0) === -8192 + (2 << 5) + (1 << 7), `alpha Off, gamma On: record ${recVal(S, T, 0)} = base + 64 + 128`);
    const y = adapterOf(pg, world({ ucm: u })); y.a.GameInitializeSettingCategory({}); list(y.a);
    ok(shown(y.a, ra.key) === 'Off' && shown(y.a, rb.key) === 'High' && shown(y.a, rg.key) === 'On', 'page opened again: Off / High / On (read back from Priority)');
    const cz = answer(S, src, S.list[1], 0, { lostControl: true }); ok(bits(S, T, rb) === 0 && S.list[1].Priority === 10 && cz.refresh === 1, 'after save + load (no control link): the answered form\'s index lands; Refresh called');
    answer(S, src, S.list[1], 3); u.sync(); ok(bits(S, T, rb) === 3 && S.list[1].Priority === 13, 'option 3 on a 3-option row (the 2-bit slot holds it): #43 stores it (the page never sends it; an older build with more options could)');
    y.a.GameInitializeSettingCategory({}); list(y.a); ok(shown(y.a, rb.key) === 'Medium' && y.a.i[rb.key - MAN.KEY0] === 1001, `page: value past the options shown as a valid option (${shown(y.a, rb.key)})`);
    const other = { __t: 'LostPracticeSquadPlayerRequest', CpuResponseMinDuration: 16777104, Priority: 0, ResponseForms: [{ Control: { Selection: 0 } }, { Control: { Selection: 1 } }], TargetUser: S.fu };
    const v0 = recVal(S, T, 0); answer(S, src, other, 1); ok(recVal(S, T, 0) === v0 && other.Priority === 0, 'a request outside the broker range (DE\'s Weekly Strategy marker): #43 does nothing');
    // the engine drops hidden requests at the Advance: the host re-issues each with the stored value
    S.list.length = 0; runIssue(S, src); ok(S.list.length === 3 && S.list.map((r) => r.Priority).join(',') === '8,11,15' && bits(S, T, rb) === 3, `after the drop (beta's stored 3 is past its options: issued as the default 1): re-issued with the stored values (${S.list.map((r) => r.Priority).join(',')})`);
    // the read snippet a mod uses
    const snip = GEN.readSnippet(T, 'modsettings-demo', 'gamma', 'msV');
    const rd = `.target none() 1\n.local record:FranchiseUser fu\n.local record:CrossArcData[] msA\n.local record:CrossArcData msC\n.local int msI\n.local int msN\n.local int msV\n  argRec 0\n  storeRec fu\n${snip.join('\n')}\n`;
    const rc = { self: S.fu }; const res = C.run(rd, rc); ok(res.L.msV === 1, 'readSnippet: gamma reads 1 (On)');
    const rc2 = { self: { __t: 'FranchiseUser', CrossArcDataArray: [] } }; ok(C.run(rd, rc2).L.msV === 0, 'readSnippet: no record -> the row default (gamma 0)');
    for (const host of [29, 283]) { const s2 = brokerSources(T, host); const S2 = save(); const ctx = host === 283 ? { self: { WeeklyGameProcessTransaction: { CareerFlow: S2.cf } }, list: S2.list, violations: [] } : { self: { CareerFlow: S2.cf }, list: S2.list, violations: [] };
      C.run(s2.issue, ctx); ok(S2.list.length === 3 && !ctx.violations.length, `host #${host} build: the same 3 requests`); }
  }
  // ================================================================================================ E
  console.log('\n== E: two mods share the band');
  {
    const tmp = path.join(os.tmpdir(), `ms-emu-${process.pid}-e`); fs.mkdirSync(tmp, { recursive: true });
    const sl = JSON.parse(fs.readFileSync(SLOTS, 'utf8'));
    sl.slots.push({ mod: 'demo-two', row: 'x', record: 0, bit: 8, width: 1, prio: 16 }, { mod: 'demo-two', row: 'y', record: 1, bit: 0, width: 2, prio: 18 });
    const SF = path.join(tmp, 'slots.json'); fs.writeFileSync(SF, JSON.stringify(sl));
    const M2 = path.join(tmp, 'demo-two.json'); fs.writeFileSync(M2, JSON.stringify({ schema: 1, id: 'demo-two', title: 'Demo Two', rows: [{ id: 'x', label: 'Two X', options: ['Off', 'On'], default: 1 }, { id: 'y', label: 'Two Y', options: ['A', 'B', 'C', 'D'], default: 3 }] }));
    const T = MAN.load([MF('modsettings-demo'), M2], SF); const src = brokerSources(T);
    const S = save(); runIssue(S, src);
    ok(T.records.join(',') === '0,1' && recVal(S, T, 0) === -8192 + 16 + 32 + 256 && recVal(S, T, 1) === -7680 + 3, `records 0 and 1 with both mods' defaults (${recVal(S, T, 0)}, ${recVal(S, T, 1)})`);
    ok(S.list.length === 5 && new Set(S.list.map((r) => r.CpuResponseMinDuration)).size === 5, '5 requests, 5 distinct markers');
    const u = ucmOfSave(S, src); const pg = pageFor([MF('modsettings-demo'), M2], { slots: SF }); const x = adapterOf(pg, world({ ucm: u })); list(x.a);
    const R = (m, id) => T.rows.find((r) => r.mod === m && r.id === id);
    ok(shown(x.a, R('demo-two', 'x').key) === 'On' && shown(x.a, R('demo-two', 'y').key) === 'D' && shown(x.a, R('modsettings-demo', 'beta').key) === 'Medium', 'each row reads its own request');
    const snap = () => T.bandRows.map((r) => bits(S, T, r)).join(',');
    const s0 = snap(); x.a.GameAdjustToggleSetting(UI.MS_CAT, R('demo-two', 'x').key, 0);
    ok(snap() === s0.split(',').map((v, k) => (T.bandRows[k].mod === 'demo-two' && T.bandRows[k].id === 'x' ? '0' : v)).join(','), `demo-two x Off: only its bit moved (${s0} -> ${snap()})`);
    const s1 = snap(); x.a.GameAdjustToggleSetting(UI.MS_CAT, R('modsettings-demo', 'alpha').key, 0);
    ok(snap() === s1.split(',').map((v, k) => (T.bandRows[k].id === 'alpha' ? '0' : v)).join(','), `demo alpha Off: only its bit moved (${snap()})`);
    x.a.GameAdjustToggleSetting(UI.MS_CAT, R('demo-two', 'y').key, 1); ok(bits(S, T, R('demo-two', 'y')) === 1 && recVal(S, T, 1) === -7680 + 1, 'demo-two y on record 1 = B; record 0 untouched by it');
    let bad = null; try { const s3 = JSON.parse(JSON.stringify(sl)); s3.slots.push({ mod: 'clash', row: 'z', record: 0, bit: 4, width: 1, prio: 30 }); fs.writeFileSync(SF, JSON.stringify(s3)); MAN.loadSlots(SF).problems.length && (bad = 'slots'); } catch (e) { bad = e.message; }
    ok(bad === 'slots', 'a slot list that gives one bit to two rows is REFUSED');
    try { const s4 = JSON.parse(JSON.stringify(sl)); s4.slots.push({ mod: 'clash', row: 'z', record: 1, bit: 5, width: 1, prio: 9 }); fs.writeFileSync(SF, JSON.stringify(s4)); MAN.load([MF('modsettings-demo')], SF); bad = 'built'; } catch (e) { bad = /prio ranges overlap/.test(e.message) ? 'prio' : e.message; }
    ok(bad === 'prio', 'overlapping priority ranges are REFUSED');
  }
  // ================================================================================================ F
  console.log('\n== F: Dynasty Extensions\' own requests');
  {
    const pg = pageFor([MF('preseason-weeks'), MF('dynasty-extensions')]); const T = pg.T;
    const reqs = [{ id: 10, typeName: 'Depth Chart', description: LL, priority: 0, nResp: 2 }, { id: 11, typeName: 'Players of the Week', description: LL, priority: 1, nResp: 2 },
      { id: 12, typeName: 'Start of Preseason', description: LL, priority: 2, nResp: 4 }, { id: 13, typeName: LL, description: LL, priority: 1, nResp: 8 },
      { id: 9, typeName: 'Depth Chart', description: LL, priority: 1, nResp: 2 }, { id: 14, typeName: 'PreSeason Game Prep', description: LL, priority: 251, nResp: 7 },
      { id: 15, typeName: 'Depth Chart', description: 'Something else', priority: 1, nResp: 2 }];
    const u = ucmMock({ requests: reqs }); const x = adapterOf(pg, world({ ucm: u })); list(x.a);
    const k = (id) => keyOf(T, id === 'spring' ? 'preseason-weeks' : 'dynasty-extensions', id);
    ok(shown(x.a, k('ws')) === 'On' && x.a.i[k('ws') - MAN.KEY0] === 10, 'Weekly Strategy: newest request (10, Priority 0 = On) -> On (an older 9 and a non-hidden 15 ignored)');
    ok(shown(x.a, k('pb')) === 'Off', 'Press Box: Priority 1 -> Off (the 1 - Priority mapping, Off = 0 / On = 1 in the UI)');
    ok(shown(x.a, k('spring')) === 'Game + Practice' && x.a.i[k('spring') - MAN.KEY0] === 12, 'Spring Game: 4-form request -> Game + Practice (a broker request with 8 forms and Press Box\'s 7-form request ignored)');
    x.a.GameAdjustToggleSetting(UI.MS_CAT, k('ws'), 0); x.a.GameAdjustToggleSetting(UI.MS_CAT, k('pb'), 1); x.a.GameAdjustToggleSetting(UI.MS_CAT, k('spring'), 0);
    ok(JSON.stringify(u.responded) === '[[10,1],[11,0],[12,0]]', `answers in DE's forms: WS Off -> form 1, PB On -> form 0, Spring Off -> form 0 (${JSON.stringify(u.responded)})`);
    const DE = require(path.join(ROOT, 'dynasty-extensions', 'src', 'ui-build-de.js'));
    ok([DE.ROW_WS, DE.ROW_PB].every((r) => { const row = T.rows.find((x2) => x2.id === r.id); return row && row.request.typeName === r.typeName && row.label === r.label && row.help === r.desc && row.inert === r.inert; }) && T.rows.find((r) => r.id === 'spring').help === DE.ROW_PW.desc,
      'manifest rows == DE\'s League Settings rows (label, TypeName, help, inert text)');
    const y = adapterOf(pg, world({ ucm: ucmMock({ requests: [{ id: 30, typeName: LL, description: LL, priority: 0, nResp: 2 }] }) })); list(y.a);
    ok(shown(y.a, k('ws')) === 'After your next Advance', 'a 2-form request with the broker TypeName is never a DE row');
  }
  // ================================================================================================ G
  console.log('\n== G: the FranTk request list (Playoff Customizer)');
  {
    const pg = pageFor([MF('preseason-weeks'), MF('dynasty-extensions')]); const T = pg.T;
    const reqs = [{ id: 10, typeName: 'Depth Chart', description: LL, priority: 1, nResp: 2 }, { id: 12, typeName: 'Start of Preseason', description: LL, priority: 1, nResp: 4 }];
    const calls = []; const fk = { InitializeRequestList: () => calls.push('init'), GetRequestList: () => { calls.push('get'); return reqs.map((r) => ({ RequestId: r.id, TypeName: r.typeName, Description: r.description })); }, ShutdownRequestList: () => calls.push('shut') };
    const x = adapterOf(pg, world({ ucm: ucmMock({ requests: reqs, hideNative: true }), fk })); list(x.a);
    ok(shown(x.a, keyOf(T, 'dynasty-extensions', 'ws')) === 'Off' && shown(x.a, keyOf(T, 'preseason-weeks', 'spring')) === 'Game Only', 'native list empty -> rows read from the FranTk list (WS Off, Spring Game Only)');
    ok(calls.join(',') === 'init,get,shut', `FranTk protocol: ${calls.join(',')}`);
    const calls2 = []; const fk2 = { InitializeRequestList: () => calls2.push('init'), GetRequestList: () => [], ShutdownRequestList: () => calls2.push('shut') };
    const y = adapterOf(pg, world({ ucm: ucmMock({ requests: reqs }), fk: fk2 })); list(y.a); ok(calls2.length === 0, 'native list has them -> the FranTk list is never touched');
  }
  // ================================================================================================ H
  console.log('\n== H: the Settings hub tab');
  for (const startOn of ['AUD', UI.MS_CAT]) {
    const pg = pageFor([MF('modsettings-demo')]); const w = world({ ucm: ucmMock() });
    const V = vm(pg.k.c, pg.k.data, w.G, warn);
    let handed = null; const mgr = { RemoveEventListener: () => {}, Shutdown: () => {}, AddEventListener: () => {}, Init: (d) => { mgr.data = d; } };
    const tabs = [{ key: 'BTN_GAME', display: 'GAME OPTIONS', icon: 'g', isEnabled: true }, { key: 'BTN_AUD', display: 'ACCESSIBILITY', icon: 'a', isEnabled: true }];
    const m = { mSettingsCategory: 'HUB', mFeatureAdapter: { GetListSettings: () => tabs }, mSettingsManager: mgr,
      mTabListProvider: { ClearAllItems: () => {}, AddItemsByItemArray: (a) => { handed = a; }, GetDefaultIndex: () => handed.findIndex((t) => t.isDefault), GetItemData: (i) => handed[i] },
      _GetSettingCategory: () => startOn, _GetSettingCategoryKey: (k) => k.replace('BTN_', ''), _SettingsManagerSettingsHubInitFinish: null, _SettingsManagerInitFinish: () => {} };
    m._GetSettingsManagerData = V.method('_GetSettingsManagerData');
    V.method('_SettingsManagerSettingsHubInitFinish').call(m, {});
    ok(handed && handed.length === 3 && handed[2].key === UI.MS_CAT && handed[2].display === 'Mod Settings' && handed[2].icon === 'a' && handed[2].isEnabled === true, `tab bar: the native tabs + "Mod Settings" last, a copy of the last native tab (start ${startOn})`);
    ok(handed[0].key === 'GAME' && handed[1].key === 'AUD', 'native tabs still get their category keys (stock loop runs first)');
    ok(handed[2].isDefault === (startOn === UI.MS_CAT) && m.mSettingsCategory === startOn && (startOn === UI.MS_CAT ? mgr.data.OverrideAdapter === m.mMsA : mgr.data.OverrideAdapter === NATIVE), `isDefault ${handed[2].isDefault}; the hub starts on ${m.mSettingsCategory} with the ${startOn === UI.MS_CAT ? 'Mod Settings' : 'native'} adapter`);
  }
  // ================================================================================================ I
  console.log('\n== I: probe pages');
  for (const [total, mods] of [[40, 3], [120, 6]]) {
    const files = BUILD.probeManifests(total, mods); const pg = pageFor(files); const x = adapterOf(pg, world({}));
    const L = list(x.a);
    ok(L.length === total + mods && L.filter((r) => r.type === 'H').length === mods && L.filter((r) => r.type === 'T').every((r) => r.isEnabled), `${total}: ${L.length} items (${mods} headers), every row live at the title screen (memory rows)`);
    const r = pg.T.rows.filter((q) => q.kind === 'toggle')[total - 1];
    const nv = (r.default + 1) % r.options.length; x.a.GameAdjustToggleSetting(UI.MS_CAT, r.key, nv);
    ok(shown(x.a, r.key) === r.options[nv], `${total}: last row changed to ${r.options[nv]} and shows it`);
    x.a.GameInitializeSettingCategory({}); list(x.a); ok(shown(x.a, r.key) === r.options[nv], `${total}: still ${r.options[nv]} after a tab switch away and back (same screen)`);
    ok(pg.p.functions === 19, `${total}: ${pg.p.functions} functions (constant, not per row)`);
  }
  // ================================================================================================ J
  console.log('\n== J: the record table');
  {
    const T = MAN.load([MF('modsettings-demo')], SLOTS); const src = brokerSources(T);
    const S = save({ rows: Array.from({ length: 149 }, (_, k) => 30000 + k) }); const c = runIssue(S, src);
    ok(S.fu.CrossArcDataArray.length === 149 && S.list.length === 0 && !c.violations.length, '149 rows: no record added, no request issued (rows stay inert), no violation');
    const S2 = save({ rows: Array.from({ length: 148 }, (_, k) => 30000 + k) }); runIssue(S2, src); ok(S2.fu.CrossArcDataArray.length === 149 && S2.list.length === 3, '148 rows: the record is added (149) and the requests issued');
    ok(T.records.length === 1, `the first users need ${T.records.length} record (demo; DE / PW slots reserved in the same record)`);
    if (!process.argv.includes('--no-budget')) {
      const rb = cp.spawnSync(process.execPath, ['--max-old-space-size=8192', path.join(ROOT, 'dynasty-extensions', 'tools', 'row-budget.js')], { cwd: path.join(ROOT, 'dynasty-extensions'), encoding: 'utf8', maxBuffer: 1 << 26 });
      const m = /pc 64 teams: .* = (\d+) \/ 150; guard rows (\d+) \/ 118/.exec(rb.stdout || '');
      ok(!!m, 'row-budget.js ran (Playoff Customizer 1.3.0 at 64 teams + Dynasty Extensions)');
      if (m) { const used = Number(m[1]), guard = Number(m[2]);
        ok(used + 1 <= 150 && used + 1 < 151 && guard + 1 <= 118, `PC 1.3.0 at 64 teams + DE + Mod Settings record 0: ${used + 1} / 150 rows (bracket build needs < 151), guard ${guard + 1} / 118`);
        ok(used + 2 <= 150 && guard + 2 <= 118, `with record 1 too: ${used + 2} / 150, guard ${guard + 2} / 118 (2 records = the ceiling there; margin ${150 - used - 2})`); }
    }
  }
  // ================================================================================================ K
  // the modsettings-p2r crash (2026-11-06): the stock #43 body is self.PlayerSigningEval.#85(self) with no null check; our requests have no
  // PlayerSigningEval. The emulator returns null for a field of null, so the guard is checked statically and the stock call is counted.
  console.log('\n== K: none()#43 and the stock call (PlayerSigningEval null guard)');
  {
    const T = MAN.load([MF('modsettings-demo')], SLOTS); const src = brokerSources(T);   // host 46 issue (the emulator's save shape); #43 is host-independent
    ok(/^\.mode replace$/m.test(src.h43) && GEN.unguardedStockCalls(src.h43) === 0 && (src.h43.match(/getRef #85/g) || []).length === 1, 'default ms-h43 = REPLACE with exactly one stock call, behind the null guard');
    const oldShape = ['.tok 9 2 0 0', ...GEN.STOCK_CALL.slice(1)].join('\n') + '\n' + GEN.h43Source(T, { h43: 'append' });
    ok(GEN.unguardedStockCalls(oldShape) === 1, 'the checker flags the crashing shape (stock body + the old APPEND) as 1 unguarded call');
    ok(GEN.unguardedStockCalls(GEN.h43Source(T, { h43: 'append' })) === 0 && !/getRef #85/.test(GEN.h43Source(T, { h43: 'append' })), 'APPEND source carries no stock call (only for bases that guard it; build.js checks the built body)');
    // our request: value lands, the stock call is never reached
    const S = save(); runIssue(S, src); const r0 = S.list[0];
    const c1 = answer(S, src, r0, 0);
    ok(!c1.stock && !c1.violations.length && c1.refresh === 1 && r0.Priority === T.bandRows[0].slot.prio, 'broker request (no PlayerSigningEval): answered, Refresh, no stock call, no violation');
    // a stock lost-practice-squad request (PlayerSigningEval set, marker 0): the stock call runs exactly once, nothing of ours
    const pse = { __t: 'PlayerSigningEval', '#85': { m: 'StockHandle' } };
    const stockReq = { __t: 'LostPracticeSquadPlayerRequest', CpuResponseMinDuration: 0, '#37': pse, Response: null };
    const arcs0 = JSON.stringify(S.fu.CrossArcDataArray);
    const c2 = { self: stockReq, list: S.list, violations: [] }; C.run(src.h43, c2);
    ok(c2.stock === 1 && !c2.violations.length && JSON.stringify(S.fu.CrossArcDataArray) === arcs0, 'stock request (PlayerSigningEval set): the stock call runs once, band untouched');
    // a foreign hidden request (another mod's, no PlayerSigningEval, marker outside our range): nothing at all
    const c3 = { self: { __t: 'LostPracticeSquadPlayerRequest', CpuResponseMinDuration: 16777104 }, list: S.list, violations: [] }; C.run(src.h43, c3);
    ok(!c3.stock && !c3.violations.length && JSON.stringify(S.fu.CrossArcDataArray) === arcs0, 'foreign hidden request (no PlayerSigningEval): no stock call, nothing written');
  }
  // ================================================================================================
  const real = warnings.filter((w) => !/super|Shutdown|prototype/.test(w));
  ok(real.length === 0, `no interpreter warnings (${real.slice(0, 5).join(' | ')})`);
  try { fs.rmSync(SRC, { recursive: true, force: true }); } catch (e) { /* temp */ }
  ok(AA.runs > 0 && AA.exposed === 0, `Stage.End auto-answer: after ${AA.runs} broker issue runs, ${AA.exposed} broker requests would reach the stock #43 with the mod removed (Option B: IsResponseRequired, no DefaultResponse)${AA.who.length ? ' -- ' + AA.who.join(' / ') : ''}`);
  { // control: the pre-Option B generator (src/gen-ms.js.bak-2026-11-10, compiled in memory) on the same manifests leaves every broker request exposed (the check bites)
    const Mod = require('module'); const fn = path.join(REPO, 'src', 'gen-ms-pre-optb.js'); const m = new Mod(fn, module); m.filename = fn; m.paths = Mod._nodeModulePaths(path.dirname(fn));
    m._compile(fs.readFileSync(path.join(REPO, 'src', 'gen-ms.js.bak-2026-11-10'), 'utf8'), fn); const OLDG = m.exports;
    const files = fs.readdirSync(path.join(REPO, 'manifests')).filter((x) => x.endsWith('.json')).map((x) => path.join(REPO, 'manifests', x)); const Tm = MAN.load(files, SLOTS);
    const So = save(), Sn = save(); C.run(OLDG.issueSource(Tm), { self: { CareerFlow: So.cf }, list: So.list, violations: [] }); C.run(GEN.issueSource(Tm), { self: { CareerFlow: Sn.cf }, list: Sn.list, violations: [] });
    const nb = Tm.bandRows.length; ok(nb > 0 && aaExposed(So).length === nb && aaExposed(Sn).length === 0 && Sn.list.filter(msOurs).length === nb, `Stage.End check control: the pre-Option B issue leaves ${aaExposed(So).length} of ${nb} broker requests exposed, the Option B issue ${aaExposed(Sn).length} of ${Sn.list.filter(msOurs).length}`); }
  console.log(`\n${checks - fails}/${checks} ${fails ? 'FAILED' : 'PASS'}`);
  process.exit(fails ? 1 : 0);
})().catch((e) => { console.error(e); process.exit(2); });
