#ifndef SNOVA_LSP_IMPORTS_H
#define SNOVA_LSP_IMPORTS_H

#include "lsp_analysis.h"
#include "lsp_document.h"

#include <stdbool.h>

/* Where a package comes from. Project packages are the developer's own code. */
typedef enum {
    LSP_PKG_PROJECT = 0,
    LSP_PKG_DEPENDENCY,
    LSP_PKG_BUILTIN
} LspPackageOrigin;

LspPackageOrigin lsp_package_origin(const SnPackageNode *node);

/* True when the unit already imports `package` itself or `package.Name`. */
bool lsp_import_present(const LspDocAnalysis *a, const char *package);

/*
 * How strongly the developer's code points at `package`, from 0 upward:
 * project packages beat dependencies beat builtins, packages other project
 * files already import score higher (more so when a file of the same package
 * imports it), and project packages that share leading segments with the
 * current package get a small affinity bonus.
 */
int lsp_import_context_score(const LspDocAnalysis *a, const char *package);

typedef struct {
    LspPosition pos;
    char text[SNOVAC_PATH_MAX + 64];
} LspImportEdit;

/*
 * Zero-width insertion that adds `import <path>`. With existing imports it
 * lands at the alphabetical position inside the import block and copies the
 * block's semicolon style; otherwise it goes after the package line, or at the
 * top of the file. Line endings follow the document.
 */
bool lsp_import_edit(const LspDocument *doc, const char *path, LspImportEdit *out);

#endif
