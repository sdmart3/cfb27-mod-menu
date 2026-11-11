# CFB 27 Mod Menu (Mod Settings)

One in-game **Mod Settings** tab for every installed mod. A modder writes a small JSON manifest (no code); players get a settings page
for it, and the DLL applies the choices. Status: **in development, test builds only.**

- `docs/manifest-spec.md`: the manifest format (schema v3)
- `docs/design.md`: how it works and why
- `dll-plugin/`: the plugin DLL (C), the Mod Manager plugin loader (C#), the companion page, packaging tools
- `tab/`: the generator for the in-game tab (.fbmod)
- `tools/validate.js`: manifest validator
- `samples/`: example manifests

## Modder quick start

One toggle:

```json
{
  "schema": 3,
  "modid": "jump-catch",
  "title": "Jump Catch",
  "version": "1.0.0",
  "rows": [
    { "type": "toggle", "key": "enabled", "label": "Jump Catches", "default": true,
      "description": "On: receivers may leap for balls in traffic.",
      "bind": { "type": "tuning", "asset": "catching/Multiplayer_SP_Catch_DB", "copy": "both",
                "field": "Catching_AllowedToJumpCatch", "values": [false, true] } }
  ]
}
```

Two ways to deliver it:

1. **JSON file:** `Plugins\ModSettings\mods\<modid>.json`.
2. **Description block:** paste the JSON into your .fbmod description between `[[MODSETTINGS` and `MODSETTINGS]]`.

Check it (Node.js required):

```
node tools/validate.js my-mod.json
```

Errors say what is wrong and how to fix it. See `docs/manifest-spec.md` for every row type and binding.

Good to know before you publish: your `modid` should be unique (a second mod with the same id is kept but its settings are stored as `modid@file`), a duplicate row `key` in one manifest means the first wins, and a manifest can add rows to another mod's section by naming that section. The new spec sections "Names/duplicates", "Adding rows to another mod's section" and "Safety and behaviour rules" cover it.

## Building

No binaries are committed. This repo carries no EA game files either.

- **DLL:** `node dll-plugin/dll/build.js` (needs a C toolchain; the build also reads game memory images for pin generation, which are
  not included: set `MS_ROOT` to a folder holding `reference/memimage-tu-10-13.bin` and `reference/memimage-tu-10-29.bin`, or use `--no-test`
  once `ms_pins.h` is already generated, as it is here).
- **Plugin loader:** `dll-plugin/plugin/build-plugin.ps1`.
- **In-game tab (.fbmod):** the built .fbmod is not in git. `tab/src/ui-build-ms.js` edits the player's own installed
  `scriptpod.ast` at build time. The maintainers' wrapper (`build.js`) depends on private tooling and is not included yet, so
  building the tab from this repo alone is not turnkey; the generator, manifests and emulator tests are here for reading and review.

## Contributing

See `CONTRIBUTING.md`.
