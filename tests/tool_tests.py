import importlib.util, json, tempfile, unittest, zipfile, os, struct
from pathlib import Path
from unittest.mock import patch
spec=importlib.util.spec_from_file_location('forge',Path(__file__).resolve().parents[1]/'tools/forge.py')
forge=importlib.util.module_from_spec(spec)
spec.loader.exec_module(forge)
class Tools(unittest.TestCase):
 def setUp(self):
  self.tmp=tempfile.TemporaryDirectory(prefix='nf-tools-',dir=Path(os.environ.get('NF_TEST_ROOT',Path.cwd())));self.root=Path(self.tmp.name)
 def tearDown(self):self.tmp.cleanup()
 def package(self,files=None):
  p=self.root/'mod.zip';manifest={'id':'com.test.example','name':'Test','version':'0.1.0','api':1,'entry':'plugin.dll'}
  with zipfile.ZipFile(p,'w') as z:
   z.writestr('manifest.json',json.dumps(manifest));z.writestr('plugin.dll',b'original-plugin');
   for n,data in (files or {}).items():z.writestr(n,data)
  return p
 def test_install_receipt_update_and_uninstall_preserve_user_files(self):
  p=self.package();game=self.root/'game';game.mkdir();plan=forge.install(p,game,dry_run=True);self.assertEqual(plan['action'],'install');self.assertFalse((game/'neonforge').exists())
  result=forge.install(p,game);target=game/'neonforge/mods/com.test.example';self.assertEqual((target/'plugin.dll').read_bytes(),b'original-plugin');self.assertEqual(result['owned_files'],2)
  (target/'personal.txt').write_text('keep me');forge.install(p,game);removed=forge.uninstall('com.test.example',game);self.assertEqual(removed['removed'],2);self.assertEqual((target/'personal.txt').read_text(),'keep me')
 def test_modified_owned_file_is_preserved(self):
  game=self.root/'game';game.mkdir();p=self.package();forge.install(p,game);dll=game/'neonforge/mods/com.test.example/plugin.dll';dll.write_bytes(b'local-change')
  with self.assertRaises(forge.ForgeError):forge.install(p,game)
  result=forge.uninstall('com.test.example',game);self.assertIn('plugin.dll',result['preserved']);self.assertEqual(dll.read_bytes(),b'local-change')
 def test_package_traversal_and_case_collisions_rejected(self):
  for files in ({'../escape':b'x'},{'Plugin.dll':b'collision'},{'native/a.dll:stream':b'x'},{'CON.txt':b'x'}):
   with self.assertRaises(forge.ForgeError):forge.inspect_package(self.package(files))
  self.assertFalse((self.root/'escape').exists())
 def test_symlink_archive_rejected(self):
  p=self.package()
  with zipfile.ZipFile(p,'a') as z:
   i=zipfile.ZipInfo('link');i.create_system=3;i.external_attr=(0o120777<<16);z.writestr(i,'outside')
  with self.assertRaises(forge.ForgeError):forge.inspect_package(p)
 def test_install_rolls_back_after_filesystem_failure(self):
  p=self.package({'extra.txt':b'old'});game=self.root/'game';game.mkdir();forge.install(p,game);target=game/'neonforge/mods/com.test.example';old_receipt=(game/'neonforge/receipts/com.test.example.json').read_bytes();p=self.package({'extra.txt':b'new','second.txt':b'new'});real=os.replace;calls=0
  def fail_once(src,dst):
   nonlocal calls;calls+=1
   if calls==2:raise OSError('simulated interrupted replace')
   return real(src,dst)
  with patch.object(forge.os,'replace',side_effect=fail_once):
   with self.assertRaises(forge.ForgeError):forge.install(p,game)
  self.assertEqual((target/'extra.txt').read_bytes(),b'old');self.assertFalse((target/'second.txt').exists());self.assertEqual((game/'neonforge/receipts/com.test.example.json').read_bytes(),old_receipt)
 def test_receipt_cannot_escape_ownership(self):
  game=self.root/'game';game.mkdir();p=self.package();forge.install(p,game);receipt=game/'neonforge/receipts/com.test.example.json';d=json.loads(receipt.read_text());d['files']['../../../outside.txt']='0'*64;receipt.write_text(json.dumps(d))
  with self.assertRaises(forge.ForgeError):forge.uninstall('com.test.example',game)
 def test_settings_migration_is_versioned_and_non_destructive(self):
  p=self.root/'settings.json';p.write_text('{"schema_version":1,"old_key":7,"custom":true}')
  result=forge.migrate_settings(p,1,2,{'old_key':'new_key'},{'default':10});self.assertEqual(result['schema_version'],2);self.assertEqual(result['new_key'],7);self.assertTrue(result['custom']);self.assertNotIn('old_key',result)
  before=p.read_bytes()
  with self.assertRaises(forge.ForgeError):forge.migrate_settings(p,1,3,{}, {})
  self.assertEqual(before,p.read_bytes())
 def test_settings_migration_rejects_collision(self):
  p=self.root/'settings.json';p.write_text('{"schema_version":1,"a":7,"b":8}')
  with self.assertRaises(forge.ForgeError):forge.migrate_settings(p,1,2,{'a':'b'}, {})
  self.assertEqual(json.loads(p.read_text())['schema_version'],1)
 def test_scaffold_never_overwrites_existing_work(self):
  p=self.root/'new';forge.scaffold(p,'com.test.new','New Mod');self.assertTrue((p/'src/plugin.c').exists());self.assertEqual(json.loads((p/'manifest.json').read_text())['id'],'com.test.new')
  with self.assertRaises(forge.ForgeError):forge.scaffold(p,'com.test.new','New Mod')
 def test_resolver_refuses_primary_collision_and_bad_bounds(self):
  sections=[{'rva':4096,'virtual_size':64,'raw_size':64,'raw_offset':512}];entries=[{'hash':'5','secondary hash':'a'*64,'offset':'0001:00000010'},{'hash':'5','secondary hash':'b'*64,'offset':'0001:00000020'}]
  with self.assertRaises(forge.ForgeError):forge.resolve_candidate(sections,entries,'5')
  result=forge.resolve_candidate(sections,entries,'5','b'*64);self.assertEqual(result['rva'],4128);self.assertFalse(result['abi_verified'])
  entries[1]['offset']='0001:00000080'
  with self.assertRaises(forge.ForgeError):forge.resolve_candidate(sections,entries,'5','b'*64)
 def test_pe_inspector_checks_header_and_section_bounds(self):
  blob=bytearray(576);blob[:2]=b'MZ';struct.pack_into('<I',blob,0x3c,128);blob[128:132]=b'PE\0\0';struct.pack_into('<HHIIIHH',blob,132,0x8664,1,0x11223344,0,0,240,0x2022);struct.pack_into('<H',blob,152,0x20b);struct.pack_into('<Q',blob,176,0x140000000);struct.pack_into('<I',blob,208,8192);blob[392:400]=b'.text\0\0\0';struct.pack_into('<IIII',blob,400,64,4096,64,512);struct.pack_into('<I',blob,428,0x60000020)
  p=self.root/'fixture.dll';p.write_bytes(blob);result=forge.inspect_pe(p);self.assertEqual(result['machine'],'0x8664');self.assertEqual(result['image_base'],0x140000000);self.assertEqual(result['sections'][0]['raw_offset'],512)
  struct.pack_into('<I',blob,412,9000);p.write_bytes(blob)
  with self.assertRaises(forge.ForgeError):forge.inspect_pe(p)
 def test_malformed_updates_cannot_replace_valid_package(self):
  p=self.package();game=self.root/'game';game.mkdir();forge.install(p,game);receipt=game/'neonforge/receipts/com.test.example.json';before=receipt.read_bytes();dll=game/'neonforge/mods/com.test.example/plugin.dll'
  invalid=[{'requires':'not-an-array'},{'version':'4294967296.0.0'},{'capabilities':[{'name':'nf.events','version':0}]},{'before':7},{'requires':[{'id':'com.test.other','min':'2.0.0','max_exclusive':'1.0.0'}]},{'capabilities':[{'name':'nf.events','version':True}]},{'requires':[{'id':'com.test.other','min':'0.1.0','typo':True}]}]
  for mutation in invalid:
   manifest={'id':'com.test.example','name':'Test','version':'0.1.0','api':1,'entry':'plugin.dll',**mutation}
   with zipfile.ZipFile(p,'w') as z:z.writestr('manifest.json',json.dumps(manifest));z.writestr('plugin.dll',b'broken-new-version')
   with self.assertRaises(forge.ForgeError):forge.install(p,game)
   self.assertEqual(receipt.read_bytes(),before);self.assertEqual(dll.read_bytes(),b'original-plugin')
if __name__=='__main__':unittest.main()
