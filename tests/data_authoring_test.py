"""Public CLI workflows exercise the same native authoring/session engine."""
from pathlib import Path
import copy
import json
import subprocess
import sys
import tempfile
import msvcrt
import os

binary = Path(sys.argv[1]).resolve() / 'nf_data.exe'
with tempfile.TemporaryDirectory(prefix='nf-authoring-', dir=binary.parent) as temp:
    root = Path(temp)
    project = root / 'project.json'
    document = {'format': 1, 'schemas': {
        'Base': {'fields': {'id': {'type': 'u64', 'required': True}}},
        'Item': {'extends': 'Base', 'fields': {'title': {'type': 'string'}}}},
        'templates': {'Original': {'type': 'Item', 'fields': {
            'id': {'type': 'u64', 'value': '18446744073709551615'}}}},
        'transactions': [{'owner': 'example.author', 'operations': [
            {'op': 'instantiate', 'record': 'Original', 'template': 'Original'}]}]}
    project.write_text(json.dumps(document), encoding='utf-8')
    original = project.read_bytes()

    def run(command, *paths):
        return subprocess.run([str(binary), command, *map(str, paths)],
                              capture_output=True, text=True, encoding='utf-8', timeout=20)

    exported = run('checkpoint', project)
    assert exported.returncode == 0, exported.stderr
    checkpoint = root / 'checkpoint.json'
    checkpoint.write_text(exported.stdout, encoding='utf-8')
    saved = json.loads(exported.stdout)
    assert saved['kind'] == 'neonforge.offline-data-checkpoint'
    restored = run('restore', checkpoint)
    assert restored.returncode == 0, restored.stderr
    assert json.loads(restored.stdout) == saved['state']
    same = run('reload', checkpoint, project)
    assert same.returncode == 0, same.stderr
    assert json.loads(same.stdout)['accepted'] and not json.loads(same.stdout)['changed']

    project.write_text('{', encoding='utf-8')
    failed = run('reload', checkpoint, project)
    assert failed.returncode != 0
    rejection = json.loads(failed.stdout)
    assert not rejection['accepted'] and not rejection['changed']
    assert 'byte' in rejection['error'] and rejection['state'] == saved['state']

    document['transactions'][0]['operations'][0]['fields'] = {
        'title': {'type': 'string', 'value': 'reloaded'}}
    project.write_text(json.dumps(document), encoding='utf-8')
    changed = run('reload', checkpoint, project)
    assert changed.returncode == 0, changed.stderr
    assert json.loads(changed.stdout)['changed']
    assert json.loads(changed.stdout)['state']['records']['Original']['fields']['title']['value'] == 'reloaded'

    project.write_text(json.dumps({'\u263a' * 1800: True}), encoding='utf-8')
    unicode_failure=run('reload', checkpoint, project)
    assert unicode_failure.returncode != 0 and unicode_failure.stdout, unicode_failure.stderr
    unicode_result=json.loads(unicode_failure.stdout)
    assert not unicode_result['accepted'] and unicode_result['state'] == saved['state']
    assert len(unicode_result['error'].encode('utf-8')) <= 4096

    # Malformed persisted data must never emit an apparent restored snapshot.
    for mutation in ['unknown', 'wrong-type', 'missing-history', 'flags', 'revision']:
        bad = copy.deepcopy(saved)
        if mutation == 'unknown': bad['unexpected'] = True
        if mutation == 'wrong-type': bad['state']['records']['Original']['fields']['id'] = {'type': 'i64', 'value': '1'}
        if mutation == 'missing-history': bad['state']['records']['Original']['provenance'] = {}
        if mutation == 'flags': bad['state']['game_tested'] = True
        if mutation == 'revision': bad['state']['revision'] = '01'
        checkpoint.write_text(json.dumps(bad), encoding='utf-8')
        rejected = run('restore', checkpoint)
        assert rejected.returncode != 0 and not rejected.stdout, mutation
    assert original != project.read_bytes()
    assert sorted(path.name for path in root.iterdir()) == ['checkpoint.json', 'project.json']

    large = {'format': 1, 'schemas': {'Text': {'fields': {'text': {'type': 'string'}}}},
             'transactions': [{'owner': 'example.large', 'operations': [
                 {'op': 'create', 'record': 'Base', 'type': 'Text'},
                 {'op': 'set', 'record': 'Base', 'field': 'text',
                  'value': {'type': 'string', 'value': 'x' * 40000}}] + [
                 {'op': 'clone', 'record': f'Copy{i}', 'from': 'Base'} for i in range(32)]}]}
    project.write_text(json.dumps(large), encoding='utf-8')
    exported_large = run('checkpoint', project)
    assert exported_large.returncode == 0, exported_large.stderr
    assert len(exported_large.stdout.encode('utf-8')) > 1048576
    checkpoint.write_text(exported_large.stdout, encoding='utf-8')
    restored_large = run('restore', checkpoint)
    assert restored_large.returncode == 0, restored_large.stderr
    assert json.loads(restored_large.stdout) == json.loads(exported_large.stdout)['state']
    streamed = subprocess.run([str(binary), 'canonicalize', '-'], input=exported_large.stdout,
                              capture_output=True, text=True, encoding='utf-8', timeout=20)
    assert streamed.returncode == 0, streamed.stderr
    assert json.loads(streamed.stdout) == json.loads(exported_large.stdout)
    streamed_project = subprocess.run([str(binary), 'checkpoint', '-'], input=original.decode('utf-8'),
                                     capture_output=True, text=True, encoding='utf-8', timeout=20)
    assert streamed_project.returncode == 0, streamed_project.stderr
    assert json.loads(streamed_project.stdout) == saved
    # A detached GUI launcher may supply stdout/stderr but no unused stdin.
    output_path=root/'detached-output';error_path=root/'detached-error'
    with output_path.open('wb') as out,error_path.open('wb') as err:
        info=subprocess.STARTUPINFO()
        info.dwFlags|=subprocess.STARTF_USESTDHANDLES
        info.hStdInput=0
        info.hStdOutput=msvcrt.get_osfhandle(out.fileno());info.hStdError=msvcrt.get_osfhandle(err.fileno())
        handles=[info.hStdOutput,info.hStdError]
        for handle in handles:os.set_handle_inheritable(handle,True)
        info.lpAttributeList={'handle_list':handles}
        detached=subprocess.run([str(binary),'validate',str(project)],startupinfo=info,close_fds=True,timeout=20)
    assert detached.returncode==0,error_path.read_bytes()
print('PASS inherited templates, exact checkpoint round trip, rejected imports and last-valid reload')
