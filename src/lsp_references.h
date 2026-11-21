#ifndef SNOVA_LSP_REFERENCES_H
#define SNOVA_LSP_REFERENCES_H

#include "lsp_analysis.h"

/*
 * Symbol-identity navigation and rename, modelled on gopls
 * (golang/references.go, highlight.go, rename.go, rename_check.go,
 * symbols.go) on top of lsp_ident.c.
 *
 * References resolve the identifier under the cursor to a symbol key and
 * report every identifier in the workspace that resolves to the same key,
 * closed files included. Locals are only searched inside their scope; other
 * symbols only in project files that mention the name.
 */

char *lsp_references_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition,
                           bool include_declaration);
char *lsp_implementation_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition);

/* textDocument/documentHighlight: same-file references, Write for
 * declarations and assignment targets, Read otherwise. */
char *lsp_document_highlight_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition);

/*
 * textDocument/prepareRename and textDocument/rename. On refusal they return
 * NULL and set *error to a heap-allocated message for a JSON-RPC error.
 * Rename refuses symbols outside the workspace, invalid identifiers and
 * keywords, and every gopls lexical conflict: a same-block redeclaration, a
 * reference that the new name would shadow (sub-block) or a reference that
 * the renamed symbol would capture (super-block), and member collisions.
 * Method overrides and implementations are renamed together, and the Snova
 * docstring tags `Doc:{Name}` / `Param{name}` follow the rename.
 */
char *lsp_prepare_rename_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition,
                               char **error);
char *lsp_rename_query(LspAnalysisEngine *, LspDocStore *, const LspDocument *, LspPosition,
                       const char *new_name, char **error);

/* workspace/symbol: project declarations ranked with the gopls fuzzy matcher. */
char *lsp_workspace_symbol_query(LspAnalysisEngine *, LspDocStore *, const char *query);

#endif
