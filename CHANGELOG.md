# Changes

## 0.1.4 development SDK

- Read-only full-file SHA-256 audit with visible traversal/read/change errors and known loader markers.
- Outside-game atomic report output preserves game files even when an existing report path is hard-linked to them.
- Audit tool included in SDK exports, with original filesystem and extracted-package regression tests.
- Explicit offline-readiness gate before live integration; fresh-install identity rechecked against the existing 2.5 baseline.
- Native and typed ABI headers unchanged. Expanded database types and engine integration remain pending.

## 0.1.3 development SDK

- Embedded, pinned Lua 5.5.1 with per-mod states, VM/memory limits and local text modules.
- Original typed-call/export bindings, exact uint64 values, owned events/tasks, JSON settings and logs.
- Unified native/Lua manifest ordering and syntax/query preflight before startup; attributed failures and safe teardown.
- Stop-and-reload embedding API with 100-cycle cleanup coverage, original SDK Lua demos and workload benchmarks.
- Lua MIT notices and complete pinned source included. Existing native and typed ABIs are unchanged. Game integration remains unavailable.

## 0.1.2 development SDK

- Separately negotiated typed C API with exact 64-bit integers, finite floats, UTF-8 and byte spans.
- Lossless explicit numeric conversion, copied function signatures and checked native invocation.
- Caller-owned results, bounded payload/handle counts, callback lifetime protection and close cleanup.
- C example, SDK import library and extracted-package rebuild tests.
- Base ABI 1 is unchanged. Engine reflection and Lua remain unavailable.

## 0.1.1 development SDK

- Per-topic event routing with cached immutable snapshots; unrelated topics are not scanned on each emission.
- Direct task queue removal and reusable pump scratch storage, preserving FIFO, cancellation and owner cleanup.
- Real-DLL benchmarks with verified workload counts, raw samples, artifact identities and same-workload comparisons.
- Additional nested-dispatch, selected-task cancellation, owner cleanup, queue churn and extracted-benchmark coverage.
- ABI 1 is unchanged. Game integration remains unfinished.

## 0.1.0 development SDK

- Original native runtime, C ABI, plugin host and C/C++ examples.
- Deterministic package validation, dependency graph and capability preflight.
- Owned events/tasks/services, shutdown states and non-reusable runtime/context identities.
- Exact JSON settings persistence, bounded logs and per-mod metrics.
- Native project generator, package receipts, dry runs, modified-file preservation and exception rollback.
- Settings migration, PE/address identity doctor and Windows-library probe.
- Regression coverage for ABI validation, exact settings preservation, runtime identities, startup ordering and package schema checks.

Game bootstrap and engine services remain unavailable. This is a development foundation, not a six-framework replacement release.
