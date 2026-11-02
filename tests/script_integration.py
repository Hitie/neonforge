"""Run original Lua packages through the real standalone host."""
import json
from pathlib import Path
import subprocess
import shutil
import sys
import tempfile

build = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='nf-script-', dir=build) as temp:
    root = Path(temp)
    mod = root / 'mods/org.neonforge.lua.example'
    mod.mkdir(parents=True)
    (mod / 'manifest.json').write_text(json.dumps(dict(id='org.neonforge.lua.example', name='Lua example', version='0.1.0', api=1, entry='init.lua', capabilities=[])))
    (mod / 'init.lua').write_text('nf.settings_write("answer", "{\\"answer\\":42}")\n')
    run = subprocess.run([str(build / 'neonforge.exe'), 'run', str(root / 'mods'), str(root / 'data')], capture_output=True, text=True, timeout=20)
    assert run.returncode == 0, 'Lua entry is not usable: ' + run.stdout + run.stderr
    assert json.loads(run.stdout)['started'] == 1
    assert json.loads((root / 'data/settings/org.neonforge.lua.example/answer.json').read_text()) == {'answer': 42}
    (mod / 'init.lua').write_text("nf.on('nf.host.ready',function() error('attributed callback failure') end)")
    run = subprocess.run([str(build / 'neonforge.exe'), 'run', str(root / 'mods'), str(root / 'failed-data')], capture_output=True, text=True, timeout=20)
    assert run.returncode != 0 and 'org.neonforge.lua.example' in run.stderr, run.stdout + run.stderr
    assert json.loads(run.stdout)['remaining_contexts'] == 0
    native = root / 'mods/org.neonforge.example'
    native.mkdir()
    shutil.copy2(build / 'nf_example.dll', native / 'plugin.dll')
    (native / 'manifest.json').write_text(json.dumps(dict(id='org.neonforge.example',name='Native example',version='0.1.0',api=1,entry='plugin.dll',capabilities=[])))
    (mod / 'init.lua').write_text('this is a syntax error')
    untouched = root / 'syntax-data'
    run = subprocess.run([str(build / 'neonforge.exe'), 'run', str(root / 'mods'), str(untouched)], capture_output=True, text=True, timeout=20)
    assert run.returncode != 0 and json.loads(run.stdout)['started'] == 0
    assert not (untouched / 'settings/org.neonforge.example/counter.json').exists(), 'native start ran before Lua syntax validation'
    (mod / 'init.lua').write_text("nf.on('nf.host.ready',function() nf.settings_write('mixed','{\"working\":true}') end)")
    run = subprocess.run([str(build / 'neonforge.exe'), 'run', str(root / 'mods'), str(root / 'mixed-data')], capture_output=True, text=True, timeout=20)
    assert run.returncode == 0 and json.loads(run.stdout)['started'] == 2, run.stdout + run.stderr
    assert json.loads((root / 'mixed-data/settings/org.neonforge.lua.example/mixed.json').read_text())['working'] is True
    for index, code in enumerate(["error(string.char(255))", "error('before'..string.char(0)..'after')", "error(string.rep('é',9000))"]):
        (mod / 'init.lua').write_text(code,encoding='utf-8')
        run = subprocess.run([str(build / 'neonforge.exe'),'run',str(root / 'mods'),str(root / ('error-data'+str(index)))],capture_output=True,text=True,encoding='utf-8',timeout=20)
        assert run.returncode != 0, run.stdout
        report = json.loads(run.stdout)
        assert report['remaining_contexts'] == 0 and 'org.neonforge.lua.example' in report['failure']
        assert len(report['failure'].encode()) <= 16384
        if index == 1: assert 'after' in report['failure']
print('PASS Lua package runs in the real standalone host')
