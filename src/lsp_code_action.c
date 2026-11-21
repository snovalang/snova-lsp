#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "lsp_code_action.h"
#include "lsp_imports.h"
#include "lsp_ident.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

typedef struct {
    LspRange range;
    char *new_text;
} ActionEdit;

typedef struct {
    char *title;
    char *kind; /* "quickfix", "source.organizeImports", "refactor.*" */
    bool is_preferred;
    char *doc_uri;
    ActionEdit *edits; /* all in doc_uri, non-overlapping */
    size_t nedits;
} ActionItem;

typedef struct {
    ActionItem *items;
    size_t len;
    size_t cap;
} ActionList;

static void actionlist_init(ActionList *list) {
    list->len = 0;
    list->cap = 16;
    list->items = (ActionItem *)malloc(sizeof(ActionItem) * list->cap);
}

static void actionlist_free(ActionList *list) {
    if (!list || !list->items) return;
    for (size_t i = 0; i < list->len; i++) {
        free(list->items[i].title);
        free(list->items[i].kind);
        free(list->items[i].doc_uri);
        for (size_t e = 0; e < list->items[i].nedits; e++) free(list->items[i].edits[e].new_text);
        free(list->items[i].edits);
    }
    free(list->items);
    list->items = NULL;
    list->len = 0;
    list->cap = 0;
}

static ActionItem *actionlist_new(ActionList *list, const char *title, const char *kind,
                                  bool is_preferred, const char *uri) {
    if (!list || !title) return NULL;
    if (list->len >= list->cap) {
        list->cap *= 2;
        list->items = (ActionItem *)realloc(list->items, sizeof(ActionItem) * list->cap);
    }
    ActionItem *ai = &list->items[list->len++];
    ai->title = strdup(title);
    ai->kind = kind ? strdup(kind) : strdup("quickfix");
    ai->is_preferred = is_preferred;
    ai->doc_uri = uri ? strdup(uri) : NULL;
    ai->edits = NULL;
    ai->nedits = 0;
    return ai;
}

static void action_edit(ActionItem *ai, LspRange range, const char *new_text) {
    if (!ai || !new_text) return;
    ActionEdit *ne = (ActionEdit *)realloc(ai->edits, sizeof(ActionEdit) * (ai->nedits + 1));
    if (!ne) return;
    ai->edits = ne;
    ai->edits[ai->nedits].range = range;
    ai->edits[ai->nedits].new_text = strdup(new_text);
    ai->nedits++;
}

static void actionlist_add(ActionList *list, const char *title, const char *kind,
                           bool is_preferred, const char *uri, LspRange range, const char *new_text) {
    if (!new_text) return;
    action_edit(actionlist_new(list, title, kind, is_preferred, uri), range, new_text);
}

static void extract_token_text_at_range(const LspDocument *doc, LspRange range, char *out_buf, size_t out_sz) {
    out_buf[0] = '\0';
    if (!doc || !doc->text) return;

    uint32_t start_off = lsp_pos_to_offset(doc, range.start);
    uint32_t end_off = lsp_pos_to_offset(doc, range.end);
    if (end_off <= start_off || end_off > doc->text_len) return;

    size_t len = end_off - start_off;
    if (len >= out_sz) len = out_sz - 1;
    memcpy(out_buf, doc->text + start_off, len);
    out_buf[len] = '\0';
}

typedef struct {
    const char *path;
    int score;
    LspPackageOrigin origin;
} ImportCandidate;

/*
 * Quick fixes for a symbol that is declared in another package but not
 * imported. Candidates are ranked by the developer's project context (see
 * lsp_import_context_score); only the best one is marked preferred, so
 * "apply preferred fix" picks the package the project already relies on.
 */
static void add_auto_import_actions(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, const char *sym_name) {
    if (!a || !doc || !sym_name || !sym_name[0]) return;

    ImportCandidate found[32];
    size_t count = 0;
    const char *iname = sn_intern_cstr((SnInternTable *)&a->intern, sym_name);

    for (SnPackageScopeEntry *pe = a->resolver.packages; pe && count < 32; pe = pe->next) {
        if (!pe->package_name || !pe->scope) continue;
        SnPackageNode *package = sn_pkggraph_find((SnPackageGraph *)&a->graph, pe->package_name);
        if (!package || !package->name) continue;
        const char *import_path = package->name;
        if (a->unit.package && strcmp(import_path, a->unit.package) == 0) continue;
        if (lsp_import_present(a, import_path)) continue;

        SnSymbol *sym = sn_scope_lookup_local(pe->scope, iname);
        if (!sym) continue;
        if (sym->kind != SN_SYM_TYPE && sym->kind != SN_SYM_FUNC &&
            sym->kind != SN_SYM_CONST && sym->kind != SN_SYM_VARIANT) continue;
        if (sym->decl && sym->decl->vis != SN_VIS_PUBLIC && sym->decl->vis != SN_VIS_DEFAULT) continue;

        bool duplicate = false;
        for (size_t i = 0; i < count; i++) {
            if (strcmp(found[i].path, import_path) == 0) duplicate = true;
        }
        if (duplicate) continue;
        found[count].path = import_path;
        found[count].score = lsp_import_context_score(a, import_path);
        found[count].origin = lsp_package_origin(package);
        count++;
    }

    /* Highest context score first; ties keep the shorter, then alphabetical path. */
    for (size_t i = 1; i < count; i++) {
        ImportCandidate key = found[i];
        size_t j = i;
        while (j > 0) {
            const ImportCandidate *prev = &found[j - 1];
            bool before = key.score > prev->score ||
                (key.score == prev->score && (strlen(key.path) < strlen(prev->path) ||
                 (strlen(key.path) == strlen(prev->path) && strcmp(key.path, prev->path) < 0)));
            if (!before) break;
            found[j] = found[j - 1];
            j--;
        }
        found[j] = key;
    }

    for (size_t i = 0; i < count; i++) {
        LspImportEdit edit;
        if (!lsp_import_edit(doc, found[i].path, &edit)) continue;
        const char *where = found[i].origin == LSP_PKG_PROJECT ? "project" :
                            found[i].origin == LSP_PKG_DEPENDENCY ? "dependency" : "builtin";
        char title[SNOVAC_PATH_MAX + 64];
        snprintf(title, sizeof(title), "Import '%s' (%s)", found[i].path, where);
        LspRange range = { .start = edit.pos, .end = edit.pos };
        actionlist_add(list, title, "quickfix", i == 0, doc->uri, range, edit.text);
    }
}

/* ── Organize imports (goimports) ────────────────────────────────────────────
 *
 * goimports removes unused imports, adds missing ones, and sorts them into
 * groups (standard library first). Snova version:
 *   - an import of a known package is unused when no identifier of the file
 *     names a symbol declared in that package (goimports' selector test,
 *     conservative: an unknown package is always kept); `import pkg.Name` is
 *     unused when `Name` does not occur;
 *   - names the compiler reports as undeclared or not imported get an import
 *     when one package is the clear best, by the same project-context score
 *     the auto-import completion uses;
 *   - groups: builtin, dependencies, project; sorted, a blank line between
 *     groups, the block's majority semicolon style and line ending.
 */

typedef struct {
    char path[SNOVAC_PATH_MAX];
    int group; /* 0 builtin, 1 dependency, 2 project/unknown */
} OrgImport;

static int import_group(const LspDocAnalysis *a, const char *path) {
    if (strncmp(path, "builtin.", 8) == 0 || strcmp(path, "builtin") == 0) return 0;
    if (!a) return 2;
    char pkg[SNOVAC_PATH_MAX];
    snprintf(pkg, sizeof(pkg), "%s", path);
    for (;;) {
        SnPackageNode *node = sn_pkggraph_find((SnPackageGraph *)&a->graph, pkg);
        if (node) {
            LspPackageOrigin o = lsp_package_origin(node);
            return o == LSP_PKG_BUILTIN ? 0 : o == LSP_PKG_DEPENDENCY ? 1 : 2;
        }
        char *dot = strrchr(pkg, '.');
        if (!dot) return 2;
        *dot = '\0';
    }
}

static int org_cmp(const void *x, const void *y) {
    const OrgImport *a = (const OrgImport *)x, *b = (const OrgImport *)y;
    if (a->group != b->group) return a->group - b->group;
    return strcmp(a->path, b->path);
}

/* True when an identifier token outside import/package lines spells `name`. */
static bool file_mentions(const LspFileIndex *ix, const char *name) {
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->kind == SN_TOK_IDENT && !t->in_import && !t->in_package && t->text && strcmp(t->text, name) == 0) return true;
    }
    return false;
}

static bool import_used(const LspDocAnalysis *a, const LspFileIndex *ix, const char *path) {
    SnScope *ps = sn_resolver_package_scope(&a->resolver, sn_intern_cstr((SnInternTable *)&a->intern, path));
    if (ps) {
        for (size_t b = 0; b < ps->nbuckets; b++) {
            for (SnSymbol *sym = ps->buckets[b]; sym; sym = sym->next) {
                if (sym->name && file_mentions(ix, sym->name)) return true;
            }
        }
        return false;
    }
    const char *dot = strrchr(path, '.');
    if (dot) {
        char pkg[SNOVAC_PATH_MAX];
        snprintf(pkg, sizeof(pkg), "%.*s", (int)(dot - path), path);
        SnScope *qs = sn_resolver_package_scope(&a->resolver, sn_intern_cstr((SnInternTable *)&a->intern, pkg));
        if (qs && sn_scope_lookup_local(qs, sn_intern_cstr((SnInternTable *)&a->intern, dot + 1))) {
            return file_mentions(ix, dot + 1);
        }
    }
    return true; /* unknown to the graph: keep it */
}

/* Best package for an undeclared `name`, or NULL when none or ambiguous. */
static const char *best_import_for(const LspDocAnalysis *a, const char *name) {
    const char *best = NULL;
    int best_score = -1, second = -1;
    const char *iname = sn_intern_cstr((SnInternTable *)&a->intern, name);
    for (SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
        if (!pe->package_name || !pe->scope) continue;
        if (a->unit.package && strcmp(pe->package_name, a->unit.package) == 0) continue;
        if (!sn_pkggraph_find((SnPackageGraph *)&a->graph, pe->package_name)) continue;
        SnSymbol *sym = sn_scope_lookup_local(pe->scope, iname);
        if (!sym || (sym->kind != SN_SYM_TYPE && sym->kind != SN_SYM_FUNC &&
                     sym->kind != SN_SYM_CONST && sym->kind != SN_SYM_VARIANT)) continue;
        if (sym->decl && sym->decl->vis != SN_VIS_PUBLIC && sym->decl->vis != SN_VIS_DEFAULT) continue;
        int score = lsp_import_context_score(a, pe->package_name);
        if (score > best_score) { second = best_score; best_score = score; best = pe->package_name; }
        else if (score > second) second = score;
    }
    return best && best_score > second ? best : NULL;
}

static void add_organize_imports_action(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc) {
    if (!doc || !doc->text) return;
    const char *nl = memchr(doc->text, '\r', doc->text_len) ? "\r\n" : "\n";

    OrgImport *imps = NULL;
    size_t n = 0, cap = 0;
    bool found = false;
    uint32_t first_line = 0, last_line = 0, line = 0;
    int with_semi = 0, without_semi = 0;
    char comments[2048] = "";

    const char *p = doc->text, *end = doc->text + doc->text_len;
    while (p < end) {
        const char *ls = p;
        while (p < end && *p != '\n') p++;
        const char *le = p;
        if (p < end) p++;
        while (ls < le && (*ls == ' ' || *ls == '\t')) ls++;
        while (le > ls && (le[-1] == '\r' || le[-1] == ' ' || le[-1] == '\t')) le--;
        size_t len = (size_t)(le - ls);
        if (len > 7 && strncmp(ls, "import", 6) == 0 && (ls[6] == ' ' || ls[6] == '\t')) {
            if (!found) first_line = line;
            found = true;
            last_line = line;
            const char *q = ls + 7;
            while (q < le && (*q == ' ' || *q == '\t')) q++;
            const char *qe = q;
            while (qe < le && *qe != ';' && *qe != ' ' && *qe != '\t' && *qe != '/') qe++;
            if (memchr(ls, ';', len)) with_semi++; else without_semi++;
            if (qe > q && (size_t)(qe - q) < SNOVAC_PATH_MAX) {
                if (n == cap) {
                    cap = cap ? cap * 2 : 16;
                    imps = (OrgImport *)realloc(imps, cap * sizeof(OrgImport));
                }
                snprintf(imps[n].path, sizeof(imps[n].path), "%.*s", (int)(qe - q), q);
                n++;
            }
        } else if (found && len >= 2 && ls[0] == '/' && (ls[1] == '/' || ls[1] == '*')) {
            /* comment inside the import block: keep it above the block */
            size_t used = strlen(comments);
            snprintf(comments + used, sizeof(comments) - used, "%.*s%s", (int)len, ls, nl);
        } else if (len > 0 && !(len >= 7 && strncmp(ls, "package", 7) == 0) && !(ls[0] == '/' || ls[0] == '*')) {
            if (found) break; /* first declaration ends the import block */
        }
        line++;
    }

    LspFileIndex ix;
    memset(&ix, 0, sizeof(ix));
    if (a && a->has_ast) lsp_index_build(&ix, a, doc);

    /* drop unused and duplicate imports */
    size_t kept = 0;
    for (size_t i = 0; i < n; i++) {
        bool dup = false;
        for (size_t k = 0; k < kept; k++) if (strcmp(imps[k].path, imps[i].path) == 0) dup = true;
        if (dup) continue;
        if (a && a->has_ast && !import_used(a, &ix, imps[i].path)) continue;
        imps[kept++] = imps[i];
    }
    n = kept;

    /* add missing imports: unqualified identifiers that resolve to nothing
     * (goimports works from unresolved references, not from diagnostics,
     * which also covers unsaved buffers the compiler has not checked) */
    if (a && a->has_ast) {
        for (size_t ti = 0; ti < ix.ntoks; ti++) {
            const LspTok *t = &ix.toks[ti];
            if (t->kind != SN_TOK_IDENT || !t->text || t->in_import || t->in_package) continue;
            if (lsp_index_token_is_qualified(&ix, ti) || sn_builtin_is_primitive_name(t->text)) continue;
            if (ti > 0 && ix.toks[ti - 1].kind == SN_TOK_AT) continue; /* decorator */
            LspSymRef r;
            if (lsp_resolve_at(&ix, t->offset, &r, NULL, NULL, NULL)) continue;
            const char *pkg = best_import_for(a, t->text);
            if (!pkg) continue;
            bool have = false;
            for (size_t k = 0; k < n; k++) {
                size_t pl = strlen(pkg);
                if (strncmp(imps[k].path, pkg, pl) == 0 && (imps[k].path[pl] == '\0' || imps[k].path[pl] == '.')) have = true;
            }
            if (have) continue;
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                imps = (OrgImport *)realloc(imps, cap * sizeof(OrgImport));
            }
            snprintf(imps[n].path, sizeof(imps[n].path), "%s", pkg);
            n++;
        }
    }
    lsp_index_free(&ix);

    if (!found && n == 0) { free(imps); return; }
    for (size_t i = 0; i < n; i++) imps[i].group = import_group(a, imps[i].path);
    qsort(imps, n, sizeof(OrgImport), org_cmp);

    const char *semi = with_semi > without_semi ? ";" : "";
    size_t out_cap = strlen(comments) + n * (SNOVAC_PATH_MAX + 16) + 16;
    char *out = (char *)malloc(out_cap);
    size_t used = (size_t)snprintf(out, out_cap, "%s", comments);
    for (size_t i = 0; i < n; i++) {
        if (i > 0 && imps[i].group != imps[i - 1].group) used += (size_t)snprintf(out + used, out_cap - used, "%s", nl);
        used += (size_t)snprintf(out + used, out_cap - used, "import %s%s%s", imps[i].path, semi, nl);
    }

    LspRange range;
    if (found) {
        range.start = (LspPosition){ first_line, 0 };
        range.end = (LspPosition){ last_line + 1, 0 };
        if (last_line + 1 >= doc->line_count) {
            /* block reaches the end of the file without a final newline */
            range.end = lsp_offset_to_pos(doc, (uint32_t)doc->text_len);
        }
    } else {
        /* no import block yet: after the package line */
        LspImportEdit ie;
        if (!lsp_import_edit(doc, imps[0].path, &ie)) { free(out); free(imps); return; }
        range.start = range.end = ie.pos;
        char *prefixed = (char *)malloc(out_cap + 4);
        snprintf(prefixed, out_cap + 4, "%s%s", ie.text[0] == '\n' || ie.text[0] == '\r' ? nl : "", out);
        free(out);
        out = prefixed;
    }

    uint32_t s_off = lsp_pos_to_offset(doc, range.start), e_off = lsp_pos_to_offset(doc, range.end);
    bool unchanged = found && e_off >= s_off && strlen(out) == e_off - s_off &&
                     strncmp(doc->text + s_off, out, e_off - s_off) == 0;
    if (!unchanged) actionlist_add(list, "Organize Imports", "source.organizeImports", true, doc->uri, range, out);
    free(out);
    free(imps);
}

static void add_generate_constructor_action(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, LspPosition pos) {
    if (!a || !doc || !a->has_ast) return;
    uint32_t offset = lsp_pos_to_offset(doc, pos);
    const SnDecl *decl = lsp_find_decl_at(a, offset);
    if (decl && decl->kind == SN_DECL_FIELD) {
        for (size_t i = 0; i < a->unit.decls.len; i++) {
            const SnDecl *top = SN_LIST_AT(a->unit.decls, const SnDecl, i);
            if (top && (top->kind == SN_DECL_CLASS || top->kind == SN_DECL_STRUCT)) {
                for (size_t j = 0; j < top->members.len; j++) {
                    if (SN_LIST_AT(top->members, const SnDecl, j) == decl) {
                        decl = top;
                        break;
                    }
                }
            }
        }
    }
    if (!decl || (decl->kind != SN_DECL_CLASS && decl->kind != SN_DECL_STRUCT)) return;

    // Check if class already has a constructor
    bool has_ctor = false;
    for (size_t i = 0; i < decl->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(decl->members, const SnDecl, i);
        if (m && m->name && (strcmp(m->name, "constructor") == 0 || strcmp(m->name, "new") == 0)) {
            has_ctor = true;
            break;
        }
    }
    if (has_ctor) return;

    // Collect fields
    char params_buf[512] = "";
    char body_buf[1024] = "";
    bool first = true;

    for (size_t i = 0; i < decl->members.len; i++) {
        const SnDecl *m = SN_LIST_AT(decl->members, const SnDecl, i);
        if (m && m->kind == SN_DECL_FIELD && m->name) {
            char ftype[128] = "any";
            if (m->type && m->type->kind == SN_TYPE_NAME && m->type->name) {
                snprintf(ftype, sizeof(ftype), "%s", m->type->name);
            }
            if (!first) {
                strcat(params_buf, ", ");
            }
            char param_part[128];
            snprintf(param_part, sizeof(param_part), "%s: %s", m->name, ftype);
            strcat(params_buf, param_part);

            char assign_part[256];
            snprintf(assign_part, sizeof(assign_part), "        this.%s = %s;\n", m->name, m->name);
            strcat(body_buf, assign_part);
            first = false;
        }
    }

    if (!first) {
        char ctor_code[2048];
        snprintf(ctor_code, sizeof(ctor_code),
                 "\n    constructor(%s) {\n%s    }\n",
                 params_buf, body_buf);

        // Insertion position: after the class declaration header
        uint32_t ins_line = (decl->span.line > 0) ? decl->span.line : pos.line + 1;
        LspRange ins_range = {
            .start = { .line = ins_line, .character = 0 },
            .end = { .line = ins_line, .character = 0 }
        };

        char title[128];
        snprintf(title, sizeof(title), "Generate constructor for '%s'", decl->name ? decl->name : "Type");
        actionlist_add(list, title, "refactor.generate.constructor", false, doc->uri, ins_range, ctor_code);
    }
}

/* ── Extract / inline local variable (gopls refactor.extract.variable,
 *    refactor.inline.variable) ──────────────────────────────────────────── */

static bool is_assign_kind(SnTokKind k) {
    return k == SN_TOK_ASSIGN || k == SN_TOK_PLUS_EQ || k == SN_TOK_MINUS_EQ || k == SN_TOK_STAR_EQ || k == SN_TOK_SLASH_EQ;
}

static uint32_t line_start_of(const LspDocument *doc, uint32_t off) {
    while (off > 0 && doc->text[off - 1] != '\n') off--;
    return off;
}

static const char *doc_newline(const LspDocument *doc) {
    return memchr(doc->text, '\r', doc->text_len) ? "\r\n" : "\n";
}

/* Statement the selection belongs to: the innermost statement list whose
 * block contains `off`, and in it the last statement starting at or before. */
static const SnStmt *anchor_in_list(const LspFileIndex *ix, const SnList *stmts, uint32_t off, int depth);

static const SnStmt *anchor_in_child(const LspFileIndex *ix, const SnStmt *child, uint32_t off, int depth) {
    if (!child || depth > 64) return NULL;
    if (child->kind == SN_STMT_BLOCK) {
        for (size_t b = 0; b < ix->nblocks; b++) {
            if (ix->blocks[b].open == child->span.offset && off > ix->blocks[b].open && off < ix->blocks[b].close) {
                return anchor_in_list(ix, &child->stmts, off, depth + 1);
            }
        }
        return NULL;
    }
    return NULL;
}

static const SnStmt *anchor_in_list(const LspFileIndex *ix, const SnList *stmts, uint32_t off, int depth) {
    const SnStmt *found = NULL;
    for (size_t i = 0; i < stmts->len; i++) {
        const SnStmt *st = SN_LIST_AT(*stmts, SnStmt, i);
        if (st && st->span.offset <= off) found = st;
    }
    if (!found) return NULL;
    const SnStmt *children[4] = { found->then_br, found->else_br, found->finally_br, NULL };
    for (int c = 0; c < 3; c++) {
        const SnStmt *deeper = anchor_in_child(ix, children[c], off, depth);
        if (deeper) return deeper;
        /* `else if`: the else branch is itself an IF statement */
        if (children[c] && children[c]->kind == SN_STMT_IF && children[c]->span.offset <= off) {
            const SnStmt *inner = anchor_in_child(ix, children[c]->then_br, off, depth);
            if (!inner) inner = anchor_in_child(ix, children[c]->else_br, off, depth);
            if (inner) return inner;
        }
    }
    for (size_t i = 0; i < found->catches.len; i++) {
        const SnStmt *deeper = anchor_in_child(ix, SN_LIST_AT(found->catches, SnStmt, i), off, depth);
        if (deeper) return deeper;
    }
    for (size_t i = 0; i < found->arms.len; i++) {
        const SnMatchArm *arm = SN_LIST_AT(found->arms, SnMatchArm, i);
        const SnStmt *deeper = arm ? anchor_in_child(ix, arm->body, off, depth) : NULL;
        if (deeper) return deeper;
    }
    return found;
}

static const SnDecl *routine_at(const LspDocAnalysis *a, uint32_t off) {
    const SnDecl *found = NULL;
    for (size_t i = 0; i < a->unit.decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(a->unit.decls, SnDecl, i);
        if (!d || d->span.offset > off) continue;
        if (d->kind == SN_DECL_FUNC) found = d;
        for (size_t j = 0; j < d->members.len; j++) {
            const SnDecl *m = SN_LIST_AT(d->members, SnDecl, j);
            if (m && m->kind == SN_DECL_METHOD && m->span.offset <= off) found = m;
        }
    }
    return found && found->body ? found : NULL;
}

static bool name_taken(const LspFileIndex *ix, const char *name, uint32_t from, uint32_t to, uint32_t anchor) {
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->offset >= from && t->offset < to && t->kind == SN_TOK_IDENT && t->text && strcmp(t->text, name) == 0) return true;
    }
    LspSymRef r;
    return lsp_lookup_name_at(ix, name, anchor, &r);
}

static void add_extract_variable_action(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, LspRange range) {
    if (!a || !a->has_ast || !doc || !doc->text) return;
    uint32_t s = lsp_pos_to_offset(doc, range.start), e = lsp_pos_to_offset(doc, range.end);
    while (s < e && isspace((unsigned char)doc->text[s])) s++;
    while (e > s && isspace((unsigned char)doc->text[e - 1])) e--;
    if (e <= s) return;

    LspFileIndex ix;
    lsp_index_build(&ix, a, doc);
    bool starts = false, ends = false, ok = true;
    for (size_t i = 0; i < ix.ntoks && ok; i++) {
        const LspTok *t = &ix.toks[i];
        if (t->in_string) { if (t->offset >= s && t->offset < e) ok = false; continue; }
        if (t->offset == s) starts = true;
        if (t->offset + t->len == e) ends = true;
        if (t->offset < s && t->offset + t->len > s) ok = false; /* selection cuts a token */
        if (t->offset < e && t->offset + t->len > e) ok = false;
        if (t->offset >= s && t->offset < e &&
            (t->kind == SN_TOK_LET || t->kind == SN_TOK_VAR || t->kind == SN_TOK_RETURN || t->kind == SN_TOK_SEMI ||
             t->kind == SN_TOK_LBRACE || t->kind == SN_TOK_RBRACE || is_assign_kind(t->kind))) ok = false;
    }
    const SnDecl *routine = routine_at(a, s);
    const SnStmt *anchor = routine ? anchor_in_list(&ix, &routine->body->stmts, s, 0) : NULL;
    if (!ok || !starts || !ends || !anchor) { lsp_index_free(&ix); return; }
    /* never hoist out of a loop header: it would be evaluated once */
    if (anchor->kind == SN_STMT_WHILE || anchor->kind == SN_STMT_FOR) {
        uint32_t body_open = anchor->then_br ? anchor->then_br->span.offset : UINT32_MAX;
        if (s < body_open) { lsp_index_free(&ix); return; }
    }

    /* the selection must be one complete expression */
    SnArena arena;
    sn_arena_init(&arena, 16 * 1024);
    SnDiagSink sink;
    sn_diag_init(&sink, "", doc->text + s, e - s);
    sink.out = NULL;
    sink.quiet = 1;
    SnTokenVec toks;
    memset(&toks, 0, sizeof(toks));
    sn_lex(&arena, &sink, doc->text + s, e - s, &toks);
    SnExpr *expr = sink.error_count == 0 ? sn_parse_expr_only(&arena, &sink, &toks) : NULL;
    bool complete = expr && sink.error_count == 0 && toks.len > 0;
    /* sn_parse_expr_only stops after a prefix without an error: also require
     * balanced brackets and edges that can start and end an expression */
    if (complete) {
        int depth = 0;
        size_t ntok = toks.len;
        while (ntok > 0 && toks.data[ntok - 1].kind == SN_TOK_EOF) ntok--;
        for (size_t k = 0; k < ntok && depth >= 0; k++) {
            SnTokKind tk = toks.data[k].kind;
            if (tk == SN_TOK_LPAREN || tk == SN_TOK_LBRACKET || tk == SN_TOK_LBRACE) depth++;
            else if (tk == SN_TOK_RPAREN || tk == SN_TOK_RBRACKET || tk == SN_TOK_RBRACE) depth--;
        }
        SnTokKind first = ntok ? toks.data[0].kind : SN_TOK_EOF, lastk = ntok ? toks.data[ntok - 1].kind : SN_TOK_EOF;
        bool bad_first = first == SN_TOK_PLUS || first == SN_TOK_STAR || first == SN_TOK_SLASH || first == SN_TOK_PERCENT ||
                         first == SN_TOK_DOT || first == SN_TOK_COMMA || first == SN_TOK_RPAREN || first == SN_TOK_ANDAND ||
                         first == SN_TOK_OROR || first == SN_TOK_EQ || first == SN_TOK_NE || first == SN_TOK_QDOT;
        bool bad_last = lastk == SN_TOK_PLUS || lastk == SN_TOK_MINUS || lastk == SN_TOK_STAR || lastk == SN_TOK_SLASH ||
                        lastk == SN_TOK_DOT || lastk == SN_TOK_COMMA || lastk == SN_TOK_LPAREN || lastk == SN_TOK_ANDAND ||
                        lastk == SN_TOK_OROR || lastk == SN_TOK_EQ || lastk == SN_TOK_NE || lastk == SN_TOK_BANG;
        complete = depth == 0 && !bad_first && !bad_last;
    }
    if (complete) {
        const SnToken *last = toks.data[toks.len - 1].kind == SN_TOK_EOF && toks.len > 1 ? &toks.data[toks.len - 2] : &toks.data[toks.len - 1];
        complete = last->span.offset + last->span.len == e - s && expr->kind != SN_EXPR_ASSIGN;
    }

    /* name: the callee for a call (gopls), otherwise `value` */
    char base[64] = "value";
    if (complete && expr->kind == SN_EXPR_CALL && expr->lhs && expr->lhs->text &&
        (expr->lhs->kind == SN_EXPR_IDENT || expr->lhs->kind == SN_EXPR_MEMBER)) {
        const char *callee = expr->lhs->text;
        if (strncmp(callee, "get", 3) == 0 && isupper((unsigned char)callee[3])) callee += 3;
        snprintf(base, sizeof(base), "%s", callee);
        base[0] = (char)tolower((unsigned char)base[0]);
        if (!lsp_is_valid_identifier(base)) snprintf(base, sizeof(base), "value");
    }
    sn_arena_free(&arena);
    if (!complete) { lsp_index_free(&ix); return; }

    const LspBlock *rblk = NULL;
    for (size_t b = 0; b < ix.nblocks; b++) if (ix.blocks[b].open == routine->body->span.offset) rblk = &ix.blocks[b];
    uint32_t r0 = rblk ? rblk->open : 0, r1 = rblk ? rblk->close : (uint32_t)doc->text_len;
    char name[80];
    snprintf(name, sizeof(name), "%s", base);
    for (int k = 1; name_taken(&ix, name, r0, r1, anchor->span.offset) && k < 100; k++) {
        snprintf(name, sizeof(name), "%s%d", base, k);
    }
    lsp_index_free(&ix);

    uint32_t ls = line_start_of(doc, anchor->span.offset);
    size_t indent = 0;
    while (ls + indent < anchor->span.offset && (doc->text[ls + indent] == ' ' || doc->text[ls + indent] == '\t')) indent++;
    if (ls + indent != anchor->span.offset) return; /* anchor shares its line with code before it */

    size_t cap = indent + strlen(name) + (e - s) + 32;
    char *decl = (char *)malloc(cap);
    snprintf(decl, cap, "%.*slet %s = %.*s%s", (int)indent, doc->text + ls, name, (int)(e - s), doc->text + s, doc_newline(doc));
    ActionItem *ai = actionlist_new(list, "Extract variable", "refactor.extract.variable", false, doc->uri);
    LspPosition at = lsp_offset_to_pos(doc, ls);
    action_edit(ai, (LspRange){ at, at }, decl);
    action_edit(ai, (LspRange){ lsp_offset_to_pos(doc, s), lsp_offset_to_pos(doc, e) }, name);
    free(decl);
}

/* An initializer that can replace a name without parentheses. */
static bool simple_expression(const char *t, size_t n) {
    int depth = 0, in_str = 0;
    for (size_t i = 0; i < n; i++) {
        char c = t[i];
        if (in_str) { if (c == '\\') i++; else if (c == '"') in_str = 0; continue; }
        if (c == '"') { in_str = 1; continue; }
        if (c == '(' || c == '[') depth++;
        else if (c == ')' || c == ']') depth--;
        else if (depth == 0 && !(isalnum((unsigned char)c) || c == '_' || c == '.')) return false;
    }
    return true;
}

static void add_inline_variable_action(ActionList *list, const LspDocAnalysis *a, const LspDocument *doc, LspRange range) {
    if (!a || !a->has_ast || !doc || !doc->text) return;
    LspFileIndex ix;
    lsp_index_build(&ix, a, doc);
    uint32_t off = lsp_pos_to_offset(doc, range.start);
    LspSymRef target;
    uint32_t tok_off = 0, tok_len = 0;
    bool on_decl = false;
    if (!lsp_resolve_at(&ix, off, &target, &tok_off, &tok_len, &on_decl) || target.cls != LSP_SYMREF_LOCAL) goto out;
    const LspBinding *bd = lsp_index_binding_at(&ix, target.decl_offset);
    if (!bd || !bd->stmt || bd->stmt->kind != SN_STMT_LET || bd->is_mutable || !bd->stmt->expr ||
        bd->stmt->extra_names.len) goto out;

    /* initializer text: after `=` up to the next statement */
    uint32_t init_s = 0, init_e = bd->visible_from;
    for (size_t i = 0; i < ix.ntoks; i++) {
        if (ix.toks[i].offset > bd->offset && !ix.toks[i].in_string && ix.toks[i].kind == SN_TOK_ASSIGN) {
            init_s = ix.toks[i].offset + 1;
            break;
        }
    }
    if (!init_s || init_s >= init_e) goto out;
    while (init_s < init_e && isspace((unsigned char)doc->text[init_s])) init_s++;
    while (init_e > init_s && (isspace((unsigned char)doc->text[init_e - 1]) || doc->text[init_e - 1] == ';')) init_e--;
    /* the statement must end where the next one starts (one statement per line) */
    for (uint32_t k = init_s; k < init_e; k++) if (doc->text[k] == '}' && k + 1 == init_e) goto out;
    if (init_e <= init_s) goto out;

    /* references to inline */
    uint32_t uses[256];
    size_t nuses = 0;
    for (size_t i = 0; i < ix.ntoks && nuses < 256; i++) {
        const LspTok *t = &ix.toks[i];
        if (t->kind != SN_TOK_IDENT || !t->text || strcmp(t->text, target.name) != 0 || t->offset == bd->offset) continue;
        LspSymRef r;
        bool d = false;
        if (lsp_resolve_at(&ix, t->offset, &r, NULL, NULL, &d) && !d && lsp_symref_same(&r, &target)) uses[nuses++] = t->offset;
    }
    if (nuses == 0) goto out;
    bool all = on_decl || nuses == 1;
    bool has_call = false; /* `f(` / `x.m(` / `g()(`, not a grouping paren */
    for (uint32_t k = init_s; k < init_e && !has_call; k++) {
        if (doc->text[k] != '(') continue;
        uint32_t q = k;
        while (q > init_s && (doc->text[q - 1] == ' ' || doc->text[q - 1] == '\t')) q--;
        has_call = q > init_s && (isalnum((unsigned char)doc->text[q - 1]) || doc->text[q - 1] == '_' ||
                                  doc->text[q - 1] == ')' || doc->text[q - 1] == ']');
    }
    if (all && nuses > 1 && has_call) goto out; /* would evaluate the call more than once */

    /* free identifiers must mean the same thing at every use, and not be `var` */
    for (size_t i = 0; i < ix.ntoks; i++) {
        const LspTok *t = &ix.toks[i];
        if (t->offset < init_s || t->offset >= init_e || t->kind != SN_TOK_IDENT || lsp_index_token_is_qualified(&ix, i)) continue;
        LspSymRef at_init;
        if (!lsp_resolve_at(&ix, t->offset, &at_init, NULL, NULL, NULL)) continue;
        if (at_init.cls == LSP_SYMREF_LOCAL && at_init.is_mutable) goto out;
        for (size_t u = 0; u < nuses; u++) {
            if (!all && uses[u] != tok_off) continue;
            LspSymRef at_use;
            if (!lsp_lookup_name_at(&ix, t->text, uses[u], &at_use) || !lsp_symref_same(&at_init, &at_use)) goto out;
        }
    }

    size_t init_len = init_e - init_s;
    bool paren = !simple_expression(doc->text + init_s, init_len);
    char *repl = (char *)malloc(init_len + 3);
    snprintf(repl, init_len + 3, "%s%.*s%s", paren ? "(" : "", (int)init_len, doc->text + init_s, paren ? ")" : "");

    char title[LSP_IDENT_NAME_MAX + 64];
    snprintf(title, sizeof(title), all ? "Inline variable '%s'" : "Inline this use of '%s'", target.name);
    ActionItem *ai = actionlist_new(list, title, "refactor.inline.variable", false, doc->uri);
    if (all) {
        /* delete the whole declaration line(s) */
        uint32_t del_s = line_start_of(doc, bd->stmt->span.offset);
        uint32_t del_e = line_start_of(doc, bd->visible_from);
        if (del_e <= del_s) del_e = bd->visible_from;
        action_edit(ai, (LspRange){ lsp_offset_to_pos(doc, del_s), lsp_offset_to_pos(doc, del_e) }, "");
    }
    for (size_t u = 0; u < nuses; u++) {
        if (!all && uses[u] != tok_off) continue;
        LspRange r = { lsp_offset_to_pos(doc, uses[u]), lsp_offset_to_pos(doc, uses[u] + (uint32_t)strlen(target.name)) };
        action_edit(ai, r, repl);
    }
    free(repl);
out:
    lsp_index_free(&ix);
}

char *lsp_code_action_query(LspAnalysisEngine *engine, LspDocStore *store, const LspDocument *doc, LspRange range, const JsonVal *context_diags) {
    if (!doc) return NULL;

    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, store, doc);
    }

    ActionList list;
    actionlist_init(&list);

    // 1. Process diagnostics in the request context or from active analysis
    if (context_diags && context_diags->kind == JSON_ARRAY) {
        size_t diag_cnt = json_arr_len(context_diags);
        for (size_t i = 0; i < diag_cnt; i++) {
            const JsonVal *d = json_arr_at(context_diags, i);
            const char *msg = json_get_str(d, "message", "");
            const char *code_str = json_get_str(d, "code", "");
            const JsonVal *dr_obj = json_get_obj(d, "range");
            LspRange diag_range = range;
            if (dr_obj) {
                const JsonVal *s = json_get_obj(dr_obj, "start");
                const JsonVal *e = json_get_obj(dr_obj, "end");
                if (s && e) {
                    diag_range.start.line = (uint32_t)json_get_int(s, "line", 0);
                    diag_range.start.character = (uint32_t)json_get_int(s, "character", 0);
                    diag_range.end.line = (uint32_t)json_get_int(e, "line", 0);
                    diag_range.end.character = (uint32_t)json_get_int(e, "character", 0);
                }
            }

            char tok_text[128] = {0};
            extract_token_text_at_range(doc, diag_range, tok_text, sizeof(tok_text));

            // A. Type not imported / Undeclared name (SNOVA0121 / SNOVA0023)
            if (strstr(code_str, "0121") || strstr(code_str, "0023") ||
                strstr(code_str, "0027") || strstr(msg, "not imported") ||
                strstr(msg, "undeclared") || strstr(msg, "Unknown type")) {
                if (tok_text[0]) {
                    add_auto_import_actions(&list, a, doc, tok_text);
                }
            }

            // B. Legacy type spelling (SNOVA0011 / SNOVA0027 with capitalized types)
            if (strstr(code_str, "0011") || strstr(code_str, "0027") || strstr(msg, "legacy") || strstr(msg, "spelling")) {
                if (strcasecmp(tok_text, "Int") == 0) {
                    actionlist_add(&list, "Change to 'int'", "quickfix", true, doc->uri, diag_range, "int");
                } else if (strcasecmp(tok_text, "String") == 0) {
                    actionlist_add(&list, "Change to 'string'", "quickfix", true, doc->uri, diag_range, "string");
                } else if (strcasecmp(tok_text, "Bool") == 0) {
                    actionlist_add(&list, "Change to 'bool'", "quickfix", true, doc->uri, diag_range, "bool");
                } else if (strcasecmp(tok_text, "Double") == 0) {
                    actionlist_add(&list, "Change to 'double'", "quickfix", true, doc->uri, diag_range, "double");
                } else if (strcasecmp(tok_text, "Long") == 0) {
                    actionlist_add(&list, "Change to 'long'", "quickfix", true, doc->uri, diag_range, "long");
                } else if (strcasecmp(tok_text, "Decimal") == 0) {
                    actionlist_add(&list, "Change to 'decimal'", "quickfix", true, doc->uri, diag_range, "decimal");
                }
            }

            // C. Immutable reassign (SNOVA0047)
            if (strstr(code_str, "0047") || strstr(msg, "immutable") || strstr(msg, "reassign")) {
                if (tok_text[0]) {
                    // Search definition of variable to replace 'let' with 'var'
                    uint32_t off = lsp_pos_to_offset(doc, diag_range.start);
                    const SnToken *tok = lsp_find_token_at(a, off);
                    if (tok) {
                        const char *name = tok->text;
                        const SnSymbol *sym = lsp_find_symbol_at(a, doc, off, NULL);
                        if (sym && sym->decl && sym->decl->span.offset > 0) {
                            LspRange r = lsp_span_to_range(doc, sym->decl->span.offset, 3, sym->decl->span.line, sym->decl->span.col);
                            char title[128];
                            snprintf(title, sizeof(title), "Change 'let %s' to 'var %s'", name, name);
                            actionlist_add(&list, title, "quickfix", true, doc->uri, r, "var");
                        }
                    }
                }
            }

            // D. Missing return (SNOVA0142)
            if (strstr(code_str, "0142") || strstr(msg, "return")) {
                uint32_t off = lsp_pos_to_offset(doc, diag_range.start);
                const SnDecl *decl = lsp_find_decl_at(a, off);
                if (decl && decl->body && decl->body->span.offset > 0) {
                    uint32_t body_end = decl->body->span.offset + decl->body->span.len;
                    if (body_end > 1) body_end--; // Before closing brace
                    LspPosition p = lsp_offset_to_pos(doc, body_end);
                    LspRange r = { .start = p, .end = p };
                    actionlist_add(&list, "Add 'return null;' statement", "quickfix", false, doc->uri, r, "        return null;\n");
                }
            }

            // E. Optional chaining non-optional (SNOVA0203)
            if (strstr(code_str, "0203") || strstr(msg, "redundant optional chaining")) {
                actionlist_add(&list, "Replace '?.' with '.'", "quickfix", true, doc->uri, diag_range, ".");
            }

            // F. Func in class (SNOVA0030) or Method at top level (SNOVA0031)
            if (strstr(code_str, "0030") || strstr(msg, "cannot declare `func` inside")) {
                actionlist_add(&list, "Change 'func' to 'method'", "quickfix", true, doc->uri, diag_range, "method");
            }
            if (strstr(code_str, "0031") || strstr(msg, "cannot declare `method` at top level")) {
                actionlist_add(&list, "Change 'method' to 'func'", "quickfix", true, doc->uri, diag_range, "func");
            }
        }
    }

    // Direct range & token inspection (quickfixes available at cursor even without explicit diagnostic context)
    char range_tok[128] = {0};
    extract_token_text_at_range(doc, range, range_tok, sizeof(range_tok));
    if (range_tok[0]) {
        // Casing fix
        if (strcmp(range_tok, "Int") == 0) {
            actionlist_add(&list, "Use primitive 'int' (preferred spelling)", "quickfix", true, doc->uri, range, "int");
        } else if (strcmp(range_tok, "String") == 0) {
            actionlist_add(&list, "Use primitive 'string' (preferred spelling)", "quickfix", true, doc->uri, range, "string");
        } else if (strcmp(range_tok, "Bool") == 0) {
            actionlist_add(&list, "Use primitive 'bool' (preferred spelling)", "quickfix", true, doc->uri, range, "bool");
        } else if (strcmp(range_tok, "Double") == 0) {
            actionlist_add(&list, "Use primitive 'double' (preferred spelling)", "quickfix", true, doc->uri, range, "double");
        } else if (strcmp(range_tok, "Long") == 0) {
            actionlist_add(&list, "Use primitive 'long' (preferred spelling)", "quickfix", true, doc->uri, range, "long");
        } else if (strcmp(range_tok, "Decimal") == 0) {
            actionlist_add(&list, "Use primitive 'decimal' (preferred spelling)", "quickfix", true, doc->uri, range, "decimal");
        }

        // Mutability fix on immutable variable reference / assignment
        uint32_t off = lsp_pos_to_offset(doc, range.start);
        const SnSymbol *sym = lsp_find_symbol_at(a, doc, off, NULL);
        if (sym && sym->kind == SN_SYM_LOCAL && !sym->is_mutable) {
            uint32_t decl_off = (sym->decl && sym->decl->span.offset > 0) ? sym->decl->span.offset : sym->span.offset;
            uint32_t decl_line = (sym->decl && sym->decl->span.line > 0) ? sym->decl->span.line : sym->span.line;
            uint32_t decl_col = (sym->decl && sym->decl->span.col > 0) ? sym->decl->span.col : sym->span.col;
            if (decl_off > 0 || decl_line > 0) {
                LspRange r = lsp_span_to_range(doc, decl_off, 3, decl_line, decl_col);
                char title[128];
                snprintf(title, sizeof(title), "Change 'let %s' to 'var %s'", sym->name ? sym->name : range_tok, sym->name ? sym->name : range_tok);
                actionlist_add(&list, title, "quickfix", true, doc->uri, r, "var");
            }
        }
    }

    // 2. Source Actions (Organize Imports)
    add_organize_imports_action(&list, a, doc);

    // 3. Refactorings
    add_generate_constructor_action(&list, a, doc, range.start);
    add_extract_variable_action(&list, a, doc, range);
    add_inline_variable_action(&list, a, doc, range);

    // 4. Build JSON Array Response
    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);

    for (size_t i = 0; i < list.len; i++) {
        const ActionItem *ai = &list.items[i];
        jb_start_obj(&jb);
        jb_kv_str(&jb, "title", ai->title);
        jb_kv_str(&jb, "kind", ai->kind);
        if (ai->is_preferred) {
            jb_kv_bool(&jb, "isPreferred", true);
        }

        // WorkspaceEdit
        jb_key(&jb, "edit");
        jb_start_obj(&jb);
        jb_key(&jb, "changes");
        jb_start_obj(&jb);

        const char *target_uri = ai->doc_uri ? ai->doc_uri : doc->uri;
        jb_key(&jb, target_uri);
        jb_start_arr(&jb);
        for (size_t e = 0; e < ai->nedits; e++) {
            const ActionEdit *ed = &ai->edits[e];
            jb_start_obj(&jb);
            jb_key(&jb, "range");
            jb_start_obj(&jb);
            jb_key(&jb, "start");
            jb_start_obj(&jb);
            jb_kv_int(&jb, "line", ed->range.start.line);
            jb_kv_int(&jb, "character", ed->range.start.character);
            jb_end_obj(&jb);
            jb_key(&jb, "end");
            jb_start_obj(&jb);
            jb_kv_int(&jb, "line", ed->range.end.line);
            jb_kv_int(&jb, "character", ed->range.end.character);
            jb_end_obj(&jb);
            jb_end_obj(&jb);
            jb_kv_str(&jb, "newText", ed->new_text);
            jb_end_obj(&jb);
        }
        jb_end_arr(&jb);
        jb_end_obj(&jb);
        jb_end_obj(&jb);

        jb_end_obj(&jb);
    }

    jb_end_arr(&jb);

    actionlist_free(&list);
    return jb_take(&jb);
}
