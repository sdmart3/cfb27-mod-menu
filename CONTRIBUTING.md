# Contributing

## Propose a shared @section name

Shared sections group related mods in the tab (several mods can join one section by using the same name). To propose one, open an
issue titled `Section: <name>` with: the name (short, plain words), what kind of mods belong in it, and two or more mods that
would use it. Names are first-come; a clash with an existing section is resolved by joining it or picking a different name.
The duplicate-name and section-joining rules are in `docs/manifest-spec.md`.

## Report a manifest bug

Open an issue with: your manifest (or the block from your .fbmod description), the output of `node tools/validate.js <file>`,
what you expected, and what the tab showed. If the validator accepts a file the game mishandles, or rejects a valid one, say so
in the title. Please do not attach game files.

## Pull requests

Keep changes small, run `node tools/validate.js --selftest` and the tests under `dll-plugin/test/`, and do not commit built
binaries or EA game files.
