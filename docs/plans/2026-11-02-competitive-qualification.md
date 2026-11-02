# Competitive qualification target

**User requirement:** NEONFORGE should be superior to the six reference frameworks, rather than merely faster than its own earlier builds.

**Goal:** Complete the original platform's capability coverage and demonstrate advantages against pinned, compatible public framework releases with equivalent observable behavior.

**Architecture:** Retain the independent runtime and new API. Compare each service with the incumbent that provides that service, and compare complete representative mod workloads with the combined dependency stack. Do not compare a no-op standalone dispatcher with an in-game engine operation.

**Scope:** The six supplied frameworks and the existing 95-group capability roadmap. A broader claim about every existing or future framework is not established by these comparisons. The current user instruction is development outside the game; live qualification requires a later isolated test setup and does not authorize changing existing installations.

## Current gap assessment

Documentation retrieved on 2 November 2026. Some pages were served from a 1 November provider cache; wiki labels and repository default branches are not treated as latest supported release versions. This is a documented-interface baseline, not an exhaustive discovery of undocumented behavior or a benchmark result.

| Reference and primary source | Documented areas to cover | Current NEONFORGE position |
| --- | --- | --- |
| [RED4ext](https://docs.red4ext.com/mod-developers/creating-a-plugin) | Native plugin negotiation/loading, game-version support, engine-ready stages, native hooks and type registration | Standalone native host exists; game bootstrap, hooks, engine lifecycle and registration remain missing |
| [Cyber Engine Tweaks](https://wiki.redmodding.org/cyber-engine-tweaks) | Lua access to internal game functions, developer interface, console, bindings, settings and TweakDB tooling | Lua, engine reflection bridge, console/overlay and in-game bindings remain missing |
| [TweakXL](https://github.com/psiberx/cp2077-tweak-xl/wiki/YAML-Tweaks) | Typed database flats/records, creation/cloning, structures, arrays and reusable declarative definitions | No engine-backed TweakDB implementation; a JSON parser and package manifests do not establish database parity |
| [ArchiveXL](https://github.com/psiberx/cp2077-archive-xl/wiki) | Dynamic appearances, conditional/interpolated components, visual tags, resource extensions and localization | No engine resource/appearance extensions; file indexing alone is insufficient |
| [Codeware](https://github.com/psiberx/cp2077-codeware/wiki) | Lifecycle, world/entities/player, UI, resources, localization and reflection services | Generic owned callbacks/services exist; the game-facing service families remain missing |
| [redscript](https://github.com/jac3km4/redscript) | Typed language/compiler, game compilation integration, tooling including formatting/decompilation and editor support | No original game compiler, validated bytecode integration or editor language service yet |

The 0.1.1 microbenchmarks qualify an improvement over NEONFORGE 0.1.0 only. No reference framework has been beaten by a comparable test so far.

## Public release candidates observed

GitHub's latest-release endpoints reported these non-prerelease, non-draft releases on 2 November 2026. They are candidates for a later compatible baseline, not an assertion that this exact combination has been installed or tested. Download hashes, compatibility and release-note feature deltas must be checked before qualification. In particular, the redscript repository's default `1.0.x` development branch is not the stable release selected here.

| Primary release source | Tag | Published UTC |
| --- | --- | --- |
| [psiberx/cp2077-archive-xl](https://github.com/psiberx/cp2077-archive-xl/releases/tag/v1.28.6) | v1.28.6 | 2026-10-29 |
| [maximegmd/CyberEngineTweaks](https://github.com/maximegmd/CyberEngineTweaks/releases/tag/v1.38.1) | v1.38.1 | 2026-10-26 |
| [psiberx/cp2077-tweak-xl](https://github.com/psiberx/cp2077-tweak-xl/releases/tag/v1.12.0) | v1.12.0 | 2026-10-25 |
| [WopsS/RED4ext](https://github.com/wopss/RED4ext/releases/tag/v1.31.0) | v1.31.0 | 2026-10-24 |
| [psiberx/cp2077-codeware](https://github.com/psiberx/cp2077-codeware/releases/tag/v1.21.2) | v1.21.2 | 2026-10-28 |
| [jac3km4/redscript](https://github.com/jac3km4/redscript/releases/tag/v0.5.32) | v0.5.32 | 2026-10-22 |

## Completion gates

1. **Coverage:** Expand every applicable roadmap group into named, observable acceptance cases. Record implemented, standalone-tested and game-tested status separately. Required missing game functionality blocks a parity or overall superiority claim.
2. **Performance:** Use equivalent behavior, data and output checks. Development target: at least 20% lower median and p95 cost for the declared primary CPU-bound workloads against the best applicable incumbent, with the gain exceeding the measured noise band. A material regression in any required workload is a failed target, not something hidden by an average score.
3. **Memory and loading:** Report peak committed memory, allocation counts, installed footprint, cold/warm startup, compilation and loading separately. Compare unique installed dependency bytes without double counting shared frameworks. A memory or loading regression must be reported even when dispatch is faster.
4. **Reliability:** Demonstrate owner/lifetime correctness, meaningful malformed-input tests, failure recovery, repeated sessions and sustained representative gameplay. In-process native plugins remain trusted code. No claim of recovery from arbitrary access violations or an absolute crash-free guarantee.
5. **Author experience:** Complete the same original sample mods through the new API, measure setup steps/time and diagnostic usefulness, and verify generated projects and documentation. Include the cost of porting; unchanged compatibility was explicitly excluded from this project's design.
6. **Packaging:** One versioned distribution with dependency provenance, source/license availability, validation, conflict reports and tested recovery. Existing user settings and unrelated mods must survive the documented workflows.
7. **Added value:** Deliver and verify useful original capabilities beyond equivalent incumbent behavior. Proposed strengths include unified ownership diagnostics, transaction provenance/rollback, clear capability negotiation and reproducible support reports; a proposal does not count as a shipped advantage.

These are development targets, not present results. A weighted total cannot conceal an unsupported family, incorrect output, data loss or a materially slower required case.

## Comparison protocol

- Select maintained, stable public releases compatible with the exact game executable; record source URLs, release identifiers, package hashes and dependency versions. Test locally modified framework stacks separately, rather than treating it as the public incumbent baseline.
- Preserve existing installations and settings. Run future comparisons on an isolated setup with a disposable save and original benchmark/sample mods.
- Declare workloads, input sizes, expected outputs, warmup rules, cache states and meaningful-regression tolerances before collecting results. Cover tiny, typical, heavy and mutation/failure cases. Publish failed and slower cases alongside wins.
- Use the same machine, executable, graphics settings, save/scene and mod payload. Compare cold and warm behavior separately. Alternate candidate/baseline order and collect at least 30 measured samples per scenario; retain raw samples and uncertainty estimates. Record background load and CPU-clock limitations.
- Measure total work and tail latency, not only callback bodies. Count actual engine operations, allocations and retained resources. A skipped, rejected or unavailable operation is not a speed win.
- Publish a per-capability scorecard with pass/fail/not measured, exact evidence and scope. A not-applicable entry needs a justification declared with the workload. Unknown remains unknown.

## Offline implementation order

1. Original typed reflection/value contracts and marshaling fixtures, then Lua contexts and owned callbacks. Engine-dependent operations continue to return unavailable until their adapters are verified.
2. Typed declarative database model, dependency/conflict analysis, provenance and transactional failure tests. Treat this as offline semantics until actual TweakDB operations are proven.
3. Resource composition, validation and lifecycle fixtures using original data; separate this from real archive registration and resource readiness.
4. Compiler front-end/type system/diagnostics and source tooling, followed by validated format/engine integration. A parser or toy bytecode VM is not a completed redscript replacement.
5. Stronger package interruption recovery and diagnostic tooling; expand benchmarks to registration, mutation, services, settings, memory and end-to-end workloads.
6. Once the original bootstrap and adapters are ready, qualify live game behavior and then run the external comparison protocol.

Priorities follow the largest missing shared foundations, not the easiest benchmark to win. The full platform goal remains open until the evidence meets these gates.
