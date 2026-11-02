# Typed values and native calls

NEONFORGE 0.1.2 adds standalone function reflection through `include/neonforge/typed.h`. A provider registers a function and its signature; callers inspect that signature and invoke it through a checked C boundary. This supports future scripting adapters. Engine RTTI, game calls and Lua remain unavailable.

## Negotiate and build

Call `nf_get_typed_api(NF_TYPED_ABI_VERSION, sizeof(nf_typed_api), &typed)` and check the status, pointer, `struct_size` and `abi_version` before accessing function pointers. A version or size mismatch returns `NF_E_ABI` and clears the output. The existing `nf_api` ABI 1 table is unchanged. Capabilities `nf.values` and `nf.reflection.functions` are version 1; engine reflection is unavailable.

Standalone applications link `lib/NeonForge.Core.lib` from the extracted SDK. Keep the matching `runtime/NeonForge.Core.dll` beside the executable. From an MSVC x64 developer prompt at the SDK root:

```bat
cl /nologo /std:c11 /W4 /MD /Iinclude examples\native\typed_calls.c lib\NeonForge.Core.lib /Fe:runtime\typed.exe
runtime\typed.exe
```

`Run-Typed-Demo.cmd` launches the prebuilt C example. It registers `demo.math.add`, resolves its two-integer signature, invokes `20 + 22`, reads/releases the result, saves `{"answer":42}` under `data/typed-demo/settings/org.neonforge.typed.consumer/result.json`, and closes both contexts. No game is required. A plugin can resolve `nf_get_typed_api` from its already loaded core module using `GetProcAddress`, checking for absence before calling it.

## Values

Set `struct_size` to `sizeof(nf_value)` and use the union member matching its kind: void (results only), Boolean 0/1, signed/unsigned 64-bit integer, finite float64, UTF-8 text or raw bytes. Each text/byte payload is at most 1 MiB. Empty spans may have null pointers. Text permits embedded NUL; its explicit length controls the value. Identifiers and base-API strings still reject embedded NUL.

`value_convert` supports explicit lossless numeric conversion. Negative signed integers cannot become unsigned; integers become float64 only when exactly representable; fractional/out-of-range floats cannot become integers. NaN, infinity, malformed UTF-8 and invalid Booleans are rejected. Other kinds support only validated same-kind copies. Failure leaves the output unchanged. Same-kind text/byte conversion copies the borrowed span, not its storage. Conversion is independent of a runtime and may run on any thread.

## Functions

Runtime-unique names use lower-case dotted identifiers, at most 128 bytes. Limits are 4,096 functions and 32 arguments per function. Set the exact signature size and zero reserved field; void is result-only. Registration copies names and argument kinds. Callback code and user storage must remain valid while calls can execute.

`function_resolve` fills `nf_function_info` with the token, provider, result kind and argument count. A null/short argument-kind array returns `NF_E_BUFFER` while still filling the info structure. Zero-argument functions need no array. Missing names return `NF_E_NOT_FOUND`. Lookup does not pin providers; invocation checks that the function still exists.

`function_invoke` requires exact arity and kinds without coercion. Inputs are borrowed until return. Callbacks receive provider/caller identities and an initialized void result. Set the matching result and return `NF_OK`, or return a defined status. Exceptions, unknown statuses and malformed/mismatched results produce `NF_E_CALLBACK`. Invalid pointers and native access violations are outside the trusted-plugin contract.

Runtime operations require the creating thread. Nested typed calls are limited to 16. Providers remain executing through result validation and copying; release during this interval returns `NF_E_BUSY`. Callbacks may unregister themselves or close their provider. Returned spans must remain valid until invocation returns; pointers into expired callback-local variables are invalid. Closing the caller during a callback prevents a result and returns `NF_E_STATE`.

Calls may have effects before an error, including result allocation/budget failures after the callback. They are not transactions; do not blindly retry effectful calls. Registration and invocation clear the output token on failure.

## Results and cleanup

Successful calls create caller-owned results, including void results. Limits are 4,096 outstanding results and 16 MiB of retained text/byte payload per runtime. Storage is copied before providers leave their executing state and survives provider removal. Release results explicitly when finished. Closing a context removes its registrations and results. Tokens cannot be used across runtimes or result owners; stale function tokens return `NF_E_HANDLE`.

`result_read` requires the exact output structure size and a non-null `needed` pointer. Primitive/void results need no buffer and report zero bytes. Text needs payload length plus one terminating NUL; span length excludes the terminator. Bytes need exactly payload length. A short/null buffer returns `NF_E_BUFFER` and required size without changing the output value or buffer. Empty text needs one byte; empty bytes need none. Successful reads copy into caller storage, which remains valid after result release.

`stats` reports current functions, results and retained payload bytes. Tests cover numeric boundaries, malformed values, copied metadata, ownership, foreign/stale handles, callback mutation/failure, nesting, resource limits and cleanup. SDK tests run the binary and rebuild the C example using only shipped headers/import library. These establish standalone behavior only.
