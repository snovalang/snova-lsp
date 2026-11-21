#ifndef SNOVA_LSP_IDENT_H
#define SNOVA_LSP_IDENT_H

/*
 * Identifier resolution with symbol identity, the base for references,
 * document highlight and rename.
 *
 * gopls answers "which object does this identifier denote?" from
 * types.Info.Uses/Defs and compares objects across packages by objectpath.
 * snovac records no such map, so this module rebuilds the two pieces it
 * needs:
 *
 *   - a lexical scope model per file (LspFileIndex): brace-matched blocks plus
 *     every local binding (let/var, parameters, for, catch, match arms, lambda
 *     parameters) with the region in which it is visible, the analogue of
 *     go/types Scope.LookupParent(name, pos);
 *   - a stable symbol key (LspSymRef) that survives across separately
 *     analysed files: LOCAL(file, binding offset), GLOBAL(package, name) and
 *     MEMBER(package, type, name), the analogue of gopls objectpath.
 *
 * Resolution follows the compiler's order (resolve.h): innermost local
 * binding, members of the enclosing type and its supertypes, the current
 * package, imports, prelude. A member access `recv.name` infers the
 * receiver type from the AST, so calls, indexing and chains work.
 * Identifiers inside `${...}` string interpolation are indexed too, since a
 * rename that skipped them would break the program.
 */

#include "lsp_analysis.h"
#include "lsp_document.h"

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    SnTokKind kind;
    uint32_t offset;
    uint32_t len;
    const char *text;
    uint8_t in_string;  /* lexed from a ${...} interpolation */
    uint8_t in_import;  /* path segment of an `import` line */
    uint8_t in_package; /* path segment of the `package` line */
} LspTok;

typedef struct {
    uint32_t open;  /* offset of '{' */
    uint32_t close; /* offset of the matching '}' (text end if unbalanced) */
} LspBlock;

typedef struct {
    const char *name;
    uint32_t offset; /* binding identifier */
    uint32_t len;
    uint32_t scope_start;  /* visibility region [scope_start, scope_end) */
    uint32_t scope_end;
    uint32_t visible_from; /* references at or after this offset see it */
    SnSymbolKind kind;     /* SN_SYM_LOCAL or SN_SYM_PARAM */
    uint8_t is_mutable;
    const SnStmt *stmt;    /* LET/VAR statement, NULL for other bindings */
    const SnDecl *routine; /* routine owning a parameter */
} LspBinding;

typedef struct {
    uint32_t name_offset; /* member name token (after '.') or struct-literal field */
    const SnExpr *expr;   /* MEMBER expression, or the STRUCT_LIT for a field */
    uint8_t is_field_init;
} LspMemberSite;

typedef struct {
    const LspDocAnalysis *a;
    const LspDocument *doc;
    LspTok *toks;
    size_t ntoks;
    LspBlock *blocks; /* sorted by open */
    size_t nblocks;
    LspBinding *binds;
    size_t nbinds;
    LspMemberSite *members;
    size_t nmembers;
} LspFileIndex;

void lsp_index_build(LspFileIndex *ix, const LspDocAnalysis *a, const LspDocument *doc);
void lsp_index_free(LspFileIndex *ix);

typedef enum {
    LSP_SYMREF_NONE = 0,
    LSP_SYMREF_LOCAL,  /* local binding or parameter */
    LSP_SYMREF_GLOBAL, /* package-level declaration */
    LSP_SYMREF_MEMBER  /* field, method, constant or variant of a type */
} LspSymRefClass;

#define LSP_IDENT_NAME_MAX 128

typedef struct {
    LspSymRefClass cls;
    SnSymbolKind kind;
    char name[LSP_IDENT_NAME_MAX];
    char package[SNOVAC_PATH_MAX];
    char owner[LSP_IDENT_NAME_MAX]; /* MEMBER: declaring type */
    char decl_path[SNOVAC_PATH_MAX];
    uint32_t decl_offset; /* LOCAL: binding identifier offset */
    uint32_t scope_start, scope_end; /* LOCAL */
    bool in_workspace; /* declared in a project package (renamable) */
    bool is_mutable;   /* LOCAL declared with `var` */
} LspSymRef;

/* What the identifier at `offset` denotes. Fills the token range when
 * non-NULL and reports whether the token is the declaration itself. */
bool lsp_resolve_at(const LspFileIndex *ix, uint32_t offset, LspSymRef *out,
                    uint32_t *tok_off, uint32_t *tok_len, bool *is_decl);

/* What an unqualified `name` would denote at `offset`, as if typed there. */
bool lsp_lookup_name_at(const LspFileIndex *ix, const char *name, uint32_t offset, LspSymRef *out);

bool lsp_symref_same(const LspSymRef *x, const LspSymRef *y);

/* Index of the token covering `offset`, or -1. */
long lsp_index_token_at(const LspFileIndex *ix, uint32_t offset);

/* True when token `i` follows `.` or `?.` (a qualified or member name). */
bool lsp_index_token_is_qualified(const LspFileIndex *ix, size_t i);

/* Local binding whose identifier sits at `offset`, or NULL. */
const LspBinding *lsp_index_binding_at(const LspFileIndex *ix, uint32_t offset);

/* Innermost local binding named `name` visible at `offset`, or NULL. */
const LspBinding *lsp_index_visible_binding(const LspFileIndex *ix, const char *name, uint32_t offset);

/* Package that declares `sym` (by scope membership), or NULL. */
const char *lsp_symbol_package(const LspDocAnalysis *a, const SnSymbol *sym);

/* Type declaration (class/struct/enum/interface/extension) enclosing `offset`. */
const SnDecl *lsp_index_enclosing_type(const LspFileIndex *ix, uint32_t offset);

/*
 * Members that must be renamed together with `m`, `m` included: a method and
 * every same-named method declared by an ancestor or descendant type, so an
 * override or an interface implementation keeps matching (gopls checkMethod).
 * Fields have no family. Returns the number of keys written to `out`.
 */
size_t lsp_member_family(const LspDocAnalysis *a, const LspSymRef *m, LspSymRef *out, size_t max);

/* Member scope of the type named `owner` in `package`, or NULL. */
const SnScope *lsp_type_member_scope(const LspDocAnalysis *a, const char *package, const char *owner);

/* Valid Snovalang identifier that is not a keyword or primitive type name. */
bool lsp_is_valid_identifier(const char *name);

#endif
