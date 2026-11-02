# Build and verification

Target: Windows x64, C++20. The source includes both a conventional CMake build and a direct-MSVC script for the tested local environment. No game SDK headers are needed for the current standalone runtime.

## CMake

```powershell
cmake -S . -B build -A x64 -DPython3_EXECUTABLE="C:/path/to/python.exe"
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Use a Windows SDK and the MSVC C/C++ workload. Python 3.10+ is used for integration and tool tests. The tested local compiler is MSVC 19.51 for x64 with Windows SDK 10.0.26100.0. The direct script detects the newest installed MSVC under Visual Studio 2026 Community and configures those SDK headers/libraries explicitly:

```powershell
.\build.ps1 -Python "C:/path/to/python.exe"
```

If tool discovery is restricted, pass the Python executable explicitly and run MSBuild with the permissions needed to read the installed toolchain metadata. Filesystem link fixtures require ordinary Windows link permissions and are confined to temporary test directories.

## Tests and proof boundaries

Package tests cover JSON, Unicode, malformed paths/manifests, semantic versions, dependency ordering/cycles and capability checks. Kernel tests exercise the real DLL, context ownership, callback mutation, task cancellation, leases, settings persistence, exact JSON preservation, stale runtime IDs and 100 cleanup cycles. Integration tests load actual C/C++ DLLs and prove service exchange, restart persistence, all-query-before-start behavior and failed-start cleanup. ABI tests pass deliberately truncated tables to the shipped examples and generated starter. Tool tests exercise real ZIP/filesystem operations, modified-file preservation and rollback. The Windows probe resolves system-library name/ordinal exports and performs representative timer calls.

Typed tests exercise exact numeric conversions, function signatures, caller-owned result storage, callback shutdown/mutation, nesting and resource limits. The SDK includes the typed C demo and an import library; distribution tests rebuild and run that example using only the extracted SDK.

Script tests exercise per-mod isolation, quotas through protected module calls, OOM/result cleanup, exact typed integers, nested/self-removing callbacks, cancellation, 100 reload cycles and original SDK Lua demos. Mixed-entry tests check syntax preflight before native startup and attributed callback errors. Script benchmarks verify real work and cleanup without CI timing thresholds.

These tests establish standalone behavior only. They do not establish engine reflection, resource loading, script compiler correctness, live reload of native modules, game performance or compatibility with other mod loaders.

## Review changes

Runtime and package regression tests cover API truncation, JSON numeric/size preservation, allocation-address runtime identity reuse, query/start ordering and installer schema validation. These cases were observed failing before the corresponding fixes and passing afterward.

## Dependency policy

The original implementation uses the Windows API, C/C++ standard library and Python standard library. The host also embeds pinned Lua 5.5.1, built unmodified as C++ for exception unwinding; `tools/verify_dependencies.py` checks every vendored source/header hash. See [notices](../THIRD_PARTY_NOTICES.md). Windows/MSVC system components remain external requirements. No upstream mod framework code or headers are bundled. Later parser/decoder dependencies must also have pinned provenance and retained attribution before entering the distribution.
