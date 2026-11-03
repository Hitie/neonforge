# Capability and evidence status

Development SDK 0.1.7. The full project remains unfinished.

| Area | Current state |
| --- | --- |
| Native standalone host and C/C++ SDK | Implemented and tested outside the game |
| Manifests, dependency graph, capability preflight | Implemented and tested outside the game |
| Context/event/task/service ownership | Implemented and tested outside the game |
| Typed values, function signatures and native invocation | Implemented and tested outside the game; see [typed-calls.md](typed-calls.md) |
| Settings, structured logs and mod metrics | Implemented and tested outside the game |
| Indexed events, direct task cancellation and real-DLL benchmarks | Implemented and tested outside the game; see performance.md |
| Package receipts, preservation and exception rollback | Developer preview; real-filesystem tests, no hard process-kill recovery |
| Settings migration and project generation | Implemented developer tools, tested outside the game |
| Exact-build/PE/address diagnostics | Read-only identity/metadata tooling; no validated engine call map |
| Windows WINMM export probe | System-library experiment; no game bootstrap qualification |
| Original game-loadable bootstrap | Not implemented/qualified |
| Engine lifecycle, native registration and reflection | Not implemented |
| Standalone Lua values/calls/events/tasks/settings/local modules/reload | Implemented and tested outside the game; see [scripting](scripting.md) |
| Lua game observers/lifecycle/input/overlay | Not implemented |
| Original offline typed data/transaction engine and authoring CLI | Implemented and tested outside the game; see [data model](data-model.md) |
| Actual TweakDB edits | Not implemented |
| Archive/resource/appearance/world/quest extensions | Not implemented |
| Original compiler and game-script bytecode integration | Not implemented |
| Game UI, input and player/world services | Not implemented |

The approved design tracks 95 broad capability groups. These groups include extensive game behavior; the working standalone services are not counted as completed game systems. Additional developer tools implement parts of that scope and improve diagnostics, but do not establish feature parity with all six reference frameworks.

Every future capability must identify its implementation, negative/lifecycle tests, exact executable identity and observed in-game acceptance scenario. A successful build, old log, archive index or matched address is insufficient evidence of runtime behavior.

## Next integration gate

Complete the [offline readiness plan](plans/2026-11-03-offline-readiness.md) before live integration. Read-only investigation of the user's fresh installation is authorized for identity, format and type research. Offline values/transactions, templates/inheritance, checkpoint import/export and caller-driven last-valid reload are implemented. The developer store now supplies polling file watching and tested process-interruption recovery. Hardware power-loss qualification, expanded native/Lua transport and engine adapters remain pending.

After that offline gate, qualify a complete original bootstrap, demonstrate safe initialization timing and verify the first engine lifecycle/native call on an isolated candidate profile. Only then enable the corresponding adapter capability. The SDK candidate remains intended for standalone developer evaluation.

## Benchmarking

No game FPS, frame-time, memory or compatibility superiority claim is supported yet. The native counters provide a basis for later measurements. Comparisons must use the same game build, scene, settings, mod workload and repeated runs, with overhead and failure cases reported.
