// build.js: builds modsettings.dll (layer 1) + modsettings-test.exe (the same sources, -DAP_TEST) + loadhost.exe, then runs every test ONCE.
// Writes ONLY under <this folder>/out. Never touches the Mod Manager folder, the game folder, the saves or another tool's folder.
// gcc / windres / objdump from PATH (the Strawberry Perl mingw-w64 the other tools build with).
//   node build.js            regenerate the pins from both known builds' dumps (TU 10-13, TU 10-29), build, test
//   node build.js --no-test  build only
'use strict';
const fs = require('fs'), path = require('path'), cp = require('child_process'), crypto = require('crypto');
function findRoot(d) { for (let i = 0; i < 6; i++, d = path.dirname(d)) if (fs.existsSync(path.join(d, 'reference'))) return d; throw new Error('reference/ not found above ' + __dirname); }
const DLL = __dirname, OUT = path.join(DLL, 'out'), ROOT = process.env.MS_ROOT || findRoot(path.resolve(DLL, '..'));
const DUMPS = [['1013', 'TU 10-13', path.join(ROOT, 'reference/CollegeFB27.memdump-2026-10-13.bin')], ['1029', 'TU 10-29', path.join(ROOT, 'reference/CollegeFB27.memdump-2026-10-29.bin')]];
const noTest = process.argv.includes('--no-test');
fs.mkdirSync(OUT, { recursive: true });
const logPath = path.join(OUT, 'build.log'); fs.writeFileSync(logPath, '');
const say = (s) => { console.log(s); fs.appendFileSync(logPath, s + '\n'); };
function run(exe, args, opt = {}) {
  const r = cp.spawnSync(exe, args, { cwd: opt.cwd || DLL, encoding: 'utf8', maxBuffer: 1 << 28, timeout: opt.timeout || 600000 });
  return { code: r.status, out: (r.stdout || '') + (r.stderr || ''), err: r.error };
}
const CF = ['-O2', '-Wall', '-Wextra', '-Wno-unused-parameter', '-Wno-unused-function', '-Wno-missing-field-initializers', '-Wno-cast-function-type', '-Wno-misleading-indentation'];
function gcc(what, args) {
  const r = run('gcc', args);
  if (r.code !== 0) { say(r.out); throw new Error(what + ' did not build'); }
  if (r.out.trim()) { say(r.out); throw new Error(what + ' built with warnings (see above)'); }
  say('  ' + what + ' built, no warnings');
}
function imports(file) {
  const r = run('objdump', ['-p', file]);
  const dlls = [...r.out.matchAll(/DLL Name: (\S+)/g)].map((m) => m[1]);
  const extra = dlls.filter((d) => !['KERNEL32.dll', 'msvcrt.dll'].includes(d));
  if (extra.length) throw new Error(file + ' imports ' + extra.join(', ') + ': link it statically. Do NOT ship this build.');
  const exp = r.out.split(/\r?\n/).filter((l) => /^\s+\[\s*\d+\]\s+\w+/.test(l));
  return { dlls, exp };
}
/* patchsafe.h is VENDORED BYTE FOR BYTE from wsdata (= Dynasty Hooks / Press Box): a difference stops the build */
function vendorCheck() {
  const mine = fs.readFileSync(path.join(DLL, 'src', 'patchsafe.h'), 'utf8').replace(/\r\n/g, '\n');
  const g = cp.spawnSync('git', ['-C', path.join(ROOT, 'wsdata'), 'show', 'main:dll/src/patchsafe.h'], { encoding: 'utf8', maxBuffer: 1 << 26 });
  if (g.status !== 0) { say('  NOTE: wsdata main has no dll/src/patchsafe.h; parity not checked'); return; }
  if (g.stdout.replace(/\r\n/g, '\n') !== mine) throw new Error('src/patchsafe.h differs from wsdata main:dll/src/patchsafe.h (copy it over: one source)');
  say('  patchsafe.h identical to wsdata main');
}
let fails = 0, nok = 0;
function test(name, args, logName) {
  const r = run(path.join(OUT, 'modsettings-test.exe'), args);
  fs.writeFileSync(path.join(OUT, logName), r.out);
  const ok = (r.out.match(/^  ok: /gm) || []).length + (r.out.match(/^patchsafe: .* -> ok\r?$/gm) || []).length; nok += ok;
  const bad = r.out.split(/\r?\n/).filter((l) => /^  FAIL|patchsafe: .*FAIL/.test(l));
  for (const l of bad) say('  ' + l);
  if (r.code !== 0) { say(`  ${name} FAILED (${logName})`); fails++; } else say(`  ${name}: ${ok} checks ok`);
  return r;
}
const md5 = (b) => crypto.createHash('md5').update(b).digest('hex'), sha256 = (b) => crypto.createHash('sha256').update(b).digest('hex');
try {
  say('--- vendored patchsafe.h');
  vendorCheck();
  say('--- pins from both known builds (the reference dumps)');
  let r = run('node', [path.join(DLL, 'tools/gen-pins.js'), ...DUMPS.map((d) => d[2])]); say('  ' + r.out.trim()); if (r.code !== 0) throw new Error('gen-pins failed');
  say('--- modsettings.dll');
  r = run('windres', ['-O', 'coff', '-I', 'src', '-o', 'out/modsettings.res.o', 'src/modsettings.rc']); if (r.code !== 0) { say(r.out); throw new Error('windres failed'); }
  gcc('modsettings.dll', [...CF, '-shared', '-static', '-s', '-Wl,--exclude-all-symbols', '-o', 'out/modsettings.dll', 'src/modsettings.c', 'out/modsettings.res.o']);
  let im = imports(path.join(OUT, 'modsettings.dll'));
  const names = im.exp.map((l) => l.trim().replace(/^\[\s*\d+\]\s+/, '')).sort();
  say('  imports: ' + im.dlls.join(', ') + '; exports: ' + (names.join(', ') || 'none'));
  if (names.join(',') !== 'ms_get') throw new Error('modsettings.dll must export exactly ms_get');
  say('--- modsettings-test.exe');
  gcc('modsettings-test.exe', [...CF, '-static', '-DAP_TEST', '-o', 'out/modsettings-test.exe', 'src/modsettings.c']);
  gcc('loadhost.exe', ['-O2', '-Wall', '-static', '-o', 'out/loadhost.exe', 'tests/loadhost.c']);
  if (!noTest) {
    say('--- self-tests');
    test('json', ['json'], 'st-json.log');
    for (const m of ['config', 'ini', 'records', 'guard', 'tab', 'names', 'rules']) { const d = path.join(OUT, 't-' + m); fs.rmSync(d, { recursive: true, force: true }); test(m, [m, d], `st-${m}.log`); }
    test('patchsafe', ['patchsafe', '300'], 'st-patchsafe.log');
    for (const [tag, name, dump] of DUMPS) { say(`--- the ${name} dump (pins, the full install, every broken pin)`); test('dump ' + tag, ['dump', dump], `dump-${tag}.log`); }
    for (const [tag, name, dump] of DUMPS) { say(`--- tuning writer on the ${name} dump (its reflection tables + a fake registry and instances)`); const d = path.join(OUT, 't-tuning-' + tag); fs.rmSync(d, { recursive: true, force: true }); test('tuning ' + tag, ['tuning', dump, d], `st-tuning-${tag}.log`); }
    say(`  ${nok} checks ok in all`);
    say('--- load test (a host that is not the game: "game not found", the status file, the double-load guard)');
    const lt = path.join(OUT, 'loadtest'); fs.rmSync(lt, { recursive: true, force: true }); fs.mkdirSync(path.join(lt, 'mods'), { recursive: true });
    fs.copyFileSync(path.join(OUT, 'modsettings.dll'), path.join(lt, 'modsettings.dll'));
    fs.copyFileSync(path.join(DLL, 'modsettings.ini'), path.join(lt, 'modsettings.ini'));
    fs.copyFileSync(path.join(DLL, '..', 'examples', 'mods', 'modsettings-demo.json'), path.join(lt, 'mods', 'modsettings-demo.json'));
    r = run(path.join(OUT, 'loadhost.exe'), [path.join(lt, 'modsettings.dll')], { cwd: lt, timeout: 30000 });
    for (const l of r.out.split(/\r?\n/)) if (l.trim()) say('  ' + l);
    const lg = fs.existsSync(path.join(lt, 'modsettings.log')) ? fs.readFileSync(path.join(lt, 'modsettings.log'), 'utf8') : '';
    let st = null; try { st = JSON.parse(fs.readFileSync(path.join(lt, 'modsettings-status.json'), 'utf8')); } catch (e) { st = null; }
    const good = r.code === 0 && /game not found/.test(lg) && /configuration read: 1 mod\(s\) \[modsettings-demo\], 3 row\(s\), 0 problem/.test(lg) && st && st.rows.length === 3 && st.manifests.loaded[0] === 'modsettings-demo';
    say(good ? '  load test ok (status: state ' + st.state + ', 3 rows from the demo manifest)' : '  load test FAILED'); if (!good) fails++;
    // the two spec v3 sample manifests (modsettings-dll/samples, written by the spec v3 pass): must load with no problem, under their modid names
    const SM = path.join(DLL, '..', 'samples'), sd = path.join(OUT, 'samples-load'); fs.rmSync(sd, { recursive: true, force: true }); fs.mkdirSync(path.join(sd, 'mods'), { recursive: true });
    const sm = fs.existsSync(SM) ? fs.readdirSync(SM).filter((f) => f.endsWith('.json')) : [];
    for (const f of sm) { const j = JSON.parse(fs.readFileSync(path.join(SM, f), 'utf8')); fs.copyFileSync(path.join(SM, f), path.join(sd, 'mods', j.modid + '.json')); }
    r = run(path.join(OUT, 'modsettings-test.exe'), ['load', sd]);
    const sOk = sm.length === 2 && r.code === 0 && /^LOADED auto-kick,pocket-presence\s*$/m.test(r.out) && !/^PROBLEM/m.test(r.out);
    const sLines = r.out.split(/\r?\n/).filter((l) => /^(LOADED|PROBLEM)/.test(l)).join(' | ');
    say(sOk ? `  samples load ok (${sm.join(', ')}: auto-kick + pocket-presence, no problem)` : `  samples load FAILED (${sm.length} sample file(s)): ${sLines}`); if (!sOk) fails++;
  }
  const dll = fs.readFileSync(path.join(OUT, 'modsettings.dll'));
  say(`modsettings.dll md5 ${md5(dll)} sha256 ${sha256(dll)} size ${dll.length}`);
  say(fails ? `BUILD DONE WITH ${fails} TEST FAILURE(S)` : 'build + every test passed');
  process.exit(fails ? 1 : 0);
} catch (e) { say('BUILD FAILED: ' + e.message); process.exit(1); }
