# Offline completion and fresh-game baseline

The user requires all feasible work outside the game to be completed before live integration. On 3 November 2026 the user supplied a fresh Steam installation and authorized use of its files. Read-only inventory, identity checks and format research can proceed now; game modification and launch are not prerequisites for this phase. The user also explicitly requires GitHub updates.

## Fresh baseline results

The supplied installation reports product 2.5 and Steam build 26161058. Executable SHA-256 `d36e74c47592e9c656ae250078cb40b2be63c782af5bfbeff2280d43e653fab4` and address-metadata SHA-256 `eab304f38074145ab5ef53e2aa741576ba00674138761fafff23252005948fd4` match the existing identity adapter. The complete local scan hashed 182 regular files totalling 81,431,334,107 bytes, with no read/change errors or known mod markers. No game file was changed or executed by the audit. Per-file evidence remains local; no game data is published.

This is a fresh user-supplied comparison baseline, not independent Steam depot authentication or engine qualification. The metadata contains 1,118,540 address records with 6,735 colliding primary hashes; a hash lookup alone cannot establish a safe engine function signature. Internal calls remain disabled/unqualified. The next core implementation remains the value/database model.

## Completion order

1. Establish a repeatable full-file baseline: relative file metadata, SHA-256, observed format signatures, traversal/read errors and known loader markers. Keep per-file inventories and game material local. A marker scan is not proof of a pristine Steam installation; publisher/depot verification remains separate. Failed reads or files changing during hashing must prevent a complete result. Never follow linked/reparse paths or write a report inside the game directory.
2. Expand typed values and construct the original offline transactional database model: records, flats, compound values, arrays, cloning, validation, revisions, conflict provenance and rollback. Build a declarative patch planner and reload model on top. Do not advertise live TweakDB support.
3. Resource composition, dependency validation, localization, conditional appearance rules and cache/reload contracts using original fixtures.
4. Compiler front end, type system, IR, deterministic cache/source maps, format readers/writers and editor tools. Engine bytecode acceptance remains unqualified until integration.
5. Worker dispatch/lifetime contracts, standalone bootstrap/hook harnesses, developer console, settings/input schemas and UI previews.
6. Complete owned-file installation/update/uninstall, profiles and automatic interruption recovery with process-kill tests.
7. Complete original examples, generators, migration guides, support reports, licensing/provenance and release automation.
8. Qualify all offline paths with malformed-input/fuzz/fault tests, longevity, resource accounting, cold/warm startup and representative end-to-end benchmarks. Explain failures and regressions.

## Gate before live integration

Each of the 95 capability groups must identify its offline implementation, acceptance evidence and later engine-dependent checks. Every required offline acceptance case must pass; sample mods must work from extracted SDK packages; installation recovery must preserve unrelated files/settings; no unresolved release-blocking review/security finding may remain. An integration scenario and build identity must exist for every engine-dependent capability.

Completing this gate does not prove game compatibility, game FPS gains or superiority over other frameworks. Engine readiness, actual database/resource changes, rendering, saves, streaming and bytecode acceptance require later live qualification. Offline findings can inform adapters without modifying the supplied installation.

## Immediate implementation: repeatable game audit

Files: `tools/game_audit.py`, `tests/game_audit_test.py`, build/test integration and documentation.

- Test complete nested inventories and exact hashes, known-loader detection, linked-path refusal, missing/unreadable roots, in-tree output refusal and concurrent-change reporting before implementation.
- Implement `audit(root, progress=None)` and `write_report(root, output)` using only read-only source opens. Output metadata/hashes and bounded header signatures, never file contents or extracted game assets.
- Verify on synthetic fixtures, then inventory/hash every reachable regular file in the supplied install. Keep errors visible and distinguish complete hashing from pristine-install or runtime qualification.
- Review and publish source/tests/docs through protected GitHub checks. Publish only a build-level summary; keep the user's absolute path and per-file inventory out of repository exports.
