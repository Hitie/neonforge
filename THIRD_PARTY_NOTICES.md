# Third-party notices

NEONFORGE's original implementation remains under the repository's existing GPL version 3 license. The standalone host also embeds Lua, independently licensed under MIT. This dependency is infrastructure, not code from the six reference mod frameworks.

## Lua 5.5.1

Copyright (C) 1994-2026 Lua.org, PUC-Rio.

Official source: https://www.lua.org/ftp/lua-5.5.1.tar.gz

Archive SHA-256: `1c4b4068d67061f2a2231ad2b5422e77acea1487ea9890f6320af614f4373dce`

The complete permission and warranty notice is retained in [third_party/lua/LICENSE.txt](third_party/lua/LICENSE.txt) and upstream headers. Source files are unmodified and listed with their original hashes in `third_party/lua/UPSTREAM.json`. NEONFORGE compiles Lua as C++ for exception unwinding and opens only selected libraries through its own adapter. The source archive includes Lua source; the binary SDK includes its license and this notice.

Lua documentation: https://www.lua.org/manual/5.5/manual.html
