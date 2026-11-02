# Expanded values and transactional data model

**Goal:** Supply an original, usable offline C++ data engine and declarative CLI before game integration.

**Authorization and spec:** Implements step 2 of [offline readiness](2026-11-03-offline-readiness.md) under the approved independent-runtime/new-API design and continued execution instructions. No additional game access is required. Existing native/typed ABI 1 tables and headers stay unchanged. This phase does not claim engine TweakDB support or expanded-value transport through Lua/native ABI 1.

**Architecture:** Immutable owned values and snapshots; copy-on-write records inside a transaction; optimistic snapshot identity checks at commit. A bad operation poisons the transaction, preventing partial publication even when its error is caught. Original tagged JSON preserves 64-bit integers as decimal strings. A separate SDK CLI uses the same production model and original examples. No game formats or framework implementations are copied.

**Approaches:** Directly mutating an engine database is excluded by the offline gate. A Python-only model would duplicate a later native implementation. The selected C++ model makes validation and transactional semantics reusable by future adapters; the CLI is an immediately usable authoring boundary while an ABI extension remains future work.

## Contracts and implementation decisions

- Value kinds: bool, i64, u64, f64, string, name, id, resource, localization, enum domains, vector2/3/4, quaternion, color, homogeneous arrays including nested arrays. Identifiers remain original symbolic data, not verified game hashes or layouts. Colors are four integer channels 0..255; vectors/quaternions preserve finite authored float64 components without normalization.
- Strict tagged JSON rejects unknown fields, noncanonical integer strings, nonfinite floats, invalid UTF-8, malformed shapes and mixed array types. Every value is bounded to 1 MiB encoded, nesting to 16 and array length to 4096. Array element descriptors are required even for empty arrays. Enum domains are checked; engine enumeration membership is not inferred.
- Schemas declare named fields, exact types and optional required fields. All field values must match. Record/schema/field identifiers are bounded nonempty printable ASCII names using letters, digits, underscore, dot and hyphen, preserving case. Owner IDs follow core mod-ID rules. Source labels are bounded UTF-8 text without NUL.
- Transactions support create, clone, set, erase record/field, append/prepend, remove matching array values and unique insertion. Required-field and whole-state budgets are enforced before publication. Clone retains source value provenance and records the cloning owner. Live field history is capped at 64 edits; exceeding it fails rather than silently dropping attribution.
- Successful changed commits increment a uint64 revision; no-op commits do not. Concurrent/stale base snapshots conflict. Failure or abort leaves the published snapshot untouched. Existing readers retain the old immutable snapshot. This is atomic publication, not persistent crash recovery or automatic undo of committed changes.
- Default budgets: 4096 records, 256 fields per record, 16 MiB current-state logical values/identifiers/provenance and 4096 operations per transaction. Retained snapshots are caller-owned and are outside the current-state budget. Operations run on the database's creating thread. Transactions cannot commit after database destruction or reuse across databases.
- CLI input is at most 1 MiB and supports validate/plan/apply of original schemas and ordered transaction batches. Output is JSON on stdout; the CLI never writes to the source/game directory. Errors return nonzero without printing an apparent successful snapshot. Provenance identifies owner/source/operation and conflicts identify the affected record/field.

## Review focus

No integer passes through floating point. Nested/empty arrays retain exact descriptors. A caught operation failure cannot publish preceding edits. Shared value/record storage cannot mutate old snapshots after commit. Database/transaction lifetime and thread checks prevent dangling access. Failed required-field/budget/stale commits leave state and revision unchanged. Clone/delete and no-op array operations preserve the documented provenance semantics. CLI diagnostics and examples use the actual production engine.

## Task 1 — values and schemas

Files: `src/data_model.hpp`, `src/data_values.cpp`, `tests/data_tests.cpp`.

- [ ] Add behavior tests and observe the missing engine before implementation.
- [ ] Implement immutable type/value parsing, exact integer wire encoding, compound/array validation and deterministic serialization.
- [ ] Test full integer boundaries, malformed descriptors, type/domain mismatch, invalid UTF-8, nonfinite numbers, depth/size limits and copied ownership.

## Task 2 — atomic records and transactions

Files: `src/data_model.cpp`, data tests.

- [ ] Test unpublished edits, immutable snapshots, stale commits, failed/aborted batches, required fields, clone/delete, array operations, provenance and budgets.
- [ ] Implement copy-on-write staging and single publication after validation; expose revision/snapshot inspection.
- [ ] Verify lifetime/thread failures and repeated cleanup. Do not enable any game capability.

## Task 3 — declarative authoring and SDK

Files: `src/data_cli.cpp`, original `examples/data`, CLI/integration/distribution tests, CMake/direct builds and package/docs updates.

- [ ] Add CLI failing tests for actual patch outputs/errors and deterministic planning.
- [ ] Implement strict project/schema/operation validation through the model, original example and extracted-SDK checks.
- [ ] Run full CMake/direct/exported-source verification, obtain fresh whole-change review, fix substantive findings RED to GREEN and publish through protected GitHub checks.

Technical stack: original C++20/Windows x64 engine using existing original JSON parser and standard library. Existing dedicated workspace remains isolated from the game; GitHub publication uses a feature branch. No new third-party dependency or live game action.
