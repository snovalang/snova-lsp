#include "lsp_imports.h"

#include <stdio.h>
#include <string.h>

static bool starts_with_segment(const char *name, const char *package) {
    size_t n = strlen(package);
    return strncmp(name, package, n) == 0 && (name[n] == '\0' || name[n] == '.');
}

LspPackageOrigin lsp_package_origin(const SnPackageNode *node) {
    if (!node || !node->name) return LSP_PKG_PROJECT;
    if (strncmp(node->name, "builtin.", 8) == 0 || strcmp(node->name, "builtin") == 0) {
        return LSP_PKG_BUILTIN;
    }
    for (const SnPackageFile *f = node->files; f; f = f->next) {
        if (f->path && (strstr(f->path, "/.snovalang/deps/") || strstr(f->path, "\\.snovalang\\deps\\"))) {
            return LSP_PKG_DEPENDENCY;
        }
    }
    return LSP_PKG_PROJECT;
}

bool lsp_import_present(const LspDocAnalysis *a, const char *package) {
    if (!a || !package || !package[0]) return false;
    for (size_t i = 0; i < a->unit.imports.len; i++) {
        const char *imp = SN_LIST_AT(a->unit.imports, const char, i);
        if (imp && starts_with_segment(imp, package)) return true;
    }
    return false;
}

static int shared_segments(const char *x, const char *y) {
    int shared = 0;
    while (*x && *y) {
        const char *xe = strchr(x, '.');
        const char *ye = strchr(y, '.');
        size_t xl = xe ? (size_t)(xe - x) : strlen(x);
        size_t yl = ye ? (size_t)(ye - y) : strlen(y);
        if (xl != yl || strncmp(x, y, xl) != 0) break;
        shared++;
        if (!xe || !ye) break;
        x = xe + 1;
        y = ye + 1;
    }
    return shared;
}

int lsp_import_context_score(const LspDocAnalysis *a, const char *package) {
    if (!a || !package) return 0;
    const SnPackageNode *target = sn_pkggraph_find((SnPackageGraph *)&a->graph, package);
    if (!target) return 0;

    LspPackageOrigin origin = lsp_package_origin(target);
    int score = origin == LSP_PKG_PROJECT ? 14 : origin == LSP_PKG_DEPENDENCY ? 8 : 4;

    const char *current = a->unit.package;
    int uses = 0;
    bool used_by_same_package = false;
    for (const SnPackageNode *n = a->graph.nodes; n; n = n->next) {
        if (lsp_package_origin(n) != LSP_PKG_PROJECT) continue;
        for (const SnPackageFile *f = n->files; f; f = f->next) {
            if (a->path && f->path && strcmp(a->path, f->path) == 0) continue;
            for (size_t i = 0; i < f->imports.len; i++) {
                const char *imp = SN_LIST_AT(f->imports, const char, i);
                if (!imp || !starts_with_segment(imp, package)) continue;
                uses++;
                if (current && n->name && strcmp(n->name, current) == 0) used_by_same_package = true;
                break;
            }
        }
    }
    score += (uses > 5 ? 5 : uses) * 5;
    if (used_by_same_package) score += 10;

    if (origin == LSP_PKG_PROJECT && current) {
        int shared = shared_segments(current, package);
        score += (shared > 3 ? 3 : shared) * 3;
    }
    return score;
}

typedef struct {
    size_t start;   /* offset of the first byte of the line */
    size_t end;     /* offset of the line terminator (or text end) */
    bool has_newline;
} Line;

static bool next_line(const LspDocument *doc, size_t *offset, Line *line) {
    if (*offset > doc->text_len) return false;
    if (*offset == doc->text_len && *offset != 0 && doc->text[*offset - 1] == '\n') return false;
    line->start = *offset;
    size_t p = *offset;
    while (p < doc->text_len && doc->text[p] != '\n' && doc->text[p] != '\r') p++;
    line->end = p;
    line->has_newline = p < doc->text_len;
    if (p < doc->text_len && doc->text[p] == '\r') p++;
    if (p < doc->text_len && doc->text[p] == '\n') p++;
    *offset = line->has_newline ? p : doc->text_len + 1;
    return true;
}

/* Copies the import target of `import a.b.C;` into out (without `;`). */
static bool import_name(const char *s, size_t len, char *out, size_t out_sz) {
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    if (len < 7 || strncmp(s, "import", 6) != 0 || (s[6] != ' ' && s[6] != '\t')) return false;
    s += 7; len -= 7;
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    size_t n = 0;
    while (n < len && s[n] != ';' && s[n] != ' ' && s[n] != '\t' && s[n] != '/') n++;
    if (n == 0 || n >= out_sz) return false;
    memcpy(out, s, n);
    out[n] = '\0';
    return true;
}

static bool line_is(const char *s, size_t len, const char *keyword) {
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    size_t k = strlen(keyword);
    return len > k && strncmp(s, keyword, k) == 0 && (s[k] == ' ' || s[k] == '\t');
}

static bool line_is_blank_or_comment(const char *s, size_t len) {
    while (len && (*s == ' ' || *s == '\t')) { s++; len--; }
    return len == 0 || (len >= 2 && s[0] == '/' && (s[1] == '/' || s[1] == '*')) || s[0] == '*';
}

typedef struct {
    uint32_t line;
    size_t end;
    bool has_newline;
    int group; /* blank lines split the block into groups */
    char name[SNOVAC_PATH_MAX];
} ImportLine;

/*
 * Group choice follows golang.org/x/tools/go/ast/astutil.AddNamedImport: the
 * new import joins the group holding the import with the longest shared
 * path prefix (in dotted segments), sorted inside it. Without any shared
 * prefix a `builtin.` path joins the builtin group and anything else the
 * last group, mirroring goimports' standard-library-first split. A file with
 * one group therefore keeps plain alphabetical order.
 */
bool lsp_import_edit(const LspDocument *doc, const char *path, LspImportEdit *out) {
    if (!doc || !path || !path[0] || !out) return false;
    const char *nl = (doc->text && memchr(doc->text, '\r', doc->text_len)) ? "\r\n" : "\n";

    uint32_t line_no = 0;
    bool have_package = false;
    uint32_t package_line = 0;
    bool package_has_nl = true;
    size_t package_end = 0;
    int with_semicolon = 0, without_semicolon = 0;
    ImportLine imports[256];
    size_t n = 0;
    int group = 0;
    bool blank_since_import = false;

    size_t offset = 0;
    Line line;
    while (doc->text && next_line(doc, &offset, &line)) {
        const char *s = doc->text + line.start;
        size_t len = line.end - line.start;
        char name[SNOVAC_PATH_MAX];
        if (line_is(s, len, "package")) {
            if (!have_package) {
                have_package = true;
                package_line = line_no;
                package_end = line.end;
                package_has_nl = line.has_newline;
            }
        } else if (import_name(s, len, name, sizeof(name))) {
            if (n > 0 && blank_since_import) group++;
            blank_since_import = false;
            if (n < sizeof(imports) / sizeof(imports[0])) {
                imports[n].line = line_no;
                imports[n].end = line.end;
                imports[n].has_newline = line.has_newline;
                imports[n].group = group;
                snprintf(imports[n].name, sizeof(imports[n].name), "%s", name);
                n++;
            }
            if (memchr(s, ';', len)) with_semicolon++; else without_semicolon++;
        } else if (!line_is_blank_or_comment(s, len)) {
            break; /* first declaration: the import block is over */
        } else if (n > 0) {
            blank_since_import = true;
        }
        line_no++;
    }

    const char *semi = with_semicolon > without_semicolon ? ";" : "";
    if (n > 0) {
        int target = -1, best_shared = 0;
        for (size_t i = 0; i < n; i++) {
            int shared = shared_segments(imports[i].name, path);
            if (shared > best_shared) { best_shared = shared; target = imports[i].group; }
        }
        if (target < 0) {
            target = imports[n - 1].group;
            if (strncmp(path, "builtin.", 8) == 0) {
                for (size_t i = 0; i < n; i++) {
                    if (strncmp(imports[i].name, "builtin.", 8) == 0) { target = imports[i].group; break; }
                }
            }
        }
        const ImportLine *before = NULL, *last = NULL;
        for (size_t i = 0; i < n; i++) {
            if (imports[i].group != target) continue;
            last = &imports[i];
            if (!before && strcmp(imports[i].name, path) > 0) before = &imports[i];
        }
        if (before) {
            out->pos = (LspPosition){ before->line, 0 };
            snprintf(out->text, sizeof(out->text), "import %s%s%s", path, semi, nl);
        } else if (last->has_newline) {
            out->pos = (LspPosition){ last->line + 1, 0 };
            snprintf(out->text, sizeof(out->text), "import %s%s%s", path, semi, nl);
        } else {
            out->pos = lsp_offset_to_pos(doc, (uint32_t)last->end);
            snprintf(out->text, sizeof(out->text), "%simport %s%s", nl, path, semi);
        }
        return true;
    }
    if (have_package) {
        if (package_has_nl) {
            out->pos = (LspPosition){ package_line + 1, 0 };
            snprintf(out->text, sizeof(out->text), "%simport %s%s", nl, path, nl);
        } else {
            out->pos = lsp_offset_to_pos(doc, (uint32_t)package_end);
            snprintf(out->text, sizeof(out->text), "%s%simport %s", nl, nl, path);
        }
        return true;
    }
    out->pos = (LspPosition){ 0, 0 };
    snprintf(out->text, sizeof(out->text), "import %s%s%s", path, nl, nl);
    return true;
}
