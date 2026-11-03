# Reusable data authoring and last-valid state implementation plan

Implementation uses regression tests first, followed by a review of the whole change.

**Goal:** Complete the next offline database milestone with schema inheritance, templates, validated checkpoints and last-valid reload.

**Spec:** Extends step 2 of [offline readiness](2026-11-03-offline-readiness.md) and the independent runtime/new API design. All representations remain original and offline. Public ABI 1 headers and GPLv3 stay unchanged. No game files are changed or launched.

**Architecture:** Native C++20 using the existing model and exact JSON parser. Resolve single-parent schema/template graphs deterministically with explicit cycle/depth/expansion limits. Stage complete projects/checkpoints in a fresh database before session publication. Immutable snapshots survive successful replacement. No new dependency.

**Approaches and decisions:** Reusing engine formats conflicts with the offline gate; a separate scripting implementation duplicates validation. Extend the original native model. Schema inheritance is additive: inherited field shadowing is rejected to preserve base contracts. Templates merge whole tagged values with explicit child/instance overrides; arrays are replaced, never implicitly concatenated. A template cannot change its inherited record type. Validate unused definitions too. Checkpoints preserve normalized schemas, exact values, revisions, commit metadata and field history; they are validated data, not authenticated provenance or game saves. Checkpoint export stays on stdout; durable file replacement, file watching and process-crash recovery are later work. Reload uses an independent generation counter; failed candidates retain the previous database, while equivalent candidates do not bump generation.

## Global constraints

- Existing format-1 projects without templates remain valid; default limits and primitive transport ABI remain unchanged.
- Inheritance depth at most 32; schemas/templates at most 4096 each, expanded fields at most 16384 per registry and encoded registry at most 4 MiB. Existing per-schema and value limits apply.
- Checkpoint JSON input/output at most 32 MiB and 262144 value/container nodes, enforced symmetrically; project CLI input remains 1 MiB. All integer counters are canonical decimal strings. Unknown fields and unsupported versions fail.
- Native session operations require the creating thread; invalid source text and wrong JSON kinds produce bounded useful errors. No apparent successful stdout on a failed CLI operation.

## Review focus

1. Shared ancestors and unused cycles cannot evade depth/expanded-size limits.
2. Inherited required/type contracts and template overrides use exactly the production value validator.
3. Malformed or forged checkpoint structure cannot publish partial state or reset a live database through an invalid restore.
4. Session failures and equivalent reloads preserve old snapshots and generation, including parse failures and schema-only changes.
5. Round trips preserve full-width counters and chronological attribution without claiming authenticity, engine readiness or crash recovery.

## Task 1: Schema and template composition

Files: `src/data_authoring.cpp`, `src/data_model.hpp`, `src/data_model.cpp`, `src/data_project.cpp`, `tests/data_tests.cpp`, both build paths.

Interfaces: `Json resolve_schemas(const Json&, Limits = {})`; `std::unique_ptr<Database> build_project(const Json&)`. Project `templates` maps names to `{extends?, type?, fields}`; `instantiate` operations name a template and optional field overrides. Resolved schemas feed both database validation and overwrite planning.

- [ ] Write real tests for inherited required fields, no shadowing, forward references, cycles/depth, template chaining/override/type checks and independent instances. Observe RED.
- [ ] Implement bounded composition and run the native/CLI data suite to GREEN.

## Task 2: Validated checkpoints and reload

Files: `src/data_model.cpp`, `src/data_session.cpp`, header and tests.

Interfaces: `Database::checkpoint()`, `Database::restore_checkpoint(const Json&)`; `ProjectSession::reload(std::string_view)`, `restore(std::string_view)`, `snapshot()`, `checkpoint()`, `generation()`, `error()`.

- [ ] Write tests for exact checkpoint round trips, subsequent edits/stale transactions, invalid imports, state budgets, lifetime/thread behavior, failed parse/type reloads, equivalent reloads and schema-only changes. Observe RED.
- [ ] Validate entire checkpoint before publishing; reuse state budget checks; preserve previous session on all candidate errors. Run focused suite to GREEN.

## Task 3: SDK authoring workflow and publication

Files: CLI, original example, CLI/distribution tests, package/version/docs and build integration.

- [ ] Write CLI tests for checkpoint/restore/reload and inherited template examples; confirm they fail first.
- [ ] Implement stdout-only commands and diagnostics, document use/limits and bump SDK development version.
- [ ] Run full CMake and direct MSVC suites, extracted SDK and independent source-export builds. Review the whole change; fix substantive findings with a failing test first, then rerun the full suite.
- [ ] Upload feature branch, verify protected GitHub checks and merge; verify main tree against the reviewed files. Deliver SDK and source.
