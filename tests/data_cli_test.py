"""Exercise original declarative data through the real SDK executable."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile

build=Path(sys.argv[1]).resolve()
binary=build/'nf_data.exe'
assert binary.exists(), 'offline data engine executable is not implemented'
with tempfile.TemporaryDirectory(prefix='nf-data-',dir=build) as temp:
    root=Path(temp);project=root/'project.json'
    document={'format':1,'schemas':{'Item':{'fields':{'damage':{'type':'u64','required':True},'tags':{'type':'array','element':{'type':'name'}}}}},'transactions':[{'owner':'example.provider','source':'original fixture','operations':[{'op':'create','record':'Items.Original','type':'Item'},{'op':'set','record':'Items.Original','field':'damage','value':{'type':'u64','value':'18446744073709551615'}},{'op':'set','record':'Items.Original','field':'tags','value':{'type':'array','element':{'type':'name'},'value':[]}}]},{'owner':'example.consumer','operations':[{'op':'clone','record':'Items.Clone','from':'Items.Original'},{'op':'append','record':'Items.Clone','field':'tags','value':{'type':'name','value':'custom'},'unique':True}]}]}
    project.write_text(json.dumps(document),encoding='utf-8')
    run=subprocess.run([str(binary),'apply',str(project)],capture_output=True,text=True,timeout=20)
    assert run.returncode==0,run.stdout+run.stderr
    result=json.loads(run.stdout)
    assert result['revision']=='2'
    assert result['records']['Items.Clone']['fields']['damage']['value']=='18446744073709551615'
    assert result['records']['Items.Clone']['fields']['tags']['value']==[{'type':'name','value':'custom'}]
    assert result['records']['Items.Original']['fields']['tags']['value']==[]
    before=project.read_bytes()
    first=subprocess.run([str(binary),'plan',str(project)],capture_output=True,text=True,timeout=20)
    second=subprocess.run([str(binary),'plan',str(project)],capture_output=True,text=True,timeout=20)
    assert first.returncode==second.returncode==0 and first.stdout==second.stdout
    assert project.read_bytes()==before
    document['transactions'][1]['operations'].append({'op':'set','record':'Items.Clone','field':'damage','value':{'type':'i64','value':'4'}})
    project.write_text(json.dumps(document))
    bad=subprocess.run([str(binary),'apply',str(project)],capture_output=True,text=True,timeout=20)
    assert bad.returncode!=0 and 'damage' in bad.stderr and not bad.stdout
    project.write_text('{"format": 1,')
    invalid=subprocess.run([str(binary),'validate',str(project)],capture_output=True,text=True,timeout=20)
    assert invalid.returncode!=0 and 'byte' in invalid.stderr and not invalid.stdout
    exact='{"format":1,"schemas":{"Item":{"fields":{"tint":{"type":"color"}}}},"transactions":[{"owner":"example.color","operations":[{"op":"create","record":"Items.Color","type":"Item"},{"op":"set","record":"Items.Color","field":"tint","value":{"type":"color","value":[255.00000000000000001,0,0,255]}}]}]}'
    for content in [exact,exact.replace('255.00000000000000001','255').replace('"format":1','"format":1.00000000000000001')]:
        project.write_text(content)
        rejected=subprocess.run([str(binary),'apply',str(project)],capture_output=True,text=True,timeout=20)
        assert rejected.returncode!=0 and not rejected.stdout,'rounded decimal accepted: '+rejected.stdout
    project.write_text(exact.replace('255.00000000000000001','2.55e2').replace('"format":1','"format":1.0'))
    accepted=subprocess.run([str(binary),'apply',str(project)],capture_output=True,text=True,timeout=20)
    assert accepted.returncode==0,accepted.stderr
    wrong=json.loads(project.read_text());wrong['transactions'][0]['operations'][1]['value']['value']='wrong kind'
    project.write_text(json.dumps(wrong))
    contextual=subprocess.run([str(binary),'apply',str(project)],capture_output=True,text=True,timeout=20)
    assert contextual.returncode!=0 and 'Items.Color' in contextual.stderr and 'tint' in contextual.stderr and 'bad variant' not in contextual.stderr
    original=Path(__file__).resolve().parents[1]/'examples/data/original-items.json'
    closed=subprocess.Popen([str(binary),'apply',str(original)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
    closed.stdout.close();errors=closed.stderr.read();status=closed.wait(timeout=20)
    assert status!=0,'closed output pipe was reported as success'
print('PASS original data transactions, exact integers, clone isolation and deterministic planning')
