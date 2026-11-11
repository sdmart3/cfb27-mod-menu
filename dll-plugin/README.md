# Mod Settings (DLL-lite), layer 1

One place for the settings of every installed mod, with **zero game files shipped**, so Mod Settings itself clashes with no mod's
files. Design: `docs/design.md` (sections 10-11 = the O1 / O2 answers and this build). Manifest
format: `docs/manifest-spec.md`. Status: **test build, offline-proven only; not run in the game yet**.

## What layer 1 does

| piece | where | what |
|---|---|---|
| manifests | `Plugins\ModSettings\mods\<id>.json` | one per mod (spec v2): rows, options, default, store |
| choices | `Plugins\ModSettings\values.json` | written ONLY by the page / `ms-set.js`; hot-read by the DLL (every second) |
| global rows | values.json | DLL mods read them with `ms_get`; an optional `ini` binding makes the DLL rewrite ONE key of that mod's ini |
| dynasty rows | the save: CrossArcData band -8192..-7169 | the DLL edits an existing band record's Value in the loaded dynasty (never creates records) |
| status | `modsettings-status.json` | written by the DLL: state, capture sites, every row's value, the dynasty's band records, pending changes |
| applied | `applied.json` | written by the DLL: which dynasty changes (by seq) already landed |
| page | `mod-settings.html` | a local page (Edge / Chrome, File System Access): pick the folder, click options; no server, no network |

**Where the band write happens (the safe point).** Only on a dynasty server thread: the server handler of
MetersFlow::RefreshFormResponse, MySchoolFlow::MySchoolRefreshFormResponse, RosterFlow::EnterResponse and
TeamScheduleFlow::EnterResponse (each RPC thunk's handler call is redirected; the handler runs first, then the band pass, then the
handler's result goes back unchanged). The pass uses the game's own generic `FindUser(flow, a2, &user)`, reads
`user.CrossArcDataArray` (handle 11), and writes with the record interface's int setter + the native change counter (wsdata's
proven write). Nothing is kept between calls. None of these four sites is patched by another DLL of ours.

**Rules for mods.** A mod must work without this DLL: its default (bit pattern 0 = default is the spec's advice) is what happens when
nothing was set. A band row needs its record to exist: the consumer mod's own expression code adds it (with its defaults) at its first
Advance; until then a dynasty change waits ("pending") and lands at the first capture after that.

## Layout

```
dll/              modsettings.dll: src/modsettings.c (+ msjson.h, selftest.h, patchsafe.h vendored from wsdata), tools/gen-pins.js, build.js
plugin/           ModSettingsPlugin.dll: the wsdata Mod Manager loader with the names changed (stand-aside key "modsettings")
companion/        mod-settings.html (the page; its <script id="ms-core"> is the shared rules)
tools/            ms-core.js (loads ms-core from the page), ms-set.js (the page on the command line), ms-check.js (manifest + slots.json check),
                  package.js (-> exports/build-mods/modsettings-dll-l1/ + BUILD.txt)
test/             companion-test.js (page rules, parity with the DLL on the same folder, ms-set, ms-check)
examples/mods/    modsettings-demo.json (the Step 1 demo rows: band record 0 bits 4-7)
```

## Build and test (offline)

```
node dll/build.js                                      # pins from both dumps, the DLL, the test exe, every self-test once, the load test
powershell -ExecutionPolicy Bypass -File plugin/build-plugin.ps1   # the loader (csc; Mod Manager refs read in place, read-only)
node test/companion-test.js                            # after dll/build.js
node tools/package.js                                  # all of the above once + the package and BUILD.txt
```

Self-tests (`modsettings-test.exe`): json, config, ini, records (fake dynasty database), guard, patchsafe, and `dump <file>` on BOTH
reference dumps (TU 10-13, TU 10-29): every pin, the full 4-site install with each window hash as expected, then each broken pin
(unknown stamp, the other build's stamp, a changed function head, a redirected call, a renamed thunk, a changed window byte,
`patch_verify = 0`). `modsettings-test.exe load <dir>` prints what the DLL makes of a folder (the companion test compares it with the
page).

## For DLL mods: `ms_get`

```c
typedef int (*ms_get_fn)(const char *mod, const char *row);
HMODULE m = GetModuleHandleA("modsettings.dll");
ms_get_fn ms_get = m ? (ms_get_fn)GetProcAddress(m, "ms_get") : NULL;
int v = ms_get ? ms_get("my-mod", "fast") : -1;   /* -1: no DLL / no such row -> use your own default */
```

Returns the option index: a global row's value (values.json or its default); a band row's value in the loaded dynasty as last read
(its default when the record is not there yet; -2 before any dynasty was read). Call it any time; it never touches the game.

## Title updates

`dll/tools/gen-pins.js` knows two builds. After a title update: verify FindUser / the ARRY count / the change counter on the new dump
(wsdata's verification), add the build to `KNOWN`, rebuild. Until then the DLL installs nothing on the new build (values.json and ini
bindings keep working; dynasty changes wait).

## Not in layer 1

No in-game rows (layer 2: wrapping the native settings adapter, see the scope doc section 10.1), no own tab, no values for players
without the DLL (they get defaults), no UI-only (ActionScript) consumers. Mod Manager only.
