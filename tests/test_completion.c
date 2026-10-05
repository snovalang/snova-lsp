#include "lsp_completion.h"
#include "lsp_symbols.h"
#include "lsp_analysis.h"
#include "lsp_document.h"
#include "lsp_code_action.h"
#include "lsp_semantic.h"
#include "json.h"

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

static void test_general_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "func getHttpRequest(): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func process(requestId: int, requestPayload: string): unit {\n"
        "    let localCount = 10;\n"
        "    let req = 42;\n"
        "    req\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion at line 9 ("    req"), character 7 (after 'req')
    LspPosition pos = { .line = 9, .character = 7 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Verify first item is exact match "req" (local variable) with preselect true
    const JsonVal *first = json_arr_at(items, 0);
    assert(strcmp(json_get_str(first, "label", ""), "req") == 0);
    assert(json_get_bool(first, "preselect", false) == true);
    assert(strcmp(json_get_str(first, "sortText", ""), "00000") == 0);

    // Verify "requestId" and "requestPayload" (parameters) rank right after exact match
    bool found_param = false;
    for (size_t i = 1; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "requestId") == 0 || strcmp(label, "requestPayload") == 0) {
            found_param = true;
            break;
        }
    }
    assert(found_param);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_general_ranking passed\n");
}

static void test_acronym_matching(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "func getHttpRequest(): string {\n"
        "    return \"ok\";\n"
        "}\n"
        "\n"
        "func test(): unit {\n"
        "    ghr\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion after typing "ghr" at line 7, col 7
    LspPosition pos = { .line = 7, .character = 7 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    assert(json_arr_len(items) > 0);

    // Verify "getHttpRequest" matched via acronym "ghr"
    const JsonVal *first = json_arr_at(items, 0);
    assert(strstr(json_get_str(first, "label", ""), "getHttpRequest") != NULL);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_acronym_matching passed\n");
}

static void test_type_context_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "class UserAccount {\n"
        "    let id: int;\n"
        "}\n"
        "\n"
        "func test(u: ): unit {\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion in type position after `u: ` at line 6, col 14
    LspPosition pos = { .line = 6, .character = 14 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Verify types are offered (e.g. UserAccount, int, string, etc.)
    bool found_user_type = false;
    for (size_t i = 0; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        if (strcmp(json_get_str(item, "label", ""), "UserAccount") == 0) {
            found_user_type = true;
            break;
        }
    }
    assert(found_user_type);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_type_context_ranking passed\n");
}

static void test_decorator_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "@\n"
        "func handler(): unit {}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///main.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // Completion after '@' at line 2, col 1
    LspPosition pos = { .line = 2, .character = 1 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    // Top items should be decorators (native, route, get, post, test, etc.)
    const JsonVal *first = json_arr_at(items, 0);
    assert(strcmp(json_get_str(first, "detail", ""), "decorator") == 0);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_decorator_ranking passed\n");
}

static void test_deps_recommendation(void) {
    // 1. Setup temporary workspace with .snovalang/deps/**/*.snl and **/*.sns
#if defined(_WIN32)
    system("if exist C:\\tmp\\snova_lsp_deps_test rmdir /s /q C:\\tmp\\snova_lsp_deps_test");
    system("mkdir C:\\tmp\\snova_lsp_deps_test");
    system("mkdir C:\\tmp\\snova_lsp_deps_test\\.snovalang");
    system("mkdir C:\\tmp\\snova_lsp_deps_test\\.snovalang\\deps");
    system("mkdir C:\\tmp\\snova_lsp_deps_test\\.snovalang\\deps\\snova-remote");
    system("mkdir C:\\tmp\\snova_lsp_deps_test\\.snovalang\\deps\\snova-remote\\src");
    system("mkdir C:\\tmp\\snova_lsp_deps_test\\src");
    const char *tmp_root = "C:/tmp/snova_lsp_deps_test";
#else
    system("rm -rf /tmp/snova_lsp_deps_test");
    system("mkdir -p /tmp/snova_lsp_deps_test/.snovalang/deps/snova-remote/src");
    system("mkdir -p /tmp/snova_lsp_deps_test/src");
    const char *tmp_root = "/tmp/snova_lsp_deps_test";
#endif

    char manifest_path[512];
    snprintf(manifest_path, sizeof(manifest_path), "%s/mod.sns", tmp_root);
    FILE *f_manifest = fopen(manifest_path, "w");
    if (f_manifest) {
        fprintf(f_manifest, "module app\n\nsnova \"1.0.0\"\n");
        fclose(f_manifest);
    }

    char dependency_path[512];
    snprintf(dependency_path, sizeof(dependency_path),
             "%s/.snovalang/deps/snova-remote/src/Remote.snl", tmp_root);
    FILE *f_dep = fopen(dependency_path, "w");
    if (f_dep) {
        fprintf(f_dep,
            "package Snova.Remote\n\n"
            "public class RemoteClient {\n"
            "    public var endpoint: string\n"
            "}\n\n"
            "public func fetchRemoteData(): string {\n"
            "    return \"payload\"\n"
            "}\n");
        fclose(f_dep);
    }

    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, tmp_root);

    const char *code =
        "package main\n"
        "\n"
        "func app(): unit {\n"
        "    fetchRemote\n"
        "}\n";

    char app_uri[512];
    snprintf(app_uri, sizeof(app_uri), "file:///%s/src/App.snl", tmp_root);
    LspDocument *doc = lsp_docstore_open(&store, app_uri, 1, code, strlen(code));
    assert(doc != NULL);

    // Query completion after typing "fetchRemote"
    LspPosition pos = { .line = 3, .character = 15 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);

    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    size_t count = json_arr_len(items);
    assert(count > 0);

    bool found_remote_func = false;
    for (size_t i = 0; i < count; i++) {
        const JsonVal *item = json_arr_at(items, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "fetchRemoteData") == 0) {
            found_remote_func = true;
            const char *detail = json_get_str(item, "detail", "");
            assert(strstr(detail, "Snova.Remote") != NULL);
            const JsonVal *edits = json_get_arr(item, "additionalTextEdits");
            assert(edits != NULL);
            assert(json_arr_len(edits) == 1);
            const JsonVal *edit = json_arr_at(edits, 0);
            assert(strcmp(json_get_str(edit, "newText", ""), "\nimport Snova.Remote\n") == 0);
            break;
        }
    }
    assert(found_remote_func);

    json_pool_destroy(pool);
    free(json_res);

    // Also test type recommendation: "Remote" -> RemoteClient
    const char *code2 =
        "package main\n"
        "\n"
        "func app(): unit {\n"
        "    let c: Remote\n"
        "}\n";
    doc = lsp_docstore_open(&store, app_uri, 2, code2, strlen(code2));
    assert(doc != NULL);

    LspPosition pos2 = { .line = 3, .character = 17 };
    char *json_res2 = lsp_completion_query(&engine, &store, doc, pos2);
    assert(json_res2 != NULL);

    JsonPool *pool2 = json_pool_create(strlen(json_res2) + 1024);
    JsonVal *root2 = json_parse(pool2, json_res2, strlen(json_res2), &err);
    assert(root2 != NULL);

    const JsonVal *items2 = json_get_arr(root2, "items");
    assert(items2 != NULL);
    bool found_remote_type = false;
    for (size_t i = 0; i < json_arr_len(items2); i++) {
        const JsonVal *item = json_arr_at(items2, i);
        const char *label = json_get_str(item, "label", "");
        if (strcmp(label, "RemoteClient") == 0) {
            found_remote_type = true;
            break;
        }
    }
    assert(found_remote_type);

    json_pool_destroy(pool2);
    free(json_res2);

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);

#if defined(_WIN32)
    system("if exist C:\\tmp\\snova_lsp_deps_test rmdir /s /q C:\\tmp\\snova_lsp_deps_test");
#else
    system("rm -rf /tmp/snova_lsp_deps_test");
#endif
    printf("✓ test_deps_recommendation passed\n");
}

static void test_semantic_member_completion(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "struct User {\n"
        "    public let name: string\n"
        "    public let age: int\n"
        "}\n"
        "\n"
        "func process(): unit {\n"
        "    let u = User(\"Alice\", 30);\n"
        "    let list = [1, 2, 3];\n"
        "    let str = \"hello\";\n"
        "    u.\n"
        "    list.\n"
        "    str.\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///semantic.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // 1. Test member completion on struct instance: u. at line 11, character 6
    LspPosition pos1 = { .line = 11, .character = 6 };
    char *json1 = lsp_completion_query(&engine, &store, doc, pos1);
    assert(json1 != NULL);

    JsonPool *pool1 = json_pool_create(strlen(json1) + 1024);
    const char *err = NULL;
    JsonVal *root1 = json_parse(pool1, json1, strlen(json1), &err);
    assert(root1 != NULL);
    const JsonVal *items1 = json_get_arr(root1, "items");
    assert(items1 != NULL);

    bool has_name = false;
    bool has_age = false;
    for (size_t i = 0; i < json_arr_len(items1); i++) {
        const JsonVal *item = json_arr_at(items1, i);
        const char *lbl = json_get_str(item, "label", "");
        if (strcmp(lbl, "name") == 0) has_name = true;
        if (strcmp(lbl, "age") == 0) has_age = true;
    }
    assert(has_name && has_age);
    json_pool_destroy(pool1);
    free(json1);

    // 2. Test member completion on array: list. at line 12, character 9
    LspPosition pos2 = { .line = 12, .character = 9 };
    char *json2 = lsp_completion_query(&engine, &store, doc, pos2);
    assert(json2 != NULL);

    JsonPool *pool2 = json_pool_create(strlen(json2) + 1024);
    JsonVal *root2 = json_parse(pool2, json2, strlen(json2), &err);
    assert(root2 != NULL);
    const JsonVal *items2 = json_get_arr(root2, "items");
    assert(items2 != NULL);

    bool has_push = false;
    bool has_len = false;
    for (size_t i = 0; i < json_arr_len(items2); i++) {
        const JsonVal *item = json_arr_at(items2, i);
        const char *lbl = json_get_str(item, "label", "");
        if (strcmp(lbl, "push") == 0) has_push = true;
        if (strcmp(lbl, "len") == 0) has_len = true;
    }
    assert(has_push && has_len);
    json_pool_destroy(pool2);
    free(json2);

    // 3. Test member completion on string: str. at line 13, character 8
    LspPosition pos3 = { .line = 13, .character = 8 };
    char *json3 = lsp_completion_query(&engine, &store, doc, pos3);
    assert(json3 != NULL);

    JsonPool *pool3 = json_pool_create(strlen(json3) + 1024);
    JsonVal *root3 = json_parse(pool3, json3, strlen(json3), &err);
    assert(root3 != NULL);
    const JsonVal *items3 = json_get_arr(root3, "items");
    assert(items3 != NULL);

    bool has_str_len = false;
    for (size_t i = 0; i < json_arr_len(items3); i++) {
        const JsonVal *item = json_arr_at(items3, i);
        const char *lbl = json_get_str(item, "label", "");
        if (strcmp(lbl, "length") == 0 || strcmp(lbl, "len") == 0) has_str_len = true;
    }
    assert(has_str_len);
    json_pool_destroy(pool3);
    free(json3);

    // 4. Test Option & Result builtin member completion
    const char *opt_code =
        "package main\n"
        "\n"
        "struct Item { public let id: int }\n"
        "\n"
        "func test_opt(opt_user: Option<Item>, res_user: Result<Item, string>): unit {\n"
        "    opt_user.\n"
        "    opt_user?.\n"
        "    res_user.\n"
        "}\n";

    LspDocument *opt_doc = lsp_docstore_open(&store, "file:///opt_test.snl", 1, opt_code, strlen(opt_code));
    assert(opt_doc != NULL);

    // Test opt_user. (Option methods)
    LspPosition pos_opt = { .line = 5, .character = 13 };
    char *json_opt = lsp_completion_query(&engine, &store, opt_doc, pos_opt);
    assert(json_opt != NULL);
    JsonPool *pool_opt = json_pool_create(strlen(json_opt) + 1024);
    JsonVal *root_opt = json_parse(pool_opt, json_opt, strlen(json_opt), &err);
    assert(root_opt != NULL);
    const JsonVal *items_opt = json_get_arr(root_opt, "items");
    assert(items_opt != NULL);
    bool has_is_some = false;
    bool has_unwrap = false;
    for (size_t i = 0; i < json_arr_len(items_opt); i++) {
        const char *lbl = json_get_str(json_arr_at(items_opt, i), "label", "");
        if (strcmp(lbl, "isSome") == 0) has_is_some = true;
        if (strcmp(lbl, "unwrap") == 0) has_unwrap = true;
    }
    assert(has_is_some && has_unwrap);
    json_pool_destroy(pool_opt);
    free(json_opt);

    // Test opt_user?. (Unwrapped Item methods/fields: id)
    LspPosition pos_opt_q = { .line = 6, .character = 14 };
    char *json_opt_q = lsp_completion_query(&engine, &store, opt_doc, pos_opt_q);
    assert(json_opt_q != NULL);
    JsonPool *pool_opt_q = json_pool_create(strlen(json_opt_q) + 1024);
    JsonVal *root_opt_q = json_parse(pool_opt_q, json_opt_q, strlen(json_opt_q), &err);
    assert(root_opt_q != NULL);
    const JsonVal *items_opt_q = json_get_arr(root_opt_q, "items");
    assert(items_opt_q != NULL);
    bool has_id = false;
    for (size_t i = 0; i < json_arr_len(items_opt_q); i++) {
        const char *lbl = json_get_str(json_arr_at(items_opt_q, i), "label", "");
        if (strcmp(lbl, "id") == 0) has_id = true;
    }
    assert(has_id);
    json_pool_destroy(pool_opt_q);
    free(json_opt_q);

    // 5. Test chained member completion: acc.profile. and for-loop variable
    const char *chain_code =
        "package main\n"
        "\n"
        "struct Profile { public let bio: string }\n"
        "struct Account { public let profile: Profile }\n"
        "\n"
        "func test_chain(): unit {\n"
        "    let acc = Account(Profile(\"dev\"));\n"
        "    acc.profile.\n"
        "}\n"
        "\n"
        "func test_loop(): unit {\n"
        "    for (let item in [1, 2, 3]) {\n"
        "        it\n"
        "    }\n"
        "}\n";

    LspDocument *chain_doc = lsp_docstore_open(&store, "file:///chain_test.snl", 1, chain_code, strlen(chain_code));
    assert(chain_doc != NULL);

    // acc.profile. at line 7, char 16
    LspPosition pos_chain = { .line = 7, .character = 16 };
    char *json_chain = lsp_completion_query(&engine, &store, chain_doc, pos_chain);
    assert(json_chain != NULL);
    JsonPool *pool_chain = json_pool_create(strlen(json_chain) + 1024);
    JsonVal *root_chain = json_parse(pool_chain, json_chain, strlen(json_chain), &err);
    assert(root_chain != NULL);
    const JsonVal *items_chain = json_get_arr(root_chain, "items");
    assert(items_chain != NULL);
    bool has_bio = false;
    for (size_t i = 0; i < json_arr_len(items_chain); i++) {
        const char *lbl = json_get_str(json_arr_at(items_chain, i), "label", "");
        if (strcmp(lbl, "bio") == 0) has_bio = true;
    }
    assert(has_bio);
    json_pool_destroy(pool_chain);
    free(json_chain);

    // for-loop variable `item` at line 12, char 10 (prefix "it")
    LspPosition pos_for = { .line = 12, .character = 10 };
    char *json_for = lsp_completion_query(&engine, &store, chain_doc, pos_for);
    assert(json_for != NULL);
    JsonPool *pool_for = json_pool_create(strlen(json_for) + 1024);
    JsonVal *root_for = json_parse(pool_for, json_for, strlen(json_for), &err);
    assert(root_for != NULL);
    const JsonVal *items_for = json_get_arr(root_for, "items");
    assert(items_for != NULL);
    bool has_item = false;
    for (size_t i = 0; i < json_arr_len(items_for); i++) {
        const char *lbl = json_get_str(json_arr_at(items_for, i), "label", "");
        if (strcmp(lbl, "item") == 0) has_item = true;
    }
    assert(has_item);
    json_pool_destroy(pool_for);
    free(json_for);

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_semantic_member_completion passed\n");
}

static void test_code_actions(void) {
    LspDocStore store;
    lsp_docstore_init(&store);

    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    // Test code with legacy Int casing, missing mutability, and unorganized imports
    const char *code =
        "package main\n"
        "import std.math;\n"
        "import std.io;\n"
        "import std.math;\n"
        "\n"
        "class Account {\n"
        "    public let id: int\n"
        "    public var balance: double\n"
        "}\n"
        "\n"
        "func compute(val: Int): int {\n"
        "    let total = 0;\n"
        "    total = total + val;\n"
        "    return total;\n"
        "}\n";

    LspDocument *doc = lsp_docstore_open(&store, "file:///actions.snl", 1, code, strlen(code));
    assert(doc != NULL);

    // 1. Query code actions on line 10 ("func compute(val: Int): int {")
    LspRange range1 = {
        .start = { .line = 10, .character = 18 },
        .end = { .line = 10, .character = 21 }
    };
    char *act_json1 = lsp_code_action_query(&engine, &store, doc, range1, NULL);
    assert(act_json1 != NULL);

    JsonPool *pool1 = json_pool_create(strlen(act_json1) + 1024);
    const char *err = NULL;
    JsonVal *root1 = json_parse(pool1, act_json1, strlen(act_json1), &err);
    assert(root1 != NULL);
    assert(root1->kind == JSON_ARRAY);

    bool found_casing_fix = false;
    bool found_organize_imports = false;
    for (size_t i = 0; i < json_arr_len(root1); i++) {
        const JsonVal *action = json_arr_at(root1, i);
        const char *title = json_get_str(action, "title", "");
        const char *kind = json_get_str(action, "kind", "");
        if (strstr(title, "Use primitive 'int'")) {
            found_casing_fix = true;
        }
        if (strcmp(kind, "source.organizeImports") == 0) {
            found_organize_imports = true;
        }
    }
    assert(found_casing_fix);
    assert(found_organize_imports);
    json_pool_destroy(pool1);
    free(act_json1);

    // 2. Query code actions on line 12 ("    total = total + val;")
    LspRange range2 = {
        .start = { .line = 12, .character = 4 },
        .end = { .line = 12, .character = 9 }
    };
    char *act_json2 = lsp_code_action_query(&engine, &store, doc, range2, NULL);
    assert(act_json2 != NULL);

    JsonPool *pool2 = json_pool_create(strlen(act_json2) + 1024);
    JsonVal *root2 = json_parse(pool2, act_json2, strlen(act_json2), &err);
    assert(root2 != NULL);

    bool found_mut_fix = false;
    for (size_t i = 0; i < json_arr_len(root2); i++) {
        const JsonVal *action = json_arr_at(root2, i);
        const char *title = json_get_str(action, "title", "");
        if (strstr(title, "Change 'let total' to 'var total'")) {
            found_mut_fix = true;
        }
    }
    assert(found_mut_fix);
    json_pool_destroy(pool2);
    free(act_json2);

    // 3. Query constructor generation action on class Account (line 5)
    LspRange range3 = {
        .start = { .line = 5, .character = 6 },
        .end = { .line = 5, .character = 13 }
    };
    char *act_json3 = lsp_code_action_query(&engine, &store, doc, range3, NULL);
    assert(act_json3 != NULL);

    JsonPool *pool3 = json_pool_create(strlen(act_json3) + 1024);
    JsonVal *root3 = json_parse(pool3, act_json3, strlen(act_json3), &err);
    assert(root3 != NULL);

    bool found_ctor_action = false;
    for (size_t i = 0; i < json_arr_len(root3); i++) {
        const JsonVal *action = json_arr_at(root3, i);
        const char *kind = json_get_str(action, "kind", "");
        if (strcmp(kind, "refactor.generate.constructor") == 0) {
            found_ctor_action = true;
        }
    }
    assert(found_ctor_action);
    json_pool_destroy(pool3);
    free(act_json3);

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_code_actions passed\n");
}

static int pos_le(int64_t line_a, int64_t col_a, int64_t line_b, int64_t col_b) {
    return line_a < line_b || (line_a == line_b && col_a <= col_b);
}

static int range_contains(const JsonVal *outer, const JsonVal *inner) {
    const JsonVal *outer_start = json_get_obj(outer, "start");
    const JsonVal *outer_end = json_get_obj(outer, "end");
    const JsonVal *inner_start = json_get_obj(inner, "start");
    const JsonVal *inner_end = json_get_obj(inner, "end");
    if (!outer_start || !outer_end || !inner_start || !inner_end) return 0;
    return pos_le(json_get_int(outer_start, "line", -1), json_get_int(outer_start, "character", -1),
                  json_get_int(inner_start, "line", -1), json_get_int(inner_start, "character", -1)) &&
           pos_le(json_get_int(inner_end, "line", -1), json_get_int(inner_end, "character", -1),
                  json_get_int(outer_end, "line", -1), json_get_int(outer_end, "character", -1));
}

static const JsonVal *find_symbol(const JsonVal *arr, const char *name) {
    size_t i;
    if (!arr || !name) return NULL;
    for (i = 0; i < json_arr_len(arr); i++) {
        const JsonVal *item = json_arr_at(arr, i);
        const JsonVal *children;
        const JsonVal *found;
        if (!item) continue;
        if (strcmp(json_get_str(item, "name", ""), name) == 0) return item;
        children = json_get_arr(item, "children");
        found = find_symbol(children, name);
        if (found) return found;
    }
    return NULL;
}

static void assert_symbol_contained(const JsonVal *symbol) {
    const JsonVal *range;
    const JsonVal *selection;
    const JsonVal *children;
    size_t i;
    assert(symbol != NULL);
    range = json_get_obj(symbol, "range");
    selection = json_get_obj(symbol, "selectionRange");
    assert(range != NULL && selection != NULL);
    assert(range_contains(range, selection));
    children = json_get_arr(symbol, "children");
    for (i = 0; i < json_arr_len(children); i++) {
        const JsonVal *child = json_arr_at(children, i);
        const JsonVal *child_range = json_get_obj(child, "range");
        assert(child_range != NULL);
        assert(range_contains(range, child_range));
        assert_symbol_contained(child);
    }
}

static void test_document_symbol_ranges(void) {
    LspDocStore store;
    LspAnalysisEngine engine;
    const char *code =
        "struct Snovalang {\n"
        "    let version: string\n"
        "\n"
        "    func greet(): unit {\n"
        "    }\n"
        "}\n"
        "\n"
        "func main(): unit {\n"
        "}\n";
    LspDocument *doc;
    char *json_res;
    JsonPool *pool;
    const char *err = NULL;
    JsonVal *root;
    const JsonVal *type_sym;
    const JsonVal *field_sym;
    const JsonVal *method_sym;
    const JsonVal *func_sym;
    const JsonVal *selection;
    const JsonVal *selection_start;

    lsp_docstore_init(&store);
    lsp_engine_init(&engine, NULL);
    doc = lsp_docstore_open(&store, "file:///main.snl", 1, code, strlen(code));
    assert(doc != NULL);
    json_res = lsp_document_symbols_query(&engine, doc);
    assert(json_res != NULL);

    pool = json_pool_create(strlen(json_res) + 1024);
    root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);

    type_sym = find_symbol(root, "Snovalang");
    field_sym = find_symbol(root, "version");
    method_sym = find_symbol(root, "greet");
    func_sym = find_symbol(root, "main");
    assert(type_sym && field_sym && method_sym && func_sym);
    assert_symbol_contained(type_sym);
    assert_symbol_contained(func_sym);

    selection = json_get_obj(type_sym, "selectionRange");
    selection_start = json_get_obj(selection, "start");
    assert(json_get_int(selection_start, "line", -1) == 0);
    assert(json_get_int(selection_start, "character", -1) == 7);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_document_symbol_ranges passed\n");
}

static int completion_has_label(const JsonVal *items, const char *label, const JsonVal **out_item) {
    size_t i;
    if (out_item) *out_item = NULL;
    for (i = 0; i < json_arr_len(items); i++) {
        const JsonVal *item = json_arr_at(items, i);
        if (strcmp(json_get_str(item, "label", ""), label) == 0) {
            if (out_item) *out_item = item;
            return 1;
        }
    }
    return 0;
}

static void test_completion_on_source_and_script(void) {
    const char *uris[] = {"file:///main.snl", "file:///tool.sns"};
    const char *code =
        "struct Snovalang {\n"
        "    let name: string\n"
        "}\n"
        "\n"
        "func main(): unit {\n"
        "    Snov\n"
        "    Console.\n"
        "}\n";
    size_t u;

    for (u = 0; u < 2; u++) {
        LspDocStore store;
        LspAnalysisEngine engine;
        LspDocument *doc;
        LspPosition name_pos = { .line = 5, .character = 8 };
        LspPosition member_pos = { .line = 6, .character = 12 };
        char *json_name;
        char *json_member;
        JsonPool *pool;
        const char *err = NULL;
        JsonVal *root;
        const JsonVal *items;
        const JsonVal *println;

        lsp_docstore_init(&store);
        lsp_engine_init(&engine, NULL);
        doc = lsp_docstore_open(&store, uris[u], 1, code, strlen(code));
        assert(doc != NULL);

        json_name = lsp_completion_query(&engine, &store, doc, name_pos);
        assert(json_name != NULL);
        pool = json_pool_create(strlen(json_name) + 1024);
        root = json_parse(pool, json_name, strlen(json_name), &err);
        assert(root != NULL);
        items = json_get_arr(root, "items");
        assert(completion_has_label(items, "Snovalang", NULL));
        json_pool_destroy(pool);
        free(json_name);

        json_member = lsp_completion_query(&engine, &store, doc, member_pos);
        assert(json_member != NULL);
        pool = json_pool_create(strlen(json_member) + 1024);
        root = json_parse(pool, json_member, strlen(json_member), &err);
        assert(root != NULL);
        items = json_get_arr(root, "items");
        assert(completion_has_label(items, "println", &println));
        assert(println != NULL);
        assert(json_get_arr(println, "additionalTextEdits") != NULL);
        assert(strstr(json_get_str(json_arr_at(json_get_arr(println, "additionalTextEdits"), 0), "newText", ""),
                      "import builtin.console.Console") != NULL);
        json_pool_destroy(pool);
        free(json_member);
        lsp_engine_destroy(&engine);
        lsp_docstore_destroy(&store);
    }
    printf("✓ test_completion_on_source_and_script passed\n");
}

static void test_sns_manifest_autoimport(void) {
#if defined(_WIN32)
    system("if exist C:\\tmp\\snova_lsp_sns_test rmdir /s /q C:\\tmp\\snova_lsp_sns_test");
    system("mkdir C:\\tmp\\snova_lsp_sns_test");
    system("mkdir C:\\tmp\\snova_lsp_sns_test\\src");
    const char *tmp_root = "C:/tmp/snova_lsp_sns_test";
#else
    system("rm -rf /tmp/snova_lsp_sns_test");
    system("mkdir -p /tmp/snova_lsp_sns_test/src");
    const char *tmp_root = "/tmp/snova_lsp_sns_test";
#endif
    char path[512];
    FILE *file;
    LspDocStore store;
    LspAnalysisEngine engine;
    const char *code =
        "package main\n"
        "\n"
        "func app(): unit {\n"
        "    Wid\n"
        "}\n";
    char uri[512];
    LspDocument *doc;
    LspPosition pos = { .line = 3, .character = 7 };
    char *json_res;
    JsonPool *pool;
    const char *err = NULL;
    JsonVal *root;
    const JsonVal *item = NULL;

    snprintf(path, sizeof(path), "%s/mod.sns", tmp_root);
    file = fopen(path, "w");
    assert(file != NULL);
    fprintf(file, "module app\n\nsnova \"1.0.0\"\n");
    fclose(file);

    snprintf(path, sizeof(path), "%s/src/Widget.snl", tmp_root);
    file = fopen(path, "w");
    assert(file != NULL);
    fprintf(file, "package widgets\n\npublic struct Widget {\n    public let id: int\n}\n");
    fclose(file);

    lsp_docstore_init(&store);
    lsp_engine_init(&engine, tmp_root);
    snprintf(uri, sizeof(uri), "file:///%s/src/App.snl", tmp_root);
    doc = lsp_docstore_open(&store, uri, 1, code, strlen(code));
    assert(doc != NULL);
    json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);
    pool = json_pool_create(strlen(json_res) + 2048);
    root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);
    assert(completion_has_label(json_get_arr(root, "items"), "Widget", &item));
    assert(item != NULL);
    assert(strstr(json_get_str(json_arr_at(json_get_arr(item, "additionalTextEdits"), 0), "newText", ""),
                  "import widgets") != NULL);

    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
#if defined(_WIN32)
    system("if exist C:\\tmp\\snova_lsp_sns_test rmdir /s /q C:\\tmp\\snova_lsp_sns_test");
#else
    system("rm -rf /tmp/snova_lsp_sns_test");
#endif
    printf("✓ test_sns_manifest_autoimport passed\n");
}

static void test_builtin_type_ranking(void) {
    LspDocStore store;
    lsp_docstore_init(&store);
    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "func test(u: str): unit {\n"
        "}\n";
    LspDocument *doc = lsp_docstore_open(&store, "file:///builtin.snl", 1, code, strlen(code));
    assert(doc != NULL);

    LspPosition pos = { .line = 2, .character = 16 };
    char *json_res = lsp_completion_query(&engine, &store, doc, pos);
    assert(json_res != NULL);
    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);
    const JsonVal *items = json_get_arr(root, "items");
    assert(items != NULL);
    assert(json_arr_len(items) > 0);
    assert(strcmp(json_get_str(json_arr_at(items, 0), "label", ""), "string") == 0);
    json_pool_destroy(pool);
    free(json_res);

    const char *bare =
        "package main\n"
        "\n"
        "func test(u: ): unit {\n"
        "}\n";
    LspDocument *doc_bare = lsp_docstore_open(&store, "file:///builtin-bare.snl", 1, bare, strlen(bare));
    assert(doc_bare != NULL);
    LspPosition pos_bare = { .line = 2, .character = 13 };
    char *json_bare = lsp_completion_query(&engine, &store, doc_bare, pos_bare);
    assert(json_bare != NULL);
    JsonPool *pool_bare = json_pool_create(strlen(json_bare) + 1024);
    JsonVal *root_bare = json_parse(pool_bare, json_bare, strlen(json_bare), &err);
    assert(root_bare != NULL);
    const JsonVal *bare_items = json_get_arr(root_bare, "items");
    assert(bare_items != NULL);
    static const char *catalog[] = {
        "int", "long", "int8", "int16", "int32", "int64", "int128",
        "byte", "float", "double", "decimal", "bool", "char", "string", "unit", "any"
    };
    for (size_t i = 0; i < sizeof(catalog) / sizeof(catalog[0]); i++) {
        assert(completion_has_label(bare_items, catalog[i], NULL));
    }
    json_pool_destroy(pool_bare);
    free(json_bare);

    const char *wide =
        "package main\n"
        "\n"
        "func test(u: int3): unit {\n"
        "}\n";
    LspDocument *doc2 = lsp_docstore_open(&store, "file:///builtin-wide.snl", 1, wide, strlen(wide));
    assert(doc2 != NULL);
    LspPosition pos2 = { .line = 2, .character = 17 };
    char *json2 = lsp_completion_query(&engine, &store, doc2, pos2);
    assert(json2 != NULL);
    JsonPool *pool2 = json_pool_create(strlen(json2) + 1024);
    JsonVal *root2 = json_parse(pool2, json2, strlen(json2), &err);
    assert(root2 != NULL);
    const JsonVal *items2 = json_get_arr(root2, "items");
    assert(items2 != NULL && json_arr_len(items2) > 0);
    assert(strcmp(json_get_str(json_arr_at(items2, 0), "label", ""), "int32") == 0);
    json_pool_destroy(pool2);
    free(json2);

    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_builtin_type_ranking passed\n");
}

static int semantic_type_at(const JsonVal *data, uint32_t line, uint32_t col) {
    uint32_t cur_line = 0;
    uint32_t cur_col = 0;
    size_t n = json_arr_len(data);
    for (size_t i = 0; i + 4 < n; i += 5) {
        uint32_t dl = (uint32_t)json_arr_at(data, i)->num_val;
        uint32_t ds = (uint32_t)json_arr_at(data, i + 1)->num_val;
        uint32_t len = (uint32_t)json_arr_at(data, i + 2)->num_val;
        uint32_t ty = (uint32_t)json_arr_at(data, i + 3)->num_val;
        cur_line += dl;
        cur_col = dl ? ds : cur_col + ds;
        if (cur_line == line && col >= cur_col && col < cur_col + len) {
            return (int)ty;
        }
    }
    return -1;
}

static void test_builtin_type_highlight(void) {
    LspDocStore store;
    lsp_docstore_init(&store);
    LspAnalysisEngine engine;
    lsp_engine_init(&engine, NULL);

    const char *code =
        "package main\n"
        "\n"
        "struct Message {\n"
        "    message: string\n"
        "}\n"
        "\n"
        "public func main(): int {\n"
        "    return 1;\n"
        "}\n";
    LspDocument *doc = lsp_docstore_open(&store, "file:///highlight.snl", 1, code, strlen(code));
    assert(doc != NULL);
    char *json_res = lsp_semantic_tokens_query(&engine, doc);
    assert(json_res != NULL);
    JsonPool *pool = json_pool_create(strlen(json_res) + 1024);
    const char *err = NULL;
    JsonVal *root = json_parse(pool, json_res, strlen(json_res), &err);
    assert(root != NULL);
    const JsonVal *data = json_get_arr(root, "data");
    assert(data != NULL);
    assert(semantic_type_at(data, 3, 13) == (int)LSP_SEMANTIC_TYPE_TYPE);
    assert(semantic_type_at(data, 6, 20) == (int)LSP_SEMANTIC_TYPE_TYPE);
    json_pool_destroy(pool);
    free(json_res);
    lsp_engine_destroy(&engine);
    lsp_docstore_destroy(&store);
    printf("✓ test_builtin_type_highlight passed\n");
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    printf("Running LSP completion & code action unit tests...\n");
    test_general_ranking();
    test_acronym_matching();
    test_type_context_ranking();
    test_decorator_ranking();
    test_deps_recommendation();
    test_semantic_member_completion();
    test_code_actions();
    test_document_symbol_ranges();
    test_completion_on_source_and_script();
    test_sns_manifest_autoimport();
    test_builtin_type_ranking();
    test_builtin_type_highlight();
    printf("All completion and code action tests passed successfully! (12/12)\n");
    return 0;
}
