/* mstune.h: the TUNING binding of modsettings.dll (Mod Menu step 2, 2026-11-11; research: docs/design.md, section "Tuning binding, 11-11").
 * Included by modsettings.c after mstab.h. A manifest row { "bind": { "type": "tuning", "asset", "field", "instance", "copy", "values" | "scale" } }
 * writes ONE value of the game's loaded AttribSys tuning data, in memory only (the files are never touched: without this DLL the next launch
 * plays stock / the installed .fbmods' values).
 *
 * HOW THE GAME HOLDS TUNING (PROVEN statically on both dumps, 10-13 and 10-29; the pins come from tools/gen-pins.js tunePins):
 *   registry   every family getter (ballrules = 0x145d17770 on 10-29) builds "<asset>[_<name>]<style suffix>" and asks the lookup core
 *              (0x142e1dc10) with rcx = [registry global 0x14f3cfae8]. The core's hash find (0x142e1edc0): map = registry + 0x10, buckets
 *              [map + 8], bucket count u32 [map + 0x10], h = 5381, h = h * 33 ^ lower(c) per key byte, bucket h % count, node { char *key;
 *              void *value; node *next }. The value is the instance. This writer walks that map itself (read only; it never calls the game).
 *   instance   an Ebx object: vtable at +0, Asset.Name (char *) at +0x18 (LIKELY the asset path; checked when it is a readable string),
 *              the fields at the offsets of the class's reflection record.
 *   reflection TypeInfoData (section typeinfo): +0 name, +0xc flags (kind = flags >> 5 & 31: 2 struct, 3 class, 4 array, 8 enum, 10 bool,
 *              11..16 int8..uint32, 19 float32, 20 float64; size = flags >> 16), +0x32 u16 field count, class fields at +0x40 (base class:
 *              the runtime TypeInfo at +0x38), struct fields at +0x68, array element: the runtime TypeInfo at +0x38 (its first qword is the
 *              element's TypeInfoData). A field record (section fieldinf) is 32 bytes { char *name; u32 hash, flags; u32 offset (+0x10); runtime
 *              TypeInfo * (+0x18) }. A class-typed field (FloatCurve) holds a pointer; an array field holds a pointer to its first element,
 *              the count is the u32 before it with bit 31 cleared (the FloatCurve evaluator 0x14076d400 reads it that way).
 *   styles     "<asset>_gamestyle_simulation" is the Simulation copy, "<asset>" the base copy (gameplay_roll_fields: each style has its own
 *              scalars AND imports its own curve objects, so a curve point written through the Simulation instance is the Simulation curve).
 *              A family without style copies: the getters fall back to the bare asset, so "simulation" writes the base copy (logged).
 *
 * WHAT A ROW DOES. Option 0 is "As installed" (spec v3 rule 2): it writes nothing and puts back the value the game had loaded (captured the
 * first time the field was found). Any other option: its value (or the slider's scale) is clamped to the field's real type and the slider's
 * range, written, and read back. Two rows on one field (rule 1): the row changed last wins; when it goes back to "As installed" the next most
 * recent row's value applies, else the loaded value. A row that is hidden (visibleIf) or greyed (enabledIf, mod not active) counts as
 * "As installed". A field, asset or index that is not in the running game greys the row ("not available: ...") and is never written.
 * Nothing is written while the registry is not ready, or on a build without tuning pins.
 *
 * SAFETY. Read only pins (core head, the hash find's first 0x67 bytes = the map shape walked here, the two reflection sections). Every read
 * checks readable(); every write checks the page is committed read-write (never changes a protection), is one aligned store of the field's
 * size, and is read back. All of it runs on the worker thread under the fault guard: a fault switches the tuning writer off for the session.
 * Instances are looked up again on every pass: a different object (the game reloaded the asset) is resolved afresh (its loaded value
 * becomes "As installed") and gets the row's value again. Something else that keeps changing a written field: after 5 rewrites the target
 * is left alone and the row says so. */
#ifndef MS_TUNE_H
#define MS_TUNE_H
#include <math.h>
#include <float.h>
#define TU_MAXT 384
#define TU_KEY 112
#define TU_FIELD 128
#define TU_INST 96
#define TU_WHY 200
static char g_tu_state[24] = "not started", g_tu_reason[300];
static int g_tu_ready, g_tu_regready, g_tu_said_wait; static ULONGLONG g_tu_t0;
static long g_tu_writes, g_tu_write_fails, g_tu_passes; static char g_tu_last[400];
static size_t g_tu_img;   /* the image size (for the address checks) */
static uintptr_t g_tu_rd_lo, g_tu_rd_hi;   /* .rdata (as the image's own pointers name it): where the type names live */

/* ------------------------------------------------------------------ memory: the running game (or the self-test's mapped dump + fake heap) */
/* In the game the image is at its preferred base (ms_pins_check refuses otherwise), so pointers are used as they are. The self-test maps a
 * dump elsewhere: a pointer into the dump's image range is moved to where the dump is mapped. */
static uintptr_t tm_p(uintptr_t p) { uintptr_t hb = (uintptr_t)g_hdr_base; if ((uintptr_t)g_base != hb && p >= hb && p < hb + g_tu_img) return p - hb + (uintptr_t)g_base; return p; }
static int tm_in_image(uintptr_t p) { uintptr_t hb = (uintptr_t)g_hdr_base; return p >= hb && p < hb + g_tu_img; }
static int tm_rd(uintptr_t a, void *o, size_t n) { if (!a) return 0; void *q = (void *)tm_p(a); if (!readable(q, n)) return 0; memcpy(o, q, n); return 1; }
static uintptr_t tm_q(uintptr_t a) { uintptr_t v = 0; return tm_rd(a, &v, 8) ? v : 0; }
static uint32_t tm_u32(uintptr_t a) { uint32_t v = 0; return tm_rd(a, &v, 4) ? v : 0; }
/* a printable C string of at most n - 1 bytes; 0 = unreadable, not printable or too long */
static int tm_str(uintptr_t a, char *o, size_t n) {
  if (!a || n < 2) return 0; const unsigned char *q = (const unsigned char *)tm_p(a); size_t k = 0;
  while (k + 1 < n) {
    size_t room = 0x1000 - (((uintptr_t)q + k) & 0xfff), lim = n - 1 - k; if (room < lim) lim = room;
    if (!readable(q + k, lim)) { if (!readable(q + k, 1)) return 0; lim = 1; }
    for (size_t i = 0; i < lim; i++) { unsigned char c = q[k + i]; if (!c) { o[k + i] = 0; return 1; } if (c < 0x20 || c >= 0x7f) return 0; o[k + i] = (char)c; }
    k += lim;
  }
  o[n - 1] = 0; return 0;
}
static int tm_writable(const void *q, size_t n) {
  MEMORY_BASIC_INFORMATION mbi; if (VirtualQuery(q, &mbi, sizeof mbi) != sizeof mbi || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return 0;
  DWORD p = mbi.Protect & 0xff; if (p != PAGE_READWRITE && p != PAGE_EXECUTE_READWRITE) return 0;
  return (uintptr_t)q + n <= (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
}

/* ------------------------------------------------------------------ the pins (read only) */
static int tune_pins(char *why, size_t n) {
  const MsTunePin *t = &g_pb->tune; unsigned char *core = (unsigned char *)AT(t->core_rva), *find = (unsigned char *)AT(t->find_rva);
  if (!readable(core, 0x40) || memcmp(core, t->core_head, 24)) { snprintf(why, n, "the AttribSys lookup core at %p is not the verified code", (void *)core); return 0; }
  int32_t rel; memcpy(&rel, core + 0x33, 4);
  if (core[0x32] != 0xe8 || core + 0x37 + rel != find) { snprintf(why, n, "the lookup core does not call the pinned hash find"); return 0; }
  if (!readable(find, MS_TUNE_FINDN) || memcmp(find, t->find_head, MS_TUNE_FINDN)) { snprintf(why, n, "the registry's hash find is not the map shape this version reads"); return 0; }
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt); int ok = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
    if (!strncmp((const char *)sh[i].Name, "typeinfo", 8) && sh[i].VirtualAddress == t->ti_rva && sh[i].Misc.VirtualSize == t->ti_size) ok |= 1;
    if (!strncmp((const char *)sh[i].Name, "fieldinf", 8) && sh[i].VirtualAddress == t->fi_rva && sh[i].Misc.VirtualSize == t->fi_size) ok |= 2;
  }
  if (ok != 3) { snprintf(why, n, "the reflection sections (typeinfo, fieldinf) are not the pinned ones"); return 0; }
  g_tu_img = nt->OptionalHeader.SizeOfImage; g_tu_rd_lo = g_tu_rd_hi = 0;
  for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) if (!strncmp((const char *)sh[i].Name, ".rdata", 8)) { g_tu_rd_lo = (uintptr_t)g_hdr_base + sh[i].VirtualAddress; g_tu_rd_hi = g_tu_rd_lo + sh[i].Misc.VirtualSize; break; }
  if (!g_tu_rd_lo || !readable((void *)tm_p(g_tu_rd_lo), g_tu_rd_hi - g_tu_rd_lo) || !readable(AT(t->ti_rva), t->ti_size)) { snprintf(why, n, "the .rdata / typeinfo sections are not readable"); return 0; }
  return 1;
}
static int tm_in_sec(uintptr_t p, uint32_t rva, uint32_t size) { uintptr_t a = (uintptr_t)g_hdr_base + rva; return p >= a && p < a + size; }

/* ------------------------------------------------------------------ reflection */
enum { TK_STRUCT = 2, TK_CLASS = 3, TK_ARRAY = 4, TK_ENUM = 8, TK_BOOL = 10, TK_I8, TK_U8, TK_I16, TK_U16, TK_I32, TK_U32, TK_I64, TK_U64, TK_F32, TK_F64 };
typedef struct { uintptr_t tid; char name[64]; int kind; uint32_t size; int nfields; uintptr_t fields, base, elem; } TmType;
static int tm_type(uintptr_t tid, TmType *o) {
  memset(o, 0, sizeof *o); if (!tid || !tm_in_sec(tid, g_pb->tune.ti_rva, g_pb->tune.ti_size)) return 0;
  uint32_t fl = tm_u32(tid + 0xc); o->tid = tid; o->kind = (int)((fl >> 5) & 31); o->size = fl >> 16;
  if (!tm_str(tm_q(tid), o->name, sizeof o->name)) snprintf(o->name, sizeof o->name, "?");
  uint16_t nf = 0; tm_rd(tid + 0x32, &nf, 2);
  if (o->kind == TK_CLASS || o->kind == TK_STRUCT) {
    o->nfields = nf; o->fields = tm_q(tid + (o->kind == TK_CLASS ? 0x40 : 0x68));
    if (o->nfields && !tm_in_sec(o->fields, g_pb->tune.fi_rva, g_pb->tune.fi_size)) { o->nfields = 0; o->fields = 0; }
    if (o->kind == TK_CLASS) { uintptr_t rt = tm_q(tid + 0x38); o->base = rt && tm_in_image(rt) ? tm_q(rt) : 0; }
  } else if (o->kind == TK_ARRAY) { uintptr_t rt = tm_q(tid + 0x38); o->elem = rt && tm_in_image(rt) ? tm_q(rt) : 0; }
  return 1;
}
/* a field of a class (its bases too) or struct, by name (case-insensitive): its offset and type */
static int tm_field(const TmType *t, const char *name, uint32_t *off, TmType *ft) {
  TmType cur = *t;
  for (int depth = 0; depth < 8; depth++) {
    for (int i = 0; i < cur.nfields && i < 4096; i++) {
      uintptr_t rec = cur.fields + 32 * (uintptr_t)i; char nm[160];
      if (!tm_str(tm_q(rec), nm, sizeof nm) || _stricmp(nm, name)) continue;
      *off = tm_u32(rec + 0x10); uintptr_t rt = tm_q(rec + 0x18);
      return rt && tm_in_image(rt) && tm_type(tm_q(rt), ft);
    }
    if (cur.kind != TK_CLASS || !cur.base || !tm_type(cur.base, &cur)) break;
  }
  return 0;
}
/* the class TypeInfoData named name (case-insensitive), from a scan of the typeinfo section (cached) */
typedef struct { char name[64]; uintptr_t tid[4]; int n; } TmCls;
static TmCls g_tcls[96]; static int g_ntcls;
static const TmCls *tm_class(const char *name) {
  for (int i = 0; i < g_ntcls; i++) if (!_stricmp(g_tcls[i].name, name)) return &g_tcls[i];
  if (g_ntcls >= (int)(sizeof g_tcls / sizeof *g_tcls)) return NULL;
  TmCls *c = &g_tcls[g_ntcls++]; memset(c, 0, sizeof *c); snprintf(c->name, sizeof c->name, "%s", name);
  const unsigned char *sec = (const unsigned char *)AT(g_pb->tune.ti_rva); uint32_t size = g_pb->tune.ti_size; size_t nl = strlen(name);
  for (uint32_t o = 0; o + 0x48 <= size && c->n < 4; o += 8) {
    uint64_t q; memcpy(&q, sec + o, 8); if (q < g_tu_rd_lo || q + nl + 1 > g_tu_rd_hi) continue;   /* .rdata was checked readable whole */
    const unsigned char *s = (const unsigned char *)tm_p((uintptr_t)q);
    if (((s[0] | 0x20) != (name[0] | 0x20)) || _strnicmp((const char *)s, name, nl) || s[nl]) continue;
    uint32_t fl; memcpy(&fl, sec + o + 0xc, 4); if (((fl >> 5) & 31) != TK_CLASS) continue;
    c->tid[c->n++] = (uintptr_t)g_hdr_base + g_pb->tune.ti_rva + o;
  }
  return c;
}

/* ------------------------------------------------------------------ the registry */
static uint32_t tm_hash(const char *k) { uint32_t h = 5381; for (const unsigned char *p = (const unsigned char *)k; *p; p++) { uint32_t c = *p; if (c - 'A' <= 25u) c += 0x20; h = (h * 33u) ^ c; } return h; }
/* 1 = the registry looks filled (its bucket array and count are sane); *buckets / *count out */
static int tm_registry(uintptr_t *buckets, uint32_t *count) {
  uintptr_t reg = tm_q((uintptr_t)g_hdr_base + g_pb->tune.reg_rva); if (!reg) return 0;
  *buckets = tm_q(reg + 0x18); *count = tm_u32(reg + 0x20);
  return *buckets && *count && *count < (1u << 24) && readable((void *)tm_p(*buckets), (size_t)*count * 8);
}
static uintptr_t tm_lookup(const char *key) {
  uintptr_t b; uint32_t n; if (!tm_registry(&b, &n)) return 0;
  uintptr_t node = tm_q(b + 8 * (uintptr_t)(tm_hash(key) % n));
  for (int g = 0; node && g < 4096; g++) { char k[TU_KEY + 16]; if (tm_str(tm_q(node), k, sizeof k) && !_stricmp(k, key)) return tm_q(node + 8); node = tm_q(node + 0x10); }
  return 0;
}

/* ------------------------------------------------------------------ values: clamp to the field's type, encode, decode */
static int tm_prim(int k) { return k == TK_BOOL || (k >= TK_I8 && k <= TK_U32) || k == TK_F32 || k == TK_F64; }
static const char *tm_kindname(int k) { static const char *const N[] = { "bool", "int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float32", "float64" }; return k >= TK_BOOL && k <= TK_F64 ? N[k - TK_BOOL] : k == TK_CLASS ? "object" : k == TK_ARRAY ? "array" : k == TK_STRUCT ? "struct" : k == TK_ENUM ? "enum" : "?"; }
/* the bytes for v in a field of kind k (size sz); *got = the value actually written (after clamping / rounding) */
static void tm_encode(int k, double v, unsigned char o[8], double *got) {
  memset(o, 0, 8); double lo = 0, hi = 0; int is_int = 1;
  switch (k) {
    case TK_BOOL: o[0] = v != 0; *got = o[0]; return;
    case TK_I8: lo = -128; hi = 127; break; case TK_U8: lo = 0; hi = 255; break; case TK_I16: lo = -32768; hi = 32767; break; case TK_U16: lo = 0; hi = 65535; break;
    case TK_I32: lo = -2147483648.0; hi = 2147483647.0; break; case TK_U32: lo = 0; hi = 4294967295.0; break;
    default: is_int = 0;
  }
  if (is_int) { double r = floor(v + 0.5); if (r < lo) r = lo; if (r > hi) r = hi; *got = r; long long x = (long long)r; memcpy(o, &x, 8); return; }   /* little endian: the low bytes */
  if (k == TK_F32) { double c = v > FLT_MAX ? FLT_MAX : v < -FLT_MAX ? -FLT_MAX : v; float f = (float)c; memcpy(o, &f, 4); *got = f; return; }
  memcpy(o, &v, 8); *got = v;
}
static double tm_decode(int k, const unsigned char *b) {
  switch (k) {
    case TK_BOOL: return b[0] != 0; case TK_I8: return (int8_t)b[0]; case TK_U8: return b[0];
    case TK_I16: { int16_t x; memcpy(&x, b, 2); return x; } case TK_U16: { uint16_t x; memcpy(&x, b, 2); return x; }
    case TK_I32: { int32_t x; memcpy(&x, b, 4); return x; } case TK_U32: { uint32_t x; memcpy(&x, b, 4); return x; }
    case TK_F32: { float f; memcpy(&f, b, 4); return f; } case TK_F64: { double d; memcpy(&d, b, 8); return d; }
  }
  return 0;
}
static void tm_fmt(int k, const unsigned char *b, char *o, size_t n) { double v = tm_decode(k, b); if (k == TK_BOOL) snprintf(o, n, "%s", v ? "true" : "false"); else if (k == TK_F32 || k == TK_F64) snprintf(o, n, "%.6g", v); else snprintf(o, n, "%.0f", v); }

/* ------------------------------------------------------------------ targets (one field value of one instance) and rows */
typedef struct {
  char key[TU_KEY], cls[64], field[TU_FIELD], inst[TU_INST]; int state;   /* 0 not looked up, 1 found, 2 not available */
  char why[TU_WHY]; uintptr_t obj, addr; int kind; uint32_t size; char tname[64];
  unsigned char orig[8], last[8]; int have_last, rewrites, stopped, owner;   /* owner: the g_tr index whose value is in memory (-1 = the loaded value) */
} TuTarget;
typedef struct { char mod[MS_MID], id[MS_ID], sig[TU_KEY + TU_FIELD + TU_INST + 16]; int opt, state, nt, t[2], fellback, seen; long long stamp; char why[TU_WHY]; } TuRow;
static TuTarget g_tt[TU_MAXT]; static int g_ntt;
static TuRow g_tr[MS_MAXROWS]; static int g_ntr; static long long g_tu_stamp;
/* a row as copied out of the configuration (no lock is held while game memory is read or written) */
typedef struct { char mod[MS_MID], id[MS_ID], asset[TU_KEY], field[TU_FIELD], inst[TU_INST], label[48]; int idx, copy, type, opt, live, tkind; double tv[MS_MAXLIST], smin, smax, sstep, slo, shi; } TuIn;

/* the asset key + its class name from the manifest's "asset": a path's last part is the asset, the folder above it the class
 * ("football/gameplay/attribsys/data/ballrules/ballrules/ballrules"); a short name is both (a known variant maps to its class); a style
 * file's name ("ballrules_gamestyle_simulation") is cut back to its base asset (copy decides the style) */
static void tu_asset(const char *in, char *key, size_t kn, char *cls, size_t cn, int *cut) {
  char buf[TU_KEY * 2]; snprintf(buf, sizeof buf, "%s", in); for (char *p = buf; *p; p++) if (*p == '\\') *p = '/';
  size_t L = strlen(buf); while (L && buf[L - 1] == '/') buf[--L] = 0;
  char *leaf = strrchr(buf, '/'); char folder[64] = "";
  if (leaf) { *leaf++ = 0; char *f = strrchr(buf, '/'); snprintf(folder, sizeof folder, "%s", f ? f + 1 : buf); } else leaf = buf;
  snprintf(key, kn, "%s", leaf); *cut = 0;
  static const char *const STY[] = { "_gamestyle_simulation", "_gamestyle_competitive", "_gamestyle_arcade", "_gamestyle_yard" };
  for (int i = 0; i < 4; i++) { size_t a = strlen(key), b = strlen(STY[i]); if (a > b && !_stricmp(key + a - b, STY[i])) { key[a - b] = 0; *cut = 1; break; } }
  static const char *const ALIAS[][2] = { { "collegerules", "footballrules" }, { "fix", "fun" } };
  if (folder[0]) { snprintf(cls, cn, "%s", folder); return; }
  for (size_t i = 0; i < sizeof ALIAS / sizeof *ALIAS; i++) if (!_stricmp(key, ALIAS[i][0])) { snprintf(cls, cn, "%s", ALIAS[i][1]); return; }
  snprintf(cls, cn, "%s", key);
}
/* the class of an asset: its name, else the name cut back at '_' ("fatiguemanager_small_team" -> "fatiguemanager") */
static int tu_class_tid(const char *cls, const char *field, uintptr_t *tid, char *found, size_t fn) {
  char c[64]; snprintf(c, sizeof c, "%s", cls);
  for (;;) {
    const TmCls *k = tm_class(c);
    for (int i = 0; k && i < k->n; i++) { TmType t, ft; uint32_t off; if (tm_type(k->tid[i], &t) && tm_field(&t, field, &off, &ft)) { *tid = k->tid[i]; snprintf(found, fn, "%s", c); return 1; } }
    if (k && k->n) { *tid = k->tid[0]; snprintf(found, fn, "%s", c); return 2; }   /* the class exists, the field does not */
    char *u = strrchr(c, '_'); if (!u) return 0; *u = 0;
  }
}
/* walk "instance" from the field: "Points[1].Y", "[2]", "Curve.Points[0].x" ("Curve." is optional on a curve field) */
static int tu_walk(uintptr_t addr, TmType cur, const char *path, uintptr_t *out, TmType *leaf, char *why, size_t wn) {
  const char *p = path ? path : "";
  while (*p) {
    if (*p == '.') { p++; continue; }
    if (*p == '[') {
      char *e; long long i = strtoll(p + 1, &e, 10); if (e == p + 1 || *e != ']' || i < 0) { snprintf(why, wn, "\"%s\": a bad index", path); return 0; }
      p = e + 1;
      if (cur.kind != TK_ARRAY) { snprintf(why, wn, "\"%s\": %s is not a list", path, cur.name); return 0; }
      uintptr_t data = tm_q(addr); if (!data) { snprintf(why, wn, "\"%s\": the list is empty", path); return 0; }
      uint32_t cnt = tm_u32(data - 4) & 0x7fffffffu; if ((unsigned long long)i >= cnt) { snprintf(why, wn, "\"%s\": index %lld is past the end (the game has %u)", path, i, cnt); return 0; }
      TmType el; if (!tm_type(cur.elem, &el)) { snprintf(why, wn, "\"%s\": unknown element type", path); return 0; }
      uint32_t stride = el.kind == TK_CLASS ? 8 : el.size; if (!stride) { snprintf(why, wn, "\"%s\": element size 0", path); return 0; }
      addr = data + (uintptr_t)i * stride; cur = el; continue;
    }
    char nm[64]; size_t k = 0; while (*p && *p != '.' && *p != '[' && k + 1 < sizeof nm) nm[k++] = *p++; nm[k] = 0;
    if (cur.kind != TK_CLASS && cur.kind != TK_STRUCT) { snprintf(why, wn, "\"%s\": %s has no part \"%s\"", path, cur.name, nm); return 0; }
    uintptr_t base = addr;
    if (cur.kind == TK_CLASS) { base = tm_q(addr) & ~(uintptr_t)7; if (!base) { snprintf(why, wn, "\"%s\": the %s points to nothing", path, cur.name); return 0; } }
    uint32_t off; TmType ft;
    if (!tm_field(&cur, nm, &off, &ft)) { if (!_stricmp(nm, "Curve")) continue; snprintf(why, wn, "\"%s\": %s has no field \"%s\"", path, cur.name, nm); return 0; }
    addr = base + off; cur = ft;
  }
  if (!tm_prim(cur.kind)) {
    if (cur.kind == TK_CLASS) snprintf(why, wn, "the field is a %s: name one value inside it with \"instance\" (e.g. \"Points[1].Y\")", cur.name);
    else if (cur.kind == TK_ARRAY) snprintf(why, wn, "the field is a list (%s): name one element with \"instance\" (e.g. \"[0]\")", cur.name);
    else snprintf(why, wn, "the value is of type %s (%s): only numbers and true/false can be set", cur.name, tm_kindname(cur.kind));
    return 0;
  }
  *out = addr; *leaf = cur; return 1;
}
/* look a target up (again): 1 found (orig captured when it is new or its instance moved), 0 not available (why), -1 registry not ready */
static int tu_resolve(TuTarget *t) {
  uintptr_t b; uint32_t n; if (!tm_registry(&b, &n)) return -1;
  uintptr_t obj = tm_lookup(t->key);
  if (!obj) { snprintf(t->why, sizeof t->why, "asset \"%s\" is not in the running game", t->key); return 0; }
  if (t->state == 1 && obj == t->obj) return 1;   /* the same instance as before */
  int moved = t->state == 1;
  if (!readable((void *)tm_p(obj), 0x20) || !tm_in_image(tm_q(obj))) { snprintf(t->why, sizeof t->why, "asset \"%s\": its instance is not readable", t->key); return 0; }
  char nm[300]; uintptr_t np = tm_q(obj + 0x18);
  if (np && tm_str(np, nm, sizeof nm)) { const char *leaf = strrchr(nm, '/'); leaf = leaf ? leaf + 1 : nm; if (_stricmp(leaf, t->key)) { snprintf(t->why, sizeof t->why, "asset \"%s\": the registry's instance is named \"%s\"", t->key, nm); return 0; } }
  uintptr_t tid; char found[64]; int r = tu_class_tid(t->cls, t->field, &tid, found, sizeof found);
  if (!r) { snprintf(t->why, sizeof t->why, "no tuning class \"%s\" in this game build", t->cls); return 0; }
  TmType ct; uint32_t off; TmType ft;
  if (r == 2 || !tm_type(tid, &ct) || !tm_field(&ct, t->field, &off, &ft)) { snprintf(t->why, sizeof t->why, "field \"%s\" is not in %s on this game build", t->field, found); return 0; }
  TmType leaf; uintptr_t addr; char why[TU_WHY];
  if (!tu_walk(obj + off, ft, t->inst, &addr, &leaf, why, sizeof why)) { snprintf(t->why, sizeof t->why, "%s.%s: %s", found, t->field, why); return 0; }
  if (leaf.size < 1 || leaf.size > 8 || (addr & (leaf.size - 1))) { snprintf(t->why, sizeof t->why, "%s.%s: an unaligned %s", found, t->field, leaf.name); return 0; }
  unsigned char cur[8] = { 0 }; if (!tm_rd(addr, cur, leaf.size)) { snprintf(t->why, sizeof t->why, "%s.%s: not readable", found, t->field); return 0; }
  t->obj = obj; t->addr = addr; t->kind = leaf.kind; t->size = leaf.size; snprintf(t->tname, sizeof t->tname, "%s", leaf.name);
  memcpy(t->orig, cur, 8); t->have_last = 0; t->rewrites = 0; t->stopped = 0; t->owner = -1; t->why[0] = 0;
  char v[48]; tm_fmt(t->kind, t->orig, v, sizeof v);
  if (moved || g_ku.log_reads) logf_("modsettings: tuning: %s %s.%s%s%s (%s at %p): as installed = %s%s", moved ? "instance changed, found again:" : "found", t->key, t->field, t->inst[0] ? "." : "", t->inst, t->tname, (void *)addr, v, moved ? " (the row's value is applied again)" : "");
  return 1;
}
static int tu_target(const char *key, const char *cls, const char *field, const char *inst) {
  for (int i = 0; i < g_ntt; i++) if (!_stricmp(g_tt[i].key, key) && !_stricmp(g_tt[i].field, field) && !_stricmp(g_tt[i].inst, inst)) return i;
  if (g_ntt >= TU_MAXT) return -1;
  TuTarget *t = &g_tt[g_ntt]; memset(t, 0, sizeof *t); t->owner = -1;
  snprintf(t->key, sizeof t->key, "%s", key); snprintf(t->cls, sizeof t->cls, "%s", cls); snprintf(t->field, sizeof t->field, "%s", field); snprintf(t->inst, sizeof t->inst, "%s", inst);
  return g_ntt++;
}
static TuRow *tu_row(const TuIn *in, int *fresh) {
  char sig[sizeof g_tr[0].sig]; snprintf(sig, sizeof sig, "%s|%s|%s|%d", in->asset, in->field, in->inst, in->copy);
  for (int i = 0; i < g_ntr; i++) if (!strcmp(g_tr[i].mod, in->mod) && !strcmp(g_tr[i].id, in->id)) {
    if (strcmp(g_tr[i].sig, sig)) { snprintf(g_tr[i].sig, sizeof g_tr[i].sig, "%s", sig); g_tr[i].state = 0; g_tr[i].nt = 0; g_tr[i].opt = -1; *fresh = 1; } else *fresh = 0;
    return &g_tr[i];
  }
  if (g_ntr >= MS_MAXROWS) return NULL;
  TuRow *r = &g_tr[g_ntr++]; memset(r, 0, sizeof *r); snprintf(r->mod, sizeof r->mod, "%s", in->mod); snprintf(r->id, sizeof r->id, "%s", in->id); snprintf(r->sig, sizeof r->sig, "%s", sig); r->opt = -1; *fresh = 1;
  return r;
}
/* the row's targets: 0 waiting (registry not ready), 1 found, 2 not available */
static int tu_row_resolve(TuRow *r, const TuIn *in) {
  if (in->tkind == 2) { snprintf(r->why, sizeof r->why, "text values are not supported for tuning fields (use numbers or true/false)"); return 2; }
  char key[TU_KEY], cls[64]; int cut; tu_asset(in->asset, key, sizeof key, cls, sizeof cls, &cut);
  if (cut && r->state == 0) logf_("modsettings: tuning: %s/%s: asset \"%s\" names a style file: using \"%s\" (\"copy\" picks the style)", in->mod, in->id, in->asset, key);
  char sim[TU_KEY + 24]; snprintf(sim, sizeof sim, "%s_gamestyle_simulation", key);
  const char *keys[2]; int nk = 0, fell = 0;
  if (in->copy & 1) { if (tm_lookup(sim)) keys[nk++] = sim; else { fell = tm_lookup(key) != 0; if (!(in->copy & 2)) keys[nk++] = key; } }
  if (in->copy & 2) keys[nk++] = key;
  if (fell && !r->fellback) logf_("modsettings: tuning: %s/%s: \"%s\" has no Simulation copy: the base copy is written (every style reads it)", in->mod, in->id, key);
  r->fellback = fell; r->nt = 0;
  for (int k = 0; k < nk; k++) {
    int ti = tu_target(keys[k], cls, in->field, in->inst); if (ti < 0) { snprintf(r->why, sizeof r->why, "too many tuning fields (the most is %d)", TU_MAXT); return 2; }
    TuTarget *t = &g_tt[ti]; int s = tu_resolve(t);
    if (s < 0) return 0;
    if (s == 0) { t->state = 2; snprintf(r->why, sizeof r->why, "%s", t->why); return 2; }
    t->state = 1; r->t[r->nt++] = ti;
  }
  return 1;
}
/* the value option opt (> 0) of the row asks for */
static double tu_value(const TuIn *in, int opt) {
  int k = opt - 1;
  if (in->type != RT_SLIDER) return in->tv[k < 0 ? 0 : k >= MS_MAXLIST ? MS_MAXLIST - 1 : k];
  double v = in->smin + (double)k * in->sstep, f = in->smax > in->smin ? (v - in->smin) / (in->smax - in->smin) : 0;
  if (f < 0) f = 0; if (f > 1) f = 1;
  double x = in->slo + f * (in->shi - in->slo), lo = in->slo < in->shi ? in->slo : in->shi, hi = in->slo < in->shi ? in->shi : in->slo;
  return x < lo ? lo : x > hi ? hi : x;   /* the slider's range */
}

/* ------------------------------------------------------------------ the pass (worker thread) */
typedef struct { TuIn *in; int n, marks; } TuPass;
static void tu_pass_body(void *arg) {
  TuPass *P = (TuPass *)arg; ULONGLONG now = GetTickCount64();
  uintptr_t b; uint32_t nb; int ready = tm_registry(&b, &nb) && tm_lookup("ballrules") != 0;
  if (ready && !g_tu_regready) { g_tu_regready = 1; logf_("modsettings: tuning: the game's tuning registry is ready (%u buckets)", nb); snprintf(g_tu_state, sizeof g_tu_state, "running"); g_tu_reason[0] = 0; }
  if (!ready) {
    if (!g_tu_said_wait) { g_tu_said_wait = 1; logf_("modsettings: tuning: waiting for the game's tuning registry"); snprintf(g_tu_state, sizeof g_tu_state, "waiting"); }
    if (!g_tu_regready && now - g_tu_t0 > 20ull * 60000ull && strcmp(g_tu_state, "failed")) { snprintf(g_tu_state, sizeof g_tu_state, "failed"); snprintf(g_tu_reason, sizeof g_tu_reason, "the game's tuning registry was not found in 20 minutes"); logf_("modsettings: tuning: %s: tuning rows are not written", g_tu_reason); }
    return;
  }
  for (int i = 0; i < g_ntr; i++) g_tr[i].seen = 0;
  /* 1. rows: find their targets (new or changed binds; instances looked up again), stamp their changes */
  int idx[MS_MAXROWS];
  for (int i = 0; i < P->n; i++) {
    const TuIn *in = &P->in[i]; int fresh; TuRow *r = tu_row(in, &fresh); idx[i] = r ? (int)(r - g_tr) : -1; if (!r) continue; r->seen = 1;
    if (r->state == 0 || r->state == 1) {
      int s = tu_row_resolve(r, in);
      if (s == 2 && r->state != 2) { logf_("modsettings: tuning: %s/%s (%s : %s%s%s) is NOT AVAILABLE: %s; it is greyed and never written", in->mod, in->id, in->asset, in->field, in->inst[0] ? " " : "", in->inst, r->why); P->marks = 1; }
      r->state = s == 2 ? 2 : s == 1 ? 1 : r->state;
    }
    if (r->state != 1) continue;
    int opt = in->live ? in->opt : 0;
    if (opt != r->opt) { r->stamp = ++g_tu_stamp; if (r->opt >= 0 || opt) logf_("modsettings: tuning: %s/%s = %s%s", in->mod, in->id, opt ? in->label : "As installed", in->live || !in->opt ? "" : " (row greyed or hidden: counts as As installed)"); r->opt = opt; }
  }
  /* 2. targets: the newest change among the rows on it wins, else the loaded value */
  for (int ti = 0; ti < g_ntt; ti++) {
    TuTarget *t = &g_tt[ti]; if (t->state != 1 || t->stopped) continue;
    int win = -1, wi = -1; long long best = -1;
    for (int i = 0; i < P->n; i++) { int ri = idx[i]; if (ri < 0) continue; TuRow *r = &g_tr[ri]; if (r->state != 1 || r->opt <= 0) continue;
      int on = 0; for (int k = 0; k < r->nt; k++) if (r->t[k] == ti) on = 1; if (on && r->stamp > best) { best = r->stamp; win = ri; wi = i; } }
    unsigned char want[8]; double asked = 0, got = 0;
    if (win >= 0) { asked = tu_value(&P->in[wi], g_tr[win].opt); tm_encode(t->kind, asked, want, &got); } else memcpy(want, t->orig, 8);
    unsigned char cur[8] = { 0 }; if (!tm_rd(t->addr, cur, t->size)) { t->state = 0; continue; }
    if (!memcmp(cur, want, t->size)) { t->owner = win; continue; }
    if (t->have_last && memcmp(cur, t->last, t->size) && memcmp(cur, t->orig, t->size)) {
      char cv[48]; tm_fmt(t->kind, cur, cv, sizeof cv);
      if (++t->rewrites > 5) { t->stopped = 1; logf_("modsettings: tuning: %s.%s%s%s keeps being changed by something else (now %s): left alone from now on", t->key, t->field, t->inst[0] ? "." : "", t->inst, cv); continue; }
      logf_("modsettings: tuning: %s.%s%s%s was changed by something else (%s); written again", t->key, t->field, t->inst[0] ? "." : "", t->inst, cv);
    }
    void *q = (void *)tm_p(t->addr); char ov[48], nv[48], rb[48]; tm_fmt(t->kind, cur, ov, sizeof ov); tm_fmt(t->kind, want, nv, sizeof nv);
    if (!tm_writable(q, t->size)) { g_tu_write_fails++; snprintf(g_tu_last, sizeof g_tu_last, "%s.%s: the page is not writable: not written", t->key, t->field); logf_("modsettings: tuning: %s", g_tu_last); t->stopped = 1; continue; }
    switch (t->size) { case 1: *(volatile uint8_t *)q = want[0]; break; case 2: { uint16_t x; memcpy(&x, want, 2); *(volatile uint16_t *)q = x; } break;
      case 4: { uint32_t x; memcpy(&x, want, 4); *(volatile uint32_t *)q = x; } break; default: { uint64_t x; memcpy(&x, want, 8); *(volatile uint64_t *)q = x; } }
    unsigned char back[8] = { 0 }; tm_rd(t->addr, back, t->size); tm_fmt(t->kind, back, rb, sizeof rb); int ok = !memcmp(back, want, t->size);
    memcpy(t->last, want, 8); t->have_last = 1; t->owner = win; if (ok) g_tu_writes++; else g_tu_write_fails++;
    char clamp[80] = ""; if (win >= 0 && got != asked) snprintf(clamp, sizeof clamp, " (asked %.6g, clamped to the %s)", asked, tm_kindname(t->kind));
    snprintf(g_tu_last, sizeof g_tu_last, "%s %s.%s%s%s %s: %s -> %s%s, read back %s%s", win >= 0 ? g_tr[win].mod : "(as installed)", t->key, t->field, t->inst[0] ? "." : "", t->inst, t->tname, ov, nv, clamp, rb, ok ? " ok" : " DIFFERENT");
    if (win >= 0) { size_t L = strlen(g_tu_last); snprintf(g_tu_last + L, sizeof g_tu_last - L, " [%s/%s]", g_tr[win].mod, g_tr[win].id); }
    logf_("modsettings: tuning: WRITE %s", g_tu_last);
  }
  g_tu_passes++;
}
/* a configuration's rows greyed by the tuning writer (a fresh build: before it is published; the live one: under the exclusive lock) */
static void tune_mark(MsCfg *c) {
  for (int i = 0; c && i < c->nrows; i++) {
    MsRow *r = &c->rows[i]; if (r->bind != BD_TUNING || (r->unavail && !r->tu_marked)) continue;
    TuRow *t = NULL; for (int k = 0; k < g_ntr; k++) if (!strcmp(g_tr[k].mod, r->mod) && !strcmp(g_tr[k].id, r->id)) { t = &g_tr[k]; break; }
    if (t && t->state == 2) { if (!r->tu_marked) { r->unavail = 1; r->tu_marked = 1; r->s_unavail = pool_put(c, t->why); } }
    else if (r->tu_marked) { r->unavail = 0; r->tu_marked = 0; r->s_unavail = -1; }
  }
}
static int tune_install(void) {
  if (!g_ku.on) { snprintf(g_tu_state, sizeof g_tu_state, "off"); snprintf(g_tu_reason, sizeof g_tu_reason, "tuning = 0 in " MS_INI); logf_("modsettings: tuning = 0: tuning rows are kept but never written"); return 1; }
  char why[300];
  if (!ms_pins_check(why, sizeof why) || !tune_pins(why, sizeof why)) { snprintf(g_tu_state, sizeof g_tu_state, "refused"); snprintf(g_tu_reason, sizeof g_tu_reason, "%s", why); logf_("modsettings: tuning: REFUSED: %s (tuning rows are kept, never written)", g_tu_reason); return 1; }
  g_tu_ready = 1; g_tu_t0 = GetTickCount64(); snprintf(g_tu_state, sizeof g_tu_state, "waiting");
  logf_("modsettings: tuning: pins ok (%s: registry global %p, lookup core %p, typeinfo + fieldinf sections): tuning rows are written once the registry is filled", g_pb->name, AT(g_pb->tune.reg_rva), AT(g_pb->tune.core_rva));
  return 1;
}
/* one pass: copy the tuning rows out of the configuration, then work without the lock */
static void tune_pass(void) {
  if (!g_tu_ready || g_tu_off) return;
  TuIn *in = (TuIn *)calloc(MS_MAXROWS, sizeof *in); if (!in) return; int n = 0;
  AcquireSRWLockShared(&g_cfg_lock);
  const MsCfg *c = g_cfg;
  for (int i = 0; c && i < c->nrows && n < MS_MAXROWS; i++) {
    const MsRow *r = &c->rows[i]; if (r->bind != BD_TUNING || (r->unavail && !r->tu_marked)) continue;   /* rules 6 / 7: never written */
    TuIn *o = &in[n++]; snprintf(o->mod, sizeof o->mod, "%s", r->mod); snprintf(o->id, sizeof o->id, "%s", r->id);
    snprintf(o->asset, sizeof o->asset, "%s", pool_str(c, r->s_asset)); snprintf(o->field, sizeof o->field, "%s", pool_str(c, r->s_field)); snprintf(o->inst, sizeof o->inst, "%s", r->s_inst >= 0 ? pool_str(c, r->s_inst) : "");
    o->idx = i; o->copy = r->copy; o->type = r->type; o->opt = c->gval[i]; o->live = row_live(c, i) && row_visible(c, i); o->tkind = r->tkind;
    memcpy(o->tv, r->tv, sizeof o->tv); o->smin = r->smin; o->smax = r->smax; o->sstep = r->sstep; o->slo = r->slo; o->shi = r->shi;
    if (o->opt > 0) opt_label(c, r, o->opt, o->label, sizeof o->label);
  }
  ReleaseSRWLockShared(&g_cfg_lock);
  TuPass P = { in, n, 0 };
  ms_guarded2(tu_pass_body, &P, "the tuning writer", &g_tu_off);
  if (P.marks) { AcquireSRWLockExclusive(&g_cfg_lock); tune_mark(g_cfg); ReleaseSRWLockExclusive(&g_cfg_lock); }
  free(in);
  InterlockedExchange(&g_status_dirty, 1);
}
/* the status file's "tuning" block */
static void tune_status(Sb *s) {
  sb_add(s, ",\"tuning\":{\"state\":"); sb_q(s, g_tu_state); sb_add(s, ",\"reason\":"); sb_q(s, g_tu_reason);
  sb_add(s, ",\"registry_ready\":%s,\"off_after_fault\":%ld,\"passes\":%ld,\"writes\":%ld,\"write_fails\":%ld,\"last\":", g_tu_regready ? "true" : "false", (long)g_tu_off, g_tu_passes, g_tu_writes, g_tu_write_fails); sb_q(s, g_tu_last);
  sb_add(s, ",\"rows\":[");
  for (int i = 0; i < g_ntr; i++) {
    const TuRow *r = &g_tr[i]; sb_add(s, "%s{\"mod\":", i ? "," : ""); sb_q(s, r->mod); sb_add(s, ",\"row\":"); sb_q(s, r->id);
    sb_add(s, ",\"state\":\"%s\",\"option\":%d", r->state == 1 ? "found" : r->state == 2 ? "not available" : "waiting", r->opt); if (r->state == 2) { sb_add(s, ",\"why\":"); sb_q(s, r->why); }
    if (r->fellback) sb_add(s, ",\"simulation_is_base\":true");
    sb_add(s, ",\"targets\":[");
    for (int k = 0; k < r->nt; k++) {
      const TuTarget *t = &g_tt[r->t[k]]; char o[48], c[48] = "?"; unsigned char cur[8] = { 0 }; tm_fmt(t->kind, t->orig, o, sizeof o); if (tm_rd(t->addr, cur, t->size)) tm_fmt(t->kind, cur, c, sizeof c);
      sb_add(s, "%s{\"asset\":", k ? "," : ""); sb_q(s, t->key); sb_add(s, ",\"field\":"); sb_q(s, t->field); sb_add(s, ",\"instance\":"); sb_q(s, t->inst);
      sb_add(s, ",\"type\":"); sb_q(s, t->tname); sb_add(s, ",\"address\":\"%p\",\"as_installed\":", (void *)t->addr); sb_q(s, o); sb_add(s, ",\"now\":"); sb_q(s, c);
      sb_add(s, ",\"set_by\":"); sb_q(s, t->owner >= 0 ? g_tr[t->owner].mod : ""); sb_add(s, ",\"left_alone\":%s}", t->stopped ? "true" : "false");
    }
    sb_add(s, "]}");
  }
  sb_add(s, "]}");
}
#endif
