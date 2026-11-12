# Mod Settings: write one JSON file, get a settings menu (spec v3, draft 2026-11-11)

You write one manifest. No code. Players see your rows in the in-game **Mod Settings** tab under your mod's name. Mods that do not
use Mod Settings are unaffected, and your mod keeps working (on your defaults) for players who do not have it.
Status: **beta 0.1.** Proven in game: the tab and its rows, saved choices (settings.json), and the `tuning` writer (a value reaches the game's loaded data and restores). Not yet proven in game: delivery B (the description block, section 2). Check your file any time: `node tools/validate.js my-mod.json`.

## 1. The smallest manifest

```json
{
  "schema": 3,
  "modid": "jump-catch",
  "title": "Jump Catch",
  "version": "1.0.0",
  "rows": [
    { "type": "toggle", "key": "enabled", "label": "Lineman Catches", "default": false,
      "description": "On: linemen may catch passes thrown their way.",
      "bind": { "type": "tuning", "asset": "catching", "copy": "both",
                "field": "shouldLinemanMultiplayerCatch", "values": [false, true] } }
  ]
}
```

That is a working settings page: one switch that changes the game's data. Full examples: `samples/`.

## 2. Where the file goes (pick one)

| | how | when |
|---|---|---|
| **A** | `Plugins\ModSettings\mods\<modid>.json` | you ship a plugin or the player copies a file |
| **B** | paste the manifest into your .fbmod **description**, between `[[MODSETTINGS` and `MODSETTINGS]]` | you ship only an .fbmod; nothing else to install |

```
My Mod: makes the QB smarter.
[[MODSETTINGS
{ "schema": 3, "modid": "my-mod", ... }
MODSETTINGS]]
```

The DLL scans the active Mod Manager pack's .fbmods for block B (**not in the beta DLL yet**: use file A until a build says otherwise). If both exist for one `modid`, file A wins. The validator reads both.

## 3. Top-level fields

| field | rule |
|---|---|
| `schema` | `3` |
| `modid` | 2-40 of `a-z 0-9 -`. Permanent: it names your saved choices. |
| `title` | 1-40 chars. Your section heading (sections sort by title). |
| `version` | text, 1-20 chars. Shown small; used in warnings. |
| `rows` | 1-120 rows, shown in order. |

## 4. Rows

Every row: `type`, `label` (1-40). Choice rows also `key` (permanent id, 1-32 of `a-z 0-9 - _`, unique in the file), `default`, `bind`, and a one-sentence `description` (up to 400).

| type | extra fields | `default` is |
|---|---|---|
| `toggle` | none | `true` / `false` |
| `list` | `options`: 2-12 labels (1-24 chars each) | the option's position, from 0 |
| `slider` | `min`, `max`, `step`, optional `unit` | a number between `min` and `max` |
| `header` | none (just `label`) | n/a, no `bind` |
| `info` | none (`label` up to 200 chars is the text) | n/a, no `bind` |

## 5. Bindings: how a row reaches your mod

### 5.1 `tuning`: change game data, no code

```json
{ "type": "list", "key": "fumbles", "label": "Fumbles", "options": ["Fewer", "Stock", "More"], "default": 1, "applies": "next-game",
  "bind": { "type": "tuning", "asset": "ballrules", "field": "Fumble_ChanceMultiplier",
            "copy": "simulation", "values": [0.5, 1.0, 2.0] } }

{ "type": "slider", "key": "fumble-low", "label": "Fumbles, Weak Carriers", "min": 0, "max": 100, "step": 10, "default": 50, "unit": "%",
  "bind": { "type": "tuning", "asset": "ballrules", "copy": "simulation", "field": "Fumble_BaseChancePointGraph",
            "instance": "Points[1].Y", "scale": { "at-min": 0.0, "at-max": 0.5 } } }

{ "type": "list", "key": "rush", "label": "CPU Rush Start", "options": ["Low", "High"], "default": 0,
  "bind": { "type": "tuning", "asset": "gameplay_tuning", "copy": "simulation", "field": "rushMove_AIControlledDefTeamBaseChance",
            "instance": "[3]", "values": [0.45, 0.6] } }
```

| field | meaning |
|---|---|
| `asset` | the tuning asset's name, e.g. `ballrules`, `gameplay_tuning`, `fatiguemanager_small_team`, or its full path (`football/gameplay/attribsys/data/ballrules/ballrules/ballrules`). Name the base asset, not a `_gamestyle_*` file: `copy` picks the style |
| `field` | the field name inside it, as in the asset (any case) |
| `instance` | optional: one value inside the field. A curve point: `Points[2].Y` (also `.X`; `Curve.` in front is allowed). A list element: `[3]`. Nested: `[0].Points[1].Y` for a list of curves. Required when the field is a curve or a list |
| `copy` | **required.** `"simulation"`, `"base"` or `"both"` |
| `values` | toggle: `[off, on]`. List: one value per option, in order. Numbers or `true`/`false` (text values are not supported for tuning: the row is greyed) |
| `scale` | slider: `{"at-min": v, "at-max": v}`; the value is interpolated linearly between the ends |

**What gets written (the tuning writer).** Only numbers and switches: float, int8-uint32, bool fields (not enums, text, whole curves or lists).
The value is fitted to the field: whole-number fields are rounded and kept inside their type (a uint32 never goes below 0), a slider stays inside its
`scale`, true/false into a number field is 1/0. A write never adds curve points or list elements: an index past the end greys the row. A row that is
hidden (`visibleIf`) or greyed (`enabledIf`, mod not active) counts as "As installed". An asset without style copies (e.g. `coachmodifiers`) has one copy
that every style reads, so `simulation` writes it. Every write is logged in `modsettings.log` and listed in `modsettings-status.json` ("tuning": each
field's loaded and current value). `applies`: use `"now"` for curve points and for fields the game reads when it decides (most fields); `"next-game"`
for values read when a game is set up (weather, for one) or when unsure (the default).

**`copy` matters.** The game keeps a Simulation copy and a base copy of tuning data (Play Now and Dynasty can read different ones).
Only you know which your mod edits, so you must name it. Wrong choice = the setting seems to do nothing in one mode. Use `both` if unsure.

The DLL writes the **loaded** data in memory. It never edits your files, and the game restores stock values on the next launch without Mod Settings.
Your .fbmod ships the default values; the DLL changes them only when the player moves a row.

### 5.2 `api`: for DLL mods

```json
{ "type": "toggle", "key": "enabled", "label": "Auto Kick", "default": true, "bind": { "type": "api" } }
```

**Beta 0.1 exports one function, `ms_get(modid, key)`** (same arguments and results as `ModSettings_Get` below, looked up by the name `ms_get`). The names `ModSettings_Get`, `ModSettings_GetFloat` and `ModSettings_OnChange` are the planned stable API and are **not in the beta DLL yet**. The planned form (C ABI, never changes without a new function name):

```c
typedef int  (*ms_get_fn)(const char *modid, const char *key);              // toggle: 0/1; list: position; slider: stop number
typedef void (*ms_changed_fn)(const char *modid, const char *key, int value, void *user);
typedef int  (*ms_on_change_fn)(const char *modid, ms_changed_fn cb, void *user);   // returns 0 = registered

HMODULE m = GetModuleHandleA("modsettings.dll");
ms_get_fn get = m ? (ms_get_fn)GetProcAddress(m, "ms_get") : NULL;   // beta 0.1 name; "ModSettings_Get" later
int v = get ? get("auto-kick", "style") : -1;      // -1 = no Mod Settings or no such row: use your own default
```

No DLL? Read `Plugins\ModSettings\settings.json` instead (section 6). Slider values: `ms_get` returns the stop number
(0 = `min`); the planned `ModSettings_GetFloat(modid, key)` will return the real value.

### 5.3 `dynasty`: for expression (SPEX) mods

```json
{ "type": "list", "key": "mode", "label": "Recruit Mode", "options": ["Stock", "Fast", "Slow"], "default": 0,
  "bind": { "type": "dynasty", "slot": "my-mod-mode" } }
```

The DLL mirrors the value into the loaded dynasty's CrossArcData band (design: `docs/design.md`);
your expression reads it there. `slot` names your line in the shared slot list: ask us for one before you ship and never move it
once released. A dynasty row holds at most 8 values. Your expression must add its record at its default; until it exists a change waits.
No hidden requests are written to the save.

## 6. What gets stored (and what players can rely on)

- `Plugins\ModSettings\settings.json`: global choices, namespaced by `modid`. `{ "schema": 3, "global": { "my-mod": { "key": value } } }`.
- Optional per-dynasty override: `Plugins\ModSettings\dynasties\<dynasty-id>.json`, same shape; a row listed there beats the global value in that dynasty.
- Rows you removed in a new version are kept and ignored; new rows start at your `default`.
- A manifest that is old (schema below 3), unknown or broken is **skipped with a warning** in `modsettings-status.json` and on the companion page. It never crashes the game and never blocks other mods' rows.

## 7. Check it

```
node tools/validate.js my-mod.json          # errors say what is wrong and how to fix it
```

Example output: `rows[1] ("pocket-depth").bind: "copy" is required and must be "simulation", "base" or "both"...`.
A text file with the `[[MODSETTINGS` block pasted in works too. Planned: a one-page builder (open `manifest-builder.html`, click, copy the JSON)
and live reload (save the file and the open game shows the change within seconds).

## 8. Rules of thumb

- Make the default the normal game for your mod. Players without Mod Settings get exactly that.
- One sentence per `description`, written for a player, not a modder.
- Do not rename a `key` or `modid` after release.

## Names, duplicates and clashes (added 11-11)

**A section is identified by `modid`, never by `title`.** The title is only what players see.

- **modid is normalised** before comparing: lower case, spaces / underscores / dots become `-`, anything else that is not a-z 0-9 `-` is dropped (`My_Mod`, `my mod`, `MY-MOD` are all `my-mod`). Recommended form: `yourname-modname` (e.g. `pocket-presence` is fine for a sample, `jdoe-penalties` for a real mod).
- **Two mods, same title, different modid:** two sections. If the visible titles match (ignoring case and spaces), each header gets its source added: `Penalties (Realism Pack.fbmod)` / `Penalties (Other Mod.fbmod)`.
- **Same modid, same mod twice** (the JSON file AND a description block, or two versions of the same .fbmod enabled): one section; the highest `version` wins; the other is listed once in the plugin log as "ignored: older copy".
- **Same modid, different mods** (different titles or rows): both are shown; the second one's settings are stored under `modid@<source file>` so neither overwrites the other, and its header shows a small "duplicate id" note. The validator and the builder page warn about this before you ship.
- **Same `key` twice inside one mod:** validator error; in game the first row wins and the log names the duplicate.
- **Renaming:** changing `title`, labels or descriptions keeps players' settings; changing `modid` or a row `key` starts that setting from its default.
- **Order:** sections sorted by visible title (A-Z), rows in the order the manifest lists them.

## Adding rows to another mod's section (added 11-11)

A manifest may set `"section"` to put its rows under a section it does not own:

```json
{ "schema": 3, "modid": "jdoe-penalty-addon", "title": "Penalty Add-on",
  "section": "realism-penalties",
  "rows": [ { "type": "toggle", "key": "late-hits", "label": "Late Hit Flags", "default": true } ] }
```

- **Target = another mod's `modid`:** the rows appear under that mod's header, after its own rows, behind a small sub-header "added by <title>". The owner may set `"closed": true` to refuse; refused rows go to the contributor's own section with a note.
- **Target = a shared section `@name`** (`@penalties`, `@pass-rush`, `@fatigue`, `@kicking`, `@presentation`, ...): an open section anyone can join, shown with a friendly title (`@pass-rush` -> "Pass Rush"). Several mods joining the same shared section each get their own sub-header inside it. The list of shared names lives in this spec; unknown `@names` still work (title from the name).
- **Ownership never moves:** settings are stored under the contributor's own `modid` + `key`, so two mods can both use `late-hits`; removing or updating one never touches the other's values.
- **Fallback:** if the target mod is not installed, the rows show in the contributor's own section (header = its title), so nothing disappears.
- **Order inside a section:** the owner's rows first, then contributors sorted by title; shared sections sort contributors by title.


## Safety and behaviour rules (added 11-11)

Mod Settings targets **offline play**. Mods are offline only, so there is no online or multiplayer guard.

| # | rule | what you write | what players see |
|---|---|---|---|
| 1 | **Two mods bind the same field** (same `asset` + `field` + `instance`, overlapping `copy`) | nothing | both rows read "... - also set by <other mod's title>"; the last change wins; the plugin log names the pair once |
| 2 | **Your own data already changed the field** | nothing | every `tuning` row gets a first option **"As installed"**. It is the default, writes nothing and keeps (or restores) the value the game loaded, so your .fbmod's own value is what plays until a player picks something else. Your `default` is ignored for tuning rows. Saved as `"as-installed"` in settings.json |
| 3 | **When a change applies** | row field `"applies"`: `"now"`, `"next-game"` (the default) or `"restart"` | the help line ends "Applies: right away / from the next game / after a restart". Keep `next-game` until the timing probe proves `now` for your field |
| 4 | **Game style** | your binding's `copy` | a `copy: "simulation"` row's help says "Simulation style only"; a `copy: "base"` row says "Not used in Simulation style". (Greying the row in the other style needs the running style, which is not read yet.) |
| 6 | **After a game update** | optional top-level `"gameVersion"`: a number or a list of numbers (the Mod Manager's game version, e.g. `4338538`) | when the running game is a different known version, your tuning rows show "not available on this game version" and are never written. A field that cannot be found in the running game gets the same treatment when the tuning writer looks it up. Never a crash |
| 7 | **Bad values** | `values` / `scale` of ONE type (all numbers, or all true/false, or all text) | mixed types: validator error; in the game the row is greyed ("values of different types"). At write time a value is clamped to the field's real type and range |
| 8 | **Your mod is turned off but its manifest is still there** | optional top-level `"fbmod"`: your .fbmod's file name | when the Mod Manager's selected pack has that file off (or not at all), your section is hidden. `show_inactive = 1` in modsettings.ini shows it greyed with "(mod not active)" instead. Without `fbmod` (plugin-only mods) the section always shows |
| 9 | **Rows that depend on each other** | row fields `"visibleIf"` / `"enabledIf"`: `{ "key": "jump-catch", "eq": true }`; also `"ne"`, or `"in": [ ... ]` (up to 8 values); `"mod": "<modid>"` to look at a row of the section's owner or of another mod in the same section | a hidden row is left out, a disabled row is greyed. `eq` compares with the row's value: true/false for a toggle, the option position or its label for a list, the number for a slider ("As installed" equals nothing). The screen keeps its list while it is open, so a dependent row changes when the tab is opened again |
| 19 | **Help pages** | top-level `"about"` (up to 2000 characters), `"credits"` (up to 400), `"links"`: up to 8 `{ "title": "...", "url": "https://..." }` | an "About <title>" row at the top of your section; its help panel shows the text, the credits and the links |
