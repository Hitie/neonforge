# Standalone Lua scripting

NEONFORGE 0.1.3 embeds Lua 5.5.1 in the standalone native host. Original Lua and DLL packages share manifest validation, dependency ordering, the core event/task/settings services and typed native calls. This is a working offline authoring layer. Engine reflection, game lifecycle/input, overlay and TweakDB remain unavailable.

## Try it

Extract the SDK and open `Run-Lua-Demo.cmd`, or run from its root:

```powershell
.\runtime\neonforge.exe run .\examples\scripts .\data\lua-demo --ticks 3
```

The Lua provider exports signed addition and an exact unsigned echo. The consumer calls both, receives `nf.host.ready`, defers a task, and saves the result under `data/lua-demo/settings/org.neonforge.lua.consumer/result.json`. Lua source and module examples are included. The normal native demo is still available as `Run-Demo.cmd`.

## Package and load

Use the existing manifest with `entry: "init.lua"` and required host capability `nf.script.lua`, version 1. Declare `requires` dependencies before calling another mod's exports during initialization. Entry paths can be nested, but remain relative to the owning mod directory. `neonforge validate` validates package manifests/dependencies; `run` additionally compiles every Lua entry and checks every native plugin's query metadata before starting any mod. Modules load on demand.

One Lua state and core context belong to each mod. Globals cannot overwrite another mod's globals. `require('helpers.math')` reads `helpers/math.lua` relative to that mod, caches its result, and rejects cyclic loads, path escape, linked paths, missing/oversized files and bytecode. Module names use lower-case dotted identifier segments; a single segment is allowed. A module returning nil is cached as true. Do not use native DLL module loading or LuaRocks packages that require unavailable libraries.

## Original `nf` API

| Function | Contract |
| --- | --- |
| `nf.call(name, ...)` | Inspect and invoke a typed function, returning its copied value; native results are always released. |
| `nf.signature(name)` | Return `{result='i64', arguments={'i64', ...}}`; names and arity come from the registered signature. |
| `nf.export(name, resultKind, argumentKinds, fn)` | Publish an owned typed function; names are runtime-unique and registration follows the [typed API](typed-calls.md). |
| `nf.u64(decimalString)` | Construct an exact unsigned 64-bit value; `tostring` and equality preserve its full value. No arithmetic/coercion to Lua numbers is provided. |
| `nf.on(name, fn)` | Subscribe `fn(payloadString, senderU64)`, returning an opaque callback handle. |
| `nf.off(handle)` | Remove an owned subscription; return false if already removed. |
| `nf.emit(name, payloadString)` | Emit bounded bytes synchronously, preserving nested dispatch semantics. |
| `nf.defer(fn)` | Enqueue a main-thread task, returning an opaque task handle. Tasks queued during a pump wait for the next pump. |
| `nf.cancel(handle)` | Cancel an owned pending task; return false if already completed/cancelled. |
| `nf.settings_write(key, jsonText)` | Validate and atomically preserve the exact JSON text under the owning mod. |
| `nf.settings_read(key)` | Return exact JSON text or nil when absent. |
| `nf.log(message)` | Write an attributed structured log entry. |
| `nf.id` | The mod's identity string for diagnostics; changing this Lua field does not change ownership. |

Kinds are `void`, `bool`, `i64`, `u64`, `f64`, `utf8` and `bytes`. Void is result-only and returns nil. Signed values require Lua integers, not floats. Unsigned arguments require `nf.u64`, avoiding double rounding and signed overflow. Float parameters accept finite numbers; converting an integer to float must be exact. UTF-8/bytes use Lua strings, including embedded NUL, with signature-dependent validation. Invalid arity, type, UTF-8 and native statuses raise ordinary Lua errors. Lua arithmetic itself follows Lua's rules: signed integer addition can wrap; check overflow where required. The included addition example does so.

```lua
nf.export('my.mod.increment', 'i64', {'i64'}, function(value)
    assert(value < math.maxinteger, 'integer overflow')
    return value + 1
end)

nf.on('nf.host.ready', function()
    local answer = nf.call('my.mod.increment', 41)
    nf.defer(function()
        nf.settings_write('result', string.format('{"answer":%d}', answer))
    end)
end)
```

## Limits, errors and reload

Defaults per mod: 8 MiB of Lua allocator memory, 1,000,000 VM instructions per entry/callback, 512 live bindings and 512 cached/in-progress modules. Script invocation nesting is limited to 16; core payload and handle limits still apply. Instruction hooks sample every 100 instructions; custom limits may overshoot by up to 99 VM instructions. VM quotas return control to the host, including through protected module calls, sort/substitution callbacks and protected error handlers. Original `pcall`/`xpcall` adapters preserve ordinary results and handler behavior while propagating quota exhaustion through non-yieldable frames. Parsing and C library/native functions are not VM instructions; quotas are not a hard wall-clock deadline. Lua allocator metrics exclude native adapter/container and core memory.

Selected base, table, string, math and utf8 functions are available. OS, I/O, debug, package, user coroutines, dynamic loading, arbitrary file access, direct garbage collection and user metatable access are unavailable. Metatable access is withheld in this first API to prevent script-defined finalizers from executing unbounded code during teardown. `print` is replaced by `nf.log`. This restricted environment is not a security sandbox for malicious native plugins or a promise of full Lua/CET API compatibility.

Unhandled entry/callback errors fault that mod, close its core context and remove its registrations/tasks. Diagnostics name the mod and chunk, and are bounded. Other states remain independently usable; the command-line host treats an unhandled callback failure as a failed run and shuts down the package set. Typed calls may already have effects before failing; they are not transactions.

The embedding `ScriptHost` offers `reload(id)` as stop-and-reload, with 100-cycle regression coverage. It closes/releases the old context and creates a fresh state, preserving settings. A failed replacement leaves the old code stopped. Reentrant unload/reload is rejected. There is no file watcher or in-game reload UI in this build; restart the standalone host to reload from the command line. All embedding operations, including destruction, require the creating thread and a live core runtime.

## Benchmarks and provenance

The SDK includes `runtime/nf_script_benchmark.exe`. Run `runtime/nf_script_benchmark.exe 250 7` for raw JSON samples of typed Lua calls, event callbacks and complete load/unload cycles. One warmup sample is discarded. Every sample validates observable results and complete resource cleanup. Reported peak Lua bytes include setup/verification allocations and exclude C++/core/process memory. These synthetic workloads do not compare external frameworks, loading real game mods or game FPS. Timing has no CI performance threshold.

Lua source is pinned to its official archive, compiled unmodified as C++ for exception unwinding, and verified against per-file hashes. See [dependency notices](../THIRD_PARTY_NOTICES.md), [the official manual](https://www.lua.org/manual/5.5/manual.html) and [the official download](https://www.lua.org/download.html). Original adapter code remains under the repository GPLv3 license; Lua retains its MIT notice.
