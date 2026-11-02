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

    def test_source_export_excludes_private_and_build_files(self):
        root = self.root / 'source'
        for name in ['src/owned.cpp', 'LICENSE', 'build/private.cpp', 'data/save.json', 'PROGRESS.md', 'game-file-inventory.csv', 'tools/__pycache__/secret.pyc']:
            path = root / name
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text('fixture')
        selected = [p.relative_to(root).as_posix() for p in package_sdk.source_files(root)]
        self.assertEqual(selected, ['LICENSE', 'src/owned.cpp'])

    def test_same_inputs_produce_identical_archives(self):
        first = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'one')
        second = package_sdk.build_distributions(SOURCE, BUILD, self.root / 'two')
        for key in ['source', 'sdk']:
            self.assertEqual(first[key]['sha256'], second[key]['sha256'])

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
