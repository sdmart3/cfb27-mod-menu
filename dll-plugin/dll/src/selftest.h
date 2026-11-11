/* selftest.h: modsettings-test.exe (the same sources with -DAP_TEST). Modes (build.js runs each; exit code = failures):
 *   json                 the JSON reader: values, escapes, UTF-8, every refusal
 *   config <dir>         manifests (spec v2) + values.json from files in <dir>: what loads, what is refused and why
 *   ini <dir>            ini bindings: the line rewrite (comments, CRLF, append), the atomic write into <dir>\Plugins\..., path refusals
 *   records <dir>        the band pass against a fake dynasty database: reads, writes, read-back, applied.json, both readings of a negative,
 *                        missing / doubled records, band_write 0, no CrossArcData, refcounts, ms_get, the capture wrapper, the status file
 *   tab <dir>            layer 2 tab feed: v3 + v2 manifests, the five slot wrappers against fake holders / Variants, settings.json, the journal,
 *                        dynasty rows through the band pass, slider rows, refusals, a fault in the feed
 *   guard                a fault inside the pass: caught, the band writer off, the handler's result still returned
 *   dump <dump>          a known build's dump: every pin, the full install (4 sites, each window hash as expected), then each broken pin
 *                        (stamp, other build, function, redirected call, name, window): what installs and what does not; enabled = 0
 *   patchsafe [n]        the vendored patch-safety test */
static int g_t_fails;
#define T_CHECK(cond, ...) do { if (cond) printf("  ok: " __VA_ARGS__); else { printf("  FAIL: " __VA_ARGS__); g_t_fails++; } printf("\n"); } while (0)
static void t_write(const char *path, const char *text) { FILE *f = fopen(path, "wb"); if (f) { fputs(text, f); fclose(f); } }
static char *t_read(const char *path) { size_t n; return read_file(path, 4 << 20, &n); }
static void t_init_common(const char *dir) {
  InitializeCriticalSection(&g_cs); InitializeCriticalSection(&g_dyn_cs); InitializeCriticalSection(&g_pass_cs); InitializeCriticalSection(&g_applied_cs);
  { static int jq; if (!jq) { InitializeCriticalSection(&g_jq_cs); jq = 1; } }
  if (dir) snprintf(g_dir, sizeof g_dir, "%s", dir);
  g_log = stdout; g_log_sync = 1;
}

/* ---------------------------------------------------------------- json */
static int selftest_json(void) {
  static JNode nodes[256]; static char text[4096]; JDoc d;
  const char *ok1 = "\xef\xbb\xbf { \"a\": [1, -2, 3.5, 1e3, true, false, null], \"s\": \"x\\\"y\\\\z\\u00e9\\ud83d\\ude00\\n\", \"o\": {} }";
  int r = j_parse(&d, nodes, 256, text, sizeof text, ok1, strlen(ok1));
  T_CHECK(r >= 0, "json: a document with every value type parses (BOM skipped)%s%s", r < 0 ? ": " : "", r < 0 ? d.err : "");
  if (r >= 0) {
    int a = j_get(&d, r, "a"); long long v;
    T_CHECK(d.n[a].count == 7 && j_isint(&d, d.n[a].first, &v) && v == 1, "json: array of 7, first = 1");
    int x = d.n[a].first; x = d.n[x].next; T_CHECK(j_isint(&d, x, &v) && v == -2, "json: -2 is an integer");
    x = d.n[x].next; T_CHECK(!j_isint(&d, x, NULL) && d.n[x].num == 3.5, "json: 3.5 is not an integer");
    x = d.n[x].next; T_CHECK(!j_isint(&d, x, NULL) && d.n[x].num == 1000, "json: 1e3 is a number, not an integer");
    const char *s = j_str(&d, j_get(&d, r, "s"));
    T_CHECK(s && !strcmp(s, "x\"y\\z\xc3\xa9\xf0\x9f\x98\x80\n"), "json: escapes, \\u00e9 and a surrogate pair decode to UTF-8");
    T_CHECK(d.n[j_get(&d, r, "o")].type == J_OBJ && d.n[j_get(&d, r, "o")].count == 0, "json: an empty object");
  }
  const char *bad[] = { "{\"a\":1,}", "[01]", "{\"a\":\"x\ny\"}", "{\"a\":\"x", "{\"a\":1,\"a\":2}", "[1] x", "{\"a\" 1}", "\"\\ud800\"", "[tru]", "" };
  const char *why[] = { "trailing comma", "leading zero", "control character", "unfinished string", "duplicate key", "text after", "missing colon", "lone surrogate", "bad literal", "empty" };
  for (size_t i = 0; i < sizeof bad / sizeof *bad; i++) { int b = j_parse(&d, nodes, 256, text, sizeof text, bad[i], strlen(bad[i])); T_CHECK(b < 0 && d.err[0], "json: refused (%s): %s", why[i], d.err); }
  char deep[80]; memset(deep, '[', 40); memset(deep + 40, ']', 40); int b = j_parse(&d, nodes, 256, text, sizeof text, deep, 80); T_CHECK(b < 0, "json: refused (nested 40 deep)");
  b = j_parse(&d, nodes, 4, text, sizeof text, "[1,2,3,4,5]", 11); T_CHECK(b < 0 && strstr(d.err, "too many"), "json: refused (more values than the arena holds)");
  char q[64]; j_quote(q, sizeof q, "a\"b\\c\n"); T_CHECK(!strcmp(q, "\"a\\\"b\\\\c\\u000a\""), "json: quote escapes quotes, backslashes, control characters");
  printf("json: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- manifests used by several modes */
static const char *T_DEMO = "{ \"schema\": 2, \"id\": \"modsettings-demo\", \"title\": \"Mod Settings Demo (Test)\", \"rows\": [\n"
  " { \"id\": \"alpha\", \"label\": \"Demo Switch A\", \"options\": [\"Off\", \"On\"], \"default\": 1, \"store\": \"band\", \"band\": { \"record\": 0, \"bit\": 4, \"width\": 1 } },\n"
  " { \"id\": \"beta\", \"label\": \"Demo Level\", \"options\": [\"Low\", \"Medium\", \"High\"], \"default\": 1, \"store\": \"band\", \"band\": { \"record\": 0, \"bit\": 5, \"width\": 2 } },\n"
  " { \"id\": \"gamma\", \"label\": \"Demo Switch B\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 0, \"bit\": 7, \"width\": 1 } } ] }\n";
static const char *T_OTHER = "{ \"schema\": 2, \"id\": \"other\", \"title\": \"Other Mod\", \"rows\": [\n"
  " { \"id\": \"x\", \"label\": \"X\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 1, \"bit\": 0, \"width\": 1 } },\n"
  " { \"id\": \"speed\", \"label\": \"Speed\", \"options\": [\"Slow\", \"Normal\", \"Fast\"], \"default\": 1, \"store\": \"global\", \"ini\": { \"file\": \"Other\\\\other.ini\", \"key\": \"speed\", \"values\": [\"0\", \"1\", \"2\"] } } ] }\n";
static void t_mkdirs(const char *dir) { char p[MAX_PATH]; CreateDirectoryA(dir, NULL); snprintf(p, sizeof p, "%s\\" MS_MODSDIR, dir); CreateDirectoryA(p, NULL); }
static void t_put(const char *dir, const char *name, const char *text) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, name); t_write(p, text); }
static int t_has_problem(const MsCfg *c, const char *needle) { return strstr(c->problems, needle) != NULL; }

/* ---------------------------------------------------------------- config */
static int selftest_config(const char *dir) {
  t_init_common(dir); t_mkdirs(dir);
  t_put(dir, "mods\\modsettings-demo.json", T_DEMO); t_put(dir, "mods\\other.json", T_OTHER);
  /* refused manifests, one reason each (loaded after the two good ones: names sort after "other") */
  t_put(dir, "mods\\p-schema1.json", "{ \"schema\": 1, \"id\": \"p-schema1\", \"title\": \"T\", \"rows\": [] }");
  t_put(dir, "mods\\p-name.json", "{ \"schema\": 2, \"id\": \"p-other-name\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"global\" } ] }");
  t_put(dir, "mods\\p-onoff.json", "{ \"schema\": 2, \"id\": \"p-onoff\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"On\", \"Off\"], \"default\": 0, \"store\": \"global\" } ] }");
  t_put(dir, "mods\\p-overlap.json", "{ \"schema\": 2, \"id\": \"p-overlap\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 0, \"bit\": 6, \"width\": 1 } } ] }");
  t_put(dir, "mods\\p-dotdot.json", "{ \"schema\": 2, \"id\": \"p-dotdot\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"global\", \"ini\": { \"file\": \"..\\\\x.ini\", \"key\": \"k\", \"values\": [\"0\", \"1\"] } } ] }");
  t_put(dir, "mods\\p-dupkey.json", "{ \"schema\": 2, \"id\": \"p-dupkey\", \"id\": \"p-dupkey\", \"title\": \"T\", \"rows\": [] }");
  t_put(dir, "mods\\p-default.json", "{ \"schema\": 2, \"id\": \"p-default\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 2, \"store\": \"global\" } ] }");
  t_put(dir, "mods\\p-width.json", "{ \"schema\": 2, \"id\": \"p-width\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"L\", \"M\", \"H\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 1, \"bit\": 4, \"width\": 1 } } ] }");
  t_put(dir, "mods\\p-store.json", "{ \"schema\": 2, \"id\": \"p-store\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0 } ] }");
  t_put(dir, "mods\\p-record.json", "{ \"schema\": 2, \"id\": \"p-record\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 2, \"bit\": 0, \"width\": 1 } } ] }");
  t_put(dir, "mods\\p-bandini.json", "{ \"schema\": 2, \"id\": \"p-bandini\", \"title\": \"T\", \"rows\": [ { \"id\": \"a\", \"label\": \"A\", \"options\": [\"Off\", \"On\"], \"default\": 0, \"store\": \"band\", \"band\": { \"record\": 1, \"bit\": 8, \"width\": 1 }, \"ini\": { \"file\": \"A\\\\a.ini\", \"key\": \"k\", \"values\": [\"0\", \"1\"] } } ] }");
  t_put(dir, "mods\\p-json.json", "{ \"schema\": 2, ");
  t_put(dir, "values.json", "{ \"schema\": 2, \"global\": { \"other\": { \"speed\": 2 }, \"gone-mod\": { \"r\": 1 } },\n"
                            "  \"dynasty\": { \"modsettings-demo\": { \"alpha\": { \"value\": 0, \"seq\": 1 }, \"beta\": { \"value\": 5, \"seq\": 2 }, \"gamma\": { \"value\": 1, \"seq\": 0 } } } }");
  MsCfg *c = cfg_build();
  T_CHECK(c && c->nmods == 2 && !strcmp(c->loaded, "modsettings-demo,other"), "config: the two good manifests load (%s)", c ? c->loaded : "-");
  T_CHECK(c && c->nrows == 5, "config: 5 rows (3 + 2)");
  struct { const char *needle, *why; } P[] = {
    { "p-schema1.json: schema must be 2", "schema 1" }, { "p-name.json: the file name must be <id>.json", "file name" }, { "a two-option row must be [\"Off\", \"On\"]", "On/Off order" },
    { "p-overlap/a: band record 0 bits 6-6 overlap modsettings-demo/beta", "a shared band bit" }, { "p-dotdot/a: ini.file must be an .ini path inside the Plugins folder", "'..' in an ini path" },
    { "p-dupkey.json: not valid JSON (a key appears twice", "a key twice" }, { "p-default/a: default must be an option index", "default out of range" },
    { "p-width/a: width 1 holds 2 values, the row has 3 options", "width too small" }, { "p-store/a: store must be", "no store" },
    { "p-record/a: band record 2 bit 0 width 1 is outside the band", "record 2" }, { "p-bandini/a: an ini binding is for global rows only", "ini on a band row" },
    { "p-json.json: not valid JSON", "broken JSON" } };
  for (size_t i = 0; i < sizeof P / sizeof *P; i++) T_CHECK(c && t_has_problem(c, P[i].needle), "config: refused (%s)", P[i].why);
  int sp = c ? row_index(c, "other", "speed") : -1, al = c ? row_index(c, "modsettings-demo", "alpha") : -1, be = c ? row_index(c, "modsettings-demo", "beta") : -1, ga = c ? row_index(c, "modsettings-demo", "gamma") : -1;
  T_CHECK(sp >= 0 && c->gval[sp] == 2 && c->gset[sp], "values: global other/speed = 2 (from values.json)");
  T_CHECK(al >= 0 && c->want[al] == 0 && c->want_seq[al] == 1, "values: dynasty change alpha = 0 (seq 1)");
  T_CHECK(be >= 0 && c->want_seq[be] == 0 && t_has_problem(c, "dynasty modsettings-demo/beta needs"), "values: beta = 5 (not an option) refused");
  T_CHECK(ga >= 0 && c->want_seq[ga] == 0 && t_has_problem(c, "dynasty modsettings-demo/gamma needs"), "values: seq 0 refused");
  T_CHECK(c && !t_has_problem(c, "gone-mod"), "values: a mod that is not installed is ignored quietly");
  int ox = c ? row_index(c, "other", "x") : -1; T_CHECK(ox >= 0 && c->rows[ox].rec == 1 && c->rows[ox].bit == 0 && c->rows[ox].width == 1, "config: other/x = band record 1 bit 0");
  T_CHECK(sp >= 0 && c->rows[sp].has_ini && !strcmp(c->rows[sp].ini_key, "speed") && !strcmp(c->rows[sp].ini_val[2], "2"), "config: other/speed binds other.ini speed = 0/1/2");
  free(c);
  t_put(dir, "values.json", "{ \"schema\": 2, \"global\": { \"other\": { \"speed\": 7 } } }");
  c = cfg_build(); sp = c ? row_index(c, "other", "speed") : -1;
  T_CHECK(sp >= 0 && c->gval[sp] == 1 && !c->gset[sp] && t_has_problem(c, "global other/speed must be an option index"), "values: a bad global value -> the default (1) and a problem line");
  free(c);
  t_put(dir, "values.json", "not json"); c = cfg_build(); sp = c ? row_index(c, "other", "speed") : -1;
  T_CHECK(sp >= 0 && c->gval[sp] == 1 && t_has_problem(c, "values.json: not valid JSON"), "values: a broken values.json -> every default");
  free(c);
  char vp[MAX_PATH]; snprintf(vp, sizeof vp, "%s\\values.json", dir); DeleteFileA(vp); c = cfg_build();
  T_CHECK(c && c->nproblems >= 12 && !t_has_problem(c, "values.json: "), "values: no values.json at all -> defaults, no problem line (%d problems)", c ? c->nproblems : -1); if (c && t_has_problem(c, "values.json: ")) printf("%s\n", c->problems);
  free(c);
  printf("config: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- ini */
static int selftest_ini(const char *dir) {
  char v[64];
  T_CHECK(ini_get("a = 1\r\n; speed = 9\r\n  Speed=2 ; note\r\n", "speed", v, sizeof v) && !strcmp(v, "2"), "ini: get skips a commented line, matches the key case-insensitively, drops the comment");
  T_CHECK(!ini_get("speedy = 3\n", "speed", v, sizeof v), "ini: a longer key does not match");
  char *t = ini_set("a = 1\r\nspeed = 1 ; keep me\r\nb = 2\r\n", "speed", "2");
  T_CHECK(t && !strcmp(t, "a = 1\r\nspeed = 2 ; keep me\r\nb = 2\r\n"), "ini: set replaces one line, keeps its comment and CRLF"); free(t);
  t = ini_set("a = 1\r\n", "speed", "0"); T_CHECK(t && !strcmp(t, "a = 1\r\nspeed = 0\r\n"), "ini: set appends a missing key (CRLF)"); free(t);
  t = ini_set("a = 1", "speed", "0"); T_CHECK(t && !strcmp(t, "a = 1\nspeed = 0\n"), "ini: set appends after a last line without a newline"); free(t);
  t = ini_set("speed = 1\nspeed = 5\n", "speed", "2"); T_CHECK(t && !strcmp(t, "speed = 2\nspeed = 5\n"), "ini: only the first line is changed (the one a reader takes)"); free(t);
  T_CHECK(ini_path_ok("CallSheet\\callsheet.ini") && ini_path_ok("A/b.ini") && !ini_path_ok("..\\x.ini") && !ini_path_ok("A\\..\\..\\x.ini") && !ini_path_ok("C:\\x.ini") && !ini_path_ok("\\x.ini") && !ini_path_ok("A\\x.txt") && !ini_path_ok("A\\.\\x.ini"),
          "ini: paths inside Plugins only (no drive, no leading slash, no '.' / '..', .ini only)");
  /* the atomic write: <dir>\Plugins\ModSettings (g_dir) and <dir>\Plugins\Other\other.ini */
  char pl[MAX_PATH], ms[MAX_PATH], od[MAX_PATH], oi[MAX_PATH];
  CreateDirectoryA(dir, NULL); snprintf(pl, sizeof pl, "%s\\Plugins", dir); CreateDirectoryA(pl, NULL);
  snprintf(ms, sizeof ms, "%s\\ModSettings", pl); snprintf(od, sizeof od, "%s\\Other", pl); CreateDirectoryA(od, NULL); t_init_common(ms); t_mkdirs(ms);
  snprintf(oi, sizeof oi, "%s\\other.ini", od); t_write(oi, "; Other mod\r\nspeed = 1\r\nmore = 3\r\n");
  t_put(ms, "mods\\other.json", T_OTHER); t_put(ms, "values.json", "{ \"schema\": 2, \"global\": { \"other\": { \"speed\": 2 } } }");
  MsCfg *c = cfg_build(); int sp = c ? row_index(c, "other", "speed") : -1; char m[600];
  T_CHECK(sp >= 0 && ini_apply(&c->rows[sp], c->gval[sp], m, sizeof m), "ini: values.json speed = 2 is written (%s)", m);
  char *after = t_read(oi); T_CHECK(after && !strcmp(after, "; Other mod\r\nspeed = 2\r\nmore = 3\r\n"), "ini: the file holds speed = 2, everything else unchanged"); free(after);
  T_CHECK(sp >= 0 && !ini_apply(&c->rows[sp], c->gval[sp], m, sizeof m) && strstr(m, "already says"), "ini: a second apply writes nothing (%s)", m);
  char tmpf[MAX_PATH + 16]; snprintf(tmpf, sizeof tmpf, "%s.ms.tmp", oi); T_CHECK(GetFileAttributesA(tmpf) == INVALID_FILE_ATTRIBUTES, "ini: no temporary file left behind");
  DeleteFileA(oi); T_CHECK(sp >= 0 && !ini_apply(&c->rows[sp], c->gval[sp], m, sizeof m) && strstr(m, "does not exist"), "ini: a missing ini is not created (the mod is not installed)");
  free(c);
  t_write(oi, "speed = 0\r\n"); t_put(ms, "values.json", "{ \"schema\": 2 }"); c = cfg_build(); ini_apply_all(c);
  after = t_read(oi); T_CHECK(after && !strcmp(after, "speed = 0\r\n"), "ini: a row values.json does not name is never written (the mod's own ini wins)"); free(after); free(c);
  printf("ini: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- a fake dynasty database (records = { vt, pad, iface, row }) */
enum { TT_USER, TT_ARR, TT_CAD };
typedef struct FIface FIface;
typedef struct { void **vt; void *pad; FIface *iface; int row; int pad2; } FRec;
struct FIface { void **vt; int type; FRec *elem[16]; int n; };
static FIface FI_USER, FI_CADARR, FI_CAD; static FRec R_USER, R_CADREF, R_CAD[16];
static short g_t_val[16]; static int g_t_id[16];          /* the CrossArcData rows: Id, Value (stored as a 16-bit field) */
static int g_t_unsigned, g_t_user11 = -5, g_t_fault, g_t_nowrite;   /* reads give 0..65535; handle 11's raw reading; fault in a getter; setter ignores writes */
static volatile LONG g_t_refs; static int g_t_setint, g_t_dirty, g_t_lastset;
static void f_none(void) {}
static void fr_addref(void *x) { (void)x; InterlockedIncrement(&g_t_refs); }
static void fr_release(void *x) { (void)x; InterlockedDecrement(&g_t_refs); }
static void **fr_qi(void *x, void **out, uint32_t iid) { if (iid == IID_ARRY) { *out = x; InterlockedIncrement(&g_t_refs); } else *out = NULL; return out; }
static void *FREC_VT[8] = { (void *)fr_addref, (void *)fr_release, (void *)f_none, (void *)f_none, (void *)f_none, (void *)fr_qi, (void *)f_none, (void *)f_none };
static int fi_int(void *ifc, int row, void *fh) {
  FIface *f = (FIface *)ifc; if (g_t_fault) { volatile int *z = NULL; return *z; }
  if (f->type == TT_USER && (uintptr_t)fh == H_CAD) return g_t_user11;
  if (f->type == TT_CAD && (uintptr_t)fh == H_CAD_ID) return g_t_id[row];
  if (f->type == TT_CAD && (uintptr_t)fh == H_CAD_VAL) return g_t_unsigned ? (int)(unsigned short)g_t_val[row] : (int)g_t_val[row];
  return 0;
}
static void fi_ref(void *ifc, void **out, int row, void *fh) {
  FIface *f = (FIface *)ifc; *out = NULL; uintptr_t h = (uintptr_t)fh; (void)row;
  if (f->type == TT_USER && h == H_CAD && g_t_user11 < 0) *out = &R_CADREF;
  else if (f->type == TT_ARR && h < (uintptr_t)f->n) *out = f->elem[h];
  if (*out) InterlockedIncrement(&g_t_refs);
}
static void fi_setint(void *ifc, int row, void *fh, int v) { FIface *f = (FIface *)ifc; g_t_lastset = v; if (f->type == TT_CAD && (uintptr_t)fh == H_CAD_VAL && row >= 0 && row < 16 && !g_t_nowrite) { g_t_val[row] = (short)v; } g_t_setint++; }
static void ff_dirty(void *ifc) { (void)ifc; g_t_dirty++; }
static size_t ff_count(void *arr) { return (size_t)((FRec *)arr)->iface->n; }
static char ff_finduser(void *flow, void *a2, void **user) { (void)flow; (void)a2; *user = &R_USER; InterlockedIncrement(&g_t_refs); return 1; }
static void *FIF_VT[0x150 / 8];
static void t_rec_init(FRec *r, FIface *f, int row) { r->vt = FREC_VT; r->iface = f; r->row = row; }
/* rows: n of (id, value) */
static void t_db(int n, const int *ids, const int *vals) {
  FIF_VT[IF_INT / 8] = (void *)fi_int; FIF_VT[IF_ENUM / 8] = (void *)fi_int; FIF_VT[IF_REF / 8] = (void *)fi_ref; FIF_VT[IF_SETINT / 8] = (void *)fi_setint;
  FIface *all[] = { &FI_USER, &FI_CADARR, &FI_CAD }; int ty[] = { TT_USER, TT_ARR, TT_CAD };
  for (int i = 0; i < 3; i++) { memset(all[i], 0, sizeof *all[i]); all[i]->vt = FIF_VT; all[i]->type = ty[i]; }
  t_rec_init(&R_USER, &FI_USER, 0); t_rec_init(&R_CADREF, &FI_CADARR, 0);
  for (int i = 0; i < n && i < 16; i++) { t_rec_init(&R_CAD[i], &FI_CAD, i); FI_CADARR.elem[i] = &R_CAD[i]; g_t_id[i] = ids[i]; g_t_val[i] = (short)vals[i]; }
  FI_CADARR.n = n;
  g_fn[MS_F_FINDUSER] = (void *)ff_finduser; g_fn[MS_F_ARRCOUNT] = (void *)ff_count; g_fn[MS_F_DIRTY] = (void *)ff_dirty;
}
static int t_pass(void) { PassArgs pa = { NULL, NULL, -1 }; int w = g_t_setint; ms_pass(&pa); return g_t_setint - w; }
static void t_values(const char *dir, const char *json) { t_put(dir, "values.json", json); cfg_poll(1); }
static uintptr_t t_handler_ret = 1; static int t_handler_calls;
static uintptr_t t_handler(void *f, void *a, void *q, void *s) { (void)f; (void)a; (void)q; (void)s; t_handler_calls++; return t_handler_ret; }

/* ---------------------------------------------------------------- records */
static int selftest_records(const char *dir) {
  t_init_common(dir); t_mkdirs(dir); g_t_fake = 1; g_k.log_captures = 100;
  char ap[MAX_PATH]; snprintf(ap, sizeof ap, "%s\\" MS_APPLIED, dir); DeleteFileA(ap);
  t_put(dir, "mods\\modsettings-demo.json", T_DEMO); t_put(dir, "mods\\other.json", T_OTHER);
  /* CrossArcData: a goals row, band record 0 at its defaults (alpha 1 << 4 | beta 1 << 5 = 48: -8144), a row in the band with another Id
   * (not ours), band record 1 at 0 (-7680) */
  int ids[] = { 0, 0, 5, 0 }, vals[] = { 22904, -8144, -8100, -7680 }; t_db(4, ids, vals);
  t_values(dir, "{ \"schema\": 2 }");
  T_CHECK(t_pass() == 0 && g_dyn.seen && g_dyn.cad_rows == 4 && g_dyn.rec_count[0] == 1 && g_dyn.rec_val[0] == -8144 && g_dyn.rec_count[1] == 1, "pass: reads 4 rows, record 0 = -8144, record 1 present, the other-Id row ignored, nothing written");
  T_CHECK(g_t_refs == 0, "pass: every reference released (refcount back to 0, got %ld)", (long)g_t_refs);
  T_CHECK(ms_get("modsettings-demo", "alpha") == 1 && ms_get("modsettings-demo", "beta") == 1 && ms_get("modsettings-demo", "gamma") == 0 && ms_get("other", "x") == 0, "ms_get: the dynasty's values (1 1 0, x 0)");
  T_CHECK(ms_get("other", "speed") == 1 && ms_get("nope", "x") == -1 && ms_get(NULL, "x") == -1, "ms_get: a global row's default (1); an unknown row -1");
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"alpha\": { \"value\": 0, \"seq\": 1 }, \"beta\": { \"value\": 2, \"seq\": 2 }, \"gamma\": { \"value\": 1, \"seq\": 3 } } } }");
  int d0 = g_t_dirty;
  T_CHECK(t_pass() == 1 && g_t_val[1] == -8000 && g_t_dirty == d0 + 1, "pass: ONE write for three changes: record 0 -8144 -> -8000 (alpha 0, beta 2, gamma 1), the change counter bumped once");
  T_CHECK(applied_seq("modsettings-demo", "alpha") == 1 && applied_seq("modsettings-demo", "beta") == 2 && applied_seq("modsettings-demo", "gamma") == 3, "pass: applied seqs 1 2 3");
  T_CHECK(g_t_refs == 0 && g_t_val[3] == -7680 && g_t_val[0] == 22904, "pass: refcount 0; the other rows untouched");
  T_CHECK(ms_get("modsettings-demo", "alpha") == 0 && ms_get("modsettings-demo", "beta") == 2 && ms_get("modsettings-demo", "gamma") == 1, "ms_get: 0 2 1 after the write");
  T_CHECK(t_pass() == 0, "pass: run again, nothing pending, nothing written");
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"alpha\": { \"value\": 0, \"seq\": 1 }, \"beta\": { \"value\": 2, \"seq\": 2 }, \"gamma\": { \"value\": 1, \"seq\": 3 } }, \"other\": { \"x\": { \"value\": 1, \"seq\": 4 } } } }");
  T_CHECK(t_pass() == 1 && g_t_val[3] == -7679 && g_t_val[1] == -8000, "pass: other/x -> record 1 -7680 -> -7679, record 0 unchanged");
  /* the same Value read back unsigned (0..65535): written back in that reading, compared signed */
  g_t_unsigned = 1;
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"alpha\": { \"value\": 1, \"seq\": 5 } } } }");
  T_CHECK(t_pass() == 1 && g_t_lastset == -7984 + 65536 && g_t_val[1] == -7984 && applied_seq("modsettings-demo", "alpha") == 5, "pass: an unsigned reading (57552) is written back unsigned (-7984 + 65536), read back ok");
  g_t_unsigned = 0;
  /* record 1 missing: the change waits; it lands once the record exists */
  { int ids2[] = { 0, 0 }, vals2[] = { 22904, -7984 }; t_db(2, ids2, vals2); }
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"other\": { \"x\": { \"value\": 0, \"seq\": 6 } } } }");
  T_CHECK(t_pass() == 0 && applied_seq("other", "x") == 4 && g_dyn.rec_count[1] == 0, "pass: record 1 not there: no write, other/x stays pending (applied 4 < 6)");
  T_CHECK(ms_get("other", "x") == 0, "ms_get: a band row whose record is missing = its default (0)");
  { int ids3[] = { 0, 0, 0 }, vals3[] = { 22904, -7984, -7679 }; t_db(3, ids3, vals3); }
  T_CHECK(t_pass() == 1 && g_t_val[2] == -7680 && applied_seq("other", "x") == 6, "pass: record 1 appears (the mod's first Advance): the waiting change lands (-7679 -> -7680)");
  /* record 0 twice: refused */
  { int ids4[] = { 0, 0 }, vals4[] = { -7984, -8100 }; t_db(2, ids4, vals4); }
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"gamma\": { \"value\": 0, \"seq\": 7 } } } }");
  T_CHECK(t_pass() == 0 && g_dyn.rec_count[0] == 2 && applied_seq("modsettings-demo", "gamma") == 3, "pass: band record 0 twice: NOT written, the change waits");
  T_CHECK(g_t_refs == 0, "pass: refcount 0 with a doubled record");
  /* band_write = 0 */
  { int ids5[] = { 0 }, vals5[] = { -7984 }; t_db(1, ids5, vals5); }
  g_k.band_write = 0; T_CHECK(t_pass() == 0 && applied_seq("modsettings-demo", "gamma") == 3, "pass: band_write = 0: read only, the change waits"); g_k.band_write = 1;
  /* the read-back differs (a field that does not take the write): not applied, counted */
  g_t_nowrite = 1; LONG wf = g_n_write_fail;
  T_CHECK(t_pass() == 1 && g_n_write_fail == wf + 1 && applied_seq("modsettings-demo", "gamma") == 3, "pass: read-back differs: counted as a failed write, the change waits");
  g_t_nowrite = 0;
  T_CHECK(t_pass() == 1 && g_t_val[0] == -8112 && applied_seq("modsettings-demo", "gamma") == 7, "pass: then a working write: -7984 -> -8112 (gamma 0), applied 7");
  /* no CrossArcData array (handle 11 not a reference / null) */
  g_t_user11 = 5; t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"gamma\": { \"value\": 1, \"seq\": 8 } } } }");
  T_CHECK(t_pass() == 0 && !g_dyn.is_ref && g_dyn.cad_rows == 0 && applied_seq("modsettings-demo", "gamma") == 7, "pass: handle 11 not a reference: nothing read, the change waits");
  g_t_user11 = 2147483647; T_CHECK(t_pass() == 0 && !g_dyn.is_ref, "pass: handle 11 null (INT_MAX): nothing read"); g_t_user11 = -5;
  T_CHECK(g_t_refs == 0, "pass: refcount 0 throughout (got %ld)", (long)g_t_refs);
  /* applied.json round trip */
  applied_save(); MsApplied keep[8]; int nk = g_napplied < 8 ? g_napplied : 8; memcpy(keep, g_applied, sizeof keep[0] * (size_t)nk); g_napplied = 0; applied_load();
  T_CHECK(g_napplied == nk && applied_seq("modsettings-demo", "gamma") == 7 && applied_seq("other", "x") == 6 && applied_seq("modsettings-demo", "alpha") == 5, "applied.json: saved and read back (%d entries)", g_napplied);
  /* the capture wrapper: the handler always runs and its result is returned; a pass only on 1; a refresh storm is throttled */
  g_orig[0] = t_handler; g_pb = &MS_BUILDS[0]; t_handler_ret = 0; LONG p0 = g_n_pass; int hc = t_handler_calls;
  T_CHECK(ms_capture(0, NULL, NULL, NULL, NULL) == 0 && t_handler_calls == hc + 1 && g_n_pass == p0, "capture: handler result 0 -> returned as is, no pass");
  t_handler_ret = 1; g_last_pass_tick = 0;
  T_CHECK(ms_capture(0, NULL, NULL, NULL, NULL) == 1 && g_n_pass == p0 + 1 && g_t_val[0] == -7984, "capture: handler result 1 -> a pass (the waiting gamma = 1 lands: -7984)");
  T_CHECK(ms_capture(0, NULL, NULL, NULL, NULL) == 1 && g_n_pass == p0 + 1, "capture: a second call within 2 s with nothing pending: no pass (refresh storm)");
  t_values(dir, "{ \"schema\": 2, \"dynasty\": { \"modsettings-demo\": { \"gamma\": { \"value\": 0, \"seq\": 9 } } } }");
  T_CHECK(ms_capture(0, NULL, NULL, NULL, NULL) == 1 && g_n_pass == p0 + 2 && g_t_val[0] == -8112, "capture: within 2 s but a change waits: a pass (gamma 0)");
  /* the status file */
  status_write(); char sp[MAX_PATH]; snprintf(sp, sizeof sp, "%s\\" MS_STATUS, dir); char *st = t_read(sp);
  static JNode nodes[8192]; static char text[1 << 17]; JDoc d; int root = st ? j_parse(&d, nodes, 8192, text, sizeof text, st, strlen(st)) : -1;
  T_CHECK(root >= 0, "status: modsettings-status.json is valid JSON%s%s", root < 0 && st ? ": " : "", root < 0 && st ? d.err : "");
  if (root >= 0) {
    int rows = j_get(&d, root, "rows"), dyn = j_get(&d, root, "dynasty"), recs = j_get(&d, dyn, "records");
    T_CHECK(d.n[rows].count == 5 && d.n[j_get(&d, root, "manifests")].count == 2, "status: 5 rows, a manifests block");
    long long v; int r0 = d.n[recs].first;
    T_CHECK(j_isint(&d, j_get(&d, r0, "value"), &v) && v == -8112 && d.n[j_get(&d, dyn, "seen")].type == J_TRUE, "status: dynasty seen, record 0 = -8112");
    int g = d.n[rows].first; for (int k = 0; k < 2; k++) g = d.n[g].next;   /* gamma */
    T_CHECK(j_isint(&d, j_get(&d, g, "value"), &v) && v == 0 && d.n[j_get(&d, g, "pending")].type == J_FALSE && j_isint(&d, j_get(&d, g, "applied_seq"), &v) && v == 9, "status: gamma value 0, not pending, applied 9");
    T_CHECK(!strcmp(j_str(&d, j_get(&d, root, "state")) ? j_str(&d, j_get(&d, root, "state")) : "", "loading"), "status: state (flat, as the plugin writes it)");
  }
  free(st);
  printf("records: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- guard */
static int selftest_guard(const char *dir) {
  t_init_common(dir); t_mkdirs(dir); g_t_fake = 1; ms_guard_init();
  t_put(dir, "mods\\modsettings-demo.json", T_DEMO); t_values(dir, "{ \"schema\": 2 }");
  int ids[] = { 0 }, vals[] = { -8144 }; t_db(1, ids, vals);
  g_t_fault = 1; PassArgs pa = { NULL, NULL, -1 };
  int ok = ms_guarded(ms_pass, &pa, "the band pass (self-test)");
  T_CHECK(!ok && g_disabled && g_faults == 1, "guard: a fault inside the pass is caught; the band writer is off");
  g_t_fault = 0; g_orig[0] = t_handler; t_handler_ret = 1; g_last_pass_tick = 0; LONG p = g_n_pass;
  T_CHECK(ms_capture(0, NULL, NULL, NULL, NULL) == 1 && g_n_pass == p, "guard: afterwards the handler still runs and returns 1, no pass");
  T_CHECK(ms_get("modsettings-demo", "alpha") >= -2, "guard: ms_get still answers");
  printf("guard: %d failure(s)\n", g_t_fails); return g_t_fails;
}

static int d_type_is(const JDoc *d, int i, int t) { const JNode *x = j_at(d, i); return x && x->type == t; }
/* ---------------------------------------------------------------- tab: the layer 2 feed (mstab.h) against fake holders + a fake Variant */
/* spec v3 manifests as the two samples (dll-plugin/samples) + a dynasty row with a slot + one v2 manifest (T_DEMO) */
static const char *T_V3_AK = "{ \"schema\": 3, \"modid\": \"auto-kick\", \"title\": \"Auto Kick\", \"version\": \"0.3.0\", \"rows\": [\n"
  " { \"type\": \"header\", \"label\": \"Kicking\" },\n"
  " { \"type\": \"toggle\", \"key\": \"enabled\", \"label\": \"Auto Kick\", \"description\": \"The game aims and times field goals and punts for you.\", \"default\": true, \"bind\": { \"type\": \"api\" } },\n"
  " { \"type\": \"list\", \"key\": \"style\", \"label\": \"Kick Style\", \"description\": \"Safe takes the sure kick.\", \"options\": [\"Safe\", \"Balanced\", \"Aggressive\"], \"default\": 1, \"bind\": { \"type\": \"api\" } },\n"
  " { \"type\": \"slider\", \"key\": \"delay-ms\", \"label\": \"Reaction Delay\", \"description\": \"Milliseconds the DLL waits.\", \"min\": 0, \"max\": 500, \"step\": 50, \"default\": 150, \"unit\": \" ms\", \"bind\": { \"type\": \"api\" } },\n"
  " { \"type\": \"info\", \"label\": \"Your DLL reads these with ModSettings_Get.\" } ] }\n";
static const char *T_V3_PP = "{ \"schema\": 3, \"modid\": \"pocket-presence\", \"title\": \"Pocket Presence\", \"version\": \"1.2.0\", \"rows\": [\n"
  " { \"type\": \"header\", \"label\": \"Pass rush\" },\n"
  " { \"type\": \"list\", \"key\": \"pocket-depth\", \"label\": \"Pocket Depth\", \"options\": [\"Shallow\", \"Stock\", \"Deep\"], \"default\": 1,\n"
  "   \"bind\": { \"type\": \"tuning\", \"asset\": \"blocking/BlockingTuning_Main\", \"field\": \"PocketCenter_DefaultOffset\", \"copy\": \"simulation\", \"values\": [2.0, 3.0, 4.5] } },\n"
  " { \"type\": \"slider\", \"key\": \"steer-strength\", \"label\": \"Open-Space Steering\", \"min\": 0, \"max\": 100, \"step\": 10, \"default\": 50, \"unit\": \"%\",\n"
  "   \"bind\": { \"type\": \"tuning\", \"asset\": \"ai/DefenderMovementTuning\", \"copy\": \"simulation\", \"field\": \"F\", \"instance\": \"Curve.Points[1].y\", \"scale\": { \"at-min\": 0.0, \"at-max\": 1.0 } } },\n"
  " { \"type\": \"slider\", \"key\": \"fine\", \"label\": \"Fine Slider\", \"min\": 0, \"max\": 1, \"step\": 0.01, \"default\": 0.25, \"bind\": { \"type\": \"api\" } },\n"
  " { \"type\": \"toggle\", \"key\": \"jump-catch\", \"label\": \"Jump Catches\", \"default\": true,\n"
  "   \"bind\": { \"type\": \"tuning\", \"asset\": \"catching/Multiplayer_SP_Catch_DB\", \"copy\": \"both\", \"field\": \"Catching_AllowedToJumpCatch\", \"values\": [false, true] } } ] }\n";
static const char *T_V3_DY = "{ \"schema\": 3, \"modid\": \"tabtest-dyn\", \"title\": \"Tab Test Dynasty\", \"version\": \"1\", \"rows\": [\n"
  " { \"type\": \"list\", \"key\": \"mode\", \"label\": \"Recruit Mode\", \"options\": [\"Stock\", \"Fast\", \"Slow\"], \"default\": 0, \"bind\": { \"type\": \"dynasty\", \"slot\": \"tabtest-dyn-mode\" } },\n"
  " { \"type\": \"toggle\", \"key\": \"noslot\", \"label\": \"No Slot\", \"default\": false, \"bind\": { \"type\": \"dynasty\", \"slot\": \"not-in-the-list\" } } ] }\n";
static const char *T_SLOTS = "{ \"schema\": 1, \"slots\": [ { \"mod\": \"modsettings-demo\", \"row\": \"alpha\", \"record\": 0, \"bit\": 4, \"width\": 1 },\n"
  " { \"slot\": \"tabtest-dyn-mode\", \"mod\": \"tabtest-dyn\", \"row\": \"mode\", \"record\": 1, \"bit\": 0, \"width\": 2 } ] }\n";
typedef struct { int key, type, en, isdef, nset; char disp[256], desc[600]; } TRowRec;
static TRowRec g_th[800]; static int g_th_n, g_th_fault;
static TRowRec *th_at(int r) { if (g_th_fault) { volatile int *z = NULL; *z = 1; } if (r < 0 || r >= 800) return NULL; if (r + 1 > g_th_n) g_th_n = r + 1; g_th[r].nset++; return &g_th[r]; }
static void fh_abool(void *h, int r, const char *n, char v) { (void)h; TRowRec *x = th_at(r); if (!x) return; if (!strcmp(n, "isEnabled")) x->en = v; else if (!strcmp(n, "isDefault")) x->isdef = v; }
static void fh_aint(void *h, int r, const char *n, int v) { (void)h; TRowRec *x = th_at(r); if (!x) return; if (!strcmp(n, "key")) x->key = v; else if (!strcmp(n, "type")) x->type = v; }
static void fh_astr(void *h, int r, const char *n, const char *v) { (void)h; TRowRec *x = th_at(r); if (!x) return; if (!strcmp(n, "display")) snprintf(x->disp, sizeof x->disp, "%s", v); else if (!strcmp(n, "description")) snprintf(x->desc, sizeof x->desc, "%s", v); }
static void *TH_ARR_VT[8] = { (void *)f_none, (void *)f_none, (void *)f_none, (void *)fh_abool, (void *)fh_aint, (void *)f_none, (void *)fh_astr, (void *)f_none };
static struct { char name[32]; int iv; char sv[100]; } g_to[24]; static int g_nto;
static int *tho_slot(const char *n) { for (int i = 0; i < g_nto; i++) if (!strcmp(g_to[i].name, n)) return &g_to[i].iv; if (g_nto < 24) { snprintf(g_to[g_nto].name, 32, "%s", n); return &g_to[g_nto++].iv; } return NULL; }
static void fho_int(void *h, const char *n, int v) { (void)h; int *p = tho_slot(n); if (p) *p = v; }
static void fho_str(void *h, const char *n, const char *v) { (void)h; int *p = tho_slot(n); if (p) snprintf(((char *)p) + sizeof(int), 100, "%s", v); }
static const char *tho_sv(const char *n) { for (int i = 0; i < g_nto; i++) if (!strcmp(g_to[i].name, n)) return g_to[i].sv; return "?"; }
static int tho_iv(const char *n) { for (int i = 0; i < g_nto; i++) if (!strcmp(g_to[i].name, n)) return g_to[i].iv; return -12345; }
static void *TH_OBJ_VT[6] = { (void *)f_none, (void *)f_none, (void *)fho_int, (void *)f_none, (void *)fho_str, (void *)f_none };
typedef struct { void **vt; int v; int pad; } TVar;
static int tv_get(void *x) { return ((TVar *)x)->v; }
static void *TV_VT[4] = { (void *)f_none, (void *)f_none, (void *)f_none, (void *)tv_get };
static struct { void **vt; void *val; } H_ARR = { TH_ARR_VT, NULL }, H_OBJ = { TH_OBJ_VT, NULL };
static int g_t_orig[TS_N];
static uintptr_t to_list(void *a, void *b, void *c) { (void)a; (void)b; (void)c; g_t_orig[TS_LIST]++; return 7; }
static uintptr_t to_tog(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; g_t_orig[TS_TOGGLE]++; return 7; }
static uintptr_t to_sld(void *a, void *b, void *c, void *d) { (void)a; (void)b; (void)c; (void)d; g_t_orig[TS_SLIDER]++; return 7; }
static uintptr_t to_adjt(void *a, void *b, void *c, void *d, void *e, const char *w) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)w; g_t_orig[TS_ADJT]++; return 7; }
static uintptr_t to_adjs(void *a, void *b, void *c, void *d, int v) { (void)a; (void)b; (void)c; (void)d; (void)v; g_t_orig[TS_ADJS]++; return 7; }
static void th_clear(void) { memset(g_th, 0, sizeof g_th); g_th_n = 0; memset(g_to, 0, sizeof g_to); g_nto = 0; }
static int t_list(int cat) { th_clear(); TVar c = { TV_VT, cat, 0 }; tab_w_list(NULL, &H_ARR, &c); return g_th_n; }
static int t_row(const char *disp) { for (int k = 0; k < g_th_n; k++) if (!strcmp(g_th[k].disp, disp)) return k; return -1; }
static int t_tkey(const char *mod, const char *id) { AcquireSRWLockShared(&g_cfg_lock); int i = g_cfg ? row_index(g_cfg, mod, id) : -1; ReleaseSRWLockShared(&g_cfg_lock); return i < 0 ? -999 : MS_TAB_KEY0 + i; }
static int t_det(int key) { th_clear(); TVar c = { TV_VT, MS_TAB_CAT, 0 }, k = { TV_VT, key, 0 }; tab_w_toggle(NULL, &H_ARR, &c, &k); return g_th_n; }
static int t_shown(void) { int s = -1, n = 0; for (int k = 0; k < g_th_n; k++) if (g_th[k].isdef) { s = k; n++; } return n == 1 ? s : -2; }
static int t_adj(int key, int v) { TVar c = { TV_VT, MS_TAB_CAT, 0 }, k = { TV_VT, key, 0 }, x = { TV_VT, v, 0 }; th_clear(); tab_w_adjt(NULL, &H_ARR, &c, &k, &x, "test"); return g_th_n; }
static char *t_settings(const char *dir) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_SETTINGS, dir); return t_read(p); }
static int selftest_tab(const char *dir) {
  t_init_common(dir); t_mkdirs(dir); g_t_fake = 1; ms_guard_init(); g_k.log_captures = 100; g_kt.log_calls = 5;
  { const char *del[] = { MS_SETTINGS, MS_VALUES, MS_APPLIED, MS_SLOTS, "mods\\auto-kick.json", "mods\\pocket-presence.json", "mods\\tabtest-dyn.json", "mods\\modsettings-demo.json", "mods\\z-bad1.json", "mods\\z-bad2.json", "mods\\z-bad3.json", "mods\\z-bad4.json", "mods\\z-bad5.json", "mods\\z-bad6.json", "mods\\z-bad7.json" };
    for (size_t i = 0; i < sizeof del / sizeof *del; i++) { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\%s", dir, del[i]); DeleteFileA(p); } }
  g_tab_orig[TS_LIST] = (void *)to_list; g_tab_orig[TS_TOGGLE] = (void *)to_tog; g_tab_orig[TS_SLIDER] = (void *)to_sld; g_tab_orig[TS_ADJT] = (void *)to_adjt; g_tab_orig[TS_ADJS] = (void *)to_adjs;
  /* 1. no manifests at all: a header + one info row */
  cfg_poll(1);
  int n = t_list(MS_TAB_CAT);
  T_CHECK(n == 2 && g_th[0].type == 0 && g_th[0].key == -1 && !strcmp(g_th[0].disp, "Mod Settings") && g_th[1].key == MS_TAB_KEY_INFO + 1 && !g_th[1].en, "no manifests: the tab lists a 'Mod Settings' header + one greyed info row (%d rows)", n);
  T_CHECK(t_det(MS_TAB_KEY_INFO + 1) == 1 && t_shown() == 0, "the info row: one option item (the stock code greys it)");
  /* 2. the v3 samples + a dynasty manifest + a v2 manifest + slots.json + broken v3 manifests */
  t_put(dir, MS_SLOTS, T_SLOTS);
  t_put(dir, "mods\\auto-kick.json", T_V3_AK); t_put(dir, "mods\\pocket-presence.json", T_V3_PP); t_put(dir, "mods\\tabtest-dyn.json", T_V3_DY); t_put(dir, "mods\\modsettings-demo.json", T_DEMO);
  t_put(dir, "mods\\z-bad1.json", "{ \"schema\": 3, \"modid\": \"z-bad1\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"list\", \"key\": \"a\", \"label\": \"A\", \"options\": [\"x\", \"y\"], \"default\": 0, \"bind\": { \"type\": \"tuning\", \"asset\": \"a\", \"field\": \"f\", \"values\": [1, 2] } } ] }");
  t_put(dir, "mods\\z-bad2.json", "{ \"schema\": 3, \"modid\": \"z-bad2\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"knob\", \"key\": \"a\", \"label\": \"A\" } ] }");
  t_put(dir, "mods\\z-bad3.json", "{ \"schema\": 3, \"modid\": \"z-bad3\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"info\", \"label\": \"I\", \"bind\": { \"type\": \"api\" } } ] }");
  t_put(dir, "mods\\z-bad4.json", "{ \"schema\": 3, \"modid\": \"z-bad4\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"slider\", \"key\": \"s\", \"label\": \"S\", \"min\": 0, \"max\": 5000, \"step\": 1, \"default\": 1, \"bind\": { \"type\": \"api\" } } ] }");
  t_put(dir, "mods\\z-bad5.json", "{ \"schema\": 3, \"modid\": \"z-bad5\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"list\", \"key\": \"l\", \"label\": \"L\", \"options\": [\"1\",\"2\",\"3\",\"4\",\"5\",\"6\",\"7\",\"8\",\"9\"], \"default\": 0, \"bind\": { \"type\": \"dynasty\", \"slot\": \"z-bad5-l\" } } ] }");
  t_put(dir, "mods\\z-bad6.json", "{ \"schema\": 3, \"modid\": \"z-other-name\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"toggle\", \"key\": \"a\", \"label\": \"A\", \"default\": false, \"bind\": { \"type\": \"api\" } } ] }");
  t_put(dir, "mods\\z-bad7.json", "{ \"schema\": 3, \"modid\": \"z-bad7\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"toggle\", \"key\": \"a\", \"label\": \"A\", \"default\": 1, \"bind\": { \"type\": \"api\" } } ] }");
  cfg_poll(1);
  AcquireSRWLockShared(&g_cfg_lock); MsCfg *c = g_cfg; int nm = c->nmods, nr = c->nrows, np = c->nproblems; char loaded[256]; snprintf(loaded, sizeof loaded, "%s", c->loaded);
  int pb[7]; const char *why[7] = { "\"copy\" is required", "\"type\" must be toggle", "a info row has no \"bind\"", "the most is 1001", "at most 8 values", "the file name must be <modid>.json", "a toggle's \"default\" is true or false" };
  for (int k = 0; k < 7; k++) pb[k] = t_has_problem(c, why[k]);
  ReleaseSRWLockShared(&g_cfg_lock);
  T_CHECK(nm == 4 && nr == 5 + 5 + 2 + 3 && !strcmp(loaded, "auto-kick,modsettings-demo,pocket-presence,tabtest-dyn"), "v3 + v2: 4 manifests load (%s), %d rows", loaded, nr);
  T_CHECK(np == 7, "7 broken v3 manifests refused, one problem each (%d problems)", np);
  for (int k = 0; k < 7; k++) T_CHECK(pb[k], "refused: %s", why[k]);
  /* 3. the list: one header per mod sorted by title, then the mod's rows; keys; types; greyed info + headers */
  n = t_list(MS_TAB_CAT);
  T_CHECK(n == 4 + 15 + 1, "list: %d rows = 4 mod headers + 15 rows + 1 'problems' info row", n);
  int hA = t_row("Auto Kick"), hD = t_row("Mod Settings Demo (Test)"), hP = t_row("Pocket Presence"), hT = t_row("Tab Test Dynasty");
  T_CHECK(hA == 0 && hD == 6 && hP == 10 && hT == 16 && g_th[hA].type == 0 && g_th[hA].key == -1 && !g_th[hA].en, "list: mod headers sorted by title (Auto Kick 0, Mod Settings Demo 6, Pocket Presence 10, Tab Test Dynasty 16), type 0, key -1, greyed");
  T_CHECK(!strcmp(g_th[0].desc, "Auto Kick 0.3.0"), "list: a mod header's help = title + version (%s)", g_th[0].desc);
  T_CHECK(!strcmp(g_th[1].disp, "Kicking") && g_th[1].type == 0 && g_th[1].key == -1, "list: the manifest's own header row -> a header");
  T_CHECK(!strcmp(g_th[2].disp, "Auto Kick") && g_th[2].type == 1 && g_th[2].en && g_th[2].key == t_tkey("auto-kick", "enabled") && !strcmp(g_th[2].desc, "The game aims and times field goals and punts for you."), "list: a toggle row: type 1, enabled, key 0x4D00 + index, the description");
  T_CHECK(g_th[4].type == 1 && !strcmp(g_th[4].disp, "Reaction Delay"), "list: an 11-stop slider is drawn as a toggle row of stops");
  T_CHECK(g_th[5].type == 1 && !g_th[5].en && !strcmp(g_th[5].disp, "Your DLL reads these with ModSettings_Get."), "list: an info row: greyed, its text as the label");
  int fine = t_row("Fine Slider"); T_CHECK(fine > 0 && g_th[fine].type == 2, "list: a 101-stop slider is the game's slider row (type 2)");
  T_CHECK(strstr(g_th[t_row("Pocket Depth")].desc, "later Mod Settings version") != NULL, "list: a tuning row says its game-data change is not in this version");
  T_CHECK(strstr(g_th[t_row("Recruit Mode")].desc, "Dynasty setting") != NULL && strstr(g_th[t_row("No Slot")].desc, "without a save slot") != NULL, "list: dynasty rows say so (one without a slot says it is kept, not applied)");
  T_CHECK(g_th[n - 1].key == MS_TAB_KEY_INFO + 2 && strstr(g_th[n - 1].disp, "7 problem(s)") && strstr(g_th[n - 1].desc, "modsettings-status.json"), "list: the last row counts the skipped manifests and points at the status file");
  /* 4. any other category goes to the game, untouched; a missing holder / Variant too */
  int o0 = g_t_orig[TS_LIST]; n = t_list(1);
  T_CHECK(n == 0 && g_t_orig[TS_LIST] == o0 + 1, "category 1 (Game Options): passed to the original slot, nothing written by us");
  { TVar cv = { TV_VT, MS_TAB_CAT, 0 }; th_clear(); int o1 = g_t_orig[TS_LIST]; tab_w_list(NULL, NULL, &cv); T_CHECK(g_th_n == 0 && g_t_orig[TS_LIST] == o1 + 1, "our category with no holder: passed to the original"); }
  { th_clear(); int o1 = g_t_orig[TS_LIST]; tab_w_list(NULL, &H_ARR, NULL); T_CHECK(g_th_n == 0 && g_t_orig[TS_LIST] == o1 + 1, "no category Variant: passed to the original"); }
  { TVar c1 = { TV_VT, 1, 0 }, k1 = { TV_VT, 5, 0 }, v1 = { TV_VT, 1, 0 }; int a0 = g_t_orig[TS_TOGGLE], a1 = g_t_orig[TS_ADJT], a2 = g_t_orig[TS_SLIDER], a3 = g_t_orig[TS_ADJS];
    tab_w_toggle(NULL, &H_ARR, &c1, &k1); tab_w_adjt(NULL, &H_ARR, &c1, &k1, &v1, "x"); tab_w_slider(NULL, &H_OBJ, &c1, &k1); tab_w_adjs(NULL, &H_ARR, &c1, &k1, 3);
    T_CHECK(g_t_orig[TS_TOGGLE] == a0 + 1 && g_t_orig[TS_ADJT] == a1 + 1 && g_t_orig[TS_SLIDER] == a2 + 1 && g_t_orig[TS_ADJS] == a3 + 1, "every other wrapper passes another category to its original slot"); }
  /* 5. option details */
  int kS = t_tkey("auto-kick", "style"), kD = t_tkey("auto-kick", "delay-ms"), kE = t_tkey("auto-kick", "enabled");
  n = t_det(kS); T_CHECK(n == 3 && !strcmp(g_th[0].disp, "Safe") && !strcmp(g_th[2].disp, "Aggressive") && g_th[1].key == 1 && t_shown() == 1, "details: a list's options, the default (Balanced) is the one shown");
  n = t_det(kD); T_CHECK(n == 11 && !strcmp(g_th[0].disp, "0 ms") && !strcmp(g_th[10].disp, "500 ms") && t_shown() == 3, "details: a slider's 11 stops '0 ms'..'500 ms', 150 shown (%s)", t_shown() >= 0 ? g_th[t_shown()].disp : "?");
  n = t_det(kE); T_CHECK(n == 2 && !strcmp(g_th[0].disp, "Off") && !strcmp(g_th[1].disp, "On") && t_shown() == 1, "details: a toggle: Off / On, On shown (default true)");
  T_CHECK(t_det(MS_TAB_KEY0 + 4000) == 0 && t_det(-1) == 0, "details: an unknown key or a header key: nothing (and not passed on)");
  int kDm = t_tkey("modsettings-demo", "beta"); n = t_det(kDm); T_CHECK(n == 3 && !strcmp(g_th[1].disp, "Medium") && t_shown() == 1, "details: a v2 band row (no dynasty read yet): its default (Medium)");
  /* 6. a change: in memory at once, settings.json from the journal flush; other mods' entries kept */
  t_put(dir, MS_SETTINGS, "{ \"schema\": 3, \"global\": { \"gone-mod\": { \"x\": 1 }, \"auto-kick\": { \"old-row\": [1, 2], \"style\": 2 } } }");
  cfg_poll(1); n = t_det(kS); T_CHECK(t_shown() == 2, "settings.json: style = 2 (Aggressive) read at load");
  t_adj(kE, 0); t_adj(kD, 5); t_adj(kS, 0);
  T_CHECK(ms_get("auto-kick", "enabled") == 0 && ms_get("auto-kick", "delay-ms") == 5 && ms_get("auto-kick", "style") == 0, "change: in memory at once (ms_get 0 / 5 / 0)");
  n = t_det(kD); T_CHECK(t_shown() == 5 && !strcmp(g_th[5].disp, "250 ms"), "change: the tab shows the new value at once (250 ms)");
  T_CHECK(g_njq == 3, "change: 3 entries in the journal before the worker runs (%d)", g_njq);
  journal_flush();
  char *sj = t_settings(dir); static JNode sn[4096]; static char stx[1 << 16]; JDoc sd; int sr = sj ? j_parse(&sd, sn, 4096, stx, sizeof stx, sj, strlen(sj)) : -1;
  T_CHECK(sr >= 0 && g_njq == 0, "flush: settings.json written, valid JSON, journal empty");
  if (sr >= 0) {
    int g = j_get(&sd, sr, "global"), ak = j_get(&sd, g, "auto-kick"); double dv; long long iv;
    T_CHECK(d_type_is(&sd, j_get(&sd, ak, "enabled"), J_FALSE) && j_isnum(&sd, j_get(&sd, ak, "delay-ms"), &dv) && dv == 250 && j_isint(&sd, j_get(&sd, ak, "style"), &iv) && iv == 0, "settings.json: auto-kick enabled false, delay-ms 250 (the real value), style 0");
    T_CHECK(j_get(&sd, j_get(&sd, g, "gone-mod"), "x") >= 0 && sd.n[j_get(&sd, ak, "old-row")].type == J_ARR, "settings.json: a mod not installed and a removed row are kept");
  }
  free(sj);
  cfg_poll(1); T_CHECK(ms_get("auto-kick", "enabled") == 0 && ms_get("auto-kick", "delay-ms") == 5 && ms_get("auto-kick", "style") == 0, "re-read from settings.json (as after Back + reopen, or a restart): 0 / 5 / 0");
  n = t_list(MS_TAB_CAT); T_CHECK(n == 20, "reopen: the same 20 rows");
  /* a change while a re-read happens: the journal is applied to the new configuration before the swap */
  t_adj(kS, 2); t_put(dir, "mods\\auto-kick.json", T_V3_AK); cfg_poll(1); T_CHECK(ms_get("auto-kick", "style") == 2, "a change not written yet survives a configuration re-read (journal re-applied)"); journal_flush();
  /* bad changes: out of range, an info row, a header key */
  int kI = t_tkey("auto-kick", "#4"); LONG ch0 = g_tab_changes;
  t_adj(kS, 9); t_adj(kI, 0); t_adj(-1, 0); t_adj(MS_TAB_KEY_INFO + 1, 0);
  T_CHECK(g_tab_changes == ch0 && ms_get("auto-kick", "style") == 2, "a value out of range, an info row, a header and the 'no mods' row: no change");
  /* 7. dynasty rows: the change waits for the band pass; settings.json carries value + seq */
  int kM = t_tkey("tabtest-dyn", "mode"), kN = t_tkey("tabtest-dyn", "noslot");
  t_adj(kM, 2); t_adj(kN, 1); journal_flush();
  T_CHECK(pending_any() == 1 && ms_get("tabtest-dyn", "noslot") == 1, "dynasty row: pending for the band pass; the slot-less one answers from settings.json (1)");
  { int ids[] = { 0, 0 }, vals[] = { -8144, -7680 }; t_db(2, ids, vals); }
  T_CHECK(t_pass() == 1 && g_t_val[1] == -7678 && ms_get("tabtest-dyn", "mode") == 2, "dynasty row: the band pass writes record 1 bits 0-1 = 2 (-7680 -> -7678)");
  n = t_det(kM); T_CHECK(t_shown() == 2, "dynasty row: the tab shows the dynasty's value (Slow)");
  sj = t_settings(dir); sr = sj ? j_parse(&sd, sn, 4096, stx, sizeof stx, sj, strlen(sj)) : -1;
  { long long v, q; int dy = sr >= 0 ? j_get(&sd, j_get(&sd, j_get(&sd, sr, "dynasty"), "tabtest-dyn"), "mode") : -1;
    T_CHECK(dy >= 0 && j_isint(&sd, j_get(&sd, dy, "value"), &v) && v == 2 && j_isint(&sd, j_get(&sd, dy, "seq"), &q) && q >= 1, "settings.json: dynasty tabtest-dyn/mode { value 2, seq %lld }", dy >= 0 && j_isint(&sd, j_get(&sd, dy, "seq"), &q) ? q : -1); }
  free(sj);
  /* 8. slider_native = 1: every slider is the game's slider row, in stop units */
  g_kt.slider_native = 1; n = t_list(MS_TAB_CAT); T_CHECK(g_th[4].type == 2, "slider_native = 1: Reaction Delay is a slider row (type 2)");
  { th_clear(); TVar cv = { TV_VT, MS_TAB_CAT, 0 }, kv = { TV_VT, kD, 0 }; tab_w_slider(NULL, &H_OBJ, &cv, &kv);
    T_CHECK(tho_iv("min") == 0 && tho_iv("max") == 10 && tho_iv("step") == 1 && tho_iv("defaultValue") == 5 && tho_iv("resetValue") == 3 && tho_iv("key") == kD && !strcmp(tho_sv("leftLabel"), "0 ms") && !strcmp(tho_sv("rightLabel"), "500 ms"), "slider details: min 0 max 10 step 1, value 5, reset 3, labels '0 ms' / '500 ms'");
    tab_w_adjs(NULL, &H_ARR, &cv, &kv, 2); T_CHECK(ms_get("auto-kick", "delay-ms") == 2, "slider change (GameAdjustSliderSetting, int): stop 2"); }
  T_CHECK(t_det(kD) == 0, "slider_native: no toggle options for a slider row");
  g_kt.slider_native = 0; journal_flush();
  /* 9. tab = 0: everything goes to the game */
  g_kt.on = 0; o0 = g_t_orig[TS_LIST]; T_CHECK(t_list(MS_TAB_CAT) == 0 && g_t_orig[TS_LIST] == o0 + 1, "tab = 0: our category goes to the original too"); g_kt.on = 1;
  /* 10. the status file's tab block */
  status_write(); { char sp[MAX_PATH]; snprintf(sp, sizeof sp, "%s\\" MS_STATUS, dir); char *st = t_read(sp); static JNode nodes[16384]; static char text[1 << 18]; JDoc d;
    int root = st ? j_parse(&d, nodes, 16384, text, sizeof text, st, strlen(st)) : -1, tb = root >= 0 ? j_get(&d, root, "tab") : -1, rows = j_get(&d, tb, "rows");
    long long cat; T_CHECK(tb >= 0 && j_isint(&d, j_get(&d, tb, "category"), &cat) && cat == MS_TAB_CAT && d.n[rows].count == 20, "status: a tab block (category 19795, 20 rows as the tab shows them)");
    if (rows >= 0) { int x = d.n[rows].first; for (int k = 0; k < 4; k++) x = d.n[x].next; const char *sh = j_str(&d, j_get(&d, x, "shows")); T_CHECK(sh && !strcmp(sh, "100 ms"), "status: a row's 'shows' = what the tab draws (%s)", sh ? sh : "-"); }
    free(st); }
  /* 11. a fault inside the feed: caught, the feed off, the game's slot answers from then on */
  g_th_fault = 1; o0 = g_t_orig[TS_LIST]; t_list(MS_TAB_CAT); g_th_fault = 0;
  T_CHECK(g_tab_off == 1 && g_disabled == 0, "a fault in the list feed: caught; the tab feed is off, the band writer is not");
  T_CHECK(t_list(MS_TAB_CAT) == 0 && g_t_orig[TS_LIST] == o0 + 1, "after the fault: our category goes to the original slot (the tab shows its fallback row)");
  printf("tab: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- dump: pins, the install, every broken pin */
static void t_tab_reset(void) { g_tab_done = 0; snprintf(g_tab_state, sizeof g_tab_state, "not installed"); g_tab_reason[0] = 0; for (int s = 0; s < TS_N; s++) g_tab_orig[s] = NULL; g_ps_ndone = 0; }
static unsigned char g_t_save[MS_NSITES][5];
static void t_reset_install(void) {
  for (int i = 0; i < MS_NSITES; i++) { g_orig[i] = NULL; g_site_ok[i] = 0; }
  g_installed_sites = 0; g_ps_ndone = 0; snprintf(g_state, sizeof g_state, "loading"); g_reason[0] = 0;
}
static void t_restore_sites(const MsBuild *b) { for (int i = 0; i < MS_NSITES; i++) { unsigned char *s = g_ps_test_base + b->sites[i].call_rva; DWORD o; VirtualProtect(s, 5, PAGE_EXECUTE_READWRITE, &o); memcpy(s, g_t_save[i], 5); } }
static int t_site_points_to(const MsBuild *b, int i, void *fn) {
  unsigned char *s = g_ps_test_base + b->sites[i].call_rva; int32_t rel; memcpy(&rel, s + 1, 4); unsigned char *t = s + 5 + rel;
  if (s[0] != 0xe8 || t[0] != 0x48 || t[1] != 0xb8) return 0; uint64_t imm; memcpy(&imm, t + 2, 8); return imm == (uint64_t)(uintptr_t)fn;
}
static int t_untouched(const MsBuild *b) { for (int i = 0; i < MS_NSITES; i++) if (memcmp(g_ps_test_base + b->sites[i].call_rva, g_t_save[i], 5)) return 0; return 1; }
static int selftest_dump(const char *file) {
  t_init_common(NULL);
  if (!ps_t_map_dump(file)) { printf("  FAIL: cannot map %s\n", file); return 1; }
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)g_ps_test_base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(g_ps_test_base + dos->e_lfanew);
  uint32_t stamp = nt->FileHeader.TimeDateStamp; const MsBuild *b = ms_build_of(stamp);
  if (!b) { printf("  FAIL: %s: PE %u is not a known build\n", file, stamp); return 1; }
  printf("dump %s: %s (PE %u)\n", file, b->name, stamp);
  for (int i = 0; i < MS_NSITES; i++) memcpy(g_t_save[i], g_ps_test_base + b->sites[i].call_rva, 5);
  /* 1. every expectation is there and matches the dump's bytes */
  for (int i = 0; i < MS_NSITES; i++) {
    unsigned char *s = g_ps_test_base + b->sites[i].call_rva; unsigned char mask[64]; uint64_t h = ps_region_hash(s, 5, mask); const PsExpect *e = ps_expect(stamp, b->sites[i].call_rva);
    T_CHECK(e && e->hash == h, "%s: window hash %016llx as expected for this build", b->sites[i].flow, (unsigned long long)h);
  }
  /* 2. the full install */
  char why[300]; t_reset_install(); g_k.enabled = 1; for (int i = 0; i < 4; i++) g_k.cap[i] = 1;
  T_CHECK(ms_pins_check(why, sizeof why) && g_pb == b, "pins ok (%s)", b->name);
  ms_install();
  T_CHECK(g_installed_sites == 4 && !strcmp(g_state, "installed"), "install: 4 of 4 capture sites, state installed (%s)", g_state);
  for (int i = 0; i < MS_NSITES; i++) T_CHECK(t_site_points_to(b, i, MS_CAP_FN[i]) && g_orig[i] == (handler_fn)(g_ps_test_base + b->sites[i].handler_rva), "install: %s -> its wrapper through a trampoline; the original handler kept", b->sites[i].flow);
  t_restore_sites(b);
  /* 3. capture switches: only two sites */
  t_reset_install(); g_k.cap[1] = g_k.cap[3] = 0; ms_install();
  T_CHECK(g_installed_sites == 2 && !strcmp(g_state, "installed") && t_site_points_to(b, 0, MS_CAP_FN[0]) && !memcmp(g_ps_test_base + b->sites[1].call_rva, g_t_save[1], 5), "capture_myschool = capture_schedule = 0: 2 sites, the other two untouched");
  t_restore_sites(b); g_k.cap[1] = g_k.cap[3] = 1;
  /* 4. enabled = 0 */
  t_reset_install(); g_k.enabled = 0; ms_install(); T_CHECK(g_installed_sites == 0 && !strcmp(g_state, "idle") && t_untouched(b), "enabled = 0: nothing looked up or patched"); g_k.enabled = 1;
  /* 5. an unknown build */
  t_reset_install(); nt->FileHeader.TimeDateStamp = 12345; ms_install(); T_CHECK(g_installed_sites == 0 && !strcmp(g_state, "refused") && strstr(g_reason, "12345") && t_untouched(b), "an unknown PE timestamp: refused, nothing patched");
  /* 6. the other build's timestamp on this build's code */
  const MsBuild *o = &MS_BUILDS[b == &MS_BUILDS[0] ? 1 : 0]; t_reset_install(); nt->FileHeader.TimeDateStamp = o->stamp; ms_install();
  T_CHECK(g_installed_sites == 0 && !strcmp(g_state, "refused") && t_untouched(b), "the other build's timestamp (%s): refused at the function pins (%s)", o->name, g_reason);
  nt->FileHeader.TimeDateStamp = stamp;
  /* 7. a function head changed */
  unsigned char *fu = g_ps_test_base + b->funcs[MS_F_DIRTY].rva; unsigned char keep = fu[3]; fu[3] ^= 0xff;
  t_reset_install(); ms_install(); T_CHECK(g_installed_sites == 0 && !strcmp(g_state, "refused") && strstr(g_reason, "DIRTY") && t_untouched(b), "a function head changed: refused, nothing patched"); fu[3] = keep;
  /* 8. a call redirected by another tool: that site skipped, the rest installed */
  unsigned char *s2 = g_ps_test_base + b->sites[2].call_rva; int32_t rel; memcpy(&rel, s2 + 1, 4); int32_t r2 = rel + 16; memcpy(s2 + 1, &r2, 4);
  t_reset_install(); ms_install(); T_CHECK(g_installed_sites == 3 && !strcmp(g_state, "partial") && !g_orig[2] && strstr(g_site_why[2], "redirected"), "%s redirected: skipped (partial, 3 sites)", b->sites[2].flow);
  t_restore_sites(b);
  /* 9. the thunk's flow name changed */
  char *nm = (char *)(g_ps_test_base + b->sites[1].name_rva); char nk = nm[0]; nm[0] = 'X';
  t_reset_install(); ms_install(); T_CHECK(g_installed_sites == 3 && !g_orig[1] && strstr(g_site_why[1], "does not name"), "%s: the thunk names another flow: skipped", b->sites[1].flow);
  nm[0] = nk; t_restore_sites(b);
  /* 10. a byte inside a site's 64-byte window (not the call itself): patch_verify refuses that site */
  unsigned char *w3 = g_ps_test_base + b->sites[3].call_rva - 12; unsigned char wk = w3[0]; w3[0] ^= 0x5a;
  t_reset_install(); ms_install(); T_CHECK(g_installed_sites == 3 && !g_orig[3] && !memcmp(g_ps_test_base + b->sites[3].call_rva, g_t_save[3], 5), "%s: a changed byte in its window: refused by patch_verify, untouched", b->sites[3].flow);
  w3[0] = wk; t_restore_sites(b);
  /* 11. patch_verify = 0 lets that site through (the override exists and works) */
  w3[0] ^= 0x5a; g_ps.verify = 0; t_reset_install(); ms_install(); T_CHECK(g_installed_sites == 4, "patch_verify = 0: the same site is patched (the documented override)"); g_ps.verify = 1; w3[0] = wk; t_restore_sites(b);
  /* 12-16. the tab feed (layer 2): the settings adapter pins, the five slot swaps, and every refusal */
  {
    const MsTabPin *t = &b->tab; void **vt = (void **)(g_ps_test_base + t->vt_rva); void *save[MS_TAB_VT_MAX]; memcpy(save, vt, sizeof(void *) * (size_t)t->nvt);
    DWORD vo; VirtualProtect(vt, sizeof(void *) * (size_t)t->nvt, PAGE_READWRITE, &vo);
#define T_VT_RESTORE() memcpy(vt, save, sizeof(void *) * (size_t)t->nvt)
#define T_VT_SAME() (!memcmp(vt, save, sizeof(void *) * (size_t)t->nvt))
    T_CHECK(t->nvt == 31 && (uint64_t)(uintptr_t)save[0x30 / 8] - g_hdr_base == t->vt_stock[0x30 / 8], "tab: the settings adapter vtable (%d slots) is stock in the dump", t->nvt);
    t_tab_reset(); g_k.enabled = 1; g_kt.on = 1; int r = tab_install();
    T_CHECK(r == 1 && !strcmp(g_tab_state, "installed"), "tab: pins ok (adapter global -> object -> vtable, both holders, the Variant vtable), installed (%s%s)", g_tab_state, g_tab_reason);
    int ok = 1; for (int s = 0; s < TS_N; s++) if (vt[TAB_SLOT[s] / 8] != TAB_FN[s] || g_tab_orig[s] != (void *)(g_ps_test_base + t->vt_stock[TAB_SLOT[s] / 8])) ok = 0;
    T_CHECK(ok, "tab: slots 0x30 0x38 0x40 0x50 0x58 -> the wrappers; each original kept for every other category");
    int others = 1; for (int k = 0; k < t->nvt; k++) { int ours = 0; for (int s = 0; s < TS_N; s++) if (k == TAB_SLOT[s] / 8) ours = 1; if (!ours && vt[k] != save[k]) others = 0; }
    T_CHECK(others, "tab: the other %d slots untouched", t->nvt - TS_N);
    T_VT_RESTORE();
    /* another tool already swapped a slot: refused, nothing swapped */
    vt[0x38 / 8] = (void *)((uintptr_t)save[0x38 / 8] + 0x10); t_tab_reset(); r = tab_install();
    T_CHECK(r == 1 && !strcmp(g_tab_state, "refused") && strstr(g_tab_reason, "slot 0x38") && vt[0x30 / 8] == save[0x30 / 8], "tab: slot 0x38 not stock (another tool): refused, nothing swapped");
    T_VT_RESTORE();
    /* the adapter is not created yet (global null): wait, nothing swapped */
    void **glob = (void **)(g_ps_test_base + t->global_rva); void *gk = *glob; *glob = NULL; t_tab_reset(); r = tab_install();
    T_CHECK(r == 0 && !strcmp(g_tab_state, "waiting") && T_VT_SAME(), "tab: the adapter global not set yet: 'waiting' (retried by the worker), nothing swapped");
    *glob = gk;
    /* a value holder with another vtable: refused */
    void **ah = (void **)(g_ps_test_base + t->arr_rva); void *ak = *ah; *ah = (void *)((uintptr_t)ak + 8); t_tab_reset(); r = tab_install();
    T_CHECK(r == 1 && !strcmp(g_tab_state, "refused") && strstr(g_tab_reason, "holder") && T_VT_SAME(), "tab: the array holder's vtable differs: refused, nothing swapped");
    *ah = ak;
    /* tab = 0 */
    g_kt.on = 0; t_tab_reset(); r = tab_install(); T_CHECK(r == 1 && !strcmp(g_tab_state, "off") && T_VT_SAME(), "tab = 0: nothing looked up or swapped"); g_kt.on = 1;
    /* the other build's timestamp on this build's data: refused at the vtable (the other build's RVAs) */
    nt->FileHeader.TimeDateStamp = o->stamp; t_tab_reset(); r = tab_install();
    T_CHECK(r == 1 && !strcmp(g_tab_state, "refused") && T_VT_SAME(), "tab: the other build's timestamp (%s): refused (%s)", o->name, g_tab_reason);
    nt->FileHeader.TimeDateStamp = stamp;
    T_VT_RESTORE(); VirtualProtect(vt, sizeof(void *) * (size_t)t->nvt, vo, &vo);
#undef T_VT_RESTORE
#undef T_VT_SAME
  }
  printf("dump: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* load <dir>: what the DLL makes of a Mod Settings folder (the companion tests compare it with the page's own reading) */
static int selftest_load(const char *dir) {
  t_init_common(dir); g_log = NULL; MsCfg *c = cfg_build(); if (!c) return 1;
  printf("LOADED %s\n", c->loaded);
  for (int i = 0; i < c->nrows; i++) { const MsRow *r = &c->rows[i]; printf("ROW %s/%s %s gval %d gset %d want %d seq %lld\n", r->mod, r->id, r->store == ST_BAND ? "band" : "global", c->gval[i], c->gset[i], c->want[i], c->want_seq[i]); }
  const char *p = c->problems; while (*p) { const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p); printf("PROBLEM %.*s\n", (int)n, p); p += n; if (*p) p++; }
  free(c); return 0;
}
int main(int argc, char **argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (argc > 2 && !strcmp(argv[1], "load")) return selftest_load(argv[2]);
  const char *m = argc > 1 ? argv[1] : "";
  int f;
  if (!strcmp(m, "json")) f = selftest_json();
  else if (!strcmp(m, "config") && argc > 2) f = selftest_config(argv[2]);
  else if (!strcmp(m, "ini") && argc > 2) f = selftest_ini(argv[2]);
  else if (!strcmp(m, "records") && argc > 2) f = selftest_records(argv[2]);
  else if (!strcmp(m, "guard") && argc > 2) f = selftest_guard(argv[2]);
  else if (!strcmp(m, "tab") && argc > 2) f = selftest_tab(argv[2]);
  else if (!strcmp(m, "dump") && argc > 2) f = selftest_dump(argv[2]);
  else if (!strcmp(m, "patchsafe")) { InitializeCriticalSection(&g_cs); f = selftest_patchsafe(argc, argv); }
  else { printf("usage: modsettings-test json | config <dir> | ini <dir> | records <dir> | guard <dir> | tab <dir> | dump <dump> | patchsafe [n]\n"); return 2; }
  printf("self-test: %d failure(s)\n", f);
  return f;
}
