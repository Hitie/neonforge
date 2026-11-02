# Standalone Lua runtime implementation plan

**Goal:** Make original Lua mods usable alongside native plugins through the existing standalone host, with typed calls, events, tasks, settings and bounded reloads.

**Authorization:** Implements the approved independent-runtime/new-API design and the user's instruction to continue the scripting-layer proposal. Routine implementation choices below are engineering decisions, not additional user preferences. No game installation changes are included.

**Architecture:** Embed pinned Lua 5.5.1 as a private static C++ library, retaining unmodified upstream source and MIT notice. A ScriptHost owns one Lua state and core context per script mod. Existing native ABI 1 and the typed API remain unchanged. Both entry types share manifests, preflight/dependency ordering and shutdown. Lua availability is a host capability, not a false core-DLL capability.

**Approaches:** A separate script runner would duplicate packaging and prevent ordinary mixed dependency chains. A homegrown interpreter would delay useful authoring and language correctness. The chosen embedded Lua adapter uses the already authorized infrastructure-library allowance; all bindings and lifecycle policy are original NEONFORGE code.

## Contracts

- Entry is an owned relative `.lua` or `.dll` path. Script source/modules are text-only, at most 1 MiB per file, and reject path escape/reparse points. `require` is mod-local, cached and limited to dotted module names. No binary modules, arbitrary file/OS/debug/package access, dynamic load/dofile/loadfile, user coroutines or bytecode loading.
- Safe base/table/string/math/utf8 libraries are exposed. `pcall`/`xpcall` remain available. Each entry/callback runs on a host-owned coroutine; exhausting its VM instruction allowance yields directly to the host, including through protected calls. No Lua coroutine API is exposed. Memory is bounded by a per-state allocator. These controls are not a hostile-native-code sandbox or a hard wall-clock timeout for C library/native calls.
- Defaults: 8 MiB Lua allocation, 1,000,000 VM instructions per entry/callback, 512 live owned callbacks/functions, at most 16 nested script invocations. Constructor limits are validated; memory/instruction limits are configurable by embedding tests/host code. Native/core payload limits continue to apply.
- `nf.call` and `nf.signature` use exact registered signatures. Native signed integers use Lua int64; uint64 uses an explicit decimal `nf.u64` userdata so UINT64_MAX survives unchanged. Float conversion must be exact for integer inputs. UTF-8 and bytes are Lua strings; signatures determine their meaning. Void returns nil. Native results are released on all paths.
- `nf.export(name, resultKind, argumentKinds, callback)` publishes an owned typed function. `nf.on/off`, `nf.emit`, `nf.defer/cancel`, `nf.settings_read/write` and `nf.log` use existing core ownership/status contracts. Event/task/function bindings are bounded and reclaimed on removal. Handles are opaque userdata, not floating-point numbers.
- An unhandled entry/callback error faults the mod and closes its core context. Other mods remain independently usable. Error attribution names the mod and chunk. Closing occurs immediately; freeing Lua states waits until no invocation is using them. No callback can retain a freed ScriptHost object.
- Reload is an explicit stop-and-reload operation, preserving settings. The old context/callbacks are removed before replacement; failure leaves the mod stopped. Reentrant reload/unload is rejected. This does not roll back external effects of script initialization.
- All host operations run on the creating thread. ScriptHost must stop before its runtime is destroyed. Game reflection, game lifecycle, input, overlay and other game services remain unavailable.

## Review focus

OOM during library/binding/coroutine setup must be protected and must not abort the process. Yield-based quota termination must survive a script catching ordinary errors. Native result release must survive Lua allocation failure. Self-removal and nested callbacks must not free live bindings/state. One hundred reloads must keep registrations, tasks, results and memory bounded. Module paths and cached errors must remain confined to the owning mod.

## Task 1: Script execution and isolation

Files: `src/script_host.hpp`, `src/script_host.cpp`, `tests/script_tests.cpp`, vendored `third_party/lua`, `CMakeLists.txt`, dependency notices.

- [ ] Add real-host integration test with a `.lua` entry and observe rejection before implementation.
- [ ] Add ScriptHost tests for isolated globals, syntax/runtime errors, memory failure, protected-call infinite loops, forbidden libraries and local module loading.
- [ ] Implement `ScriptHost(api, typed, runtime, ScriptLimits)`, `load(id, root, entry)`, `execute(id, source)`, `reload(id)`, `unload(id)`, `context(id)`, `stats(id)` and attributed diagnostics.
- [ ] Verify allocator cleanup and coroutine quotas through the real Lua VM.

## Task 2: Owned script bindings

Files: script host/value helpers and tests, original Lua examples.

- [ ] Write failing tests for exact typed signed/unsigned values, malformed values, native failures, result release, callback self-removal, nested dispatch, cancellation and settings persistence.
- [ ] Implement the documented `nf` functions against the existing core and typed tables; add typed Lua exports.
- [ ] Verify 100 reload cycles, fault cleanup and independent mod survival.

## Task 3: Unified host and distribution

Files: `src/host.cpp`, `src/manifest.cpp`, `tools/forge.py`, manifest schema, build scripts, SDK packager, integration/package tests, documentation.

- [ ] Extend entry validation and shared preflight; validate native query metadata and Lua syntax before any startup effects.
- [ ] Load native and Lua entries in dependency order and stop them safely in reverse order.
- [ ] Ship the Lua demo and upstream notices; verify extracted SDK and exported-source builds, then full CMake/direct-MSVC tests.
- [ ] Add script workload benchmarks with real effects/counter validation; report scope without external-framework superiority claims.
- [ ] Obtain fresh whole-change review, address substantive defects with failing regression tests, publish through protected GitHub checks.

Tech: Windows x64, C++20, Lua 5.5.1 (official source archive SHA-256 `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`), existing C ABI 1 and typed ABI 1. Source: https://www.lua.org/download.html and https://www.lua.org/manual/5.5/manual.html .
