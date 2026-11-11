#!/usr/bin/env node
// package-tab.js: the Mod Menu TAB test package -> exports/build-mods/modmenu-tab-test/ (Plugins\ wiped and rebuilt; the .fbmod kept) + BUILD-INFO.txt.
// Takes dll/out as the last full `node dll/build.js` left it (its build.log must end in a full pass), the loader from plugin/out, the two spec v3
// samples (renamed <modid>.json), modsettings/slots.json, and the tab .fbmod that `node modsettings/build.js --tab` already put in the folder
// (its md5 must equal modsettings/build/build-record-tab.json). Never writes a Mod Manager, game or saves folder; never publishes.
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process'), crypto = require('crypto');
const REPO = path.join(__dirname, '..'), ROOT = path.join(REPO, '..');
// --out <folder name under exports/build-mods> (default modmenu-tab-test); a folder a lab test is about to use is never rebuilt by hand: pick a new name
const OUTNAME = process.argv.includes('--out') ? process.argv[process.argv.indexOf('--out') + 1] : 'modmenu-tab-test';
if (!/^modmenu-tab-test[-a-z0-9]*$/.test(OUTNAME)) { console.log('package-tab: --out must be modmenu-tab-test or modmenu-tab-test-<suffix>'); process.exit(2); }
const OUT = path.join(ROOT, 'exports', 'build-mods', OUTNAME);
const run = (cmd, args, cwd) => { const r = cp.spawnSync(cmd, args, { cwd, encoding: 'utf8', maxBuffer: 1 << 28 }); return { code: r.status, out: (r.stdout || '') + (r.stderr || '') }; };
const die = (m) => { console.log('package-tab: ' + m + ': nothing packaged'); process.exit(1); };
const h = (alg, b) => crypto.createHash(alg).update(b).digest('hex');
const BLOG = path.join(REPO, 'dll', 'out', 'build.log');
const blog = fs.existsSync(BLOG) ? fs.readFileSync(BLOG, 'utf8') : '';
if (!/build \+ every test passed\s*$/.test(blog)) die('dll/out/build.log does not end in a full pass (run node dll/build.js)');
const TEST = path.join(ROOT, 'modsettings', 'test', 'tab-emu.js');
const te = run('node', [TEST], ROOT); if (te.code !== 0) { console.log(te.out); die('modsettings/test/tab-emu.js failed'); }
const teLine = te.out.trim().split(/\r?\n/).slice(-1)[0];
const ct = run('node', [path.join('test', 'companion-test.js')], REPO); if (ct.code !== 0) { console.log(ct.out); die('the companion test failed'); }
const plug = path.join(REPO, 'plugin', 'out', 'Plugins'); if (!fs.existsSync(path.join(plug, 'ModSettingsPlugin.dll'))) die('plugin/out/Plugins/ModSettingsPlugin.dll missing (plugin/build-plugin.ps1)');
const recF = path.join(ROOT, 'modsettings', 'build', 'build-record-tab.json'); if (!fs.existsSync(recF)) die('no modsettings/build/build-record-tab.json (node modsettings/build.js --tab)');
const rec = JSON.parse(fs.readFileSync(recF, 'utf8')); const FB = path.join(OUT, 'Mod Settings Tab (Test).fbmod');
if (!fs.existsSync(FB) || h('md5', fs.readFileSync(FB)) !== rec.md5) die('the tab .fbmod in ' + OUT + ' is missing or not the recorded build');
fs.rmSync(path.join(OUT, 'Plugins'), { recursive: true, force: true });
const cp1 = (from, to) => { fs.mkdirSync(path.dirname(path.join(OUT, to)), { recursive: true }); fs.copyFileSync(from, path.join(OUT, to)); };
cp1(path.join(plug, 'ModSettingsPlugin.dll'), 'Plugins/ModSettingsPlugin.dll');
cp1(path.join(plug, 'ModSettings', 'plugin.cfg'), 'Plugins/ModSettings/plugin.cfg');
cp1(path.join(REPO, 'dll', 'out', 'modsettings.dll'), 'Plugins/ModSettings/modsettings.dll');
console.log('package-tab: -> ' + OUT);
cp1(path.join(REPO, 'dll', 'modsettings.ini'), 'Plugins/ModSettings/modsettings.ini');
cp1(path.join(ROOT, 'modsettings', 'slots.json'), 'Plugins/ModSettings/slots.json');
const SM = path.join(REPO, 'samples'); const samples = fs.readdirSync(SM).filter((f) => f.endsWith('.json'));
for (const f of samples) { const j = JSON.parse(fs.readFileSync(path.join(SM, f), 'utf8')); cp1(path.join(SM, f), `Plugins/ModSettings/mods/${j.modid}.json`); }
const git = (d) => run('git', ['-C', d, 'log', '-1', '--format=%h %s'], d).out.trim() + (run('git', ['-C', d, 'status', '--porcelain', '--untracked-files=no'], d).out.trim() ? ' (+ uncommitted changes)' : '');
const branch = (d) => run('git', ['-C', d, 'rev-parse', '--abbrev-ref', 'HEAD'], d).out.trim();
const files = []; (function walk(d) { for (const f of fs.readdirSync(d)) { const p = path.join(d, f); if (fs.statSync(p).isDirectory()) walk(p); else if (f !== 'BUILD-INFO.txt') files.push(p); } })(OUT);
const nchecks = (blog.match(/(\d+) checks ok in all/) || [])[1], cchecks = (ct.out.match(/companion: (\d+) ok/) || [])[1];
let txt = `Mod Menu TAB (${OUTNAME}) - TEST BUILD, not a release (${new Date().toISOString()}). Category Test. Never a live folder; never publish.
What it is: the in-game Settings > "Mod Settings" tab (the .fbmod: a GenericSettings copy whose tab asks the game's settings adapter for
category ${rec.feedCategory}) + modsettings.dll 0.2.0-tab1 (wraps the settings adapter's slots 0x30 / 0x38 / 0x40 / 0x50 / 0x58 for that category
only and answers them from the manifests in Plugins\\ModSettings\\mods: one header per mod, then its rows; a change -> settings.json).
${OUTNAME === 'modmenu-tab-test' ? '' : 'Follow-up build (not the one lab modmenu-tab-1 tests): + spec v3 "Names, duplicates and clashes", "Adding rows to another mod\'s section", "Safety and behaviour rules".\n'}
Docs: docs/design.md section 12 ("Layer 2 tab feed, 11-11"), docs/manifest-spec.md.
Source: dll-plugin/ branch ${branch(REPO)} @ ${git(REPO)}; modsettings/ branch ${branch(path.join(ROOT, 'modsettings'))} @ ${git(path.join(ROOT, 'modsettings'))}.
Tests (offline): dll/build.js ${nchecks} self-test checks (incl. 'tab': 55 checks against fake holders / Variants; both dumps: the settings
adapter pins, the five swaps, every refusal) + the load test + the samples load; modsettings/test/tab-emu.js ${teLine}; companion-test.js ${cchecks} ok.
Game builds: TU 10-13 (PE 1791226072), TU 10-29 (PE 1792187070). Anything else: nothing is swapped and the tab shows "plugin is not running".

Layout (the lab copies Plugins\\ into the Mod Manager folder with its stage script; agents never stage):
  Mod Settings Tab (Test).fbmod             the tab (UI file common/ui/fb_node_com/scriptpod.ast only; no save data, no expression file)
  Plugins\\ModSettingsPlugin.dll            the Mod Manager loader (layer 1's, unchanged; loads modsettings.dll at Launch, settle 30 s)
  Plugins\\ModSettings\\modsettings.dll      the DLL (exports ms_get only; KERNEL32 + msvcrt)
  Plugins\\ModSettings\\modsettings.ini      tab = 1, slider_native = 0 (+ layer 1's keys)
  Plugins\\ModSettings\\slots.json           the shared band slot list (dynasty rows find their bits here)
  Plugins\\ModSettings\\mods\\auto-kick.json, pocket-presence.json   the two spec v3 samples (api rows; tuning rows are stored, not applied yet)
  written in game: Plugins\\ModSettings\\settings.json (the choices), modsettings-status.json ("tab" block = the rows as drawn), modsettings.log

Files (md5, sha256, bytes):
`;
for (const f of files.sort()) { const b = fs.readFileSync(f); txt += `  ${path.relative(OUT, f).replace(/\//g, '\\')}  md5 ${h('md5', b)}  sha256 ${h('sha256', b)}  ${b.length}\n`; }
fs.writeFileSync(path.join(OUT, 'BUILD-INFO.txt'), txt.replace(/\n/g, '\r\n'));
console.log(txt);
