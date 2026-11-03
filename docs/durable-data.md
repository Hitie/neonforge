# Durable offline data and watched reload

`tools/data_store.py` adds an owned Windows checkpoint store and polling watch service around the original native data engine. It is developer tooling, requires Python 3.10+ and runs outside the game. The native validator still owns all value/schema/checkpoint semantics; Python does not implement a second database.

From the extracted SDK:

```powershell
python .\tools\data_store.py save .\examples\data\reusable-items.json .\checkpoint-store
python .\tools\data_store.py load .\checkpoint-store
python .\tools\data_store.py watch .\examples\data\reusable-items.json .\checkpoint-store
```

The first operation creates a complete owned store when the destination does not exist. Its parent must already exist. An existing directory must already contain valid NEONFORGE store ownership; unrelated directories, including existing empty ones, are refused. `save` validates the project and commits its canonical native checkpoint. `load` validates committed content and prints its checkpoint. `watch` compares read-only project bytes every 250 ms by default, requiring 500 ms of unchanged bytes before trying a candidate. Ctrl+C ends the watcher. `--poll-ms`, `--settle-ms` and `--iterations` support bounded developer runs; iterations zero means continuous. In a source checkout pass `--validator path\to\nf_data.exe`; the extracted SDK finds its own native executable automatically.

Native data commands also accept `-` for binary UTF-8 stdin. `nf_data canonicalize checkpoint.json` validates and prints a normalized checkpoint. Normalization uses the same exact parser, schema validation, 32 MiB/262144-node checkpoint limits and disabled game flags as the database model. Project input remains limited to 1 MiB. See [data model](data-model.md).

## Commit and recovery contract

A store owns `owner.json`, `LOCK`, `sequence.json`, `head.json`, `previous.json`, `slot-0.json`, `slot-1.json` and `.pending-write`. Other entries are preserved. Ownership and commit metadata are strict bounded JSON. A marker binds its store identity, a canonical uint64 generation string, slot and SHA-256 digest. The initial generation-zero marker represents no saved checkpoint.

Each save takes an exclusive OS handle lock, validates both commit markers, reserves a new generation in `sequence.json`, preserves the active marker as `previous.json`, flushes a newly written inactive slot, then flushes and renames the new `head.json`. Publication happens at the final head rename. The old commit remains authoritative before that point; a prepared slot is never promoted just because it is newer. Canonically equal checkpoints do not create another generation. Reservations can leave gaps after interruption, but recovery does not reuse a generation. Library callers supply the returned `token` as `expected` to reject competing commits, including replacement of the entire store with another store at the same generation.

Reads check both markers, digests and native checkpoint validation. Confirmed missing or malformed markers/content can use the other valid commit; the newer valid generation wins, and conflicting identities at the same generation fail. Access/sharing errors, native launch failures and timeouts abort without selecting an older commit or mutating slots. A valid previous commit or an unused reservation yields visible `recovered` status, including on writes and watch startup. If neither marker is valid, or the reservation metadata is invalid/behind a valid commit, the tool fails closed. A corrupted current slot can lose the latest commit while preserving the previous one; recovery status must not be ignored. SHA-256 detects mismatch and does not authenticate provenance against an editor who can rewrite the entire store.

The persistent lock file is not a stale PID lock: Windows releases the exclusive handle on process exit. A restart removes the fixed interrupted staging file by its verified handle while holding this lock. Initial directory creation stages complete owner/empty-head/reservation metadata before publishing the directory through its held handle. An interrupted or competing unpublished initialization can leave a uniquely named `.nf-store-init-*` sibling; it is not a store or a committed checkpoint and is never adopted. Initialization never cleans a vacated staging pathname or recursively cleans sibling directories.

File writes flush before same-directory rename through the still-open source handle; publication and staging cleanup remain bound to that verified object. Tests forcibly terminate the writer at half-write, flush and rename boundaries and verify old/new commit recovery. This qualifies tested process-interruption behavior, not hardware power-loss durability, network filesystems, game saves or malicious privileged access. The implementation uses documented [CreateFile sharing semantics](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew) and [SetFileInformationByHandle](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-setfileinformationbyhandle).

## File and watcher boundaries

Paths must name local fixed-drive Windows filesystem entries of at most 240 characters. Raw names are checked before Win32 normalization. UNC/network drives, device paths, alternate streams, ambiguous trailing names, reparse components and multiply linked managed files are refused. Source/store containment compares handle-resolved volume GUID paths rather than drive-letter or short-name spelling. Checked file handles deny concurrent modification while bytes are read; ancestor directory handles remain open against rename during each operation. This protects normal local operations and explicit link mistakes, not a security boundary against another privileged writer.

The watched/saved project must be outside the owned store. Its complete bytes determine change identity, so timestamp-preserving edits are still detected. Failed reads, missing files, invalid drafts and failed native validation retain the active checkpoint. Repeated identical document-rejection errors are quiet until content changes; native launch/timeouts and storage I/O failures use bounded-backoff retry. A competing commit identity produces a conflict and requires restart/resync. Active state/generation updates only after the durable save succeeds. Successful events contain persisted generation strings; recovery/error events are separate from committed events. The bounded watch command's exit code reports completion of the watch loop; inspect events for candidate acceptance.

Polling and native validation occur in the developer process, not the game frame loop. No measured game performance or engine-backed reload claim follows from these checks.
