#ifndef SNOVA_LSP_SYMBOL_SPAN_H
#define SNOVA_LSP_SYMBOL_SPAN_H

#include "lsp_document.h"
#include "ast.h"

/* Outline range for a declaration, with the identifier as selectionRange.
   The parser span is only the opening keyword. selection is always inside
   range, and nested member ranges are inside the parent range. */
void lsp_decl_outline(const LspDocument *doc, const SnDecl *decl,
                      uint32_t *range_off, uint32_t *range_end,
                      uint32_t *sel_off, uint32_t *sel_len);

/* Range covering a keyword and the name that follows it. */
void lsp_keyword_name_outline(const LspDocument *doc, uint32_t kw_off, uint32_t kw_len,
                              const char *name,
                              uint32_t *range_off, uint32_t *range_end,
                              uint32_t *sel_off, uint32_t *sel_len);

#endif /* SNOVA_LSP_SYMBOL_SPAN_H */
