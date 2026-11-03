# Original package and developer tools

`tools/forge.py` uses Python's standard library. The runtime itself does not use Python. Packages may declare native DLL or Lua entries; the project generator currently produces native starter projects. Validating a manifest does not guarantee its executable code is safe or implements its claims.

## Full read-only inventory

```powershell
python tools/game_audit.py "C:/path/to/Cyberpunk 2077" --output "C:/path/outside-game/game-inventory.json"
```

This separate tool enumerates regular files, computes a full SHA-256 for each, records bounded header signatures and reports known loader/mod-directory markers. Linked/reparse paths are refused rather than followed. Traversal/read errors and detected file changes make the report incomplete. Report output must be outside the game directory. No file from the game is modified or executed.

An error-free inventory establishes a local comparison baseline, not publisher/depot authenticity, absence of every possible mod, or game compatibility. Filesystem changes during traversal are not atomically snapshotted; close game/update/package processes for a useful baseline. Link checks are not a sandbox against a hostile concurrent local process. Keep per-file inventories local unless deliberately sharing diagnostics; no game contents or user-specific inventory are included in repository exports.

```powershell
python tools/forge.py new work/MyMod --id com.example.mymod --name "My Mod"
python tools/forge.py inspect MyMod.zip
python tools/forge.py install MyMod.zip --root C:/Developer/ScratchProfile --dry-run
python tools/forge.py install MyMod.zip --root C:/Developer/ScratchProfile
python tools/forge.py uninstall com.example.mymod --root C:/Developer/ScratchProfile --dry-run
```

Generate C source and build it with the shipped header. Set `NEONFORGE_SDK` to the SDK root when configuring the generated CMake project. Put the built `plugin.dll` beside `manifest.json`, then validate its containing mods directory with `neonforge validate` before loading.

Packages contain a root `manifest.json`, its declared relative DLL or Lua entry and other owned files. Use a ZIP with relative forward-slash paths. Links, encrypted entries, path traversal, Windows device names, streams and case-colliding names are refused. Limits are 10,000 entries, 256 MiB per file and 512 MiB total uncompressed data. Schema validation checks nested dependency/capability types, versions and fields before file mutation.

The manager confines packages to `<root>/neonforge/mods/<mod-id>` and receipts to `<root>/neonforge/receipts`. Updates reject modified or unowned replacement targets. Uninstall removes only matching owned hashes; modified and untracked files are preserved. Review the dry-run result before using the developer-preview installer on a valuable profile.

## Rollback boundary

Normal Python/I/O failures during replacement trigger rollback; tests inject a replacement failure and verify prior files and receipt. This is not a power-loss/process-kill guarantee. A killed process may leave `.manager.lock` and `.transactions/nf-*/journal.json` plus before-images. There is no automatic recovery command in this build. Preserve those files and inspect the journal/before-images before manual recovery; do not blindly delete them or rerun an update over a partially changed installation. Do not run package operations concurrently with native modules using those files. Reparse checks are not a claim of adversarial race-free confinement.

## Settings migration

```powershell
python tools/forge.py migrate-settings data/settings/com.example.mymod/preferences.json --from-version 1 --to-version 2 --renames '{"old_name":"new_name"}' --defaults '{"enabled":true}'
```

Run with the relevant host/game closed. The tool checks `schema_version`, rejects destination collisions and preserves unknown user fields. It writes through a temporary file and replaces only after successful validation. It does not coordinate a live settings writer.

## Exact-build doctor

```powershell
python tools/forge.py doctor "C:/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077"
```

The included baseline identifies the scanned Steam 2.5 build. The doctor hashes the executable/address file, parses PE sections, compares timestamps and counts address collisions. It reads local files and prints metadata; it does not upload game data, alter files or enable hooks. A matching baseline deliberately still reports `engine_adapter_ready: false` and `game_tested: false`.
