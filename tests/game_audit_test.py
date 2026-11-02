"""Exercise a read-only full-file inventory with original filesystem fixtures."""
import hashlib
import importlib.util
from pathlib import Path
import tempfile
import os
import subprocess
import unittest

source=Path(__file__).resolve().parents[1]
spec=importlib.util.spec_from_file_location('game_audit',source/'tools/game_audit.py')
assert spec and spec.loader and (source/'tools/game_audit.py').exists(), 'repeatable game audit is not implemented'
audit=importlib.util.module_from_spec(spec);spec.loader.exec_module(audit)

class AuditTests(unittest.TestCase):
    def setUp(self):
        self.temp=tempfile.TemporaryDirectory(prefix='nf-audit-',dir=source);self.root=Path(self.temp.name)/'game';self.root.mkdir()
    def tearDown(self):self.temp.cleanup()
    def file(self,name,data):
        path=self.root/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(data);return path
    def test_hashes_every_regular_file_and_preserves_input(self):
        self.file('bin/x64/Cyberpunk2077.exe',b'MZoriginal fixture')
        self.file('archive/pc/content/base.archive',b'RDARfixture')
        result=audit.audit(self.root)
        self.assertTrue(result['complete']);self.assertEqual(result['files_hashed'],2)
        entries={entry['path']:entry for entry in result['files']}
        self.assertEqual(entries['archive/pc/content/base.archive']['sha256'],hashlib.sha256(b'RDARfixture').hexdigest())
        self.assertEqual(entries['archive/pc/content/base.archive']['header_hex'],'5244415266697874757265')
        self.assertEqual((self.root/'bin/x64/Cyberpunk2077.exe').read_bytes(),b'MZoriginal fixture')
        self.assertFalse(result['pristine_install_verified'])
    def test_known_markers_do_not_count_vendor_dlls_as_mods(self):
        self.file('bin/x64/nvngx_dlss.dll',b'vendor');self.file('bin/x64/winmm.dll',b'proxy')
        self.file('r6/scripts/example.reds',b'original test')
        report=audit.audit(self.root)
        self.assertIn('bin/x64/winmm.dll',report['known_mod_markers'])
        self.assertIn('r6/scripts',report['known_mod_markers'])
        self.assertNotIn('bin/x64/nvngx_dlss.dll',report['known_mod_markers'])
    def test_rejects_output_under_game_even_when_missing(self):
        with self.assertRaises(ValueError):audit.write_report(self.root,self.root/'new/report.json')
        self.assertFalse((self.root/'new').exists())
    def test_missing_root_is_an_error(self):
        with self.assertRaises(ValueError):audit.audit(self.root/'missing')
    def test_outside_hardlinked_report_cannot_overwrite_game_file(self):
        game_file=self.file('original.bin',b'preserve original bytes')
        output=Path(self.temp.name)/'report.json'
        os.link(game_file,output)
        report=audit.write_report(self.root,output)
        self.assertEqual(game_file.read_bytes(),b'preserve original bytes')
        self.assertTrue(report['complete'])
    def test_changed_file_prevents_complete_result(self):
        path=self.file('changed.bin',b'first')
        original=audit.hash_file
        def change(file):
            answer=original(file);file.write_bytes(b'changed size');return answer
        audit.hash_file=change
        try:report=audit.audit(self.root)
        finally:audit.hash_file=original
        self.assertFalse(report['complete']);self.assertEqual(report['files_hashed'],0)
        self.assertTrue(report['errors'])
    def test_linked_directory_is_reported_without_reading_target(self):
        outside=Path(self.temp.name)/'outside';outside.mkdir();(outside/'private').write_text('fixture')
        try:(self.root/'linked').symlink_to(outside,target_is_directory=True)
        except OSError as error:
            if os.name!='nt':self.skipTest('Link fixture unavailable: '+str(error))
            linked=subprocess.run(['powershell','-NoProfile','-Command','New-Item -ItemType Junction -Path $env:NF_AUDIT_LINK -Target $env:NF_AUDIT_TARGET | Out-Null'],env={**os.environ,'NF_AUDIT_LINK':str(self.root/'linked'),'NF_AUDIT_TARGET':str(outside)},capture_output=True,text=True,timeout=20)
            self.assertEqual(linked.returncode,0,linked.stderr)
        report=audit.audit(self.root)
        self.assertFalse(report['complete']);self.assertEqual(report['files_hashed'],0)
        self.assertTrue(report['errors'])

    def test_read_error_is_reported_and_other_files_are_still_hashed(self):
        self.file('bad.bin',b'fixture');self.file('good.bin',b'keep reading')
        original=audit.hash_file
        def fail_one(path):
            if path.name=='bad.bin':raise PermissionError('injected read error')
            return original(path)
        audit.hash_file=fail_one
        try:report=audit.audit(self.root)
        finally:audit.hash_file=original
        self.assertFalse(report['complete']);self.assertEqual(report['files_hashed'],1)
        self.assertEqual(report['files'][0]['path'],'good.bin')
        self.assertEqual(report['errors'][0]['path'],'bad.bin')

if __name__=='__main__':unittest.main()
