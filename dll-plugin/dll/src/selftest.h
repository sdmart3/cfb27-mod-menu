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
  t_put(dir, "mods\\z-bad6.json", "{ \"schema\": 3, \"modid\": \"!?\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"toggle\", \"key\": \"a\", \"label\": \"A\", \"default\": false, \"bind\": { \"type\": \"api\" } } ] }");
  t_put(dir, "mods\\z-bad7.json", "{ \"schema\": 3, \"modid\": \"z-bad7\", \"title\": \"B\", \"version\": \"1\", \"rows\": [ { \"type\": \"toggle\", \"key\": \"a\", \"label\": \"A\", \"default\": 1, \"bind\": { \"type\": \"api\" } } ] }");
  cfg_poll(1);
  AcquireSRWLockShared(&g_cfg_lock); MsCfg *c = g_cfg; int nm = c->nmods, nr = c->nrows, np = c->nproblems; char loaded[256]; snprintf(loaded, sizeof loaded, "%s", c->loaded);
  int pb[7]; const char *why[7] = { "\"copy\" is required", "\"type\" must be toggle", "a info row has no \"bind\"", "the most is 1001", "at most 8 values", "\"modid\" must be 2-40 characters", "a toggle's \"default\" is true or false" };
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
  T_CHECK(!strcmp(g_th[2].disp, "Auto Kick") && g_th[2].type == 1 && g_th[2].en && g_th[2].key == t_tkey("auto-kick", "enabled") && !strcmp(g_th[2].desc, "The game aims and times field goals and punts for you. Applies: from the next game."), "list: a toggle row: type 1, enabled, key 0x4D00 + index, the description");
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

/* ---------------------------------------------------------------- names: spec v3 "Names, duplicates and clashes" + "Adding rows to another mod's section" */
static void t_clear_mods(const char *dir) {
  char pat[MAX_PATH]; snprintf(pat, sizeof pat, "%s\\mods\\*.json", dir); WIN32_FIND_DATAA fd; HANDLE h = FindFirstFileA(pat, &fd);
  if (h != INVALID_HANDLE_VALUE) { do { char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\mods\\%s", dir, fd.cFileName); DeleteFileA(p); } while (FindNextFileA(h, &fd)); FindClose(h); }
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_SETTINGS, dir); DeleteFileA(p);
}
static char *t_mf(char *b, size_t n, const char *modid, const char *title, const char *ver, const char *extra, const char *rows) {
  snprintf(b, n, "{ \"schema\": 3, \"modid\": \"%s\", \"title\": \"%s\", \"version\": \"%s\"%s, \"rows\": [ %s ] }", modid, title, ver, extra, rows); return b;
}
#define T_TOG(k, l) "{ \"type\": \"toggle\", \"key\": \"" k "\", \"label\": \"" l "\", \"default\": true, \"bind\": { \"type\": \"api\" } }"
static int t_seq(const char *const *want, int n) {   /* the list's display texts == want[] in order (from row 0) */
  if (g_th_n != n) return 0; for (int k = 0; k < n; k++) if (strcmp(g_th[k].disp, want[k])) return 0; return 1;
}
static void t_dump_list(void) { for (int k = 0; k < g_th_n; k++) printf("    [%d] type %d key %d '%s'\n", k, g_th[k].type, g_th[k].key, g_th[k].disp); }
static int t_count(const char *text, const char *needle) { int n = 0; for (const char *p = text; p && (p = strstr(p, needle)); p++) n++; return n; }
static int selftest_names(const char *dir) {
  t_init_common(dir); t_mkdirs(dir); g_t_fake = 1; ms_guard_init(); g_kt.log_calls = 0;
  char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\names.log", dir); g_log = fopen(lp, "w"); g_log_sync = 1;
  g_tab_orig[TS_LIST] = (void *)to_list; g_tab_orig[TS_TOGGLE] = (void *)to_tog; g_tab_orig[TS_SLIDER] = (void *)to_sld; g_tab_orig[TS_ADJT] = (void *)to_adjt; g_tab_orig[TS_ADJS] = (void *)to_adjs;
  char b[2048]; t_clear_mods(dir);
  /* ---- A. names, duplicates, clashes */
  t_put(dir, "mods\\my-mod-two.json", t_mf(b, sizeof b, "My_Mod.Two", "Normalised", "1", "", T_TOG("n", "N")));
  t_put(dir, "mods\\pen-a.json", t_mf(b, sizeof b, "realism-pen", "Penalties", "1", "", T_TOG("p", "P")));
  t_put(dir, "mods\\pen-b.json", t_mf(b, sizeof b, "other-pen", "PENAL TIES", "1", "", T_TOG("q", "Q")));
  t_put(dir, "mods\\same-1.json", t_mf(b, sizeof b, "same-mod", "same mod", "1.2.0", "", T_TOG("x", "Old X")));
  t_put(dir, "mods\\same-2.json", t_mf(b, sizeof b, "same-mod", "Same Mod", "1.10.0", "", T_TOG("x", "New X") ", " T_TOG("y", "New Y")));
  t_put(dir, "mods\\clash-a.json", t_mf(b, sizeof b, "clash", "Alpha Clash", "1", "", T_TOG("a", "A")));
  t_put(dir, "mods\\clash-b.json", t_mf(b, sizeof b, "clash", "Beta Clash", "1", "", T_TOG("b", "B")));
  t_put(dir, "mods\\dupkey.json", t_mf(b, sizeof b, "dupkey", "Dup Key", "1", "", T_TOG("k", "First") ", " T_TOG("k", "Second") ", " T_TOG("m", "Third")));
  cfg_poll(1); cfg_poll(1);   /* twice: the "older copy" line is logged once */
  AcquireSRWLockShared(&g_cfg_lock); char loaded[512]; snprintf(loaded, sizeof loaded, "%s", g_cfg->loaded); int np = g_cfg->nproblems; ReleaseSRWLockShared(&g_cfg_lock);
  T_CHECK(np == 0 && !strcmp(loaded, "clash,clash@clash-b.json,dupkey,my-mod-two,realism-pen,other-pen,same-mod"), "loaded: %s (%d problems)", loaded, np);
  T_CHECK(strstr(loaded, "my-mod-two") != NULL, "modid normalised: \"My_Mod.Two\" -> my-mod-two");
  T_CHECK(ms_get("same-mod", "y") == 1 && t_tkey("same-mod", "y") > 0, "same modid + same mod (title matches, shared key): one section, the highest version (1.10.0 over 1.2.0) wins");
  char *lg = t_read(lp);
  T_CHECK(lg && t_count(lg, "same-1.json: ignored: older copy of same-mod (version 1.2.0; same-2.json has version 1.10.0)") == 1, "the older copy is logged once (\"ignored: older copy\"), across two reloads");
  T_CHECK(lg && strstr(lg, "clash-b.json: modid clash is already used by another mod: both are shown; this one's settings are stored as clash@clash-b.json"), "same modid + different mods: both kept, the second stored as clash@clash-b.json (logged)");
  T_CHECK(lg && strstr(lg, "the key \"k\" appears twice in this mod: the first row wins"), "a duplicate key inside one mod: logged");
  free(lg);
  int n = t_list(MS_TAB_CAT);
  { const char *want[] = { "Alpha Clash", "A", "Beta Clash (duplicate id)", "B", "Dup Key", "First", "Third", "Normalised", "N", "PENAL TIES (pen-b.json)", "Q", "Penalties (pen-a.json)", "P", "Same Mod", "New X", "New Y" };
    int ok = t_seq(want, (int)(sizeof want / sizeof *want)); if (!ok) t_dump_list();
    T_CHECK(ok, "list (%d rows): sections sorted by visible title; matching titles get their source file; the second 'clash' says duplicate id; the duplicate key's first row (First) wins, Second not shown; the newer Same Mod copy's rows", n); }
  { int k2 = t_row("Beta Clash (duplicate id)"); T_CHECK(k2 >= 0 && strstr(g_th[k2].desc, "clash@clash-b.json") != NULL, "the duplicate-id header's help names where its settings are kept"); }
  t_adj(t_tkey("clash@clash-b.json", "b"), 0); journal_flush();
  { char *sj = t_settings(dir); T_CHECK(sj && strstr(sj, "\"clash@clash-b.json\": { \"b\": false }") && !strstr(sj, "\"clash\": {"), "a change in the second 'clash' is stored under clash@clash-b.json; the first 'clash' is untouched"); free(sj); }
  T_CHECK(ms_get("clash", "a") == 1 && ms_get("clash@clash-b.json", "b") == 0, "ms_get: clash/a 1 (untouched), clash@clash-b.json/b 0");
  /* ---- B. adding rows to another mod's section */
  t_clear_mods(dir);
  t_put(dir, "mods\\owner-mod.json", t_mf(b, sizeof b, "owner-mod", "Realism Penalties", "1", "", T_TOG("own1", "Owner Row")));
  t_put(dir, "mods\\addon-z.json", t_mf(b, sizeof b, "addon-z", "Zed Addon", "1", ", \"section\": \"owner-mod\"", T_TOG("late-hits", "Late Hits Z")));
  t_put(dir, "mods\\addon-a.json", t_mf(b, sizeof b, "addon-a", "Able Addon", "1", ", \"section\": \"Owner_Mod\"", T_TOG("late-hits", "Late Hits A")));
  t_put(dir, "mods\\closed-owner.json", t_mf(b, sizeof b, "closed-owner", "Kept Closed", "1", ", \"closed\": true", T_TOG("c1", "Closed Row")));
  t_put(dir, "mods\\closed-addon.json", t_mf(b, sizeof b, "closed-addon", "Closed Addon", "1", ", \"section\": \"closed-owner\"", T_TOG("c2", "Refused Row")));
  t_put(dir, "mods\\sh-1.json", t_mf(b, sizeof b, "sh-1", "Kicker Tweaks", "1", ", \"section\": \"@pass-rush\"", T_TOG("r1", "Rush One")));
  t_put(dir, "mods\\sh-2.json", t_mf(b, sizeof b, "sh-2", "A Rush Mod", "1", ", \"section\": \"@Pass_Rush\"", T_TOG("r2", "Rush Two")));
  t_put(dir, "mods\\orphan.json", t_mf(b, sizeof b, "orphan", "Orphan Mod", "1", ", \"section\": \"not-installed\"", T_TOG("o1", "Orphan Row")));
  t_put(dir, "mods\\bad-section.json", t_mf(b, sizeof b, "bad-section", "Bad", "1", ", \"section\": 5", T_TOG("z", "Z")));
  t_put(dir, "mods\\bad-closed.json", t_mf(b, sizeof b, "bad-closed", "Bad", "1", ", \"closed\": \"yes\"", T_TOG("z", "Z")));
  cfg_poll(1);
  AcquireSRWLockShared(&g_cfg_lock); np = g_cfg->nproblems; int p1 = t_has_problem(g_cfg, "\"section\" is another mod's modid"), p2 = t_has_problem(g_cfg, "\"closed\" is true or false"); ReleaseSRWLockShared(&g_cfg_lock);
  T_CHECK(np == 2 && p1 && p2, "a \"section\" that is not text and a \"closed\" that is not true / false: refused (%d problems)", np);
  n = t_list(MS_TAB_CAT);
  { const char *want[] = { "Closed Addon", "Meant for the closed-owner section", "Refused Row", "Kept Closed", "Closed Row", "Orphan Mod", "Orphan Row", "Pass Rush", "added by A Rush Mod", "Rush Two", "added by Kicker Tweaks", "Rush One",
                           "Realism Penalties", "Owner Row", "added by Able Addon", "Late Hits A", "added by Zed Addon", "Late Hits Z", "2 problem(s) in the mod manifests" };
    int ok = t_seq(want, (int)(sizeof want / sizeof *want)); if (!ok) t_dump_list();
    T_CHECK(ok, "list (%d rows): sections by visible title; contributors after the owner's rows behind 'added by <title>', by title; the shared @pass-rush section is 'Pass Rush' (two spellings, one section); a closed owner's contributor keeps its own section + a note; a missing target falls back to the contributor's own section", n); }
  { int k = t_row("Meant for the closed-owner section"); T_CHECK(k >= 0 && g_th[k].type == 1 && !g_th[k].en && g_th[k].key == MS_TAB_KEY_INFO + 3 && strstr(g_th[k].desc, "does not take rows"), "the refusal note: a greyed info row (key 0x4CF3) saying why");
    T_CHECK(t_det(MS_TAB_KEY_INFO + 3) == 1, "the note's options: one item"); }
  { t_list(MS_TAB_CAT); int k = t_row("added by Able Addon"); T_CHECK(k >= 0 && g_th[k].type == 0 && g_th[k].key == -1 && strstr(g_th[k].desc, "addon-a"), "'added by' is a sub-header (type 0) whose help names the contributor's modid"); }
  T_CHECK(t_tkey("addon-a", "late-hits") != t_tkey("addon-z", "late-hits") && t_tkey("addon-a", "late-hits") > 0, "two contributors with the same key: two rows, each under its own modid");
  t_adj(t_tkey("addon-a", "late-hits"), 0); journal_flush();
  { char *sj = t_settings(dir); T_CHECK(sj && strstr(sj, "\"addon-a\": { \"late-hits\": false }") && !strstr(sj, "\"addon-z\"") && !strstr(sj, "\"owner-mod\""), "a change of a contributed row is stored under the contributor (addon-a/late-hits), never the owner or the other contributor"); free(sj); }
  T_CHECK(ms_get("addon-a", "late-hits") == 0 && ms_get("addon-z", "late-hits") == 1, "ms_get: addon-a/late-hits 0, addon-z/late-hits still 1");
  /* the owner removed: its contributors fall back to their own sections, values kept */
  { char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\mods\\owner-mod.json", dir); DeleteFileA(p); } cfg_poll(1); n = t_list(MS_TAB_CAT);
  T_CHECK(t_row("Able Addon") >= 0 && t_row("Zed Addon") >= 0 && t_row("added by Able Addon") < 0 && ms_get("addon-a", "late-hits") == 0, "the owner uninstalled: each contributor shows its own section (header = its title); addon-a's value is still 0");
  if (g_log) fclose(g_log); g_log = stdout;
  printf("names: %d failure(s)\n", g_t_fails); return g_t_fails;
}

/* ---------------------------------------------------------------- rules: spec v3 "Safety and behaviour rules" (1, 2, 3, 4, 6, 7, 8, 9, 19) */
static int t_has_row(const char *disp) { return t_row(disp) >= 0; }
static int selftest_rules(const char *dir) {
  t_init_common(dir); t_mkdirs(dir); g_t_fake = 1; ms_guard_init(); g_kt.log_calls = 0;
  char lp[MAX_PATH]; snprintf(lp, sizeof lp, "%s\\rules.log", dir); g_log = fopen(lp, "w"); g_log_sync = 1;
  g_tab_orig[TS_LIST] = (void *)to_list; g_tab_orig[TS_TOGGLE] = (void *)to_tog; g_tab_orig[TS_SLIDER] = (void *)to_sld; g_tab_orig[TS_ADJT] = (void *)to_adjt; g_tab_orig[TS_ADJS] = (void *)to_adjs;
  t_clear_mods(dir);
  snprintf(g_mm_config, sizeof g_mm_config, "%s\\manager_config.json", dir);
  t_put(dir, "manager_config.json", "{ \"Games\": { \"Madden27\": {}, \"CollegeFB27\": { \"Options\": { \"SelectedPack\": \"Runner\" }, \"Packs\": { \"Default\": \"Off Mod.fbmod:True\", \"Runner\": \"zzRunner - Active Mod.fbmod:True|Off Mod.fbmod:False\" } } } }");
#define TUN(a, f, cp) "\"bind\": { \"type\": \"tuning\", \"asset\": \"" a "\", \"field\": \"" f "\", \"copy\": \"" cp "\""
  t_put(dir, "mods\\rule-a.json", "{ \"schema\": 3, \"modid\": \"rule-a\", \"title\": \"Rule A\", \"version\": \"1\", \"fbmod\": \"Active Mod.fbmod\",\n"
    " \"about\": \"What Rule A does.\", \"credits\": \"Made by the test.\", \"links\": [ { \"title\": \"Site\", \"url\": \"https://example.org/rule-a\" } ], \"rows\": [\n"
    " { \"type\": \"list\", \"key\": \"depth\", \"label\": \"Depth\", \"options\": [\"Shallow\", \"Stock\", \"Deep\"], \"default\": 1, \"applies\": \"now\", " TUN("blocking/BT", "Pocket", "simulation") ", \"values\": [2.0, 3.0, 4.5] } },\n"
    " { \"type\": \"toggle\", \"key\": \"jump\", \"label\": \"Jump\", \"default\": true, " TUN("catching/C", "Jump", "base") ", \"values\": [false, true] } },\n"
    " { \"type\": \"list\", \"key\": \"mixed\", \"label\": \"Mixed\", \"options\": [\"A\", \"B\"], \"default\": 0, " TUN("x/Y", "Z", "both") ", \"values\": [1, \"x\"] } },\n"
    " { \"type\": \"toggle\", \"key\": \"master\", \"label\": \"Master\", \"default\": true, \"bind\": { \"type\": \"api\" } },\n"
    " { \"type\": \"list\", \"key\": \"style\", \"label\": \"Style\", \"options\": [\"Safe\", \"Balanced\", \"Aggressive\"], \"default\": 1, \"bind\": { \"type\": \"api\" }, \"visibleIf\": { \"key\": \"master\", \"eq\": true } },\n"
    " { \"type\": \"toggle\", \"key\": \"extra\", \"label\": \"Extra\", \"default\": false, \"bind\": { \"type\": \"api\" }, \"enabledIf\": { \"key\": \"style\", \"in\": [\"Aggressive\", 0] } },\n"
    " { \"type\": \"slider\", \"key\": \"restart-me\", \"label\": \"Restart Me\", \"min\": 0, \"max\": 10, \"step\": 1, \"default\": 5, \"applies\": \"restart\", \"bind\": { \"type\": \"api\" } } ] }");
  t_put(dir, "mods\\rule-b.json", "{ \"schema\": 3, \"modid\": \"rule-b\", \"title\": \"Rule B\", \"version\": \"1\", \"rows\": [\n"
    " { \"type\": \"list\", \"key\": \"depth-b\", \"label\": \"Depth B\", \"options\": [\"X\", \"Y\"], \"default\": 0, " TUN("Blocking/BT", "Pocket", "both") ", \"values\": [1, 2] } },\n"
    " { \"type\": \"toggle\", \"key\": \"stray\", \"label\": \"Stray\", \"default\": true, \"bind\": { \"type\": \"api\" }, \"visibleIf\": { \"mod\": \"rule-a\", \"key\": \"master\", \"eq\": false } } ] }");
  t_put(dir, "mods\\rule-off.json", "{ \"schema\": 3, \"modid\": \"rule-off\", \"title\": \"Off Mod\", \"version\": \"1\", \"fbmod\": \"Off Mod.fbmod\", \"rows\": [ " T_TOG("o", "Off Row") " ] }");
  t_put(dir, "mods\\rule-ghost.json", "{ \"schema\": 3, \"modid\": \"rule-ghost\", \"title\": \"Ghost\", \"version\": \"1\", \"fbmod\": \"Ghost.fbmod\", \"rows\": [ " T_TOG("g", "Ghost Row") " ] }");
  t_put(dir, "mods\\rule-pin.json", "{ \"schema\": 3, \"modid\": \"rule-pin\", \"title\": \"Pinned\", \"version\": \"1\", \"gameVersion\": 1234, \"rows\": [ { \"type\": \"toggle\", \"key\": \"p\", \"label\": \"Pinned Row\", \"default\": true, " TUN("p/Q", "R", "both") ", \"values\": [0, 1] } } ] }");
  const char *bad[][2] = { { "z-applies", "\"applies\": \"soon\"" }, { "z-vis", "\"visibleIf\": { \"key\": \"a\" }" }, { "z-gv", "" }, { "z-link", "" }, { "z-fb", "" } };
  char b[2048];
  for (int k = 0; k < 5; k++) {
    char row[400]; snprintf(row, sizeof row, "{ \"type\": \"toggle\", \"key\": \"a\", \"label\": \"A\", \"default\": true, \"bind\": { \"type\": \"api\" }%s%s }", bad[k][1][0] ? ", " : "", bad[k][1]);
    const char *top = k == 2 ? ", \"gameVersion\": \"x\"" : k == 3 ? ", \"links\": [ { \"title\": \"T\", \"url\": \"ftp://x\" } ]" : k == 4 ? ", \"fbmod\": \"mod.zip\"" : "";
    char nm[64]; snprintf(nm, sizeof nm, "mods\\%s.json", bad[k][0]); t_put(dir, nm, t_mf(b, sizeof b, bad[k][0], "Bad", "1", top, row));
  }
  g_run_gamever = 4338538; cfg_poll(1); cfg_poll(1);
  AcquireSRWLockShared(&g_cfg_lock); int np = g_cfg->nproblems; int pr[5] = { t_has_problem(g_cfg, "\"applies\" is"), t_has_problem(g_cfg, "\"visibleIf\" is"), t_has_problem(g_cfg, "\"gameVersion\" is"), t_has_problem(g_cfg, "links[0] needs"), t_has_problem(g_cfg, "\"fbmod\" is your") }; ReleaseSRWLockShared(&g_cfg_lock);
  T_CHECK(np == 5 && pr[0] && pr[1] && pr[2] && pr[3] && pr[4], "refused: applies not now / next-game / restart, a malformed visibleIf, a gameVersion that is not a number, a non-http link, an fbmod that is not .fbmod (%d problems)", np);
  char *lg = t_read(lp);
  /* 1. two mods on one field */
  int n = t_list(MS_TAB_CAT);
  T_CHECK(t_has_row("Depth - also set by Rule B") && t_has_row("Depth B - also set by Rule A"), "rule 1: both rows on blocking/BT : Pocket read 'also set by <the other mod>' (asset case ignored; copies simulation + both overlap)");
  T_CHECK(t_has_row("Jump") && lg && t_count(lg, "both set blocking/BT : Pocket: the last change wins") == 1, "rule 1: logged once across two reloads; a row on another field is not marked");
  /* 2. As installed */
  int kD = t_tkey("rule-a", "depth"), kJ = t_tkey("rule-a", "jump");
  n = t_det(kD); T_CHECK(n == 4 && !strcmp(g_th[0].disp, "As installed") && !strcmp(g_th[3].disp, "Deep") && t_shown() == 0, "rule 2: a tuning list gets 'As installed' first, and it is the default (the manifest's default is not)");
  n = t_det(kJ); T_CHECK(n == 3 && !strcmp(g_th[0].disp, "As installed") && !strcmp(g_th[1].disp, "Off") && !strcmp(g_th[2].disp, "On") && t_shown() == 0, "rule 2: a tuning toggle: As installed / Off / On");
  t_adj(kD, 3); t_adj(kJ, 1); journal_flush();
  { char *sj = t_settings(dir); T_CHECK(sj && strstr(sj, "\"rule-a\": { \"depth\": 2, \"jump\": false }"), "rule 2: picked Deep and Off: settings.json depth 2 (its list position), jump false"); free(sj); }
  t_adj(kD, 0); journal_flush(); cfg_poll(1);
  { char *sj = t_settings(dir); T_CHECK(sj && strstr(sj, "\"depth\": \"as-installed\"") && ms_get("rule-a", "depth") == 0, "rule 2: back to As installed: stored as \"as-installed\", read back as option 0"); free(sj); }
  /* 3 + 4. applies and game style */
  n = t_list(MS_TAB_CAT);
  { int r1 = t_row("Depth - also set by Rule B"), r2 = t_row("Restart Me"), r3 = t_row("Master"), r4 = t_row("Jump");
    T_CHECK(r1 >= 0 && strstr(g_th[r1].desc, "Applies: right away.") && r2 >= 0 && strstr(g_th[r2].desc, "Applies: after a restart of the game.") && r3 >= 0 && strstr(g_th[r3].desc, "Applies: from the next game."), "rule 3: the help line says when a change applies (now / restart / the default next-game)");
    T_CHECK(strstr(g_th[r1].desc, "Simulation style only.") && r4 >= 0 && strstr(g_th[r4].desc, "Not used in Simulation style."), "rule 4: copy simulation -> 'Simulation style only'; copy base -> 'Not used in Simulation style'"); }
  /* 6. gameVersion pin */
  { int kP = t_tkey("rule-pin", "p"), r = t_row("Pinned Row"); LONG ch = g_tab_changes;
    T_CHECK(r >= 0 && !g_th[r].en && strstr(g_th[r].desc, "not available on this game version"), "rule 6: pinned to 1234, running 4338538: the tuning row is greyed, 'not available on this game version'");
    n = t_det(kP); T_CHECK(n == 1 && !strcmp(g_th[0].disp, "not available on this game version"), "rule 6: its options: one item saying so");
    t_adj(kP, 1); T_CHECK(g_tab_changes == ch, "rule 6: a change is refused (never written)");
    g_run_gamever = 0; cfg_poll(1); t_list(MS_TAB_CAT); r = t_row("Pinned Row"); T_CHECK(r >= 0 && g_th[r].en, "rule 6: an unknown running version: the pin is not checked (the row is live)");
    t_put(dir, "mods\\rule-pin.json", "{ \"schema\": 3, \"modid\": \"rule-pin\", \"title\": \"Pinned\", \"version\": \"1\", \"gameVersion\": [1234, 4338538], \"rows\": [ { \"type\": \"toggle\", \"key\": \"p\", \"label\": \"Pinned Row\", \"default\": true, " TUN("p/Q", "R", "both") ", \"values\": [0, 1] } } ] }");
    g_run_gamever = 4338538; cfg_poll(1); t_list(MS_TAB_CAT); r = t_row("Pinned Row"); T_CHECK(r >= 0 && g_th[r].en, "rule 6: a pin list that names the running version: live"); }
  /* 7. values of different types */
  { int r = t_row("Mixed"); LONG ch = g_tab_changes; T_CHECK(r >= 0 && !g_th[r].en && strstr(g_th[r].desc, "different types"), "rule 7: values of mixed types: greyed, says why");
    n = t_det(t_tkey("rule-a", "mixed")); T_CHECK(n == 1 && strstr(g_th[0].disp, "different types"), "rule 7: its options: one item saying why"); t_adj(t_tkey("rule-a", "mixed"), 1); T_CHECK(g_tab_changes == ch, "rule 7: a change is refused"); }
  /* 8. the mod turned off in the selected pack */
  t_list(MS_TAB_CAT);
  T_CHECK(t_has_row("Rule A") && !t_has_row("Off Mod") && !t_has_row("Off Row") && !t_has_row("Ghost"), "rule 8: on in the pack (a runner's 'zzRunner - ' copy counts): shown; off, or not in the pack at all: hidden");
  g_kt.show_inactive = 1; t_list(MS_TAB_CAT);
  { int r = t_row("Off Mod (mod not active)"), rr = t_row("Off Row"); LONG ch = g_tab_changes;
    T_CHECK(r >= 0 && rr >= 0 && !g_th[rr].en && t_has_row("Ghost (mod not active)"), "rule 8: show_inactive = 1: greyed sections '(mod not active)'");
    t_adj(t_tkey("rule-off", "o"), 0); T_CHECK(g_tab_changes == ch, "rule 8: a row of an inactive mod cannot be changed"); }
  g_kt.show_inactive = 0;
  /* 9. visibleIf / enabledIf */
  int kM = t_tkey("rule-a", "master"), kS = t_tkey("rule-a", "style");
  t_list(MS_TAB_CAT); T_CHECK(t_has_row("Style") && !g_th[t_row("Extra")].en, "rule 9: Master on -> Style visible; Style Balanced -> Extra disabled (enabledIf in [Aggressive, 0])");
  t_adj(kS, 2); t_list(MS_TAB_CAT); T_CHECK(g_th[t_row("Extra")].en, "rule 9: Style Aggressive (by label) -> Extra enabled");
  t_adj(kS, 0); t_list(MS_TAB_CAT); T_CHECK(g_th[t_row("Extra")].en, "rule 9: Style Safe (by position 0) -> Extra enabled");
  t_adj(kM, 0); t_list(MS_TAB_CAT); { LONG ch = g_tab_changes; T_CHECK(!t_has_row("Style"), "rule 9: Master off -> Style hidden"); t_adj(kS, 1); T_CHECK(g_tab_changes == ch, "rule 9: a hidden row cannot be changed"); }
  t_adj(kM, 1); t_list(MS_TAB_CAT); T_CHECK(t_has_row("Style"), "rule 9: Master on again -> Style back");
  free(lg); lg = t_read(lp);
  T_CHECK(t_has_row("Stray") && lg && strstr(lg, "rule-b/stray: visibleIf names rule-a/master, which is not a row of this mod or of a mod in its section: the condition is ignored"), "rule 9: a condition on an unrelated mod's row is ignored (logged), the row stays visible");
  /* 19. help pages */
  { int r = t_row("About Rule A"); T_CHECK(r >= 0 && r == t_row("Rule A") + 1 && !g_th[r].en && g_th[r].key == MS_TAB_KEY_ABOUT + 0, "rule 19: an 'About Rule A' row right under the header (greyed, key 0x4C00 + mod)");
    T_CHECK(r >= 0 && strstr(g_th[r].desc, "What Rule A does.") && strstr(g_th[r].desc, "Credits: Made by the test.") && strstr(g_th[r].desc, "Links:\nSite: https://example.org/rule-a"), "rule 19: its help panel holds about, credits and links");
    T_CHECK(t_det(MS_TAB_KEY_ABOUT + 0) == 1, "rule 19: its options: one item"); }
  free(lg);
  if (g_log) fclose(g_log); g_log = stdout; g_mm_config[0] = 0;
  printf("rules: %d failure(s)\n", g_t_fails); return g_t_fails;
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
  else if (!strcmp(m, "names") && argc > 2) f = selftest_names(argv[2]);
  else if (!strcmp(m, "rules") && argc > 2) f = selftest_rules(argv[2]);
  else if (!strcmp(m, "dump") && argc > 2) f = selftest_dump(argv[2]);
  else if (!strcmp(m, "patchsafe")) { InitializeCriticalSection(&g_cs); f = selftest_patchsafe(argc, argv); }
  else { printf("usage: modsettings-test json | config <dir> | ini <dir> | records <dir> | guard <dir> | tab <dir> | dump <dump> | patchsafe [n]\n"); return 2; }
  printf("self-test: %d failure(s)\n", f);
  return f;
}
