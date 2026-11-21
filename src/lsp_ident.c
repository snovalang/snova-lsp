#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif

#include "lsp_ident.h"
#include "lsp_imports.h"
#include "lsp_symbol_span.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── growable arrays ─────────────────────────────────────────────────────── */

#define PUSH(arr, n, cap, val)                                                 \
    do {                                                                       \
        if ((n) >= (cap)) {                                                    \
            size_t nc_ = (cap) ? (cap) * 2 : 64;                               \
            void *np_ = realloc((arr), nc_ * sizeof(*(arr)));                  \
            if (!np_) break;                                                   \
            (arr) = np_;                                                       \
            (cap) = nc_;                                                       \
        }                                                                      \
        (arr)[(n)++] = (val);                                                  \
    } while (0)

typedef struct {
    LspFileIndex *ix;
    size_t tok_cap, block_cap, bind_cap, member_cap;
} Builder;

/* ── tokens ──────────────────────────────────────────────────────────────── */

static uint32_t text_len(const LspFileIndex *ix) {
    return ix->doc && ix->doc->text ? (uint32_t)ix->doc->text_len : 0;
}

/* Identifiers inside `${...}` are not tokens of the file; lex each
 * interpolation body on its own and shift the offsets into the file. */
static void add_interpolation_tokens(Builder *b, const SnToken *tok) {
    LspFileIndex *ix = b->ix;
    const char *s = ix->doc->text + tok->span.offset;
    size_t n = tok->span.len;
    size_t i = 0;
    while (i + 1 < n) {
        if (s[i] != '$' || s[i + 1] != '{') { i++; continue; }
        if (i > 0 && s[i - 1] == '$') { i += 2; continue; } /* `$$` escapes */
        size_t begin = i + 2, j = begin;
        int depth = 1, in_str = 0;
        while (j < n && depth) {
            if (s[j] == '\\' && j + 1 < n) { j += 2; continue; }
            if (s[j] == '"') in_str = !in_str;
            else if (!in_str && s[j] == '{') depth++;
            else if (!in_str && s[j] == '}') { if (--depth == 0) break; }
            j++;
        }
        size_t len = j - begin;
        if (len > 0) {
            SnDiagSink sink;
            sn_diag_init(&sink, "", s + begin, len);
            sink.out = NULL;
            sink.quiet = 1;
            SnTokenVec inner;
            memset(&inner, 0, sizeof(inner));
            SnArena *arena = (SnArena *)&ix->a->arena;
            sn_lex(arena, &sink, s + begin, len, &inner);
            for (size_t k = 0; k < inner.len; k++) {
                const SnToken *t = &inner.data[k];
                if (t->kind == SN_TOK_EOF) break;
                LspTok lt = {
                    .kind = t->kind,
                    .offset = tok->span.offset + (uint32_t)begin + t->span.offset,
                    .len = t->span.len,
                    .text = t->text,
                    .in_string = 1,
                };
                PUSH(ix->toks, ix->ntoks, b->tok_cap, lt);
            }
        }
        i = j + 1;
    }
}

static void build_tokens(Builder *b) {
    LspFileIndex *ix = b->ix;
    const SnTokenVec *tv = &ix->a->tokens;
    int path_mode = 0; /* 1 import, 2 package */
    uint32_t path_line = 0;
    for (size_t i = 0; i < tv->len; i++) {
        const SnToken *t = &tv->data[i];
        if (t->kind == SN_TOK_EOF) break;
        if (path_mode && (t->span.line != path_line || t->kind == SN_TOK_SEMI)) path_mode = 0;
        LspTok lt = { .kind = t->kind, .offset = t->span.offset, .len = t->span.len, .text = t->text };
        if (path_mode == 1) lt.in_import = 1;
        if (path_mode == 2) lt.in_package = 1;
        PUSH(ix->toks, ix->ntoks, b->tok_cap, lt);
        if (t->kind == SN_TOK_IMPORT || t->kind == SN_TOK_PACKAGE) {
            path_mode = t->kind == SN_TOK_IMPORT ? 1 : 2;
            path_line = t->span.line;
        }
        if (t->kind == SN_TOK_STRING && t->span.offset + t->span.len <= text_len(ix) &&
            (t->has_interpolation || (t->text && strstr(t->text, "${")))) {
            add_interpolation_tokens(b, t);
        }
    }
}

long lsp_index_token_at(const LspFileIndex *ix, uint32_t offset) {
    if (!ix) return -1;
    long best = -1;
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (offset >= t->offset && offset < t->offset + t->len) {
            best = (long)i; /* interpolation tokens follow their string: prefer them */
        }
    }
    if (best < 0 && offset > 0) {
        /* Cursor right after an identifier, as editors send it. */
        for (size_t i = 0; i < ix->ntoks; i++) {
            const LspTok *t = &ix->toks[i];
            if (t->kind == SN_TOK_IDENT && offset == t->offset + t->len) best = (long)i;
        }
    }
    return best;
}

bool lsp_index_token_is_qualified(const LspFileIndex *ix, size_t i) {
    if (!ix || i == 0 || i >= ix->ntoks) return false;
    const LspTok *prev = &ix->toks[i - 1];
    if (prev->in_string != ix->toks[i].in_string) return false;
    return prev->kind == SN_TOK_DOT || prev->kind == SN_TOK_QDOT;
}

/* ── blocks ──────────────────────────────────────────────────────────────── */

static void build_blocks(Builder *b) {
    LspFileIndex *ix = b->ix;
    size_t *stack = NULL, sp = 0, scap = 0;
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->in_string) continue;
        if (t->kind == SN_TOK_LBRACE) {
            LspBlock blk = { t->offset, text_len(ix) };
            PUSH(ix->blocks, ix->nblocks, b->block_cap, blk);
            PUSH(stack, sp, scap, ix->nblocks - 1);
        } else if (t->kind == SN_TOK_RBRACE && sp > 0) {
            ix->blocks[stack[--sp]].close = t->offset;
        }
    }
    free(stack);
}

static const LspBlock *block_opening_at(const LspFileIndex *ix, uint32_t open) {
    size_t lo = 0, hi = ix->nblocks;
    while (lo < hi) {
        size_t mid = (lo + hi) / 2;
        if (ix->blocks[mid].open < open) lo = mid + 1; else hi = mid;
    }
    return lo < ix->nblocks && ix->blocks[lo].open == open ? &ix->blocks[lo] : NULL;
}

/* First block that opens at or after `offset`. */
static const LspBlock *block_from(const LspFileIndex *ix, uint32_t offset) {
    for (size_t i = 0; i < ix->nblocks; i++) {
        if (ix->blocks[i].open >= offset) return &ix->blocks[i];
    }
    return NULL;
}

static const LspBlock *innermost_block(const LspFileIndex *ix, uint32_t offset) {
    const LspBlock *best = NULL;
    for (size_t i = 0; i < ix->nblocks; i++) {
        const LspBlock *blk = &ix->blocks[i];
        if (blk->open > offset) break;
        if (offset <= blk->close) best = blk; /* later opens are nested deeper */
    }
    return best;
}

/* Innermost '(' ... ')' pair strictly enclosing `offset`, or the block. */
static uint32_t enclosing_group_end(const LspFileIndex *ix, uint32_t offset) {
    long depth = 0;
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->in_string || t->offset <= offset) continue;
        if (t->kind == SN_TOK_LPAREN || t->kind == SN_TOK_LBRACKET) depth++;
        else if (t->kind == SN_TOK_RPAREN || t->kind == SN_TOK_RBRACKET) {
            if (depth == 0) return t->offset;
            depth--;
        } else if (t->kind == SN_TOK_RBRACE && depth == 0) {
            return t->offset;
        }
    }
    return text_len(ix);
}

/* ── bindings ────────────────────────────────────────────────────────────── */

/* First identifier token named `name` at or after `from`, outside strings. */
static const LspTok *ident_from(const LspFileIndex *ix, uint32_t from, const char *name) {
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->in_string || t->offset < from || t->kind != SN_TOK_IDENT) continue;
        if (t->text && strcmp(t->text, name) == 0) return t;
    }
    return NULL;
}

/* Last identifier named `name` before `before`, within `window` bytes. */
static const LspTok *ident_before(const LspFileIndex *ix, uint32_t before, const char *name, uint32_t window) {
    const LspTok *found = NULL;
    for (size_t i = 0; i < ix->ntoks; i++) {
        const LspTok *t = &ix->toks[i];
        if (t->offset >= before) break;
        if (t->in_string || t->kind != SN_TOK_IDENT || !t->text || strcmp(t->text, name) != 0) continue;
        if (before - t->offset <= window) found = t;
    }
    return found;
}

static void add_binding(Builder *b, const char *name, const LspTok *tok, uint32_t scope_start,
                        uint32_t scope_end, uint32_t visible_from, SnSymbolKind kind,
                        uint8_t is_mutable, const SnStmt *stmt, const SnDecl *routine) {
    if (!name || !tok || !name[0] || strcmp(name, "_") == 0) return;
    LspBinding bd = {
        .name = tok->text, .offset = tok->offset, .len = tok->len,
        .scope_start = scope_start, .scope_end = scope_end,
        .visible_from = visible_from, .kind = kind, .is_mutable = is_mutable,
        .stmt = stmt, .routine = routine,
    };
    PUSH(b->ix->binds, b->ix->nbinds, b->bind_cap, bd);
}

static void walk_stmt(Builder *b, const SnStmt *s, uint32_t next_offset);
static void walk_expr(Builder *b, const SnExpr *e);

static void bind_pattern(Builder *b, const SnPattern *p, uint32_t scope_start, uint32_t scope_end) {
    if (!p) return;
    if (p->kind == SN_PAT_BINDING && p->name) {
        const LspTok *t = ident_from(b->ix, p->span.offset, p->name);
        if (t && t->offset == p->span.offset) {
            add_binding(b, p->name, t, scope_start, scope_end, t->offset, SN_SYM_LOCAL, 0, NULL, NULL);
        }
    }
    for (size_t i = 0; i < p->subs.len; i++) {
        bind_pattern(b, SN_LIST_AT(p->subs, SnPattern, i), scope_start, scope_end);
    }
}

static void walk_arms(Builder *b, const SnList *arms) {
    for (size_t i = 0; i < arms->len; i++) {
        const SnMatchArm *arm = SN_LIST_AT(*arms, SnMatchArm, i);
        if (!arm) continue;
        uint32_t start = arm->pattern ? arm->pattern->span.offset : arm->span.offset;
        uint32_t end;
        if (i + 1 < arms->len) {
            const SnMatchArm *next = SN_LIST_AT(*arms, SnMatchArm, i + 1);
            end = next->pattern ? next->pattern->span.offset : next->span.offset;
        } else {
            const LspBlock *outer = innermost_block(b->ix, start);
            end = outer ? outer->close : text_len(b->ix);
        }
        bind_pattern(b, arm->pattern, start, end);
        walk_expr(b, arm->guard);
        walk_expr(b, arm->value);
        walk_stmt(b, arm->body, end);
    }
}

static void walk_params_into(Builder *b, const SnList *params, uint32_t scope_start,
                             uint32_t scope_end, const SnDecl *routine) {
    for (size_t i = 0; i < params->len; i++) {
        const SnParam *p = SN_LIST_AT(*params, SnParam, i);
        if (!p || !p->name) continue;
        const LspTok *t = ident_from(b->ix, p->span.offset, p->name);
        if (t) add_binding(b, p->name, t, scope_start, scope_end, scope_start, SN_SYM_PARAM, 0, NULL, routine);
        walk_expr(b, p->def);
    }
}

static void record_member(Builder *b, uint32_t name_offset, const SnExpr *e, uint8_t field_init) {
    LspMemberSite m = { name_offset, e, field_init };
    PUSH(b->ix->members, b->ix->nmembers, b->member_cap, m);
}

static void walk_expr(Builder *b, const SnExpr *e) {
    if (!e) return;
    LspFileIndex *ix = b->ix;
    switch (e->kind) {
        case SN_EXPR_MEMBER: {
            /* span is the '.'; the member name is the next token */
            for (size_t i = 0; i + 1 < ix->ntoks; i++) {
                if (ix->toks[i].offset == e->span.offset && !ix->toks[i].in_string &&
                    ix->toks[i + 1].kind == SN_TOK_IDENT) {
                    record_member(b, ix->toks[i + 1].offset, e, 0);
                    break;
                }
            }
            break;
        }
        case SN_EXPR_STRUCT_LIT: {
            /* `Type { field: value }`: field names at brace depth 1 */
            int depth = 0;
            size_t want = 0;
            for (size_t i = 0; i < ix->ntoks && want < e->field_names.len; i++) {
                const LspTok *t = &ix->toks[i];
                if (t->in_string || t->offset < e->span.offset) continue;
                if (t->kind == SN_TOK_LBRACE) { depth++; continue; }
                if (t->kind == SN_TOK_RBRACE) { if (--depth <= 0) break; continue; }
                if (depth != 1 || t->kind != SN_TOK_IDENT || i + 1 >= ix->ntoks ||
                    ix->toks[i + 1].kind != SN_TOK_COLON) continue;
                const char *fname = SN_LIST_AT(e->field_names, const char, want);
                while (!fname && want + 1 < e->field_names.len) {
                    fname = SN_LIST_AT(e->field_names, const char, ++want);
                }
                if (fname && t->text && strcmp(t->text, fname) == 0) {
                    record_member(b, t->offset, e, 1);
                    want++;
                }
            }
            break;
        }
        case SN_EXPR_LAMBDA: {
            uint32_t start = e->span.offset, end;
            if (e->body && e->body->kind == SN_STMT_BLOCK) {
                const LspBlock *blk = block_opening_at(ix, e->body->span.offset);
                end = blk ? blk->close : enclosing_group_end(ix, start);
            } else {
                end = enclosing_group_end(ix, start);
            }
            walk_params_into(b, &e->params, start, end, NULL);
            break;
        }
        case SN_EXPR_MATCH:
            walk_arms(b, &e->arms);
            break;
        default:
            break;
    }
    walk_expr(b, e->lhs);
    walk_expr(b, e->rhs);
    for (size_t i = 0; i < e->args.len; i++) walk_expr(b, SN_LIST_AT(e->args, SnExpr, i));
    walk_expr(b, e->value);
    if (e->body) walk_stmt(b, e->body, 0);
    if (e->else_body) walk_stmt(b, e->else_body, 0);
}

static void walk_block_list(Builder *b, const SnList *stmts, uint32_t block_close) {
    for (size_t i = 0; i < stmts->len; i++) {
        const SnStmt *s = SN_LIST_AT(*stmts, SnStmt, i);
        uint32_t next = block_close;
        for (size_t k = i + 1; k < stmts->len; k++) {
            const SnStmt *n = SN_LIST_AT(*stmts, SnStmt, k);
            if (n && n->span.offset > (s ? s->span.offset : 0)) { next = n->span.offset; break; }
        }
        walk_stmt(b, s, next);
    }
}

/* `next_offset`: where the following sibling statement starts. A let binding
 * becomes visible there, so `let x = x + 1` still reads the outer `x`. */
static void walk_stmt(Builder *b, const SnStmt *s, uint32_t next_offset) {
    if (!s) return;
    LspFileIndex *ix = b->ix;
    switch (s->kind) {
        case SN_STMT_LET:
        case SN_STMT_VAR: {
            const LspBlock *blk = innermost_block(ix, s->span.offset);
            uint32_t start = blk ? blk->open : 0;
            uint32_t end = blk ? blk->close : text_len(ix);
            uint32_t visible = next_offset ? next_offset : end;
            const LspTok *t = s->name ? ident_from(ix, s->span.offset, s->name) : NULL;
            add_binding(b, s->name, t, start, end, visible, SN_SYM_LOCAL,
                        s->kind == SN_STMT_VAR, s, NULL);
            uint32_t from = t ? t->offset + 1 : s->span.offset;
            for (size_t i = 0; i < s->extra_names.len; i++) {
                const char *extra = SN_LIST_AT(s->extra_names, const char, i);
                const LspTok *et = extra ? ident_from(ix, from, extra) : NULL;
                add_binding(b, extra, et, start, end, visible, SN_SYM_LOCAL, 1, NULL, NULL);
                if (et) from = et->offset + 1;
            }
            walk_expr(b, s->expr);
            break;
        }
        case SN_STMT_FOR: {
            walk_expr(b, s->expr);
            const LspTok *t = s->name ? ident_from(ix, s->span.offset, s->name) : NULL;
            const LspBlock *body = s->then_br ? block_opening_at(ix, s->then_br->span.offset) : NULL;
            if (!body && t) body = block_from(ix, t->offset);
            if (t && body) add_binding(b, s->name, t, body->open, body->close, body->open, SN_SYM_LOCAL, 0, NULL, NULL);
            walk_stmt(b, s->then_br, 0);
            break;
        }
        case SN_STMT_BLOCK: {
            const LspBlock *blk = block_opening_at(ix, s->span.offset);
            if (s->name) { /* catch block with a bound error */
                const LspTok *t = ident_before(ix, s->span.offset, s->name, 64);
                if (t && blk) add_binding(b, s->name, t, blk->open, blk->close, blk->open, SN_SYM_LOCAL, 0, NULL, NULL);
            }
            walk_block_list(b, &s->stmts, blk ? blk->close : text_len(ix));
            break;
        }
        case SN_STMT_TRY:
            walk_stmt(b, s->then_br, 0);
            for (size_t i = 0; i < s->catches.len; i++) walk_stmt(b, SN_LIST_AT(s->catches, SnStmt, i), 0);
            walk_stmt(b, s->finally_br, 0);
            break;
        case SN_STMT_MATCH:
            walk_expr(b, s->expr);
            walk_arms(b, &s->arms);
            break;
        default:
            walk_expr(b, s->expr);
            walk_stmt(b, s->then_br, 0);
            walk_stmt(b, s->else_br, 0);
            for (size_t i = 0; i < s->stmts.len; i++) walk_stmt(b, SN_LIST_AT(s->stmts, SnStmt, i), 0);
            break;
    }
}

static void walk_decl(Builder *b, const SnDecl *d) {
    if (!d) return;
    LspFileIndex *ix = b->ix;
    if ((d->kind == SN_DECL_FUNC || d->kind == SN_DECL_METHOD) && d->body) {
        const LspBlock *body = block_opening_at(ix, d->body->span.offset);
        if (body) walk_params_into(b, &d->params, body->open, body->close, d);
        walk_stmt(b, d->body, 0);
    }
    walk_expr(b, d->init);
    if (d->getter) walk_expr(b, d->getter->proj);
    if (d->setter) walk_expr(b, d->setter->proj);
    for (size_t i = 0; i < d->members.len; i++) walk_decl(b, SN_LIST_AT(d->members, SnDecl, i));
}

void lsp_index_build(LspFileIndex *ix, const LspDocAnalysis *a, const LspDocument *doc) {
    memset(ix, 0, sizeof(*ix));
    ix->a = a;
    ix->doc = doc;
    if (!a || !doc || !doc->text || !a->has_ast) return;
    Builder b = { .ix = ix };
    build_tokens(&b);
    build_blocks(&b);
    for (size_t i = 0; i < a->unit.decls.len; i++) walk_decl(&b, SN_LIST_AT(a->unit.decls, SnDecl, i));
}

void lsp_index_free(LspFileIndex *ix) {
    if (!ix) return;
    free(ix->toks);
    free(ix->blocks);
    free(ix->binds);
    free(ix->members);
    memset(ix, 0, sizeof(*ix));
}

/* ── lookup helpers ──────────────────────────────────────────────────────── */

const LspBinding *lsp_index_binding_at(const LspFileIndex *ix, uint32_t offset) {
    for (size_t i = 0; ix && i < ix->nbinds; i++) {
        if (ix->binds[i].offset == offset) return &ix->binds[i];
    }
    return NULL;
}

const LspBinding *lsp_index_visible_binding(const LspFileIndex *ix, const char *name, uint32_t offset) {
    const LspBinding *best = NULL;
    for (size_t i = 0; ix && name && i < ix->nbinds; i++) {
        const LspBinding *bd = &ix->binds[i];
        if (!bd->name || strcmp(bd->name, name) != 0) continue;
        if (offset < bd->scope_start || offset >= bd->scope_end) continue;
        if (offset < bd->visible_from && offset != bd->offset) continue;
        if (!best || bd->scope_start > best->scope_start ||
            (bd->scope_start == best->scope_start && bd->offset > best->offset)) {
            best = bd;
        }
    }
    return best;
}

static const char *intern(const LspDocAnalysis *a, const char *s) {
    return sn_intern_cstr((SnInternTable *)&a->intern, s);
}

static const char *current_package(const LspDocAnalysis *a) {
    return (a->unit.package && a->unit.package[0]) ? a->unit.package : "main";
}

const char *lsp_symbol_package(const LspDocAnalysis *a, const SnSymbol *sym) {
    if (!a || !sym || !sym->name) return NULL;
    for (const SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
        if (pe->scope && sn_scope_lookup_local(pe->scope, sym->name) == sym) return pe->package_name;
    }
    /* Same declaration collected twice (disk parse and open buffer). */
    for (const SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
        SnSymbol *s = pe->scope ? sn_scope_lookup_local(pe->scope, sym->name) : NULL;
        if (s && s->decl && sym->decl && s->decl->kind == sym->decl->kind && s->origin && sym->origin &&
            s->origin->path && sym->origin->path && strcmp(s->origin->path, sym->origin->path) == 0) {
            return pe->package_name;
        }
    }
    return NULL;
}

static bool package_in_workspace(const LspDocAnalysis *a, const char *package) {
    const SnPackageNode *node = package ? sn_pkggraph_find((SnPackageGraph *)&a->graph, package) : NULL;
    if (!node) return package && strncmp(package, "builtin", 7) != 0;
    return lsp_package_origin(node) == LSP_PKG_PROJECT;
}

static void set_global(const LspDocAnalysis *a, LspSymRef *out, const SnSymbol *sym,
                       const char *package, const char *name) {
    memset(out, 0, sizeof(*out));
    out->cls = LSP_SYMREF_GLOBAL;
    out->kind = sym ? sym->kind : SN_SYM_FUNC;
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->package, sizeof(out->package), "%s", package ? package : "");
    if (sym && sym->origin && sym->origin->path) snprintf(out->decl_path, sizeof(out->decl_path), "%s", sym->origin->path);
    out->in_workspace = package_in_workspace(a, package);
}

static void set_local(const LspFileIndex *ix, LspSymRef *out, const LspBinding *bd) {
    memset(out, 0, sizeof(*out));
    out->cls = LSP_SYMREF_LOCAL;
    out->kind = bd->kind;
    snprintf(out->name, sizeof(out->name), "%s", bd->name);
    snprintf(out->decl_path, sizeof(out->decl_path), "%s", ix->doc->path ? ix->doc->path : "");
    out->decl_offset = bd->offset;
    out->scope_start = bd->scope_start;
    out->scope_end = bd->scope_end;
    out->in_workspace = true;
    out->is_mutable = bd->is_mutable;
}

/* Type symbol named by a (possibly dotted) type name, as seen from this file. */
static SnSymbol *find_type_symbol(const LspDocAnalysis *a, const char *type_name) {
    if (!type_name) return NULL;
    const char *dot = strrchr(type_name, '.');
    const char *base = dot ? dot + 1 : type_name;
    const char *iname = intern(a, base);
    if (dot) {
        char pkg[SNOVAC_PATH_MAX];
        snprintf(pkg, sizeof(pkg), "%.*s", (int)(dot - type_name), type_name);
        SnScope *ps = sn_resolver_package_scope(&a->resolver, intern(a, pkg));
        SnSymbol *s = ps ? sn_scope_lookup_local(ps, iname) : NULL;
        if (s) return s;
    }
    SnScope *cur = sn_resolver_package_scope(&a->resolver, intern(a, current_package(a)));
    SnSymbol *s = cur ? sn_scope_lookup_local(cur, iname) : NULL;
    if (s && s->kind == SN_SYM_TYPE) return s;
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        SnScope *is = imp ? sn_resolver_package_scope(&a->resolver, intern(a, imp)) : NULL;
        s = is ? sn_scope_lookup_local(is, iname) : NULL;
        if (s && s->kind == SN_SYM_TYPE) return s;
    }
    if (a->resolver.prelude_scope) {
        s = sn_scope_lookup_local(a->resolver.prelude_scope, iname);
        if (s && s->kind == SN_SYM_TYPE) return s;
    }
    for (const SnPackageScopeEntry *pe = a->resolver.packages; pe; pe = pe->next) {
        s = pe->scope ? sn_scope_lookup_local(pe->scope, iname) : NULL;
        if (s && s->kind == SN_SYM_TYPE) return s;
    }
    return NULL;
}

static const SnScope *member_scope_of(const LspDocAnalysis *a, const SnDecl *type_decl) {
    if (!type_decl) return NULL;
    SnScope *ms = sn_resolver_type_scope(&a->resolver, type_decl);
    if (ms) return ms;
    /* The open buffer's decl is a different pointer from the disk parse. */
    for (const SnTypeScopeEntry *te = a->resolver.type_scopes; te; te = te->next) {
        if (te->type_decl && te->type_decl->name && type_decl->name &&
            strcmp(te->type_decl->name, type_decl->name) == 0 && te->type_decl->kind == type_decl->kind) {
            return te->member_scope;
        }
    }
    return NULL;
}

/* Finds member `name` on `type_sym` or a supertype; returns the declaring
 * type symbol so inherited uses share one key with the declaration. */
static const SnSymbol *find_member(const LspDocAnalysis *a, const SnSymbol *type_sym, const char *name,
                                   const SnSymbol **member_out, int depth) {
    if (!type_sym || !type_sym->decl || depth > 8) return NULL;
    const SnScope *ms = member_scope_of(a, type_sym->decl);
    const char *iname = intern(a, name);
    SnSymbol *m = ms ? sn_scope_lookup_local(ms, iname) : NULL;
    if (m) {
        if (member_out) *member_out = m;
        return type_sym;
    }
    const SnDecl *td = type_sym->decl;
    for (size_t i = 0; i < td->supertypes.len; i++) {
        const SnType *st = SN_LIST_AT(td->supertypes, SnType, i);
        SnSymbol *super = st ? find_type_symbol(a, st->name) : NULL;
        if (!super || super == type_sym) continue;
        const SnSymbol *owner = find_member(a, super, name, member_out, depth + 1);
        if (owner) return owner;
    }
    return NULL;
}

static bool set_member(const LspDocAnalysis *a, LspSymRef *out, const SnSymbol *type_sym, const char *name) {
    const SnSymbol *member = NULL;
    const SnSymbol *owner = find_member(a, type_sym, name, &member, 0);
    if (!owner) return false;
    memset(out, 0, sizeof(*out));
    out->cls = LSP_SYMREF_MEMBER;
    out->kind = member ? member->kind : SN_SYM_FIELD;
    snprintf(out->name, sizeof(out->name), "%s", name);
    snprintf(out->owner, sizeof(out->owner), "%s", owner->name);
    const char *pkg = lsp_symbol_package(a, owner);
    snprintf(out->package, sizeof(out->package), "%s", pkg ? pkg : current_package(a));
    const SnSymbol *where = member && member->origin ? member : owner;
    if (where->origin && where->origin->path) snprintf(out->decl_path, sizeof(out->decl_path), "%s", where->origin->path);
    out->in_workspace = package_in_workspace(a, out->package);
    return true;
}

/* Top-level declaration whose extent contains `offset`. */
static const SnDecl *top_decl_at(const LspDocAnalysis *a, uint32_t offset) {
    const SnDecl *found = NULL;
    for (size_t i = 0; i < a->unit.decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(a->unit.decls, SnDecl, i);
        if (d && d->span.offset <= offset) found = d;
    }
    return found;
}

static bool is_type_like(const SnDecl *d) {
    return d && (d->kind == SN_DECL_CLASS || d->kind == SN_DECL_STRUCT || d->kind == SN_DECL_ENUM ||
                 d->kind == SN_DECL_INTERFACE || d->kind == SN_DECL_EXTENSION);
}

static SnSymbol *type_symbol_of_decl(const LspDocAnalysis *a, const SnDecl *d) {
    return d && d->name ? find_type_symbol(a, d->name) : NULL;
}

static bool lookup_package_level(const LspDocAnalysis *a, const char *name, LspSymRef *out) {
    const char *iname = intern(a, name);
    const char *pkg = current_package(a);
    SnScope *ps = sn_resolver_package_scope(&a->resolver, intern(a, pkg));
    SnSymbol *s = ps ? sn_scope_lookup_local(ps, iname) : NULL;
    if (s) { set_global(a, out, s, pkg, name); return true; }

    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        if (!imp) continue;
        SnScope *is = sn_resolver_package_scope(&a->resolver, intern(a, imp));
        if (is) {
            s = sn_scope_lookup_local(is, iname);
            if (s) { set_global(a, out, s, imp, name); return true; }
            continue;
        }
        const char *dot = strrchr(imp, '.'); /* `import pkg.Name` */
        if (dot && strcmp(dot + 1, name) == 0) {
            char pkgname[SNOVAC_PATH_MAX];
            snprintf(pkgname, sizeof(pkgname), "%.*s", (int)(dot - imp), imp);
            SnScope *qs = sn_resolver_package_scope(&a->resolver, intern(a, pkgname));
            s = qs ? sn_scope_lookup_local(qs, iname) : NULL;
            if (s) { set_global(a, out, s, pkgname, name); return true; }
        }
    }
    if (a->resolver.prelude_scope) {
        s = sn_scope_lookup_local(a->resolver.prelude_scope, iname);
        if (s) {
            const char *p = lsp_symbol_package(a, s);
            /* Prelude membership is compiler/version dependent. Resolve its
             * declaring package from the graph instead of assuming the
             * current builtin layout; older/newer Snova releases can move a
             * builtin type between packages. */
            set_global(a, out, s, p, name);
            return true;
        }
    }
    return false;
}

bool lsp_lookup_name_at(const LspFileIndex *ix, const char *name, uint32_t offset, LspSymRef *out) {
    if (!ix || !ix->a || !name || !out) return false;
    const LspDocAnalysis *a = ix->a;
    const LspBinding *bd = lsp_index_visible_binding(ix, name, offset);
    if (bd) { set_local(ix, out, bd); return true; }

    const SnDecl *top = top_decl_at(a, offset);
    if (is_type_like(top) && top->span.offset < offset) {
        SnSymbol *ts = type_symbol_of_decl(a, top);
        if (ts && set_member(a, out, ts, name)) return true;
    }
    return lookup_package_level(a, name, out);
}

/* ── member access ───────────────────────────────────────────────────────── */

static SnChecker *checker_for(const LspFileIndex *ix, SnChecker *checker, SnDiagSink *sink) {
    const LspDocAnalysis *a = ix->a;
    sn_diag_init(sink, a->path ? a->path : "", "", 0);
    sink->out = NULL;
    sink->quiet = 1;
    sn_checker_init(checker, (SnArena *)&a->arena, (SnInternTable *)&a->intern, sink,
                    (SnResolver *)&a->resolver, (SnTypeTable *)&a->types);
    return checker;
}

/* Dotted chain of identifiers ending just before token `i` (exclusive). */
static bool chain_before(const LspFileIndex *ix, size_t i, char *out, size_t out_sz) {
    size_t k = i;
    size_t start = i;
    while (k >= 2 && (ix->toks[k - 1].kind == SN_TOK_DOT || ix->toks[k - 1].kind == SN_TOK_QDOT) &&
           (ix->toks[k - 2].kind == SN_TOK_IDENT || ix->toks[k - 2].kind == SN_TOK_THIS) &&
           ix->toks[k - 2].in_string == ix->toks[i].in_string) {
        start = k - 2;
        k -= 2;
    }
    if (start == i) return false;
    out[0] = '\0';
    size_t used = 0;
    for (size_t t = start; t + 1 < i; t += 2) {
        int n = snprintf(out + used, out_sz - used, "%s%s", used ? "." : "", ix->toks[t].text ? ix->toks[t].text : "this");
        if (n < 0 || (size_t)n >= out_sz - used) return false;
        used += (size_t)n;
    }
    return true;
}

static SnSymbol *type_symbol_of(const SnTypeRep *ty) {
    if (!ty) return NULL;
    if (sn_type_is_ref(ty)) ty = sn_type_pointee(ty);
    if (!ty) return NULL;
    if (ty->tag == SN_T_NAMED && ty->decl && ty->decl->kind == SN_SYM_TYPE) return ty->decl;
    return NULL;
}

static bool resolve_member(const LspFileIndex *ix, size_t i, LspSymRef *out) {
    const LspDocAnalysis *a = ix->a;
    const LspTok *t = &ix->toks[i];
    const char *name = t->text;

    /* 1. Static path: package prefix (`pkg.sub.Name`) or type name (`Type.member`). */
    char chain[SNOVAC_PATH_MAX] = "";
    bool have_chain = chain_before(ix, i, chain, sizeof(chain));
    if (have_chain) {
        SnScope *ps = sn_resolver_package_scope(&a->resolver, intern(a, chain));
        SnSymbol *s = ps ? sn_scope_lookup_local(ps, intern(a, name)) : NULL;
        if (s) { set_global(a, out, s, chain, name); return true; }
        if (t->in_import) return false; /* `import pkg.Name` names a package member only */
        if (!strchr(chain, '.') && strcmp(chain, "this") != 0 &&
            !lsp_index_visible_binding(ix, chain, t->offset)) {
            LspSymRef head;
            if (lsp_lookup_name_at(ix, chain, t->offset, &head) && head.cls == LSP_SYMREF_GLOBAL &&
                head.kind == SN_SYM_TYPE) {
                SnScope *hps = sn_resolver_package_scope(&a->resolver, intern(a, head.package));
                SnSymbol *ts = hps ? sn_scope_lookup_local(hps, intern(a, chain)) : NULL;
                if (!ts) ts = find_type_symbol(a, chain);
                if (ts && set_member(a, out, ts, name)) return true;
            }
        }
    }

    /* 2. Value receiver: infer the receiver type from the AST. */
    SnDiagSink sink;
    SnChecker checker;
    checker_for(ix, &checker, &sink);
    SnScope *scope = lsp_build_scope_at(a, &checker, t->offset, NULL, NULL);
    SnTypeRep *recv = NULL;
    for (size_t m = 0; m < ix->nmembers; m++) {
        const LspMemberSite *site = &ix->members[m];
        if (site->name_offset != t->offset) continue;
        if (site->is_field_init) {
            const SnExpr *ty_expr = site->expr->lhs;
            char tname[256] = "";
            if (ty_expr && ty_expr->kind == SN_EXPR_IDENT && ty_expr->text) snprintf(tname, sizeof(tname), "%s", ty_expr->text);
            SnSymbol *ts = tname[0] ? find_type_symbol(a, tname) : NULL;
            return ts && set_member(a, out, ts, name);
        }
        if (site->expr->lhs && site->expr->lhs->kind == SN_EXPR_THIS) {
            const SnDecl *top = top_decl_at(a, t->offset);
            SnSymbol *ts = is_type_like(top) ? type_symbol_of_decl(a, top) : NULL;
            return ts && set_member(a, out, ts, name);
        }
        recv = sn_check_expr(&checker, scope, site->expr->lhs);
        break;
    }
    if (!recv && have_chain) {
        if (strcmp(chain, "this") == 0) {
            const SnDecl *top = top_decl_at(a, t->offset);
            SnSymbol *ts = is_type_like(top) ? type_symbol_of_decl(a, top) : NULL;
            return ts && set_member(a, out, ts, name);
        }
        recv = lsp_infer_expr_type_at(a, &checker, scope, chain);
    }
    if (!recv || recv->tag == SN_T_ERROR) return false;
    SnSymbol *ts = type_symbol_of(recv);
    if (ts && set_member(a, out, ts, name)) return true;
    /* `opt?.field`: the member lives on the Option's payload type. */
    if (ts && ts->name && strcmp(ts->name, "Option") == 0 && recv->nargs >= 1) {
        SnSymbol *inner = type_symbol_of(recv->args[0]);
        if (inner && set_member(a, out, inner, name)) return true;
    }
    return false;
}

/* ── declaration sites ───────────────────────────────────────────────────── */

static bool decl_name_at(const LspFileIndex *ix, const SnDecl *d, uint32_t offset) {
    if (!d || !d->name) return false;
    uint32_t r0, r1, sel, sel_len;
    lsp_decl_outline(ix->doc, d, &r0, &r1, &sel, &sel_len);
    return offset >= sel && offset < sel + sel_len && sel_len == strlen(d->name);
}

static bool resolve_decl_site(const LspFileIndex *ix, uint32_t offset, LspSymRef *out) {
    const LspDocAnalysis *a = ix->a;
    for (size_t i = 0; i < a->unit.decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(a->unit.decls, SnDecl, i);
        if (!d) continue;
        if (d->kind != SN_DECL_EXTENSION && decl_name_at(ix, d, offset)) {
            const char *pkg = current_package(a);
            SnScope *ps = sn_resolver_package_scope(&a->resolver, intern(a, pkg));
            SnSymbol *s = ps ? sn_scope_lookup_local(ps, intern(a, d->name)) : NULL;
            set_global(a, out, s, pkg, d->name);
            if (!s) out->kind = d->kind == SN_DECL_FUNC ? SN_SYM_FUNC : SN_SYM_TYPE;
            if (!out->decl_path[0] && ix->doc->path) snprintf(out->decl_path, sizeof(out->decl_path), "%s", ix->doc->path);
            return true;
        }
        if (!is_type_like(d)) continue;
        const SnList *lists[2] = { &d->members, &d->variants };
        for (int l = 0; l < 2; l++) {
            for (size_t j = 0; j < lists[l]->len; j++) {
                const SnDecl *m = SN_LIST_AT(*lists[l], SnDecl, j);
                if (!decl_name_at(ix, m, offset)) continue;
                SnSymbol *ts = type_symbol_of_decl(a, d);
                if (ts && set_member(a, out, ts, m->name)) return true;
                memset(out, 0, sizeof(*out));
                out->cls = LSP_SYMREF_MEMBER;
                out->kind = m->kind == SN_DECL_METHOD ? SN_SYM_METHOD : SN_SYM_FIELD;
                snprintf(out->name, sizeof(out->name), "%s", m->name);
                snprintf(out->owner, sizeof(out->owner), "%s", d->name ? d->name : "");
                snprintf(out->package, sizeof(out->package), "%s", current_package(a));
                out->in_workspace = package_in_workspace(a, out->package);
                return true;
            }
        }
    }
    return false;
}

/* ── type relations ──────────────────────────────────────────────────────── */

const SnDecl *lsp_index_enclosing_type(const LspFileIndex *ix, uint32_t offset) {
    if (!ix || !ix->a) return NULL;
    const SnDecl *top = top_decl_at(ix->a, offset);
    return is_type_like(top) ? top : NULL;
}

const SnScope *lsp_type_member_scope(const LspDocAnalysis *a, const char *package, const char *owner) {
    if (!a || !owner) return NULL;
    SnScope *ps = package ? sn_resolver_package_scope(&a->resolver, intern(a, package)) : NULL;
    SnSymbol *ts = ps ? sn_scope_lookup_local(ps, intern(a, owner)) : NULL;
    if (!ts) ts = find_type_symbol(a, owner);
    return ts && ts->decl ? member_scope_of(a, ts->decl) : NULL;
}

static bool inherits_from(const LspDocAnalysis *a, const SnSymbol *type_sym, const SnSymbol *ancestor, int depth) {
    if (!type_sym || !type_sym->decl || !ancestor || depth > 8) return false;
    const SnDecl *td = type_sym->decl;
    for (size_t i = 0; i < td->supertypes.len; i++) {
        const SnType *st = SN_LIST_AT(td->supertypes, SnType, i);
        SnSymbol *super = st ? find_type_symbol(a, st->name) : NULL;
        if (!super || super == type_sym) continue;
        if (super == ancestor || (super->name && ancestor->name && strcmp(super->name, ancestor->name) == 0 &&
                                  super->decl == ancestor->decl)) {
            return true;
        }
        if (inherits_from(a, super, ancestor, depth + 1)) return true;
    }
    return false;
}

size_t lsp_member_family(const LspDocAnalysis *a, const LspSymRef *m, LspSymRef *out, size_t max) {
    if (!a || !m || !out || max == 0) return 0;
    size_t n = 0;
    out[n++] = *m;
    if (m->cls != LSP_SYMREF_MEMBER || m->kind != SN_SYM_METHOD) return n;
    SnScope *ps = sn_resolver_package_scope(&a->resolver, intern(a, m->package));
    SnSymbol *owner = ps ? sn_scope_lookup_local(ps, intern(a, m->owner)) : NULL;
    if (!owner) return n;
    const char *iname = intern(a, m->name);
    for (const SnPackageScopeEntry *pe = a->resolver.packages; pe && n < max; pe = pe->next) {
        if (!pe->scope || !pe->scope->buckets) continue;
        for (size_t bk = 0; bk < pe->scope->nbuckets && n < max; bk++) {
            for (SnSymbol *ts = pe->scope->buckets[bk]; ts && n < max; ts = ts->next) {
                if (ts->kind != SN_SYM_TYPE || ts == owner || !ts->decl) continue;
                const SnScope *ms = member_scope_of(a, ts->decl);
                if (!ms || !sn_scope_lookup_local(ms, iname)) continue;
                if (!inherits_from(a, ts, owner, 0) && !inherits_from(a, owner, ts, 0)) continue;
                LspSymRef k = *m;
                snprintf(k.owner, sizeof(k.owner), "%s", ts->name);
                snprintf(k.package, sizeof(k.package), "%s", pe->package_name);
                k.in_workspace = package_in_workspace(a, k.package);
                out[n++] = k;
            }
        }
    }
    return n;
}

/* ── public resolution ───────────────────────────────────────────────────── */

bool lsp_resolve_at(const LspFileIndex *ix, uint32_t offset, LspSymRef *out,
                    uint32_t *tok_off, uint32_t *tok_len, bool *is_decl) {
    if (is_decl) *is_decl = false;
    if (!ix || !ix->a || !out) return false;
    long ti = lsp_index_token_at(ix, offset);
    if (ti < 0) return false;
    const LspTok *t = &ix->toks[ti];
    if (t->kind != SN_TOK_IDENT || !t->text || t->in_package) return false;
    if (tok_off) *tok_off = t->offset;
    if (tok_len) *tok_len = t->len;

    if (t->in_import) {
        if (!lsp_index_token_is_qualified(ix, (size_t)ti)) return false;
        return resolve_member(ix, (size_t)ti, out);
    }
    if (lsp_index_token_is_qualified(ix, (size_t)ti)) return resolve_member(ix, (size_t)ti, out);

    for (size_t m = 0; m < ix->nmembers; m++) {
        if (ix->members[m].name_offset == t->offset && ix->members[m].is_field_init) {
            return resolve_member(ix, (size_t)ti, out);
        }
    }

    const LspBinding *bd = lsp_index_binding_at(ix, t->offset);
    if (bd) {
        set_local(ix, out, bd);
        if (is_decl) *is_decl = true;
        return true;
    }
    if (!t->in_string && resolve_decl_site(ix, t->offset, out)) {
        if (is_decl) *is_decl = true;
        return true;
    }
    return lsp_lookup_name_at(ix, t->text, t->offset, out);
}

bool lsp_symref_same(const LspSymRef *x, const LspSymRef *y) {
    if (!x || !y || x->cls != y->cls || x->cls == LSP_SYMREF_NONE) return false;
    if (strcmp(x->name, y->name) != 0) return false;
    switch (x->cls) {
        case LSP_SYMREF_LOCAL:
            return x->decl_offset == y->decl_offset && strcmp(x->decl_path, y->decl_path) == 0;
        case LSP_SYMREF_GLOBAL:
            return strcmp(x->package, y->package) == 0;
        case LSP_SYMREF_MEMBER:
            return strcmp(x->package, y->package) == 0 && strcmp(x->owner, y->owner) == 0;
        default:
            return false;
    }
}

bool lsp_is_valid_identifier(const char *name) {
    if (!name || !name[0]) return false;
    if (!(isalpha((unsigned char)name[0]) || name[0] == '_')) return false;
    for (const char *p = name + 1; *p; p++) {
        if (!(isalnum((unsigned char)*p) || *p == '_')) return false;
    }
    if (strcmp(name, "_") == 0 || strcmp(name, "this") == 0) return false;
    if (sn_builtin_is_primitive_name(name)) return false;

    /* Keywords: whatever the lexer does not lex as a plain identifier. */
    SnArena arena;
    sn_arena_init(&arena, 4096);
    SnDiagSink sink;
    sn_diag_init(&sink, "", name, strlen(name));
    sink.out = NULL;
    sink.quiet = 1;
    SnTokenVec toks;
    memset(&toks, 0, sizeof(toks));
    sn_lex(&arena, &sink, name, strlen(name), &toks);
    bool ok = toks.len >= 1 && toks.data[0].kind == SN_TOK_IDENT && toks.data[0].span.len == strlen(name);
    sn_arena_free(&arena);
    return ok;
}
