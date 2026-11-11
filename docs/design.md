# Mod Menu (universal Mod Settings), scope v3, 2026-11-11

Scoping only: nothing built, launched or published. Goal: maximum usability and least hassle for other modders; a little extra work on the maintainers' side is fine.
Principle: **a modder writes one JSON manifest and no code; we absorb the hard parts.**
Builds on `docs/design.md` (Layer 1 built, tested in game 11-08: manifests, values JSON, band writes at 4 server captures,
companion page), `docs/design.md` and spec v2. The modder-facing spec is `docs/manifest-spec.md`.
Labels: **PROVEN** / **LIKELY** / **UNKNOWN**.

## 1. What changes from v2

| v2 (Layer 1, built) | v3 |
|---|---|
| rows: options list + band slot / global + ini | row types toggle / list / slider / header / info; a `bind` per row |
| only dynasty (band) and global-ini consumers | three bindings reach every mod kind: `tuning` (data mods), `api` (DLL mods), `dynasty` (expression mods) |
| delivery: file in `Plugins\ModSettings\mods\` only | plus (b): block pasted in the .fbmod description |
| UI: companion page only | in-game **Mod Settings** tab (.fbmod GenericSettings tab + DLL-fed rows), companion page as fallback |
| `ms_get(mod,row)` | `ModSettings_Get` + on-change callback + `settings.json` |

v2 manifests (schema 2) stay loadable by the Layer 1 code until the v3 loader replaces it; the v3 loader skips them with a warning. A converter v2 to v3 is trivial (band row = `dynasty` bind, global row = `api`).

## 2. Design

### 2.1 Manifest and delivery
One manifest per mod (`modid`, `title`, `version`, `schema`, `rows`). Two paths: **(a)** `Plugins\ModSettings\mods\<modid>.json`; **(b)** a block between `[[MODSETTINGS` / `MODSETTINGS]]` in the .fbmod description.
Path (b) is the least-hassle route: an .fbmod-only mod gets a menu with no extra file to install.
Finding block (b): the DLL scans the .fbmods of the **active Mod Manager pack**. `tools/manager-cache.js` already finds the manager folder and reads `Caches/collegefootball27.cache`, but the cache holds game assets, not the pack list, and the description lives in the .fbmod header.
**UNKNOWN (probe M1):** where the manager records the active pack and the mod folder (a profile / pack file next to the manager exe, or the loaded-mods list the game process already reads). Fallbacks, in order: the manager's mods folder scan (all .fbmods, not only active ones; wrong for disabled mods, so rows would show for mods not loaded); our plugin loader reporting the active list (it is already a Mod Manager plugin with manager refs). Note the description is also visible to the manifest only if a `modid` collision rule exists: file (a) beats block (b).
Parsing is JSON in a bounded text window (64 KB per block); anything malformed is skipped with a warning.

### 2.2 In-game tab
- **Shell:** our own .fbmod adds a "Mod Settings" tab to the Settings hub using GenericSettings (PROVEN 11-06: probe 120 drew 120 rows in a new tab; no row cap in the AS loop). Decision:  the file clash with the broadcast mod is not a concern (its owner would adopt ours), so the .fbmod route stays.
- **Rows:** the tab's list is fed by the DLL: one header per mod (sorted by title), then that mod's rows. The static .fbmod ships an empty list; the DLL fills it through the settings adapter (O1 LIKELY from 10-29 vtable work: adapter 0x14baca988, holder 0x14da1dda0, private keys from 0x4D00). Slider rows render as a stepped list (the game's settings screen has no native slider; a list of stops is the safe draw). Header and info rows are label-only rows.
- **Fallback:** if the tab cannot be fed (adapter pin fails after a title update), the companion page (`mod-settings.html`) still edits every value. No DLL = no rows, mods run on defaults.
- Open: whether the tab can also be reached with **no .fbmod** (Layer 2: block at the bottom of Game Options, DLL-only, no files). Keep as the zero-file alternative if the .fbmod tab loses on clashes.

### 2.3 Bindings
- **tuning (the big new piece).** `asset` + `field` (+ `instance` path for curves) + one value per option or a slider `scale`. The DLL resolves the loaded tuning object (an Ebx instance in memory, field by name through the type info) and writes the value. **Simulation vs base copy:** the game keeps both; the modder must name `copy: simulation | base | both` (validator error if missing). Open items: how the DLL finds the live asset by path (UNKNOWN, probe T1: the Ebx partition/instance lookup the game uses); when a re-read happens (does gameplay cache the value at game start or read each play; probe T2, decides whether a change applies "next snap", "next game" or "next load"); float vs int field coercion; curve points count (a write never adds points). Writes only primitives; unknown asset/field = row greyed with a clear status, never a crash. Original value is captured at first write so "Off" can restore stock.
- **api.** C ABI, version-stable: `ModSettings_Get(modid, key)`, `ModSettings_GetFloat`, `ModSettings_OnChange(modid, cb, user)`. Layer 1's `ms_get` stays as an alias. Or read `settings.json` (hot-reloaded).
- **dynasty.** Layer 1's band write (4 server captures, record must pre-exist, no hidden requests) renamed as a binding; slot registry in `modsettings/slots.json`. Capacity unchanged: 8 values per row, 9 bits per record (`docs/design.md`).

### 2.4 Storage
Global `Plugins\ModSettings\settings.json`, namespaced by `modid`. Optional per-dynasty override `dynasties\<id>.json` (the DLL knows the loaded dynasty from the capture). Unknown or older manifests: ignored with a warning in `modsettings-status.json` + the page, never a crash. Rows for removed mods are kept. **Nothing hidden in the save** except the explicit `dynasty` band bits. Writes are atomic (temp + rename), as Layer 1.

## 3. Modder tooling (what makes it low-hassle)
1. **Validator** `tools/validate.js` (written 11-11, 27 selftests, both samples pass): human errors with "did you mean", reads a .json or a pasted block.
2. **Builder page**: one self-contained HTML, no CDN: add rows, pick binding, live preview of the in-game list, copy JSON / copy description block. Reuses the validator core (same pattern as ms-core in the companion page).
3. **Live reload**: DLL re-reads manifests when files change (Layer 1 hot-reads values.json every second; extend to mods\). Description blocks reload when the pack changes.
4. Error text: every message says which row, what is wrong, how to fix it.
5. A one-page how-to; the source is public in this repo.

## 4. Effort and order
| step | what | effort | gate |
|---|---|---|---|
| 0 | this scope, spec v3, samples, validator | done 11-11 | none |
| 1 | **Tab + manifests**: v3 loader (path a + b), settings.json, in-game tab shell + DLL-fed rows, page update | ~1.5-2 wk + 2 labs | probes M1 (active pack), O1/P1 (adapter feeding) |
| 2 | **tuning binding** | ~1-1.5 wk + 2 labs | probes T1, T2 |
| 3 | **C API** (+ on-change, GetFloat, header file `modsettings.h`) | ~3-4 days + 1 lab | none; cheap |
| 4 | builder page, live reload, how-to | ~1 wk | with step 1-2 |

## 5. Risks and open questions
1. **Active pack discovery (M1)** is the biggest unknown for path (b). If unreliable, ship (a) first and (b) when found.
2. **Tuning write safety**: a wrong write can desync gameplay; restrict to numeric/bool fields, bound-check slider scales, capture originals, never write during a play unless the probe says the data is read per snap. Multiplayer/online: tuning edits are local; the manifest and docs must say so (parity with other tuning mods).
3. **Adapter feeding** (O1) not yet shown in game; Layer 2 and the tab depend on it. Companion page is the floor.
4. **Title update repin** for every hook (band captures, adapter, tuning lookup); the loader refuses on unknown builds (as Layer 1).
5. **Two mods, one field**: last write wins; the DLL should warn when two manifests bind the same `asset+field`.
6. **Description size**: .fbmod descriptions may be length-capped by the Mod Manager (UNKNOWN; probe with a 4 KB block). If capped, long manifests need path (a).
7. **Simulation/base choice** can be wrong; validator forces the question, the docs explain it, a status line shows which copy was written.
8. **Slider on a list screen**: draw as stops; real sliders would need AS work.
9. **Slot registry** for dynasty bits needs a human step ("ask us"); could be automated later by a hash-assigned slot, but the 150-record table is tight at 64 teams (about 18 options).
10. Adoption: needs other modders to write a manifest; worst case it serves our own mods (DE, Ratings Lab, AutoKick, Midgame Save) first.
11. No names of other modders appear in any of these files.

## Note 11-11 (0b): probe M1 is already answered
The active pack IS discoverable: tools/game-runner.js (readMmConfig / setRunnerPack, lines ~330-345) reads the Mod Manager's config JSON (MM_CONFIG = %LOCALAPPDATA%MMCmanager_config.json): `Games[<cfb>].Options.SelectedPack` = the active pack name, `Games[<cfb>].Packs[<name>]` = "file.fbmod:True|file2.fbmod:False|..." (enabled flag per mod, in load order). The mod files live in the manager's mods folder. So the DLL can read the selected pack's enabled .fbmods at load and scan their descriptions for [[MODSETTINGS blocks (path b). Remaining check: the config path when the manager is not the one in the runner's discovery (use the same discovery as tools/manager-cache.js findManagerDirs).
