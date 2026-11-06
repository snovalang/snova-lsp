# snova-lsp — Snovalang Language Server Protocol

IDE-agnostic Language Server Protocol (LSP) server for Snovalang based on the `snovac` compiler pipeline and Microsoft LSP 3.17 specification.

## Features
- **Real-Time Diagnostics (`textDocument/publishDiagnostics`)**: Compile errors, syntax errors, and type warnings with SNOVA diagnostic codes.
- **Hover Documentation (`textDocument/hover`)**: Rich Markdown tooltips with syntax signatures and parsed `/* -- Doc:{Name} */` documentation.
- **Auto-Completion (`textDocument/completion`)**: Keywords, primitives, prelude types, local variables, and member access.
- **Go to Definition (`textDocument/definition`)**: Jump to declaration site across local scope, members, and project packages.
- **Document Symbols (`textDocument/documentSymbol`)**: Hierarchical outline view of classes, structs, enums, functions, and methods.
- **References and Implementations**: Navigate usages and method implementations across open workspace files.
- **Semantic Highlighting**: Token highlighting, including expressions embedded in interpolated strings.
- **Pulsar completions**: Templates for `pulsar func` and `pulsar method`, with async stream-aware signatures.
- **Package imports**: Resolves package and qualified-symbol imports for completion and navigation.

## Snovalang (`.snl`) migration

Pure LSP logic is moving from C into `src/*.snl` under package `Snova.Lsp` (protocol models, JSON writer, Content-Length framing, URI codec, line index, completion ranking, semantic delta encoding, RPC shaping). The production stdio server and snovac analysis bridge remain in C for now (`TODO(SL-2)` markers).

Builtin types: snl modules use the snovac release Builtin surface (`Array`, `Option`, `Result`, `Text`, primitive `toString` / `toLower` / string indexing) instead of parallel helper catalogs. Prefer `builtin.*` packages over bespoke utilities.

Versioning: bump the root `VERSION` file and `src/Version.snl` in the **same commit** as each migrated or new feature. The C binary embeds `VERSION` via `-DLSP_VERSION` for `serverInfo.version`.

Determinism: same inputs produce the same outputs — insertion-ordered JSON keys, insertion-sort ranking with zero-padded `sortText`, index-sorted semantic tokens, and no wall-clock or hash-map ordering.

## Install from a terminal

On macOS or Linux, install the latest server and matching compiler with:

```sh
curl -fsSL https://raw.githubusercontent.com/snovalang/snova-lsp/master/install.sh | sh
```

The installer downloads both source repositories, builds them, and places
`snova-lsp` in `~/.local/bin` and the matching compiler builtin sources in
`~/.snovalang/std/src`, the standard discovery path used by `snovac`. Add the
binary directory to `PATH` if needed. For a checkout, `make` downloads
`snovalang/snovac` automatically; `make install` installs the server and its
compiler builtins.

```sh
make test-snl
make test
make test-all
```
## Windows

The `install.sh` helper supports macOS and Linux. Windows users can clone
[`snovalang/snova-lsp`](https://github.com/snovalang/snova-lsp) and build with
the compiler source from [`snovalang/snovac`](https://github.com/snovalang/snovac).

Snovalang sources use the `.snl` extension. Snovalang scripts use `.sns`. The server indexes and analyzes only those two extensions. `.snova`, `.java`, and every other extension are ignored.

The project manifest is `mod.sns` (or `snova.sns`), not `mod.sno`. Document symbols keep each `selectionRange` inside its `range`, including fields and functions nested in a struct. Completion covers names in the open `.snl` or `.sns` file, members such as `Console.println`, and an autoimport edit for a symbol defined in another project file.

## Docstring Support
The LSP server automatically parses documentation formatted as:
```snl
/* -- Doc:{FuncName}
 *
 * -- Description: Documentation description text.
 *
 * -- Param{paramName}: Parameter description.
 * -- Returns: Return description.
 */
```

## License

This project is licensed under the [Apache License, Version 2.0](LICENSE).
Copyright 2026 Snovalang contributors. See [NOTICE](NOTICE).
