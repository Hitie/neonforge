# Original offline resource composition

**Goal:** Supply a native resource-authoring catalog with deterministic dependencies, typed property patches, localization fallback, conditional appearances and last-valid cache/reload semantics.

**Spec:** Implements the next original offline resource foundation in readiness step 3, following the explicit request to continue after durable data/watch. No engine formats, asset mounting, rendering or game modification. Public primitive ABI headers and GPLv3 remain unchanged. Native C++20 reuses the original manifest resolver and data model; no upstream framework code or headers.

**Architecture:** A format-1 JSON project declares original data `types`, packages, target/fallback locale and typed selection context. Topological package order is stable by ID, with existing semantic-version/dependency rules. Each package publishes data-model records as canonical virtual resource paths and can patch typed fields through explicit prior-owner expectations. Composition is a fresh candidate; immutable resource sessions publish only after complete validation. SHA-256 over canonical result content supplies a deterministic offline cache key through Windows CNG.

## Contracts and decisions

- Project input at most 1 MiB; at most 256 packages, 4096 resources, 4096 localization entries/appearance variants, 256 fields/resource and 64 edits/field or resource layer. Output at most 16 MiB and 262144 JSON nodes. Existing typed value/schema limits apply. Identifiers and unknown fields are strict.
- Virtual paths use safe lowercase ASCII letters/digits, slash, dot, underscore and hyphen, at most 240 bytes. Backslashes, uppercase aliases, traversal and filesystem-reserved components are rejected. Paths are symbolic catalog keys, never disk access.
- Package fields: id/version, optional requires/before/after, resources, patches, localization, appearances. Missing/incompatible dependencies and cycles fail. A patch or cross-package resource/localization reference requires a declared direct/transitive dependency on its provider; ordering hints alone are not dependencies.
- A resource defines a type and tagged fields. Duplicate complete definitions fail explicitly; this milestone composes properties rather than guessing asset replacement semantics. A patch names a resource, tagged fields and optional per-field expected prior owners. Changing another owner's existing value requires an exact expected owner; stale/unused expectations fail. Equal values are no-ops. Required fields are checked on each package commit; no deletion/reset operations in this milestone.
- Localization entries contain text plus optional expected prior owner. Different cross-owner overwrites require expectations/dependencies. All keys must exist in the selected fallback locale. Selection uses the requested language then that fallback, retaining locale/owner/history. Text is original UTF-8, at most 16384 bytes per entry, with global text budget 4 MiB.
- Appearance groups declare one consistent fallback virtual path plus variants containing typed `when` conditions, resource and canonical signed-int64 priority string (default zero). All paths, including inactive variants, must exist and match fallback type. Context is at most 64 scalar bool/i64/u64/string/name/enum values. Highest matching priority wins; tied top matches for different resources fail rather than depend on input order. Missing context takes the fallback. Variant providers must declare dependencies.
- Validate nested typed resource/localization references against the final catalog and their field author's dependencies. Forward references are allowed when package dependencies are declared.
- Output includes normalized types/context, resolved package ID/version order, resources and field provenance/layers, all translations plus selected localized texts, appearance rules/selections and false engine/game flags. Cache key covers canonical output excluding the key itself; semantically equivalent package-array permutations yield identical output. No disk cache import or engine-safe reload claim.
- `ResourceSession::reload(source)` stages a complete candidate, preserves the last valid immutable plan on failure, ignores equivalent content keys and uses a process-local uint64 generation. Creating-thread ownership and bounded UTF-8 diagnostics follow the data-session contracts.

## Review focus

1. Dependency/order permutations and undeclared transitive providers cannot silently alter winners or allow stale owner assumptions.
2. Schema inheritance, required fields and nested resource/localization values use the same exact production validator, including unused definitions/rules.
3. Localization fallback, equal values and cross-owner patches retain correct attribution without silently dropping a mod's changes.
4. Conditional priority ties, missing context/paths and inactive variants fail or fall back deterministically; no expression executes code.
5. Expanded reports, provenance and caches remain bounded; malformed/late failures cannot replace a live resource snapshot or enable game capabilities.

## Task 1 — Native composition model

Files: `src/resource_model.hpp`, `src/resource_model.cpp`, native resource tests and both build paths. Interface: `Json compose_resources(const Json&)`. Add dependency, field composition/type/expectation, canonical path/reference, localization and appearance tests; observe missing API RED. Implement using original Manifest/Database/Value validators and explicit budgets; run focused suite.

## Task 2 — Content identity and immutable reload

Files: `src/resource_hash.cpp`, native tests. Interfaces: `std::string resource_digest(std::string_view)`, `ResourceSession::reload`, snapshot/generation/error. Add deterministic hash, semantic-equivalence, retained old reader, bad candidate, changed-context/schema and thread tests before implementation. Use Windows CNG for SHA-256 with known-vector verification; stage the complete artifact before publication.

## Task 3 — Authoring CLI and SDK

Files: `src/resource_cli.cpp`, original `examples/resources`, CLI/distribution tests, package/version/docs. `nf_resources plan|validate project.json` supports bounded binary stdin; stdout only. Add failing real-executable/extracted-SDK tests and original multilingual/appearance sample. Run full CMake/direct/exported-source checks, a review of the whole change, test-first fixes for substantive findings and protected GitHub publication. Keep game flags false and describe remaining engine work.
