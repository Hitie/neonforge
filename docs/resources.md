# Original offline resource catalog

`runtime/nf_resources.exe` composes an original typed resource project into a deterministic catalog. It runs on Windows 10+ x64 and reuses the original data model and dependency resolver. The example contains symbolic records and original text, not game assets or engine resource encodings.

```powershell
.\runtime\nf_resources.exe validate .\examples\resources\original-packages.json
.\runtime\nf_resources.exe plan .\examples\resources\original-packages.json
```

Both commands accept `-` for bounded UTF-8 binary stdin. Input is at most 1 MiB. `plan` prints the complete catalog; `validate` prints a successful summary and its content key. Errors return nonzero with no apparent successful plan. Diagnostics are bounded UTF-8 and syntax errors include byte offsets. No source or game file is written.

## Project and ordering

The root has exactly `format: 1`, `types`, `packages`, `locale`, `fallback_locale` and optional `context`. Types use the [data model's schemas](data-model.md), including additive inheritance and exact tagged scalar/compound/array values. Context maps at most 64 identifier keys to bool/i64/u64/string/name/enum values. Conditions contain data, without executable expressions or implicit numeric conversion.

Each package has `id` and `version`, with optional `requires`, `before`, `after`, `resources`, `patches`, `localization` and `appearances`. Versions use three canonical nonnegative integer components. Dependencies specify `id`, `min` and optional `max_exclusive`. Missing/incompatible dependencies and cycles fail through the same original resolver as the host. Ready packages sort by ID, so package-array permutations do not change results. Ordering hints do not make a provider a declared dependency.

Resource keys are virtual paths of at most 240 bytes using lowercase ASCII letters/digits, `/`, `.`, `_` and `-`. Backslashes, uppercase aliases, traversal, reserved components and ambiguous filenames are rejected. These keys never cause disk access. A cross-package patch or authored resource/localization reference requires a declared direct/transitive dependency on its provider. All references, including later-overwritten fields and inactive variants, are checked against the final catalog. Reference cycles remain symbolic links here; no asynchronous loading or engine cycle handling is implied.

## Typed property composition

A package's `resources` maps each path to `{type, fields}`, with original tagged values. Complete duplicate definitions fail. Every definition must satisfy required fields when its package's definition batch commits.

Each `patches` entry names `resource`, a `fields` object and optional `expect` map from field names to current owner IDs. Changing another owner's existing value needs its exact expected owner. Stale or unused expectations fail, including expectations on absent fields. New optional fields need no prior owner. Equal values are no-ops and retain attribution. Patches cannot delete/reset a resource or erase fields in this format.

The result exposes definition ownership, per-field chronological provenance and define/patch layer history. The data engine copies edited records and shares immutable values. The complete candidate must validate before a resource session publishes it.

## Localization

`localization` maps canonical lowercase locale tags to key/entry objects. An entry has `text` and optional `expect_owner`. Text is UTF-8 without NUL, at most 16384 bytes. Different cross-owner overwrites need expectations and dependencies; identical text is a no-op. Every declared key needs an entry in the chosen fallback locale. Requested-language text wins when available, otherwise that fallback is selected explicitly.

Output retains every translation, current owner and history, plus selected text with its locale/fallback flag. References depend on the original fallback-key provider rather than forcing an upstream package to depend on a downstream translation override. Language extensions must depend on that key provider. Keys use case-sensitive symbolic identifiers; no game localization hashes are inferred. Locale syntax supports two/three-letter language components followed by lowercase 2..8-character letter/digit components, rather than complete BCP 47 syntax.

## Conditional appearances

`appearances` maps logical IDs to `{fallback, variants}`. Packages may add variants only while keeping the same fallback path. A variant has `resource`, typed `when` conditions and optional canonical signed-int64 `priority` string, default `"0"`. Missing or differently typed context values do not match. The highest matching priority wins; tied highest matches for different paths fail. Lower-priority ties do not affect a later winner. With no match, the fallback wins.

All paths must exist, including inactive variants, and their declared resource types must match the fallback. Rule authors must depend on external providers. Output includes normalized rules, chosen path, rule owners and fallback status. These are authoring decisions, without actual game appearance selection or body/mesh/material layout validation.

## Cache and reload

`cache_key` is SHA-256 of canonical complete output before adding the key. It covers normalized types/context, ordered package IDs/versions, resources/provenance/layers, translations and appearance rules/selections. Changed semantic output invalidates the key; equivalent normalized output retains it. Windows [CNG BCryptHash](https://learn.microsoft.com/en-us/windows/win32/api/bcrypt/nf-bcrypt-bcrypthash) supplies SHA-256, checked against known vectors and an independently hashed CLI artifact.

`ResourceSession::reload` stages the entire candidate, then publishes an immutable shared plan if its key changed. Syntax, dependency, schema, reference and late-selection errors retain the prior plan/generation. Old readers keep their snapshot. Generations are process-local uint64 counters; session methods require the creating thread, while owned immutable plans can be read independently. Error text is capped at 4096 UTF-8 bytes. This is a C++ source interface, without extending the primitive C ABI or Lua/game adapter.

Limits include 256 packages, 4096 resources/localization entries/total variants, 256 variants per appearance, 256 fields per resource, 64 edits per field/resource layer, 4 MiB retained translation text and 16 MiB/262144 nodes per output. Existing schema/value/transaction limits also apply. These are validation caps, not a process-wide memory guarantee.

No disk cache import, asset mounting/decoding, asynchronous engine loading, file-backed resource watcher or engine-safe hot reload is implemented here. The [durable data watcher](durable-data.md) handles data projects and does not interpret this resource format. Archive readers/writers, concrete type coverage and engine adapters remain later work. All engine-ready/game-tested flags stay false.
