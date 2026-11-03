# NEONFORGE Core — capability ledger

Research/design baseline, 2 November 2026 (Australia/Sydney). 95 tracked capability groups describe the full target and required acceptance evidence. Some standalone foundation work is implemented; [current status](status.md) is authoritative for what works today. The full game-facing groups below are not qualified by standalone tests. This baseline comes from the six reference frameworks plus the original package/SDK design, not a claim that every undocumented behavior has been discovered.

A row can be split into smaller test cases during implementation. Passing a host fixture does not qualify as game-tested. Acceptance applies to a specific game build, module version and feature configuration. Native crashes cannot be reliably isolated inside one process; exceptions and malformed inputs are caught where safe, otherwise unsupported execution is refused.

## Native runtime and plugins

Target: Core / Native. Earliest milestone: 1. Reference interfaces: [source 1](https://docs.red4ext.com/mod-developers/creating-a-plugin).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| HOST-01 | Windows x64 bootstrap and teardown | Launch the exact supported executable into the menu, then exit cleanly in 20 repeated runs. |
| HOST-02 | Versioned C ABI and feature negotiation | Load an older compatible example plugin; reject an incompatible ABI without entering its code. |
| HOST-03 | Plugin discovery, load and dependency ordering | Run a three-plugin dependency chain; report a missing dependency and a cycle by name. |
| HOST-04 | Plugin lifecycle and engine-ready stages | Prove init, engine-ready, session, shutdown order; no engine access before allocator/RTTI readiness. |
| HOST-05 | Native engine type registration | Register one original engine-visible type and construct/use/dispose it through the game VM. |
| HOST-06 | Native function and game-state callbacks | Invoke a registered function from a compiled game script and receive a typed result. |
| HOST-07 | Owned hooks, conflict detection and removal | Install two chained hooks, reject overlapping incompatible patches, and restore original bytes on teardown. |
| HOST-08 | Build identification and validated address resolution | Accept the scanned build; reject a changed executable or ambiguous hash with no engine writes. |
| HOST-09 | Thread dispatch, async jobs and cancellation | Send a worker result to the main thread; cancel pending work at session end without a stale callback. |
| HOST-10 | Per-plugin diagnostics, metrics and bounded logs | Attribute an error to the plugin and rotate logs under a repeated-error workload. |

## Runtime scripting and developer overlay

Target: Script / Overlay. Earliest milestone: 3. Reference interfaces: [source 1](https://www.nexusmods.com/cyberpunk2077/mods/107), [source 2](https://wiki.redmodding.org/cyber-engine-tweaks/cet-functions).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| LUA-01 | Embedded Lua environment per mod | Two mods with identical globals cannot overwrite each other's state; module loading remains mod-local. |
| LUA-02 | Game reflection calls and typed values | Round-trip names, IDs, vectors, enums, arrays, refs, weak refs, variants and nulls without corruption. |
| LUA-03 | Lifecycle, frame, draw and overlay callbacks | Check callback sequence across menu, load, pause, resume, reload and exit. |
| LUA-04 | Keyboard, mouse and controller bindings | Bind press, release, held and axis input, resolve a conflict, and retain remapped settings. |
| LUA-05 | Before/after observers and function replacements | Observe a method and replace another with a reversible, ordered chain; test return values and recursion. |
| LUA-06 | Dynamic proxy implementations | Supply a script callback for a supported game interface and release it at session end. |
| LUA-07 | Console, command history and command registry | Execute a command, see source-attributed errors, and restore history after restart. |
| LUA-08 | Immediate developer UI and widget/input capture | Draw controls and text, interact with them, then prove game input resumes when the overlay closes. |
| LUA-09 | Database read/edit tools | Inspect and change a typed database value; detect a type error and restore the previous value. |
| LUA-10 | Safe script reload and callback cleanup | Reload 100 times; callbacks and references remain bounded and old code is not called. |
| LUA-11 | Per-mod storage, filesystem and inter-mod services | Persist an owned file, reject path escape, and call a versioned service exported by another mod. |
| LUA-12 | Opt-in game patches | Enable and disable each explicitly listed patch independently; confirm no unrelated setting changes. |

## Database edits and declarative data

Target: Database. Earliest milestone: 4. Reference interfaces: [source 1](https://github.com/psiberx/cp2077-tweak-xl/wiki/YAML-Tweaks), [source 2](https://www.nexusmods.com/cyberpunk2077/mods/4197).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| DB-01 | Read, set, create, clone and delete records/flats | Create and clone an original item record; inspect it in the running game and roll it back. |
| DB-02 | Type-safe scalar and compound values | Reject a mismatched scalar; round-trip vector, colour, quaternion, ID, resource and localization values. |
| DB-03 | Array append, prepend, remove, replace and unique operations | Apply competing edits with stable ordering and verify the exact resulting game array. |
| DB-04 | Original declarative patch format | Validate, plan and apply an original patch document; show file/line diagnostics for invalid input. |
| DB-05 | Inline records and reusable templates | Expand a parameterized item definition to distinct records without ID collisions. |
| DB-06 | Inheritance and dependency resolution | Resolve a three-level inheritance chain and report cyclic or missing references. |
| DB-07 | Dynamic/script-driven database changes | Apply a change at the documented runtime stage and update the affected record correctly. |
| DB-08 | Atomic patch transactions and rollback | Inject a failure halfway through a multi-record batch; prior values remain intact. |
| DB-09 | Load order, provenance and conflict explanation | For two edits to one value, show both owners, order and the resolved result. |
| DB-10 | Watch/reload, inspection and generated bindings | Reload a valid patch; retain the last valid state on a syntax error; bindings match the live schema. |
| DB-11 | Original convenience definitions for localization/icons/vendors | Create a fully usable example item with name, icon and vendor entry using documented NEONFORGE data. |

## Archives and resources

Target: Resources. Earliest milestone: 5. Reference interfaces: [source 1](https://github.com/psiberx/cp2077-archive-xl/wiki), [source 2](https://www.nexusmods.com/cyberpunk2077/mods/4198).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| RES-01 | Archive registration and explicit priority | Mount two test archives, resolve a deliberate collision predictably, and show ownership. |
| RES-02 | Resource existence, asynchronous loading and references | Load a valid resource, reject a missing path, cancel a request and release every acquired handle. |
| RES-03 | Entity factories and original item resources | Add and spawn an original entity/item without replacing a base archive. |
| RES-04 | Localized texts, fallback and runtime refresh | Display original multilingual text and verify a missing-language fallback. |
| RES-05 | Composable entity/appearance/mesh property edits | Apply two non-overlapping edits; explain and reject an incompatible overlap. |
| RES-06 | Dynamic appearance parameters and fallbacks | Resolve an appearance for varied body/arms/feet states and verify a missing-variant fallback. |
| RES-07 | Material templates and dynamic paths | Render the intended material in daylight/night and invalidate it after a safe reload. |
| RES-08 | Component overrides, parts, masks and visual tags | Equip/remove an original garment and verify intended components in first/third person. |
| RES-09 | Custom body types and attachment slots | Equip an item in an original slot; confirm wardrobe and camera transitions. |
| RES-10 | Character customization options and hair/eyes | Select original supported options, save/reload and verify appearance persistence. |
| RES-11 | Animations, resource links and library widgets | Use a linked resource and animation/widget from an original package without a base-file replacement. |
| RES-12 | World streaming registration and node mutation | Stream into/out of an original test block; edit/remove a selected node without nearby collateral changes. |
| RES-13 | Journals, generated mappins and quest-phase extensions | Load an original journal/phase addition and verify map display and progression. |
| RES-14 | Voice, subtitles, lipsync and audio metadata extension | Play an original test line with matching subtitle and declared audio metadata. |
| RES-15 | Resource-type patch coverage registry | Keep separate fixtures for ent/app/mesh/material/morphtarget/curves/inkanim/devices/psrep and additional declared types. |
| RES-16 | Safe resource hot reload and cache invalidation | Reload a declared safe resource; require a session restart for a resource without verified reload semantics. |

## Independent game-script compiler and tooling

Target: Compiler / Game VM. Earliest milestone: 6. Reference interfaces: [source 1](https://www.nexusmods.com/cyberpunk2077/mods/1511), [source 2](https://github.com/jac3km4/redscript).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| VM-01 | Original typed source language and parser | Parse a published grammar corpus; malformed input receives precise source spans. |
| VM-02 | Type checker, overloads and inheritance | Accept valid overload/inheritance cases and reject ambiguous or incompatible calls. |
| VM-03 | Intermediate representation and game bytecode emitter | Run a compiled arithmetic/control-flow test inside the actual engine VM. |
| VM-04 | Original class, field and method additions | Add a class/field/method and access it from running game script code. |
| VM-05 | Method wrapping and replacement | Compose two wrappers in declared order and preserve arguments, result and original-call behavior. |
| VM-06 | Game-script blob read/write and symbol resolution | Round-trip an untouched local blob in scratch and prove engine acceptance; never publish the game blob. |
| VM-07 | Native imports and annotations | Bind an original native function with validated types and reject a missing signature. |
| VM-08 | Isolated cache, incremental builds and source maps | A one-file change rebuilds deterministically into the NEONFORGE cache; report the correct source line on error. |
| VM-09 | Compilation integration and fail-closed startup | Load the generated cache only after a successful compile; report an error without replacing the original. |
| VM-10 | Disassembly, decompilation and debugging helpers | Inspect only user-owned test scripts; preserve control flow/types to the documented fidelity. |
| VM-11 | Editor completion, diagnostics and project bindings | An example project receives completion and a real type diagnostic through the supplied editor tooling. |

## World, entity and player services

Target: World / Entities / Player. Earliest milestone: 7. Reference interfaces: [source 1](https://github.com/psiberx/cp2077-codeware/wiki).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| WORLD-01 | Session, save, pause and entity lifecycle events | Verify ordering and reference lifetimes across two sessions and a save/load cycle. |
| WORLD-02 | Target-filtered, once-only and lifetime-bound callbacks | Test ID/type/path/record/tag filters and automatic session cleanup. |
| WORLD-03 | Custom inter-mod events | Dispatch an original typed event between two mods with documented ownership. |
| WORLD-04 | Persistent script services outside saves | Restart the game and restore only versioned service data; migrate an older test schema. |
| WORLD-05 | Dynamic NPC/vehicle/entity creation and disposal | Spawn each supported category; despawn while streaming and prove no stale handles. |
| WORLD-06 | Static entities and asynchronous creation | Create/remove many test props; account for pending/cancelled creation jobs. |
| WORLD-07 | Weather, world nodes and world variants | Change a test weather/variant and restore prior state across session end. |
| WORLD-08 | Communities and population spawners | Control a specific test population source and verify unrelated sources remain active. |
| WORLD-09 | Mappins and bounded spatial queries | Query known objects by shape/range and validate edge/boundary cases in game. |
| WORLD-10 | Entity templates, components and transforms | Read and change a declared safe component/transform with explicit lifetime checks. |
| WORLD-11 | Mesh appearance and morph controls | Apply and restore a test mesh appearance and morph without persisting accidental edits. |
| WORLD-12 | Player objects, wardrobe and vehicle unlock services | Perform each supported action on a disposable profile and verify persistence rules. |
| WORLD-13 | Reflection type/property/function inspection | Inspect inherited fields and callable signatures against observed game behavior. |
| WORLD-14 | Reflection arrays, variants and callbacks | Round-trip nested values and release callback bridges without a leak. |
| WORLD-15 | Resource-load interception and post-processing | Modify only the targeted test resource at the documented phase and reject a wrong type. |
| WORLD-16 | Names, hashes, IDs, bit operations, vectors and time | Run deterministic known-vector tests and validate conversions through the engine bridge. |

## Game UI, settings and localization

Target: UI / Input / Localization. Earliest milestone: 7. Reference interfaces: [source 1](https://github.com/psiberx/cp2077-codeware/wiki), [source 2](https://wiki.redmodding.org/cyber-engine-tweaks/cet-functions).

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| UI-01 | UI layers, windows and resource widget spawning | Open/close an original window and asynchronous library widget through menu/session transitions. |
| UI-02 | Original scripted widgets, lists and popups | Exercise a virtualized list, popup and custom controller with correct event propagation. |
| UI-03 | Multilingual text entry and clipboard | Enter, copy and paste multilingual text; confirm focus and selection behavior. |
| UI-04 | Settings schemas, categories, defaults and validation | Render an original mod settings page, reject invalid input and restore defaults. |
| UI-05 | Localization providers and translation packages | Load a translation package and resolve an override/fallback with provenance. |
| UI-06 | UI scaling, font, focus and controller navigation | Use settings and dialogs at 1080p/4K and with a controller; no off-screen required controls. |
| UI-07 | Input action routing and conflict management | Resolve two mods using one binding and respect game/menu capture boundaries. |

## One-package experience and modder SDK

Target: Package / SDK / Diagnostics. Earliest milestone: 1. Original product requirements extending the six reference areas.

| ID | Planned capability | Required evidence |
| --- | --- | --- |
| TOOLS-01 | Single distributable with explicit module capabilities | Install one labelled candidate package and enumerate exactly which tested modules it contains. |
| TOOLS-02 | Manifest schema, API ranges and capability requests | Validate a package and explain every incompatible dependency before loading any executable code. |
| TOOLS-03 | Deterministic load graph and startup explanation | Produce the same order and explanation for the same package set across repeated runs. |
| TOOLS-04 | Owned-file installation, update and uninstall | Update/uninstall only files listed in the package receipt; preserve modified user settings and other mods. |
| TOOLS-05 | Profiles, dry runs and conflict previews | Preview conflicts in the current install; exercise bootstrap ownership in an isolated profile. |
| TOOLS-06 | Original native/Lua/data/compiler example mods | Build and run one source-available example for each supported authoring route. |
| TOOLS-07 | Project generator, API reference and migration guide | Generate a project and implement a small migrated mod using only the shipped documentation. |
| TOOLS-08 | Per-mod settings and versioned data migration | Migrate sample settings and recover from an interrupted write. |
| TOOLS-09 | Support report with build IDs and reproducible evidence | Export versions, module state and opted-in diagnostics without saves or personal data by default. |
| TOOLS-10 | Performance profiling and per-mod budgets | Measure frame-time overhead and callback cost against the same baseline scene. |
| TOOLS-11 | Stable source releases, dependency notices and artifact hashes | Rebuild the tagged source with pinned dependencies and verify the exact packaged artifacts. |
| TOOLS-12 | Negative, shutdown and longevity test harnesses | Inject supported failures and run repeated sessions/reloads; retain results by game build and capability. |

## Evidence states

- Proposed: design only.
- Implemented: production path exists; a placeholder or mock is not sufficient.
- Offline-tested: named automated checks passed against that exact artifact.
- Game-tested: exact scenario passed on the recorded executable/build and profile.
- Release-qualified: negative cases, lifecycle, conflicts and packaging checks pass for the declared support matrix.

The full request is satisfied only after every agreed capability is implemented and its required evidence is recorded. Milestone 1 alone is not a substitute for the full core.
