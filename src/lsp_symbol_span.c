#include "lsp_symbol_span.h"

#include <ctype.h>
#include <string.h>

static int ident_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static int find_name(const char *text, size_t n, uint32_t from, const char *name,
                     uint32_t *off, uint32_t *len) {
    size_t name_len;
    size_t limit;
    size_t i;

    if (!text || !name || !name[0] || from > n) return 0;
    name_len = strlen(name);
    limit = from + 256;
    if (limit > n) limit = n;
    for (i = from; i + name_len <= limit; i++) {
        if (memcmp(text + i, name, name_len) != 0) continue;
        if (i > 0 && ident_char((unsigned char)text[i - 1])) continue;
        if (i + name_len < n && ident_char((unsigned char)text[i + name_len])) continue;
        *off = (uint32_t)i;
        *len = (uint32_t)name_len;
        return 1;
    }
    return 0;
}

/* End offset just after the `}` that closes the first `{` at or after `from`.
   Returns `from` when the declaration has no brace. */
static uint32_t closing_brace(const char *text, size_t n, uint32_t from) {
    int depth = 0;
    int seen = 0;
    int mode = 0; /* 1 line comment, 2 block comment, 3 string */
    size_t i;

    if (!text || from > n) return from;
    for (i = from; i < n; i++) {
        char c = text[i];
        char next = (i + 1 < n) ? text[i + 1] : 0;
        if (mode == 1) {
            if (c == '\n') mode = 0;
            continue;
        }
        if (mode == 2) {
            if (c == '*' && next == '/') {
                mode = 0;
                i++;
            }
            continue;
        }
        if (mode == 3) {
            if (c == '\\' && next) {
                i++;
                continue;
            }
            if (c == '"') mode = 0;
            continue;
        }
        if (c == '/' && next == '/') {
            mode = 1;
            i++;
            continue;
        }
        if (c == '/' && next == '*') {
            mode = 2;
            i++;
            continue;
        }
        if (c == '"') {
            mode = 3;
            continue;
        }
        if (c == '{') {
            depth++;
            seen = 1;
            continue;
        }
        if (c == '}') {
            if (depth > 0) depth--;
            if (seen && depth == 0) return (uint32_t)(i + 1);
        }
    }
    return from;
}

static uint32_t statement_end(const char *text, size_t n, uint32_t from) {
    size_t i;
    if (!text || from > n) return from;
    for (i = from; i < n; i++) {
        if (text[i] == ';') return (uint32_t)(i + 1);
        if (text[i] == '\n') return (uint32_t)i;
        if (text[i] == '{') {
            uint32_t end = closing_brace(text, n, (uint32_t)i);
            if (end > i) return end;
        }
    }
    return (uint32_t)n;
}

static int kind_uses_brace(SnDeclKind kind) {
    switch (kind) {
        case SN_DECL_CLASS:
        case SN_DECL_STRUCT:
        case SN_DECL_INTERFACE:
        case SN_DECL_ENUM:
        case SN_DECL_FUNC:
        case SN_DECL_METHOD:
        case SN_DECL_EXTENSION:
            return 1;
        case SN_DECL_FIELD:
        case SN_DECL_CONST:
        case SN_DECL_VARIANT:
        case SN_DECL_TYPEALIAS:
            return 0;
        default:
            return 0;
    }
}

static void cover(uint32_t *start, uint32_t *end, uint32_t child_off, uint32_t child_end) {
    if (child_off < *start) *start = child_off;
    if (child_end > *end) *end = child_end;
}

static void clamp_selection(uint32_t *start, uint32_t *end, uint32_t *sel_off, uint32_t *sel_len) {
    if (*end < *start) *end = *start;
    if (*sel_off < *start) *sel_off = *start;
    if (*sel_off > *end) *sel_off = *start;
    if (*sel_off + *sel_len > *end) {
        *sel_len = *end - *sel_off;
    }
    if (*sel_len == 0 && *end > *sel_off) *sel_len = 1;
    if (*end == *start) *end = *start + (*sel_len > 0 ? *sel_len : 1);
    if (*sel_off + *sel_len > *end) *sel_len = *end - *sel_off;
}

void lsp_keyword_name_outline(const LspDocument *doc, uint32_t kw_off, uint32_t kw_len,
                              const char *name,
                              uint32_t *range_off, uint32_t *range_end,
                              uint32_t *sel_off, uint32_t *sel_len) {
    uint32_t start = kw_off;
    uint32_t end = kw_off + kw_len;
    uint32_t name_off = start;
    uint32_t name_len = kw_len > 0 ? kw_len : 1;
    const char *text = doc ? doc->text : NULL;
    size_t n = doc ? doc->text_len : 0;

    if (text && find_name(text, n, start, name, &name_off, &name_len)) {
        if (name_off + name_len > end) end = name_off + name_len;
        if (name_off < start) start = name_off;
    }
    if (text) {
        uint32_t stmt = statement_end(text, n, name_off);
        if (stmt > end) end = stmt;
    }
    clamp_selection(&start, &end, &name_off, &name_len);
    *range_off = start;
    *range_end = end;
    *sel_off = name_off;
    *sel_len = name_len;
}

static void outline_children(const LspDocument *doc, const SnList *kids,
                             uint32_t *start, uint32_t *end) {
    size_t i;
    if (!kids) return;
    for (i = 0; i < kids->len; i++) {
        const SnDecl *child = SN_LIST_AT(*kids, const SnDecl, i);
        uint32_t child_off = 0, child_end = 0, sel_off = 0, sel_len = 0;
        if (!child) continue;
        lsp_decl_outline(doc, child, &child_off, &child_end, &sel_off, &sel_len);
        cover(start, end, child_off, child_end);
    }
}

void lsp_decl_outline(const LspDocument *doc, const SnDecl *decl,
                      uint32_t *range_off, uint32_t *range_end,
                      uint32_t *sel_off, uint32_t *sel_len) {
    uint32_t start = 0;
    uint32_t end = 1;
    uint32_t name_off = 0;
    uint32_t name_len = 1;
    const char *text = doc ? doc->text : NULL;
    size_t n = doc ? doc->text_len : 0;

    if (!decl) {
        *range_off = 0;
        *range_end = 1;
        *sel_off = 0;
        *sel_len = 1;
        return;
    }

    start = decl->span.offset;
    end = start + (decl->span.len > 0 ? decl->span.len : 1);
    name_off = start;
    name_len = decl->span.len > 0 ? decl->span.len : 1;

    if (text && start > n) start = 0;
    if (text && find_name(text, n, start, decl->name, &name_off, &name_len)) {
        if (name_off < start) start = name_off;
        if (name_off + name_len > end) end = name_off + name_len;
    }

    if (text && kind_uses_brace(decl->kind)) {
        uint32_t brace = closing_brace(text, n, start);
        if (brace > end) {
            end = brace;
        } else {
            uint32_t stmt = statement_end(text, n, name_off);
            if (stmt > end) end = stmt;
        }
    } else if (text) {
        uint32_t stmt = statement_end(text, n, name_off);
        if (stmt > end) end = stmt;
    }

    outline_children(doc, &decl->members, &start, &end);
    outline_children(doc, &decl->variants, &start, &end);
    clamp_selection(&start, &end, &name_off, &name_len);

    *range_off = start;
    *range_end = end;
    *sel_off = name_off;
    *sel_len = name_len;
}
