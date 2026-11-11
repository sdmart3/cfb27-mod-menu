'use strict';
// as-vm.js: a small Apt ActionScript interpreter for the Mod Settings tests (NOT the engine: objects are JS objects, natives are mocks).
// Extends Dynasty Extensions' test/apt-interp.js with what the page needs: DefineFunction2 closures (anonymous functions with their own registers and
// parameter arrays), InitArray, NewMethod, PushValueOfVar, CallNamedMethod(Pop), StrictEquals, TypeOf, Increment, Multiply.
const path = require('path');
const ROOT = path.join(__dirname, '..', '..');
const apt = require(path.join(ROOT, 'tools', 'apt.js'));

function vm(c, data, G, warn = () => {}) {
  const pool = c.ia.pool; const ins = c.s.ins; const idx = new Map(ins.map((x, k) => [x, k]));
  const P = (i) => (i.op === 0xa3 ? pool[i.raw.readUInt16LE(0)] : pool[i.raw[0]]);
  const params = (f) => { const n = f.raw.readUInt32LE(8); const pp = Number(f.raw.readBigUInt64LE(0x10)); const out = []; for (let k = 0; k < n; k++) out.push(data.readUInt32LE(pp + k * 16)); return out; };
  const walk = (o, names, what) => { for (const n of names) { if (o === undefined || o === null) { warn(`path ${names.join('.')} broke at ${n} (${what})`); return undefined; } o = o[n]; } return o; };
  const counters = { calls: 0, steps: 0 };
  function closure(f) {
    const regsN = f.raw[12]; const pr = params(f); const start = idx.get(f) + 1; const end = idx.get(f.bodyEnd);
    const flags = f.raw.readUInt16LE(14);
    const fn = function (...args) { counters.calls++; return exec(start, end, this, args, pr, regsN, flags & 0x10 ? fn.__super : undefined); };
    fn.__rec = f; return fn;
  }
  function exec(start, end, thisObj, args, pr, regsN, superObj) {
    const regs = new Array(256).fill(undefined); regs[1] = thisObj; if (superObj !== undefined) regs[2] = superObj; pr.forEach((r, k) => { regs[r] = args[k]; });
    const st = []; let pc = start;
    const pop = () => { if (!st.length) throw new Error('AS stack underflow at ' + apt.text(ins[pc - 1], pool, data)); return st.pop(); };
    for (;;) {
      if (++counters.steps > 5e6) throw new Error('step limit');
      if (pc >= end) { counters.lastStack = st.length; return undefined; }
      const i = ins[pc]; pc++;
      switch (i.op) {
        case 0xb9: { const r = i.raw[0]; if (r >= regsN) throw new Error(`register r${r} >= ${regsN} declared`); st.push(regs[r]); break; }
        case 0x87: { const r = i.raw.readUInt32LE(0); if (r >= regsN) throw new Error(`store r${r} >= ${regsN} declared`); regs[r] = st[st.length - 1]; break; }
        case 0x17: pop(); break;
        case 0xa2: case 0xa3: st.push(P(i)); break;
        case 0x59: st.push(0); break; case 0x5a: st.push(1); break;
        case 0xb5: st.push(i.raw.readInt8(0)); break; case 0xb6: st.push(i.raw.readInt16LE(0)); break; case 0xb7: st.push(i.raw.readInt32LE(0)); break;
        case 0xb4: st.push(i.raw.readFloatLE(0)); break;
        case 0x73: st.push(true); break; case 0x74: st.push(false); break; case 0x76: st.push(undefined); break; case 0x75: st.push(null); break;
        case 0x71: st.push(G); break;
        case 0xae: st.push(G[P(i)]); break;
        case 0x4c: st.push(st[st.length - 1]); break;
        case 0x4e: { const m = pop(); const o = pop(); if (o === undefined || o === null) warn(`GetMember ${String(m)} on ${o}`); st.push(o === undefined || o === null ? undefined : o[m]); break; }
        case 0x4f: { const v = pop(); const m = pop(); const o = pop(); if (o === undefined || o === null) { warn(`SetMember ${String(m)} on ${o}`); break; } o[m] = v; break; }
        case 0xaf: { const o = pop(); if (o === undefined || o === null) warn(`GetNamedMember ${P(i)} on ${o}`); st.push(o === undefined || o === null ? undefined : o[P(i)]); break; }
        case 0xac: { const o = pop(); st.push(walk(o, [...i.raw.subarray(1)].map((k) => pool[k]), 'GetMemberPath')); break; }
        case 0xad: st.push(walk(G, [...i.raw.subarray(1)].map((k) => pool[k]), 'PushGlobalPath')); break;
        case 0x52: case 0x5d: case 0xb2: case 0xb3: {
          let name, o;
          if (i.op === 0x52 || i.op === 0x5d) { name = pop(); o = pop(); } else { name = P(i); o = pop(); }
          const n = pop(); const a = []; for (let k = 0; k < n; k++) a.push(pop());
          let r;
          if (o === undefined || o === null) warn(`call ${name} on ${o}`);
          else if (typeof o[name] !== 'function') warn(`call of non-function ${name}`);
          else r = (superObj !== undefined && o === superObj) ? o[name].apply(thisObj, a) : o[name](...a);   // a super call keeps this
          if (i.op === 0x52 || i.op === 0xb3) st.push(r);
          break;
        }
        case 0x53: { const name = pop(); const o = pop(); const n = pop(); const a = []; for (let k = 0; k < n; k++) a.push(pop()); const C = o && o[name]; if (typeof C !== 'function') { warn(`NewMethod ${name}`); st.push({}); } else st.push(new C(...a)); break; }
        case 0x40: { const cls = pop(); const n = pop(); for (let k = 0; k < n; k++) pop(); st.push(cls === 'Array' ? [] : {}); break; }
        case 0x42: { const n = pop(); const a = []; for (let k = 0; k < n; k++) a.push(pop()); st.push(a); break; }
        case 0x43: { const n = pop(); const o = {}; for (let k = 0; k < n; k++) { const v = pop(); const nm = pop(); o[nm] = v; } st.push(o); break; }
        case 0x49: { const b = pop(); const a = pop(); st.push(a == b); break; }   // eslint-disable-line eqeqeq
        case 0x66: { const b = pop(); const a = pop(); st.push(a === b); break; }
        case 0x48: { const b = pop(); const a = pop(); st.push(a < b); break; }
        case 0x67: { const b = pop(); const a = pop(); st.push(a > b); break; }
        case 0x47: { const b = pop(); const a = pop(); st.push(a + b); break; }
        case 0x0b: { const b = pop(); const a = pop(); st.push(a - b); break; }
        case 0x0c: { const b = pop(); const a = pop(); st.push(a * b); break; }
        case 0x12: st.push(!pop()); break;
        case 0x3a: { const nm = pop(); const o = pop(); if (o === undefined || o === null) { warn(`Delete ${String(nm)} on ${o}`); st.push(false); } else st.push(delete o[nm]); break; }
        case 0x3d: { const nm = pop(); const n = pop(); const a = []; for (let k = 0; k < n; k++) a.push(pop()); const f = G[nm]; if (typeof f !== 'function') { warn(`call of non-function ${nm}`); st.push(undefined); } else st.push(f(...a)); break; }
        case 0x50: st.push(pop() + 1); break;
        case 0x44: { const v = pop(); st.push(typeof v === 'function' ? 'function' : v === null ? 'null' : v === undefined ? 'undefined' : typeof v); break; }
        case 0x9d: { const v = pop(); if (v) pc = idx.get(i.target); break; }
        case 0xb8: { const v = pop(); if (!v) pc = idx.get(i.target); break; }
        case 0x99: pc = idx.get(i.target); break;
        case 0x3e: return pop();
        case 0x8e: { const f = closure(i); st.push(f); pc = idx.get(i.bodyEnd); break; }
        case 0x26: pop(); break;   // trace
        default: throw new Error(`op 0x${i.op.toString(16)} ${apt.text(i, pool, data)} not handled`);
      }
    }
  }
  // run a method of the class by name (its DefineFunction2 in the class script)
  function method(name) {
    const hits = []; for (let k = 0; k + 1 < ins.length; k++) { if (ins[k + 1].op !== 0x8e) continue; const t = apt.text(ins[k], pool, data); if (t === `PushConst ${JSON.stringify(name)}` || t === `PushConstW ${JSON.stringify(name)}`) hits.push(ins[k + 1]); }
    if (hits.length !== 1) throw new Error(`${name}: ${hits.length} definitions`);
    return closure(hits[0]);
  }
  // run the instructions from `from` up to (not including) `to` (both instruction objects of this class script) with the given this; returns the leftover stack depth
  function run(from, to, thisObj, regsN = 12) { counters.lastStack = -1; exec(idx.get(from), idx.get(to), thisObj, [], [], regsN); return counters.lastStack; }
  return { method, counters, run, idx };
}
function classOf(ast, name) { const s = apt.streamsOf(ast); const ia = apt.findClass(s.data, s.apt1, name); return { c: { s: apt.decode(s.data, ia.script), ia }, data: s.data }; }
module.exports = { vm, classOf };
