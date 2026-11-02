local math_helpers = require('helpers')
nf.export('demo.lua.add', 'i64', {'i64', 'i64'}, math_helpers.add)
nf.export('demo.lua.echo', 'u64', {'u64'}, function(value) return value end)
