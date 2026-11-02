# Standalone runtime performance

NEONFORGE 0.1.1 indexes subscriptions by event name and caches an immutable ordered token snapshot per topic. Repeated emission avoids scanning unrelated subscriptions or rebuilding the snapshot. A changed topic rebuilds its snapshot on its next emission. Every token is checked against the canonical ownership table before calling it, preserving cancellation and close behavior.

Task nodes contain their queue links. Cancellation performs handle lookup and direct unlinking, without a full queue scan or a second queue-node allocation. Pumping detaches its selected batch before invoking callbacks and reuses bounded token scratch storage. Owner shutdown removes both queued and selected pending work through canonical handles.

## Run the benchmark

After a CMake Release build:

```powershell
python tools/benchmark.py --executable build/Release/nf_benchmark.exe --runtime build/Release/NeonForge.Core.dll --output benchmark.json --iterations 5000 --samples 7
```

The binary SDK includes the same tools:

```powershell
python tools/benchmark.py --executable tools/nf_benchmark.exe --runtime runtime/NeonForge.Core.dll --output benchmark.json --iterations 5000 --samples 7
```

To compare two runtime builds, preserve the baseline DLL, run it first, then use the **same benchmark executable** for the candidate with `--baseline baseline.json --output candidate.json`. The runner rejects differing executable hashes, environments, configurations and workload parameters. Runtime DLL hashes are recorded separately. Reports contain OS/architecture/CPU metadata, not local file paths or game data.

Nine scenarios cover idle emission, one listener with 0/1024/8192 unrelated subscriptions, 64 matching listeners with 0/8192 unrelated subscriptions, a missing topic with 8192 unrelated subscriptions, cancellation of 4096 queued tasks, and pumping 4096 tasks in batches of 64. Unrelated subscriptions use distinct names. Each sample verifies exact callback counts and complete cleanup. Setup/teardown are outside the measured interval; event routing is primed before steady-state emission. One complete warmup sample is discarded. Reports retain each measured sample plus minimum, median and nearest-rank p95 nanoseconds per operation.

An event operation is one emission. A cancellation operation is one cancelled task. A pump operation is one executed task, including its no-op callback and callback metrics; its timing excludes task enqueueing. These units must not be compared as interchangeable work.

## Evidence boundaries

Measurements compare NEONFORGE builds on the same machine. They do not benchmark CET, RED4ext or other frameworks, and do not establish game FPS, frame times, resource loading or engine-call performance. A faster steady-state dispatch does not imply faster registration or lower total memory. The event index adds per-subscription bookkeeping and cached token storage; active nested emissions may retain older snapshots. Task scratch storage can retain up to 4096 token slots until runtime destruction.

Hosted CI records a small benchmark smoke report as an artifact. It checks execution and counters without enforcing timing thresholds on shared runners. Use repeated Release measurements on an otherwise idle machine for performance work; CPU clocks and scheduling still introduce variation. No universal superiority claim is made.

## Reference measurements: 2 November 2026

Same reviewed Release benchmark executable, 5,000 emissions per event sample, seven measured samples after one warmup; one machine. Task samples cancel or run 4,096 tasks. Every measured workload completed with the expected callbacks and zero pending tasks before owner cleanup. Compilation and tests had finished before this pair of runs.

| Scenario | 0.1.0 median ns/op | 0.1.1 median ns/op | Baseline / candidate |
| --- | ---: | ---: | ---: |
| event.empty | 66.78 | 64.60 | 1.03× |
| event.one | 227.48 | 141.16 | 1.61× |
| event.one.unrelated1024 | 5911.80 | 257.96 | 22.92× |
| event.one.unrelated8192 | 48512.18 | 352.42 | 137.65× |
| event.fanout64 | 4398.12 | 3176.80 | 1.38× |
| event.fanout64.unrelated8192 | 55207.40 | 5740.50 | 9.62× |
| event.missing.unrelated8192 | 46684.60 | 195.26 | 239.09× |
| task.cancel4096 | 3041.85 | 96.34 | 31.57× |
| task.pump4096.batch64 | 110.11 | 104.20 | 1.06× |

The small empty-event and task-pump differences are close enough to normal run variation that they should not be treated as established gains. The larger unrelated-subscription and cancellation gains reflect the removed linear scans. These are synthetic workloads with trivial callbacks, not a forecast of normal mods or game frame time.

Raw reports: [baseline](../benchmarks/results/baseline.json) and [candidate](../benchmarks/results/candidate.json). They include raw sample timings, CPU/OS/compiler metadata and full artifact hashes. The baseline source corresponds to commit `18add733dbe96923e18f287bcf6d741676563af5`. Reference runtime DLL SHA-256 values are `0d363362e0564dd99fa8f2c9c7ba4a6b5deb9bcaf3df6570dd3cf344759ab603` (baseline) and `d707917f2935905c4c8ce40665b615b3d533c5d9901840409ad2cf20a2fc3f12` (candidate). These historical measurements do not automatically qualify binaries compiled by a different machine or a later workflow run.
