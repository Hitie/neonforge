# Typed native call foundation

**Goal:** Add original typed values, function signature introspection, checked native invocation and caller-owned results for future scripting adapters. This is standalone function reflection, not engine RTTI or Lua support.

**Architecture:** Preserve ABI 1. Add a separately negotiated `nf_get_typed_api` extension in the existing core DLL, with fixed-width C structures and primitive value kinds. Registered functions and copied results have context ownership; closing a context invalidates its registrations/results. Function tokens never pin providers and are revalidated on each call. Executing providers cannot be released.

## Contracts

- Values: void result, Boolean 0/1, signed/unsigned 64-bit integer, finite float64, UTF-8 text and raw bytes. Text and bytes use bounded spans; each payload is at most 1 MiB. Text spans may contain embedded NUL.
- Signatures: exact argument kinds, maximum 32 arguments; void is result-only. Native invocation does not coerce argument kinds. Explicit numeric conversion is lossless or rejected, including signed/unsigned boundaries and float precision.
- Function names: original dotted identifiers, runtime-unique, maximum 128 bytes. Registration copies signature metadata. Maximum 4096 functions and 4096 outstanding results per runtime; copied result payload budget 16 MiB.
- Invocation borrows inputs for the call, enters the provider's executing state, validates returned values, and copies the result before the provider can be released. Return spans must remain valid until invocation returns. Provider C++ exceptions become callback errors. Native memory faults are outside the trust boundary.
- Results belong to the caller and survive provider removal. Reading copies text/bytes into caller storage; text includes a trailing NUL but its span length remains explicit. Short buffers report needed bytes without overwriting them. Result handles cannot be read/released by other owners and close invalidates them.
- Calls can have side effects before reporting an error, including result allocation/budget failure. They are not transactions. Engine calls and engine type discovery remain unavailable.

## Implementation and verification

1. Add the C header and actual-DLL behavior tests; confirm the extension is absent before implementation.
2. Implement strict value validation/owned copies and explicit numeric conversion. Test full integer ranges, finite floats, invalid UTF-8, malformed spans and exact byte preservation.
3. Implement owned function registry, signature lookup, invocation and result lifecycle. Test callback exceptions, wrong arity/types, stale/foreign handles, close during callbacks, reentrant calls and provider removal.
4. Add a C example and compile the header as C11. Ship a typed demo and extracted-SDK verification; retain existing native examples and ABI tests.
5. Build through both supported paths, run the full suite, obtain a fresh focused review and publish through required GitHub checks. Version the SDK only after the extension is working. No live game work is included.

## Review focus

No integer or float conversion silently loses information. Output structs are checked before writes. Callback mutation cannot invalidate metadata used after entry. Caller shutdown during a call cannot create a result for a dead owner. Provider lifetime covers result copying. Failed registrations and result copies leave canonical ownership tables consistent. Payload limits and retained-byte accounting remain bounded through repeated close/release cycles.
