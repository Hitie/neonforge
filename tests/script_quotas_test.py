"""Bound hangs inside non-yieldable library callbacks in child processes."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
cases = [
    "table.sort({2,1}, function() while true do pcall(function() while true do end end) end end)",
    "string.gsub('a','a',function() while true do pcall(function() while true do end end) end end)",
    "xpcall(function() table.sort({2,1},function() while true do end end) end,function() while true do end end)",
    "xpcall(function() error('ordinary') end,function() while true do pcall(function() while true do end end) end end)",
]
with tempfile.TemporaryDirectory(prefix='nf-quota-', dir=build) as temp:
    root = Path(temp)
    mod = root / 'mods/quota.mod'
    mod.mkdir(parents=True)
    (mod / 'manifest.json').write_text(json.dumps(dict(id='quota.mod',name='Quota fixture',version='0.1.0',api=1,entry='init.lua',capabilities=[])))
    for index, code in enumerate(cases):
        (mod / 'init.lua').write_text(code)
        try:
            run = subprocess.run([str(build / 'neonforge.exe'),'run',str(root / 'mods'),str(root / ('data'+str(index)))],capture_output=True,text=True,timeout=3)
        except subprocess.TimeoutExpired:
            raise AssertionError('instruction quota did not terminate non-yieldable case '+str(index)) from None
        assert run.returncode != 0 and 'instruction' in run.stderr, run.stdout+run.stderr
        assert json.loads(run.stdout)['remaining_contexts'] == 0
print('PASS instruction limits in sort, substitution and protected error handlers')
