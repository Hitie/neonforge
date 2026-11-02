"""Verify pinned Lua survives a real Windows Git checkout."""
import importlib.util
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

source = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location('verify_dependencies', source / 'tools/verify_dependencies.py')
verify_dependencies = importlib.util.module_from_spec(spec)
spec.loader.exec_module(verify_dependencies)

class CheckoutTests(unittest.TestCase):
    @unittest.skipUnless(shutil.which('git'), 'Git required only for checkout characterization')
    def test_autocrlf_checkout_retains_exact_upstream_bytes(self):
        with tempfile.TemporaryDirectory(prefix='nf-checkout-') as temp:
            root = Path(temp)
            repo = root / 'repo'
            repo.mkdir()
            shutil.copytree(source / 'third_party', repo / 'third_party')
            if (source / '.gitattributes').exists():
                shutil.copy2(source / '.gitattributes', repo / '.gitattributes')
            commands = [
                ['init','--quiet'], ['add','.'],
                ['checkout-index','--all','--prefix='+(root / 'export').as_posix()+'/'],
            ]
            for args in commands:
                run = subprocess.run(['git','-c','core.autocrlf=true','-c','core.attributesfile=',*args],cwd=repo,capture_output=True,text=True,timeout=30)
                self.assertEqual(run.returncode,0,run.stdout+run.stderr)
            self.assertEqual(verify_dependencies.verify(root / 'export'),62)

if __name__ == '__main__': unittest.main()
