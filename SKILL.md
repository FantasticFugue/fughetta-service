---
name: fughetta
description: Navigate and edit a Fughetta workspace through its MCP tools. Use the language tool for anything above a function body (symbols, signatures, types, callers) and worldline apply for text inside bodies.
---

# Working in a Fughetta workspace

Fughetta splits code work at the function body. Above it (modules, types,
signatures, callers) the `language` tool answers in one call what would take
many searches and file reads. Below it (statements and expressions) you edit
text with `worldline(apply)`. Each language result names the `version` it
describes; pass that version on as `observed`.

## The three-step loop

1. **Navigate with `language`.**
   `inspect` with a symbol name or `symbol_id` returns its signature, the
   one-hop type surface of its parameters and result, the producers and
   consumers of those types, its definition coordinates, and the body text as
   `source`. Do not `rg` or read whole files to find a definition.
   `definitions` and `references` locate uses; `relations` follows `calls`,
   `binds` and `emits` across languages.
2. **Edit with `worldline(apply)`.**
   Send `observed` (the version you read), a fresh `operation_id`, and
   `changes`. Prefer `kind: "replace"` with `path`, `old_text` and
   `new_text`: copy `old_text` from the body `source` that `inspect`
   returned, and pass the symbol's scope `reference` from
   `language(scopes)` as `scope` when the same text appears elsewhere in the
   file. The match must be unique in that window, so no offsets are counted.
   `kind: "edit"` with `offset`, `count` and `text` (Unicode scalars in the
   observed version) remains for insertions at a known position. Pass no `leases`:
   the service locks the smallest function or type that encloses each edit
   for you, renews the lock on your next edit, and releases it when your task
   stops. If another agent holds that scope you get `scope_conflict` with its
   name, holder and expiry. Work elsewhere, or `fork` a worldline.
3. **Check by `reach`.**
   The apply reply reports `reach`:
   - `body_only`: every edit stayed inside a function body that `language`
     returned. Run `language(diagnostics)` on the new `version` for the edited
     files only.
   - `declaration_touched`: the batch changed a signature, a declaration or
     text outside any known body. Run
     `language(impact, changes: ["declaration"])` on the new version and fix
     the callers it lists before building.

Repeat an `operation_id` with identical input to recover the recorded result
of an interrupted write; use a new one for different input.

## Orchestrators only: explicit scopes

An orchestrator that hands disjoint regions to several agents before they
start can reserve them with `language(scopes)` references and
`scope(acquire)`, then give each agent its lease tokens to pass as
`worldline(apply, leases)`. A single agent never needs the `scope` tool.
