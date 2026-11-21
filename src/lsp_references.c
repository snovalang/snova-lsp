#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_references.h"
#include "lsp_ident.h"
#include "lsp_imports.h"
#include "lsp_fuzzy.h"
#include "lsp_symbol_span.h"
#include "json.h"

#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── workspace search ────────────────────────────────────────────────────────
 *
 * gopls type-checks the declaring package and its reverse dependencies. The
 * snovac graph is per analysis, so every candidate file is analysed on its
 * own (a few milliseconds each) and compared by symbol key. Closed files are
 * read from disk into a private store and engine that live for one request.
 */

typedef struct {
    LspAnalysisEngine *engine;
    LspDocStore *store;
    LspAnalysisEngine tmp_engine;
    LspDocStore tmp_store;
    /* Client spelling of canonical paths, learned from open documents
     * (e.g. canonical /private/tmp/... is /tmp/... for the editor). */
    char real_prefix[SNOVAC_PATH_MAX];
    char client_prefix[SNOVAC_PATH_MAX];
} Search;

/* Paths are canonical (lsp_uri_to_path normalizes); only an open document's
 * URI keeps the client's spelling. Learn the differing URI prefixes. */
static void learn_spelling(Search *s, const LspDocument *doc) {
    if (s->real_prefix[0] || !doc || !doc->path || !doc->uri) return;
    char *canon = lsp_path_to_uri(doc->path);
    if (!canon) return;
    const char *client = doc->uri;
    size_t rl = strlen(canon), cl = strlen(client);
    if (strcmp(canon, client) != 0) {
        while (rl > 0 && cl > 0 && canon[rl - 1] == client[cl - 1]) { rl--; cl--; }
        while (canon[rl] && canon[rl] != '/') { rl++; cl++; } /* whole segments */
        snprintf(s->real_prefix, sizeof(s->real_prefix), "%.*s", (int)rl, canon);
        snprintf(s->client_prefix, sizeof(s->client_prefix), "%.*s", (int)cl, client);
    }
    free(canon);
}

/* URI for a canonical path, in the client's spelling. */
static char *client_uri(const Search *s, const char *path) {
    char *uri = lsp_path_to_uri(path);
    size_t rl = strlen(s->real_prefix);
    if (uri && rl && strncmp(uri, s->real_prefix, rl) == 0) {
        size_t n = strlen(s->client_prefix) + strlen(uri + rl) + 1;
        char *mapped = (char *)malloc(n);
        if (mapped) {
            snprintf(mapped, n, "%s%s", s->client_prefix, uri + rl);
            free(uri);
            uri = mapped;
        }
    }
    return uri;
}

static void search_init(Search *s, LspAnalysisEngine *engine, LspDocStore *store) {
    s->engine = engine;
    s->store = store;
    s->real_prefix[0] = s->client_prefix[0] = '\0';
    for (size_t i = 0; i < store->len; i++) learn_spelling(s, store->docs[i]);
    lsp_engine_init(&s->tmp_engine, engine->workspace_root[0] ? engine->workspace_root : NULL);
    lsp_docstore_init(&s->tmp_store);
}

static void search_free(Search *s) {
    lsp_engine_destroy(&s->tmp_engine);
    lsp_docstore_destroy(&s->tmp_store);
}

static LspDocAnalysis *analysis_of(Search *s, const LspDocument *doc) {
    if (!doc) return NULL;
    if (lsp_docstore_get(s->store, doc->uri) == doc) {
        LspDocAnalysis *a = lsp_engine_get_analysis(s->engine, doc->uri);
        if (!a || a->version != doc->version) a = lsp_engine_analyze_document(s->engine, s->store, doc);
        return a;
    }
    return lsp_engine_analyze_document(&s->tmp_engine, &s->tmp_store, doc);
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    *len = got;
    return buf;
}

/* Two spellings of one file (the graph stores canonical paths; editors may
 * send a symlinked one such as /tmp on macOS). */
static bool same_file(const char *x, const char *y) {
    if (!x || !y) return false;
    if (strcmp(x, y) == 0) return true;
    char rx[SNOVAC_PATH_MAX], ry[SNOVAC_PATH_MAX];
#ifdef _WIN32
    return _fullpath(rx, x, sizeof(rx)) && _fullpath(ry, y, sizeof(ry)) &&
           _stricmp(rx, ry) == 0;
#else
    return realpath(x, rx) && realpath(y, ry) && strcmp(rx, ry) == 0;
#endif
}

static LspDocument *store_doc_for_path(LspDocStore *store, const char *path) {
    LspDocument *doc = lsp_docstore_get_by_path(store, path);
    for (size_t i = 0; !doc && i < store->len; i++) {
        if (same_file(store->docs[i]->path, path)) doc = store->docs[i];
    }
    return doc;
}

/* Open document for `path`, or a private copy read from disk. */
static LspDocument *doc_for_path(Search *s, const char *path) {
    LspDocument *doc = store_doc_for_path(s->store, path);
    if (doc) return doc;
    doc = lsp_docstore_get_by_path(&s->tmp_store, path);
    if (doc) return doc;
    size_t len = 0;
    char *text = read_file(path, &len);
    if (!text) return NULL;
    char *uri = client_uri(s, path);
    if (uri) doc = lsp_docstore_open(&s->tmp_store, uri, 0, text, len);
    free(uri);
    free(text);
    return doc;
}

static bool is_ident_char(char c) { return isalnum((unsigned char)c) || c == '_'; }

static bool mentions_word(const char *text, size_t len, const char *word) {
    size_t wl = strlen(word);
    if (!text || !wl) return false;
    for (const char *p = text; (p = strstr(p, word)) != NULL; p++) {
        size_t at = (size_t)(p - text);
        if (at + wl > len) break;
        bool left = at == 0 || !is_ident_char(text[at - 1]);
        bool right = at + wl == len || !is_ident_char(text[at + wl]);
        if (left && right) return true;
    }
    return false;
}

typedef struct {
    char **v;
    size_t n, cap;
} PathSet;

static bool same_file(const char *x, const char *y);

static void paths_add(PathSet *ps, const char *path) {
    if (!path || !path[0] || !lsp_is_snovalang_source(path) || lsp_is_manifest_file(path)) return;
    for (size_t i = 0; i < ps->n; i++) if (same_file(ps->v[i], path)) return;
    if (ps->n == ps->cap) {
        size_t nc = ps->cap ? ps->cap * 2 : 32;
        char **nv = (char **)realloc(ps->v, nc * sizeof(char *));
        if (!nv) return;
        ps->v = nv;
        ps->cap = nc;
    }
    ps->v[ps->n++] = strdup(path);
}

static void paths_free(PathSet *ps) {
    for (size_t i = 0; i < ps->n; i++) free(ps->v[i]);
    free(ps->v);
}

/* Project files (own packages, not dependencies or builtins) and open docs. */
static void workspace_paths(Search *s, const LspDocAnalysis *a, PathSet *out) {
    if (a) {
        for (const SnPackageNode *n = a->graph.nodes; n; n = n->next) {
            if (lsp_package_origin(n) != LSP_PKG_PROJECT) continue;
            for (const SnPackageFile *f = n->files; f; f = f->next) paths_add(out, f->path);
        }
    }
    for (size_t i = 0; i < s->store->len; i++) paths_add(out, s->store->docs[i]->path);
}

typedef bool (*FileFn)(void *user, LspFileIndex *ix);

/* Calls `fn` for the origin file and, unless `origin_only`, for every
 * workspace file mentioning one of `words`. Stops when `fn` returns false. */
static void for_each_file(Search *s, const LspDocument *origin, const LspDocAnalysis *oa, bool origin_only,
                          const char *const *words, size_t nwords, FileFn fn, void *user) {
    LspFileIndex ix;
    lsp_index_build(&ix, oa, origin);
    bool go = fn(user, &ix);
    lsp_index_free(&ix);
    if (!go || origin_only) return;

    PathSet paths = {0};
    workspace_paths(s, oa, &paths);
    for (size_t i = 0; i < paths.n && go; i++) {
        if (same_file(paths.v[i], origin->path)) continue;
        LspDocument *doc = doc_for_path(s, paths.v[i]);
        if (!doc || !doc->text) continue;
        bool mentioned = false;
        for (size_t w = 0; w < nwords && !mentioned; w++) mentioned = mentions_word(doc->text, doc->text_len, words[w]);
        if (!mentioned) continue;
        LspDocAnalysis *a = analysis_of(s, doc);
        if (!a || !a->has_ast) continue;
        lsp_index_build(&ix, a, doc);
        go = fn(user, &ix);
        lsp_index_free(&ix);
    }
    paths_free(&paths);
}

/* ── hits ────────────────────────────────────────────────────────────────── */

typedef struct {
    char *uri;
    char *path;
    uint32_t offset, len;
    LspRange range;
    bool is_decl;
    bool is_write;
    bool qualified;
} Hit;

typedef struct {
    Hit *v;
    size_t n, cap;
} Hits;

static void hits_add(Hits *h, const LspFileIndex *ix, uint32_t off, uint32_t len, bool is_decl, bool is_write,
                     bool qualified) {
    for (size_t i = 0; i < h->n; i++) {
        if (h->v[i].offset == off && strcmp(h->v[i].uri, ix->doc->uri) == 0) return;
    }
    if (h->n == h->cap) {
        size_t nc = h->cap ? h->cap * 2 : 32;
        Hit *nv = (Hit *)realloc(h->v, nc * sizeof(Hit));
        if (!nv) return;
        h->v = nv;
        h->cap = nc;
    }
    Hit *x = &h->v[h->n++];
    x->uri = strdup(ix->doc->uri);
    x->path = strdup(ix->doc->path ? ix->doc->path : "");
    x->offset = off;
    x->len = len;
    x->range.start = lsp_offset_to_pos(ix->doc, off);
    x->range.end = lsp_offset_to_pos(ix->doc, off + len);
    x->is_decl = is_decl;
    x->is_write = is_write;
    x->qualified = qualified;
}

static void hits_free(Hits *h) {
    for (size_t i = 0; i < h->n; i++) { free(h->v[i].uri); free(h->v[i].path); }
    free(h->v);
}

static int hit_cmp(const void *x, const void *y) {
    const Hit *a = (const Hit *)x, *b = (const Hit *)y;
    int c = strcmp(a->uri, b->uri);
    if (c) return c;
    return a->offset < b->offset ? -1 : a->offset > b->offset;
}

static void json_range(JsonBuilder *jb, const char *key, LspRange r) {
    jb_key(jb, key);
    jb_start_obj(jb);
    jb_key(jb, "start"); jb_start_obj(jb);
    jb_kv_int(jb, "line", r.start.line); jb_kv_int(jb, "character", r.start.character);
    jb_end_obj(jb);
    jb_key(jb, "end"); jb_start_obj(jb);
    jb_kv_int(jb, "line", r.end.line); jb_kv_int(jb, "character", r.end.character);
    jb_end_obj(jb);
    jb_end_obj(jb);
}

static char *errorf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    return strdup(buf);
}

/* ── reference search ────────────────────────────────────────────────────── */

#define MAX_FAMILY 32

typedef struct {
    LspSymRef keys[MAX_FAMILY];
    size_t nkeys;
    Hits hits;
    bool same_file_only;
} RefSearch;

static bool matches_any(const RefSearch *rs, const LspSymRef *r) {
    for (size_t i = 0; i < rs->nkeys; i++) if (lsp_symref_same(&rs->keys[i], r)) return true;
    return false;
}

static bool is_assign_tok(SnTokKind k) {
    return k == SN_TOK_ASSIGN || k == SN_TOK_PLUS_EQ || k == SN_TOK_MINUS_EQ || k == SN_TOK_STAR_EQ ||
           k == SN_TOK_SLASH_EQ;
}

static bool collect_refs_fn(void *user, LspFileIndex *ix) {
    RefSearch *rs = (RefSearch *)user;
    const LspSymRef *target = &rs->keys[0];
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->kind != SN_TOK_IDENT || !t->text || strcmp(t->text, target->name) != 0) continue;
        /* a parameter's declaration sits before the body that is its scope */
        if (target->cls == LSP_SYMREF_LOCAL && t->offset != target->decl_offset &&
            (t->offset < target->scope_start || t->offset >= target->scope_end)) continue;
        LspSymRef r;
        bool is_decl = false;
        if (!lsp_resolve_at(ix, t->offset, &r, NULL, NULL, &is_decl) || !matches_any(rs, &r)) continue;
        bool is_write = is_decl || (i + 1 < ix->ntoks && is_assign_tok(ix->toks[i + 1].kind) &&
                                    ix->toks[i + 1].in_string == t->in_string);
        hits_add(&rs->hits, ix, t->offset, t->len, is_decl, is_write, lsp_index_token_is_qualified(ix, i));
    }
    return true;
}

/* Resolves the cursor and collects every reference of its symbol family. */
static bool find_references(Search *s, const LspDocument *doc, LspPosition pos, RefSearch *rs,
                            LspDocAnalysis **out_a) {
    memset(rs, 0, sizeof(*rs));
    LspDocAnalysis *a = analysis_of(s, doc);
    if (out_a) *out_a = a;
    if (!a || !a->has_ast) return false;
    LspFileIndex ix;
    lsp_index_build(&ix, a, doc);
    bool ok = lsp_resolve_at(&ix, lsp_pos_to_offset(doc, pos), &rs->keys[0], NULL, NULL, NULL);
    lsp_index_free(&ix);
    if (!ok) return false;
    rs->nkeys = lsp_member_family(a, &rs->keys[0], rs->keys, MAX_FAMILY);
    const char *words[1] = { rs->keys[0].name };
    bool origin_only = rs->same_file_only || rs->keys[0].cls == LSP_SYMREF_LOCAL;
    for_each_file(s, doc, a, origin_only, words, 1, collect_refs_fn, rs);
    qsort(rs->hits.v, rs->hits.n, sizeof(Hit), hit_cmp);
    return true;
}

char *lsp_references_query(LspAnalysisEngine *e, LspDocStore *store, const LspDocument *doc, LspPosition pos,
                           bool include_declaration) {
    if (!doc) return NULL;
    Search s;
    search_init(&s, e, store);
    RefSearch rs;
    bool ok = find_references(&s, doc, pos, &rs, NULL);
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);
    for (size_t i = 0; ok && i < rs.hits.n; i++) {
        const Hit *h = &rs.hits.v[i];
        if (h->is_decl && !include_declaration) continue;
        jb_start_obj(&jb);
        jb_kv_str(&jb, "uri", h->uri);
        json_range(&jb, "range", h->range);
        jb_end_obj(&jb);
    }
    jb_end_arr(&jb);
    hits_free(&rs.hits);
    search_free(&s);
    return jb_take(&jb);
}

char *lsp_document_highlight_query(LspAnalysisEngine *e, LspDocStore *store, const LspDocument *doc, LspPosition pos) {
    if (!doc) return NULL;
    Search s;
    search_init(&s, e, store);
    RefSearch rs;
    memset(&rs, 0, sizeof(rs));
    rs.same_file_only = true;
    LspDocAnalysis *a = analysis_of(&s, doc);
    bool ok = false;
    if (a && a->has_ast) {
        LspFileIndex ix;
        lsp_index_build(&ix, a, doc);
        ok = lsp_resolve_at(&ix, lsp_pos_to_offset(doc, pos), &rs.keys[0], NULL, NULL, NULL);
        if (ok) {
            rs.nkeys = lsp_member_family(a, &rs.keys[0], rs.keys, MAX_FAMILY);
            collect_refs_fn(&rs, &ix);
        }
        lsp_index_free(&ix);
    }
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);
    for (size_t i = 0; ok && i < rs.hits.n; i++) {
        jb_start_obj(&jb);
        json_range(&jb, "range", rs.hits.v[i].range);
        jb_kv_int(&jb, "kind", rs.hits.v[i].is_write ? 3 : 2);
        jb_end_obj(&jb);
    }
    jb_end_arr(&jb);
    hits_free(&rs.hits);
    search_free(&s);
    return jb_take(&jb);
}

/* Implementations: the method family minus the method itself (overrides and
 * interface implementations). Falls back to same-named method declarations
 * when the cursor is not on a resolvable method. */
char *lsp_implementation_query(LspAnalysisEngine *e, LspDocStore *store, const LspDocument *doc, LspPosition pos) {
    if (!doc) return NULL;
    Search s;
    search_init(&s, e, store);
    RefSearch rs;
    bool ok = find_references(&s, doc, pos, &rs, NULL);
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);
    for (size_t i = 0; ok && i < rs.hits.n; i++) {
        const Hit *h = &rs.hits.v[i];
        if (!h->is_decl || rs.keys[0].kind != SN_SYM_METHOD) continue;
        jb_start_obj(&jb);
        jb_kv_str(&jb, "uri", h->uri);
        json_range(&jb, "range", h->range);
        jb_end_obj(&jb);
    }
    jb_end_arr(&jb);
    hits_free(&rs.hits);
    search_free(&s);
    return jb_take(&jb);
}

/* ── rename ──────────────────────────────────────────────────────────────── */

static const char *kind_word(const LspSymRef *r) {
    switch (r->kind) {
        case SN_SYM_TYPE: return "type";
        case SN_SYM_FUNC: return "func";
        case SN_SYM_METHOD: return "method";
        case SN_SYM_FIELD: return "field";
        case SN_SYM_CONST: return "const";
        case SN_SYM_VARIANT: return "variant";
        case SN_SYM_PARAM: return "parameter";
        case SN_SYM_LOCAL: return r->is_mutable ? "var" : "let";
        default: return "symbol";
    }
}

static uint32_t line_of(const LspFileIndex *ix, uint32_t off) { return lsp_offset_to_pos(ix->doc, off).line + 1; }

/* Refuses symbols gopls would refuse: nothing resolved, or declared outside
 * the workspace (builtins and dependencies are not the developer's code). */
static bool rename_target(Search *s, const LspDocument *doc, LspPosition pos, LspSymRef *target,
                          uint32_t *tok_off, uint32_t *tok_len, char **error) {
    LspDocAnalysis *a = analysis_of(s, doc);
    if (!a || !a->has_ast) { *error = errorf("no analysis available for this file"); return false; }
    LspFileIndex ix;
    lsp_index_build(&ix, a, doc);
    uint32_t off = lsp_pos_to_offset(doc, pos);
    bool ok = lsp_resolve_at(&ix, off, target, tok_off, tok_len, NULL);
    long ti = lsp_index_token_at(&ix, off);
    bool keyword = ti >= 0 && ix.toks[ti].kind != SN_TOK_IDENT;
    lsp_index_free(&ix);
    if (!ok) {
        *error = errorf(keyword ? "cannot rename a keyword" : "no renamable symbol at this position");
        return false;
    }
    if (!target->in_workspace) {
        *error = errorf("cannot rename `%s`: it is declared in %s, outside the workspace", target->name,
                        target->package[0] ? target->package : "a builtin or dependency package");
        return false;
    }
    return true;
}

char *lsp_prepare_rename_query(LspAnalysisEngine *e, LspDocStore *store, const LspDocument *doc, LspPosition pos,
                               char **error) {
    *error = NULL;
    if (!doc) return NULL;
    Search s;
    search_init(&s, e, store);
    LspSymRef target;
    uint32_t off = 0, len = 0;
    char *res = NULL;
    if (rename_target(&s, doc, pos, &target, &off, &len, error)) {
        JsonBuilder jb;
        jb_init(&jb);
        jb_start_obj(&jb);
        LspRange r = { lsp_offset_to_pos(doc, off), lsp_offset_to_pos(doc, off + len) };
        json_range(&jb, "range", r);
        jb_kv_str(&jb, "placeholder", target.name);
        jb_end_obj(&jb);
        res = jb_take(&jb);
    }
    search_free(&s);
    return res;
}

typedef struct {
    char *uri;
    LspRange range;
    char *text;
} Edit;

typedef struct {
    RefSearch rs;
    const char *new_name;
    const LspBinding *target_binding; /* copy of the LOCAL binding, origin file */
    LspBinding binding_copy;
    char *conflict;
    Edit *edits;
    size_t nedits, cap;
} RenameCtx;

static void add_edit(RenameCtx *rc, const LspFileIndex *ix, uint32_t off, uint32_t len, const char *text) {
    for (size_t i = 0; i < rc->nedits; i++) {
        LspPosition p = lsp_offset_to_pos(ix->doc, off);
        if (strcmp(rc->edits[i].uri, ix->doc->uri) == 0 && rc->edits[i].range.start.line == p.line &&
            rc->edits[i].range.start.character == p.character) return;
    }
    if (rc->nedits == rc->cap) {
        size_t nc = rc->cap ? rc->cap * 2 : 32;
        Edit *nv = (Edit *)realloc(rc->edits, nc * sizeof(Edit));
        if (!nv) return;
        rc->edits = nv;
        rc->cap = nc;
    }
    Edit *ed = &rc->edits[rc->nedits++];
    ed->uri = strdup(ix->doc->uri);
    ed->range.start = lsp_offset_to_pos(ix->doc, off);
    ed->range.end = lsp_offset_to_pos(ix->doc, off + len);
    ed->text = strdup(text);
}

static const char *find_in(const char *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    for (size_t i = 0; n && i + n <= hay_len; i++) if (memcmp(hay + i, needle, n) == 0) return hay + i;
    return NULL;
}

/* The Doc comment block directly above the line of `off` (decorator lines
 * may sit between); returns its [start, end) offsets. */
static bool docstring_above(const LspDocument *doc, uint32_t off, uint32_t *start, uint32_t *end) {
    const char *t = doc->text;
    size_t line_start = off;
    while (line_start > 0 && t[line_start - 1] != '\n') line_start--;
    size_t p = line_start;
    while (p > 0) {
        size_t le = p - 1; /* the '\n' ending the previous line */
        size_t ls = le;
        while (ls > 0 && t[ls - 1] != '\n') ls--;
        size_t a = ls, b = le;
        while (a < b && (t[a] == ' ' || t[a] == '\t')) a++;
        while (b > a && (t[b - 1] == ' ' || t[b - 1] == '\t' || t[b - 1] == '\r')) b--;
        if (a == b || t[a] == '@') { p = ls; continue; }
        if (b - a >= 2 && t[b - 2] == '*' && t[b - 1] == '/') {
            size_t c = b - 2;
            while (c > 0 && !(t[c - 1] == '/' && t[c] == '*')) c--;
            if (c == 0) return false;
            *start = (uint32_t)(c - 1);
            *end = (uint32_t)b;
            return true;
        }
        return false;
    }
    return false;
}

/* Snova docstrings name their subject: `Doc:{Name}` and `Param{name}`. */
static void rename_docstring_tag(RenameCtx *rc, const LspFileIndex *ix, uint32_t decl_off, const char *tag,
                                 const char *old_name) {
    uint32_t ds, de;
    if (!docstring_above(ix->doc, decl_off, &ds, &de)) return;
    char needle[LSP_IDENT_NAME_MAX + 16];
    snprintf(needle, sizeof(needle), "%s{%s}", tag, old_name);
    const char *hit = find_in(ix->doc->text + ds, de - ds, needle);
    if (!hit) return;
    uint32_t at = (uint32_t)(hit - ix->doc->text) + (uint32_t)strlen(tag) + 1;
    add_edit(rc, ix, at, (uint32_t)strlen(old_name), rc->new_name);
}

static void conflict(RenameCtx *rc, const char *fmt, ...) {
    if (rc->conflict) return;
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    rc->conflict = strdup(buf);
}

/* gopls checkInLexicalScope for a local binding, in its file. */
static void check_local(RenameCtx *rc, const LspFileIndex *ix) {
    const LspSymRef *from = &rc->rs.keys[0];
    const LspBinding *fb = lsp_index_binding_at(ix, from->decl_offset);
    if (!fb) return;
    const char *to = rc->new_name;

    /* same-block conflict */
    for (size_t i = 0; i < ix->nbinds; i++) {
        const LspBinding *b = &ix->binds[i];
        if (b == fb || !b->name || strcmp(b->name, to) != 0) continue;
        if (b->scope_start == fb->scope_start && b->scope_end == fb->scope_end) {
            conflict(rc, "renaming this %s `%s` to `%s` conflicts with the %s declared at line %u in the same block",
                     kind_word(from), from->name, to, b->kind == SN_SYM_PARAM ? "parameter" : "binding",
                     line_of(ix, b->offset));
            return;
        }
    }
    /* sub-block conflict: a deeper `to` would capture a reference */
    for (size_t i = 0; i < rc->rs.hits.n; i++) {
        const Hit *h = &rc->rs.hits.v[i];
        if (h->is_decl || strcmp(h->uri, ix->doc->uri) != 0) continue;
        const LspBinding *b = lsp_index_visible_binding(ix, to, h->offset);
        if (b && b->scope_start > fb->scope_start) {
            conflict(rc, "renaming `%s` to `%s` would make the reference at line %u resolve to the `%s` declared at line %u",
                     from->name, to, h->range.start.line + 1, to, line_of(ix, b->offset));
            return;
        }
    }
    /* super-block conflict: the renamed binding would shadow an outer `to` */
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->kind != SN_TOK_IDENT || !t->text || strcmp(t->text, to) != 0) continue;
        if (lsp_index_token_is_qualified(ix, i) || t->offset < fb->visible_from || t->offset >= fb->scope_end) continue;
        if (lsp_index_binding_at(ix, t->offset)) continue;
        const LspBinding *b = lsp_index_visible_binding(ix, to, t->offset);
        bool inner_wins = b && (b->scope_start > fb->scope_start ||
                                (b->scope_start == fb->scope_start && b->offset > fb->offset));
        if (!inner_wins) {
            conflict(rc, "renaming `%s` to `%s` would shadow the reference to `%s` at line %u",
                     from->name, to, to, line_of(ix, t->offset));
            return;
        }
    }
}

/* Package-level and member renames, per file. */
static void check_nonlocal(RenameCtx *rc, const LspFileIndex *ix) {
    const LspSymRef *from = &rc->rs.keys[0];
    const char *to = rc->new_name;
    const char *file_pkg = (ix->a->unit.package && ix->a->unit.package[0]) ? ix->a->unit.package : "main";

    /* sub-block: an unqualified reference would now find a local or member `to` */
    for (size_t i = 0; i < rc->rs.hits.n; i++) {
        const Hit *h = &rc->rs.hits.v[i];
        if (h->is_decl || h->qualified || strcmp(h->uri, ix->doc->uri) != 0) continue;
        long ti = lsp_index_token_at(ix, h->offset);
        if (ti >= 0 && ix->toks[ti].in_import) continue;
        LspSymRef r;
        if (!lsp_lookup_name_at(ix, to, h->offset, &r)) continue;
        bool shadows = r.cls == LSP_SYMREF_LOCAL ||
                       (from->cls == LSP_SYMREF_GLOBAL && r.cls == LSP_SYMREF_MEMBER);
        if (shadows) {
            conflict(rc, "renaming `%s` to `%s` would make the reference at line %u resolve to the %s `%s` instead",
                     from->name, to, h->range.start.line + 1, kind_word(&r), to);
            return;
        }
    }
    /* super-block: existing `to` references the renamed symbol would capture */
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->kind != SN_TOK_IDENT || !t->text || strcmp(t->text, to) != 0) continue;
        if (lsp_index_token_is_qualified(ix, i) || t->in_import || t->in_package) continue;
        LspSymRef r;
        bool is_decl = false;
        if (!lsp_resolve_at(ix, t->offset, &r, NULL, NULL, &is_decl) || is_decl) continue;
        if (from->cls == LSP_SYMREF_GLOBAL && r.cls == LSP_SYMREF_GLOBAL &&
            strcmp(file_pkg, from->package) == 0 && strcmp(r.package, from->package) != 0) {
            conflict(rc, "renaming `%s` to `%s` would shadow `%s` from %s used at %s:%u",
                     from->name, to, to, r.package, ix->doc->path ? ix->doc->path : ix->doc->uri,
                     line_of(ix, t->offset));
            return;
        }
        if (from->cls == LSP_SYMREF_MEMBER && r.cls == LSP_SYMREF_GLOBAL) {
            const SnDecl *td = lsp_index_enclosing_type(ix, t->offset);
            for (size_t k = 0; td && td->name && k < rc->rs.nkeys; k++) {
                if (strcmp(td->name, rc->rs.keys[k].owner) == 0) {
                    conflict(rc, "renaming `%s.%s` to `%s` would shadow the %s `%s` used at line %u",
                             from->owner, from->name, to, kind_word(&r), to, line_of(ix, t->offset));
                    return;
                }
            }
        }
    }
}

static bool rename_fn(void *user, LspFileIndex *ix) {
    RenameCtx *rc = (RenameCtx *)user;
    size_t before = rc->rs.hits.n;
    collect_refs_fn(&rc->rs, ix);
    const LspSymRef *from = &rc->rs.keys[0];

    if (from->cls == LSP_SYMREF_LOCAL) check_local(rc, ix);
    else check_nonlocal(rc, ix);
    if (rc->conflict) return false;

    for (size_t i = before; i < rc->rs.hits.n; i++) {
        const Hit *h = &rc->rs.hits.v[i];
        add_edit(rc, ix, h->offset, h->len, rc->new_name);
        if (h->is_decl && from->cls != LSP_SYMREF_LOCAL) rename_docstring_tag(rc, ix, h->offset, "Doc:", from->name);
        if (h->is_decl && from->kind == SN_SYM_PARAM) {
            const LspBinding *b = lsp_index_binding_at(ix, h->offset);
            if (b && b->routine) {
                uint32_t r0, r1, sel, sel_len;
                lsp_decl_outline(ix->doc, b->routine, &r0, &r1, &sel, &sel_len);
                rename_docstring_tag(rc, ix, sel, "Param", from->name);
            }
        }
    }
    return true;
}

/* gopls rename_check: names already taken where the symbol is declared. */
static bool check_declaration_space(const LspDocAnalysis *a, RenameCtx *rc) {
    const LspSymRef *from = &rc->rs.keys[0];
    const char *to = rc->new_name;
    const char *ito = sn_intern_cstr((SnInternTable *)&a->intern, to);
    if (from->cls == LSP_SYMREF_GLOBAL) {
        SnScope *ps = sn_resolver_package_scope(&a->resolver,
                                                sn_intern_cstr((SnInternTable *)&a->intern, from->package));
        SnSymbol *prev = ps ? sn_scope_lookup_local(ps, ito) : NULL;
        if (prev) {
            conflict(rc, "renaming this %s `%s` to `%s` conflicts with the %s `%s` already declared in package %s",
                     kind_word(from), from->name, to, prev->kind == SN_SYM_TYPE ? "type" : "declaration", to,
                     from->package);
            return false;
        }
    } else if (from->cls == LSP_SYMREF_MEMBER) {
        for (size_t k = 0; k < rc->rs.nkeys; k++) {
            const SnScope *ms = lsp_type_member_scope(a, rc->rs.keys[k].package, rc->rs.keys[k].owner);
            if (ms && sn_scope_lookup_local(ms, ito)) {
                conflict(rc, "renaming %s `%s.%s` to `%s` conflicts with the member `%s.%s`", kind_word(from),
                         rc->rs.keys[k].owner, from->name, to, rc->rs.keys[k].owner, to);
                return false;
            }
        }
    }
    return true;
}

static int edit_cmp(const void *x, const void *y) {
    const Edit *a = (const Edit *)x, *b = (const Edit *)y;
    int c = strcmp(a->uri, b->uri);
    if (c) return c;
    if (a->range.start.line != b->range.start.line) return a->range.start.line < b->range.start.line ? -1 : 1;
    return a->range.start.character < b->range.start.character ? -1 : a->range.start.character > b->range.start.character;
}

char *lsp_rename_query(LspAnalysisEngine *e, LspDocStore *store, const LspDocument *doc, LspPosition pos,
                       const char *new_name, char **error) {
    *error = NULL;
    if (!doc) return NULL;
    if (!lsp_is_valid_identifier(new_name)) {
        *error = errorf("`%s` is not a valid Snovalang identifier", new_name ? new_name : "");
        return NULL;
    }
    Search s;
    search_init(&s, e, store);
    RenameCtx rc;
    memset(&rc, 0, sizeof(rc));
    rc.new_name = new_name;
    char *res = NULL;
    uint32_t off, len;

    if (!rename_target(&s, doc, pos, &rc.rs.keys[0], &off, &len, error)) goto done;
    LspDocAnalysis *a = analysis_of(&s, doc);
    const LspSymRef *from = &rc.rs.keys[0];
    if (strcmp(from->name, new_name) == 0) {
        res = strdup("{\"changes\":{}}");
        goto done;
    }
    rc.rs.nkeys = lsp_member_family(a, from, rc.rs.keys, MAX_FAMILY);
    if (!check_declaration_space(a, &rc)) {
        *error = rc.conflict;
        rc.conflict = NULL;
        goto done;
    }
    {
        const char *words[2] = { from->name, new_name };
        for_each_file(&s, doc, a, from->cls == LSP_SYMREF_LOCAL, words, 2, rename_fn, &rc);
    }
    if (rc.conflict) {
        *error = rc.conflict;
        rc.conflict = NULL;
        goto done;
    }

    qsort(rc.edits, rc.nedits, sizeof(Edit), edit_cmp);
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_obj(&jb);
    jb_key(&jb, "changes");
    jb_start_obj(&jb);
    for (size_t i = 0; i < rc.nedits;) {
        jb_key(&jb, rc.edits[i].uri);
        jb_start_arr(&jb);
        size_t j = i;
        for (; j < rc.nedits && strcmp(rc.edits[j].uri, rc.edits[i].uri) == 0; j++) {
            jb_start_obj(&jb);
            json_range(&jb, "range", rc.edits[j].range);
            jb_kv_str(&jb, "newText", rc.edits[j].text);
            jb_end_obj(&jb);
        }
        jb_end_arr(&jb);
        i = j;
    }
    jb_end_obj(&jb);
    jb_end_obj(&jb);
    res = jb_take(&jb);

done:
    for (size_t i = 0; i < rc.nedits; i++) { free(rc.edits[i].uri); free(rc.edits[i].text); }
    free(rc.edits);
    free(rc.conflict);
    hits_free(&rc.rs.hits);
    search_free(&s);
    return res;
}

/* ── workspace symbols ───────────────────────────────────────────────────── */

typedef struct {
    char *name;      /* label shown: `Name` or `Type.member` */
    char *container; /* package */
    int kind;
    float score;
    char *path;
    const SnDecl *decl;
} WsSym;

static int symbol_kind(const SnDecl *d, SnSymbolKind k) {
    if (d) {
        switch (d->kind) {
            case SN_DECL_CLASS: return 5;
            case SN_DECL_STRUCT: return 23;
            case SN_DECL_ENUM: return 10;
            case SN_DECL_INTERFACE: return 11;
            case SN_DECL_FUNC: return 12;
            case SN_DECL_METHOD: return 6;
            case SN_DECL_FIELD: return 8;
            case SN_DECL_CONST: return 14;
            case SN_DECL_VARIANT: return 22;
            case SN_DECL_TYPEALIAS: return 5;
            default: break;
        }
    }
    return k == SN_SYM_FUNC ? 12 : 13;
}

static int ws_cmp(const void *x, const void *y) {
    const WsSym *a = (const WsSym *)x, *b = (const WsSym *)y;
    if (a->score != b->score) return a->score > b->score ? -1 : 1;
    int c = strcmp(a->name, b->name);
    return c ? c : strcmp(a->container, b->container);
}

char *lsp_workspace_symbol_query(LspAnalysisEngine *e, LspDocStore *store, const char *query) {
    Search s;
    search_init(&s, e, store);
    const LspDocAnalysis *a = e->analyses;
    for (size_t i = 0; !a && i < store->len; i++) {
        if (store->docs[i]->path && lsp_is_snovalang_source(store->docs[i]->path)) a = analysis_of(&s, store->docs[i]);
    }

    WsSym *syms = NULL;
    size_t n = 0, cap = 0;
    LspFuzzyMatcher *m = lsp_fuzzy_new(query ? query : "");
    for (const SnPackageScopeEntry *pe = a ? a->resolver.packages : NULL; pe; pe = pe->next) {
        const SnPackageNode *node = sn_pkggraph_find((SnPackageGraph *)&a->graph, pe->package_name);
        if (!pe->scope || (node && lsp_package_origin(node) != LSP_PKG_PROJECT)) continue;
        for (size_t bk = 0; bk < pe->scope->nbuckets; bk++) {
            for (SnSymbol *sym = pe->scope->buckets[bk]; sym; sym = sym->next) {
                if (!sym->name || !sym->decl || !sym->origin || !sym->origin->path) continue;
                /* top-level symbol, then its members as `Type.member` */
                for (size_t j = 0; j <= sym->decl->members.len; j++) {
                    const SnDecl *d = j == 0 ? sym->decl : SN_LIST_AT(sym->decl->members, SnDecl, j - 1);
                    if (!d || !d->name) continue;
                    char label[2 * LSP_IDENT_NAME_MAX + 2];
                    if (j == 0) snprintf(label, sizeof(label), "%s", d->name);
                    else snprintf(label, sizeof(label), "%s.%s", sym->name, d->name);
                    float sc = lsp_fuzzy_score(m, label);
                    if (sc <= 0.0f) continue;
                    if (n == cap) {
                        cap = cap ? cap * 2 : 64;
                        WsSym *nv = (WsSym *)realloc(syms, cap * sizeof(WsSym));
                        if (!nv) break;
                        syms = nv;
                    }
                    syms[n++] = (WsSym){ strdup(label), strdup(pe->package_name), symbol_kind(d, sym->kind), sc,
                                         strdup(sym->origin->path), d };
                }
            }
        }
    }
    lsp_fuzzy_free(m);
    qsort(syms, n, sizeof(WsSym), ws_cmp);

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);
    for (size_t i = 0; i < n && i < 100; i++) {
        LspDocument *doc = doc_for_path(&s, syms[i].path);
        char *uri = doc ? strdup(doc->uri) : NULL;
        if (!doc || !uri) { free(uri); continue; }
        uint32_t r0, r1, sel, sel_len;
        lsp_decl_outline(doc, syms[i].decl, &r0, &r1, &sel, &sel_len);
        /* decl spans come from the disk parse; trust them only if the name is there */
        if (sel + sel_len > doc->text_len || strncmp(doc->text + sel, syms[i].decl->name, sel_len) != 0) {
            sel = syms[i].decl->span.offset;
            sel_len = syms[i].decl->span.len;
        }
        LspRange r = { lsp_offset_to_pos(doc, sel), lsp_offset_to_pos(doc, sel + sel_len) };
        jb_start_obj(&jb);
        jb_kv_str(&jb, "name", syms[i].name);
        jb_kv_int(&jb, "kind", syms[i].kind);
        jb_kv_str(&jb, "containerName", syms[i].container);
        jb_key(&jb, "location");
        jb_start_obj(&jb);
        jb_kv_str(&jb, "uri", uri);
        json_range(&jb, "range", r);
        jb_end_obj(&jb);
        jb_end_obj(&jb);
        free(uri);
    }
    jb_end_arr(&jb);
    for (size_t i = 0; i < n; i++) { free(syms[i].name); free(syms[i].container); free(syms[i].path); }
    free(syms);
    search_free(&s);
    return jb_take(&jb);
}
