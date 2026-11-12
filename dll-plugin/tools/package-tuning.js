#!/usr/bin/env node
// package-tuning.js: the Mod Menu TUNING test package -> exports/build-mods/modmenu-tuning-test/ (wiped and rebuilt) + BUILD-INFO.txt.
// Takes dll/out as the last full `node dll/build.js` left it (its build.log must end in a full pass), the loader from plugin/out, and the lab
// manifest samples-lab/tuning-lab.json. No .fbmod: the lab changes the rows through settings.json (hot-read), not through the tab.
// Never writes a Mod Manager, game or saves folder; never publishes.
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process'), crypto = require('crypto');
const REPO = path.join(__dirname, '..'), ROOT = path.join(REPO, '..');
const OUT = path.join(ROOT, 'exports', 'build-mods', 'modmenu-tuning-test');
const die = (m) => { console.log('package-tuning: ' + m + ': nothing packaged'); process.exit(1); };
const h = (alg, b) => crypto.createHash(alg).update(b).digest('hex');
const run = (cmd, args, cwd) => { const r = cp.spawnSync(cmd, args, { cwd, encoding: 'utf8' }); return (r.stdout || '').trim(); };
const blog = fs.existsSync(path.join(REPO, 'dll', 'out', 'build.log')) ? fs.readFileSync(path.join(REPO, 'dll', 'out', 'build.log'), 'utf8') : '';
if (!/build \+ every test passed\s*$/.test(blog)) die('dll/out/build.log does not end in a full pass (run node dll/build.js)');
const plug = path.join(REPO, 'plugin', 'out', 'Plugins'); if (!fs.existsSync(path.join(plug, 'ModSettingsPlugin.dll'))) die('plugin/out/Plugins/ModSettingsPlugin.dll missing (plugin/build-plugin.ps1)');
fs.rmSync(OUT, { recursive: true, force: true });
const cp1 = (from, to) => { fs.mkdirSync(path.dirname(path.join(OUT, to)), { recursive: true }); fs.copyFileSync(from, path.join(OUT, to)); };
cp1(path.join(plug, 'ModSettingsPlugin.dll'), 'Plugins/ModSettingsPlugin.dll');
cp1(path.join(plug, 'ModSettings', 'plugin.cfg'), 'Plugins/ModSettings/plugin.cfg');
cp1(path.join(REPO, 'dll', 'out', 'modsettings.dll'), 'Plugins/ModSettings/modsettings.dll');
// the ini: tuning on, the tab feed off (no tab .fbmod in this package: nothing to answer)
const ini = fs.readFileSync(path.join(REPO, 'dll', 'modsettings.ini'), 'utf8').replace(/^tab\s*=.*$/m, 'tab = 0');
fs.mkdirSync(path.join(OUT, 'Plugins', 'ModSettings'), { recursive: true });
if (!/^tab = 0/m.test(ini) || !/^tuning = 1/m.test(ini)) die('dll/modsettings.ini has no "tab =" / "tuning = 1" line');
fs.writeFileSync(path.join(OUT, 'Plugins', 'ModSettings', 'modsettings.ini'), ini);
cp1(path.join(REPO, 'samples-lab', 'tuning-lab.json'), 'Plugins/ModSettings/mods/tuning-lab.json');
const files = []; (function walk(d) { for (const f of fs.readdirSync(d)) { const p = path.join(d, f); if (fs.statSync(p).isDirectory()) walk(p); else files.push(p); } })(OUT);
const nchecks = (blog.match(/(\d+) checks ok in all/) || [])[1];
let txt = `Mod Menu TUNING (modmenu-tuning-test) - TEST BUILD, not a release (${new Date().toISOString()}). Category Test. Never a live folder; never publish.
What it is: modsettings.dll with the tuning writer (spec v3 bind "tuning": writes one value of the game's loaded AttribSys tuning data, in memory
only) + the Mod Manager loader + one lab manifest, tuning-lab.json: fumble = ballrules.Fumble_ChanceMultiplier (both copies; read live: applies
now), wind = gameplay_tuning.wind_base_value[0] (both copies; read when a game's weather is set up: next game). tab = 0 (no tab .fbmod here).
Change a row: write Plugins\\ModSettings\\settings.json { "schema": 3, "global": { "tuning-lab": { "fumble": 1 } } } (hot-read within ~1 s);
"as-installed" puts the loaded value back. Every write is a "tuning: WRITE" line in modsettings.log; the status file's "tuning" block shows each
field's loaded and current value.
Docs: docs/design.md (section "Tuning binding, 11-11": T1 / T2), docs/manifest-spec.md 5.1.
Lab: testing/queue/modmenu-tuning-1.json (stage with MODMENU_PKG=modmenu-tuning-test MODMENU_TEST=modmenu-tuning-1 node testing/runner/stage-modmenu.js stage).
Source: modsettings-dll/ branch ${run('git', ['-C', REPO, 'rev-parse', '--abbrev-ref', 'HEAD'], REPO)} @ ${run('git', ['-C', REPO, 'log', '-1', '--format=%h %s'], REPO).slice(0, 120)}${run('git', ['-C', REPO, 'status', '--porcelain', '--untracked-files=no'], REPO) ? ' (+ uncommitted changes)' : ''}.
Tests (offline): dll/build.js ${nchecks} self-test checks, incl. 'tuning' on both dumps (49 each: the dump's own reflection tables, a fake registry
and instances: found / not available / clamps / two mods on one field / instance replaced / something else writing / a no-access page).
Game builds: TU 10-13 (PE 1791226072), TU 10-29 (PE 1792187070). Anything else: tuning rows are kept and never written.

Files (md5, sha256, bytes):
`;
for (const f of files.sort()) { const b = fs.readFileSync(f); txt += `  ${path.relative(OUT, f).replace(/\//g, '\\')}  md5 ${h('md5', b)}  sha256 ${h('sha256', b)}  ${b.length}\n`; }
fs.writeFileSync(path.join(OUT, 'BUILD-INFO.txt'), txt.replace(/\n/g, '\r\n'));
console.log(txt);
