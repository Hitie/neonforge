"""Real native validation, Windows file guards, process-kill recovery and watch."""
from pathlib import Path
import copy
import importlib
import json
import os
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch
import shutil

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]
assert (ROOT / 'tools/data_store.py').is_file(), 'durable store is not implemented'
sys.path.insert(0, str(ROOT / 'tools'))
store = importlib.import_module('data_store')
files = importlib.import_module('windows_files')


def project(value='1'):
    return json.dumps({'format': 1, 'schemas': {'A': {'fields': {'id': {'type': 'u64', 'required': True}}}},
                       'transactions': [{'owner': 'example.store', 'operations': [
                           {'op': 'create', 'record': 'A', 'type': 'A'},
                           {'op': 'set', 'record': 'A', 'field': 'id',
                            'value': {'type': 'u64', 'value': value}}]}]}).encode()


class DurableTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nf-store-', dir=BUILD)
        self.root = Path(self.temp.name)
        self.validator = store.Validator(BUILD / 'nf_data.exe')
        self.directory = self.root / 'store'
        self.store = store.Store(self.directory, self.validator)
        self.one = self.validator.checkpoint(project())
        self.two = self.validator.checkpoint(project('18446744073709551615'))

    def tearDown(self):
        self.temp.cleanup()

    def test_roundtrip_noop_conflict_and_source_preservation(self):
        empty=self.store.load()
        self.assertEqual(empty['generation'], 0)
        first = self.store.save(self.one, expected=empty['token'])
        self.assertEqual(first['generation'], 1)
        self.assertEqual(self.store.save(self.one, expected=first['token'])['generation'], 1)
        before = {p.name: p.read_bytes() for p in self.directory.iterdir()}
        with self.assertRaises(store.StoreError): self.store.save(self.two, expected=empty['token'])
        self.assertEqual(before, {p.name: p.read_bytes() for p in self.directory.iterdir()})
        second=self.store.save(self.two, expected=first['token'])
        self.assertEqual(second['generation'], 2)
        self.assertEqual(self.store.load()['checkpoint'], self.two)
        sentinel = self.directory / 'unrelated.txt'; sentinel.write_bytes(b'preserve')
        self.store.save(self.one, expected=second['token'])
        self.assertEqual(sentinel.read_bytes(), b'preserve')

    def test_corruption_recovers_previous_and_never_promotes_orphan(self):
        self.store.save(self.one); second = self.store.save(self.two)
        head = self.directory / 'head.json'
        head.write_bytes(b'{')
        recovered = self.store.load()
        self.assertTrue(recovered['recovered'])
        self.assertEqual(recovered['generation'], 1)
        self.assertEqual(recovered['checkpoint'], self.one)
        (self.directory / 'previous.json').write_bytes(b'{}')
        with self.assertRaises(store.StoreError): self.store.load()
        self.assertEqual(second['generation'], 2)

    def test_invalid_checkpoint_and_unowned_directory_are_untouched(self):
        self.store.save(self.one)
        with self.assertRaises(store.StoreError): self.store.save(b'{}')
        self.assertEqual(self.store.load()['checkpoint'], self.one)
        foreign = self.root / 'foreign'; foreign.mkdir(); sentinel = foreign / 'head.json'
        sentinel.write_bytes(b'foreign')
        with self.assertRaises(store.StoreError): store.Store(foreign, self.validator).load()
        self.assertEqual(sentinel.read_bytes(), b'foreign')

    def test_lock_released_by_os_and_linked_managed_files_refused(self):
        self.store.load()
        with files.exclusive(self.directory / 'LOCK'):
            with self.assertRaises(store.StoreError): self.store.load()
        self.store.save(self.one)
        target = self.root / 'outside'; target.write_bytes(b'outside')
        managed = self.directory / 'previous.json'; managed.unlink()
        os.link(target, managed)
        with self.assertRaises(store.StoreError): self.store.save(self.two)
        self.assertEqual(target.read_bytes(), b'outside')

    def test_directory_junction_and_alternate_stream_refused(self):
        target = self.root / 'target'; target.mkdir()
        link = self.root / 'linked'
        result = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(target)], capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        with self.assertRaises(store.StoreError): store.Store(link / 'store', self.validator).load()
        with self.assertRaises(store.StoreError): store.Store(str(self.directory) + ':stream', self.validator).load()

    def test_process_kill_before_and_after_commit_recovers_old_or_new(self):
        self.store.save(self.one)
        payload = self.root / 'new.json'; payload.write_bytes(self.two)
        for phase in ['sequence:half', 'sequence:flushed', 'sequence:renamed', 'previous:half', 'previous:flushed', 'previous:renamed', 'slot:half', 'slot:flushed', 'slot:renamed', 'head:half', 'head:flushed', 'head:renamed']:
            with self.subTest(phase=phase):
                # Reset through the public protocol, never alter store metadata.
                self.store.save(self.one)
                barrier = self.root / 'ready'; barrier.unlink(missing_ok=True)
                child_code = "import sys,time;from pathlib import Path;sys.path.insert(0,sys.argv[1]);from data_store import Store,Validator\ndef fault(stage):\n if stage==sys.argv[5]:\n  Path(sys.argv[6]).write_text('ready');time.sleep(60)\nStore(sys.argv[2],Validator(sys.argv[3]),fault=fault).save(Path(sys.argv[4]).read_bytes())"
                child = subprocess.Popen([sys.executable, '-c', child_code, str(ROOT / 'tools'), str(self.directory), str(BUILD / 'nf_data.exe'), str(payload), phase, str(barrier)], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                try:
                    deadline = time.monotonic() + 15
                    while not barrier.exists() and child.poll() is None and time.monotonic() < deadline: time.sleep(.02)
                    self.assertTrue(barrier.exists(), child.communicate(timeout=1) if child.poll() is not None else phase)
                    child.kill(); child.communicate(timeout=10)
                finally:
                    if child.poll() is None: child.kill(); child.communicate(timeout=10)
                recovered = self.store.load()
                self.assertEqual(recovered['checkpoint'], self.two if phase == 'head:renamed' else self.one)
                self.assertFalse((self.directory / '.pending-write').exists())

    def test_watcher_settles_content_and_retains_last_valid(self):
        source = self.root / 'project.json'; source.write_bytes(project())
        watch = store.Watcher(source, self.store, self.validator, settle=.1)
        self.assertIsNone(watch.poll(0)); event = watch.poll(.2)
        self.assertEqual(event['status'], 'committed'); self.assertEqual(watch.generation, 1)
        old_time = source.stat().st_mtime_ns
        source.write_bytes(project('2')); os.utime(source, ns=(old_time, old_time))
        self.assertIsNone(watch.poll(.3)); self.assertEqual(watch.poll(.5)['status'], 'committed')
        valid = watch.checkpoint
        source.write_bytes(b'{'); self.assertIsNone(watch.poll(.6))
        self.assertEqual(watch.poll(.8)['status'], 'error'); self.assertIsNone(watch.poll(1.0))
        self.assertEqual(watch.checkpoint, valid)
        source.unlink(); self.assertEqual(watch.poll(1.1)['status'], 'error')
        source.write_bytes(project('3')); self.assertIsNone(watch.poll(1.2))
        self.assertEqual(watch.poll(1.4)['status'], 'committed')

    def test_watcher_detects_competing_writer_and_retries_io(self):
        source = self.root / 'project.json'; source.write_bytes(project())
        watch = store.Watcher(source, self.store, self.validator, settle=.1)
        watch.poll(0); watch.poll(.2)
        source.write_bytes(project('2')); watch.poll(.3)
        with files.exclusive(self.directory / 'LOCK'):
            self.assertEqual(watch.poll(.5)['status'], 'error')
        self.assertEqual(watch.poll(2)['status'], 'committed')
        source.write_bytes(project('3')); watch.poll(3)
        self.store.save(self.two)
        event = watch.poll(3.2)
        self.assertEqual(event['status'], 'conflict')
        self.assertIsNone(watch.poll(5))
        self.assertNotEqual(watch.checkpoint, self.two)

    def test_real_cli_watch_and_exact_reported_generations(self):
        tool = ROOT / 'tools/data_store.py'
        source = self.root / 'project.json'; source.write_bytes(project('18446744073709551615'))
        command = [sys.executable, str(tool), 'watch', str(source), str(self.directory), '--validator', str(BUILD / 'nf_data.exe'), '--poll-ms', '20', '--settle-ms', '10', '--iterations', '3']
        run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', timeout=20)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)['status'], 'committed')
        loaded = subprocess.run([sys.executable,str(tool),'load',str(self.directory),'--validator',str(BUILD/'nf_data.exe')], capture_output=True, timeout=20)
        self.assertEqual(loaded.returncode,0,loaded.stderr)
        self.assertEqual(json.loads(loaded.stdout)['state']['records']['A']['fields']['id']['value'],'18446744073709551615')
        saved = subprocess.run([sys.executable,str(tool),'save',str(source),str(self.directory),'--validator',str(BUILD/'nf_data.exe')],capture_output=True,text=True,encoding='utf-8',timeout=20)
        self.assertEqual(saved.returncode,0,saved.stderr)
        self.assertEqual(json.loads(saved.stdout)['generation'],'1')
    def test_cli_refuses_source_inside_owned_store(self):
        self.store.load()
        tool=ROOT/'tools/data_store.py'
        inside=self.directory/'project.json';inside.write_bytes(project())
        rejected=subprocess.run([sys.executable,str(tool),'save',str(inside),str(self.directory),'--validator',str(BUILD/'nf_data.exe')],capture_output=True,timeout=20)
        self.assertNotEqual(rejected.returncode,0)
        self.assertEqual(inside.read_bytes(),project())

    def test_strict_metadata_and_generation_exhaustion(self):
        self.store.save(self.one)
        head=self.directory/'head.json';previous=self.directory/'previous.json'
        original=head.read_bytes();definition=json.loads(original)
        for key,value in [('generation',True),('generation','01'),('generation','18446744073709551616'),('slot',False),('format',True),('unknown',1)]:
            bad=copy.deepcopy(definition);bad[key]=value
            head.write_bytes(json.dumps(bad).encode());previous.write_bytes(b'{}')
            with self.assertRaises(store.StoreError):self.store.load()
        definition['generation']='18446744073709551615';head.write_bytes(json.dumps(definition).encode())
        sequence=json.loads((self.directory/'sequence.json').read_bytes());sequence['generation']=definition['generation']
        (self.directory/'sequence.json').write_bytes(json.dumps(sequence).encode())
        self.assertEqual(self.store.save(self.one)['generation'],2**64-1)
        with self.assertRaises(store.StoreError):self.store.save(self.two)
        self.assertEqual(self.store.load()['checkpoint'],self.one)

    def test_interrupted_first_slot_is_not_a_commit(self):
        self.store.load()
        payload=self.root/'new.json';payload.write_bytes(self.one)
        barrier=self.root/'ready'
        code="import sys,time;from pathlib import Path;sys.path.insert(0,sys.argv[1]);from data_store import Store,Validator\ndef fault(stage):\n if stage=='slot:renamed':\n  Path(sys.argv[5]).write_text('ready');time.sleep(60)\nStore(sys.argv[2],Validator(sys.argv[3]),fault=fault).save(Path(sys.argv[4]).read_bytes())"
        child=subprocess.Popen([sys.executable,'-c',code,str(ROOT/'tools'),str(self.directory),str(BUILD/'nf_data.exe'),str(payload),str(barrier)],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
        try:
            deadline=time.monotonic()+15
            while not barrier.exists() and child.poll() is None and time.monotonic()<deadline:time.sleep(.02)
            self.assertTrue(barrier.exists())
        finally:
            if child.poll() is None:child.kill()
            child.communicate(timeout=10)
        recovered=self.store.load()
        self.assertEqual(recovered['generation'],0)
        self.assertIsNone(recovered['checkpoint'])
        self.assertGreater(self.store.save(self.one)['generation'],0)

    def test_locked_current_marker_never_falls_back_and_destroys_valid_commit(self):
        self.store.save(self.one);self.store.save(self.two)
        before={p.name:p.read_bytes() for p in self.directory.iterdir()}
        with files.exclusive(self.directory/'head.json'):
            with self.assertRaises(store.StoreError):self.store.save(self.validator.checkpoint(project('3')))
        self.assertEqual(self.store.load()['checkpoint'],self.two)
        self.assertEqual(before,{p.name:p.read_bytes() for p in self.directory.iterdir()})

    def test_recovery_does_not_reuse_a_watcher_commit_identity(self):
        self.store.save(self.one);self.store.save(self.two)
        source=self.root/'project.json';source.write_bytes(project('4'))
        watch=store.Watcher(source,self.store,self.validator,settle=.1);watch.poll(0)
        (self.directory/'head.json').write_bytes(b'{')
        recovered=self.store.save(self.validator.checkpoint(project('3')))
        event=watch.poll(.2)
        self.assertEqual(event['status'],'conflict')
        self.assertGreater(recovered['generation'],2)
        self.assertTrue(recovered['recovered'])

    def test_genuine_old_head_cannot_silently_reset_populated_store(self):
        self.store.load();empty=(self.directory/'head.json').read_bytes()
        self.store.save(self.one);self.store.save(self.two)
        (self.directory/'head.json').write_bytes(empty)
        recovered=self.store.load()
        self.assertEqual(recovered['generation'],1)
        self.assertEqual(recovered['checkpoint'],self.one)
        self.assertTrue(recovered['recovered'])

    def test_publication_keeps_verified_staging_handle_pinned(self):
        self.store.save(self.one)
        unrelated=self.root/'unrelated';unrelated.write_bytes(b'preserve outside file')
        original_move=files.move;blocked=[]
        def interleave(source,target,replace=True):
            path=getattr(source,'path',source)
            if Path(path).name=='.pending-write' and Path(target).name=='slot-1.json':
                try:os.replace(unrelated,path)
                except PermissionError:blocked.append(True)
                else:blocked.append(False)
            return original_move(source,target,replace)
        with patch.object(files,'move',interleave):self.store.save(self.two)
        self.assertEqual(blocked,[True])
        self.assertEqual(unrelated.read_bytes(),b'preserve outside file')
        self.assertEqual(self.store.load()['checkpoint'],self.two)

    def test_initialization_never_cleans_a_substituted_directory(self):
        unrelated=self.root/'unrelated';unrelated.mkdir()
        (unrelated/'owner.json').write_bytes(b'original owner')
        (unrelated/'head.json').write_bytes(b'original head')
        original_move=files.move;replacement=[]
        def interleave(source,target,replace=True):
            path=Path(getattr(source,'path',source))
            result=original_move(source,target,replace)
            if path.name.startswith('.nf-store-init-'):
                os.rename(unrelated,path);replacement.append(path)
            return result
        with patch.object(files,'move',interleave):self.store.load()
        self.assertEqual(len(replacement),1)
        self.assertEqual((replacement[0]/'owner.json').read_bytes(),b'original owner')
        self.assertEqual((replacement[0]/'head.json').read_bytes(),b'original head')

    def test_transient_native_launch_failure_retries_same_source(self):
        binary=self.root/'validator.exe';shutil.copy2(BUILD/'nf_data.exe',binary)
        validator=store.Validator(binary);target=store.Store(self.directory,validator)
        source=self.root/'project.json';source.write_bytes(project())
        watch=store.Watcher(source,target,validator,settle=.1)
        moved=binary.with_suffix('.disabled');binary.rename(moved)
        try:
            watch.poll(0);self.assertEqual(watch.poll(.2)['status'],'error')
        finally:moved.rename(binary)
        event=watch.poll(20)
        self.assertIsNotNone(event)
        self.assertEqual(event['status'],'committed')

    def test_ambiguous_raw_path_is_rejected_before_normalization(self):
        for value in ['store.','store ','a./store']:
            with self.subTest(value=value):
                with self.assertRaises(store.StoreError):files.absolute(self.root/value)

    def test_mapped_drive_class_and_volume_normalized_containment(self):
        with patch.object(files._win,'GetDriveTypeW',return_value=4):
            with self.assertRaises(store.StoreError):files.absolute(self.root/'store')
        self.store.load()
        alias_source=self.root/'SHORT_ALIAS'/'project.json'
        with patch.object(files,'canonical_directory',return_value='\\\\?\\volume{fixture}\\same-store'):
            self.assertTrue(files.is_within(alias_source,self.directory))
        self.assertIn('volume{',files.canonical_directory(self.directory))

    def test_same_generation_in_replaced_store_conflicts(self):
        self.store.save(self.one)
        source=self.root/'project.json';source.write_bytes(project('2'))
        watch=store.Watcher(source,self.store,self.validator,settle=.1);watch.poll(0)
        replacement=self.root/'replacement';other=store.Store(replacement,self.validator);other.save(self.one)
        self.directory.rename(self.root/'old-store');replacement.rename(self.directory)
        self.assertEqual(watch.poll(.2)['status'],'conflict')
        self.assertEqual(self.store.load()['checkpoint'],self.one)

    def test_watch_reports_recovery_before_trying_a_new_candidate(self):
        self.store.save(self.one);self.store.save(self.two)
        (self.directory/'head.json').write_bytes(b'{')
        source=self.root/'project.json'
        watch=store.Watcher(source,self.store,self.validator,settle=.1)
        self.assertEqual(watch.poll(0)['status'],'recovered')
        self.assertEqual(watch.checkpoint,self.one)


if __name__ == '__main__': unittest.main()
