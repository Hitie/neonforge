# Contributing to NEONFORGE Core

Start with [current status](docs/status.md), the [API contracts](docs/api.md) and [capability roadmap](docs/roadmap.md). This is an early standalone SDK. Game integration needs separate evidence for each exact supported executable.

## Propose a change

Open a feature issue for significant API or architecture changes before implementation. Include the mod-author use case, expected behavior, ownership/lifetime rules and a concrete acceptance scenario. Keep pull requests focused and explain any ABI or manifest changes.

## Build and verify

Follow [development.md](docs/development.md). Run the Release build and full CTest suite before submitting. Add behavior tests for runtime/tool changes, including malformed inputs and failure recovery where relevant. Use the repository's clang-format configuration for native code. Documentation-only changes need link and accuracy checks.

Separate standalone evidence from live game evidence. Include the exact game build and test scenario when making game claims. Do not describe an unimplemented capability as supported.

## Original source and licensing

Submit work you have the right to contribute under the repository's GPLv3 license. Do not copy implementations or SDK headers from reference frameworks. Do not upload game binaries, extracted game assets, saves, private inventories, credentials or third-party material without permission and appropriate notices. Discuss any infrastructure dependency and its license before adding it.

## Reports and discussion

Use the issue templates for reproducible bugs and scoped feature requests. Remove personal paths and private data from logs. Report suspected vulnerabilities privately using [SECURITY.md](SECURITY.md). Keep discussions specific, constructive and respectful.
