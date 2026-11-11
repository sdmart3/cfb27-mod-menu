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
static int slider_stop_of(const MsRow *r, double v) { long long k = (long long)((v - r->smin) / r->sstep + (v >= r->smin ? 0.5 : -0.5)); if (k < 0) k = 0; if (k >= r->nopt) k = r->nopt - 1; return (int)k; }
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

/* one schema 3 manifest (already parsed) into c */
static void load_manifest_v3(MsCfg *c, JDoc *dp, int root, const char *name) {
  JDoc *d = dp; int before = c->nproblems, pool0 = c->pool_used;
  const char *id = j_str(d, j_get(d, root, "modid"));
  if (!id_ok(id, 0)) { cfg_problem(c, "%s: \"modid\" must be 2-40 characters: lower case letters, digits, '-'", name); return; }
  { char want[64]; snprintf(want, sizeof want, "%s.json", id); if (_stricmp(want, name)) { cfg_problem(c, "%s: the file name must be <modid>.json (%s)", name, want); return; } }
  for (int m = 0; m < c->nmods; m++) if (!strcmp(c->mods[m].id, id)) { cfg_problem(c, "%s: a second manifest for %s", name, id); return; }
  const char *title = j_str(d, j_get(d, root, "title")), *version = j_str(d, j_get(d, root, "version"));
  if (!title || !*title || u8len(title) > 40) { cfg_problem(c, "%s: \"title\" missing or longer than 40 characters", name); return; }
  if (!version || !*version || u8len(version) > 20) { cfg_problem(c, "%s: \"version\" missing or longer than 20 characters", name); return; }
  int rows = j_get(d, root, "rows");
  if (!j_at(d, rows) || d->n[rows].type != J_ARR || d->n[rows].count < 1 || d->n[rows].count > 120) { cfg_problem(c, "%s: \"rows\" must be an array of 1-120 rows", name); return; }
  if (c->nmods >= MS_MAXMODS || c->nrows + d->n[rows].count > MS_MAXROWS) { cfg_problem(c, "%s: too many mods or rows in total (%d mods, %d rows)", name, MS_MAXMODS, MS_MAXROWS); return; }
  int first = c->nrows, k = 0;
  for (int r = d->n[rows].first; r >= 0; r = d->n[r].next, k++) {
    MsRow *o = &c->rows[first + k]; memset(o, 0, sizeof *o);
    snprintf(o->mod, sizeof o->mod, "%s", id); o->schema = 3; o->rec = -1; o->s_label = o->s_desc = o->s_unit = o->s_where = -1; for (int x = 0; x < MS_MAXLIST; x++) o->s_opt[x] = -1;
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
    for (int q = first; q < first + k; q++) if (!strcmp(c->rows[q].id, key)) cfg_problem(c, "%s/%s: the key appears twice", id, key);
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
      else { int vs = j_get(d, bind, "values"); if (!j_at(d, vs) || d->n[vs].type != J_ARR || d->n[vs].count != o->nopt) { cfg_problem(c, "%s/%s: bind \"values\" needs one value per option (%d)", id, key, o->nopt); continue; } }
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
        for (int q = 0; q < first + k; q++) { const MsRow *p = &c->rows[q]; if (p->store == ST_BAND && p->rec == o->rec && p->bit < o->bit + o->width && o->bit < p->bit + p->width) cfg_problem(c, "%s/%s: band record %d bits %d-%d overlap %s/%s", id, key, o->rec, o->bit, o->bit + o->width - 1, p->mod, p->id); }
      } else logf_("modsettings: %s/%s: slot \"%s\" is not in " MS_SLOTS " (or does not hold %d values): the choice is kept in " MS_SETTINGS " but not written into the dynasty", id, key, slot, o->nopt);
    } else { cfg_problem(c, "%s/%s: bind \"type\" must be \"api\", \"tuning\" or \"dynasty\"", id, key); continue; }
  }
  if (c->nproblems == before) {
    MsMod *m = &c->mods[c->nmods++]; snprintf(m->id, sizeof m->id, "%s", id); m->first = first; m->nrows = k; c->nrows += k;
    m->schema = 3; m->s_title = pool_put(c, title); m->s_version = pool_put(c, version);
    snprintf(c->loaded + strlen(c->loaded), sizeof c->loaded - strlen(c->loaded), "%s%s", c->loaded[0] ? "," : "", id);
  } else c->pool_used = pool0;
}

/* a settings.json value for row r -> its option index (-1 = not a value this row takes) */
static int settings_index(const MsRow *r, const JDoc *d, int x) {
  long long v; int b; double f;
  if (r->type == RT_SLIDER) { if (!j_isnum(d, x, &f) || f < r->smin - 1e-9 || f > r->smax + 1e-9) return -1; return slider_stop_of(r, f); }
  if (r->type == RT_TOGGLE && j_isbool(d, x, &b)) return b;
  if (j_isint(d, x, &v) && v >= 0 && v < r->nopt) return (int)v;
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
