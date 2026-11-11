/* msjson.h: a small strict JSON reader for modsettings.dll (manifests, values.json). No heap per node: one caller-owned arena of
 * nodes; strings are decoded into a caller-owned text buffer. Objects / arrays keep their children as a linked list (first, next).
 * Limits are the caller's (nodes, text bytes, depth 32); anything over a limit, or any syntax error, fails the whole parse with a
 * message and the byte offset. Numbers: integers only are kept exactly (a fraction or exponent is parsed and kept as a double too).
 * Strings: \uXXXX escapes are decoded to UTF-8 (surrogate pairs joined); control characters inside a string are refused. */
#ifndef MS_JSON_H
#define MS_JSON_H
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct JNode { int type; const char *key; const char *str; double num; long long inum; int isint; int first, next, count; } JNode;
typedef struct { JNode *n; int cap, used; char *text; size_t tcap, tused; const char *src, *p, *end; char err[160]; int depth; } JDoc;

static int j_fail(JDoc *d, const char *what) { if (!d->err[0]) snprintf(d->err, sizeof d->err, "%s at byte %d", what, (int)(d->p - d->src)); return -1; }
static void j_ws(JDoc *d) { while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\r' || *d->p == '\n')) d->p++; }
static int j_new(JDoc *d, int type) { if (d->used >= d->cap) return j_fail(d, "too many values"); JNode *x = &d->n[d->used]; memset(x, 0, sizeof *x); x->type = type; x->first = x->next = -1; return d->used++; }
static int j_put(JDoc *d, const char *s, size_t n) { if (d->tused + n + 1 > d->tcap) return 0; memcpy(d->text + d->tused, s, n); d->tused += n; return 1; }
static int j_hex4(const char *p, unsigned *out) { unsigned v = 0; for (int i = 0; i < 4; i++) { char c = p[i]; v <<= 4; if (c >= '0' && c <= '9') v |= (unsigned)(c - '0'); else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10); else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10); else return 0; } *out = v; return 1; }
/* a string at d->p (the opening quote): decoded into the text buffer; returns its start or NULL */
static const char *j_string(JDoc *d) {
  if (d->p >= d->end || *d->p != '"') { j_fail(d, "a string was expected"); return NULL; }
  d->p++; size_t start = d->tused;
  while (d->p < d->end && *d->p != '"') {
    unsigned char c = (unsigned char)*d->p;
    if (c < 0x20) { j_fail(d, "a control character inside a string"); return NULL; }
    if (c != '\\') { if (!j_put(d, d->p, 1)) { j_fail(d, "the text is too long"); return NULL; } d->p++; continue; }
    if (d->p + 1 >= d->end) { j_fail(d, "an unfinished escape"); return NULL; }
    char e = d->p[1]; d->p += 2; char o;
    switch (e) {
      case '"': o = '"'; break; case '\\': o = '\\'; break; case '/': o = '/'; break; case 'b': o = '\b'; break;
      case 'f': o = '\f'; break; case 'n': o = '\n'; break; case 'r': o = '\r'; break; case 't': o = '\t'; break;
      case 'u': {
        unsigned u; if (d->end - d->p < 4 || !j_hex4(d->p, &u)) { j_fail(d, "a bad \\u escape"); return NULL; } d->p += 4;
        if (u >= 0xd800 && u <= 0xdbff) { unsigned l; if (d->end - d->p < 6 || d->p[0] != '\\' || d->p[1] != 'u' || !j_hex4(d->p + 2, &l) || l < 0xdc00 || l > 0xdfff) { j_fail(d, "a lone surrogate"); return NULL; } d->p += 6; u = 0x10000 + ((u - 0xd800) << 10) + (l - 0xdc00); }
        else if (u >= 0xdc00 && u <= 0xdfff) { j_fail(d, "a lone surrogate"); return NULL; }
        char b[4]; size_t n;
        if (u < 0x80) { b[0] = (char)u; n = 1; } else if (u < 0x800) { b[0] = (char)(0xc0 | (u >> 6)); b[1] = (char)(0x80 | (u & 0x3f)); n = 2; }
        else if (u < 0x10000) { b[0] = (char)(0xe0 | (u >> 12)); b[1] = (char)(0x80 | ((u >> 6) & 0x3f)); b[2] = (char)(0x80 | (u & 0x3f)); n = 3; }
        else { b[0] = (char)(0xf0 | (u >> 18)); b[1] = (char)(0x80 | ((u >> 12) & 0x3f)); b[2] = (char)(0x80 | ((u >> 6) & 0x3f)); b[3] = (char)(0x80 | (u & 0x3f)); n = 4; }
        if (!j_put(d, b, n)) { j_fail(d, "the text is too long"); return NULL; }
        continue;
      }
      default: j_fail(d, "an unknown escape"); return NULL;
    }
    if (!j_put(d, &o, 1)) { j_fail(d, "the text is too long"); return NULL; }
  }
  if (d->p >= d->end) { j_fail(d, "an unfinished string"); return NULL; }
  d->p++; if (!j_put(d, "", 1)) { j_fail(d, "the text is too long"); return NULL; }
  return d->text + start;
}
static int j_value(JDoc *d);
static int j_link(JDoc *d, int parent, int *last, int child) { JNode *P = &d->n[parent]; if (*last < 0) P->first = child; else d->n[*last].next = child; *last = child; P->count++; return 0; }
static int j_value(JDoc *d) {
  j_ws(d); if (d->p >= d->end) return j_fail(d, "a value was expected");
  char c = *d->p;
  if (c == '{' || c == '[') {
    if (++d->depth > 32) return j_fail(d, "nested too deep");
    int me = j_new(d, c == '{' ? J_OBJ : J_ARR); if (me < 0) return -1; int last = -1; d->p++; j_ws(d);
    char close = c == '{' ? '}' : ']';
    if (d->p < d->end && *d->p == close) { d->p++; d->depth--; return me; }
    for (;;) {
      const char *key = NULL;
      if (c == '{') { j_ws(d); key = j_string(d); if (!key) return -1; j_ws(d); if (d->p >= d->end || *d->p != ':') return j_fail(d, "':' was expected"); d->p++; }
      int v = j_value(d); if (v < 0) return -1; d->n[v].key = key; j_link(d, me, &last, v);
      j_ws(d); if (d->p >= d->end) return j_fail(d, "an unfinished object or array");
      if (*d->p == ',') { d->p++; continue; }
      if (*d->p == close) { d->p++; break; }
      return j_fail(d, "',' or the closing bracket was expected");
    }
    if (c == '{') {   /* duplicate keys are refused (a manifest that says a field twice is a mistake) */
      for (int a = d->n[me].first; a >= 0; a = d->n[a].next) for (int b = d->n[a].next; b >= 0; b = d->n[b].next) if (!strcmp(d->n[a].key, d->n[b].key)) return j_fail(d, "a key appears twice in one object");
    }
    d->depth--; return me;
  }
  if (c == '"') { const char *s = j_string(d); if (!s) return -1; int me = j_new(d, J_STR); if (me < 0) return -1; d->n[me].str = s; return me; }
  if (!strncmp(d->p, "true", 4) && d->end - d->p >= 4) { d->p += 4; return j_new(d, J_TRUE); }
  if (!strncmp(d->p, "false", 5) && d->end - d->p >= 5) { d->p += 5; return j_new(d, J_FALSE); }
  if (!strncmp(d->p, "null", 4) && d->end - d->p >= 4) { d->p += 4; return j_new(d, J_NULL); }
  if (c == '-' || (c >= '0' && c <= '9')) {
    const char *s = d->p; if (*d->p == '-') d->p++;
    if (d->p >= d->end || !(*d->p >= '0' && *d->p <= '9')) return j_fail(d, "a bad number");
    if (*d->p == '0' && d->p + 1 < d->end && d->p[1] >= '0' && d->p[1] <= '9') return j_fail(d, "a number with a leading zero");
    while (d->p < d->end && *d->p >= '0' && *d->p <= '9') d->p++;
    int isint = 1;
    if (d->p < d->end && *d->p == '.') { isint = 0; d->p++; if (d->p >= d->end || !(*d->p >= '0' && *d->p <= '9')) return j_fail(d, "a bad number"); while (d->p < d->end && *d->p >= '0' && *d->p <= '9') d->p++; }
    if (d->p < d->end && (*d->p == 'e' || *d->p == 'E')) { isint = 0; d->p++; if (d->p < d->end && (*d->p == '+' || *d->p == '-')) d->p++; if (d->p >= d->end || !(*d->p >= '0' && *d->p <= '9')) return j_fail(d, "a bad number"); while (d->p < d->end && *d->p >= '0' && *d->p <= '9') d->p++; }
    char tmp[64]; size_t n = (size_t)(d->p - s); if (n >= sizeof tmp) return j_fail(d, "a number too long");
    memcpy(tmp, s, n); tmp[n] = 0;
    int me = j_new(d, J_NUM); if (me < 0) return -1; d->n[me].num = strtod(tmp, NULL); d->n[me].isint = isint && n <= 18; d->n[me].inum = d->n[me].isint ? strtoll(tmp, NULL, 10) : (long long)d->n[me].num;
    return me;
  }
  return j_fail(d, "an unexpected character");
}
/* parse src[0..len) into the arena; returns the root node index, or -1 (d->err says why). A UTF-8 byte order mark is skipped. */
static int j_parse(JDoc *d, JNode *nodes, int cap, char *text, size_t tcap, const char *src, size_t len) {
  memset(d, 0, sizeof *d); d->n = nodes; d->cap = cap; d->text = text; d->tcap = tcap; d->src = src; d->p = src; d->end = src + len;
  if (len >= 3 && (unsigned char)src[0] == 0xef && (unsigned char)src[1] == 0xbb && (unsigned char)src[2] == 0xbf) d->p += 3;
  int root = j_value(d); if (root < 0) return -1;
  j_ws(d); if (d->p != d->end) return j_fail(d, "text after the value");
  return root;
}
static const JNode *j_at(const JDoc *d, int i) { return i >= 0 && i < d->used ? &d->n[i] : NULL; }
static int j_get(const JDoc *d, int obj, const char *key) { const JNode *o = j_at(d, obj); if (!o || o->type != J_OBJ) return -1; for (int c = o->first; c >= 0; c = d->n[c].next) if (!strcmp(d->n[c].key, key)) return c; return -1; }
static const char *j_str(const JDoc *d, int i) { const JNode *x = j_at(d, i); return x && x->type == J_STR ? x->str : NULL; }
static int j_isint(const JDoc *d, int i, long long *out) { const JNode *x = j_at(d, i); if (!x || x->type != J_NUM || !x->isint) return 0; if (out) *out = x->inum; return 1; }
/* write s as a JSON string literal (quotes included) into out; returns the length or 0 if it does not fit */
static size_t j_quote(char *out, size_t n, const char *s) {
  size_t o = 0; if (n < 3) return 0; out[o++] = '"';
  for (const unsigned char *c = (const unsigned char *)s; *c; c++) {
    char esc[8]; size_t k;
    if (*c == '"' || *c == '\\') { esc[0] = '\\'; esc[1] = (char)*c; k = 2; }
    else if (*c < 0x20) { k = (size_t)snprintf(esc, sizeof esc, "\\u%04x", *c); }
    else { esc[0] = (char)*c; k = 1; }
    if (o + k + 2 > n) return 0; memcpy(out + o, esc, k); o += k;
  }
  out[o++] = '"'; out[o] = 0; return o;
}
#endif
