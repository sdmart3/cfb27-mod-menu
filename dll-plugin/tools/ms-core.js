// ms-core.js: loads MsCore (the Mod Settings rules) from companion/mod-settings.html, so the page, the CLI tools and the tests share ONE source.
'use strict';
const fs = require('fs'), path = require('path'), vm = require('vm');
const HTML = path.join(__dirname, '..', 'companion', 'mod-settings.html');
function load(file = HTML) {
  const html = fs.readFileSync(file, 'utf8');
  const m = html.match(/<script id="ms-core">([\s\S]*?)<\/script>/);
  if (!m) throw new Error(`${file}: no <script id="ms-core"> block`);
  const ctx = { module: { exports: null } }; vm.createContext(ctx); vm.runInContext(m[1], ctx, { filename: 'mod-settings.html#ms-core' });
  return ctx.module.exports;
}
module.exports = load();
module.exports.loadFrom = load;
