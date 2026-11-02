# Security policy

## Supported builds

Only the latest development source on `main` is maintained at this stage. No production release or game integration is qualified. Older development archives may contain issues fixed on main.

## Report a vulnerability

Use [GitHub private vulnerability reporting](https://github.com/Hitie/neonforge/security/advisories/new) when enabled. If unavailable, open a minimal issue asking the maintainer to enable a private reporting channel; do not post exploit details, tokens or private data publicly. There is no guaranteed response deadline or bounty program.

Include the affected commit, reproduction steps, expected impact and a small original test case. Redact usernames, local paths, game data and credentials. Do not test against other people's systems or mods without authorization.

## Trust boundaries

Native plugins execute with the host user's permissions. Capabilities and API ownership checks are coordination mechanisms, not a native-code sandbox. Install only plugins you trust. In-process access violations cannot be reliably recovered from.

The installer rejects unsafe paths, links and conflicting ownership and rolls back ordinary handled I/O failures. It does not promise protection against an attacker racing filesystem changes or complete recovery after abrupt process termination. See the tool documentation before using development packages.

CI builds use GitHub-hosted runners, read-only checkout credentials, pinned actions and no game/account secrets. CodeQL findings and passing tests are evidence within their coverage, not a guarantee that software is vulnerability-free. Release downloads must be checked against their published SHA-256 hashes.
