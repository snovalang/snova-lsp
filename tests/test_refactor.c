/* Tests for the gopls-style features: fuzzy matcher, symbol-identity
 * references, document highlight, rename with conflict checks, workspace
 * symbols, and the refactoring code actions. */

#include "lsp_analysis.h"
#include "lsp_code_action.h"
#include "lsp_completion.h"
#include "lsp_document.h"
#include "lsp_fuzzy.h"
#include "lsp_imports.h"
#include "lsp_references.h"
#include "json.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WS_ROOT "/tmp/snova_lsp_refactor_ws"

static int failures = 0;

#define CHECK(cond, msg)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);    \
            failures++;                                                        \
        }                                                                      \
    } while (0)

typedef struct {
    LspDocStore store;
    LspAnalysisEngine engine;
} Env;

static void env_init(Env *env, const char *root) {
    lsp_docstore_init(&env->store);
    lsp_engine_init(&env->engine, root);
}

static void env_free(Env *env) {
    lsp_engine_destroy(&env->engine);
    lsp_docstore_destroy(&env->store);
}

static LspDocument *open_doc(Env *env, const char *uri, const char *text) {
    LspDocument *doc = lsp_docstore_open(&env->store, uri, 1, text, strlen(text));
    assert(doc);
    lsp_engine_analyze_document(&env->engine, &env->store, doc);
    return doc;
}

/* Position of the `nth` (0-based) occurrence of `needle`, plus `delta` chars. */
static LspPosition pos_of(const LspDocument *doc, const char *needle, int nth, int delta) {
    const char *p = doc->text;
    for (int i = 0; p && i <= nth; i++) {
        p = strstr(i == 0 ? p : p + 1, needle);
    }
    assert(p && "needle not found");
    return lsp_offset_to_pos(doc, (uint32_t)(p - doc->text) + (uint32_t)delta);
}

static void write_file(const char *path, const char *text) {
    FILE *f = fopen(path, "w");
    assert(f);
    fputs(text, f);
    fclose(f);
}

static JsonVal *parse(JsonPool **pool, const char *json) {
    *pool = json_pool_create(strlen(json) + 4096);
    const char *err = NULL;
    JsonVal *v = json_parse(*pool, json, strlen(json), &err);
    assert(v);
    return v;
}

/* Applies a rename WorkspaceEdit's edits for `uri` to `text`. */
static char *apply_edits(const char *text, const JsonVal *changes, const char *uri) {
    const JsonVal *edits = json_get_arr(changes, uri);
    if (!edits) return strdup(text);
    LspDocStore tmp;
    lsp_docstore_init(&tmp);
    LspDocument *doc = lsp_docstore_open(&tmp, "file:///tmp_apply.snl", 0, text, strlen(text));
    size_t n = json_arr_len(edits);
    uint32_t *starts = calloc(n, sizeof(uint32_t)), *ends = calloc(n, sizeof(uint32_t));
    for (size_t i = 0; i < n; i++) {
        const JsonVal *r = json_get_obj(json_arr_at(edits, i), "range");
        const JsonVal *s = json_get_obj(r, "start"), *e = json_get_obj(r, "end");
        LspPosition ps = { (uint32_t)json_get_int(s, "line", 0), (uint32_t)json_get_int(s, "character", 0) };
        LspPosition pe = { (uint32_t)json_get_int(e, "line", 0), (uint32_t)json_get_int(e, "character", 0) };
        starts[i] = lsp_pos_to_offset(doc, ps);
        ends[i] = lsp_pos_to_offset(doc, pe);
    }
    size_t cap = strlen(text) + n * 64 + 1;
    char *out = malloc(cap);
    size_t o = 0, cur = 0;
    /* edits come sorted by position */
    for (size_t i = 0; i < n; i++) {
        memcpy(out + o, text + cur, starts[i] - cur);
        o += starts[i] - cur;
        const char *nt = json_get_str(json_arr_at(edits, i), "newText", "");
        memcpy(out + o, nt, strlen(nt));
        o += strlen(nt);
        cur = ends[i];
    }
    strcpy(out + o, text + cur);
    free(starts);
    free(ends);
    lsp_docstore_destroy(&tmp);
    return out;
}

static char *rename_ok(Env *env, LspDocument *doc, LspPosition pos, const char *new_name) {
    char *err = NULL;
    char *res = lsp_rename_query(&env->engine, &env->store, doc, pos, new_name, &err);
    if (err) fprintf(stderr, "  unexpected rename error: %s\n", err);
    CHECK(res && !err, "rename should succeed");
    free(err);
    return res;
}

static char *rename_err(Env *env, LspDocument *doc, LspPosition pos, const char *new_name) {
    char *err = NULL;
    char *res = lsp_rename_query(&env->engine, &env->store, doc, pos, new_name, &err);
    CHECK(!res && err, "rename should be refused");
    free(res);
    return err;
}

/* ── fuzzy ─────────────────────────────────────────────────────────────── */

static void test_fuzzy_matcher(void) {
    CHECK(lsp_fuzzy_match("", "anything") == 1.0f, "empty pattern matches all");
    CHECK(lsp_fuzzy_match("req", "requestId") == 1.0f, "prefix normalizes to 1");
    CHECK(lsp_fuzzy_match("ghr", "getHttpRequest") > 0.5f, "camel humps");
    CHECK(lsp_fuzzy_match("ghr", "github") == 0.0f, "no subsequence across humps");
    CHECK(lsp_fuzzy_match("xyz", "println") == 0.0f, "no match");
    CHECK(lsp_fuzzy_match("out", "builtin.io.console.output") >
          lsp_fuzzy_match("out", "builtin.io.output.console"), "last dotted segment wins");
    /* gopls prefers aligning every pattern char with a word head */
    /* gopls: a mid-word start needs a 3-char run and is penalized */
    CHECK(lsp_fuzzy_match("ser", "User") > 0.0f && lsp_fuzzy_match("ser", "User") < 0.2f, "mid-word start penalized");
    CHECK(lsp_fuzzy_match("se", "User") == 0.0f, "short mid-word start rejected");
    CHECK(lsp_fuzzy_match("mkusr", "makeUser") == 0.0f, "scattered mid-word matches are poor");
    printf("✓ test_fuzzy_matcher\n");
}

/* ── local rename and lexical conflicts ────────────────────────────────── */

static const char *LOCAL_SRC =
    "package main\n"
    "\n"
    "func run(n: int): int {\n"
    "    let total = n\n"
    "    if total > 0 {\n"
    "        let total = 2\n"
    "        return total\n"
    "    }\n"
    "    let label = \"sum ${total}\"\n"
    "    let other = 1\n"
    "    return total + other\n"
    "}\n";

static void test_local_rename_scopes(void) {
    Env env;
    env_init(&env, NULL);
    LspDocument *doc = open_doc(&env, "file:///local.snl", LOCAL_SRC);

    /* outer `total`: decl, `total > 0`, interpolation, `total + other`; not the inner block */
    char *res = rename_ok(&env, doc, pos_of(doc, "total", 0, 1), "sum");
    JsonPool *pool;
    JsonVal *root = parse(&pool, res ? res : "{}");
    char *after = apply_edits(doc->text, json_get_obj(root, "changes"), doc->uri);
    CHECK(strstr(after, "let sum = n"), "declaration renamed");
    CHECK(strstr(after, "if sum > 0"), "use renamed");
    CHECK(strstr(after, "\"sum ${sum}\""), "interpolation renamed");
    CHECK(strstr(after, "return sum + other"), "later use renamed");
    CHECK(strstr(after, "let total = 2") && strstr(after, "return total\n"), "inner shadowing binding untouched");
    free(after);
    json_pool_destroy(pool);
    free(res);

    /* references from the inner binding stay inside its block */
    char *refs = lsp_references_query(&env.engine, &env.store, doc, pos_of(doc, "total", 2, 1), true);
    root = parse(&pool, refs);
    CHECK(json_arr_len(root) == 2, "inner total: declaration + one use");
    json_pool_destroy(pool);
    free(refs);

    /* parameter: highlight marks the declaration as Write */
    char *hl = lsp_document_highlight_query(&env.engine, &env.store, doc, pos_of(doc, "n: int", 0, 0));
    root = parse(&pool, hl);
    CHECK(json_arr_len(root) == 2, "param n: declaration + use");
    CHECK(json_get_int(json_arr_at(root, 0), "kind", 0) == 3, "declaration highlighted as write");
    json_pool_destroy(pool);
    free(hl);

    /* same-block conflict */
    char *err = rename_err(&env, doc, pos_of(doc, "other", 0, 1), "label");
    CHECK(err && strstr(err, "same block"), "same-block conflict reported");
    free(err);
    /* sub-block conflict: outer total -> inner name would capture `return total` ... inverse:
     * renaming the parameter `n` to `total` makes `let total = n` read itself */
    err = rename_err(&env, doc, pos_of(doc, "n: int", 0, 0), "total");
    CHECK(err != NULL, "parameter rename into a shadowed name refused");
    free(err);
    /* super-block conflict: inner total -> other would hide nothing, but `label` -> `n`
     * shadows the parameter used later? `n` is not used after label, so allowed */
    free(rename_ok(&env, doc, pos_of(doc, "label", 0, 1), "n2"));
    /* invalid identifiers and keywords */
    err = rename_err(&env, doc, pos_of(doc, "other", 0, 1), "return");
    CHECK(err && strstr(err, "valid"), "keyword refused");
    free(err);
    err = rename_err(&env, doc, pos_of(doc, "other", 0, 1), "9lives");
    free(err);

    env_free(&env);
    printf("✓ test_local_rename_scopes\n");
}

static void test_super_block_conflict(void) {
    Env env;
    env_init(&env, NULL);
    LspDocument *doc = open_doc(&env, "file:///shadow.snl",
        "package main\n"
        "\n"
        "func f(limit: int): int {\n"
        "    if limit > 1 {\n"
        "        let step = 2\n"
        "        return step + limit\n"
        "    }\n"
        "    return limit\n"
        "}\n");
    /* renaming `step` to `limit` would hide the parameter read in `step + limit` */
    char *err = rename_err(&env, doc, pos_of(doc, "step", 0, 1), "limit");
    CHECK(err && strstr(err, "shadow"), "super-block conflict reported");
    free(err);
    env_free(&env);
    printf("✓ test_super_block_conflict\n");
}

/* ── workspace: members, types, imports, docstrings, closed files ──────── */

static void setup_workspace(void) {
    assert(system("rm -rf " WS_ROOT " && mkdir -p " WS_ROOT "/src/app/model " WS_ROOT "/src/app/web") == 0);
    write_file(WS_ROOT "/mod.sns", "module app\n\nsnova \"1.0.0\"\n");
    write_file(WS_ROOT "/src/app/model/User.snl",
        "package app.model\n"
        "\n"
        "/* -- Doc:{User}\n"
        " *\n"
        " * -- Description: An account holder.\n"
        " */\n"
        "public struct User {\n"
        "    public let name: string\n"
        "}\n"
        "\n"
        "public class Animal {\n"
        "    public method speak(): string = \"...\"\n"
        "}\n"
        "\n"
        "public class Dog : Animal {\n"
        "    override method speak(): string = \"woof\"\n"
        "}\n"
        "\n"
        "/* -- Doc:{makeUser}\n"
        " *\n"
        " * -- Param{who}: Display name.\n"
        " */\n"
        "public func makeUser(who: string): User {\n"
        "    return User { name: who }\n"
        "}\n");
    write_file(WS_ROOT "/src/app/web/Page.snl",
        "package app.web\n"
        "\n"
        "import app.model.User\n"
        "import app.model\n"
        "\n"
        "func title(u: User): string {\n"
        "    let d = Dog()\n"
        "    return u.name + d.speak()\n"
        "}\n");
}

static char *read_disk(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return strdup(buf);
}

static void test_workspace_rename(void) {
    setup_workspace();
    setenv("SNOVA_BUILTIN_DIR", SNOVA_TEST_BUILTIN_DIR, 1);
    Env env;
    env_init(&env, WS_ROOT);
    char *model_text = read_disk(WS_ROOT "/src/app/model/User.snl");
    char *page_text = read_disk(WS_ROOT "/src/app/web/Page.snl");
    const char *model_uri = "file://" WS_ROOT "/src/app/model/User.snl";
    const char *page_uri = "file://" WS_ROOT "/src/app/web/Page.snl";
    LspDocument *model = open_doc(&env, model_uri, model_text);
    JsonPool *pool;

    /* field: declaration, struct-literal field, member access in a closed file */
    char *refs = lsp_references_query(&env.engine, &env.store, model, pos_of(model, "name:", 0, 1), true);
    JsonVal *root = parse(&pool, refs);
    size_t n = json_arr_len(root);
    bool in_page = false;
    for (size_t i = 0; i < n; i++) in_page |= strcmp(json_get_str(json_arr_at(root, i), "uri", ""), page_uri) == 0;
    CHECK(n == 3, "field name: declaration, struct literal, u.name");
    CHECK(in_page, "reference found in a closed file");
    json_pool_destroy(pool);
    free(refs);

    char *res = rename_ok(&env, model, pos_of(model, "name:", 0, 1), "fullName");
    root = parse(&pool, res ? res : "{}");
    const JsonVal *changes = json_get_obj(root, "changes");
    char *m2 = apply_edits(model_text, changes, model_uri);
    char *p2 = apply_edits(page_text, changes, page_uri);
    CHECK(strstr(m2, "public let fullName: string"), "field declaration renamed");
    CHECK(strstr(m2, "User { fullName: who }"), "struct literal field renamed");
    CHECK(strstr(p2, "u.fullName + d.speak()"), "member access renamed in closed file");
    free(m2); free(p2);
    json_pool_destroy(pool);
    free(res);

    /* type: import line and Doc tag follow */
    res = rename_ok(&env, model, pos_of(model, "struct User", 0, 8), "Account");
    root = parse(&pool, res ? res : "{}");
    changes = json_get_obj(root, "changes");
    m2 = apply_edits(model_text, changes, model_uri);
    p2 = apply_edits(page_text, changes, page_uri);
    CHECK(strstr(m2, "Doc:{Account}"), "docstring tag renamed");
    CHECK(strstr(m2, "public struct Account {"), "type declaration renamed");
    CHECK(strstr(m2, "): Account {") && strstr(m2, "return Account { name"), "uses in declaring file renamed");
    CHECK(strstr(p2, "import app.model.Account\n"), "import line renamed");
    CHECK(strstr(p2, "func title(u: Account)"), "type use renamed in closed file");
    free(m2); free(p2);
    json_pool_destroy(pool);
    free(res);

    /* parameter: Param{who} docstring tag follows */
    res = rename_ok(&env, model, pos_of(model, "who: string", 0, 0), "displayName");
    root = parse(&pool, res ? res : "{}");
    m2 = apply_edits(model_text, json_get_obj(root, "changes"), model_uri);
    CHECK(strstr(m2, "Param{displayName}"), "param docstring tag renamed");
    CHECK(strstr(m2, "name: displayName }"), "param use renamed");
    free(m2);
    json_pool_destroy(pool);
    free(res);

    /* method family: renaming the base method renames the override and its call */
    res = rename_ok(&env, model, pos_of(model, "speak", 0, 1), "talk");
    root = parse(&pool, res ? res : "{}");
    changes = json_get_obj(root, "changes");
    m2 = apply_edits(model_text, changes, model_uri);
    p2 = apply_edits(page_text, changes, page_uri);
    CHECK(strstr(m2, "public method talk()") && strstr(m2, "override method talk()"), "override renamed with base");
    CHECK(strstr(p2, "d.talk()"), "call through subclass renamed");
    free(m2); free(p2);
    json_pool_destroy(pool);
    free(res);

    /* package-level collision */
    char *err = rename_err(&env, model, pos_of(model, "makeUser(", 0, 1), "Dog");
    CHECK(err && strstr(err, "already declared"), "package collision reported");
    free(err);

    /* prepareRename on a builtin type is refused; on a project symbol returns the range */
    char *perr = NULL;
    char *prep = lsp_prepare_rename_query(&env.engine, &env.store, model, pos_of(model, "string", 0, 1), &perr);
    CHECK(!prep, "primitive is not renamable");
    free(prep); free(perr); perr = NULL;
    prep = lsp_prepare_rename_query(&env.engine, &env.store, model, pos_of(model, "makeUser(", 0, 2), &perr);
    CHECK(prep && strstr(prep, "\"placeholder\":\"makeUser\""), "prepareRename placeholder");
    free(prep); free(perr);

    /* workspace symbols use the fuzzy matcher */
    char *ws = lsp_workspace_symbol_query(&env.engine, &env.store, "mkUser");
    root = parse(&pool, ws);
    CHECK(json_arr_len(root) >= 1 && strcmp(json_get_str(json_arr_at(root, 0), "name", ""), "makeUser") == 0,
          "workspace symbol fuzzy match");
    json_pool_destroy(pool);
    free(ws);
    ws = lsp_workspace_symbol_query(&env.engine, &env.store, "Dog.speak");
    CHECK(ws && strstr(ws, "\"Dog.speak\""), "members listed as Type.member");
    free(ws);

    free(model_text);
    free(page_text);
    env_free(&env);
    printf("✓ test_workspace_rename\n");
}


/* ── expected type & deep completion ───────────────────────────────────── */

static int rank_of(const JsonVal *items, const char *label) {
    for (size_t i = 0; i < json_arr_len(items); i++) {
        if (strcmp(json_get_str(json_arr_at(items, i), "label", ""), label) == 0) return (int)i;
    }
    return -1;
}

static JsonVal *complete_at(Env *env, LspDocument *doc, LspPosition pos, JsonPool **pool, char **raw) {
    *raw = lsp_completion_query(&env->engine, &env->store, doc, pos);
    assert(*raw);
    return (JsonVal *)json_get_arr(parse(pool, *raw), "items");
}

static void test_expected_type_completion(void) {
    Env env;
    env_init(&env, NULL);
    LspDocument *doc = open_doc(&env, "file:///expected.snl",
        "package main\n"
        "\n"
        "struct User {\n"
        "    public let name: string\n"
        "    public let age: int\n"
        "}\n"
        "\n"
        "func greet(who: string): string {\n"
        "    return who\n"
        "}\n"
        "\n"
        "func run(u: User, count: int, title: string): string {\n"
        "    let n: int = \n"
        "    greet(t)\n"
        "    return \n"
        "}\n");
    JsonPool *pool;
    char *raw;

    const JsonVal *items = complete_at(&env, doc, pos_of(doc, "let n: int = ", 0, 13), &pool, &raw);
    CHECK(rank_of(items, "count") >= 0 && rank_of(items, "count") < rank_of(items, "title"),
          "int wanted: `count` above `title`");
    CHECK(rank_of(items, "u.age") >= 0, "deep candidate u.age offered for int");
    CHECK(rank_of(items, "u.name") < 0, "u.name (string) not offered for int");
    json_pool_destroy(pool);
    free(raw);

    items = complete_at(&env, doc, pos_of(doc, "return \n", 0, 7), &pool, &raw);
    CHECK(rank_of(items, "title") >= 0 && rank_of(items, "title") < rank_of(items, "count"),
          "string return: `title` above `count`");
    CHECK(rank_of(items, "u.name") >= 0, "deep candidate u.name offered for string");
    json_pool_destroy(pool);
    free(raw);

    items = complete_at(&env, doc, pos_of(doc, "greet(t", 0, 7), &pool, &raw);
    CHECK(rank_of(items, "title") == 0, "argument of type string: `title` first");
    json_pool_destroy(pool);
    free(raw);

    env_free(&env);
    printf("✓ test_expected_type_completion\n");
}

/* ── code actions: extract / inline variable, organize imports ─────────── */

/* Applies the edits of the first action with `kind` (and title containing
 * `title_part` when given); NULL when no such action is offered. */
static char *apply_action(Env *env, LspDocument *doc, LspRange range, const char *kind, const char *title_part) {
    char *json = lsp_code_action_query(&env->engine, &env->store, doc, range, NULL);
    JsonPool *pool;
    JsonVal *root = parse(&pool, json);
    char *out = NULL;
    for (size_t i = 0; i < json_arr_len(root) && !out; i++) {
        const JsonVal *act = json_arr_at(root, i);
        if (strcmp(json_get_str(act, "kind", ""), kind) != 0) continue;
        if (title_part && !strstr(json_get_str(act, "title", ""), title_part)) continue;
        const JsonVal *changes = json_get_obj(json_get_obj(act, "edit"), "changes");
        out = apply_edits(doc->text, changes, doc->uri);
    }
    json_pool_destroy(pool);
    free(json);
    return out;
}

static LspRange range_of(const LspDocument *doc, const char *needle, int nth) {
    LspPosition s = pos_of(doc, needle, nth, 0);
    LspPosition e = pos_of(doc, needle, nth, (int)strlen(needle));
    return (LspRange){ s, e };
}

static void test_extract_inline_variable(void) {
    Env env;
    env_init(&env, NULL);
    LspDocument *doc = open_doc(&env, "file:///refactor.snl",
        "package main\n"
        "\n"
        "func area(w: int, h: int): int {\n"
        "    let border = 2\n"
        "    let total = (w + border) * (h + border)\n"
        "    while w > border * 3 {\n"
        "        return total\n"
        "    }\n"
        "    return total + border\n"
        "}\n");

    char *out = apply_action(&env, doc, range_of(doc, "w + border", 0), "refactor.extract.variable", NULL);
    CHECK(out && strstr(out, "    let value = w + border\n    let total = (value) * (h + border)"),
          "extract variable inserts a let before the statement");
    free(out);

    out = apply_action(&env, doc, range_of(doc, "border * 3", 0), "refactor.extract.variable", NULL);
    CHECK(out == NULL, "no extraction out of a loop condition");
    free(out);
    out = apply_action(&env, doc, range_of(doc, "+ border)", 0), "refactor.extract.variable", NULL);
    CHECK(out == NULL, "partial expression not extractable");
    free(out);

    /* inline on the declaration: every use, declaration removed, parenthesized */
    out = apply_action(&env, doc, range_of(doc, "total", 0), "refactor.inline.variable", NULL);
    CHECK(out && !strstr(out, "let total") &&
          strstr(out, "return ((w + border) * (h + border))\n") &&
          strstr(out, "return ((w + border) * (h + border)) + border"), "inline all uses");
    free(out);
    /* inline one use only */
    out = apply_action(&env, doc, range_of(doc, "border", 3), "refactor.inline.variable", "this use");
    CHECK(out && strstr(out, "let border = 2") && strstr(out, "while w > 2 * 3"), "inline a single use");
    free(out);

    LspDocument *mut = open_doc(&env, "file:///mut.snl",
        "package main\n"
        "\n"
        "func f(): int {\n"
        "    var n = 1\n"
        "    let twice = n * 2\n"
        "    n = 5\n"
        "    return twice\n"
        "}\n");
    out = apply_action(&env, mut, range_of(mut, "twice", 1), "refactor.inline.variable", NULL);
    CHECK(out == NULL, "no inlining of an initializer that reads a `var`");
    free(out);
    env_free(&env);
    printf("✓ test_extract_inline_variable\n");
}

static void test_organize_imports(void) {
    setup_workspace();
    write_file(WS_ROOT "/src/app/web/Util.snl",
        "package app.web.util\n"
        "\n"
        "public func slug(s: string): string {\n"
        "    return s\n"
        "}\n");
    Env env;
    env_init(&env, WS_ROOT);
    const char *text =
        "package app.web\n"
        "\n"
        "import app.model;\n"
        "import builtin.io.console.output;\n"
        "import app.model;\n"
        "import app.web.util;\n"
        "\n"
        "func page(): string {\n"
        "    Console.println(\"x\")\n"
        "    return slug(\"a\")\n"
        "}\n";
    LspDocument *doc = open_doc(&env, "file://" WS_ROOT "/src/app/web/Index.snl", text);
    LspRange r = { { 7, 0 }, { 7, 0 } };
    char *out = apply_action(&env, doc, r, "source.organizeImports", NULL);
    CHECK(out != NULL, "organize imports offered");
    if (out) {
        CHECK(!strstr(out, "import app.model"), "unused import removed");
        CHECK(strstr(out, "import builtin.io.console.output;\n\nimport app.web.util;\n"),
              "builtin group first, blank line, project group, semicolons kept");
    }
    free(out);

    /* already organized: no action (gopls offers it only when it changes something) */
    LspDocument *clean = open_doc(&env, "file://" WS_ROOT "/src/app/web/Clean.snl",
        "package app.web\n"
        "\n"
        "import app.web.util\n"
        "\n"
        "func f(): string {\n"
        "    return slug(\"a\")\n"
        "}\n");
    out = apply_action(&env, clean, r, "source.organizeImports", NULL);
    CHECK(out == NULL, "no-op organize imports not offered");
    free(out);

    /* missing import added */
    LspDocument *missing = open_doc(&env, "file://" WS_ROOT "/src/app/web/Missing.snl",
        "package app.web\n"
        "\n"
        "func f(): string {\n"
        "    return slug(\"a\")\n"
        "}\n");
    out = apply_action(&env, missing, r, "source.organizeImports", NULL);
    CHECK(out && strstr(out, "package app.web\n\nimport app.web.util\n"), "missing import added");
    free(out);
    env_free(&env);
    printf("✓ test_organize_imports\n");
}

static void test_import_insertion_groups(void) {
    LspDocStore store;
    lsp_docstore_init(&store);
    const char *text =
        "package app.web\n"
        "\n"
        "import builtin.io.console.output\n"
        "import builtin.text\n"
        "\n"
        "import app.model\n"
        "import zeta.lib\n"
        "\n"
        "func f(): unit {}\n";
    LspDocument *doc = lsp_docstore_open(&store, "file:///groups.snl", 1, text, strlen(text));
    LspImportEdit e;
    CHECK(lsp_import_edit(doc, "builtin.io.files", &e) && e.pos.line == 3, "joins the builtin group by shared prefix");
    CHECK(lsp_import_edit(doc, "app.jobs", &e) && e.pos.line == 5, "joins the project group, sorted");
    CHECK(lsp_import_edit(doc, "builtin.zzz", &e) && e.pos.line == 4, "builtin path without shared segment: builtin group end");
    CHECK(lsp_import_edit(doc, "core.x", &e) && e.pos.line == 6, "other path without shared segment: last group");
    lsp_docstore_destroy(&store);
    printf("✓ test_import_insertion_groups\n");
}

int main(void) {
    printf("Running LSP refactoring tests...\n");
    setenv("SNOVA_BUILTIN_DIR", SNOVA_TEST_BUILTIN_DIR, 1);
    test_fuzzy_matcher();
    test_local_rename_scopes();
    test_super_block_conflict();
    test_workspace_rename();
    test_expected_type_completion();
    test_extract_inline_variable();
    test_organize_imports();
    test_import_insertion_groups();
    if (failures) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    printf("All refactoring tests passed.\n");
    return 0;
}
