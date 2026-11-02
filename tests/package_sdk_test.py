"""Validate actual distributables and run the extracted native SDK example."""
import hashlib
import importlib.util
import json
import shutil
import subprocess
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path

SOURCE = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]
module_path = SOURCE / 'tools/package_sdk.py'
assert module_path.exists(), 'SDK/source distribution builder is not implemented'
spec = importlib.util.spec_from_file_location('package_sdk', module_path)
package_sdk = importlib.util.module_from_spec(spec)
spec.loader.exec_module(package_sdk)


class DistributionTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nf-distribution-', dir=BUILD)
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def test_extracted_sdk_runs_and_hashes_match(self):
        result = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'dist')
        archive = self.root / 'dist' / result['sdk']['name']
        with zipfile.ZipFile(archive) as z:
            self.assertIsNone(z.testzip())
            z.extractall(self.root / 'extracted')
        sdk = next((self.root / 'extracted').iterdir())
        for line in (sdk / 'SHA256SUMS.txt').read_text().splitlines():
            digest, name = line.split('  ', 1)
            self.assertEqual(hashlib.sha256((sdk / name).read_bytes()).hexdigest(), digest)
        run = subprocess.run([str(sdk / 'runtime/neonforge.exe'), 'run', str(sdk / 'examples/mods'), str(sdk / 'data'), '--ticks', '3'], capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(run.returncode, 0, run.stderr)
        self.assertEqual(json.loads(run.stdout)['started'], 2)
        self.assertEqual(json.loads((sdk / 'data/settings/org.neonforge.consumer/result.json').read_text())['answer'], 42)
        with zipfile.ZipFile(self.root / 'dist' / result['source']['name']) as source:
            paths = source.namelist()
            self.assertTrue(any(name.endswith('/src/kernel.cpp') for name in paths))
            self.assertFalse(any('/build-' in name or '/data/' in name or name.endswith('PROGRESS.md') for name in paths))

    def test_extracted_sdk_benchmark_uses_the_packaged_runtime(self):
        result = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'dist')
        with zipfile.ZipFile(self.root / 'dist' / result['sdk']['name']) as archive:
            archive.extractall(self.root / 'extracted')
        sdk = next((self.root / 'extracted').iterdir())
        self.assertTrue((sdk / 'tools/nf_benchmark.exe').exists(), 'SDK benchmark binary missing')
        self.assertTrue((sdk / 'tools/benchmark.py').exists(), 'SDK benchmark runner missing')
        command = [sys.executable, str(sdk / 'tools/benchmark.py'), '--executable', str(sdk / 'tools/nf_benchmark.exe'), '--runtime', str(sdk / 'runtime/NeonForge.Core.dll'), '--iterations', '64', '--samples', '3', '--output', str(self.root / 'report.json')]
        run = subprocess.run(command, capture_output=True, text=True, timeout=45)
        self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
        report = json.loads((self.root / 'report.json').read_text())
        self.assertEqual(len(report['cases']), 9)
        self.assertEqual(report['artifacts']['runtime_sha256'], hashlib.sha256((sdk / 'runtime/NeonForge.Core.dll').read_bytes()).hexdigest())

    def test_extracted_typed_c_demo_runs_and_persists_its_result(self):
        result=package_sdk.build_distributions(SOURCE,BUILD,self.root/'dist')
        with zipfile.ZipFile(self.root/'dist'/result['sdk']['name']) as archive:
            archive.extractall(self.root/'extracted')
        sdk=next((self.root/'extracted').iterdir())
        demo=sdk/'runtime/nf_typed_example.exe'
        self.assertTrue(demo.exists(),'typed C demo binary missing from SDK')
        self.assertTrue((sdk/'include/neonforge/typed.h').exists())
        self.assertTrue((sdk/'lib/NeonForge.Core.lib').read_bytes().startswith(b'!<arch>\n'))
        run=subprocess.run([str(demo)],cwd=sdk,capture_output=True,text=True,timeout=30)
        self.assertEqual(run.returncode,0,run.stderr+run.stdout)
        self.assertEqual(json.loads(run.stdout)['answer'],42)
        saved=sdk/'data/typed-demo/settings/org.neonforge.typed.consumer/result.json'
        self.assertEqual(json.loads(saved.read_text())['answer'],42)
        project=self.root/'typed-source'
        project.mkdir()
        (project/'CMakeLists.txt').write_text('cmake_minimum_required(VERSION 3.25)\nproject(TypedExample LANGUAGES C)\nadd_executable(typed "'+(sdk/'examples/native/typed_calls.c').as_posix()+'")\ntarget_include_directories(typed PRIVATE "'+(sdk/'include').as_posix()+'")\ntarget_link_libraries(typed PRIVATE "'+(sdk/'lib/NeonForge.Core.lib').as_posix()+'")\n')
        for command in [
            ['cmake','-S',str(project),'-B',str(project/'build'),'-A','x64'],
            ['cmake','--build',str(project/'build'),'--config','Release'],
        ]:
            built=subprocess.run(command,capture_output=True,text=True,timeout=120)
            self.assertEqual(built.returncode,0,built.stdout+built.stderr)
        executable=project/'build/Release/typed.exe'
        shutil.copy2(sdk/'runtime/NeonForge.Core.dll',executable.parent/'NeonForge.Core.dll')
        run=subprocess.run([str(executable)],cwd=project,capture_output=True,text=True,timeout=30)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        self.assertEqual(json.loads(run.stdout)['answer'],42)

    def test_source_export_excludes_private_and_build_files(self):
        root = self.root / 'source'
        for name in ['src/owned.cpp', 'LICENSE', 'build/private.cpp', 'data/save.json', 'PROGRESS.md', 'game-file-inventory.csv', 'tools/__pycache__/secret.pyc', '.github/private-key']:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        selected = [p.relative_to(root).as_posix() for p in package_sdk.source_files(root)]
        self.assertEqual(selected, ['LICENSE', 'src/owned.cpp'])

    def test_extracted_sdk_audit_runs_without_modifying_fixture(self):
        result=package_sdk.build_distributions(SOURCE,BUILD,self.root/'dist')
        with zipfile.ZipFile(self.root/'dist'/result['sdk']['name']) as archive:
            archive.extractall(self.root/'extracted')
        sdk=next((self.root/'extracted').iterdir())
        self.assertTrue((sdk/'tools/game_audit.py').is_file(),'documented audit tool missing from SDK')
        game=self.root/'game';game.mkdir();(game/'original.bin').write_bytes(b'original')
        output=self.root/'audit.json'
        run=subprocess.run([sys.executable,str(sdk/'tools/game_audit.py'),str(game),'--output',str(output)],capture_output=True,text=True,timeout=30)
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
        self.assertEqual(json.loads(output.read_text())['files_hashed'],1)
        self.assertEqual((game/'original.bin').read_bytes(),b'original')

    def test_extracted_sdk_runs_original_lua_mods_and_retains_notices(self):
        result = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'dist')
        with zipfile.ZipFile(self.root / 'dist' / result['sdk']['name']) as archive:
            archive.extractall(self.root / 'extracted')
        sdk = next((self.root / 'extracted').iterdir())
        run = subprocess.run([str(sdk / 'runtime/neonforge.exe'), 'run', str(sdk / 'examples/scripts'), str(sdk / 'script-data'), '--ticks', '3'], capture_output=True, text=True, timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(json.loads(run.stdout)['started'], 2)
        self.assertEqual(json.loads((sdk / 'script-data/settings/org.neonforge.lua.consumer/result.json').read_text())['answer'], 42)
        self.assertTrue((sdk / 'THIRD_PARTY_NOTICES.md').is_file())
        self.assertTrue((sdk / 'third_party/lua/LICENSE.txt').is_file())
        with zipfile.ZipFile(self.root / 'dist' / result['source']['name']) as archive:
            self.assertTrue(any(name.endswith('/third_party/lua/lvm.c') for name in archive.namelist()))

    def test_same_inputs_produce_identical_archives(self):
        first = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'one')
        second = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'two')
        for key in ['source', 'sdk']:
            self.assertEqual(first[key]['sha256'], second[key]['sha256'])

    def test_missing_or_changed_upstream_source_cannot_be_packaged(self):
        source = self.root / 'altered-source'
        for path in package_sdk.source_files(SOURCE):
            target = source / path.relative_to(SOURCE)
            target.parent.mkdir(parents=True,exist_ok=True)
            shutil.copy2(path,target)
        header = source / 'third_party/lua/lua.h'
        original = header.read_bytes()
        for change in ['missing','modified']:
            if change == 'missing': header.unlink()
            else: header.write_bytes(original+b'\n/* modified */\n')
            output = self.root / ('rejected-'+change)
            with self.assertRaises(package_sdk.PackageError):
                package_sdk.build_distributions(source,BUILD,output)
            self.assertFalse(output.exists())

    def test_missing_binary_cannot_produce_a_partial_distribution(self):
        incomplete = self.root / 'incomplete'
        incomplete.mkdir()
        shutil.copy2(BUILD / 'neonforge.exe', incomplete / 'neonforge.exe')
        out = self.root / 'must-not-exist'
        with self.assertRaises(package_sdk.PackageError):
            package_sdk.build_distributions(SOURCE, incomplete, out)
        self.assertFalse(out.exists())

    def test_generated_mod_builds_against_the_extracted_sdk_root(self):
        result = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'dist')
        with zipfile.ZipFile(self.root / 'dist' / result['sdk']['name']) as archive:
            archive.extractall(self.root / 'sdk')
        sdk = next((self.root / 'sdk').iterdir())
        self.assertTrue((sdk / 'include/neonforge/api.h').is_file(), 'documented SDK root must contain include/neonforge/api.h')
        mods = self.root / 'mods'
        project = mods / 'generated'
        commands = [
            [sys.executable, str(sdk / 'tools/forge.py'), 'new', str(project), '--id', 'com.test.generated', '--name', 'Extracted SDK Example'],
            ['cmake', '-S', str(project), '-B', str(self.root / 'b'), '-A', 'x64', '-DNEONFORGE_SDK=' + str(sdk)],
            ['cmake', '--build', str(self.root / 'b'), '--config', 'Release'],
        ]
        for command in commands:
            run = subprocess.run(command, capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=120)
            self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        shutil.copy2(self.root / 'b/Release/plugin.dll', project / 'plugin.dll')
        run = subprocess.run([str(sdk / 'runtime/neonforge.exe'), 'run', str(mods), str(self.root / 'generated-data')], capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(run.returncode, 0, run.stdout + run.stderr)
        self.assertEqual(json.loads(run.stdout)['started'], 1)

    def test_source_directory_junction_cannot_export_external_sentinel(self):
        source = self.root / 'source'
        source.mkdir()
        external = self.root / 'private'
        external.mkdir()
        (external / 'sentinel.md').write_text('private fixture; must never be exported')
        link = source / 'docs'
        create = subprocess.run(['cmd', '/d', '/c', 'mklink', '/J', str(link), str(external)], capture_output=True, text=True, errors='replace')
        self.assertEqual(create.returncode, 0, create.stdout + create.stderr)
        with self.assertRaises(package_sdk.PackageError):
            package_sdk.source_files(source)


if __name__ == '__main__':
    unittest.main()
