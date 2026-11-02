# Native SDK contracts

The header `include/neonforge/api.h` is usable from C and C++. The target is Windows x64. API major remains 1; runtime version is 0.1.1. The performance update preserves the existing table layout and contracts.

## Negotiation

Call `nf_get_api(NF_ABI_VERSION, sizeof(nf_api), &api)`. A mismatched version or table size returns `NF_E_ABI` and clears the output. This development ABI requires exact table sizes. A plugin must check `api->struct_size` before reading later fields, then check `abi_version`.

Plugins export `nf_plugin_query` and `nf_plugin_start` with C linkage and `NF_CALL`. Query metadata must match the manifest exactly. The host validates all package dependencies and capabilities, then all DLL queries, before calling any plugin's start function. Windows necessarily executes DLL initialization during loading; query preflight is not a sandbox for untrusted native DLLs.

Set `nf_plugin_lifecycle.stop` and `user` before performing partial initialization that needs manual cleanup. The host calls stop after an attempted start, including a failed start, when the lifecycle structure remains valid. The stop callback must not throw. Callers may not pass invalid memory merely because an API uses C linkage.

## Ownership and threading

- `nf_runtime*` is an opaque identity. It is not an address callers may dereference, free or perform pointer arithmetic on. Its value does not revive after state allocation reuse.
- Contexts and tokens are unique opaque 64-bit identities. They are not transferable between owners or runtimes.
- Runtime APIs run on the creating thread. A call on another thread returns `NF_E_THREAD`; the current task queue is a deferred main-thread queue, not a worker-thread executor.
- `nf_str`/`nf_bytes` are borrowed spans for the duration of a call. Strings are UTF-8, size-delimited and cannot contain embedded NUL. Event payload bytes may contain NUL.
- Exceptions and STL containers must not cross the public ABI. The core contains ordinary C++ callback exceptions where possible, but does not promise recovery from native access violations or malicious plugin code.

## Context lifecycle

Create one context per reverse-DNS mod identity. Duplicate active identities are rejected. `context_close` prevents new work, removes subscriptions, cancels queued tasks, stops service publication and releases that context's consumer leases. Closing is idempotent. It does not unload a DLL.

`context_release` requires a closed context and returns `NF_E_BUSY` if a callback is executing or another context still has a service lease on its provider. Release invalidates the identity. Runtime destruction requires all contexts released and no active API calls.

The host shutdown order is: close every context, run plugin stop callbacks in reverse order, release every context, destroy runtime state, then unload DLLs. Stop callbacks may free owned allocations and log; they must not depend on already-expired service leases or queue more work. If cleanup cannot prove safety, the standalone host exits without manually unloading the affected libraries.

## Events and tasks

Names use lower-case dotted identifiers. Event dispatch is stable in subscription order. A dispatch snapshots subscription identities: removal takes effect before a pending callback, while additions start on the next dispatch. Nested dispatch is limited to 16. A callback cannot release its own context while still executing.

Routing looks up only the requested topic and reuses an immutable token snapshot until that topic changes. A nested emission observes registrations made before it starts; an already running outer emission keeps its original snapshot. Canonical registration lookup still skips removed callbacks before invocation.

Task pumping snapshots the eligible queue. Tasks enqueued during the pump wait until the next pump. Cancellation and owner closing remove pending work. Pumping is non-reentrant. Current bounds: 1,024 contexts, 16,384 subscriptions, 4,096 queued tasks, 4,096 published services, 16,384 service leases; payloads/settings/JSON input are limited to 1 MiB.

Pending tasks have links embedded in their canonically owned nodes. Cancellation detaches a node directly rather than scanning the queue; handle lookup still uses an ordered map. Pump selection detaches nodes before callbacks, so selected work can be cancelled safely. Bounded pump scratch storage is reused. These services remain main-thread only.

Callback timing measures the execution interval of callbacks, including nested work. It is not total game-frame cost or proof of performance improvement. `stats` reports aggregate counters; `context_stats` attributes counts and callback time to one mod.

## Services

A provider publishes a versioned immutable table with a byte size and keeps its storage alive until its context can be released. A consumer acquires a lease with minimum version and table size. Higher service versions must preserve the documented prefix contract. An incompatible requirement returns `NF_E_ABI`.

Each successful acquisition pins the provider. Release the lease explicitly, or allow consumer context closing to release it. A copied raw table pointer is valid only while its lease remains active. Native code that ignores that rule is outside the lifetime guarantee.

## Settings and diagnostics

Settings keys contain ASCII letters/digits, underscore or hyphen and are not file paths. Each document is stored under `settings/<mod-id>/<key>.json` relative to the selected data root. JSON is validated and its original bytes are retained: large integer IDs and escape sequences are not converted through floating point. Replacement uses an exclusive temporary file, flushing and a replace operation. A failed validation does not replace the prior document.

Reads use caller-owned buffers. `needed` includes the terminating NUL. A size query or short buffer returns `NF_E_BUFFER`. Missing documents return `NF_E_NOT_FOUND`. Reparse paths are rejected; log files with multiple hardlinks are rejected. These checks are not a strong sandbox against a hostile concurrent native process replacing filesystem paths.

Logs are escaped JSONL with mod ownership and a sequence number. A file rotates around 1 MiB, retaining five bounded files. The maximum message is 16 KiB. `last_error` supplies a diagnostic string; status codes remain the contract. The returned capability structure does not self-certify a binary as tested: the distributed verification report records evidence. Engine capabilities return unavailable.

The examples demonstrate real DLL boundaries, not engine calls. Windows timer-export probing and matching an executable hash do not establish game layouts, engine readiness or ABI correctness.
