#!/usr/bin/env node
// package.js: the layer 1 TEST package -> exports/build-mods/modsettings-dll-l1/ (wiped first) with BUILD.txt (md5 + sha256 of every file).
// Runs the DLL build + every test ONCE (dll/build.js), the companion tests, and takes the plugin from plugin/out (build it first with
// plugin/build-plugin.ps1). Never writes a Mod Manager, game or saves folder.
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process'), crypto = require('crypto');
const REPO = path.join(__dirname, '..'), ROOT = path.join(REPO, '..'), OUT = path.join(ROOT, 'exports', 'build-mods', 'modsettings-dll-l1');
const run = (cmd, args, cwd) => { const r = cp.spawnSync(cmd, args, { cwd, encoding: 'utf8', maxBuffer: 1 << 28 }); return { code: r.status, out: (r.stdout || '') + (r.stderr || '') }; };
const say = (s) => console.log(s);
// --reuse-dll: take dll/out as the last full build left it (its build.log must end in a full pass), instead of building + testing again
const REUSE = process.argv.includes('--reuse-dll'); const BLOG = path.join(REPO, 'dll', 'out', 'build.log');
let r = REUSE ? { code: fs.existsSync(BLOG) && /build \+ every test passed\s*$/.test(fs.readFileSync(BLOG, 'utf8')) ? 0 : 1, out: fs.existsSync(BLOG) ? fs.readFileSync(BLOG, 'utf8') : '' } : run('node', ['build.js'], path.join(REPO, 'dll'));
const blog = r.out; say(blog.split(/\r?\n/).filter((l) => /checks ok|load test|md5|build \+|BUILD|FAIL/.test(l)).join('\n'));
if (r.code !== 0) { say('package: the DLL build or a test failed: nothing packaged'); process.exit(1); }
r = run('node', [path.join('test', 'companion-test.js')], REPO); const ctest = r.out; say(ctest.trim().split(/\r?\n/).slice(-1)[0]);
if (r.code !== 0) { say(ctest); say('package: a companion test failed: nothing packaged'); process.exit(1); }
const plug = path.join(REPO, 'plugin', 'out', 'Plugins');
if (!fs.existsSync(path.join(plug, 'ModSettingsPlugin.dll'))) { say('package: plugin/out/Plugins/ModSettingsPlugin.dll missing: run plugin/build-plugin.ps1 first'); process.exit(1); }
fs.rmSync(OUT, { recursive: true, force: true });
const cp1 = (from, to) => { fs.mkdirSync(path.dirname(path.join(OUT, to)), { recursive: true }); fs.copyFileSync(from, path.join(OUT, to)); };
cp1(path.join(plug, 'ModSettingsPlugin.dll'), 'Plugins/ModSettingsPlugin.dll');
cp1(path.join(plug, 'ModSettings', 'plugin.cfg'), 'Plugins/ModSettings/plugin.cfg');
cp1(path.join(plug, 'ModSettings', 'README.txt'), 'Plugins/ModSettings/README.txt');
cp1(path.join(REPO, 'dll', 'out', 'modsettings.dll'), 'Plugins/ModSettings/modsettings.dll');
cp1(path.join(REPO, 'dll', 'modsettings.ini'), 'Plugins/ModSettings/modsettings.ini');
cp1(path.join(REPO, 'examples', 'mods', 'modsettings-demo.json'), 'Plugins/ModSettings/mods/modsettings-demo.json');
cp1(path.join(REPO, 'companion', 'mod-settings.html'), 'Plugins/ModSettings/mod-settings.html');
cp1(path.join(REPO, 'companion', 'mod-settings.html'), 'companion/mod-settings.html');
for (const t of ['ms-core.js', 'ms-set.js', 'ms-check.js']) cp1(path.join(REPO, 'tools', t), 'tools/' + t);
cp1(path.join(REPO, 'README.md'), 'README.md');
const git = run('git', ['-C', REPO, 'log', '-1', '--format=%h %s'], REPO).out.trim();
const dirty = run('git', ['-C', REPO, 'status', '--porcelain'], REPO).out.trim();
const files = []; (function walk(d) { for (const f of fs.readdirSync(d)) { const p = path.join(d, f); if (fs.statSync(p).isDirectory()) walk(p); else files.push(p); } })(OUT);
const h = (alg, b) => crypto.createHash(alg).update(b).digest('hex');
const nchecks = (blog.match(/(\d+) checks ok in all/) || [])[1], cchecks = (ctest.match(/companion: (\d+) ok/) || [])[1];
let txt = `Mod Settings (DLL-lite) layer 1 - TEST BUILD, not a release (${new Date().toISOString()})
Source: dll-plugin/ (git ${git || 'no commit'}${dirty ? ', with uncommitted changes' : ''}). Docs: docs/design.md (section 10 = O1 / O2,
section 11 = this build), docs/manifest-spec.md.
Tests: dll/build.js ${nchecks} self-test checks (json, config, ini, records, guard, patchsafe, both dumps' pins + install + every broken pin) + the load
test; test/companion-test.js ${cchecks} checks (the page rules, parity with the DLL on the same folder, ms-set.js, ms-check.js vs slots.json).
Game builds: TU 10-13 (PE 1791226072), TU 10-29 (PE 1792187070). Anything else: the DLL installs nothing (values.json + ini bindings still work).

Layout (copy Plugins\\ into the Mod Manager folder ONLY through the lab's staging; agents never stage):
  Plugins\\ModSettingsPlugin.dll            the Mod Manager loader (loads modsettings.dll at Launch; settle 30 s)
  Plugins\\ModSettings\\modsettings.dll      the DLL (exports ms_get only; KERNEL32 + msvcrt)
  Plugins\\ModSettings\\modsettings.ini      settings (enabled, band_write, ini_bind, capture_*)
  Plugins\\ModSettings\\mods\\               manifests (the demo: modsettings-demo, band record 0 bits 4-7, as Step 1)
  Plugins\\ModSettings\\mod-settings.html    the companion page (Edge / Chrome; writes values.json only)
  tools\\ms-set.js / ms-check.js / ms-core.js   command-line page (lab) / manifest checker (modders); ms-core reads companion\\mod-settings.html

Files (md5, sha256, bytes):
`;
for (const f of files.sort()) { const b = fs.readFileSync(f); txt += `  ${path.relative(OUT, f).replace(/\//g, '\\')}  md5 ${h('md5', b)}  sha256 ${h('sha256', b)}  ${b.length}\n`; }
fs.writeFileSync(path.join(OUT, 'BUILD.txt'), txt.replace(/\n/g, '\r\n'));
say(txt);
