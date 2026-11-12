/* selftest_tune.h: the tuning writer's self-test (included by selftest.h). modsettings-test tuning <dump> <dir>
 * The game's code and reflection data come from a reference dump (mapped like the dump test); the AttribSys registry and the tuning instances
 * are a FAKE heap built here: a registry object with the game's map layout (registry + 0x18 buckets, + 0x20 count, node { key, value, next },
 * djb2-xor of the lower-cased key), instances with a vtable inside the image and Asset.Name at +0x18. The field offsets used to fill the fakes
 * are the ones index/ebx-sdk-schema.json lists (an independent source): the writer itself finds them in the dump's typeinfo / fieldinf. */
#ifndef MS_SELFTEST_TUNE_H
#define MS_SELFTEST_TUNE_H
typedef struct TNode { char *key; void *val; struct TNode *next; } TNode;
#define T_NB 31
static unsigned char *g_tr_reg; static TNode **g_tr_bk;
static uint32_t t_hash(const char *k) { return tm_hash(k); }
static void t_reg_put(const char *key, void *val) {
  for (uint32_t b = 0; b < T_NB; b++) for (TNode *n = g_tr_bk[b]; n; n = n->next) if (!strcmp(n->key, key)) { n->val = val; return; }
  TNode *n = (TNode *)calloc(1, sizeof *n); n->key = _strdup(key); n->val = val; uint32_t b = t_hash(key) % T_NB; n->next = g_tr_bk[b]; g_tr_bk[b] = n;
}
/* an instance of a tuning asset: 0x1000 bytes, vtable = an address inside the image, Name = the asset path */
static unsigned char *t_inst(const char *path) {
  unsigned char *o = (unsigned char *)calloc(1, 0x1000); uintptr_t vt = (uintptr_t)g_hdr_base + g_pb->tune.core_rva; memcpy(o, &vt, 8);
  char *nm = _strdup(path); memcpy(o + 0x18, &nm, 8); return o;
}
/* a FloatCurve object (0x28 bytes, Points at +0x18) with n points (0x1c bytes each, X +4, Y +0x10); the count is the u32 before the data */
static unsigned char *t_curve(int n, float y0) {
  unsigned char *c = (unsigned char *)calloc(1, 0x28), *arr = (unsigned char *)calloc(1, 16 + 0x1c * (size_t)n); uint32_t cnt = (uint32_t)n; memcpy(arr + 12, &cnt, 4);
  unsigned char *data = arr + 16; for (int i = 0; i < n; i++) { float x = (float)i / (float)(n > 1 ? n - 1 : 1), y = y0 - 0.01f * (float)i; memcpy(data + 0x1c * i + 4, &x, 4); memcpy(data + 0x1c * i + 0x10, &y, 4); }
  memcpy(c + 0x18, &data, 8); return c;
}
static unsigned char *t_farr(int n, float v0) { unsigned char *arr = (unsigned char *)calloc(1, 16 + 4 * (size_t)n); uint32_t cnt = (uint32_t)n | 0x80000000u; memcpy(arr + 12, &cnt, 4); for (int i = 0; i < n; i++) { float v = v0 + (float)i; memcpy(arr + 16 + 4 * i, &v, 4); } return arr + 16; }
static float t_f(const unsigned char *p, int off) { float f; memcpy(&f, p + off, 4); return f; }
static void t_setf(unsigned char *p, int off, float f) { memcpy(p + off, &f, 4); }
static int32_t t_i(const unsigned char *p, int off) { int32_t x; memcpy(&x, p + off, 4); return x; }
static float t_curve_y(const unsigned char *c, int i) { unsigned char *d; memcpy(&d, c + 0x18, 8); return t_f(d, 0x1c * i + 0x10); }
static int t_rowi(const char *mod, const char *key) { AcquireSRWLockShared(&g_cfg_lock); int i = g_cfg ? row_index(g_cfg, mod, key) : -1; ReleaseSRWLockShared(&g_cfg_lock); return i; }
static int t_tset(const char *mod, const char *key, int v) { int i = t_rowi(mod, key); return i >= 0 && tab_set(i, v, "self-test"); }
static const MsRow *t_mrow(const char *mod, const char *key) { int i = t_rowi(mod, key); return i >= 0 ? &g_cfg->rows[i] : NULL; }
static const TuRow *t_trow(const char *mod, const char *key) { for (int i = 0; i < g_ntr; i++) if (!strcmp(g_tr[i].mod, mod) && !strcmp(g_tr[i].id, key)) return &g_tr[i]; return NULL; }
#define TB(a, f, cp) "\"bind\": { \"type\": \"tuning\", \"asset\": \"" a "\", \"field\": \"" f "\", \"copy\": \"" cp "\""
static int selftest_tuning(const char *file, const char *dir) {
  t_init_common(dir); t_mkdirs(dir); ms_guard_init(); g_kt.log_calls = 0;
  if (!ps_t_map_dump(file)) { printf("  FAIL: cannot map %s\n", file); return 1; }
  char why[300]; T_CHECK(ms_pins_check(why, sizeof why), "pins ok (%s, PE %u)", g_pb->name, g_pb->stamp);
  { char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\tuning.log", dir); g_log = fopen(lp, "w"); g_log_sync = 1; }
  t_put(dir, "mods\\tune-a.json", "{ \"schema\": 3, \"modid\": \"tune-a\", \"title\": \"Tune A\", \"version\": \"1\", \"rows\": [\n"
    " { \"type\": \"list\", \"key\": \"fumble\", \"label\": \"Fumbles\", \"options\": [\"Half\", \"Stock\", \"x20\"], \"default\": 1, \"applies\": \"now\", " TB("ballrules", "Fumble_ChanceMultiplier", "both") ", \"values\": [0.5, 1.0, 20.0] } },\n"
    " { \"type\": \"toggle\", \"key\": \"lineman\", \"label\": \"Lineman Catch\", \"default\": false, " TB("football/gameplay/attribsys/data/catching/catching/catching", "shouldLinemanMultiplayerCatch", "both") ", \"values\": [false, true] } },\n"
    " { \"type\": \"slider\", \"key\": \"curve\", \"label\": \"Curve Y\", \"min\": 0, \"max\": 100, \"step\": 10, \"default\": 50, " TB("ballrules", "Fumble_BaseChancePointGraph", "simulation") ", \"instance\": \"Curve.Points[1].y\", \"scale\": { \"at-min\": 0.0, \"at-max\": 1.0 } } },\n"
    " { \"type\": \"list\", \"key\": \"kicks\", \"label\": \"Kick Blocks\", \"options\": [\"Few\", \"Huge\"], \"default\": 0, " TB("gameplay_tuning", "MaxKickBlocksPerTeam", "base") ", \"values\": [2.6, 30000000000] } },\n"
    " { \"type\": \"list\", \"key\": \"dmove\", \"label\": \"Double Move\", \"options\": [\"Neg\", \"Seven\"], \"default\": 0, " TB("gameplay_tuning", "DoubleMoveChanceMax", "base") ", \"values\": [-5, 7] } },\n"
    " { \"type\": \"list\", \"key\": \"rush2\", \"label\": \"Rush [2]\", \"options\": [\"Low\", \"High\"], \"default\": 0, " TB("gameplay_tuning_gamestyle_simulation", "rushMove_AIControlledDefTeamBaseChance", "simulation") ", \"instance\": \"[2]\", \"values\": [0.1, 0.9] } },\n"
    " { \"type\": \"list\", \"key\": \"tempo\", \"label\": \"Tempo DC\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("coachmodifiers", "tempo_firstDownCoachBonus_DC", "simulation") ", \"values\": [0.25, 0.75] } },\n"
    " { \"type\": \"list\", \"key\": \"nofield\", \"label\": \"No Field\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("ballrules", "No_Such_Field", "both") ", \"values\": [1, 2] } },\n"
    " { \"type\": \"list\", \"key\": \"noasset\", \"label\": \"No Asset\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("nosuchasset", "Fumble_ChanceMultiplier", "base") ", \"values\": [1, 2] } },\n"
    " { \"type\": \"list\", \"key\": \"pastend\", \"label\": \"Past End\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("ballrules", "Fumble_BaseChancePointGraph", "simulation") ", \"instance\": \"Points[9].Y\", \"values\": [1, 2] } },\n"
    " { \"type\": \"list\", \"key\": \"wholecurve\", \"label\": \"Whole Curve\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("ballrules", "Fumble_BaseChancePointGraph", "base") ", \"values\": [1, 2] } },\n"
    " { \"type\": \"list\", \"key\": \"enumpt\", \"label\": \"Curve Type\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("ballrules", "Fumble_BaseChancePointGraph", "base") ", \"instance\": \"Points[0].CurveType\", \"values\": [1, 2] } },\n"
    " { \"type\": \"list\", \"key\": \"text\", \"label\": \"Text\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("ballrules", "Fumble_ChanceMultiplier", "base") ", \"values\": [\"a\", \"b\"] } },\n"
    " { \"type\": \"list\", \"key\": \"bad\", \"label\": \"Bad Inst\", \"options\": [\"A\", \"B\"], \"default\": 0, " TB("blocking", "Fumble_ChanceMultiplier", "base") ", \"values\": [1, 2] } } ] }");
  t_put(dir, "mods\\tune-b.json", "{ \"schema\": 3, \"modid\": \"tune-b\", \"title\": \"Tune B\", \"version\": \"1\", \"rows\": [\n"
    " { \"type\": \"list\", \"key\": \"fumble-b\", \"label\": \"Fumbles B\", \"options\": [\"Low\", \"High\"], \"default\": 0, " TB("ballrules", "Fumble_ChanceMultiplier", "simulation") ", \"values\": [0.25, 3.0] } } ] }");
  cfg_poll(1);
  T_CHECK(g_cfg && g_cfg->nmods == 2 && g_cfg->nproblems == 0, "manifests: 2 mods, no problem (%s)", g_cfg ? g_cfg->problems : "-");
  { const MsRow *r = t_mrow("tune-a", "fumble"); T_CHECK(r && r->asis && r->nopt == 4 && r->tkind == 0 && r->tv[2] == 20.0, "fumble: As installed + 3 options, numbers, values kept (20)"); }
  { const MsRow *r = t_mrow("tune-a", "curve"); T_CHECK(r && r->slo == 0.0 && r->shi == 1.0 && r->nopt == 12, "curve: a slider of 11 stops + As installed, scale 0..1"); }
  { const MsRow *r = t_mrow("tune-a", "fumble"); T_CHECK(r && r->s_also >= 0 && strstr(pool_str(g_cfg, r->s_also), "Tune B"), "rule 1 (config): fumble is also set by Tune B"); }
  /* 1. the pins */
  T_CHECK(tune_install() && g_tu_ready && !strcmp(g_tu_state, "waiting"), "tuning pins ok: lookup core, hash find (0x%x bytes), typeinfo + fieldinf (state %s: %s)", MS_TUNE_FINDN, g_tu_state, g_tu_reason);
  /* 2. the registry not filled yet: nothing written, rows wait */
  uintptr_t *glob = (uintptr_t *)AT(g_pb->tune.reg_rva); uintptr_t keep_glob = *glob; *glob = 0;
  tune_pass(); T_CHECK(!g_tu_regready && g_tu_writes == 0 && g_ntr == 0 && !strcmp(g_tu_state, "waiting"), "registry global 0: waiting, nothing looked up or written");
  /* the fake registry + instances */
  g_tr_reg = (unsigned char *)calloc(1, 0x40); g_tr_bk = (TNode **)calloc(T_NB + 1, sizeof *g_tr_bk); uintptr_t bk = (uintptr_t)g_tr_bk; uint32_t nb = T_NB;
  memcpy(g_tr_reg + 0x18, &bk, 8); memcpy(g_tr_reg + 0x20, &nb, 4);
  const char *P = "football/gameplay/attribsys/data/";
  char pa[200];
  snprintf(pa, sizeof pa, "%sballrules/ballrules/ballrules", P); unsigned char *br = t_inst(pa);
  snprintf(pa, sizeof pa, "%sballrules/ballrules/ballrules_gamestyle_simulation", P); unsigned char *brs = t_inst(pa);
  snprintf(pa, sizeof pa, "%scatching/catching/catching", P); unsigned char *ca = t_inst(pa);
  snprintf(pa, sizeof pa, "%scatching/catching/catching_gamestyle_simulation", P); unsigned char *cas = t_inst(pa);
  snprintf(pa, sizeof pa, "%sgameplay_tuning/gameplay_tuning/gameplay_tuning", P); unsigned char *gt = t_inst(pa);
  snprintf(pa, sizeof pa, "%sgameplay_tuning/gameplay_tuning/gameplay_tuning_gamestyle_simulation", P); unsigned char *gts = t_inst(pa);
  snprintf(pa, sizeof pa, "%scoachmodifiers/coachmodifiers", P); unsigned char *cm = t_inst(pa);
  snprintf(pa, sizeof pa, "%sblocking/blocking/blocking", P); unsigned char *bl = t_inst(pa); { char *wrong = _strdup("football/gameplay/attribsys/data/tackle/tackle/tackle"); memcpy(bl + 0x18, &wrong, 8); }
  t_setf(br, 0x49c, 1.5f); t_setf(brs, 0x49c, 1.0f);   /* ballrules.Fumble_ChanceMultiplier (Float32 +0x49c) */
  unsigned char *cb = t_curve(5, 0.25f), *cs = t_curve(5, 0.30f); memcpy(br + 0x28, &cb, 8); memcpy(brs + 0x28, &cs, 8);   /* Fumble_BaseChancePointGraph (+0x28) */
  ca[0x100] = 0; cas[0x100] = 0;   /* catching.shouldLinemanMultiplayerCatch (Boolean +0x100) */
  { int32_t v = 2; memcpy(gt + 0x428, &v, 4); uint32_t u = 3; memcpy(gt + 0x204, &u, 4); }   /* MaxKickBlocksPerTeam Int32 +0x428, DoubleMoveChanceMax UInt32 +0x204 */
  unsigned char *ra = t_farr(4, 0.5f); memcpy(gts + 0xf8, &ra, 8);   /* rushMove_AIControlledDefTeamBaseChance Float32[] +0xf8 */
  t_setf(cm, 0x20, 0.125f);   /* coachmodifiers.tempo_firstDownCoachBonus_DC Float32 +0x20: no style copies */
  t_reg_put("ballrules", br); t_reg_put("ballrules_gamestyle_simulation", brs); t_reg_put("catching", ca); t_reg_put("catching_gamestyle_simulation", cas);
  t_reg_put("gameplay_tuning", gt); t_reg_put("gameplay_tuning_gamestyle_simulation", gts); t_reg_put("coachmodifiers", cm); t_reg_put("blocking", bl);
  for (int i = 0; i < 40; i++) { char k[40]; snprintf(k, sizeof k, "filler_asset_%d", i); t_reg_put(k, t_inst("x/filler")); }   /* chains longer than one node */
  { uintptr_t r = (uintptr_t)g_tr_reg; *glob = r; }
  /* 3. found, as installed: nothing written */
  tune_pass();
  T_CHECK(g_tu_regready && !strcmp(g_tu_state, "running") && g_tu_writes == 0, "registry filled: running, every row As installed, nothing written (writes %ld)", g_tu_writes);
  const TuRow *tf = t_trow("tune-a", "fumble"); T_CHECK(tf && tf->state == 1 && tf->nt == 2, "fumble (both): found, 2 targets (Simulation + base)");
  T_CHECK(tf && g_tt[tf->t[0]].addr == (uintptr_t)(brs + 0x49c) && g_tt[tf->t[1]].addr == (uintptr_t)(br + 0x49c) && g_tt[tf->t[0]].kind == TK_F32, "fumble: the writer found Fumble_ChanceMultiplier at +0x49c (Float32) of each copy, from the dump's reflection tables");
  const TuRow *tc = t_trow("tune-a", "curve"); T_CHECK(tc && tc->state == 1 && tc->nt == 1 && g_tt[tc->t[0]].addr == (uintptr_t)(*(unsigned char **)(cs + 0x18) + 0x1c + 0x10), "curve: Curve.Points[1].y = the Simulation curve's point 1 Y (FloatCurve -> Points -> FloatCurvePoint.Y)");
  const TuRow *tl = t_trow("tune-a", "lineman"); T_CHECK(tl && tl->state == 1 && tl->nt == 2 && g_tt[tl->t[0]].kind == TK_BOOL && g_tt[tl->t[1]].addr == (uintptr_t)(ca + 0x100), "lineman: a full asset path works (class from the folder), Boolean at +0x100");
  const TuRow *tr2 = t_trow("tune-a", "rush2"); T_CHECK(tr2 && tr2->state == 1 && g_tt[tr2->t[0]].addr == (uintptr_t)(ra + 8), "rush2: a style file name is cut back to its asset; [2] of a Float32[] list");
  const TuRow *tt = t_trow("tune-a", "tempo"); T_CHECK(tt && tt->state == 1 && tt->fellback && tt->nt == 1 && g_tt[tt->t[0]].addr == (uintptr_t)(cm + 0x20), "tempo: coachmodifiers has no Simulation copy: the base copy is the target (logged)");
  struct { const char *key, *needle; } NA[] = { { "nofield", "field \"No_Such_Field\" is not in ballrules" }, { "noasset", "asset \"nosuchasset\" is not in the running game" },
    { "pastend", "index 9 is past the end (the game has 5)" }, { "wholecurve", "name one value inside it" }, { "enumpt", "only numbers and true/false" },
    { "text", "text values are not supported" }, { "bad", "the registry's instance is named" } };
  for (size_t k = 0; k < sizeof NA / sizeof *NA; k++) { const TuRow *r = t_trow("tune-a", NA[k].key); const MsRow *m = t_mrow("tune-a", NA[k].key);
    T_CHECK(r && r->state == 2 && strstr(r->why, NA[k].needle) && m && m->unavail && m->tu_marked && strstr(pool_str(g_cfg, m->s_unavail), NA[k].needle), "%s: not available (\"%s\"), greyed in the tab", NA[k].key, r ? r->why : "-"); }
  T_CHECK(t_f(br, 0x49c) == 1.5f && t_f(brs, 0x49c) == 1.0f && t_i(gt, 0x428) == 2 && ca[0x100] == 0, "nothing in the instances changed yet");
  /* 4. changes from the tab: written, clamped to the type, read back */
  long w0 = g_tu_writes;
  T_CHECK(t_tset("tune-a", "fumble", 3) && t_tset("tune-a", "lineman", 2) && t_tset("tune-a", "curve", 1 + 7) && t_tset("tune-a", "kicks", 2) && t_tset("tune-a", "dmove", 2) && t_tset("tune-a", "rush2", 2) && t_tset("tune-a", "tempo", 2), "tab: seven rows changed");
  T_CHECK(!t_tset("tune-a", "nofield", 1), "tab: a not-available row cannot be changed");
  tune_pass();
  T_CHECK(t_f(brs, 0x49c) == 20.0f && t_f(br, 0x49c) == 20.0f, "fumble x20: both copies 20 (sim %g, base %g)", t_f(brs, 0x49c), t_f(br, 0x49c));
  T_CHECK(cas[0x100] == 1 && ca[0x100] == 1, "lineman on: both copies true");
  T_CHECK(fabsf(t_curve_y(cs, 1) - 0.7f) < 1e-6f && fabsf(t_curve_y(cb, 1) - 0.24f) < 1e-6f && fabsf(t_curve_y(cs, 0) - 0.30f) < 1e-6f, "curve stop 70: Simulation point 1 Y = 0.7; the base curve and point 0 untouched");
  T_CHECK(t_i(gt, 0x428) == 2147483647, "kicks 3e10 -> clamped to the Int32 maximum (%d)", t_i(gt, 0x428));
  T_CHECK(t_i(gt, 0x204) == 7, "dmove 7 -> 7 (UInt32)");
  T_CHECK(fabsf(t_f(ra, 8) - 0.9f) < 1e-6f && t_f(ra, 4) == 1.5f, "rush2 -> element [2] = 0.9, element [1] untouched");
  T_CHECK(t_f(cm, 0x20) == 0.75f, "tempo -> the base copy (no Simulation copy) = 0.75");
  T_CHECK(g_tu_writes - w0 == 9 && g_tu_write_fails == 0, "9 writes (fumble 2, lineman 2, curve, kicks, dmove, rush2, tempo), every read-back ok (%ld)", g_tu_writes - w0);
  long w1 = g_tu_writes; tune_pass(); T_CHECK(g_tu_writes == w1, "a pass with nothing changed writes nothing");
  t_tset("tune-a", "kicks", 1); t_tset("tune-a", "dmove", 1); tune_pass();
  T_CHECK(t_i(gt, 0x428) == 3 && t_i(gt, 0x204) == 0, "kicks 2.6 -> rounded to 3 (Int32); dmove -5 -> clamped to 0 (UInt32 minimum)");
  /* 5. two mods on one field: the newest change wins; back to As installed falls to the next one, then to the loaded value */
  t_tset("tune-b", "fumble-b", 1); tune_pass();
  T_CHECK(t_f(brs, 0x49c) == 0.25f && t_f(br, 0x49c) == 20.0f, "Tune B (simulation) changed last: Simulation 0.25, base keeps Tune A's 20");
  t_tset("tune-a", "fumble", 2); tune_pass(); T_CHECK(t_f(brs, 0x49c) == 1.0f && t_f(br, 0x49c) == 1.0f, "Tune A changed again (Stock = 1): both copies 1");
  t_tset("tune-a", "fumble", 0); tune_pass(); T_CHECK(t_f(brs, 0x49c) == 0.25f && t_f(br, 0x49c) == 1.5f, "Tune A back to As installed: Simulation = Tune B's 0.25, base = its loaded 1.5");
  t_tset("tune-b", "fumble-b", 0); tune_pass(); T_CHECK(t_f(brs, 0x49c) == 1.0f, "Tune B back to As installed: Simulation = its loaded 1.0");
  /* 6. As installed restores every other row */
  t_tset("tune-a", "lineman", 0); t_tset("tune-a", "curve", 0); t_tset("tune-a", "rush2", 0); t_tset("tune-a", "tempo", 0); t_tset("tune-a", "dmove", 0); t_tset("tune-a", "kicks", 0); tune_pass();
  T_CHECK(ca[0x100] == 0 && cas[0x100] == 0 && fabsf(t_curve_y(cs, 1) - 0.29f) < 1e-6f && t_f(ra, 8) == 2.5f && t_f(cm, 0x20) == 0.125f && t_i(gt, 0x204) == 3 && t_i(gt, 0x428) == 2, "every row As installed: every loaded value back (curve 0.29, rush 2.5, tempo 0.125, dmove 3, kicks 2)");
  /* 7. the game replaces an instance: found again, its value is the new As installed, the row's value written again */
  t_tset("tune-a", "fumble", 3); tune_pass();
  unsigned char *brs2 = t_inst("football/gameplay/attribsys/data/ballrules/ballrules/ballrules_gamestyle_simulation"); t_setf(brs2, 0x49c, 2.0f); t_reg_put("ballrules_gamestyle_simulation", brs2);
  tune_pass(); T_CHECK(t_f(brs2, 0x49c) == 20.0f && g_tt[tf->t[0]].obj == (uintptr_t)brs2, "a new Simulation instance: found again, 20 written into it");
  t_tset("tune-a", "fumble", 0); tune_pass(); T_CHECK(t_f(brs2, 0x49c) == 2.0f, "As installed now = the new instance's loaded 2.0");
  /* 8. something else keeps changing a written field: after 5 rewrites it is left alone */
  t_tset("tune-a", "fumble", 3); tune_pass();
  for (int k = 0; k < 7; k++) { t_setf(br, 0x49c, 7.0f + (float)k); tune_pass(); }
  T_CHECK(g_tt[tf->t[1]].stopped && t_f(br, 0x49c) == 13.0f && t_f(brs2, 0x49c) == 20.0f, "a field changed by something else 7 times: written again 5 times, then left alone (base %g); the other copy keeps 20", t_f(br, 0x49c));
  /* 9. an unreadable instance: not available, no fault */
  unsigned char *na = (unsigned char *)VirtualAlloc(NULL, 0x1000, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS); t_reg_put("catching_gamestyle_simulation", na);
  long f0 = g_faults; tune_pass(); const TuRow *tl2 = t_trow("tune-a", "lineman");
  T_CHECK(tl2 && tl2->state == 2 && strstr(tl2->why, "not readable") && g_faults == f0 && !g_tu_off, "an instance on a no-access page: lineman not available (%s), no fault", tl2 ? tl2->why : "-");
  /* 10. status */
  status_write(); char sp[MAX_PATH]; snprintf(sp, sizeof sp, "%s\\" MS_STATUS, dir); char *st = t_read(sp);
  T_CHECK(st && strstr(st, "\"tuning\":{\"state\":\"running\"") && strstr(st, "\"field\":\"Fumble_ChanceMultiplier\"") && strstr(st, "\"as_installed\":\"1.5\""), "status: the tuning block (state, rows, targets with their loaded values)"); free(st);
  /* 11. the tab text of a tuning row: no "later version" note; a not-available row says why */
  { char d[600]; const MsRow *r = t_mrow("tune-a", "fumble"); tab_desc(g_cfg, r, d, sizeof d); T_CHECK(!strstr(d, "later") && strstr(d, "As installed") && strstr(d, "Also set by"), "tab help: fumble (%s)", d); }
  { char d[600]; const MsRow *r = t_mrow("tune-a", "nofield"); tab_desc(g_cfg, r, d, sizeof d); T_CHECK(strstr(d, "Not available: field \"No_Such_Field\""), "tab help: nofield says why (%s)", d); }
  /* 12. a configuration re-read keeps the greyed rows greyed */
  cfg_poll(1); { const MsRow *m = t_mrow("tune-a", "nofield"); T_CHECK(m && m->unavail && m->tu_marked, "after a re-read: nofield still greyed"); }
  /* 13. tuning = 0: refused to start, nothing written */
  { int on = g_ku.on; g_ku.on = 0; g_tu_ready = 0; tune_install(); long w = g_tu_writes; t_tset("tune-a", "fumble", 1); tune_pass(); T_CHECK(!g_tu_ready && !strcmp(g_tu_state, "off") && g_tu_writes == w, "tuning = 0: off, nothing written"); g_ku.on = on; }
  *glob = keep_glob;
  if (g_log && g_log != stdout) { fclose(g_log); g_log = stdout; }
  { char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\tuning.log", dir); char *lg = t_read(lp); int nw = 0; for (const char *p = lg; p && (p = strstr(p, "tuning: WRITE ")); p++) nw++;
    T_CHECK(lg && nw == (int)g_tu_writes && strstr(lg, "as installed = 1.5") && strstr(lg, "NOT AVAILABLE") && strstr(lg, "no Simulation copy"), "the log has one WRITE line per write (%d), the loaded values and the not-available rows", nw); free(lg); }
  return g_t_fails;
}
#endif
