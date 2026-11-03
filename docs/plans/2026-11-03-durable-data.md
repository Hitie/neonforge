# Durable checkpoint storage and watched reload

**Goal:** Persist validated offline data and automatically try settled file changes while retaining the last committed state on failure.

**Spec:** Extends offline readiness step 2 and the original standalone data engine. Independent API, original implementation, unchanged primitive ABI headers and GPLv3. No game action. Implementation proceeds inline with regression-first checks and one independent whole-change review. After this milestone, continue into original offline resource composition and dependency validation as requested.

**Architecture:** Python standard-library developer tooling around the production native validator, without duplicating type/schema semantics. Native CLI gains bounded binary stdin and checkpoint normalization. A local Windows store has two checkpoint slots and current/previous SHA-256-bound commit markers. Save prepares the inactive slot before atomically publishing its marker. OS handle locks release on process exit. Polling compares complete owned bytes, waits for a settle interval, validates once per distinct candidate and publishes only after storage succeeds.

**Alternatives:** A single overwritten file cannot retain a previous commit after interrupted publication/corruption. Treating the newest slot as authoritative can promote an uncommitted preparation. The selected marker protocol preserves the old commit until the new marker is published, with explicit previous-commit recovery. File access uses checked Windows handles and pinned directory ancestors; writable files are limited to the store's reserved names. SHA-256 detects content mismatch, not malicious forgery or author identity. Process-termination tests do not establish hardware power-loss durability.

## Contracts

- Local absolute Windows paths; refuse UNC/device paths, alternate data streams, reparse paths and non-regular/multiply-linked managed files. An existing unrelated directory cannot become a store. Create a new complete store by staging and renaming its initialized directory. Preserve unrelated files; reserved names are documented.
- Owner marker identifies the store. Head markers bind format, store identity, canonical uint64 generation, slot and SHA-256. The initial marker represents generation zero with no payload. Never promote an orphan slot. Both invalid markers fail closed; a valid previous marker yields a visible recovered result. Native validation applies to all loaded/saved checkpoint content.
- Hold an exclusive non-inherited store lock throughout each operation. Publication and fixed staging cleanup use verified open handles. Reserve generations durably, allowing gaps after interruption without reuse. Compare expected commit tokens containing store identity, generation and payload identity. Canonically equal checkpoints do not increment generation or rewrite slots. A committed marker rename is the publication boundary. Transient I/O/native execution failure never triggers fallback or slot mutation; recovery from confirmed corruption is visible.
- Project inputs remain at most 1 MiB; checkpoints at most 32 MiB and 262144 nodes. Metadata is at most 4096 bytes. Generations are persisted uint64 strings. Unknown metadata fields and Boolean/inexact counters fail.
- Watch settled bytes rather than timestamps alone. Invalid, missing, unreadable and mid-edit input retains the committed checkpoint. Repeated identical validation errors are quiet. Storage I/O failure is retried with backoff; a concurrent-writer conflict requires an explicit restart/resync. Source files are read-only, outside the owned store. Changes are not executed as code.

## Review focus

1. Real process kills before/after every publication phase recover exactly old or new committed data, never a partial/uncommitted payload.
2. Store markers/slots, stale generations and corruption cannot silently reset a populated store or overwrite unrelated files.
3. Reparse/hardlink/device paths, locked files and namespace races are checked at the actual file boundary.
4. Same-size/timestamp changes, invalid drafts, delete/recreate, repeated errors and storage conflicts preserve last-valid state without busy retry loops.
5. SDK/source exports contain every dependency; stdin/normalization still use the original bounded native parser and exact integer representation.

## Task 1 — Native streaming boundary

Files: data CLI, authoring tests. Add `-` binary stdin and `canonicalize` checkpoint command. Observe missing-command RED; verify exact round trips, byte caps and broken stdout through production CLI.

## Task 2 — Durable developer store

Files: `tools/data_store.py`, `tools/windows_files.py`, `tests/data_store_test.py`. Interfaces: `Validator.checkpoint(bytes)`, `normalize(bytes)`; `Store.load()`, `save(checkpoint, expected=None)`. Results carry canonical checkpoint bytes, generation, digest and recovery status. Test round trips/idempotence, stale writes, corruption/fallback, exclusive locks, links, unowned paths and real process termination before implementation. Implement marker protocol and filesystem guards; run focused suite.

## Task 3 — Watch and distribution

Interface: `Watcher.poll(now=None)` returns a change/error event or None; tracks checkpoint/generation only after durable save. CLI `save`, `load`, `watch` uses packaged validator by default. Test settle/coalescing, same-size changes, invalid/missing/restored files, storage retries/conflicts and real bounded watch subprocess. Integrate both build paths, SDK smoke checks and docs; bump development version. Full CMake/direct/exported-source checks, independent review, single substantive-fix pass and protected GitHub publication.
