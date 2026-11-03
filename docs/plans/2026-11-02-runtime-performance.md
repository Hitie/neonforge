# Standalone runtime performance implementation plan

**Goal:** Measure and improve event dispatch and task cancellation at realistic high registration counts, while preserving ABI 1 and existing lifecycle behavior.

**Architecture:** Add a native benchmark that calls the actual runtime DLL through its C ABI. Compare an unchanged baseline DLL with the candidate using the same benchmark executable. Index events by name with immutable cached token snapshots; use links embedded in canonically owned task nodes for direct task removal without an extra queue allocation. These are implementation changes within the standalone core design.

**Constraints:** Windows x64/C++20; original implementation; no game or CPVC changes; no competitor superiority claims from these measurements; no timing thresholds in correctness tests; source and GPLv3 retained.

## Review focus

- Nested event registration/removal must preserve registration order and each emission's snapshot.
- Closing an owner during dispatch must skip its remaining callbacks without freeing an executing context.
- Cancelling or closing tasks already selected by a pump must not erase an invalid queue iterator.
- Failed registration/enqueue allocations must leave indexes, ownership tables and queues consistent.
- Benchmark counts, labels, DLL identities and timing boundaries must support the reported comparisons.

## Task 1: Reproducible baseline

- [x] Preserve baseline runtime DLL and original source for review.
- [x] Add a failing benchmark integration test requiring JSON results and verified event/task counts.
- [x] Implement `nf_benchmark.exe RUNTIME_DLL DATA_ROOT ITERATIONS SAMPLES`, dynamic ABI loading and per-scenario median/min/p95 samples.
- [x] Add `tools/benchmark.py` with artifact hashes, environment metadata and same-workload comparison validation.
- [x] Run a Release baseline against 0/1/64 matching listeners and 0/1024/8192 unrelated listeners, plus 4096-task cancellation/pumping.

## Task 2: Event routing

- [x] Add route behavior tests for stable snapshots, registration order, nested mutation, channel cleanup and empty topics; observe missing behavior.
- [x] Implement `EventRoutes::add/remove/snapshot` in `src/event_routes.hpp` using per-name ordered tokens and shared immutable cached snapshots.
- [x] Integrate subscription/unsubscription/owner-close and dispatch; retain canonical ownership lookups before each callback.
- [x] Add real-DLL lifecycle regressions; run the full suite and compare event timings with the preserved baseline.

## Task 3: Task cancellation and delivery

- [x] Add real-DLL regressions for cancellation of selected tasks, closing other owners, queue churn, FIFO and non-reentrant pumping.
- [x] Replace queue scans with stable task-node links. Mark tasks removed from the queue before callbacks; reuse bounded pump scratch storage.
- [x] Run all tests, then the same Release benchmark executable against baseline and candidate DLLs.
- [x] Version and document the development SDK, package original source/runtime, review the change and fix substantive findings with reproducing tests.
- [ ] Publish through a pull request and required GitHub checks. Record measured gains and tradeoffs; retain unfinished game status.

The original game/bootstrap and 95-capability roadmap remains open. This phase improves standalone infrastructure rather than qualifying game support.

## Verification record

CMake Release suite: 9/9 groups passed. Direct MSVC build and its complete checks passed. Review found no runtime blockers; a benchmark validation issue was reproduced with a defective DLL, fixed, and the full suite passed. No deferred findings. Reference measurements and artifact hashes are in performance.md and benchmarks/results/. Publication is tracked by the protected pull request rather than assumed from local tests.
