"""Exercise the benchmark against the real runtime DLL, without timing gates."""
import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(sys.argv[1]).resolve()
sys.argv = sys.argv[:1]


class BenchmarkTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='nf-benchmark-', dir=BUILD)
        self.root = Path(self.temp.name)
        self.exe = BUILD / 'nf_benchmark.exe'
        self.dll = BUILD / 'NeonForge.Core.dll'

    def tearDown(self):
        self.temp.cleanup()

    def run_native(self, iterations='64', samples='3'):
        self.assertTrue(self.exe.exists(), 'native ABI benchmark is not implemented')
        return subprocess.run([str(self.exe), str(self.dll), str(self.root / 'data'), iterations, samples], capture_output=True, text=True, timeout=45)

    def test_real_abi_workloads_count_callbacks_and_tasks(self):
        result = self.run_native()
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report['schema_version'], 1)
        self.assertEqual(report['iterations'], 64)
        self.assertEqual(report['samples'], 3)
        self.assertEqual(len(report['cases']), 9)
        for case in report['cases']:
            self.assertEqual(len(case['samples_ns_per_operation']), 3)
            self.assertGreater(case['median_ns_per_operation'], 0)
            self.assertLessEqual(case['min_ns_per_operation'], case['median_ns_per_operation'])
            self.assertLessEqual(case['median_ns_per_operation'], case['p95_ns_per_operation'])
            expected = 64 * case['matching_listeners'] if case['operation'] == 'event_emit' else (4096 if case['operation'] == 'task_run' else 0)
            self.assertEqual(case['verified_callbacks_per_sample'], expected)
            self.assertTrue(case['cleanup_verified'])

    def test_successful_noop_cancellation_cannot_produce_a_valid_report(self):
        self.assertTrue(self.exe.exists())
        defective = BUILD / 'nf_noncancelling_runtime.dll'
        self.assertTrue(defective.exists())
        result = subprocess.run([str(self.exe), str(defective), str(self.root / 'defective'), '64', '3'], capture_output=True, text=True, timeout=45)
        self.assertNotEqual(result.returncode, 0, 'benchmark accepted cancellation that did no work')
        self.assertIn('pending tasks', result.stderr)

    def test_invalid_workloads_are_rejected_before_benchmarking(self):
        for iterations, samples in [('0', '3'), ('64', '0'), ('64', '2'), ('64oops', '3'), ('64', '1001')]:
            result = self.run_native(iterations, samples)
            self.assertNotEqual(result.returncode, 0)

    def test_runner_records_identity_and_rejects_mismatched_comparisons(self):
        self.assertTrue(self.exe.exists(), 'native ABI benchmark is not implemented')
        runner = ROOT / 'tools/benchmark.py'
        self.assertTrue(runner.exists(), 'benchmark report runner is not implemented')
        base = self.root / 'base.json'
        candidate = self.root / 'candidate.json'
        command = [sys.executable, str(runner), '--executable', str(self.exe), '--runtime', str(self.dll), '--iterations', '64', '--samples', '3']
        result = subprocess.run(command + ['--output', str(base)], capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stderr)
        original = json.loads(base.read_text())
        self.assertEqual(len(original['artifacts']['runtime_sha256']), 64)
        self.assertNotIn(str(ROOT), base.read_text())
        result = subprocess.run(command + ['--output', str(candidate), '--baseline', str(base)], capture_output=True, text=True, timeout=45)
        self.assertEqual(result.returncode, 0, result.stderr)
        compared = json.loads(candidate.read_text())
        self.assertEqual(len(compared['comparison']['cases']), 9)
        original['iterations'] += 1
        base.write_text(json.dumps(original))
        result = subprocess.run(command + ['--output', str(self.root / 'bad.json'), '--baseline', str(base)], capture_output=True, text=True, timeout=45)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse((self.root / 'bad.json').exists())


if __name__ == '__main__':
    unittest.main()
