# Offline typed data engine

NEONFORGE's original C++ data model validates typed records and publishes complete transaction batches atomically. The SDK includes `runtime/nf_data.exe` and an original project under `examples/data/`. This is standalone data authoring, not a reader/writer for Cyberpunk's TweakDB files. It does not launch the game, edit its files or certify engine layouts.

## Run an original project

```powershell
.\runtime\nf_data.exe validate .\examples\data\original-items.json
.\runtime\nf_data.exe plan .\examples\data\original-items.json
.\runtime\nf_data.exe apply .\examples\data\original-items.json
```

`Run-Data-Demo.cmd` prints the plan and final result. All three commands read one UTF-8 JSON project of at most 1 MiB. `validate` executes validation against a temporary offline model. `plan` reports revisions, changed records and cross-owner field overwrites in input transaction order. `apply` prints the final snapshot, including value provenance. Output goes to stdout; the executable never writes a project or game file. Failure returns nonzero, reports its owner/source/record/field where available, and prints no apparent successful snapshot. JSON syntax errors include their byte offset.

Expanded plans have their own limits: 4096 entries across batch summaries, changed-record references and overwrite entries, plus 4 MiB of encoded transaction-row content. A project may apply successfully yet exceed these preview limits; planning then fails explicitly. A failed stdout write also returns nonzero, since a truncated result is not a successful export.

The example creates `Items.Original`, clones it into `Items.Custom`, edits the clone through a second owner and applies an explicitly ordered balance patch through a third owner. The original stays unchanged. The final snapshot preserves the exact unsigned identity `18446744073709551615`, and the plan identifies the prior owner of the overwritten damage field. These are original symbolic examples, not game records or usable game items.

## Project format

The root has exactly `format: 1`, a `schemas` object and a `transactions` array. A schema defines a `fields` object whose descriptors specify exact value types and an optional Boolean `required`. Required fields may be temporarily absent while constructing a record, but every one must exist at commit. Unknown fields, schemas and document properties are rejected.

Each batch has `owner`, optional `source`, optional canonical decimal `expect_revision` and `operations`. Owners follow core mod-ID rules. Batches execute in their authored array order. Later valid field writes win; `plan` explains cross-owner overwrites rather than silently treating them as unrelated edits. A future policy layer can impose stricter conflict rules; this version does not infer mod dependencies from field edits.

| Operation | Required fields besides `op` |
| --- | --- |
| `create` | `record`, `type` |
| `clone` | Target `record`, source `from` |
| `set` | `record`, `field`, tagged `value` |
| `erase_record` | `record` |
| `erase_field` | `record`, `field` |
| `append`, `prepend` | `record`, `field`, tagged element `value`; optional Boolean `unique` |
| `remove` | `record`, `field`, tagged element `value`; removes all exact matches |

Record identifiers are at most 256 bytes; schema/field identifiers are at most 128. They use nonempty ASCII letters, digits, underscore, dot and hyphen, preserving case. Source labels are UTF-8 without NUL, at most 240 bytes. These identifiers are not filesystem paths.

## Exact value representation

Values are objects with `type` and `value`. An enum additionally declares `enum_type`; an array additionally declares its `element` type descriptor. For example:

```json
{"type":"u64","value":"18446744073709551615"}
```

```json
{"type":"array","element":{"type":"enum","enum_type":"Rarity"},"value":[{"type":"enum","enum_type":"Rarity","value":"Rare"}]}
```

| Type | Representation and validation |
| --- | --- |
| `bool` | JSON Boolean |
| `i64`, `u64` | Canonical decimal string in the full signed/unsigned range; no leading plus/zeros or negative zero |
| `id` | Canonical uint64 decimal string; no game-specific hashing or null semantics inferred |
| `f64` | Finite JSON number with float64 precision; exact identifiers belong in integer/string types |
| `string` | UTF-8 text; embedded NUL is preserved |
| `name`, `localization` | Nonempty UTF-8 symbolic text without NUL, at most 128 bytes |
| `resource` | Owned relative path syntax, at most 240 bytes; existence and engine loading are not checked |
| `enum` | Named domain plus an identifier member; domain identity is enforced, engine enumeration membership is not known |
| `vector2`, `vector3`, `vector4`, `quaternion` | Exactly 2/3/4/4 finite float64 components, without normalization |
| `color` | Four integer channels in 0..255 |
| `array` | At most 4096 tagged elements of exactly its declared type/domain, including when empty |

Nested arrays are supported to depth 16. A value is at most 1 MiB encoded, with additional structural/text preflight bounds. No implicit type coercion occurs. Compound values are original offline representations; they do not claim a matching engine size, packing, coordinate convention or precision. The existing native typed ABI and Lua bridge continue to support their previously documented primitive kinds; this engine does not silently change those ABI tables.

The data CLI preserves numeric source tokens until exact integer/color/version checks finish, rejecting a fractional literal that would otherwise round to an integer in float64. Native C++ source consumers should use `parse_json_exact` for the same source-level guarantee; the generic `parse_json` retains its existing float64 behavior. Programmatically constructed numeric values are checked as their supplied binary values. Valid mathematical integer spellings such as `2.55e2` are accepted for a color channel; identifiers and 64-bit integers still require canonical decimal strings. Value storage discards numeric-token metadata after validation.

## Transactions, ownership and limits

Values own immutable encoded data. Snapshots share immutable records; a transaction copies only a record it edits, retaining shared value storage. Readers holding an old snapshot continue to see the old state after a commit. References obtained from a snapshot require that snapshot to remain alive; copying a Value retains its owned data independently.

A transaction records its starting snapshot. A changed successful commit increments the uint64 revision. A no-op means the final live record types, values and provenance match the starting snapshot; transient create/delete or optional-field set/delete pairs do not publish new revisions. Clean batches do not charge their unpublished labels to the state budget. A stale base conflicts even if it would otherwise be a no-op. An exception from an entered operation poisons the batch, so catching it cannot publish earlier partial edits. Value parsing that fails before a C++ operation is called cannot poison that operation; the CLI discards the failed batch/project automatically. Explicit abort also works after an operation fails. Abandoned transactions release their unpublished state.

Default limits are 4096 records, 256 fields per record, 4096 operations per transaction, 64 provenance entries per live field and 16 MiB of current-state logical data. Byte accounting includes encoded values, identifiers and provenance text/counters. Exceeding history or state budgets fails rather than discarding attribution. These bounds exclude C++ container/allocator overhead, the schema registry, private staging and caller-retained snapshots; they are not a process-wide RAM cap.

Database and transaction operations require the creating thread. Transaction entry is serialized so a rejected cross-thread operation before commit also poisons its staged batch. Callers must still synchronize moving or destroying the handle itself. Immutable snapshots can be inspected independently. A snapshot can outlive its database; an outstanding transaction cannot commit after database destruction. Moved-from handles reject access. Field history is chronological; its sequence number is relative to the authoring transaction, not a globally unique edit ID. No crash-recovery persistence, automatic committed-change undo or game-session handling is provided by this in-memory model.

## Remaining database work

Template expansion/schema inheritance, file watching and last-valid reload, persistent snapshot import/export recovery, native/Lua expanded-value adapters and engine-backed TweakDB operations remain separate work. This milestone provides the production validation/transaction foundation and a usable original authoring CLI. All engine-ready/game-tested flags remain false.
