/* msv3.h: spec v3 manifests (docs/manifest-spec.md) and settings.json, for modsettings.dll (included by modsettings.c,
 * before load_manifest). A v3 manifest is all or nothing, like v2: any problem and the mod adds no rows (it is listed in the status file).
 *   rows: header / info (label only) and toggle / list / slider (key, label, description, default, bind)
 *   bind: api (the value lives in settings.json; DLL mods read it with ms_get), tuning (stored in settings.json now; the tuning write is the
 *         next step of the Mod Menu plan), dynasty (the value is mirrored into the loaded dynasty's band slot, layer 1's band writer; the slot
 *         is looked up by name in slots.json beside this DLL: an entry's "slot", or "<mod>-<row>")
 * settings.json (schema 3, beside this DLL): { "global": { modid: { key: value } }, "dynasty": { modid: { key: { "value": v, "seq": n } } } }
 *   value: a toggle true / false, a list its option position, a slider the real number. v2 rows keep their option index.
 *   Written by this DLL when a row is changed in the game's Mod Settings tab (mstab.h); read here at every (re)load. Entries of mods that
 *   are not installed are kept when the file is rewritten. A global row named in settings.json beats values.json (the v2 page's file). */
#ifndef MS_V3_H
#define MS_V3_H
#define MS_SETTINGS "settings.json"
#define MS_SLOTS "slots.json"
#define MS_DYN_MAXOPT 8   /* a dynasty row holds at most 8 values (3 band bits) */
static int row_index(const MsCfg *c, const char *mod, const char *row);

static int key_ok(const char *s) {
  size_t n = s ? strlen(s) : 0; if (n < 1 || n > 32) return 0;
  for (size_t i = 0; i < n; i++) if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '-' || s[i] == '_')) return 0;
  return 1;
}
static int j_isnum(const JDoc *d, int i, double *out) { const JNode *x = j_at(d, i); if (!x || x->type != J_NUM) return 0; if (out) *out = x->num; return 1; }
static int j_isbool(const JDoc *d, int i, int *out) { const JNode *x = j_at(d, i); if (!x || (x->type != J_TRUE && x->type != J_FALSE)) return 0; if (out) *out = x->type == J_TRUE; return 1; }
static int slider_stops(double mn, double mx, double st) { return (int)((mx - mn) / st + 1e-9) + 1; }
static int slider_stop_of(const MsRow *r, double v) { long long k = (long long)((v - r->smin) / r->sstep + (v >= r->smin ? 0.5 : -0.5)); int n = r->nopt - r->asis; if (k < 0) k = 0; if (k >= n) k = n - 1; return (int)k; }   /* a REAL stop (no "As installed") */
static double slider_value(const MsRow *r, int stop) { return r->smin + (double)stop * r->sstep; }

/* slots.json beside the DLL (the shared slot list, modsettings/slots.json): read once per configuration build */
typedef struct { char name[96]; int rec, bit, width; } MsSlot;
static MsSlot g_slots[128]; static int g_nslots;
static void slots_load(void) {
  g_nslots = 0; char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_SLOTS, g_dir); size_t len; char *src = read_file(p, 256 * 1024, &len); if (!src) return;
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES); char *text = (char *)malloc(J_TEXT); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES, text, J_TEXT, src, len) : -1, sl = root >= 0 ? j_get(&d, root, "slots") : -1;
  if (sl >= 0 && d.n[sl].type == J_ARR) for (int x = d.n[sl].first; x >= 0 && g_nslots < 128; x = d.n[x].next) {
    const char *nm = j_str(&d, j_get(&d, x, "slot")), *mod = j_str(&d, j_get(&d, x, "mod")), *row = j_str(&d, j_get(&d, x, "row")); long long rec, bit, w;
    if (!j_isint(&d, j_get(&d, x, "record"), &rec) || !j_isint(&d, j_get(&d, x, "bit"), &bit) || !j_isint(&d, j_get(&d, x, "width"), &w)) continue;
    MsSlot *s = &g_slots[g_nslots]; if (nm) snprintf(s->name, sizeof s->name, "%s", nm); else if (mod && row) snprintf(s->name, sizeof s->name, "%s-%s", mod, row); else continue;
    s->rec = (int)rec; s->bit = (int)bit; s->width = (int)w; g_nslots++;
  }
  free(nodes); free(text); free(src);
}
static const MsSlot *slot_find(const char *name) { for (int i = 0; i < g_nslots; i++) if (!strcmp(g_slots[i].name, name)) return &g_slots[i]; return NULL; }

/* ---- names, duplicates and clashes (spec v3, added 11-11) */
/* modid normalisation: lower case; space / '_' / '.' -> '-'; anything else outside a-z 0-9 '-' dropped ("My_Mod", "my mod", "MY-MOD" -> "my-mod") */
static void norm_modid(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 1 < n; p++) {
    unsigned char ch = *p; if (ch >= 'A' && ch <= 'Z') ch = (unsigned char)(ch - 'A' + 'a');
    if (ch == ' ' || ch == '_' || ch == '.') ch = '-';
    if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') out[o++] = (char)ch;
  }
  out[o] = 0;
}
/* a visible title for comparing: lower case, spaces ignored */
static void norm_title(const char *in, char *out, size_t n) {
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)(in ? in : ""); *p && o + 1 < n; p++) { if (*p == ' ' || *p == '\t') continue; out[o++] = (char)(*p >= 'A' && *p <= 'Z' ? *p - 'A' + 'a' : *p); }
  out[o] = 0;
}
/* version order: digit runs compare as numbers ("1.10.0" > "1.2.0"), everything else as characters; < 0, 0, > 0 */
static int ver_cmp(const char *a, const char *b) {
  while (*a || *b) {
    if (*a >= '0' && *a <= '9' && *b >= '0' && *b <= '9') { char *ea, *eb; long long x = strtoll(a, &ea, 10), y = strtoll(b, &eb, 10); if (x != y) return x < y ? -1 : 1; a = ea; b = eb; continue; }
    if (*a != *b) return (unsigned char)*a < (unsigned char)*b ? -1 : 1;
    a++; b++;
  }
  return 0;
}
/* the copy plan: which manifest files load. Two schema 3 files with the same (normalised) modid are the SAME mod when their titles match
 * (case and spaces ignored) and they share at least one row key (or neither has one): only the highest version loads, at the place of the
 * first copy; every other copy is logged once "ignored: older copy". Files of DIFFERENT mods with one modid all load (load_manifest_v3
 * stores the later ones as <modid>@<source file>). load[i] = the file to load at position i, -1 = nothing. */
typedef struct { int ok; char id[MS_ID]; char tn[96]; char ver[24]; char keys[1536]; } MsCand;
static void cand_read(const char *path, MsCand *k) {
  memset(k, 0, sizeof *k); size_t len; char *src = read_file(path, 256 * 1024, &len); if (!src) return;
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES); char *text = (char *)malloc(J_TEXT); JDoc d; long long sc = 0;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES, text, J_TEXT, src, len) : -1;
  if (root >= 0 && d.n[root].type == J_OBJ && j_isint(&d, j_get(&d, root, "schema"), &sc) && sc == 3 && j_str(&d, j_get(&d, root, "modid"))) {
    norm_modid(j_str(&d, j_get(&d, root, "modid")), k->id, sizeof k->id); norm_title(j_str(&d, j_get(&d, root, "title")), k->tn, sizeof k->tn);
    snprintf(k->ver, sizeof k->ver, "%s", j_str(&d, j_get(&d, root, "version")) ? j_str(&d, j_get(&d, root, "version")) : "");
    snprintf(k->keys, sizeof k->keys, ","); int rows = j_get(&d, root, "rows");
    if (rows >= 0 && d.n[rows].type == J_ARR) for (int r = d.n[rows].first; r >= 0; r = d.n[r].next) { const char *key = j_str(&d, j_get(&d, r, "key")); if (key && strlen(k->keys) + strlen(key) + 2 < sizeof k->keys) { strcat(k->keys, key); strcat(k->keys, ","); } }
    k->ok = k->id[0] != 0;
  }
  free(nodes); free(text); free(src);
}
static int keys_share(const char *a, const char *b) {
  if (!a[1] && !b[1]) return 1;
  for (const char *p = a + 1; *p; ) { const char *e = strchr(p, ','); if (!e) break; char t[40]; size_t n = (size_t)(e - p); if (n && n + 3 < sizeof t) { t[0] = ','; memcpy(t + 1, p, n); t[n + 1] = ','; t[n + 2] = 0; if (strstr(b, t)) return 1; } p = e + 1; }
  return 0;
}
static char g_plan_logged[4096];
static void cfg_plan(char (*names)[MAX_PATH], int nn, int *load) {
  MsCand *k = (MsCand *)calloc((size_t)(nn ? nn : 1), sizeof *k); int *grp = (int *)malloc(sizeof(int) * (size_t)(nn ? nn : 1)); char lg[4096] = "";
  for (int i = 0; i < nn; i++) { load[i] = i; grp[i] = -1; }
  if (!k || !grp) { free(k); free(grp); return; }
  for (int i = 0; i < nn; i++) { char p[MAX_PATH * 2]; snprintf(p, sizeof p, "%s\\" MS_MODSDIR "\\%s", g_dir, names[i]); cand_read(p, &k[i]); }
  for (int i = 0; i < nn; i++) {
    if (!k[i].ok || grp[i] >= 0) continue; grp[i] = i;
    for (int j = i + 1; j < nn; j++) if (k[j].ok && grp[j] < 0 && !strcmp(k[i].id, k[j].id) && !strcmp(k[i].tn, k[j].tn) && keys_share(k[i].keys, k[j].keys)) grp[j] = i;
  }
  for (int h = 0; h < nn; h++) {
    if (grp[h] != h) continue; int win = h, n = 0;
    for (int j = h; j < nn; j++) if (grp[j] == h) { n++; if (ver_cmp(k[j].ver, k[win].ver) > 0) win = j; }
    if (n < 2) continue;
    for (int j = h; j < nn; j++) if (grp[j] == h) {
      load[j] = -1;
      if (j != win) snprintf(lg + strlen(lg), sizeof lg - strlen(lg), "modsettings: %s: ignored: older copy of %s (version %s; %s has version %s)\n", names[j], k[j].id, k[j].ver, names[win], k[win].ver);
    }
    load[h] = win;
  }
  if (strcmp(lg, g_plan_logged)) { snprintf(g_plan_logged, sizeof g_plan_logged, "%s", lg); for (char *p = lg; *p; ) { char *e = strchr(p, '\n'); if (e) *e = 0; logf_("%s", p); if (!e) break; p = e + 1; } }
  free(k); free(grp);
}

/* ---- safety and behaviour rules (spec v3, 11-11) */
/* option v of row r as the player reads it: "As installed" (tuning rows' option 0), a list label, a slider value + unit */
static void opt_label(const MsCfg *c, const MsRow *r, int v, char *out, size_t n) {
  if (r->asis && v == 0) { snprintf(out, n, "As installed"); return; }
  int k = v - r->asis;
  if (r->type == RT_SLIDER) snprintf(out, n, "%g%s", slider_value(r, k), pool_str(c, r->s_unit)); else snprintf(out, n, "%s", k >= 0 && k < MS_MAXLIST ? pool_str(c, r->s_opt[k]) : "");
}
/* the Mod Manager's selected pack (rule 8): which .fbmod files are on. g_mm_config empty = %LOCALAPPDATA%\MMC\manager_config.json */
static char g_mm_config[MAX_PATH]; static char *g_pack; static int g_pack_ok;
static void pack_load(void) {
  free(g_pack); g_pack = NULL; g_pack_ok = 0; char p[MAX_PATH];
  if (g_mm_config[0]) snprintf(p, sizeof p, "%s", g_mm_config); else { const char *la = getenv("LOCALAPPDATA"); if (!la) return; snprintf(p, sizeof p, "%s\\MMC\\manager_config.json", la); }
  size_t len; char *src = read_file(p, 8 << 20, &len); if (!src) return;
  int cap = 1 << 17; size_t tcap = len + 1024; JNode *nodes = (JNode *)malloc(sizeof(JNode) * (size_t)cap); char *text = (char *)malloc(tcap); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, cap, text, tcap, src, len) : -1, games = root >= 0 ? j_get(&d, root, "Games") : -1, g = -1;
  if (games >= 0 && d.n[games].type == J_OBJ) for (int x = d.n[games].first; x >= 0; x = d.n[x].next) if (!_stricmp(d.n[x].key, "CollegeFB27") || (g < 0 && strstr(d.n[x].key, "College"))) g = x;
  const char *sel = g >= 0 ? j_str(&d, j_get(&d, j_get(&d, g, "Options"), "SelectedPack")) : NULL;
  const char *pk = sel ? j_str(&d, j_get(&d, j_get(&d, g, "Packs"), sel)) : NULL;
  if (pk) { g_pack = _strdup(pk); g_pack_ok = g_pack != NULL; }
  free(nodes); free(text); free(src);
}
/* 1 = that .fbmod is on in the selected pack ("name.fbmod:True|..."; a runner's "zzRunner - name.fbmod" counts); unknown pack = 1 */
static int pack_active(const char *fbmod) {
  if (!g_pack_ok || !fbmod || !*fbmod) return 1;
  size_t fl = strlen(fbmod);
  for (const char *p = g_pack; *p; ) {
    const char *e = strchr(p, '|'); size_t n = e ? (size_t)(e - p) : strlen(p); const char *colon = NULL;
    for (const char *q = p + n; q > p; q--) if (q[-1] == ':') { colon = q - 1; break; }
    if (colon) { size_t nl = (size_t)(colon - p); if (nl >= fl && !_strnicmp(colon - fl, fbmod, fl) && (nl == fl || colon[-(long)fl - 1] == ' ')) return !_strnicmp(colon + 1, "true", 4); }
    p = e ? e + 1 : p + n;
  }
  return 0;   /* not in the pack at all: not active */
}
static int cond_parse(MsCfg *c, const JDoc *d, int x, MsCond *k, const char *id, const char *key, const char *what) {
  memset(k, 0, sizeof *k); k->row = -1; if (x < 0) return 1;
  const char *ck = j_str(d, j_get(d, x, "key")), *cm = j_str(d, j_get(d, x, "mod")); int ve = j_get(d, x, "eq"), vn = j_get(d, x, "ne"), vi = j_get(d, x, "in");
  if (d->n[x].type != J_OBJ || !key_ok(ck) || (ve >= 0) + (vn >= 0) + (vi >= 0) != 1) { cfg_problem(c, "%s/%s: \"%s\" is { \"key\": \"<a row key>\", \"eq\" | \"ne\" | \"in\": ... } (and optionally \"mod\")", id, key, what); return 0; }
  snprintf(k->key, sizeof k->key, "%s", ck); if (cm) norm_modid(cm, k->mod, sizeof k->mod);
  k->op = ve >= 0 ? 0 : vn >= 0 ? 1 : 2; int vals = ve >= 0 ? ve : vn >= 0 ? vn : vi, first = vals, single = 1;
  if (k->op == 2) { if (d->n[vals].type != J_ARR || d->n[vals].count < 1 || d->n[vals].count > 8) { cfg_problem(c, "%s/%s: \"%s\" \"in\" is a list of 1-8 values", id, key, what); return 0; } first = d->n[vals].first; single = 0; }
  for (int v = first; v >= 0 && k->n < 8; v = single ? -1 : d->n[v].next) {
    const JNode *nd = &d->n[v]; int i = k->n;
    if (nd->type == J_TRUE || nd->type == J_FALSE) { k->kind[i] = 1; k->b[i] = nd->type == J_TRUE; }
    else if (nd->type == J_NUM) { k->kind[i] = 0; k->num[i] = nd->num; }
    else if (nd->type == J_STR) { k->kind[i] = 2; k->s[i] = pool_put(c, nd->str); }
    else { cfg_problem(c, "%s/%s: \"%s\" compares with true / false, a number or a text", id, key, what); return 0; }
    k->n++;
  }
  k->on = 1; return 1;
}
static int mods_related(const MsCfg *c, int a, int b) {   /* the same mod, or one joins the other's section, or both join one section */
  if (a == b) return 1;
  const char *sa = pool_str(c, c->mods[a].s_section), *sb = pool_str(c, c->mods[b].s_section);
  return !strcmp(sa, c->mods[b].id) || !strcmp(sb, c->mods[a].id) || (*sa && !strcmp(sa, sb));
}
static int mod_of_row(const MsCfg *c, int i) { for (int m = 0; m < c->nmods; m++) if (i >= c->mods[m].first && i < c->mods[m].first + c->mods[m].nrows) return m; return -1; }
static char g_resolve_logged[4096];
static void cfg_resolve(MsCfg *c) {
  char lg[4096] = ""; pack_load();
  for (int m = 0; m < c->nmods; m++) {
    MsMod *mo = &c->mods[m];
    mo->active = mo->s_fbmod < 0 || pack_active(pool_str(c, mo->s_fbmod));
    if (mo->npin && g_run_gamever) {   /* rule 6: a pinned mod on another known game version: its tuning rows are never written */
      int hit = 0; for (int k = 0; k < mo->npin; k++) if (mo->pin[k] == (long long)g_run_gamever) hit = 1;
      if (!hit) { int why = pool_put(c, "not available on this game version"); for (int i = mo->first; i < mo->first + mo->nrows; i++) if (c->rows[i].bind == BD_TUNING) { c->rows[i].unavail = 1; c->rows[i].s_unavail = why; }
        snprintf(lg + strlen(lg), sizeof lg - strlen(lg), "modsettings: %s is pinned to other game versions (running %u): its tuning rows are not available\n", mo->id, (unsigned)g_run_gamever); }
    }
  }
  /* rule 1: two mods on one field (asset + field + instance, overlapping copy): both rows say so; the last change wins */
  char (*al)[160] = (char (*)[160])calloc((size_t)(c->nrows ? c->nrows : 1), 160);
  for (int i = 0; al && i < c->nrows; i++) for (int j = i + 1; j < c->nrows; j++) {
    const MsRow *a = &c->rows[i], *b = &c->rows[j];
    if (a->bind != BD_TUNING || b->bind != BD_TUNING || !strcmp(a->mod, b->mod) || !(a->copy & b->copy)) continue;
    if (_stricmp(pool_str(c, a->s_asset), pool_str(c, b->s_asset)) || strcmp(pool_str(c, a->s_field), pool_str(c, b->s_field)) || strcmp(pool_str(c, a->s_inst), pool_str(c, b->s_inst))) continue;
    int ma = mod_of_row(c, i), mb = mod_of_row(c, j); if (ma < 0 || mb < 0) continue;
    snprintf(al[i] + strlen(al[i]), 160 - strlen(al[i]), "%s%s", al[i][0] ? ", " : "", pool_str(c, c->mods[mb].s_title));
    snprintf(al[j] + strlen(al[j]), 160 - strlen(al[j]), "%s%s", al[j][0] ? ", " : "", pool_str(c, c->mods[ma].s_title));
    snprintf(lg + strlen(lg), sizeof lg - strlen(lg), "modsettings: %s/%s and %s/%s both set %s : %s: the last change wins\n", a->mod, a->id, b->mod, b->id, pool_str(c, a->s_asset), pool_str(c, a->s_field));
  }
  for (int i = 0; al && i < c->nrows; i++) c->rows[i].s_also = al[i][0] ? pool_put(c, al[i]) : -1;
  free(al);
  /* rule 9: each condition's row (same mod by default; "mod" = a related mod) */
  for (int i = 0; i < c->nrows; i++) for (int q = 0; q < 2; q++) {
    MsCond *k = &c->rows[i].cond[q]; if (!k->on) continue; k->row = -1;
    int me = mod_of_row(c, i), t = me;
    if (k->mod[0]) { t = -1; for (int m = 0; m < c->nmods; m++) if (!strcmp(c->mods[m].id, k->mod) && mods_related(c, me, m)) t = m; }
    int r = t >= 0 ? row_index(c, c->mods[t].id, k->key) : -1;
    if (r >= 0 && (c->rows[r].type == RT_TOGGLE || c->rows[r].type == RT_LIST || c->rows[r].type == RT_SLIDER)) k->row = r;
    else snprintf(lg + strlen(lg), sizeof lg - strlen(lg), "modsettings: %s/%s: %s names %s%s%s, which is not a row of this mod or of a mod in its section: the condition is ignored\n", c->rows[i].mod, c->rows[i].id, q ? "enabledIf" : "visibleIf", k->mod[0] ? k->mod : "", k->mod[0] ? "/" : "", k->key);
  }
  if (strcmp(lg, g_resolve_logged)) { snprintf(g_resolve_logged, sizeof g_resolve_logged, "%s", lg); for (char *p = lg; *p; ) { char *e = strchr(p, '\n'); if (e) *e = 0; logf_("%s", p); if (!e) break; p = e + 1; } }
}

/* one schema 3 manifest (already parsed) into c */
static void load_manifest_v3(MsCfg *c, JDoc *dp, int root, const char *name) {
  JDoc *d = dp; int before = c->nproblems, pool0 = c->pool_used;
  const char *raw = j_str(d, j_get(d, root, "modid")); char base[MS_ID * 2], id[MS_MID]; norm_modid(raw, base, sizeof base);
  if (!raw || strlen(base) > 40 || !id_ok(base, 0)) { cfg_problem(c, "%s: \"modid\" must be 2-40 characters: lower case letters, digits, '-' (read as \"%s\")", name, base); return; }
  snprintf(id, sizeof id, "%s", base); int dup = 0;
  for (int m = 0; m < c->nmods; m++) if (!strcmp(c->mods[m].id, base)) dup = 1;
  if (dup) {   /* another mod already has this modid: keep both; this one's settings live under <modid>@<source file> */
    snprintf(id, sizeof id, "%s@%.*s", base, (int)(MS_MID - strlen(base) - 2), name);
    for (int m = 0; m < c->nmods; m++) if (!strcmp(c->mods[m].id, id)) { cfg_problem(c, "%s: %s is already loaded", name, id); return; }
    logf_("modsettings: %s: modid %s is already used by another mod: both are shown; this one's settings are stored as %s", name, base, id);
  }
  /* "section": another mod's modid or a shared @name (rows shown there; stored here). "closed": true = refuse other mods' rows */
  int sn = j_get(d, root, "section"), cn = j_get(d, root, "closed"), closed = 0; char sect[64] = "";
  if (sn >= 0) {
    const char *sv = j_str(d, sn); char t[64];
    if (!sv) { cfg_problem(c, "%s: \"section\" is another mod's modid or a shared \"@name\"", name); return; }
    norm_modid(sv[0] == '@' ? sv + 1 : sv, t, sizeof t);
    if (strlen(t) < 2 || strlen(t) > 40 || !id_ok(t, 0)) { cfg_problem(c, "%s: \"section\" (%s) is not a modid or a shared \"@name\"", name, sv); return; }
    if (sv[0] == '@') snprintf(sect, sizeof sect, "@%s", t); else if (strcmp(t, base)) snprintf(sect, sizeof sect, "%s", t);
  }
  if (cn >= 0 && !j_isbool(d, cn, &closed)) { cfg_problem(c, "%s: \"closed\" is true or false", name); return; }
  long long pins[8]; int npin = 0; int gvn = j_get(d, root, "gameVersion");   /* rule 6 */
  if (gvn >= 0) {
    if (d->n[gvn].type == J_ARR) { for (int x = d->n[gvn].first; x >= 0; x = d->n[x].next) { long long v; if (npin >= 8 || !j_isint(d, x, &v)) { npin = -1; break; } pins[npin++] = v; } }
    else if (j_isint(d, gvn, &pins[0])) npin = 1; else npin = -1;
    if (npin < 1) { cfg_problem(c, "%s: \"gameVersion\" is a game version number or a list of up to 8", name); return; }
  }
  int fbn = j_get(d, root, "fbmod"); const char *fbmod = j_str(d, fbn);   /* rule 8 */
  if (fbn >= 0 && (!fbmod || strlen(fbmod) < 7 || strlen(fbmod) > 200 || _stricmp(fbmod + strlen(fbmod) - 6, ".fbmod"))) { cfg_problem(c, "%s: \"fbmod\" is your .fbmod's file name, e.g. \"My Mod.fbmod\"", name); return; }
  char about[3200] = ""; { int an = j_get(d, root, "about"), crn = j_get(d, root, "credits"), ln = j_get(d, root, "links"); const char *ab = j_str(d, an), *cr = j_str(d, crn);   /* rule 19 */
    if (an >= 0 && (!ab || !*ab || u8len(ab) > 2000)) { cfg_problem(c, "%s: \"about\" needs 1-2000 characters", name); return; }
    if (crn >= 0 && (!cr || !*cr || u8len(cr) > 400)) { cfg_problem(c, "%s: \"credits\" needs 1-400 characters", name); return; }
    if (ab) snprintf(about, sizeof about, "%s", ab);
    if (cr) snprintf(about + strlen(about), sizeof about - strlen(about), "%sCredits: %s", about[0] ? "\n\n" : "", cr);
    if (ln >= 0) {
      if (d->n[ln].type != J_ARR || d->n[ln].count > 8) { cfg_problem(c, "%s: \"links\" is a list of up to 8 { \"title\", \"url\" }", name); return; }
      int k2 = 0; for (int x = d->n[ln].first; x >= 0; x = d->n[x].next, k2++) {
        const char *lt = j_str(d, j_get(d, x, "title")), *lu = j_str(d, j_get(d, x, "url"));
        if (!lt || !*lt || u8len(lt) > 40 || !lu || strlen(lu) > 200 || (_strnicmp(lu, "https://", 8) && _strnicmp(lu, "http://", 7))) { cfg_problem(c, "%s: links[%d] needs a \"title\" (1-40) and an http(s) \"url\"", name, k2); return; }
        snprintf(about + strlen(about), sizeof about - strlen(about), "%s%s: %s", k2 ? "\n" : (about[0] ? "\n\nLinks:\n" : "Links:\n"), lt, lu);
      }
    } }
  const char *title = j_str(d, j_get(d, root, "title")), *version = j_str(d, j_get(d, root, "version"));
  if (!title || !*title || u8len(title) > 40) { cfg_problem(c, "%s: \"title\" missing or longer than 40 characters", name); return; }
  if (!version || !*version || u8len(version) > 20) { cfg_problem(c, "%s: \"version\" missing or longer than 20 characters", name); return; }
  int rows = j_get(d, root, "rows");
  if (!j_at(d, rows) || d->n[rows].type != J_ARR || d->n[rows].count < 1 || d->n[rows].count > 120) { cfg_problem(c, "%s: \"rows\" must be an array of 1-120 rows", name); return; }
  if (c->nmods >= MS_MAXMODS || c->nrows + d->n[rows].count > MS_MAXROWS) { cfg_problem(c, "%s: too many mods or rows in total (%d mods, %d rows)", name, MS_MAXMODS, MS_MAXROWS); return; }
  int first = c->nrows, k = 0, w = 0;   /* k: the manifest's row; w: rows stored (a duplicate key is skipped) */
  for (int r = d->n[rows].first; r >= 0; r = d->n[r].next, k++, w++) {
    MsRow *o = &c->rows[first + w]; memset(o, 0, sizeof *o);
    snprintf(o->mod, sizeof o->mod, "%s", id); o->schema = 3; o->rec = -1; o->s_label = o->s_desc = o->s_unit = o->s_where = -1; for (int x = 0; x < MS_MAXLIST; x++) o->s_opt[x] = -1;
    o->applies = 1; o->s_unavail = o->s_asset = o->s_field = o->s_inst = o->s_also = -1; o->cond[0].row = o->cond[1].row = -1;
    if (d->n[r].type != J_OBJ) { cfg_problem(c, "%s: rows[%d] is not an object", id, k); continue; }
    const char *ty = j_str(d, j_get(d, r, "type")), *label = j_str(d, j_get(d, r, "label"));
    o->type = !ty ? 0 : !strcmp(ty, "header") ? RT_HEADER : !strcmp(ty, "info") ? RT_INFO : !strcmp(ty, "toggle") ? RT_TOGGLE : !strcmp(ty, "list") ? RT_LIST : !strcmp(ty, "slider") ? RT_SLIDER : 0;
    if (!o->type) { cfg_problem(c, "%s: rows[%d]: \"type\" must be toggle, list, slider, header or info", id, k); continue; }
    int lmax = o->type == RT_INFO ? 200 : 40;
    if (!label || !*label || u8len(label) > lmax) { cfg_problem(c, "%s: rows[%d]: \"label\" needs 1-%d characters", id, k, lmax); continue; }
    o->s_label = pool_put(c, label);
    int bind = j_get(d, r, "bind");
    if (o->type == RT_HEADER || o->type == RT_INFO) {
      if (bind >= 0) cfg_problem(c, "%s: rows[%d] (%s): a %s row has no \"bind\"", id, k, label, ty);
      snprintf(o->id, sizeof o->id, "#%d", k); continue;   /* never stored; not a key a player choice can name */
    }
    const char *key = j_str(d, j_get(d, r, "key"));
    if (!key_ok(key)) { cfg_problem(c, "%s: rows[%d] (%s): \"key\" needs 1-32 of a-z 0-9 - _", id, k, label); continue; }
    snprintf(o->id, sizeof o->id, "%s", key);
    { int twice = 0; for (int q = first; q < first + w; q++) if (!strcmp(c->rows[q].id, key)) twice = 1;
      if (twice) { logf_("modsettings: %s: rows[%d] (%s): the key \"%s\" appears twice in this mod: the first row wins, this one is not shown", id, k, label, key); w--; continue; } }
    int dn = j_get(d, r, "description"); const char *desc = j_str(d, dn);
    if (dn >= 0 && (!desc || !*desc || u8len(desc) > 400)) cfg_problem(c, "%s/%s: \"description\" needs 1-400 characters (or leave it out)", id, key);
    if (desc && *desc) o->s_desc = pool_put(c, desc);
    int dv = j_get(d, r, "default");
    if (o->type == RT_TOGGLE) {
      int b; if (!j_isbool(d, dv, &b)) { cfg_problem(c, "%s/%s: a toggle's \"default\" is true or false", id, key); continue; }
      o->nopt = 2; o->dflt = b; o->s_opt[0] = pool_put(c, "Off"); o->s_opt[1] = pool_put(c, "On");
    } else if (o->type == RT_LIST) {
      int opts = j_get(d, r, "options");
      if (!j_at(d, opts) || d->n[opts].type != J_ARR || d->n[opts].count < 2 || d->n[opts].count > MS_MAXLIST) { cfg_problem(c, "%s/%s: a list needs \"options\": 2-12 labels", id, key); continue; }
      o->nopt = d->n[opts].count; int oi = 0;
      for (int x = d->n[opts].first; x >= 0; x = d->n[x].next, oi++) { const char *s = j_str(d, x); if (!s || !*s || u8len(s) > 24) cfg_problem(c, "%s/%s: options[%d] needs 1-24 characters", id, key, oi); else o->s_opt[oi] = pool_put(c, s); }
      long long v; if (!j_isint(d, dv, &v) || v < 0 || v >= o->nopt) { cfg_problem(c, "%s/%s: a list's \"default\" is an option position, 0-%d", id, key, o->nopt - 1); continue; }
      o->dflt = (int)v;
    } else {
      double mn, mx, st, df;
      if (!j_isnum(d, j_get(d, r, "min"), &mn) || !j_isnum(d, j_get(d, r, "max"), &mx) || !j_isnum(d, j_get(d, r, "step"), &st) || !j_isnum(d, dv, &df)) { cfg_problem(c, "%s/%s: a slider needs numbers for \"min\", \"max\", \"step\" and \"default\"", id, key); continue; }
      if (!(st > 0) || !(mx > mn)) { cfg_problem(c, "%s/%s: a slider needs \"max\" above \"min\" and \"step\" above 0", id, key); continue; }
      int n = slider_stops(mn, mx, st); if (n > MS_MAXSTOPS) { cfg_problem(c, "%s/%s: %d stops; the most is %d (use a bigger \"step\")", id, key, n, MS_MAXSTOPS); continue; }
      if (df < mn || df > mx) { cfg_problem(c, "%s/%s: \"default\" must lie between \"min\" and \"max\"", id, key); continue; }
      o->smin = mn; o->smax = mx; o->sstep = st; o->nopt = n; o->dflt = slider_stop_of(o, df);
      int un = j_get(d, r, "unit"); const char *unit = j_str(d, un);
      if (un >= 0 && (!unit || !*unit || u8len(unit) > 8)) cfg_problem(c, "%s/%s: \"unit\" needs 1-8 characters", id, key);
      if (unit && *unit) o->s_unit = pool_put(c, unit);
    }
    const char *bt = j_str(d, j_get(d, bind, "type"));
    if (bind < 0 || d->n[bind].type != J_OBJ || !bt) { cfg_problem(c, "%s/%s: \"bind\" is missing: { \"type\": \"api\" | \"tuning\" | \"dynasty\", ... }", id, key); continue; }
    if (!strcmp(bt, "api")) { o->bind = BD_API; o->store = ST_GLOBAL; }
    else if (!strcmp(bt, "tuning")) {
      o->bind = BD_TUNING; o->store = ST_GLOBAL;
      const char *asset = j_str(d, j_get(d, bind, "asset")), *field = j_str(d, j_get(d, bind, "field")), *copy = j_str(d, j_get(d, bind, "copy"));
      if (!asset || !*asset || !field || !*field) { cfg_problem(c, "%s/%s: a tuning bind needs \"asset\" and \"field\"", id, key); continue; }
      if (!copy || (strcmp(copy, "simulation") && strcmp(copy, "base") && strcmp(copy, "both"))) { cfg_problem(c, "%s/%s: bind \"copy\" is required and must be \"simulation\", \"base\" or \"both\"", id, key); continue; }
      if (o->type == RT_SLIDER) { int sc = j_get(d, bind, "scale"); if (!j_isnum(d, j_get(d, sc, "at-min"), NULL) || !j_isnum(d, j_get(d, sc, "at-max"), NULL)) { cfg_problem(c, "%s/%s: a slider's tuning bind needs \"scale\": { \"at-min\", \"at-max\" }", id, key); continue; } }
      else { int vs = j_get(d, bind, "values"); if (!j_at(d, vs) || d->n[vs].type != J_ARR || d->n[vs].count != o->nopt) { cfg_problem(c, "%s/%s: bind \"values\" needs one value per option (%d)", id, key, o->nopt); continue; }
        int t0 = -1, mixed = 0; for (int x = d->n[vs].first; x >= 0; x = d->n[x].next) { int ty = d->n[x].type == J_FALSE ? J_TRUE : d->n[x].type; if (t0 < 0) t0 = ty; else if (ty != t0) mixed = 1; }
        if (mixed) { o->unavail = 1; o->s_unavail = pool_put(c, "its values are of different types"); logf_("modsettings: %s/%s: bind \"values\" mix types: the row is greyed and never written", id, key); } }   /* rule 7 */
      o->copy = !strcmp(copy, "simulation") ? 1 : !strcmp(copy, "base") ? 2 : 3;
      o->s_asset = pool_put(c, asset); o->s_field = pool_put(c, field); { const char *in = j_str(d, j_get(d, bind, "instance")); o->s_inst = in ? pool_put(c, in) : -1; }
      char w[300]; snprintf(w, sizeof w, "%s : %s (%s)", asset, field, copy); o->s_where = pool_put(c, w);
    } else if (!strcmp(bt, "dynasty")) {
      o->bind = BD_DYNASTY; o->store = ST_BAND;
      const char *slot = j_str(d, j_get(d, bind, "slot"));
      if (!id_ok(slot, 0)) { cfg_problem(c, "%s/%s: a dynasty bind needs \"slot\" (your line in the shared slot list)", id, key); continue; }
      if (o->nopt > MS_DYN_MAXOPT) { cfg_problem(c, "%s/%s: a dynasty row holds at most 8 values; this row has %d", id, key, o->nopt); continue; }
      o->s_where = pool_put(c, slot);
      const MsSlot *s = slot_find(slot);
      if (s && s->rec >= 0 && s->rec < MS_BAND_RECORDS && s->bit >= 0 && s->width >= 1 && s->width <= 3 && s->bit + s->width <= MS_BAND_BITS && (1 << s->width) >= o->nopt) {
        o->rec = s->rec; o->bit = s->bit; o->width = s->width;
        for (int q = 0; q < first + w; q++) { const MsRow *p = &c->rows[q]; if (p->store == ST_BAND && p->rec == o->rec && p->bit < o->bit + o->width && o->bit < p->bit + p->width) cfg_problem(c, "%s/%s: band record %d bits %d-%d overlap %s/%s", id, key, o->rec, o->bit, o->bit + o->width - 1, p->mod, p->id); }
      } else logf_("modsettings: %s/%s: slot \"%s\" is not in " MS_SLOTS " (or does not hold %d values): the choice is kept in " MS_SETTINGS " but not written into the dynasty", id, key, slot, o->nopt);
    } else { cfg_problem(c, "%s/%s: bind \"type\" must be \"api\", \"tuning\" or \"dynasty\"", id, key); continue; }
    { int an = j_get(d, r, "applies"); const char *ap = j_str(d, an);   /* rule 3 */
      if (an >= 0) { if (ap && !strcmp(ap, "now")) o->applies = 0; else if (ap && !strcmp(ap, "next-game")) o->applies = 1; else if (ap && !strcmp(ap, "restart")) o->applies = 2; else { cfg_problem(c, "%s/%s: \"applies\" is \"now\", \"next-game\" or \"restart\"", id, key); continue; } } }
    if (!cond_parse(c, d, j_get(d, r, "visibleIf"), &o->cond[0], id, key, "visibleIf") || !cond_parse(c, d, j_get(d, r, "enabledIf"), &o->cond[1], id, key, "enabledIf")) continue;   /* rule 9 */
    if (o->bind == BD_TUNING) { o->asis = 1; o->nopt += 1; o->dflt = 0; }   /* rule 2: option 0 "As installed" = the default, writes nothing */
  }
  if (c->nproblems == before) {
    MsMod *m = &c->mods[c->nmods++]; snprintf(m->id, sizeof m->id, "%s", id); m->first = first; m->nrows = w; c->nrows += w;
    m->schema = 3; m->s_title = pool_put(c, title); m->s_version = pool_put(c, version); m->s_source = pool_put(c, name); m->dup = dup;
    m->s_section = sect[0] ? pool_put(c, sect) : -1; m->closed = closed;
    m->s_fbmod = fbmod ? pool_put(c, fbmod) : -1; m->active = 1; m->s_about = about[0] ? pool_put(c, about) : -1; m->npin = npin; for (int k2 = 0; k2 < npin; k2++) m->pin[k2] = pins[k2];
    snprintf(c->loaded + strlen(c->loaded), sizeof c->loaded - strlen(c->loaded), "%s%s", c->loaded[0] ? "," : "", id);
  } else c->pool_used = pool0;
}

/* a settings.json value for row r -> its option index (-1 = not a value this row takes) */
static int settings_index(const MsRow *r, const JDoc *d, int x) {
  long long v; int b; double f; const char *sv = j_str(d, x);
  if (r->asis && sv && !strcmp(sv, "as-installed")) return 0;
  if (r->type == RT_SLIDER) { if (!j_isnum(d, x, &f) || f < r->smin - 1e-9 || f > r->smax + 1e-9) return -1; return slider_stop_of(r, f) + r->asis; }
  if (r->type == RT_TOGGLE && j_isbool(d, x, &b)) return b + r->asis;
  if (j_isint(d, x, &v) && v >= 0 && v < r->nopt - r->asis) return (int)v + r->asis;
  return -1;
}
static long long g_seq_seen;
static void load_settings(MsCfg *c) {
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_SETTINGS, g_dir);
  size_t len; char *src = read_file(p, 1024 * 1024, &len); if (!src) return;
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES * 4); char *text = (char *)malloc(J_TEXT * 4); JDoc d;
  int root = nodes && text ? j_parse(&d, nodes, J_NODES * 4, text, J_TEXT * 4, src, len) : -1; long long schema = 0;
  if (root < 0 || d.n[root].type != J_OBJ) { cfg_problem(c, MS_SETTINGS ": not valid JSON (%s): its choices are not used", root < 0 ? d.err : "not an object"); goto done; }
  if (!j_isint(&d, j_get(&d, root, "schema"), &schema) || schema != 3) { cfg_problem(c, MS_SETTINGS ": schema must be 3: its choices are not used"); goto done; }
  for (int pass = 0; pass < 2; pass++) {
    int top = j_get(&d, root, pass ? "dynasty" : "global"); if (top < 0 || d.n[top].type != J_OBJ) continue;
    for (int m = d.n[top].first; m >= 0; m = d.n[m].next) {
      if (d.n[m].type != J_OBJ) continue;
      for (int x = d.n[m].first; x >= 0; x = d.n[x].next) {
        int i = row_index(c, d.n[m].key, d.n[x].key); if (i < 0) continue;   /* a mod or row not installed: kept in the file, ignored here */
        const MsRow *r = &c->rows[i];
        if (!pass) {
          if (r->store != ST_GLOBAL) continue;
          int v = settings_index(r, &d, x); if (v < 0) { cfg_problem(c, MS_SETTINGS ": global %s/%s is not a value of this row (its default is used)", r->mod, r->id); continue; }
          c->gval[i] = v; c->gset[i] = 1;
        } else {
          if (r->store != ST_BAND) continue; long long seq = 0;
          int v = settings_index(r, &d, j_get(&d, x, "value"));
          if (v < 0 || !j_isint(&d, j_get(&d, x, "seq"), &seq) || seq < 1) { cfg_problem(c, MS_SETTINGS ": dynasty %s/%s needs { \"value\": <a value of the row>, \"seq\": 1 or more }", r->mod, r->id); continue; }
          if (seq > c->want_seq[i]) { c->want[i] = v; c->want_seq[i] = seq; }
          if (seq > g_seq_seen) g_seq_seen = seq;
        }
      }
    }
  }
done:
  free(nodes); free(text); free(src);
}
#endif
