/* mstab.h: Layer 2 "tab feed" of modsettings.dll (2026-11-11; docs/design.md section 12). Included by
 * modsettings.c after the status writer.
 *
 * THE PROTOCOL. Our .fbmod's "Mod Settings" tab (GenericSettings copy, modsettings/src/ui-build-ms.js buildFeedPage) keeps its own small
 * ActionScript adapter object for the screen's life cycle (init / shutdown notify: PROVEN in game by the Step 1 probes), and asks the
 * game's NATIVE settings adapter for its data with a private category id, MS_TAB_CAT (19795 = 0x4D53; the stock categories are
 * 0-11, 15-17, 20, 21: PROVEN, the static table of CollegeGameSpecificSettings on 10-29):
 *   GetListSettings(19795)               -> slot 0x30 (self, arrayHolder, Variant *category)
 *   GetToggleDetails(19795, key)         -> slot 0x38 (self, arrayHolder, Variant *category, Variant *key)
 *   GetSliderDetails(19795, key)         -> slot 0x40 (self, objectHolder, Variant *category, Variant *key)
 *   GameAdjustToggleSetting(19795, k, v, why) -> slot 0x50 (self, arrayHolder, Variant *category, Variant *key, Variant *value, const char *why)
 *   GameAdjustSliderSetting(19795, k, v, why) -> slot 0x58 (self, arrayHolder, Variant *category, Variant *key, int value)
 * (glue -> marshaller -> slot: PROVEN statically on both dumps, tools/gen-pins.js tabPins). This DLL swaps those five vtable slots for
 * wrappers that answer ONLY when the category is 19795 and the holder is the game's static holder; every other call goes to the original
 * slot, untouched. Without this DLL the native managers get category 19795, find it in neither table and write nothing (PROVEN statically:
 * the list builder 0x147f34030 loops its 17 entries and returns; toggle details look the key up and return), so the tab shows its own
 * "plugin not running" row (the AS side).
 *
 * THE ROWS. One header per installed mod (sorted by title), then that mod's rows: a manifest header -> a header row; info -> a greyed
 * one-option row; toggle / list -> a toggle row with its options; a slider -> a toggle row of its stops (up to 21 stops; more, or
 * slider_native = 1: the game's slider row, in stop units). Keys: 0x4D00 + the row's index. The fields are the ones Step 1's AS rows used
 * (PROVEN to draw: key, type, display, description, isEnabled; details: display, key, isDefault) with the native type numbers
 * (0 header, 1 toggle, 2 slider: the native table).
 *
 * A CHANGE. The wrapper sets the value in memory at once (the tab and ms_get see it), and queues it in a journal; the worker thread writes
 * settings.json (atomically, keeping other mods' entries) within a moment and then re-reads it like any other input. A dynasty row's
 * change gets the next seq and is written into the loaded dynasty's band by layer 1's capture pass (never on the UI thread).
 *
 * SAFETY. The whole adapter vtable (31 slots) must be stock, the adapter global must point at the pinned object, both holders and the
 * Variant vtable must be the pinned ones, or nothing is swapped (the band writer is independent). Each wrapper checks self, the holder and
 * the Variant before reading anything; our own work runs under the fault guard (a fault turns the tab feed off, the game keeps going).
 * Nothing of the game is held across calls; no lock is held while a game setter runs (rows are copied out first). */
#ifndef MS_TAB_H
#define MS_TAB_H
#define MS_TAB_CAT 19795
#define MS_TAB_KEY0 0x4D00
#define MS_TAB_KEY_INFO 0x4CF0       /* + 1 "no mods", + 2 "skipped manifests", + 3..14 notes */
#define MS_TAB_KEY_ABOUT 0x4C00      /* + mod index: a mod's "About" row (rule 19) */
#define MS_TAB_STOPS_LIST 21         /* a slider with more stops is drawn as the game's slider row */
enum { TS_LIST, TS_TOGGLE, TS_SLIDER, TS_ADJT, TS_ADJS, TS_N };
static const int TAB_SLOT[TS_N] = { 0x30, 0x38, 0x40, 0x50, 0x58 };
static const char *const TAB_NAME[TS_N] = { "GetListSettings", "GetToggleDetails", "GetSliderDetails", "GameAdjustToggleSetting", "GameAdjustSliderSetting" };
static volatile LONG g_tab_off;                 /* a fault in a tab wrapper: answers go back to the game (the tab shows its fallback row) */
static int g_tab_done; static char g_tab_state[24] = "not installed", g_tab_reason[300];
static void *g_tab_orig[TS_N]; static volatile LONG g_tab_calls[TS_N], g_tab_ours[TS_N], g_tab_changes;
static char g_tab_last[240];
static int ms_guarded2(void (*fn)(void *), void *arg, const char *what, volatile LONG *off);

/* ------------------------------------------------------------------ values: what the tab shows, and a change */
/* the row's current option index: global = its value; dynasty = a change still waiting, else the loaded dynasty's bits, else the last wish / default */
static int tab_value(const MsCfg *c, int i) {
  const MsRow *r = &c->rows[i];
  if (r->store != ST_BAND) return c->gval[i];
  if (r->rec < 0) return c->want_seq[i] > 0 ? c->want[i] : r->dflt;
  if (c->want_seq[i] > 0) { EnterCriticalSection(&g_applied_cs); long long a = applied_seq(r->mod, r->id); LeaveCriticalSection(&g_applied_cs); if (c->want_seq[i] > a) return c->want[i]; }
  int v = -1; EnterCriticalSection(&g_dyn_cs);
  if (g_dyn.seen && g_dyn.rec_count[r->rec] == 1) { v = ((g_dyn.rec_val[r->rec] - band_lo(r->rec)) >> r->bit) & ((1 << r->width) - 1); if (v >= r->nopt) v = r->dflt; }
  LeaveCriticalSection(&g_dyn_cs);
  return v >= 0 ? v : c->want_seq[i] > 0 ? c->want[i] : r->dflt;
}
typedef struct { char mod[MS_MID], id[MS_ID]; int v, dyn; long long seq; } MsJ;
#define MS_JMAX 128
static MsJ g_jq[MS_JMAX]; static int g_njq; static CRITICAL_SECTION g_jq_cs; static HANDLE g_wake;
static void journal_apply(MsCfg *c, const MsJ *j, int n) {
  for (int k = 0; k < n; k++) {
    int i = row_index(c, j[k].mod, j[k].id); if (i < 0 || j[k].v < 0 || j[k].v >= c->rows[i].nopt) continue;
    if (j[k].dyn) { if (c->rows[i].store == ST_BAND && j[k].seq > c->want_seq[i]) { c->want[i] = j[k].v; c->want_seq[i] = j[k].seq; } }
    else if (c->rows[i].store == ST_GLOBAL) { c->gval[i] = j[k].v; c->gset[i] = 1; }
  }
}
static int tab_choice(const MsRow *r) { return r->type == RT_TOGGLE || r->type == RT_LIST || r->type == RT_SLIDER; }
/* rule 9: a condition against its row's current value ("As installed" equals nothing); an unresolved condition is true */
static int cond_ok(const MsCfg *c, const MsCond *k) {
  if (!k->on || k->row < 0) return 1;
  const MsRow *t = &c->rows[k->row]; int idx = tab_value(c, k->row), real = idx - t->asis, any = 0; char lbl[100];
  for (int q = 0; q < k->n; q++) {
    int m = 0;
    if (t->asis && idx == 0) m = 0;
    else if (k->kind[q] == 1) m = t->type == RT_TOGGLE && (real == 1) == (k->b[q] != 0);
    else if (k->kind[q] == 0) { double dv = slider_value(t, real) - k->num[q], av = k->num[q] < 0 ? -k->num[q] : k->num[q]; m = t->type == RT_SLIDER ? (dv < 0 ? -dv : dv) < 1e-9 * (1 + av) : (double)real == k->num[q]; }
    else { opt_label(c, t, idx, lbl, sizeof lbl); m = !_stricmp(lbl, pool_str(c, k->s[q])); }
    if (m) any = 1;
  }
  return k->op == 1 ? !any : any;
}
static int row_visible(const MsCfg *c, int i) { return cond_ok(c, &c->rows[i].cond[0]); }
/* a row the player may change: its mod is active, it is available on this game version, its enabledIf holds */
static int row_live(const MsCfg *c, int i) { int m = mod_of_row(c, i); return m >= 0 && c->mods[m].active && !c->rows[i].unavail && cond_ok(c, &c->rows[i].cond[1]); }
/* a change from the tab (UI thread): in memory now, settings.json from the worker. 1 = taken */
static int tab_set(int i, int v, const char *how) {
  MsJ j; memset(&j, 0, sizeof j); int ok = 0; char lbl[48] = "";
  AcquireSRWLockExclusive(&g_cfg_lock);
  MsCfg *c = g_cfg;
  if (c && i >= 0 && i < c->nrows && tab_choice(&c->rows[i]) && v >= 0 && v < c->rows[i].nopt && row_live(c, i) && row_visible(c, i)) {
    MsRow *r = &c->rows[i]; snprintf(j.mod, sizeof j.mod, "%s", r->mod); snprintf(j.id, sizeof j.id, "%s", r->id); j.v = v;
    if (r->store == ST_BAND) { j.dyn = 1; j.seq = InterlockedIncrement64((volatile LONG64 *)&g_seq_seen); c->want[i] = v; c->want_seq[i] = j.seq; }
    else { c->gval[i] = v; c->gset[i] = 1; }
    opt_label(c, r, v, lbl, sizeof lbl);
    ok = 1;
  }
  ReleaseSRWLockExclusive(&g_cfg_lock);
  if (!ok) return 0;
  EnterCriticalSection(&g_jq_cs);
  int k = 0; for (; k < g_njq; k++) if (!strcmp(g_jq[k].mod, j.mod) && !strcmp(g_jq[k].id, j.id)) break;   /* the same row again: one entry, the newest */
  if (k < g_njq) g_jq[k] = j; else if (g_njq < MS_JMAX) g_jq[g_njq++] = j; else { memmove(g_jq, g_jq + 1, sizeof g_jq[0] * (MS_JMAX - 1)); g_jq[MS_JMAX - 1] = j; }
  LeaveCriticalSection(&g_jq_cs);
  InterlockedIncrement(&g_tab_changes);
  snprintf(g_tab_last, sizeof g_tab_last, "%s/%s = %d (%s)%s by %s", j.mod, j.id, v, lbl, j.dyn ? " [dynasty, waits for the next capture]" : "", how);
  logf_("modsettings: tab change on thread %lu: %s", GetCurrentThreadId(), g_tab_last);
  InterlockedExchange(&g_status_dirty, 1);
  if (g_wake) SetEvent(g_wake);
  return 1;
}

/* ------------------------------------------------------------------ settings.json writer (worker thread) */
static void j_emit(Sb *s, const JDoc *d, int x) {
  const JNode *n = j_at(d, x); if (!n) { sb_add(s, "null"); return; }
  switch (n->type) {
    case J_NULL: sb_add(s, "null"); break; case J_TRUE: sb_add(s, "true"); break; case J_FALSE: sb_add(s, "false"); break;
    case J_NUM: if (n->isint) sb_add(s, "%lld", n->inum); else sb_add(s, "%.17g", n->num); break;
    case J_STR: sb_q(s, n->str); break;
    case J_ARR: sb_add(s, "["); for (int c = n->first, k = 0; c >= 0; c = d->n[c].next, k++) { if (k) sb_add(s, ","); j_emit(s, d, c); } sb_add(s, "]"); break;
    default: sb_add(s, "{"); for (int c = n->first, k = 0; c >= 0; c = d->n[c].next, k++) { if (k) sb_add(s, ","); sb_q(s, d->n[c].key); sb_add(s, ":"); j_emit(s, d, c); } sb_add(s, "}"); break;
  }
}
static void sb_value(Sb *s, const MsRow *r, int v) {
  if (r->asis && v == 0) { sb_add(s, "\"as-installed\""); return; }
  int k = v - r->asis;
  if (r->schema == 3 && r->type == RT_TOGGLE) sb_add(s, k ? "true" : "false");
  else if (r->type == RT_SLIDER) sb_add(s, "%.10g", slider_value(r, k));
  else sb_add(s, "%d", k);
}
static long g_settings_writes;
static int settings_write(void) {
  char p[MAX_PATH]; snprintf(p, sizeof p, "%s\\" MS_SETTINGS, g_dir);
  size_t len = 0; char *src = read_file(p, 1024 * 1024, &len);
  JNode *nodes = (JNode *)malloc(sizeof(JNode) * J_NODES * 4); char *text = (char *)malloc(J_TEXT * 4); JDoc d; memset(&d, 0, sizeof d); int root = -1;
  if (src && nodes && text) {
    long long sc = 0; root = j_parse(&d, nodes, J_NODES * 4, text, J_TEXT * 4, src, len);
    if (root < 0 || d.n[root].type != J_OBJ || !j_isint(&d, j_get(&d, root, "schema"), &sc) || sc != 3) {
      char bad[MAX_PATH + 16]; snprintf(bad, sizeof bad, "%s.unreadable", p); CopyFileA(p, bad, FALSE);
      logf_("modsettings: " MS_SETTINGS " was not a readable schema 3 file: kept as " MS_SETTINGS ".unreadable, a new one is written"); root = -1;
    }
  }
  Sb s = { 0 };
  sb_add(&s, "{\n  \"schema\": 3,\n  \"note\": \"Mod Settings choices: written by modsettings.dll when you change a row in the game's Mod Settings tab. One block per mod id; a dynasty row also has the seq of the change.\",\n");
  AcquireSRWLockShared(&g_cfg_lock);
  const MsCfg *c = g_cfg;
  for (int pass = 0; pass < 2; pass++) {
    sb_add(&s, "  \"%s\": {", pass ? "dynasty" : "global"); int nm = 0;
    int top = root >= 0 ? j_get(&d, root, pass ? "dynasty" : "global") : -1; if (top >= 0 && d.n[top].type != J_OBJ) top = -1;
    for (int m = 0; c && m < c->nmods; m++) {
      const MsMod *mo = &c->mods[m]; int old = top >= 0 ? j_get(&d, top, mo->id) : -1; if (old >= 0 && d.n[old].type != J_OBJ) old = -1;
      int ne = 0; size_t mark = s.n;
      sb_add(&s, "%s\n    ", nm ? "," : ""); sb_q(&s, mo->id); sb_add(&s, ": {");
      for (int i = mo->first; i < mo->first + mo->nrows; i++) {
        const MsRow *r = &c->rows[i]; if (!tab_choice(r)) continue;
        if (!pass && r->store == ST_GLOBAL && c->gset[i]) { sb_add(&s, "%s ", ne++ ? "," : ""); sb_q(&s, r->id); sb_add(&s, ": "); sb_value(&s, r, c->gval[i]); }
        if (pass && r->store == ST_BAND && c->want_seq[i] > 0) { sb_add(&s, "%s ", ne++ ? "," : ""); sb_q(&s, r->id); sb_add(&s, ": { \"value\": "); sb_value(&s, r, c->want[i]); sb_add(&s, ", \"seq\": %lld }", c->want_seq[i]); }
      }
      if (old >= 0) for (int x = d.n[old].first; x >= 0; x = d.n[x].next) if (row_index(c, mo->id, d.n[x].key) < 0) { sb_add(&s, "%s ", ne++ ? "," : ""); sb_q(&s, d.n[x].key); sb_add(&s, ": "); j_emit(&s, &d, x); }   /* a row this version no longer has: kept */
      if (ne) { sb_add(&s, " }"); nm++; } else { s.n = mark; if (s.b) s.b[s.n] = 0; }
    }
    if (top >= 0) for (int x = d.n[top].first; x >= 0; x = d.n[x].next) {   /* mods not installed: kept as they were */
      int known = 0; for (int m = 0; c && m < c->nmods; m++) if (!strcmp(c->mods[m].id, d.n[x].key)) known = 1;
      if (!known) { sb_add(&s, "%s\n    ", nm++ ? "," : ""); sb_q(&s, d.n[x].key); sb_add(&s, ": "); j_emit(&s, &d, x); }
    }
    sb_add(&s, "\n  }%s\n", pass ? "" : ",");
  }
  ReleaseSRWLockShared(&g_cfg_lock);
  sb_add(&s, "}\n");
  int ok = s.b && write_atomic(p, s.b, s.n);
  if (ok) g_settings_writes++;
  logf_("modsettings: " MS_SETTINGS " %s (%u bytes)", ok ? "written" : "could NOT be written (retried at the next change)", (unsigned)s.n);
  free(s.b); free(nodes); free(text); free(src);
  return ok;
}
/* worker: the journal into the configuration, then into settings.json; entries leave the journal only once written */
static void journal_flush(void) {
  static MsJ copy[MS_JMAX]; int n;
  EnterCriticalSection(&g_jq_cs); n = g_njq; memcpy(copy, g_jq, sizeof copy[0] * (size_t)n); LeaveCriticalSection(&g_jq_cs);
  if (!n) return;
  AcquireSRWLockExclusive(&g_cfg_lock); if (g_cfg) journal_apply(g_cfg, copy, n); ReleaseSRWLockExclusive(&g_cfg_lock);
  if (!settings_write()) return;
  EnterCriticalSection(&g_jq_cs);
  int keep = 0; for (int k = 0; k < g_njq; k++) { int same = 0; for (int q = 0; q < n; q++) if (!memcmp(&g_jq[k], &copy[q], sizeof copy[q])) same = 1; if (!same) g_jq[keep++] = g_jq[k]; }   /* a newer change of a row stays */
  g_njq = keep; LeaveCriticalSection(&g_jq_cs);
}

/* ------------------------------------------------------------------ the row model (copied out under the lock; no lock while the game's setters run) */
typedef struct { int type, key, enabled, disp, desc; } TabItem;
typedef struct { int n; TabItem *it; char *text; size_t tn, tcap; } TabSnap;
static int snap_put(TabSnap *t, const char *s) {
  size_t n = strlen(s) + 1; if (t->tn + n > t->tcap) { size_t nc = t->tcap ? t->tcap * 2 : 65536; while (nc < t->tn + n) nc *= 2; char *nb = (char *)realloc(t->text, nc); if (!nb) return -1; t->text = nb; t->tcap = nc; }
  memcpy(t->text + t->tn, s, n); int at = (int)t->tn; t->tn += n; return at;
}
static const char *snap_str(const TabSnap *t, int at) { return at >= 0 && t->text ? t->text + at : ""; }
static int tab_rowtype(const MsRow *r) { return r->type == RT_HEADER ? 0 : r->type == RT_SLIDER && (g_kt.slider_native || r->nopt > MS_TAB_STOPS_LIST) ? 2 : 1; }
static void tab_desc(const MsCfg *c, const MsRow *r, char *out, size_t n) {
  const char *base = r->type == RT_INFO ? pool_str(c, r->s_label) : pool_str(c, r->s_desc);
  const char *note = r->store == ST_BAND ? (r->rec >= 0 ? " (Dynasty setting: it goes into the dynasty you have loaded.)" : " (Dynasty setting without a save slot yet: kept, not applied.)")
                   : r->bind == BD_TUNING && !g_ku.on ? " (Kept, not applied: the tuning writer is switched off in modsettings.ini.)" : "";
  snprintf(out, n, "%s%s", base, *base ? note : note[0] ? note + 1 : "");
#define DESC_ADD(...) do { size_t l_ = strlen(out); snprintf(out + l_, n - l_, "%s", l_ ? " " : ""); l_ = strlen(out); snprintf(out + l_, n - l_, __VA_ARGS__); } while (0)
  if (r->copy == 1) DESC_ADD("Simulation style only.");   /* rule 4 */
  else if (r->copy == 2) DESC_ADD("Not used in Simulation style.");
  if (r->applies == 0) DESC_ADD("Applies: right away."); else if (r->applies == 1 && tab_choice(r)) DESC_ADD("Applies: from the next game."); else if (r->applies == 2) DESC_ADD("Applies: after a restart of the game.");   /* rule 3 */
  if (r->asis) DESC_ADD("As installed = the value the game loaded (nothing is written).");   /* rule 2 */
  if (r->s_also >= 0) DESC_ADD("Also set by %s: the last change wins.", pool_str(c, r->s_also));   /* rule 1 */
  if (r->unavail) DESC_ADD("Not available: %s.", pool_str(c, r->s_unavail));   /* rules 6 and 7 */
#undef DESC_ADD
}
static void tab_optlabel(const MsCfg *c, const MsRow *r, int j, char *out, size_t n) { opt_label(c, r, j, out, n); }
/* ---- sections (spec v3 "Names, duplicates and clashes" + "Adding rows to another mod's section", 11-11)
 * A section is a mod that shows its own rows (its header = its visible title), or a shared "@name" section (header = the name made friendly).
 * Visible title: the title; when another loaded mod's title matches (case and spaces ignored) + " (<source file>)"; a mod stored as
 * <modid>@<source> + " (duplicate id)". A mod with "section" = another mod's modid joins that mod's section behind a sub-header
 * "added by <title>" (after the owner's rows; contributors by title), unless the owner is "closed" (then: its own section + a greyed note
 * row) or not installed / itself placed elsewhere (then: its own section). "@name" joins the shared section (contributors by title, each
 * behind its sub-header). Sections are sorted by visible title. Storage never moves: a row stays <its own modid> + <key>. */
typedef struct { char title[160]; int kind, m; char shared[48]; } TabSec;   /* kind 0 = mod m's section, 1 = shared section "shared" */
static void tab_friendly(const char *at, char *out, size_t n) {   /* "@pass-rush" -> "Pass Rush" */
  size_t o = 0; int up = 1;
  for (const char *p = at[0] == '@' ? at + 1 : at; *p && o + 1 < n; p++) { if (*p == '-') { out[o++] = ' '; up = 1; continue; } out[o++] = up && *p >= 'a' && *p <= 'z' ? (char)(*p - 32) : *p; up = 0; }
  out[o] = 0;
}
static void tab_vis(const MsCfg *c, int m, char *out, size_t n) {
  const MsMod *mo = &c->mods[m]; char a[96], b[96]; int clash = 0; norm_title(pool_str(c, mo->s_title), a, sizeof a);
  for (int x = 0; x < c->nmods; x++) if (x != m) { norm_title(pool_str(c, c->mods[x].s_title), b, sizeof b); if (!strcmp(a, b)) clash = 1; }
  snprintf(out, n, "%s%s%s%s%s%s", pool_str(c, mo->s_title), clash ? " (" : "", clash ? pool_str(c, mo->s_source) : "", clash ? ")" : "", mo->dup ? " (duplicate id)" : "", mo->active ? "" : " (mod not active)");
}
/* where mod m's rows go: -1 its own section, -2 a shared section, else the owner mod's index; *refused = the owner is closed */
static int tab_place(const MsCfg *c, int m, int *refused) {
  *refused = 0; const char *s = pool_str(c, c->mods[m].s_section); if (!*s) return -1;
  if (s[0] == '@') return -2;
  for (int x = 0; x < c->nmods; x++) if (x != m && !strcmp(c->mods[x].id, s) && (c->mods[x].active || g_kt.show_inactive)) {   /* a hidden (inactive) owner counts as missing */
    if (c->mods[x].closed) { *refused = 1; return -1; }
    if (c->mods[x].s_section >= 0) return -1;   /* the owner shows its rows elsewhere: this mod keeps its own section */
    return x;
  }
  return -1;   /* the target is not installed: fall back to its own section */
}
static int tab_snapshot(TabSnap *t) {
  memset(t, 0, sizeof *t); char buf[2400], vis[200];
  AcquireSRWLockShared(&g_cfg_lock);
  const MsCfg *c = g_cfg; int nm = c ? c->nmods : 0, max = (c ? c->nrows + 3 * c->nmods : 0) + 6;
  t->it = (TabItem *)calloc((size_t)max, sizeof *t->it);
  TabSec *sec = (TabSec *)calloc((size_t)(nm ? nm : 1), sizeof *sec); int ns = 0;
  int place[MS_MAXMODS], refused[MS_MAXMODS], ord[MS_MAXMODS];
  if (!t->it || !sec) { free(sec); ReleaseSRWLockShared(&g_cfg_lock); free(t->it); t->it = NULL; return 0; }
#define TAB_ADD(ty, ky, en, dp, ds) do { TabItem *x_ = &t->it[t->n++]; x_->type = (ty); x_->key = (ky); x_->enabled = (en); x_->disp = snap_put(t, (dp)); x_->desc = snap_put(t, (ds)); } while (0)
  if (!nm) {
    TAB_ADD(0, -1, 0, "Mod Settings", "");
    TAB_ADD(1, MS_TAB_KEY_INFO + 1, 0, "No mod with settings is installed", "A mod adds its rows here with one manifest file in Plugins\\ModSettings\\mods (or its .fbmod description).");
  }
  for (int m = 0; m < nm; m++) {
    if (!c->mods[m].active && !g_kt.show_inactive) { place[m] = -3; refused[m] = 0; continue; }   /* rule 8: hidden */
    place[m] = tab_place(c, m, &refused[m]);
    if (place[m] == -1) { TabSec *s = &sec[ns++]; s->kind = 0; s->m = m; tab_vis(c, m, s->title, sizeof s->title); }
    else if (place[m] == -2) {
      const char *sh = pool_str(c, c->mods[m].s_section); int have = 0;
      for (int q = 0; q < ns; q++) if (sec[q].kind == 1 && !strcmp(sec[q].shared, sh)) have = 1;
      if (!have) { TabSec *s = &sec[ns++]; s->kind = 1; s->m = -1; snprintf(s->shared, sizeof s->shared, "%s", sh); tab_friendly(sh, s->title, sizeof s->title); }
    }
  }
  for (int a = 1; a < ns; a++) for (int b = a; b > 0; b--) {   /* sections by visible title (then mod id / shared name) */
    int x = _stricmp(sec[b - 1].title, sec[b].title); if (!x) x = strcmp(sec[b - 1].kind ? sec[b - 1].shared : c->mods[sec[b - 1].m].id, sec[b].kind ? sec[b].shared : c->mods[sec[b].m].id);
    if (x <= 0) break; TabSec tmp = sec[b]; sec[b] = sec[b - 1]; sec[b - 1] = tmp;
  }
  int notes = 0;
  for (int q = 0; q < ns; q++) {
    const TabSec *s = &sec[q];
    if (s->kind == 0) {
      const MsMod *mo = &c->mods[s->m]; const char *ver = pool_str(c, mo->s_version);
      if (mo->dup) snprintf(buf, sizeof buf, "%s %s. Another mod already uses the id; this one's settings are kept as %s.", pool_str(c, mo->s_title), ver, mo->id);
      else snprintf(buf, sizeof buf, "%s%s%s", pool_str(c, mo->s_title), *ver ? " " : "", ver);
      TAB_ADD(0, -1, 0, s->title, buf);
      if (refused[s->m]) {
        snprintf(vis, sizeof vis, "Meant for the %s section", pool_str(c, mo->s_section));
        snprintf(buf, sizeof buf, "These rows asked to join the section of %s, which does not take rows from other mods, so they are shown here.", pool_str(c, mo->s_section));
        TAB_ADD(1, MS_TAB_KEY_INFO + 3 + (notes++ % 12), 0, vis, buf);
      }
    } else { snprintf(buf, sizeof buf, "Shared section %s: rows several mods add here.", s->shared); TAB_ADD(0, -1, 0, s->title, buf); }
    /* the owner's own rows (kind 0), then the contributors by visible title, each behind "added by <title>" */
    int no = 0;
    for (int m = 0; m < nm; m++) if ((s->kind == 0 && place[m] == s->m) || (s->kind == 1 && place[m] == -2 && !strcmp(pool_str(c, c->mods[m].s_section), s->shared))) ord[no++] = m;
    for (int a = 1; a < no; a++) for (int b = a; b > 0; b--) { char va[200], vb[200]; tab_vis(c, ord[b - 1], va, sizeof va); tab_vis(c, ord[b], vb, sizeof vb); int x = _stricmp(va, vb); if (!x) x = strcmp(c->mods[ord[b - 1]].id, c->mods[ord[b]].id); if (x <= 0) break; int tmp = ord[b]; ord[b] = ord[b - 1]; ord[b - 1] = tmp; }
    for (int z = -1; z < no; z++) {
      int m = z < 0 ? (s->kind == 0 ? s->m : -1) : ord[z]; if (m < 0) continue;
      const MsMod *mo = &c->mods[m];
      if (z >= 0) { tab_vis(c, m, vis, sizeof vis); char sub[220]; snprintf(sub, sizeof sub, "added by %s", vis); snprintf(buf, sizeof buf, "Rows of %s (settings kept under %s).", vis, mo->id); TAB_ADD(0, -1, 0, sub, buf); }
      if (mo->s_about >= 0) { char ab[200]; snprintf(ab, sizeof ab, "About %s", pool_str(c, mo->s_title)); TAB_ADD(1, MS_TAB_KEY_ABOUT + m, 0, ab, pool_str(c, mo->s_about)); }   /* rule 19 */
      for (int i = mo->first; i < mo->first + mo->nrows; i++) {
        const MsRow *r = &c->rows[i]; tab_desc(c, r, buf, sizeof buf);
        if (!row_visible(c, i)) continue;   /* rule 9 */
        char lbl[240]; snprintf(lbl, sizeof lbl, "%s%s%s", pool_str(c, r->s_label), r->s_also >= 0 ? " - also set by " : "", r->s_also >= 0 ? pool_str(c, r->s_also) : "");
        if (r->type == RT_HEADER) TAB_ADD(0, -1, 0, pool_str(c, r->s_label), "");
        else TAB_ADD(tab_rowtype(r), MS_TAB_KEY0 + i, r->type != RT_INFO && row_live(c, i), lbl, buf);
      }
    }
  }
  if (c && c->nproblems) {
    char first[300] = ""; const char *e = strchr(c->problems, '\n'); size_t fl = e ? (size_t)(e - c->problems) : strlen(c->problems); if (fl >= sizeof first) fl = sizeof first - 1; memcpy(first, c->problems, fl); first[fl] = 0;
    char d1[64]; snprintf(d1, sizeof d1, "%d problem(s) in the mod manifests", c->nproblems);
    snprintf(buf, sizeof buf, "Those rows are not shown. The list is in Plugins\\ModSettings\\modsettings-status.json. First: %s", first);
    TAB_ADD(1, MS_TAB_KEY_INFO + 2, 0, d1, buf);
  }
#undef TAB_ADD
  ReleaseSRWLockShared(&g_cfg_lock);
  free(sec);
  return 1;
}
static void snap_free(TabSnap *t) { free(t->it); free(t->text); memset(t, 0, sizeof *t); }
/* one row's options + current value, copied out */
typedef struct { int ok, rt, nopt, cur, dflt, choice, special, live; MsRow row; char unit[40]; char opts[MS_MAXLIST][100]; char why[100]; } TabRow;
static int tab_row(int key, TabRow *o) {
  memset(o, 0, sizeof *o);
  if ((key >= MS_TAB_KEY_INFO + 1 && key <= MS_TAB_KEY_INFO + 14) || (key >= MS_TAB_KEY_ABOUT && key < MS_TAB_KEY_ABOUT + MS_MAXMODS)) { o->ok = 1; o->special = 1; o->rt = 1; o->nopt = 1; return 1; }   /* info rows of the feed itself (0x4CFF is the page's own fallback key) */
  int i = key - MS_TAB_KEY0;
  AcquireSRWLockShared(&g_cfg_lock);
  const MsCfg *c = g_cfg;
  if (c && i >= 0 && i < c->nrows && c->rows[i].type != RT_HEADER) {
    const MsRow *r = &c->rows[i]; o->ok = 1; o->row = *r; o->rt = tab_rowtype(r); o->choice = tab_choice(r); o->nopt = o->choice ? r->nopt : 1; o->cur = o->choice ? tab_value(c, i) : 0; o->dflt = r->dflt;
    snprintf(o->unit, sizeof o->unit, "%s", pool_str(c, r->s_unit));
    if (r->type != RT_SLIDER) for (int j = 0; j < r->nopt - r->asis && j < MS_MAXLIST; j++) snprintf(o->opts[j], sizeof o->opts[j], "%s", pool_str(c, r->s_opt[j]));
    o->live = row_live(c, i) && row_visible(c, i); if (r->unavail) snprintf(o->why, sizeof o->why, "%s", pool_str(c, r->s_unavail));
  }
  ReleaseSRWLockShared(&g_cfg_lock);
  return o->ok;
}
static void tab_row_label(const TabRow *o, int j, char *out, size_t n) {
  if (o->row.asis && j == 0) { snprintf(out, n, "As installed"); return; }
  int k = j - o->row.asis;
  if (o->row.type == RT_SLIDER) snprintf(out, n, "%g%s", slider_value(&o->row, k), o->unit); else snprintf(out, n, "%s", k >= 0 && k < MS_MAXLIST ? o->opts[k] : "");
}

/* ------------------------------------------------------------------ the game's holders + Variant (their own vtables: wsdata's setters) */
typedef void (*th_abool)(void *h, int row, const char *name, char v);
typedef void (*th_aint)(void *h, int row, const char *name, int v);
typedef void (*th_astr)(void *h, int row, const char *name, const char *v);
typedef void (*th_oint)(void *h, const char *name, int v);
typedef void (*th_ostr)(void *h, const char *name, const char *v);
static void ta_bool(void *h, int r, const char *n, int v) { (*(th_abool **)h)[0x18 / 8](h, r, n, (char)(v != 0)); }
static void ta_int(void *h, int r, const char *n, int v) { (*(th_aint **)h)[0x20 / 8](h, r, n, v); }
static void ta_str(void *h, int r, const char *n, const char *v) { (*(th_astr **)h)[0x30 / 8](h, r, n, v); }
static void to_int(void *h, const char *n, int v) { (*(th_oint **)h)[0x10 / 8](h, n, v); }
static void to_str(void *h, const char *n, const char *v) { (*(th_ostr **)h)[0x20 / 8](h, n, v); }
typedef int (*tv_getint)(void *v);
static int tab_var_int(void *v, int *out) {
  if (!v || !readable(v, 16)) return 0;
  void **vt = *(void ***)v; if (!vt || !readable(vt, 0x20)) return 0;
  if (!g_t_fake && (void *)vt != AT(g_pb->tab.var_vt_rva)) return 0;
  *out = ((tv_getint)vt[0x18 / 8])(v); return 1;
}
/* ours: the private category, the pinned adapter object, the game's static holder of the expected kind */
static int tab_ours(int ts, void *self, void *holder, void *vcat) {
  InterlockedIncrement(&g_tab_calls[ts]);
  if (g_tab_off || !g_kt.on) return 0;
  int cat; if (!tab_var_int(vcat, &cat) || cat != MS_TAB_CAT) return 0;
  int obj = ts == TS_SLIDER, ok = holder && readable(holder, 16);
  if (ok && !g_t_fake) {
    const MsTabPin *t = &g_pb->tab;
    ok = self == AT(t->obj_rva) && holder == AT(obj ? t->objh_rva : t->arr_rva) && *(void **)holder == AT(obj ? t->objh_vt_rva : t->arr_vt_rva);
  }
  LONG n = InterlockedIncrement(&g_tab_ours[ts]);
  if (n <= (LONG)g_kt.log_calls) logf_("modsettings: tab %s #%ld on thread %lu (holder %p)%s", TAB_NAME[ts], n, GetCurrentThreadId(), holder, ok ? "" : " - NOT the game's holder / adapter: left to the game");
  return ok;
}
typedef uintptr_t (*tab_f3)(void *, void *, void *);
typedef uintptr_t (*tab_f4)(void *, void *, void *, void *);
typedef uintptr_t (*tab_f6)(void *, void *, void *, void *, void *, const char *);
typedef uintptr_t (*tab_f5i)(void *, void *, void *, void *, int);

typedef struct { void *h, *vkey, *vval; int ival, ts, n; } TabArgs;
static void tab_do_list(void *arg) {
  TabArgs *a = (TabArgs *)arg; TabSnap t; if (!tab_snapshot(&t)) return;
  for (int k = 0; k < t.n; k++) {
    const TabItem *x = &t.it[k];
    ta_int(a->h, k, "key", x->key); ta_int(a->h, k, "type", x->type); ta_str(a->h, k, "display", snap_str(&t, x->disp));
    ta_str(a->h, k, "description", snap_str(&t, x->desc)); ta_bool(a->h, k, "isEnabled", x->enabled);
  }
  a->n = t.n; snap_free(&t);
}
static void tab_do_toggle(void *arg) {
  TabArgs *a = (TabArgs *)arg; int key; if (!tab_var_int(a->vkey, &key)) return;
  TabRow o; if (!tab_row(key, &o) || o.rt != 1) return;
  if (!o.choice) { ta_str(a->h, 0, "display", ""); ta_int(a->h, 0, "key", 0); ta_bool(a->h, 0, "isDefault", 1); a->n = 1; return; }   /* info: one item (greyed) */
  if (o.row.unavail) { ta_str(a->h, 0, "display", o.why); ta_int(a->h, 0, "key", 0); ta_bool(a->h, 0, "isDefault", 1); a->n = 1; return; }   /* rules 6 / 7: one item that says why */
  char lbl[100];
  for (int j = 0; j < o.nopt; j++) { tab_row_label(&o, j, lbl, sizeof lbl); ta_str(a->h, j, "display", lbl); ta_int(a->h, j, "key", j); ta_bool(a->h, j, "isDefault", j == o.cur); }
  a->n = o.nopt;
}
static void tab_do_slider(void *arg) {
  TabArgs *a = (TabArgs *)arg; int key; if (!tab_var_int(a->vkey, &key)) return;
  TabRow o; if (!tab_row(key, &o) || o.rt != 2) return;
  char l[100];
  to_int(a->h, "min", 0); to_int(a->h, "max", o.nopt - 1); to_int(a->h, "step", 1); to_int(a->h, "defaultValue", o.cur); to_int(a->h, "resetValue", o.dflt); to_int(a->h, "key", key);
  tab_row_label(&o, 0, l, sizeof l); to_str(a->h, "leftLabel", l); to_str(a->h, "middleLabel", ""); tab_row_label(&o, o.nopt - 1, l, sizeof l); to_str(a->h, "rightLabel", l);
  a->n = 1;
}
static void tab_do_adjust(void *arg) {
  TabArgs *a = (TabArgs *)arg; int key, v = a->ival;
  if (!tab_var_int(a->vkey, &key)) return;
  if (a->ts == TS_ADJT && !tab_var_int(a->vval, &v)) return;
  a->n = tab_set(key - MS_TAB_KEY0, v, a->ts == TS_ADJT ? "the tab (toggle)" : "the tab (slider)");
}
static uintptr_t tab_w_list(void *self, void *h, void *vcat) {
  if (!tab_ours(TS_LIST, self, h, vcat)) return ((tab_f3)g_tab_orig[TS_LIST])(self, h, vcat);
  TabArgs a = { h, NULL, NULL, 0, TS_LIST, 0 }; ms_guarded2(tab_do_list, &a, "the Mod Settings tab list", &g_tab_off);
  if (g_tab_ours[TS_LIST] <= g_kt.log_calls) logf_("modsettings: tab list: %d row(s)", a.n);
  return 0;
}
static uintptr_t tab_w_toggle(void *self, void *h, void *vcat, void *vkey) {
  if (!tab_ours(TS_TOGGLE, self, h, vcat)) return ((tab_f4)g_tab_orig[TS_TOGGLE])(self, h, vcat, vkey);
  TabArgs a = { h, vkey, NULL, 0, TS_TOGGLE, 0 }; ms_guarded2(tab_do_toggle, &a, "the Mod Settings tab options", &g_tab_off); return 0;
}
static uintptr_t tab_w_slider(void *self, void *h, void *vcat, void *vkey) {
  if (!tab_ours(TS_SLIDER, self, h, vcat)) return ((tab_f4)g_tab_orig[TS_SLIDER])(self, h, vcat, vkey);
  TabArgs a = { h, vkey, NULL, 0, TS_SLIDER, 0 }; ms_guarded2(tab_do_slider, &a, "the Mod Settings tab slider", &g_tab_off); return 0;
}
static uintptr_t tab_w_adjt(void *self, void *h, void *vcat, void *vkey, void *vval, const char *why) {
  if (!tab_ours(TS_ADJT, self, h, vcat)) return ((tab_f6)g_tab_orig[TS_ADJT])(self, h, vcat, vkey, vval, why);
  TabArgs a = { h, vkey, vval, 0, TS_ADJT, 0 }; ms_guarded2(tab_do_adjust, &a, "a Mod Settings tab change", &g_tab_off); return 0;
}
static uintptr_t tab_w_adjs(void *self, void *h, void *vcat, void *vkey, int v) {
  if (!tab_ours(TS_ADJS, self, h, vcat)) return ((tab_f5i)g_tab_orig[TS_ADJS])(self, h, vcat, vkey, v);
  TabArgs a = { h, vkey, NULL, v, TS_ADJS, 0 }; ms_guarded2(tab_do_adjust, &a, "a Mod Settings tab slider change", &g_tab_off); return 0;
}
static void *const TAB_FN[TS_N] = { (void *)tab_w_list, (void *)tab_w_toggle, (void *)tab_w_slider, (void *)tab_w_adjt, (void *)tab_w_adjs };

/* ------------------------------------------------------------------ install (worker; retried while the adapter object is not created yet) */
/* 1 = finished (installed, or refused for good: logged), 0 = try again later */
static int tab_install(void) {
  if (g_tab_done) return 1;
  if (!g_k.enabled || !g_kt.on) { g_tab_done = 1; snprintf(g_tab_state, sizeof g_tab_state, "off"); logf_("modsettings: tab feed off (%s = 0)", g_k.enabled ? "tab" : "enabled"); return 1; }
  size_t size = 0; uint32_t stamp = 0; int pref = 0; unsigned char *base = ps_image(&size, &stamp, &pref);
  const MsBuild *pb = base ? ms_build_of(stamp) : NULL;
#define TAB_REFUSE(...) do { snprintf(g_tab_reason, sizeof g_tab_reason, __VA_ARGS__); snprintf(g_tab_state, sizeof g_tab_state, "refused"); g_tab_done = 1; logf_("modsettings: tab feed REFUSED: %s (the tab shows its 'not running' row; band writes are separate)", g_tab_reason); return 1; } while (0)
  if (!pb) TAB_REFUSE("this game build (PE %u) is not one this version knows", stamp);
  if (!pref) TAB_REFUSE("the game is not at its preferred base");
  g_base = base; { IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base; IMAGE_NT_HEADERS64 *nt = (IMAGE_NT_HEADERS64 *)(base + dos->e_lfanew); g_hdr_base = nt->OptionalHeader.ImageBase; }
  g_pb = pb; const MsTabPin *t = &pb->tab;
  void **vt = (void **)AT(t->vt_rva);
  if (!readable(vt, (size_t)t->nvt * 8)) TAB_REFUSE("the settings adapter vtable is not readable");
  for (int k = 0; k < t->nvt; k++) if ((uint64_t)(uintptr_t)vt[k] - g_hdr_base != t->vt_stock[k]) TAB_REFUSE("settings adapter slot 0x%x is %p, not stock RVA 0x%x (another tool patched it, or another build)", k * 8, vt[k], t->vt_stock[k]);
  /* pointers compared as RVAs against the header's image base (the game runs at its preferred base, checked above) */
#define TAB_RVA(p) ((uint64_t)(uintptr_t)(p) - g_hdr_base)
  void **glob = (void **)AT(t->global_rva);
  if (!readable(glob, 8) || TAB_RVA(*glob) != t->obj_rva) { snprintf(g_tab_state, sizeof g_tab_state, "waiting"); snprintf(g_tab_reason, sizeof g_tab_reason, "the game's settings adapter is not created yet"); return 0; }
  if (!readable(AT(t->obj_rva), 8) || TAB_RVA(*(void **)AT(t->obj_rva)) != t->vt_rva) TAB_REFUSE("the settings adapter object does not use the pinned vtable");
  if (!readable(AT(t->arr_rva), 16) || TAB_RVA(*(void **)AT(t->arr_rva)) != t->arr_vt_rva || !readable(AT(t->objh_rva), 16) || TAB_RVA(*(void **)AT(t->objh_rva)) != t->objh_vt_rva) TAB_REFUSE("a value holder is not the pinned one");
#undef TAB_RVA
  if (!readable(AT(t->var_vt_rva), 0x20)) TAB_REFUSE("the Variant vtable is not readable");
  ms_guard_init();
  int done = 0;
  for (int s = 0; s < TS_N; s++) {
    g_tab_orig[s] = AT(t->vt_stock[TAB_SLOT[s] / 8]);   /* set before the swap: the UI thread may call the wrapper at once */
    char w[96]; snprintf(w, sizeof w, "modsettings: settings adapter slot 0x%x %s", TAB_SLOT[s], TAB_NAME[s]);
    if (!ps_patch_ptr(&vt[TAB_SLOT[s] / 8], TAB_FN[s], w)) break;
    done++;
  }
  if (done < TS_N) {
    for (int s = 0; s < done; s++) ps_patch_ptr(&vt[TAB_SLOT[s] / 8], g_tab_orig[s], "modsettings: settings adapter slot put back");
    TAB_REFUSE("slot 0x%x could not be written: the other %d put back", TAB_SLOT[done], done);
  }
#undef TAB_REFUSE
  g_tab_done = 1; snprintf(g_tab_state, sizeof g_tab_state, "installed"); g_tab_reason[0] = 0;
  logf_("modsettings: tab feed installed (%s): settings adapter slots 0x30 0x38 0x40 0x50 0x58 wrapped for category %d; every other category goes to the game", pb->name, MS_TAB_CAT);
  InterlockedExchange(&g_status_dirty, 1);
  return 1;
}
/* the status file's "tab" block: the state and the rows exactly as the tab shows them */
static void tab_status(Sb *s) {
  sb_add(s, ",\"tab\":{\"state\":"); sb_q(s, g_tab_state); sb_add(s, ",\"reason\":"); sb_q(s, g_tab_reason);
  sb_add(s, ",\"category\":%d,\"off_after_fault\":%ld,\"changes\":%ld,\"settings_writes\":%ld,\"journal\":%d,\"calls\":{", MS_TAB_CAT, (long)g_tab_off, (long)g_tab_changes, g_settings_writes, g_njq);
  for (int k = 0; k < TS_N; k++) sb_add(s, "%s\"%s\":[%ld,%ld]", k ? "," : "", TAB_NAME[k], (long)g_tab_calls[k], (long)g_tab_ours[k]);
  sb_add(s, "},\"last_change\":"); sb_q(s, g_tab_last); sb_add(s, ",\"rows\":[");
  TabSnap t; if (tab_snapshot(&t)) {
    for (int k = 0; k < t.n; k++) {
      const TabItem *x = &t.it[k]; sb_add(s, "%s{\"key\":%d,\"type\":%d,\"enabled\":%d,\"display\":", k ? "," : "", x->key, x->type, x->enabled); sb_q(s, snap_str(&t, x->disp));
      TabRow o; if (x->key >= MS_TAB_KEY0 && tab_row(x->key, &o) && o.choice) { char l[100]; tab_row_label(&o, o.cur, l, sizeof l); sb_add(s, ",\"value\":%d,\"shows\":", o.cur); sb_q(s, l); }
      sb_add(s, "}");
    }
    snap_free(&t);
  }
  sb_add(s, "]}");
}
#endif
