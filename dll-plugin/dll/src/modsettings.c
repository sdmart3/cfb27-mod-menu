/* modsettings.c: "Mod Settings", layer 1 (core) of the lightweight DLL version (docs/design.md).
 *
 * WHAT IT DOES. Ships ZERO game files. It reads every installed mod's manifest (Plugins\ModSettings\mods\<id>.json, spec v2:
 * docs/manifest-spec.md), keeps the player's choices in Plugins\ModSettings\values.json (written ONLY by the companion
 * page / ms-set.js, never by this DLL), and applies them:
 *   global rows  -> values.json itself (DLL mods read them with the exported ms_get, or through an optional ini binding: this DLL
 *                   rewrites that ONE key of the bound ini atomically, and only when values.json names a value for the row);
 *   band rows    -> the loaded dynasty's shared settings band: CrossArcData records of user 0 whose Value is in -8192..-7169 (record r =
 *                   -8192 + 512 r + 9 bits; modsettings/slots.json). The DLL edits the Value of a record that ALREADY exists (it never
 *                   creates records: each consumer mod's own expression code adds its record at its first Advance, "ensure record").
 * Mods must work without this DLL: every row's bit pattern 0 / its default is what a mod does when nothing was changed.
 *
 * WHERE THE BAND IS WRITTEN (the safe point, docs/design.md section 10, O2). Only on a dynasty SERVER thread, inside the
 * server handler of a few flows (the RPC thunk's handler call is redirected, the handler runs first, then this pass runs on the same thread
 * with the same flow, the way wsdata does at PracticeHome Enter): MetersFlow::RefreshFormResponse, MySchoolFlow::MySchoolRefreshFormResponse,
 * RosterFlow::EnterResponse, TeamScheduleFlow::EnterResponse (none patched by another DLL of ours). The pass looks the user up with the game's
 * own generic FindUser(flow, a2, &user) (every one of these handlers calls it first with its own flow / a2), reads user.CrossArcDataArray
 * (save schema handle 11) rows {Id handle 0, Value handle 1}, and writes a band record's Value with the record interface's int setter
 * (vt[0x148]) + the native change counter (the pair every native int write uses; wsdata's gameplan-focus write, PROVEN in game). Nothing is kept
 * across calls: every record reference is released before the handler returns to the game.
 *
 * SAFETY. Pinned per build (TU 10-13 = PE 1791226072, TU 10-29 = PE 1792187070; any other build: nothing installed). Function heads (24 bytes)
 * must match or nothing installs; each capture site must be a call to its pinned handler, name its flow 0xd0 bytes before the call, and have
 * the pinned 64-byte window hash (patchsafe.h, patch_verify), or that one site is skipped. Code writes go through patchsafe.h (vendored byte
 * for byte from wsdata / Dynasty Hooks: frozen threads, interlocked writes, read-back). Record work runs under a per-thread fault guard
 * (VEH + longjmp): a fault switches the band writer off for the session (the handler's own result is always returned untouched).
 *
 * FILES (Plugins\ModSettings\): modsettings.dll, modsettings.ini (read once), mods\*.json (manifests, hot-read), values.json (hot-read),
 * modsettings-status.json (written by this DLL: state, sites, manifests, every row's value, the loaded dynasty's band records), applied.json
 * (which dynasty changes were applied: their seq numbers), modsettings.log.
 * Build: node build.js (gcc -O2 -Wall -shared -static; KERNEL32 + msvcrt only; one export, ms_get; -DAP_TEST = modsettings-test.exe). */
#include "product.h"
#include <windows.h>
#include <psapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <setjmp.h>
#include "msjson.h"

#define MS_INI     MS_STEM ".ini"
#define MS_LOG     MS_STEM ".log"
#define MS_STATUS  MS_STEM "-status.json"
#define MS_VALUES  "values.json"
#define MS_APPLIED "applied.json"
#define MS_MODSDIR "mods"

/* the shared band (modsettings/slots.json: base -8192, 512 per record, 9 bits, 2 records) */
#define MS_BAND_BASE (-8192)
#define MS_BAND_RECSIZE 512
#define MS_BAND_BITS 9
#define MS_BAND_RECORDS 2

/* ------------------------------------------------------------------ settings + log */
static char g_dir[MAX_PATH];
static FILE *g_log;
static CRITICAL_SECTION g_cs;
static int g_log_sync;
static struct { int safe; int verify; } g_ps = { 1, 1 };
/* enabled: 0 = nothing looked up or installed; band_write: 0 = the band is read and reported, never written; ini_bind: 0 = ini bindings are
 * not written; capture_*: each capture flow on / off; log_captures: how many capture lines per flow are logged; poll_ms: file poll period */
static struct { int enabled, band_write, ini_bind, log_captures, poll_ms, cap[4]; } g_k = { 1, 1, 1, 20, 1000, { 1, 1, 1, 1 } };
static int g_ini_found;
/* the layer 2 tab feed (mstab.h): tab = 0 leaves the settings adapter alone; slider_native = 1 draws every slider as the game's slider row */
static struct { int on, slider_native, log_calls, show_inactive; } g_kt = { 1, 0, 20, 0 };
static char g_state[24] = "loading", g_reason[300];

static void logf_(const char *fmt, ...) {
  if (!g_log) return;
  char text[2048]; va_list ap; va_start(ap, fmt); vsnprintf(text, sizeof text, fmt, ap); va_end(ap);
  SYSTEMTIME st; GetLocalTime(&st);
  EnterCriticalSection(&g_cs);
  fprintf(g_log, "%02d:%02d:%02d %s\n", st.wHour, st.wMinute, st.wSecond, text); if (g_log_sync) fflush(g_log);
  LeaveCriticalSection(&g_cs);
}
static void log_flush(void) { if (!g_log) return; EnterCriticalSection(&g_cs); fflush(g_log); LeaveCriticalSection(&g_cs); }

/* ------------------------------------------------------------------ readable(), in_exe() (wsdata's) */
#define MS_FAST_PAGES 16
static int ms_walk(const char *a, const char *end) {
  do {
    MEMORY_BASIC_INFORMATION mbi; if (VirtualQuery(a, &mbi, sizeof mbi) != sizeof mbi) return 0;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) || (mbi.Protect & 0xff) == PAGE_NOACCESS) return 0;
    a = (const char *)mbi.BaseAddress + mbi.RegionSize;
  } while (a < end);
  return 1;
}
static int ms_fast(uintptr_t first, uintptr_t last) {
  size_t np = (size_t)((last - first) >> 12) + 1; if (np > MS_FAST_PAGES) return 0;
  PSAPI_WORKING_SET_EX_INFORMATION w[MS_FAST_PAGES];
  for (size_t i = 0; i < np; i++) { w[i].VirtualAddress = (void *)(first + (i << 12)); w[i].VirtualAttributes.Flags = 0; }
  if (!K32QueryWorkingSetEx(GetCurrentProcess(), w, (DWORD)(np * sizeof w[0]))) return 0;
  for (size_t i = 0; i < np; i++) {
    ULONG_PTR prot = w[i].VirtualAttributes.Win32Protection;
    if (!w[i].VirtualAttributes.Valid || !prot || (prot & PAGE_GUARD) || (prot & 0xff) == PAGE_NOACCESS) return 0;
  }
  return 1;
}
static int readable(const void *p, size_t n) {
  if (!p) return 0;
  const char *a = (const char *)p, *end = a + n;
  uintptr_t first = (uintptr_t)a & ~(uintptr_t)0xfff, last = ((uintptr_t)a + (n ? n - 1 : 0)) & ~(uintptr_t)0xfff;
  if (last < first) return 0;
  DWORD err = GetLastError(); int r = ms_fast(first, last); if (!r) r = ms_walk(a, end); SetLastError(err);
  return r;
}
#include "patchsafe.h"   /* vendored byte for byte from wsdata (= Dynasty Hooks / Press Box) */
#ifdef AP_TEST
static HMODULE ps_module_handle(LPCSTR name) { return !name && g_ps_test_base ? (HMODULE)g_ps_test_base : GetModuleHandleA(name); }
#define GetModuleHandleA(n) ps_module_handle(n)
#endif
static int in_exe(const void *p) {
  size_t size = 0; unsigned char *base = ps_image(&size, NULL, NULL);
  return base && (const unsigned char *)p >= base && (const unsigned char *)p < base + size;
}

/* ------------------------------------------------------------------ trampolines (wsdata's alloc_near / cave_near / tramp_alloc) */
#define MS_TRAMP_REACH 0x7ff00000ull
static void *alloc_near(unsigned char *target) {
  SYSTEM_INFO si; GetSystemInfo(&si);
  uintptr_t gran = si.dwAllocationGranularity, t = (uintptr_t)target & ~(gran - 1);
  uintptr_t reach = (MS_TRAMP_REACH - 2 * gran) & ~(gran - 1);
  uintptr_t amin = ((uintptr_t)si.lpMinimumApplicationAddress + gran - 1) & ~(gran - 1), amax = (uintptr_t)si.lpMaximumApplicationAddress;
  uintptr_t lo = t > amin + reach ? t - reach : amin, hi = t + reach < amax ? t + reach : amax;
  uintptr_t up = t, down = t >= lo + gran ? t - gran : 0; int upOk = up < hi, downOk = down >= lo && down != 0;
  for (unsigned n = 0; (upOk || downOk) && n < 30000u; n++) {
    int useUp = upOk && (!downOk || up - t <= t - down);
    uintptr_t c = useUp ? up : down;
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)c, &mbi, sizeof mbi) != sizeof mbi) { if (useUp) upOk = 0; else downOk = 0; continue; }
    uintptr_t rb = (uintptr_t)mbi.BaseAddress, re = rb + mbi.RegionSize;
    if (mbi.State == MEM_FREE && c >= rb && c + 0x1000 <= re) {
      void *r = VirtualAlloc((void *)c, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
      if (r) return r;
      if (useUp) up += gran; else down = down >= lo + gran ? down - gran : 0;
    } else if (useUp) up = (re + gran - 1) & ~(gran - 1);
    else down = rb > lo + gran ? ((rb - 1) & ~(gran - 1)) : 0;
    if (useUp && up >= hi) upOk = 0;
    if (!useUp && (down < lo || down == 0)) downOk = 0;
  }
  return NULL;
}
static unsigned char *cave_near(unsigned char *target) {
  size_t size = 0; unsigned char *base = ps_image(&size, NULL, NULL); if (!base) return NULL;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sh = IMAGE_FIRST_SECTION(nt); unsigned char *best = NULL; uint64_t bestd = ~0ull;
  for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++, sh++) {
    if (!(sh->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
    unsigned char *lo = base + sh->VirtualAddress, *hi = lo + sh->Misc.VirtualSize;
    if (target < lo || target >= hi) continue;
    for (unsigned char *p = lo; p < hi; ) {
      MEMORY_BASIC_INFORMATION mbi; if (VirtualQuery(p, &mbi, sizeof mbi) != sizeof mbi) break;
      unsigned char *re = (unsigned char *)mbi.BaseAddress + mbi.RegionSize; if (re > hi) re = hi;
      DWORD pr = mbi.Protect & 0xff;
      if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_EXECUTE_READ || pr == PAGE_EXECUTE_READWRITE || pr == PAGE_EXECUTE_WRITECOPY))
        for (unsigned char *w = (unsigned char *)(((uintptr_t)p + 16) & ~(uintptr_t)15); w + 17 <= re; w += 16) {
          if (w[-1] != 0xcc || w[16] != 0xcc) continue;
          int k = 0; for (; k < 16; k++) if (w[k] != 0xcc) break;
          if (k < 16) continue;
          uint64_t d = w > target ? (uint64_t)(w - target) : (uint64_t)(target - w);
          if (d < bestd && d < MS_TRAMP_REACH) { best = w; bestd = d; }
        }
      p = re;
    }
  }
  return best;
}
static unsigned char *g_tramp_page; static int g_tramp_used;   /* one page holds every site's 16-byte stub */
static unsigned char *tramp_alloc(unsigned char *at) {
  if (g_tramp_page && g_tramp_used < 0x1000 / 16) {
    int64_t d = (int64_t)(g_tramp_page - at); if (d < 0x7ff00000ll && d > -0x7ff00000ll) return g_tramp_page + 16 * g_tramp_used++;
  }
  unsigned char *p = (unsigned char *)alloc_near(at);
  if (p) { g_tramp_page = p; g_tramp_used = 1; logf_("trampoline: page %p for %p", (void *)p, (void *)at); return p; }
  p = cave_near(at);
  if (p) logf_("trampoline: no grantable page near %p; int3 padding slot %p instead", (void *)at, (void *)p);
  else logf_("trampoline: no page and no padding slot within 2 GB of %p; the hook is not installed", (void *)at);
  return p;
}
static const char *g_patch_what = "modsettings: a capture site";   /* what the next patch_call logs (patchsafe.h fixes patch_call at three arguments) */
static int patch_call(unsigned char *site, void *dst_fn, void **tramp_out) {
  const char *what = g_patch_what;
  unsigned char *tramp = tramp_alloc(site); if (!tramp) return 0;
  if (!tramp_write(tramp, dst_fn)) return 0;
  int64_t delta = (int64_t)(tramp - (site + 5)); if (delta > INT32_MAX || delta < INT32_MIN) return 0;
  unsigned char code[5] = { site[0], 0, 0, 0, 0 }; int32_t rel32 = (int32_t)delta; memcpy(code + 1, &rel32, 4);
  if (!ps_patch(site, code, 5, what)) return 0;
  *tramp_out = tramp; return 1;
}

/* ------------------------------------------------------------------ pins (one MsBuild per known game build) */
#include "ms_pins.h"
/* the running game's Mod Manager game version, by PE timestamp (TU 10-29 = 4338538: the version every queue entry and .fbmod build since
 * 10-29 was stamped with; TU 10-13: not recorded). 0 = unknown: gameVersion pins are not checked */
static const struct { uint32_t stamp, gamever; } MS_GAMEVER[] = { { 1792187070u, 4338538u } };
static uint32_t gamever_of(uint32_t stamp) { for (size_t i = 0; i < sizeof MS_GAMEVER / sizeof *MS_GAMEVER; i++) if (MS_GAMEVER[i].stamp == stamp) return MS_GAMEVER[i].gamever; return 0; }
static volatile uint32_t g_run_gamever; static volatile LONG g_cfg_force;
static unsigned char *g_base; static uint64_t g_hdr_base;
#define AT(rva) ((void *)(g_base + (rva)))
static const MsBuild *g_pb = &MS_BUILDS[0];
static const MsBuild *ms_build_of(uint32_t stamp) { for (int i = 0; i < MS_NBUILDS; i++) if (MS_BUILDS[i].stamp == stamp) return &MS_BUILDS[i]; return NULL; }
static int g_t_fake;               /* self-test: fake records (no pins asked of them) */
static void *g_fn[MS_NFUNCS];
static int g_site_ok[MS_NSITES];   /* this build's site i matched its pins (else it is skipped) */
static char g_site_why[MS_NSITES][160];
#define NAME_BACK 0xd0
/* every pin of the running build. Functions: all or nothing (1 / 0). Sites: g_site_ok[] per site. why gets the first miss. */
static int ms_pins_check(char *why, size_t n) {
  size_t size = 0; uint32_t stamp = 0; int pref = 0; g_base = ps_image(&size, &stamp, &pref);
  if (!g_base) { snprintf(why, n, "no image"); return 0; }
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_base + dos->e_lfanew);
  g_hdr_base = nt->OptionalHeader.ImageBase;
  const MsBuild *pb = ms_build_of(stamp);
  if (!pb) {
    char known[160] = ""; for (int i = 0; i < MS_NBUILDS; i++) snprintf(known + strlen(known), sizeof known - strlen(known), "%s%u (%s)", i ? ", " : "", MS_BUILDS[i].stamp, MS_BUILDS[i].name);
    snprintf(why, n, "this game build (PE timestamp %u) is not one this version was made for: %s", stamp, known); return 0;
  }
  g_pb = pb;
  { uint32_t gv = gamever_of(stamp); if (gv && gv != g_run_gamever) { g_run_gamever = gv; InterlockedExchange(&g_cfg_force, 1); } }   /* gameVersion pins re-checked */
  if (!pref) { snprintf(why, n, "the game is not at its preferred base"); return 0; }
  for (int i = 0; i < MS_NFUNCS; i++) {
    unsigned char *f = (unsigned char *)AT(pb->funcs[i].rva);
    if (!readable(f, 24) || memcmp(f, pb->funcs[i].head, 24)) { snprintf(why, n, "game function %s at %p is not the verified code (not unpacked yet, or another build)", pb->funcs[i].name, (void *)f); return 0; }
  }
  for (int i = 0; i < MS_NFUNCS; i++) g_fn[i] = AT(pb->funcs[i].rva);
  for (int i = 0; i < MS_NSITES; i++) {
    const MsSitePin *s = &pb->sites[i]; unsigned char *call = (unsigned char *)AT(s->call_rva), *h = (unsigned char *)AT(s->handler_rva); g_site_ok[i] = 0;
    if (!readable(call - NAME_BACK, NAME_BACK + 5) || !readable(h, 24) || !readable(AT(s->name_rva), strlen(s->flow) + 1)) { snprintf(g_site_why[i], sizeof g_site_why[i], "not readable"); continue; }
    int32_t rel; memcpy(&rel, call + 1, 4);
    if (call[0] != 0xe8 || call + 5 + rel != h) { snprintf(g_site_why[i], sizeof g_site_why[i], "the call does not reach its handler (redirected by another tool?)"); continue; }
    if (memcmp(h, s->head, 24)) { snprintf(g_site_why[i], sizeof g_site_why[i], "the handler is not the verified code"); continue; }
    unsigned char *q = call - NAME_BACK; int32_t nd; memcpy(&nd, q + 3, 4);
    if (q[0] != 0x4c || q[1] != 0x8d || q[2] != 0x05 || q + 7 + nd != (unsigned char *)AT(s->name_rva) || strcmp((const char *)AT(s->name_rva), s->flow)) { snprintf(g_site_why[i], sizeof g_site_why[i], "the thunk does not name %s", s->flow); continue; }
    g_site_ok[i] = 1; g_site_why[i][0] = 0;
  }
  return 1;
}

/* ------------------------------------------------------------------ the game's calls + records (wsdata's record helpers) */
typedef char (*finduser_fn)(void *flow, void *a2, void **user);
typedef size_t (*arrcount_fn)(void *arr);
typedef uintptr_t (*handler_fn)(void *flow, void *a2, void *req, void *resp);
typedef void *(*vt0_fn)(void *self);
typedef void **(*qi_fn)(void *self, void **out, uint32_t iid);
typedef int (*getint_fn)(void *iface, int row, void *fh);
typedef void (*getref_fn)(void *iface, void **out, int row, void *fh);
typedef void (*setint_fn)(void *iface, int row, void *fh, int v);   /* record interface vt[0x148] */
typedef void (*dirty_fn)(void *iface);                             /* ++[[iface+0x10]+0x1a0]: after every native int write */
#define FN(i) (g_fn[i])
#define IF_INT 0x100
#define IF_ENUM 0x108
#define IF_REF 0x128
#define IF_SETINT 0x148
#define IID_ARRY 0x41525259u
#define H_CAD 11        /* FranchiseUser.CrossArcDataArray (save schema index) */
#define H_CAD_ID 0      /* CrossArcData.Id (enum; 0 = PLACEHOLDER) */
#define H_CAD_VAL 1     /* CrossArcData.Value (signed 16-bit) */
static void **sc_vt(void *x) { if (!x || !readable(x, 0x1c)) return NULL; void **vt = *(void ***)x; return vt && readable(vt, 0x40) && (g_t_fake || in_exe(vt[1])) ? vt : NULL; }
static void sc_release(void *x) { void **vt = sc_vt(x); if (vt) ((vt0_fn)vt[1])(x); }
static void **sc_ivt(void *x, void **iface, int *row) {
  if (!x || !readable(x, 0x1c)) return NULL;
  memcpy(iface, (char *)x + 0x10, 8); memcpy(row, (char *)x + 0x18, 4);
  if (!*iface || !readable(*iface, 8)) return NULL;
  void **ivt = *(void ***)*iface; return ivt && readable(ivt, IF_SETINT + 8) ? ivt : NULL;
}
static int rd_num(void *x, void *fh, int slot, int dflt) { void *ifc; int row; void **ivt = sc_ivt(x, &ifc, &row); if (!ivt) return dflt; return ((getint_fn)ivt[slot / 8])(ifc, row, fh); }
static void *rd_ref(void *x, void *fh) { void *ifc; int row; void **ivt = sc_ivt(x, &ifc, &row); if (!ivt) return NULL; void *o = NULL; ((getref_fn)ivt[IF_REF / 8])(ifc, &o, row, fh); return o; }
static void *arr_of(void *x, void *fh) {
  void *ref = rd_ref(x, fh); if (!ref) return NULL;
  void **vt = sc_vt(ref); void *arr = NULL;
  if (vt) { void *tmp = NULL; void **pp = ((qi_fn)vt[5])(ref, &tmp, IID_ARRY); if (pp) { arr = *pp; *pp = NULL; } if (tmp) sc_release(tmp); }
  sc_release(ref); return arr;
}
static int arr_count(void *arr) { return arr ? (int)((arrcount_fn)FN(MS_F_ARRCOUNT))(arr) : 0; }
static void *arr_get(void *arr, int i) { void *ifc; int row; void **ivt = sc_ivt(arr, &ifc, &row); if (!ivt) return NULL; void *o = NULL; ((getref_fn)ivt[IF_REF / 8])(ifc, &o, row, (void *)(uintptr_t)i); return o; }

/* ------------------------------------------------------------------ fault guard (per thread; wsdata's VEH + longjmp) */
static DWORD g_tls = TLS_OUT_OF_INDEXES; static PVOID g_veh; static volatile LONG g_faults, g_disabled;
static volatile uintptr_t g_fault_ip, g_fault_addr;
static LONG CALLBACK ms_veh(EXCEPTION_POINTERS *ep) {
  DWORD c = ep->ExceptionRecord->ExceptionCode;
  if (g_tls == TLS_OUT_OF_INDEXES) return EXCEPTION_CONTINUE_SEARCH;
  jmp_buf *jb = (jmp_buf *)TlsGetValue(g_tls);
  if (jb && (c == EXCEPTION_ACCESS_VIOLATION || c == EXCEPTION_IN_PAGE_ERROR || c == EXCEPTION_ILLEGAL_INSTRUCTION || c == EXCEPTION_PRIV_INSTRUCTION)) {
    g_fault_ip = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    g_fault_addr = ep->ExceptionRecord->NumberParameters >= 2 ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    TlsSetValue(g_tls, NULL); longjmp(*jb, 1);
  }
  return EXCEPTION_CONTINUE_SEARCH;
}
static void ms_guard_init(void) { if (g_tls == TLS_OUT_OF_INDEXES) g_tls = TlsAlloc(); if (!g_veh) g_veh = AddVectoredExceptionHandler(1, ms_veh); }
/* off: the switch a fault turns on (g_disabled = the band writer; g_tab_off = the tab feed) */
static int ms_guarded2(void (*fn)(void *), void *arg, const char *what, volatile LONG *off) {
  if (g_tls == TLS_OUT_OF_INDEXES) { fn(arg); return 1; }
  jmp_buf jb; void *volatile prev = TlsGetValue(g_tls);
  if (setjmp(jb)) {
    TlsSetValue(g_tls, prev); InterlockedIncrement(&g_faults); InterlockedExchange(off, 1);
    logf_("modsettings: FAULT in %s (code at %p, address %p): %s is off for this session (global rows, ini bindings and ms_get keep working)", what, (void *)g_fault_ip, (void *)g_fault_addr, off == &g_disabled ? "the band writer" : "the tab feed");
    log_flush(); return 0;
  }
  TlsSetValue(g_tls, &jb); fn(arg); TlsSetValue(g_tls, prev);
  return 1;
}
static int ms_guarded(void (*fn)(void *), void *arg, const char *what) { return ms_guarded2(fn, arg, what, &g_disabled); }

/* ------------------------------------------------------------------ the configuration: manifests + values.json */
#define MS_MAXMODS 64
#define MS_MAXROWS 512
#define MS_ID 41
#define MS_MID 108      /* a mod id as stored: <modid> or, for a second mod with a used modid, <modid>@<source file> (spec v3 "Names, duplicates and clashes") */
#define MS_MAXOPT 8
#define MS_MAXPEND 64   /* pending dynasty changes handled per pass */
#define MS_MAXLIST 12   /* spec v3: a list row has 2-12 options */
#define MS_MAXSTOPS 1001 /* spec v3: a slider has at most 1001 stops */
#define MS_POOL (512 * 1024)
enum { ST_BAND = 1, ST_GLOBAL = 2 };
/* row types (spec v3; a v2 row is a toggle when its options are ["Off", "On"], else a list) and v3 bindings */
enum { RT_HEADER = 1, RT_INFO, RT_TOGGLE, RT_LIST, RT_SLIDER };
enum { BD_V2 = 0, BD_API, BD_TUNING, BD_DYNASTY };
/* visibleIf / enabledIf (spec v3 "Safety and behaviour rules" 9): { "key", optional "mod", "eq" | "ne" | "in": [...] } */
typedef struct { int on, op, n, row; char mod[MS_MID], key[MS_ID]; int kind[8], b[8], s[8]; double num[8]; } MsCond;   /* op 0 eq, 1 ne, 2 in; kind 0 number, 1 bool, 2 text */
typedef struct {
  char mod[MS_MID], id[MS_ID]; int nopt, dflt, store, rec, bit, width;
  int has_ini; char ini_file[MAX_PATH], ini_key[64], ini_val[MS_MAXOPT][48];
  /* the tab (layer 2) + spec v3. Strings are offsets into MsCfg.pool (-1 = none). A slider's nopt = its stops (option index = stop). */
  int schema, type, bind, s_label, s_desc, s_opt[MS_MAXLIST], s_unit, s_where;   /* s_where: tuning "asset:field" / dynasty slot name */
  /* safety and behaviour rules: asis = option 0 is "As installed" (tuning rows; the real options follow); applies -1 (v2, not shown) / 0 now /
   * 1 next-game / 2 restart; copy 1 simulation, 2 base, 3 both; unavail + s_unavail = never written, greyed, why; s_also = "also set by ..." */
  int asis, applies, copy, unavail, s_unavail, s_asset, s_field, s_inst, s_also; MsCond cond[2];   /* cond[0] visibleIf, cond[1] enabledIf */
  double smin, smax, sstep;
} MsRow;
typedef struct { char id[MS_MID]; int first, nrows, schema, s_title, s_version, s_source, dup, s_section, closed, s_fbmod, active, s_about, npin; long long pin[8]; } MsMod;   /* dup: stored as <modid>@<source>; s_section: "section" (a modid or "@name") */   /* dup: stored as <modid>@<source> (its modid was taken) */
typedef struct {
  int nmods, nrows; MsMod mods[MS_MAXMODS]; MsRow rows[MS_MAXROWS];
  int gval[MS_MAXROWS], gset[MS_MAXROWS];            /* global value (index) and whether values.json / settings.json named it (else the default) */
  int want[MS_MAXROWS]; long long want_seq[MS_MAXROWS];   /* band rows: the dynasty change values.json / settings.json asks for (seq 0 = none) */
  char problems[4096]; int nproblems;
  char loaded[1024];                                    /* the manifest ids, comma-separated (log + status) */
  char pool[MS_POOL]; int pool_used;                    /* the strings the tab shows (labels, help, options, titles) */
} MsCfg;
/* a string into the configuration's pool; -1 when it does not fit (the tab then shows nothing for it) */
static int pool_put(MsCfg *c, const char *s) {
  if (!s) return -1; size_t n = strlen(s) + 1; if ((size_t)c->pool_used + n > sizeof c->pool) return -1;
  memcpy(c->pool + c->pool_used, s, n); int at = c->pool_used; c->pool_used += (int)n; return at;
}
static const char *pool_str(const MsCfg *c, int at) { return at >= 0 && at < c->pool_used ? c->pool + at : ""; }
static MsCfg *g_cfg;                 /* current; swapped whole under g_cfg_lock */
static SRWLOCK g_cfg_lock = SRWLOCK_INIT;
static void cfg_problem(MsCfg *c, const char *fmt, ...) {
  char m[400]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof m, fmt, ap); va_end(ap);
  c->nproblems++; size_t l = strlen(c->problems); if (l + strlen(m) + 2 < sizeof c->problems) { if (l) strcat(c->problems, "\n"); strcat(c->problems, m); }
}
static int id_ok(const char *s, int short_ok) {
  size_t n = s ? strlen(s) : 0;
  if (short_ok && n >= 1 && n <= 2) { for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9'))) return 0; return 1; }
  if (n < 2 || n > 40) return 0;
  if (!((s[0] >= 'a' && s[0] <= 'z') || (s[0] >= '0' && s[0] <= '9'))) return 0;
  for (size_t i = 1; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-')) return 0;
  return 1;
}
static int u8len(const char *s) { int n = 0; for (; *s; s++) if (((unsigned char)*s & 0xc0) != 0x80) n++; return n; }
/* an ini binding's file: relative to the Plugins folder (the parent of Plugins\ModSettings), no drive, no leading slash, no ".." segment */
static int ini_path_ok(const char *f) {
  if (!f || !*f || strlen(f) > 200 || strchr(f, ':') || f[0] == '\\' || f[0] == '/') return 0;
  const char *p = f; while (*p) { const char *e = p; while (*e && *e != '\\' && *e != '/') e++; size_t n = (size_t)(e - p); if (n == 0 || (n == 2 && p[0] == '.' && p[1] == '.') || (n == 1 && p[0] == '.')) return 0; p = *e ? e + 1 : e; }
  size_t n = strlen(f); return n > 4 && !_stricmp(f + n - 4, ".ini");
}
static int ini_key_ok(const char *k) { size_t n = k ? strlen(k) : 0; if (n < 1 || n > 63) return 0; for (size_t i = 0; i < n; i++) if (!((k[i] >= 'a' && k[i] <= 'z') || (k[i] >= 'A' && k[i] <= 'Z') || (k[i] >= '0' && k[i] <= '9') || k[i] == '_' || k[i] == '.' || k[i] == '-')) return 0; return 1; }
static int ini_val_ok(const char *v) { size_t n = v ? strlen(v) : 0; if (n < 1 || n > 47) return 0; for (size_t i = 0; i < n; i++) if (v[i] == '\r' || v[i] == '\n' || v[i] == ';' || v[i] == '#' || v[i] == '=') return 0; return 1; }
static char *read_file(const char *path, size_t max, size_t *len) {
  FILE *f = fopen(path, "rb"); if (!f) return NULL;
  fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
  if (n < 0 || (size_t)n > max) { fclose(f); return NULL; }
  char *b = (char *)malloc((size_t)n + 1); if (!b) { fclose(f); return NULL; }
  size_t got = fread(b, 1, (size_t)n, f); fclose(f); b[got] = 0; if (len) *len = got; return b;
}
#define J_NODES 4096
#define J_TEXT (128 * 1024)
#include "msv3.h"   /* spec v3 manifests (schema 3), settings.json, the in-game change journal */
/* one manifest (spec v2) into c; all or nothing: a manifest with any problem adds no rows. name = the file name. */
static void load_manifest(MsCfg *c, const char *path, const char *name) {
  size_t len; char *src = read_file(path, 256 * 1024, &len);
  if (!src) { cfg_problem(c, "%s: cannot read it (or larger than 256 KB)", name); return; }
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES); char *text = (char *)malloc(J_TEXT); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES, text, J_TEXT, src, len) : -1;
  int before = c->nproblems;
  if (root < 0) { cfg_problem(c, "%s: not valid JSON (%s)", name, nodes && text ? d.err : "out of memory"); goto done; }
  if (d.n[root].type != J_OBJ) { cfg_problem(c, "%s: the manifest must be a JSON object", name); goto done; }
  long long schema = 0;
  if (j_isint(&d, j_get(&d, root, "schema"), &schema) && schema == 3) { load_manifest_v3(c, &d, root, name); goto done; }
  if (!j_isint(&d, j_get(&d, root, "schema"), &schema) || schema != 2) { cfg_problem(c, "%s: schema must be 2 (a schema 1 manifest belongs to the tabled data-only page; see the v2 spec)", name); goto done; }
  const char *id = j_str(&d, j_get(&d, root, "id"));
  if (!id_ok(id, 0)) { cfg_problem(c, "%s: id must be 2-40 characters: lower case letters, digits, '-'", name); goto done; }
  { char want[64]; snprintf(want, sizeof want, "%s.json", id); if (_stricmp(want, name)) { cfg_problem(c, "%s: the file name must be <id>.json (%s)", name, want); goto done; } }
  for (int m = 0; m < c->nmods; m++) if (!strcmp(c->mods[m].id, id)) { cfg_problem(c, "%s: a second manifest for %s", name, id); goto done; }
  const char *title = j_str(&d, j_get(&d, root, "title"));
  if (!title || !*title || u8len(title) > 40) { cfg_problem(c, "%s: title missing or longer than 40 characters", name); goto done; }
  int rows = j_get(&d, root, "rows");
  if (!j_at(&d, rows) || d.n[rows].type != J_ARR || d.n[rows].count < 1 || d.n[rows].count > 64) { cfg_problem(c, "%s: rows must be an array of 1-64 rows", name); goto done; }
  if (c->nmods >= MS_MAXMODS || c->nrows + d.n[rows].count > MS_MAXROWS) { cfg_problem(c, "%s: too many mods or rows in total (%d mods, %d rows)", name, MS_MAXMODS, MS_MAXROWS); goto done; }
  int first = c->nrows, k = 0, pool0 = c->pool_used;
  for (int r = d.n[rows].first; r >= 0; r = d.n[r].next, k++) {
    MsRow *o = &c->rows[first + k]; memset(o, 0, sizeof *o);
    snprintf(o->mod, sizeof o->mod, "%s", id);
    o->schema = 2; o->bind = BD_V2; o->s_label = o->s_desc = o->s_unit = o->s_where = -1; o->applies = -1; o->s_unavail = o->s_asset = o->s_field = o->s_inst = o->s_also = -1; o->cond[0].row = o->cond[1].row = -1; for (int x = 0; x < MS_MAXLIST; x++) o->s_opt[x] = -1;
    if (d.n[r].type != J_OBJ) { cfg_problem(c, "%s: row %d is not an object", name, k); continue; }
    const char *rid = j_str(&d, j_get(&d, r, "id"));
    if (!id_ok(rid, 1)) { cfg_problem(c, "%s: row %d: id must be lower case letters, digits, '-'", name, k); continue; }
    snprintf(o->id, sizeof o->id, "%s", rid);
    for (int q = first; q < first + k; q++) if (!strcmp(c->rows[q].id, rid)) cfg_problem(c, "%s/%s: the row id appears twice", id, rid);
    const char *label = j_str(&d, j_get(&d, r, "label"));
    if (!label || !*label || u8len(label) > 40) cfg_problem(c, "%s/%s: label missing or longer than 40 characters", id, rid);
    const char *help = j_str(&d, j_get(&d, r, "help")); if (j_get(&d, r, "help") >= 0 && (!help || u8len(help) > 400)) cfg_problem(c, "%s/%s: help must be text of at most 400 characters", id, rid);
    o->s_label = pool_put(c, label); if (help && *help) o->s_desc = pool_put(c, help);
    const char *inert = j_str(&d, j_get(&d, r, "inert")); if (j_get(&d, r, "inert") >= 0 && (!inert || u8len(inert) > 60)) cfg_problem(c, "%s/%s: inert must be text of at most 60 characters", id, rid);
    int opts = j_get(&d, r, "options");
    if (!j_at(&d, opts) || d.n[opts].type != J_ARR || d.n[opts].count < 2 || d.n[opts].count > MS_MAXOPT) { cfg_problem(c, "%s/%s: options must be 2-8 labels", id, rid); continue; }
    o->nopt = d.n[opts].count; int oi = 0; const char *o0 = NULL, *o1 = NULL;
    for (int x = d.n[opts].first; x >= 0; x = d.n[x].next, oi++) {
      const char *s = j_str(&d, x); if (!s || !*s || u8len(s) > 24) cfg_problem(c, "%s/%s: option %d must be 1-24 characters", id, rid, oi);
      if (oi == 0) o0 = s; if (oi == 1) o1 = s;
      o->s_opt[oi] = pool_put(c, s);
    }
    o->type = o->nopt == 2 && o0 && o1 && !strcmp(o0, "Off") && !strcmp(o1, "On") ? RT_TOGGLE : RT_LIST;
    if (o->nopt == 2 && !(o0 && o1 && !strcmp(o0, "Off") && !strcmp(o1, "On"))) cfg_problem(c, "%s/%s: a two-option row must be [\"Off\", \"On\"] (Off = 0, On = 1)", id, rid);
    long long dflt = -1; if (!j_isint(&d, j_get(&d, r, "default"), &dflt) || dflt < 0 || dflt >= o->nopt) { cfg_problem(c, "%s/%s: default must be an option index", id, rid); continue; }
    o->dflt = (int)dflt;
    const char *store = j_str(&d, j_get(&d, r, "store"));
    if (store && !strcmp(store, "band")) {
      o->store = ST_BAND; int b = j_get(&d, r, "band"); long long rec = -1, bit = -1, width = -1;
      if (!j_isint(&d, j_get(&d, b, "record"), &rec) || !j_isint(&d, j_get(&d, b, "bit"), &bit) || !j_isint(&d, j_get(&d, b, "width"), &width)) { cfg_problem(c, "%s/%s: a band row needs \"band\": { \"record\", \"bit\", \"width\" } (its line in slots.json)", id, rid); continue; }
      if (rec < 0 || rec >= MS_BAND_RECORDS || width < 1 || width > 3 || bit < 0 || bit + width > MS_BAND_BITS) { cfg_problem(c, "%s/%s: band record %lld bit %lld width %lld is outside the band (records 0-%d, bits 0-%d, width 1-3)", id, rid, rec, bit, width, MS_BAND_RECORDS - 1, MS_BAND_BITS - 1); continue; }
      if ((1 << width) < o->nopt) { cfg_problem(c, "%s/%s: width %lld holds %d values, the row has %d options", id, rid, width, 1 << (int)width, o->nopt); continue; }
      o->rec = (int)rec; o->bit = (int)bit; o->width = (int)width;
      if (j_get(&d, r, "ini") >= 0) cfg_problem(c, "%s/%s: an ini binding is for global rows only", id, rid);
      for (int q = 0; q < first + k; q++) {   /* no two band rows share a bit (earlier manifests win) */
        const MsRow *p = &c->rows[q]; if (p->store != ST_BAND || p->rec != o->rec) continue;
        if (p->bit < o->bit + o->width && o->bit < p->bit + p->width) cfg_problem(c, "%s/%s: band record %d bits %d-%d overlap %s/%s (bits %d-%d)", id, rid, o->rec, o->bit, o->bit + o->width - 1, p->mod, p->id, p->bit, p->bit + p->width - 1);
      }
    } else if (store && !strcmp(store, "global")) {
      o->store = ST_GLOBAL; int ib = j_get(&d, r, "ini");
      if (ib >= 0) {
        const char *f = j_str(&d, j_get(&d, ib, "file")), *key = j_str(&d, j_get(&d, ib, "key")); int vals = j_get(&d, ib, "values");
        if (!ini_path_ok(f)) { cfg_problem(c, "%s/%s: ini.file must be an .ini path inside the Plugins folder, e.g. \"CallSheet\\\\callsheet.ini\" (no drive, no '..')", id, rid); continue; }
        if (!ini_key_ok(key)) { cfg_problem(c, "%s/%s: ini.key must be 1-63 letters, digits, '_', '.', '-'", id, rid); continue; }
        if (!j_at(&d, vals) || d.n[vals].type != J_ARR || d.n[vals].count != o->nopt) { cfg_problem(c, "%s/%s: ini.values must list one value per option (%d)", id, rid, o->nopt); continue; }
        int vi = 0; for (int x = d.n[vals].first; x >= 0; x = d.n[x].next, vi++) { const char *s = j_str(&d, x); if (!ini_val_ok(s)) { cfg_problem(c, "%s/%s: ini.values[%d] must be 1-47 characters without ; # = or a line break", id, rid, vi); break; } snprintf(o->ini_val[vi], sizeof o->ini_val[vi], "%s", s); }
        o->has_ini = 1; snprintf(o->ini_file, sizeof o->ini_file, "%s", f); snprintf(o->ini_key, sizeof o->ini_key, "%s", key);
      }
    } else cfg_problem(c, "%s/%s: store must be \"band\" (the value lives in the dynasty save) or \"global\" (values.json)", id, rid);
  }
  if (c->nproblems == before) {
    MsMod *m = &c->mods[c->nmods++]; snprintf(m->id, sizeof m->id, "%s", id); m->first = first; m->nrows = k; c->nrows += k;
    m->schema = 2; m->s_title = pool_put(c, title); m->s_version = -1; m->s_source = pool_put(c, name); m->dup = 0; m->s_section = -1; m->closed = 0; m->s_fbmod = -1; m->active = 1; m->s_about = -1; m->npin = 0;
    snprintf(c->loaded + strlen(c->loaded), sizeof c->loaded - strlen(c->loaded), "%s%s", c->loaded[0] ? "," : "", id);
  } else c->pool_used = pool0;
done:
  free(nodes); free(text); free(src);
}
static int row_index(const MsCfg *c, const char *mod, const char *row) { for (int i = 0; i < c->nrows; i++) if (!strcmp(c->rows[i].mod, mod) && !strcmp(c->rows[i].id, row)) return i; return -1; }
/* values.json (schema 2): { "global": { mod: { row: index } }, "dynasty": { mod: { row: { "value": index, "seq": n } } } } (g_seq_seen: msv3.h) */
static void load_values(MsCfg *c, const char *path) {
  for (int i = 0; i < c->nrows; i++) { c->gval[i] = c->rows[i].dflt; c->gset[i] = 0; c->want[i] = -1; c->want_seq[i] = 0; }
  size_t len; char *src = read_file(path, 1024 * 1024, &len); if (!src) return;   /* no values.json yet: every row at its default */
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES * 4); char *text = (char *)malloc(J_TEXT * 4); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES * 4, text, J_TEXT * 4, src, len) : -1;
  if (root < 0 || d.n[root].type != J_OBJ) { cfg_problem(c, MS_VALUES ": not valid JSON (%s): every row at its default", root < 0 ? d.err : "not an object"); goto done; }
  long long schema = 0; if (!j_isint(&d, j_get(&d, root, "schema"), &schema) || schema != 2) { cfg_problem(c, MS_VALUES ": schema must be 2: every row at its default"); goto done; }
  for (int pass = 0; pass < 2; pass++) {
    int top = j_get(&d, root, pass ? "dynasty" : "global"); if (top < 0) continue;
    if (d.n[top].type != J_OBJ) { cfg_problem(c, MS_VALUES ": \"%s\" must be an object", pass ? "dynasty" : "global"); continue; }
    for (int m = d.n[top].first; m >= 0; m = d.n[m].next) {
      if (d.n[m].type != J_OBJ) continue;
      for (int r = d.n[m].first; r >= 0; r = d.n[r].next) {
        int i = row_index(c, d.n[m].key, d.n[r].key); if (i < 0) continue;   /* a mod not installed (any more): kept by the companion, ignored here */
        const MsRow *row = &c->rows[i]; long long v = -1, seq = 0;
        if (!pass) {
          if (row->store != ST_GLOBAL) continue;
          if (!j_isint(&d, r, &v) || v < 0 || v >= row->nopt) { cfg_problem(c, MS_VALUES ": global %s/%s must be an option index 0-%d (the default is used)", row->mod, row->id, row->nopt - 1); continue; }
          c->gval[i] = (int)v; c->gset[i] = 1;
        } else {
          if (row->store != ST_BAND) continue;
          if (!j_isint(&d, j_get(&d, r, "value"), &v) || !j_isint(&d, j_get(&d, r, "seq"), &seq) || v < 0 || v >= row->nopt || seq < 1) { cfg_problem(c, MS_VALUES ": dynasty %s/%s needs { \"value\": 0-%d, \"seq\": 1 or more }", row->mod, row->id, row->nopt - 1); continue; }
          c->want[i] = (int)v; c->want_seq[i] = seq; if (seq > g_seq_seen) g_seq_seen = seq;
        }
      }
    }
  }
done:
  free(nodes); free(text); free(src);
}
static int cmp_names(const void *a, const void *b) { return _stricmp((const char *)a, (const char *)b); }
static MsCfg *cfg_build(void) {
  MsCfg *c = (MsCfg *)calloc(1, sizeof *c); if (!c) return NULL;
  slots_load();
  char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\" MS_MODSDIR "\\*.json", g_dir);
  static char names[MS_MAXMODS * 2][MAX_PATH]; int nn = 0;
  WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
  if (h != INVALID_HANDLE_VALUE) { do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && nn < MS_MAXMODS * 2) snprintf(names[nn++], MAX_PATH, "%s", fd.cFileName); } while (FindNextFileA(h, &fd)); FindClose(h); }
  qsort(names, (size_t)nn, sizeof names[0], cmp_names);   /* a fixed order: earlier manifests win a band bit clash */
  int plan[MS_MAXMODS * 2]; cfg_plan(names, nn, plan);   /* same mod twice: only its highest version (msv3.h) */
  for (int i = 0; i < nn; i++) { if (plan[i] < 0) continue; char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\" MS_MODSDIR "\\%s", g_dir, names[plan[i]]); load_manifest(c, p, names[plan[i]]); }
  cfg_resolve(c);   /* pack activity, gameVersion pins, two mods on one field, visibleIf / enabledIf targets (msv3.h) */
  char vp[MAX_PATH]; snprintf(vp, sizeof vp, "%s\\" MS_VALUES, g_dir); load_values(c, vp);
  load_settings(c);   /* settings.json (the in-game tab's choices) after values.json: a global row it names wins; a dynasty change by seq */
  return c;
}
/* the change stamp of the inputs: values.json + every manifest (size, write time, count) */
static unsigned long long inputs_stamp(void) {
  unsigned long long h = 1469598103934665603ull; WIN32_FILE_ATTRIBUTE_DATA a; char p[MAX_PATH];
  snprintf(p, sizeof p, "%s\\" MS_VALUES, g_dir);
  if (GetFileAttributesExA(p, GetFileExInfoStandard, &a)) { h ^= a.ftLastWriteTime.dwLowDateTime; h *= 1099511628211ull; h ^= a.nFileSizeLow; h *= 1099511628211ull; } else h ^= 7;
  snprintf(p, sizeof p, "%s\\" MS_SETTINGS, g_dir);
  if (GetFileAttributesExA(p, GetFileExInfoStandard, &a)) { h ^= a.ftLastWriteTime.dwLowDateTime; h *= 1099511628211ull; h ^= a.nFileSizeLow; h *= 1099511628211ull; } else h ^= 11;
  snprintf(p, sizeof p, "%s\\" MS_MODSDIR "\\*.json", g_dir);
  WIN32_FIND_DATAA fd; HANDLE f = FindFirstFileA(p, &fd);
  if (f != INVALID_HANDLE_VALUE) { do { for (const char *s = fd.cFileName; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; } h ^= fd.ftLastWriteTime.dwLowDateTime; h *= 1099511628211ull; h ^= fd.nFileSizeLow; h *= 1099511628211ull; } while (FindNextFileA(f, &fd)); FindClose(f); }
  return h;
}

/* ------------------------------------------------------------------ applied.json: the dynasty changes already written (by seq) */
typedef struct { char key[MS_MID + MS_ID + 2]; long long seq; } MsApplied;
static MsApplied g_applied[MS_MAXROWS * 2]; static int g_napplied; static volatile LONG g_applied_dirty;
static CRITICAL_SECTION g_applied_cs;
static long long applied_seq(const char *mod, const char *row) { char k[MS_MID + MS_ID + 2]; snprintf(k, sizeof k, "%s/%s", mod, row); for (int i = 0; i < g_napplied; i++) if (!strcmp(g_applied[i].key, k)) return g_applied[i].seq; return 0; }
static void applied_set(const char *mod, const char *row, long long seq) {
  char k[MS_MID + MS_ID + 2]; snprintf(k, sizeof k, "%s/%s", mod, row);
  for (int i = 0; i < g_napplied; i++) if (!strcmp(g_applied[i].key, k)) { if (g_applied[i].seq != seq) { g_applied[i].seq = seq; InterlockedExchange(&g_applied_dirty, 1); } return; }
  if (g_napplied < MS_MAXROWS * 2) { snprintf(g_applied[g_napplied].key, sizeof g_applied[g_napplied].key, "%s", k); g_applied[g_napplied++].seq = seq; InterlockedExchange(&g_applied_dirty, 1); }
}
static void applied_load(void) {
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_APPLIED, g_dir); size_t len; char *src = read_file(p, 256 * 1024, &len); if (!src) return;
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES); char *text = (char *)malloc(J_TEXT); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES, text, J_TEXT, src, len) : -1;
  int a = root >= 0 ? j_get(&d, root, "applied") : -1;
  if (a >= 0 && d.n[a].type == J_OBJ) for (int x = d.n[a].first; x >= 0 && g_napplied < MS_MAXROWS * 2; x = d.n[x].next) { long long s; if (j_isint(&d, x, &s) && strlen(d.n[x].key) < sizeof g_applied[0].key) { snprintf(g_applied[g_napplied].key, sizeof g_applied[0].key, "%s", d.n[x].key); g_applied[g_napplied++].seq = s; } }
  else logf_("modsettings: " MS_APPLIED " is not readable: every dynasty change in values.json counts as not applied yet");
  free(nodes); free(text); free(src);
}
static int write_atomic(const char *path, const char *data, size_t n) {
  char tmp[MAX_PATH + 16]; snprintf(tmp, sizeof tmp, "%s.ms.tmp", path);
  FILE *f = fopen(tmp, "wb"); if (!f) return 0;
  int ok = fwrite(data, 1, n, f) == n; ok = fclose(f) == 0 && ok;
  if (!ok || !MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) { DeleteFileA(tmp); return 0; }
  return 1;
}
static void applied_save(void) {
  if (!InterlockedExchange(&g_applied_dirty, 0)) return;
  char *b = (char *)malloc(64 * 1024); if (!b) return; size_t o = 0;
  EnterCriticalSection(&g_applied_cs);
  o += (size_t)snprintf(b + o, 64 * 1024 - o, "{\"schema\":1,\"note\":\"written by modsettings.dll: the dynasty changes of values.json already written into a dynasty (by seq). Delete this file to apply every change again.\",\"applied\":{");
  for (int i = 0; i < g_napplied && o < 60 * 1024; i++) { char q[200]; j_quote(q, sizeof q, g_applied[i].key); o += (size_t)snprintf(b + o, 64 * 1024 - o, "%s%s:%lld", i ? "," : "", q, g_applied[i].seq); }
  LeaveCriticalSection(&g_applied_cs);
  o += (size_t)snprintf(b + o, 64 * 1024 - o, "}}\n");
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_APPLIED, g_dir);
  if (!write_atomic(p, b, o)) { logf_("modsettings: " MS_APPLIED " could not be written (it is retried)"); InterlockedExchange(&g_applied_dirty, 1); }
  free(b);
}

/* ------------------------------------------------------------------ ini bindings (global rows): rewrite ONE key, atomically */
/* the current value of key in the ini text (the first uncommented "key = value" line); 1 = found */
static int ini_get(const char *text, const char *key, char *out, size_t n) {
  const char *p = text; size_t kl = strlen(key);
  while (*p) {
    const char *e = p; while (*e && *e != '\n') e++;
    const char *s = p; while (s < e && (*s == ' ' || *s == '\t')) s++;
    if ((size_t)(e - s) > kl && !_strnicmp(s, key, kl)) {
      const char *q = s + kl; while (q < e && (*q == ' ' || *q == '\t')) q++;
      if (q < e && *q == '=') {
        q++; while (q < e && (*q == ' ' || *q == '\t')) q++;
        const char *v = q, *ve = q; while (ve < e && *ve != ';' && *ve != '#' && *ve != '\r') ve++;
        while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
        size_t vl = (size_t)(ve - v); if (vl >= n) vl = n - 1; memcpy(out, v, vl); out[vl] = 0; return 1;
      }
    }
    p = *e ? e + 1 : e;
  }
  return 0;
}
/* text with key set to val: the key's line replaced by "key = val" (its trailing comment kept), else the line appended. Caller frees. */
static char *ini_set(const char *text, const char *key, const char *val) {
  size_t tl = strlen(text), kl = strlen(key), vl = strlen(val); char *o = (char *)malloc(tl + kl + vl + 16); if (!o) return NULL;
  const char *p = text; size_t w = 0; int done = 0; int crlf = strstr(text, "\r\n") != NULL || !tl;
  while (*p) {
    const char *e = p; while (*e && *e != '\n') e++;
    const char *s = p; while (s < e && (*s == ' ' || *s == '\t')) s++;
    int hit = 0;
    if (!done && (size_t)(e - s) > kl && !_strnicmp(s, key, kl)) { const char *q = s + kl; while (q < e && (*q == ' ' || *q == '\t')) q++; if (q < e && *q == '=') hit = 1; }
    if (hit) {
      const char *c = s; while (c < e && *c != ';' && *c != '#') c++;
      memcpy(o + w, p, (size_t)(s - p)); w += (size_t)(s - p);
      w += (size_t)sprintf(o + w, "%s = %s", key, val);
      if (c < e) { const char *ce = e; if (ce > c && ce[-1] == '\r') ce--; o[w++] = ' '; memcpy(o + w, c, (size_t)(ce - c)); w += (size_t)(ce - c); }
      if (e > p && e[-1] == '\r') o[w++] = '\r';
      if (*e) o[w++] = '\n';
      done = 1;
    } else { size_t n = (size_t)(e - p) + (*e ? 1 : 0); memcpy(o + w, p, n); w += n; }
    p = *e ? e + 1 : e;
  }
  if (!done) { if (w && o[w - 1] != '\n') { if (crlf) o[w++] = '\r'; o[w++] = '\n'; } w += (size_t)sprintf(o + w, "%s = %s%s", key, val, crlf ? "\r\n" : "\n"); }
  o[w] = 0; return o;
}
static long g_ini_writes;
/* one global row with an ini binding: when values.json names a value and the ini says something else, rewrite that key. 1 = written */
static int ini_apply(const MsRow *r, int v, char *msg, size_t mn) {
  char plugins[MAX_PATH]; snprintf(plugins, sizeof plugins, "%s", g_dir); char *sl = strrchr(plugins, '\\'); if (sl) *sl = 0;
  char want[MAX_PATH * 2]; snprintf(want, sizeof want, "%s\\%s", plugins, r->ini_file); for (char *c = want; *c; c++) if (*c == '/') *c = '\\';
  char full[MAX_PATH]; if (!GetFullPathNameA(want, MAX_PATH, full, NULL)) { snprintf(msg, mn, "bad path"); return 0; }
  size_t pl = strlen(plugins); if (_strnicmp(full, plugins, pl) || full[pl] != '\\') { snprintf(msg, mn, "%s is outside the Plugins folder: not written", full); return 0; }
  size_t len = 0; char *text = read_file(full, 1024 * 1024, &len);
  if (!text) { snprintf(msg, mn, "%s does not exist (the mod is not installed?): not written", full); return 0; }
  char cur[64]; int have = ini_get(text, r->ini_key, cur, sizeof cur);
  if (have && !strcmp(cur, r->ini_val[v])) { free(text); snprintf(msg, mn, "%s already says %s = %s", full, r->ini_key, cur); return 0; }
  char *nt = ini_set(text, r->ini_key, r->ini_val[v]); free(text); if (!nt) { snprintf(msg, mn, "out of memory"); return 0; }
  int ok = write_atomic(full, nt, strlen(nt)); free(nt);
  if (ok) { g_ini_writes++; snprintf(msg, mn, "%s: %s = %s (was %s)", full, r->ini_key, r->ini_val[v], have ? cur : "not set"); }
  else snprintf(msg, mn, "%s could not be replaced (in use?): retried at the next change", full);
  return ok;
}
static void ini_apply_all(const MsCfg *c) {
  if (!g_k.ini_bind) return;
  for (int i = 0; i < c->nrows; i++) {
    const MsRow *r = &c->rows[i]; if (r->store != ST_GLOBAL || !r->has_ini || !c->gset[i]) continue;
    char m[600]; int w = ini_apply(r, c->gval[i], m, sizeof m);
    logf_("modsettings: ini binding %s/%s -> %s%s", r->mod, r->id, w ? "written: " : "", m);
  }
}

/* ------------------------------------------------------------------ the loaded dynasty: the band pass (server thread) */
typedef struct {
  int seen; SYSTEMTIME at; char flow[64];
  int is_ref, cad_rows, rec_count[MS_BAND_RECORDS], rec_val[MS_BAND_RECORDS];
  uintptr_t db;                    /* the user record's database interface (+0x10): changes when another dynasty is loaded */
} MsDyn;
static MsDyn g_dyn; static CRITICAL_SECTION g_dyn_cs; static volatile LONG g_status_dirty;
static volatile LONG g_n_pass, g_n_write, g_n_write_fail;
static char g_last_write[200];
typedef struct { void *flow, *a2; int site; } PassArgs;
static int band_lo(int rec) { return MS_BAND_BASE + MS_BAND_RECSIZE * rec; }
/* the bits after applying every pending change of record rec; marks[] = which rows it changed (for applied.json) */
typedef struct { int rec, bit, width, want; long long seq; char mod[MS_MID], id[MS_ID]; } PendRow;
static int band_apply_bits(int bits, const PendRow *p, int np, int rec) {
  for (int i = 0; i < np; i++) if (p[i].rec == rec) { int mask = ((1 << p[i].width) - 1) << p[i].bit; bits = (bits & ~mask) | ((p[i].want << p[i].bit) & mask); }
  return bits;
}
static long long g_pend_logged[MS_BAND_RECORDS];   /* the highest seq whose "record not there yet" was logged (no repeats) */
static void ms_pass(void *arg) {
  PassArgs *pa = (PassArgs *)arg; void *user = NULL;
  ((finduser_fn)FN(MS_F_FINDUSER))(pa->flow, pa->a2, &user);
  MsDyn d; memset(&d, 0, sizeof d); d.seen = 1; GetLocalTime(&d.at); snprintf(d.flow, sizeof d.flow, "%s", pa->site >= 0 ? g_pb->sites[pa->site].flow : "test");
  if (!user) { if (InterlockedIncrement(&g_n_pass) <= 20) logf_("modsettings: %s: no user record (FindUser): nothing read", d.flow); return; }
  memcpy(&d.db, (char *)user + 0x10, sizeof d.db);
  /* the pending dynasty changes (values.json seq above applied.json's), copied out of the configuration */
  PendRow pend[MS_MAXPEND]; int np = 0;   /* on the game's server thread: kept small (about 7 KB); more waiting changes land at the next pass */
  AcquireSRWLockShared(&g_cfg_lock);
  if (g_cfg) for (int i = 0; i < g_cfg->nrows && np < MS_MAXPEND; i++) {
    const MsRow *r = &g_cfg->rows[i]; if (r->store != ST_BAND || r->rec < 0 || g_cfg->want_seq[i] <= 0) continue;
    EnterCriticalSection(&g_applied_cs); long long a = applied_seq(r->mod, r->id); LeaveCriticalSection(&g_applied_cs);
    if (g_cfg->want_seq[i] <= a) continue;
    PendRow *p = &pend[np++]; p->rec = r->rec; p->bit = r->bit; p->width = r->width; p->want = g_cfg->want[i]; p->seq = g_cfg->want_seq[i];
    snprintf(p->mod, sizeof p->mod, "%s", r->mod); snprintf(p->id, sizeof p->id, "%s", r->id);
  }
  ReleaseSRWLockShared(&g_cfg_lock);
  int probe = rd_num(user, (void *)(uintptr_t)H_CAD, IF_ENUM, 0);
  d.is_ref = probe < 0;   /* a reference reads (packed ref | 0x80000000) - 1 (negative); INT_MAX = null (no rows) */
  void *hold[MS_BAND_RECORDS] = { 0 }; int raw[MS_BAND_RECORDS] = { 0 };
  void *cad = NULL;
  if (d.is_ref) {
    cad = arr_of(user, (void *)(uintptr_t)H_CAD); int nc = arr_count(cad);
    for (int i = 0; i < nc && i < 400; i++) {
      void *e = arr_get(cad, i); if (!e) continue; d.cad_rows++;
      int id = rd_num(e, (void *)(uintptr_t)H_CAD_ID, IF_ENUM, -1), v = rd_num(e, (void *)(uintptr_t)H_CAD_VAL, IF_INT, 0), rv = v;
      if (v >= 32768 && v <= 65535) v -= 65536;   /* Value is a signed 16-bit field: accept either reading of a negative */
      int kept = 0;
      if (id == 0 && v >= MS_BAND_BASE && v < MS_BAND_BASE + MS_BAND_RECORDS * MS_BAND_RECSIZE) {
        int r = (v - MS_BAND_BASE) / MS_BAND_RECSIZE; d.rec_count[r]++;
        if (d.rec_count[r] == 1) { hold[r] = e; raw[r] = rv; d.rec_val[r] = v; kept = 1; }
      }
      if (!kept) sc_release(e);
    }
  }
  for (int r = 0; r < MS_BAND_RECORDS; r++) {
    int has = 0; long long top = 0; for (int i = 0; i < np; i++) if (pend[i].rec == r) { has = 1; if (pend[i].seq > top) top = pend[i].seq; }
    if (!has) continue;
    if (d.rec_count[r] == 0) {
      if (top > g_pend_logged[r]) { g_pend_logged[r] = top; logf_("modsettings: band record %d is not in this dynasty yet (the mod that uses it adds it at its first Advance): %d change(s) wait", r, has); }
      continue;
    }
    if (d.rec_count[r] > 1) { if (top > g_pend_logged[r]) { g_pend_logged[r] = top; logf_("modsettings: band record %d appears %d times in this dynasty: NOT written (which one the mods read is not defined); the changes wait", r, d.rec_count[r]); } continue; }
    int lo = band_lo(r), bits = d.rec_val[r] - lo, nb = band_apply_bits(bits, pend, np, r), ok = 0;
    if (nb == bits) ok = 1;
    else if (!g_k.band_write) { if (top > g_pend_logged[r]) { g_pend_logged[r] = top; logf_("modsettings: band_write = 0: record %d would change %d -> %d; not written", r, d.rec_val[r], lo + nb); } continue; }
    else {
      void *ifc; int row; void **ivt = sc_ivt(hold[r], &ifc, &row);
      if (ivt && (g_t_fake || in_exe(ivt[IF_SETINT / 8]))) {
        int nv = lo + nb, wv = raw[r] >= 32768 ? nv + 65536 : nv;   /* the representation the field read back in */
        ((setint_fn)ivt[IF_SETINT / 8])(ifc, row, (void *)(uintptr_t)H_CAD_VAL, wv); ((dirty_fn)FN(MS_F_DIRTY))(ifc);
        int back = rd_num(hold[r], (void *)(uintptr_t)H_CAD_VAL, IF_INT, 0); if (back >= 32768 && back <= 65535) back -= 65536;
        ok = back == nv; InterlockedIncrement(ok ? &g_n_write : &g_n_write_fail);
        snprintf(g_last_write, sizeof g_last_write, "record %d %d -> %d (read back %d)%s", r, d.rec_val[r], nv, back, ok ? "" : " READ-BACK DIFFERS");
        logf_("modsettings: band write on %s, thread %lu: %s", d.flow, GetCurrentThreadId(), g_last_write);
        if (ok) d.rec_val[r] = nv;
      } else logf_("modsettings: band record %d: its record interface is not the game's: not written", r);
    }
    if (ok) { EnterCriticalSection(&g_applied_cs); for (int i = 0; i < np; i++) if (pend[i].rec == r) applied_set(pend[i].mod, pend[i].id, pend[i].seq); LeaveCriticalSection(&g_applied_cs); }
  }
  for (int r = 0; r < MS_BAND_RECORDS; r++) if (hold[r]) sc_release(hold[r]);
  if (cad) sc_release(cad);
  sc_release(user);
  LONG n = InterlockedIncrement(&g_n_pass);
  if (n <= (LONG)g_k.log_captures || np) logf_("modsettings: pass #%ld on %s, thread %lu: CrossArcData %s%d row(s); band record 0: %d x%s; record 1: %d x%s; %d pending change(s)", n, d.flow, GetCurrentThreadId(), d.is_ref ? "" : "(none) ", d.cad_rows,
                                               d.rec_count[0], d.rec_count[0] ? "" : " (not there yet)", d.rec_count[1], d.rec_count[1] ? "" : " (not there yet)", np);
  EnterCriticalSection(&g_dyn_cs); g_dyn = d; LeaveCriticalSection(&g_dyn_cs); InterlockedExchange(&g_status_dirty, 1);
}
/* the capture: the redirected handler call (server thread). The handler's own result is returned unchanged, always. */
static handler_fn g_orig[MS_NSITES];
static volatile LONG g_ncap[MS_NSITES];
static CRITICAL_SECTION g_pass_cs;
static volatile ULONGLONG g_last_pass_tick;
static int pending_any(void) {
  int any = 0; AcquireSRWLockShared(&g_cfg_lock);
  if (g_cfg) for (int i = 0; i < g_cfg->nrows && !any; i++) if (g_cfg->rows[i].store == ST_BAND && g_cfg->rows[i].rec >= 0 && g_cfg->want_seq[i] > 0) { EnterCriticalSection(&g_applied_cs); any = g_cfg->want_seq[i] > applied_seq(g_cfg->rows[i].mod, g_cfg->rows[i].id); LeaveCriticalSection(&g_applied_cs); }
  ReleaseSRWLockShared(&g_cfg_lock); return any;
}
static uintptr_t ms_capture(int site, void *flow, void *a2, void *req, void *resp) {
  uintptr_t r = g_orig[site](flow, a2, req, resp);
  LONG n = InterlockedIncrement(&g_ncap[site]);
  if (n <= (LONG)g_k.log_captures) logf_("modsettings: %s #%ld on thread %lu returned %llu", g_pb->sites[site].flow, n, GetCurrentThreadId(), (unsigned long long)r);
  if (r != 1 || g_disabled) return r;
  ULONGLONG now = GetTickCount64();
  if (now - g_last_pass_tick < 2000 && !pending_any()) return r;   /* a refresh storm: one read every 2 s unless a change waits */
  if (!TryEnterCriticalSection(&g_pass_cs)) return r;               /* never make a server thread wait on another pass */
  g_last_pass_tick = now;
  PassArgs pa = { flow, a2, site }; ms_guarded(ms_pass, &pa, "the band pass");
  LeaveCriticalSection(&g_pass_cs);
  return r;
}
static uintptr_t ms_cap0(void *f, void *a, void *q, void *s) { return ms_capture(0, f, a, q, s); }
static uintptr_t ms_cap1(void *f, void *a, void *q, void *s) { return ms_capture(1, f, a, q, s); }
static uintptr_t ms_cap2(void *f, void *a, void *q, void *s) { return ms_capture(2, f, a, q, s); }
static uintptr_t ms_cap3(void *f, void *a, void *q, void *s) { return ms_capture(3, f, a, q, s); }
static void *const MS_CAP_FN[MS_NSITES] = { (void *)ms_cap0, (void *)ms_cap1, (void *)ms_cap2, (void *)ms_cap3 };
static const char *const MS_CAP_KEY[MS_NSITES] = { "capture_meters", "capture_myschool", "capture_roster", "capture_schedule" };

/* ------------------------------------------------------------------ ms_get: the one export (DLL mods in the same process) */
/* the value (option index) of mod/row: a global row = values.json or its default; a band row = its bits in the loaded dynasty's record as
 * last read (the default when the record is not there yet, -2 when no dynasty was read yet); -1 = no such row (or the manifest was refused) */
__declspec(dllexport) int ms_get(const char *mod, const char *row) {
  if (!mod || !row) return -1;
  int v = -1; AcquireSRWLockShared(&g_cfg_lock);
  if (g_cfg) { int i = row_index(g_cfg, mod, row); if (i >= 0) {
    const MsRow *r = &g_cfg->rows[i];
    if (r->store == ST_GLOBAL) v = g_cfg->gval[i];
    else if (r->rec < 0) v = g_cfg->want_seq[i] > 0 ? g_cfg->want[i] : r->dflt;   /* a v3 dynasty row whose slot is unknown: settings.json only */
    else { EnterCriticalSection(&g_dyn_cs); if (!g_dyn.seen) v = -2; else if (g_dyn.rec_count[r->rec] != 1) v = r->dflt; else { v = ((g_dyn.rec_val[r->rec] - band_lo(r->rec)) >> r->bit) & ((1 << r->width) - 1); if (v >= r->nopt) v = r->dflt; } LeaveCriticalSection(&g_dyn_cs); }
  } }
  ReleaseSRWLockShared(&g_cfg_lock); return v;
}

/* ------------------------------------------------------------------ install */
static int tab_install(void);   /* mstab.h: the layer 2 tab feed (its own pins; retried by the worker until the adapter exists) */
static int g_installed_sites;
static void ms_install(void) {
  char why[300];
  if (!g_k.enabled) { logf_("modsettings: enabled = 0 in " MS_INI ": nothing looked up, nothing installed (values.json and ini bindings still work)"); snprintf(g_state, sizeof g_state, "idle"); return; }
  if (!ms_pins_check(why, sizeof why)) { logf_("modsettings: REFUSED, nothing installed: %s (values.json, ini bindings and ms_get still work; the band is not written)", why); snprintf(g_state, sizeof g_state, "refused"); snprintf(g_reason, sizeof g_reason, "%s", why); return; }
  logf_("modsettings: pins ok (%s, PE %u: %d game functions)", g_pb->name, g_pb->stamp, MS_NFUNCS);
  ms_guard_init();
  int want = 0;
  for (int i = 0; i < MS_NSITES; i++) {
    if (!g_k.cap[i]) { logf_("modsettings: %s off (%s = 0)", g_pb->sites[i].flow, MS_CAP_KEY[i]); continue; }
    want++;
    if (!g_site_ok[i]) { logf_("modsettings: %s SKIPPED: %s", g_pb->sites[i].flow, g_site_why[i]); continue; }
    unsigned char *site = (unsigned char *)AT(g_pb->sites[i].call_rva); void *tramp = NULL;
    g_orig[i] = (handler_fn)AT(g_pb->sites[i].handler_rva);
    char w[96]; snprintf(w, sizeof w, "modsettings: %s handler call", g_pb->sites[i].flow);
    g_patch_what = w; if (patch_call(site, MS_CAP_FN[i], &tramp)) g_installed_sites++;
    else { g_orig[i] = NULL; logf_("modsettings: %s was NOT patched", g_pb->sites[i].flow); }   /* g_orig is set first: a server thread may enter the wrapper the moment the call is patched */
  }
  snprintf(g_state, sizeof g_state, g_installed_sites == want && want ? "installed" : g_installed_sites ? "partial" : "failed");
  if (!g_installed_sites) snprintf(g_reason, sizeof g_reason, "no capture site installed: the band is not written");
  logf_("modsettings: install %s: %d of %d capture site(s); band_write %d", g_state, g_installed_sites, want, g_k.band_write);
}

/* ------------------------------------------------------------------ modsettings.ini (read once) */
static void ini_load(const char *path) {
  struct { const char *key; int *v; } KEYS[] = { { "enabled", &g_k.enabled }, { "band_write", &g_k.band_write }, { "ini_bind", &g_k.ini_bind }, { "log_captures", &g_k.log_captures },
    { "poll_ms", &g_k.poll_ms }, { "capture_meters", &g_k.cap[0] }, { "capture_myschool", &g_k.cap[1] }, { "capture_roster", &g_k.cap[2] }, { "capture_schedule", &g_k.cap[3] },
    { "log_sync", &g_log_sync }, { "patch_safe", &g_ps.safe }, { "patch_verify", &g_ps.verify },
    { "tab", &g_kt.on }, { "slider_native", &g_kt.slider_native }, { "log_tab", &g_kt.log_calls }, { "show_inactive", &g_kt.show_inactive } };
  FILE *f = fopen(path, "r"); if (!f) return; g_ini_found = 1; char line[512];
  while (fgets(line, sizeof line, f)) {
    char *c = strpbrk(line, ";#"); if (c) *c = 0;
    char *eq = strchr(line, '='); if (!eq) continue; *eq = 0;
    char *k = line; while (*k == ' ' || *k == '\t') k++; char *e = k + strlen(k); while (e > k && (e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    char *v = eq + 1; while (*v == ' ' || *v == '\t') v++;
    char *end; long n = strtol(v, &end, 10); if (end == v) continue;
    for (size_t i = 0; i < sizeof KEYS / sizeof *KEYS; i++) if (!_stricmp(k, KEYS[i].key)) *KEYS[i].v = (int)n;
  }
  fclose(f);
  if (g_k.poll_ms < 250) g_k.poll_ms = 250; if (g_k.poll_ms > 10000) g_k.poll_ms = 10000;
}

/* ------------------------------------------------------------------ the status file (the companion reads it) */
typedef struct { char *b; size_t n, cap; } Sb;
static void sb_add(Sb *s, const char *fmt, ...) {
  va_list ap; va_start(ap, fmt); char tmp[2048]; int k = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap); if (k < 0) return; if ((size_t)k >= sizeof tmp) k = sizeof tmp - 1;
  if (s->n + (size_t)k + 1 > s->cap) { size_t nc = (s->cap ? s->cap * 2 : 16384); while (nc < s->n + (size_t)k + 1) nc *= 2; char *nb = (char *)realloc(s->b, nc); if (!nb) return; s->b = nb; s->cap = nc; }
  memcpy(s->b + s->n, tmp, (size_t)k); s->n += (size_t)k; s->b[s->n] = 0;
}
static void sb_q(Sb *s, const char *str) { char q[2400]; if (!j_quote(q, sizeof q, str ? str : "")) snprintf(q, sizeof q, "\"(too long)\""); sb_add(s, "%s", q); }
static void tab_status(Sb *s);   /* mstab.h */
static const char *g_st_dir;
static void status_write(void) {
  Sb s = { 0 }; SYSTEMTIME u; GetSystemTime(&u);
  sb_add(&s, "{\"product\":\"" MS_STEM "\",\"source\":\"dll\",\"version\":\"" MS_VERSION "\",\"pid\":%lu,\"updated\":\"%04d-%02d-%02dT%02d:%02d:%02dZ\",\"state\":", GetCurrentProcessId(), u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute, u.wSecond);
  sb_q(&s, g_state); sb_add(&s, ",\"reason\":"); sb_q(&s, g_reason);
  sb_add(&s, ",\"build\":"); sb_q(&s, g_base ? g_pb->name : "");
  sb_add(&s, ",\"settings\":{\"enabled\":%d,\"band_write\":%d,\"ini_bind\":%d,\"poll_ms\":%d},\"sites\":[", g_k.enabled, g_k.band_write, g_k.ini_bind, g_k.poll_ms);
  for (int i = 0; i < MS_NSITES; i++) { sb_add(&s, "%s{\"flow\":", i ? "," : ""); sb_q(&s, MS_BUILDS[0].sites[i].flow); sb_add(&s, ",\"on\":%d,\"installed\":%s,\"captures\":%ld}", g_k.cap[i], g_orig[i] ? "true" : "false", (long)g_ncap[i]); }
  sb_add(&s, "],\"counts\":{\"passes\":%ld,\"band_writes\":%ld,\"band_write_fails\":%ld,\"ini_writes\":%ld,\"faults\":%ld,\"disabled\":%ld},\"last_write\":", (long)g_n_pass, (long)g_n_write, (long)g_n_write_fail, g_ini_writes, (long)g_faults, (long)g_disabled);
  sb_q(&s, g_last_write);
  MsDyn d; EnterCriticalSection(&g_dyn_cs); d = g_dyn; LeaveCriticalSection(&g_dyn_cs);
  sb_add(&s, ",\"dynasty\":{\"seen\":%s", d.seen ? "true" : "false");
  if (d.seen) {
    sb_add(&s, ",\"at\":\"%02d:%02d:%02d\",\"flow\":", d.at.wHour, d.at.wMinute, d.at.wSecond); sb_q(&s, d.flow);
    sb_add(&s, ",\"crossarcdata\":%s,\"table_rows\":%d,\"records\":[", d.is_ref ? "true" : "false", d.cad_rows);
    for (int r = 0; r < MS_BAND_RECORDS; r++) { sb_add(&s, "%s{\"record\":%d,\"count\":%d", r ? "," : "", r, d.rec_count[r]); if (d.rec_count[r] == 1) sb_add(&s, ",\"value\":%d,\"bits\":%d", d.rec_val[r], d.rec_val[r] - band_lo(r)); sb_add(&s, "}"); }
    sb_add(&s, "]");
  }
  sb_add(&s, "},\"manifests\":{\"loaded\":[");
  AcquireSRWLockShared(&g_cfg_lock);
  const MsCfg *c = g_cfg;
  if (c) for (int m = 0; m < c->nmods; m++) { sb_add(&s, "%s", m ? "," : ""); sb_q(&s, c->mods[m].id); }
  sb_add(&s, "],\"problems\":[");
  if (c && c->problems[0]) { const char *p = c->problems; int first = 1; while (*p) { const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p); char one[420]; if (n >= sizeof one) n = sizeof one - 1; memcpy(one, p, n); one[n] = 0; sb_add(&s, "%s", first ? "" : ","); sb_q(&s, one); first = 0; p += n; if (*p == '\n') p++; } }
  sb_add(&s, "]},\"rows\":[");
  if (c) for (int i = 0; i < c->nrows; i++) {
    const MsRow *r = &c->rows[i]; sb_add(&s, "%s{\"mod\":", i ? "," : ""); sb_q(&s, r->mod); sb_add(&s, ",\"row\":"); sb_q(&s, r->id);
    if (r->store == ST_GLOBAL) sb_add(&s, ",\"store\":\"global\",\"value\":%d,\"from\":\"%s\"", c->gval[i], c->gset[i] ? "values" : "default");
    else {
      int v = -1; const char *from = "no dynasty read yet";
      if (r->rec < 0) { v = c->want_seq[i] > 0 ? c->want[i] : r->dflt; from = "no band slot (settings.json only)"; }
      else if (d.seen && d.rec_count[r->rec] == 1) { v = ((d.rec_val[r->rec] - band_lo(r->rec)) >> r->bit) & ((1 << r->width) - 1); from = "dynasty"; }
      else if (d.seen) from = d.rec_count[r->rec] ? "record twice" : "record not there yet";
      EnterCriticalSection(&g_applied_cs); long long a = applied_seq(r->mod, r->id); LeaveCriticalSection(&g_applied_cs);
      sb_add(&s, ",\"store\":\"band\",\"record\":%d,\"bit\":%d,\"width\":%d,\"value\":%d,\"from\":\"%s\",\"default\":%d,\"wish\":%d,\"wish_seq\":%lld,\"applied_seq\":%lld,\"pending\":%s",
             r->rec, r->bit, r->width, v, from, r->dflt, c->want[i], c->want_seq[i], a, c->want_seq[i] > a ? "true" : "false");
    }
    sb_add(&s, "}");
  }
  ReleaseSRWLockShared(&g_cfg_lock);
  sb_add(&s, "]"); tab_status(&s); sb_add(&s, "}\n");   /* tab_status takes the configuration lock itself (never nested) */
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_STATUS, g_st_dir ? g_st_dir : g_dir);
  if (s.b) write_atomic(p, s.b, s.n);
  free(s.b);
}
#include "mstab.h"   /* layer 2: the in-game Mod Settings tab feed, settings.json writer, change journal */
/* reload the configuration when its files changed (worker thread) */
static unsigned long long g_inputs;
static int cfg_poll(int force) {
  if (InterlockedExchange(&g_cfg_force, 0)) force = 1;
  unsigned long long st = inputs_stamp(); if (!force && st == g_inputs) return 0;
  g_inputs = st; MsCfg *c = cfg_build(); if (!c) return 0;
  EnterCriticalSection(&g_jq_cs); journal_apply(c, g_jq, g_njq); LeaveCriticalSection(&g_jq_cs);   /* tab changes not written yet stay visible */
  AcquireSRWLockExclusive(&g_cfg_lock); MsCfg *old = g_cfg; g_cfg = c; ReleaseSRWLockExclusive(&g_cfg_lock); free(old);
  logf_("modsettings: configuration %s: %d mod(s) [%s], %d row(s), %d problem(s)%s%s", force ? "read" : "re-read (a file changed)", c->nmods, c->loaded, c->nrows, c->nproblems, c->nproblems ? ": " : "", c->nproblems ? c->problems : "");
  ini_apply_all(c);
  InterlockedExchange(&g_status_dirty, 1);
  return 1;
}

#ifndef AP_TEST
static int g_game_host;   /* the host is CollegeFB27.exe (the worker installs only then) */
static DWORD WINAPI worker(LPVOID arg) {
  (void)arg; ULONGLONG t0 = GetTickCount64(), last_status = 0; int done = !g_game_host || !g_k.enabled, said = 0, tab_done = !g_game_host;
  if (g_game_host && !g_k.enabled) ms_install();
  cfg_poll(1);
  for (;;) {
    if (!done) {
      char why[300];
      if (ms_pins_check(why, sizeof why)) { done = 1; ms_install(); }
      else if (GetTickCount64() - t0 > 10ull * 60000ull) { done = 1; logf_("modsettings: gave up after 10 minutes, nothing installed: %s", why); snprintf(g_state, sizeof g_state, "failed"); snprintf(g_reason, sizeof g_reason, "%s", why); }
      else if (!said) { said = 1; snprintf(g_state, sizeof g_state, "waiting"); logf_("modsettings: waiting for the game's code (%s)", why); }
    }
    if (done && !tab_done) { tab_done = tab_install();   /* after the band pins passed once (the code is unpacked), or their 10 minutes ran out */
      if (!tab_done && GetTickCount64() - t0 > 20ull * 60000ull) { tab_done = 1; snprintf(g_tab_state, sizeof g_tab_state, "failed"); logf_("modsettings: tab feed gave up after 20 minutes: %s", g_tab_reason); } }
    journal_flush();   /* tab changes -> settings.json, before any re-read */
    cfg_poll(0); applied_save();
    ULONGLONG now = GetTickCount64();
    if (InterlockedExchange(&g_status_dirty, 0) || now - last_status > 5000) { status_write(); last_status = now; }
    log_flush();
    WaitForSingleObject(g_wake, (DWORD)g_k.poll_ms);   /* a tab change wakes the worker at once */
  }
  return 0;
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID reserved) {
  (void)reserved;
  if (reason == DLL_PROCESS_ATTACH) {
    DisableThreadLibraryCalls(h);
    char mname[64]; snprintf(mname, sizeof mname, "Local\\" MS_MUTEX ".%lu", GetCurrentProcessId());
    HANDLE mutex = CreateMutexA(NULL, FALSE, mname);
    if (mutex && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(mutex); return FALSE; }
    InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_dyn_cs); InitializeCriticalSection(&g_pass_cs); InitializeCriticalSection(&g_applied_cs); InitializeCriticalSection(&g_jq_cs);
    g_wake = CreateEventA(NULL, FALSE, FALSE, NULL);
    GetModuleFileNameA(h, g_dir, MAX_PATH); char *sl = strrchr(g_dir, '\\'); if (sl) *sl = 0;
    char ip[MAX_PATH]; snprintf(ip, sizeof ip, "%s\\" MS_INI, g_dir); ini_load(ip);
    char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\" MS_LOG, g_dir); g_log = fopen(lp, "a");
    if (g_log && !g_log_sync) setvbuf(g_log, NULL, _IOFBF, 1 << 16);
    char exe[MAX_PATH]; GetModuleFileNameA(NULL, exe, MAX_PATH); const char *base = strrchr(exe, '\\'); base = base ? base + 1 : exe;
    g_game_host = !_stricmp(base, "CollegeFB27.exe");
    logf_("loaded into %s (pid %lu): " MS_PRODUCT " " MS_VERSION " from %s; %s: enabled=%d band_write=%d ini_bind=%d capture meters/myschool/roster/schedule=%d%d%d%d poll_ms=%d patch_safe=%d patch_verify=%d tab=%d slider_native=%d",
          exe, GetCurrentProcessId(), g_dir, g_ini_found ? MS_INI " read" : "no " MS_INI " (defaults)", g_k.enabled, g_k.band_write, g_k.ini_bind, g_k.cap[0], g_k.cap[1], g_k.cap[2], g_k.cap[3], g_k.poll_ms, g_ps.safe, g_ps.verify, g_kt.on, g_kt.slider_native);
    applied_load();
    if (!g_game_host) { logf_("modsettings: the host is not CollegeFB27.exe: game not found, nothing is installed"); snprintf(g_state, sizeof g_state, "failed"); snprintf(g_reason, sizeof g_reason, "game not found (the host is %s)", base); }
    HANDLE t = CreateThread(NULL, 0, worker, NULL, 0, NULL); if (t) CloseHandle(t);
  } else if (reason == DLL_PROCESS_DETACH && g_log && TryEnterCriticalSection(&g_cs)) { fflush(g_log); LeaveCriticalSection(&g_cs); }
  return TRUE;
}
#else
#include "selftest.h"
#endif
