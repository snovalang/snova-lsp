#include "lsp_symbols.h"
#include "lsp_symbol_span.h"
#include "json.h"

#include <stdlib.h>
#include <string.h>

static void emit_position(JsonBuilder *jb, const char *key, uint32_t line, uint32_t character) {
    jb_key(jb, key);
    jb_start_obj(jb);
    jb_kv_int(jb, "line", (int)line);
    jb_kv_int(jb, "character", (int)character);
    jb_end_obj(jb);
}

static void emit_range(JsonBuilder *jb, const char *key, LspRange range) {
    jb_key(jb, key);
    jb_start_obj(jb);
    emit_position(jb, "start", range.start.line, range.start.character);
    emit_position(jb, "end", range.end.line, range.end.character);
    jb_end_obj(jb);
}

static void emit_decl_symbol(JsonBuilder *jb, const LspDocument *doc, const SnDecl *d) {
    if (!d || !d->name) return;

    LspSymbolKind sym_kind = LSP_SYMBOL_VARIABLE;
    const char *detail = "";

    switch (d->kind) {
        case SN_DECL_CLASS: sym_kind = LSP_SYMBOL_CLASS; detail = "class"; break;
        case SN_DECL_STRUCT: sym_kind = LSP_SYMBOL_STRUCT; detail = "struct"; break;
        case SN_DECL_INTERFACE: sym_kind = LSP_SYMBOL_INTERFACE; detail = "interface"; break;
        case SN_DECL_ENUM: sym_kind = LSP_SYMBOL_ENUM; detail = "enum"; break;
        case SN_DECL_FUNC: sym_kind = LSP_SYMBOL_FUNCTION; detail = "func"; break;
        case SN_DECL_METHOD: sym_kind = LSP_SYMBOL_METHOD; detail = "method"; break;
        case SN_DECL_FIELD: sym_kind = LSP_SYMBOL_FIELD; detail = "field"; break;
        case SN_DECL_CONST: sym_kind = LSP_SYMBOL_CONSTANT; detail = "const"; break;
        case SN_DECL_VARIANT: sym_kind = LSP_SYMBOL_ENUM_MEMBER; detail = "variant"; break;
        case SN_DECL_TYPEALIAS: sym_kind = LSP_SYMBOL_TYPE_PARAM; detail = "typealias"; break;
        case SN_DECL_EXTENSION: sym_kind = LSP_SYMBOL_MODULE; detail = "extension"; break;
        default: break;
    }

    uint32_t range_off = 0, range_end = 0, sel_off = 0, sel_len = 0;
    LspRange full;
    LspRange selection;
    lsp_decl_outline(doc, d, &range_off, &range_end, &sel_off, &sel_len);
    full = lsp_span_to_range(doc, range_off, range_end > range_off ? range_end - range_off : 1, 0, 0);
    selection = lsp_span_to_range(doc, sel_off, sel_len > 0 ? sel_len : 1, 0, 0);

    jb_start_obj(jb);
    jb_kv_str(jb, "name", d->name);
    jb_kv_int(jb, "kind", (int)sym_kind);
    if (detail[0]) jb_kv_str(jb, "detail", detail);
    emit_range(jb, "range", full);
    emit_range(jb, "selectionRange", selection);

    // Children members
    bool has_children = (d->members.len > 0 || d->variants.len > 0);
    if (has_children) {
        jb_key(jb, "children");
        jb_start_arr(jb);
        for (size_t i = 0; i < d->members.len; i++) {
            const SnDecl *m = SN_LIST_AT(d->members, const SnDecl, i);
            emit_decl_symbol(jb, doc, m);
        }
        for (size_t i = 0; i < d->variants.len; i++) {
            const SnDecl *v = SN_LIST_AT(d->variants, const SnDecl, i);
            emit_decl_symbol(jb, doc, v);
        }
        jb_end_arr(jb);
    }

    jb_end_obj(jb);
}

char *lsp_document_symbols_query(LspAnalysisEngine *engine, const LspDocument *doc) {
    if (!doc) return NULL;
    LspDocAnalysis *a = lsp_engine_get_analysis(engine, doc->uri);
    if (!a) {
        a = lsp_engine_analyze_document(engine, NULL, doc);
    }
    if (!a || !a->has_ast) return NULL;

    JsonBuilder jb;
    jb_init(&jb);
    jb_start_arr(&jb);

    // Package namespace symbol if any
    if (a->unit.package) {
        uint32_t range_off = 0, range_end = 0, sel_off = 0, sel_len = 0;
        LspRange full;
        LspRange selection;
        lsp_keyword_name_outline(doc, a->unit.package_span.offset, a->unit.package_span.len,
                                 a->unit.package, &range_off, &range_end, &sel_off, &sel_len);
        full = lsp_span_to_range(doc, range_off, range_end > range_off ? range_end - range_off : 1, 0, 0);
        selection = lsp_span_to_range(doc, sel_off, sel_len > 0 ? sel_len : 1, 0, 0);
        jb_start_obj(&jb);
        jb_kv_str(&jb, "name", a->unit.package);
        jb_kv_int(&jb, "kind", (int)LSP_SYMBOL_PACKAGE);
        jb_kv_str(&jb, "detail", "package");
        emit_range(&jb, "range", full);
        emit_range(&jb, "selectionRange", selection);
        jb_end_obj(&jb);
    }

    for (size_t i = 0; i < a->unit.decls.len; i++) {
        const SnDecl *d = SN_LIST_AT(a->unit.decls, const SnDecl, i);
        emit_decl_symbol(&jb, doc, d);
    }

    jb_end_arr(&jb);
    return jb_take(&jb);
}
