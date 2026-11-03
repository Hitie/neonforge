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

The root has `format: 1`, a `schemas` object, an optional `templates` object and a `transactions` array. A schema defines a `fields` object whose descriptors specify exact value types and an optional Boolean `required`. Required fields may be temporarily absent while constructing a record, but every one must exist at commit. Unknown fields, schemas and document properties are rejected.

Each batch has `owner`, optional `source`, optional canonical decimal `expect_revision` and `operations`. Owners follow core mod-ID rules. Batches execute in their authored array order. Later valid field writes win; `plan` explains cross-owner overwrites rather than silently treating them as unrelated edits. A future policy layer can impose stricter conflict rules; this version does not infer mod dependencies from field edits.

| Operation | Required fields besides `op` |
| --- | --- |
| `create` | `record`, `type` |
| `instantiate` | `record`, `template`; optional `fields` object overrides template values |
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

Database and transaction operations require the creating thread. Transaction entry is serialized so a rejected cross-thread operation before commit also poisons its staged batch. Callers must still synchronize moving or destroying the handle itself. Immutable snapshots can be inspected independently. A snapshot can outlive its database; an outstanding transaction cannot commit after database destruction. Moved-from handles reject access. Field history is chronological; its sequence number is relative to the authoring transaction, not a globally unique edit ID. Checkpoints preserve that authored order without claiming its authenticity. No automatic committed-change undo or game-session handling is provided.

## Reusable schemas and templates

`examples/data/reusable-items.json` is an original complete example. A schema can add `extends: "BaseSchema"`; inheritance is single-parent and additive. A child may add fields but cannot redeclare any inherited field, including to weaken its required/type contract. Forward references work; every definition is checked, including unused ones. Normalized schemas in checkpoints contain the expanded fields with explicit required flags.

A template has `type` and `fields`, or `extends` and `fields` to inherit another template's type/defaults. A child cannot change its parent's type. Whole tagged values from child fields replace parent defaults; instance `fields` then replace template defaults. Arrays replace as a whole. All defaults, including unused ones, pass normal value/type validation. Partial templates may omit required fields, which must be supplied by a child or instance before commit. Each instance has independent record storage. Provenance records the instantiating owner/source and the generated `set` operations; template origin is represented by the project source, not a second attribution identity.

Schema and template registries are each capped at 4096 definitions, depth 32, 16384 total expanded fields and 4 MiB encoded expanded registry. Existing per-record and tagged-value bounds also apply. Generated create/set operations consume the ordinary transaction operation budget. Validated template defaults share immutable value storage across instances; a record edit replaces its own value without changing other instances. The same native engine serves database construction, authoring validation and overwrite plans.

## Checkpoints and last-valid reload

```powershell
.\runtime\nf_data.exe checkpoint .\examples\data\reusable-items.json
.\runtime\nf_data.exe restore .\checkpoint.json
.\runtime\nf_data.exe reload .\checkpoint.json .\candidate-project.json
```

`checkpoint` prints a versioned `neonforge.offline-data-checkpoint` JSON document containing normalized schemas and the complete current state. Save its UTF-8 stdout through your chosen file API. `restore` validates a checkpoint and prints its snapshot. Checkpoint inputs/outputs are at most 32 MiB and 262144 JSON value/container nodes; object keys are not separate nodes. Export checks the expanded node count before materializing its output; restore enforces the same cap while parsing. Project inputs and the generic JSON parsers retain their existing 1 MiB limit. Output includes exact string counters and retained provenance. Unknown fields, schema/type mismatches, missing required fields, malformed histories, budget violations and enabled game flags fail. Histories start with `set`; array operations require array fields. Revision-one checkpoints also require matching live/changed names and consistent first-transaction attribution/sequence order. Checkpoints are editable, unauthenticated offline data; they do not prove who originally made an edit. A standalone Database may be too large for checkpoint output even when it fits its logical state budget; the checkpoint limit fails explicitly.

`ProjectSession::reload` builds a complete fresh candidate, verifies its checkpoint export, then replaces the active database only on success. Parse, schema, template and transaction errors leave the active state and session generation unchanged. Equivalent normalized checkpoints do not increment generation; schema-only changes do. Generations are process-local uint64 counters; rebuilt projects retain their own transaction revision counts, so revisions alone are not reload generations. Existing snapshots survive replacement. Session methods require the creating thread. `restore` uses the same staged replacement boundary. Session error text is capped at 4096 bytes.

CLI `reload` restores the supplied checkpoint then tries one candidate. It prints `accepted`, `changed`, `generation`, `error` and `state`; a rejected candidate returns nonzero **with the retained last-valid state**. This structured rejection output is intentional. An invalid starting checkpoint or unreadable/oversized input fails without a successful snapshot. CLI calls are separate processes; use the native session for repeated reloads. Successful direct Database restore replaces its state identity, invalidating pre-restore transactions even when numeric revisions match; failed restores preserve outstanding transactions.

The executable writes only stdout/stderr. There is no built-in watcher, durable file replacement, interrupted-write recovery, authenticity check or game-save support. Do not redirect over your only good checkpoint before checking the candidate; use a separate output and your own atomic file-publication policy. Windows PowerShell versions differ in default redirection encodings; checkpoint files must be UTF-8.

## Remaining database work

File watching, durable checkpoint publication/interruption recovery, native/Lua expanded-value adapters and engine-backed TweakDB operations remain separate work. This milestone provides reusable offline authoring and validated checkpoint recovery through a caller-controlled storage boundary. All engine-ready/game-tested flags remain false.
