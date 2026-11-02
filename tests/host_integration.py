"""Exercise the shipped executables and actual plugin DLLs, not mocked loaders."""
import json, subprocess, sys, tempfile, shutil
from pathlib import Path
build=Path(sys.argv[1]).resolve()
host=build/'neonforge.exe'
assert host.exists(), 'native plugin host is not implemented'
def run(*args):
    return subprocess.run([str(host),*map(str,args)],capture_output=True,text=True,encoding='utf-8',timeout=30)
with tempfile.TemporaryDirectory(prefix='nf-integration-',dir=build) as scratch:
    root=Path(scratch);mods=root/'mods';mods.mkdir();data=root/'data'
    def package(name,version='0.1.0',requires=None,caps=None,dll='nf_example.dll'):
        directory=mods/name;directory.mkdir()
        shutil.copy2(build/dll,directory/'plugin.dll')
        manifest={'id':'org.neonforge.'+name,'name':name,'version':version,'api':1,'entry':'plugin.dll','capabilities':caps or []}
        if requires:manifest['requires']=requires
        (directory/'manifest.json').write_text(json.dumps(manifest),encoding='utf-8')
        return directory
    provider=package('example')
    consumer=package('consumer',requires=[{'id':'org.neonforge.example','min':'0.1.0','max_exclusive':'1.0.0'}],dll='nf_consumer.dll')
    result=run('validate',mods)
    assert result.returncode==0,result.stderr
    assert json.loads(result.stdout)['load_order']==['org.neonforge.example','org.neonforge.consumer']
    result=run('run',mods,data,'--ticks','3')
    assert result.returncode==0,result.stderr+result.stdout
    report=json.loads(result.stdout)
    assert report['started']==2 and report['remaining_contexts']==0 and report['remaining_tasks']==0,report
    assert json.loads((data/'settings/org.neonforge.example/counter.json').read_text())['boots']==1
    assert json.loads((data/'settings/org.neonforge.consumer/result.json').read_text())['answer']==42
    result=run('run',mods,data,'--ticks','1')
    assert result.returncode==0,result.stderr
    assert json.loads((data/'settings/org.neonforge.example/counter.json').read_text())['boots']==2
    consumer_manifest=json.loads((consumer/'manifest.json').read_text())
    consumer_manifest['version']='0.2.0'
    (consumer/'manifest.json').write_text(json.dumps(consumer_manifest))
    mismatch_data=root/'mismatch-data'
    result=run('run',mods,mismatch_data)
    assert result.returncode!=0,result.stdout
    assert json.loads(result.stdout)['started']==0,result.stdout
    assert not (mismatch_data/'settings/org.neonforge.example/counter.json').exists()
    consumer_manifest['version']='0.1.0'
    (consumer/'manifest.json').write_text(json.dumps(consumer_manifest))
    manifest=json.loads((provider/'manifest.json').read_text())
    manifest['capabilities']=[{'name':'nf.game.world','version':1,'required':True}]
    (provider/'manifest.json').write_text(json.dumps(manifest))
    rejected=root/'must-not-exist'
    result=run('run',mods,rejected)
    assert result.returncode!=0 and not rejected.exists(),result.stdout
    assert 'nf.game.world' in result.stderr
    manifest['capabilities']=[]
    (provider/'manifest.json').write_text(json.dumps(manifest))
    shutil.copy2(build/'nf_failing.dll',provider/'plugin.dll')
    result=run('run',mods,data)
    assert result.returncode!=0,result.stdout
    failure=json.loads(result.stdout)
    assert failure['remaining_contexts']==0 and failure['remaining_tasks']==0 and failure['remaining_subscriptions']==0,failure
    print('PASS native DLL order, services, callbacks, persistence, preflight and failed-start cleanup')
