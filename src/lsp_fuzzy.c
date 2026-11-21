#include "lsp_fuzzy.h"

#include <stdlib.h>
#include <string.h>

/* Port of gopls internal/fuzzy (matcher.go + input.go). Comments name the
 * gopls rule each step mirrors so the two can be compared side by side. */

typedef enum { R_NONE = 0, R_SEP, R_TAIL, R_UCTAIL, R_HEAD } Role;
typedef enum { RT_NONE = 0, RT_PUNCT, RT_LOWER, RT_UPPER } RuneType;

/* gopls `rt` table: digits and lower-case letters are "lower", '.', ':' and
 * other punctuation are "punct", '_' is "none" (so the letter after it is a
 * word head). */
static const char RT[] =
    "00000000000000000000000000000000000000000000001122222222221000000333333333333333333333333330000002222222222222222222222222200000";

#define MIN_SCORE (-10000)

struct LspFuzzyMatcher {
    char pattern[LSP_FUZZY_MAX_PATTERN + 1];
    char pattern_lower[LSP_FUZZY_MAX_PATTERN + 1];
    size_t plen;
    size_t short_len; /* min(3, plen): mid-word first-char matches need this prefix */
    int case_sensitive;
    Role pattern_roles[LSP_FUZZY_MAX_PATTERN];
    float scale;

    /* scores[i][j][k] = (value << 1) | prevK, as in gopls scoreVal. */
    int scores[LSP_FUZZY_MAX_INPUT + 1][LSP_FUZZY_MAX_PATTERN + 1][2];
    Role roles[LSP_FUZZY_MAX_INPUT];
    char cand[LSP_FUZZY_MAX_INPUT + 1];
    char lower[LSP_FUZZY_MAX_INPUT + 1];
    size_t clen;
};

static int sv(int val, int prev_k) { return val * 2 + prev_k; }
static int sval(int s) { return s >> 1; }
static int sprev(int s) { return s & 1; }

static char to_lower(char c) { return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c; }

static RuneType rune_type(unsigned char c) {
    if (c > 127) return RT_LOWER;
    return (RuneType)(RT[c] - '0');
}

/* gopls RuneRoles. */
static void rune_roles(const char *s, size_t n, Role *out) {
    RuneType prev = RT_NONE, prev2 = RT_NONE;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        RuneType curr = rune_type(c);
        Role role = R_NONE;
        if (curr == RT_LOWER) {
            role = (prev == RT_NONE || prev == RT_PUNCT) ? R_HEAD : R_TAIL;
        } else if (curr == RT_UPPER) {
            role = R_HEAD;
            if (prev == RT_UPPER && i + 1 == n) role = R_UCTAIL;
        } else if (curr == RT_PUNCT) {
            if (c == '.' || c == ':') role = R_SEP;
        }
        if (curr != RT_LOWER) {
            if (i > 1 && out[i - 1] == R_HEAD && prev2 == RT_UPPER &&
                (out[i - 2] == R_HEAD || out[i - 2] == R_UCTAIL)) {
                out[i - 1] = R_UCTAIL;
            }
        }
        out[i] = role;
        prev2 = prev;
        prev = curr;
    }
}

LspFuzzyMatcher *lsp_fuzzy_new(const char *pattern) {
    LspFuzzyMatcher *m = (LspFuzzyMatcher *)calloc(1, sizeof(LspFuzzyMatcher));
    if (!m) return NULL;
    if (!pattern) pattern = "";
    size_t n = strlen(pattern);
    if (n > LSP_FUZZY_MAX_PATTERN) n = LSP_FUZZY_MAX_PATTERN;
    memcpy(m->pattern, pattern, n);
    m->plen = n;
    for (size_t i = 0; i < n; i++) {
        m->pattern_lower[i] = to_lower(pattern[i]);
        if (m->pattern_lower[i] != pattern[i]) m->case_sensitive = 1;
    }
    m->short_len = n > 3 ? 3 : n;
    rune_roles(m->pattern, n, m->pattern_roles);
    if (n > 0) m->scale = 1.0f / (float)(4 * n); /* maxCharScore = 4 */
    return m;
}

void lsp_fuzzy_free(LspFuzzyMatcher *m) { free(m); }

static int best_k(const LspFuzzyMatcher *m, size_t i, size_t j) {
    return sval(m->scores[i][j][0]) < sval(m->scores[i][j][1]) ? 1 : 0;
}

/* gopls match(): cheap subsequence test before the dynamic program. */
static int subsequence(const LspFuzzyMatcher *m) {
    size_t j = 0;
    for (size_t i = 0; i < m->clen && j < m->plen; i++) {
        if (m->lower[i] == m->pattern_lower[j]) j++;
    }
    return j == m->plen;
}

/* gopls computeScore(). */
static int compute_score(LspFuzzyMatcher *m) {
    size_t plen = m->plen, clen = m->clen;

    for (size_t j = 0; j <= plen; j++) {
        m->scores[0][j][0] = MIN_SCORE * 2;
        m->scores[0][j][1] = MIN_SCORE * 2;
    }
    m->scores[0][0][0] = sv(0, 0);

    int segments_left = 1;
    size_t last_seg_start = 0;
    for (size_t i = 0; i < clen; i++) {
        if (m->roles[i] == R_SEP) {
            segments_left++;
            last_seg_start = i + 1;
        }
    }

    const int consecutive_bonus = 2;
    for (size_t i = 1; i <= clen; i++) {
        Role role = m->roles[i - 1];
        if (role == R_SEP && segments_left > 1) segments_left--;

        int skip_penalty = 0;
        if (i == 1 || (i - 1) == last_seg_start) skip_penalty++; /* skipping a segment start */

        for (size_t j = 0; j <= plen; j++) {
            m->scores[i][j][1] = MIN_SCORE * 2;

            int k = sval(m->scores[i - 1][j][0]) < sval(m->scores[i - 1][j][1]) ? 1 : 0;
            int skip_score = sval(m->scores[i - 1][j][k]);
            if (j != plen) skip_score -= skip_penalty; /* no penalty after the last match */
            m->scores[i][j][0] = sv(skip_score, k);

            if (j == 0 || m->lower[i - 1] != m->pattern_lower[j - 1]) continue;
            Role prole = m->pattern_roles[j - 1];

            if (role == R_TAIL && prole == R_HEAD) {
                if (j > 1) continue; /* a pattern head cannot match a candidate tail */
                /* First pattern char may land mid-word only with a 3-char run. */
                if (clen - (i - 1) < m->short_len ||
                    strncmp(m->lower + (i - 1), m->pattern_lower, m->short_len) != 0) {
                    continue;
                }
            }

            int char_score = 0;
            if (segments_left <= 1) char_score++; /* char is in the last segment */
            if (m->cand[i - 1] == m->pattern[j - 1] || (role == R_HEAD && !m->case_sensitive)) {
                char_score++; /* exact case, or head with an all-lowercase pattern */
            }
            if (role == R_TAIL && prole == R_HEAD) char_score--;
            if (j == 1 && role == R_TAIL) char_score -= 4; /* first char mid-word */

            for (int pk = 0; pk < 2; pk++) {
                int sc = sval(m->scores[i - 1][j - 1][pk]) + char_score;
                int consecutive = pk == 1 || i - 1 == 0 || i - 1 == last_seg_start;
                if (consecutive) sc += consecutive_bonus;
                if (pk == 0 && (role == R_TAIL || role == R_UCTAIL)) sc -= 3; /* unaligned */
                if (sc > sval(m->scores[i][j][1])) m->scores[i][j][1] = sv(sc, pk);
            }
        }
    }
    return sval(m->scores[clen][plen][best_k(m, clen, plen)]);
}

/* gopls poorMatch(): more than one short match in the middle of a word. */
static int poor_match(const LspFuzzyMatcher *m) {
    if (m->plen < 2) return 0;
    size_t i = m->clen, j = m->plen;
    int k = best_k(m, i, j);
    int counter = 0, run = 0;
    while (i > 0) {
        int take = k == 1;
        k = sprev(m->scores[i][j][k]);
        if (take) {
            run++;
            if (k == 0 && run < 3 && m->roles[i - 1] == R_TAIL) {
                if (++counter > 1) return 1;
            }
            j--;
        } else {
            run = 0;
        }
        i--;
    }
    return 0;
}

float lsp_fuzzy_score(LspFuzzyMatcher *m, const char *candidate) {
    if (!m) return 0.0f;
    if (!candidate) candidate = "";
    size_t n = strlen(candidate);
    if (n > LSP_FUZZY_MAX_INPUT) n = LSP_FUZZY_MAX_INPUT;
    memcpy(m->cand, candidate, n);
    m->cand[n] = '\0';
    for (size_t i = 0; i < n; i++) m->lower[i] = to_lower(candidate[i]);
    m->lower[n] = '\0';
    m->clen = n;

    if (m->plen == 0) return 1.0f;
    if (!subsequence(m)) return 0.0f;
    rune_roles(m->cand, n, m->roles);

    int sc = compute_score(m);
    if (sc <= MIN_SCORE / 2 || poor_match(m)) return 0.0f;
    if (m->plen == n) return 1.0f; /* same length after a match: perfect */
    if (sc < 0) sc = 0;
    float norm = (float)sc * m->scale;
    return norm > 1.0f ? 1.0f : norm;
}

float lsp_fuzzy_match(const char *pattern, const char *candidate) {
    LspFuzzyMatcher *m = lsp_fuzzy_new(pattern);
    float s = lsp_fuzzy_score(m, candidate);
    lsp_fuzzy_free(m);
    return s;
}
