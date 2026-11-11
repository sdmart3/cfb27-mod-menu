Mod Settings (modsettings.dll) 0.1.0-l1 - TEST BUILD, not a release
====================================================================

What it does: one place for the settings of the mods you have installed. Each mod that joins puts a small description
file (a "manifest") in Plugins\ModSettings\mods\. You pick your choices on the Mod Settings page (mod-settings.html).
Mod Settings itself ships NO game files, so it cannot clash with any mod's files.

Two kinds of settings:
- Settings for the whole game (for tools such as Call Sheet): kept in values.json. A mod reads them itself, or Mod Settings
  writes the one line of that mod's own .ini the setting is bound to.
- Settings for your dynasty: kept inside the dynasty save. Mod Settings writes them into the dynasty that is loaded, the
  next time the dynasty hub refreshes or you open Roster or Schedule. Then save as usual. A dynasty setting can only land
  after that mod's first Advance in a new dynasty (the mod makes its storage then); until then it waits.

Without this DLL every mod still works: it uses its own defaults.

Needs: College Football 27 at the 2026-10-29 or the 2026-10-13 title update (any other version: nothing is installed in the
game; modsettings.log says why). Mod Manager only (the plugin loads the DLL when you press Launch).

Files: Plugins\ModSettingsPlugin.dll (the loader) + Plugins\ModSettings\ (modsettings.dll, modsettings.ini, plugin.cfg,
mods\, this README). Written while running: values.json (by the page), modsettings-status.json, applied.json,
modsettings.log, plugin.log.
Uninstall: delete Plugins\ModSettingsPlugin.dll and the Plugins\ModSettings folder.
