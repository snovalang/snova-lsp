#ifndef SNOVA_LSP_FUZZY_H
#define SNOVA_LSP_FUZZY_H

/*
 * Fuzzy matcher ported from gopls (golang.org/x/tools/gopls/internal/fuzzy).
 *
 * Every candidate byte gets a role (word head, lower-case tail, upper-case
 * tail, separator) and a dynamic program aligns the pattern with the
 * candidate. Matches on word heads, consecutive runs and the last dotted
 * segment score higher; a pattern character matched in the middle of a word
 * is penalized. The result is normalized to [0, 1]: 0 means no match and a
 * prefix or same-length match scores 1.
 *
 * Snovalang qualified names use '.' as the segment separator
 * (`builtin.io.console.output`), so the last segment of a dotted name wins,
 * which is what an import path or a `Type.member` label needs.
 *
 * Pure function of (pattern, candidate): deterministic and allocation free.
 */

#define LSP_FUZZY_MAX_INPUT 127
#define LSP_FUZZY_MAX_PATTERN 63

typedef struct LspFuzzyMatcher LspFuzzyMatcher;

/* Heap-allocates a matcher for `pattern` (truncated to LSP_FUZZY_MAX_PATTERN). */
LspFuzzyMatcher *lsp_fuzzy_new(const char *pattern);
void lsp_fuzzy_free(LspFuzzyMatcher *m);

/* Score in [0, 1]. An empty pattern matches everything with 1. */
float lsp_fuzzy_score(LspFuzzyMatcher *m, const char *candidate);

/* One-shot helper for callers that score a single candidate. */
float lsp_fuzzy_match(const char *pattern, const char *candidate);

#endif
