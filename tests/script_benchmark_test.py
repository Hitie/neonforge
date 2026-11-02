"""Exercise the real Lua VM benchmark and verify its declared work."""
import json
from pathlib import Path
import subprocess
import sys

binary = Path(sys.argv[1]) / 'nf_script_benchmark.exe'
assert binary.exists(), 'Lua workload benchmark is not implemented'
run = subprocess.run([str(binary), '16', '3'], capture_output=True, text=True, timeout=30)
assert run.returncode == 0, run.stdout + run.stderr
report = json.loads(run.stdout)
assert report['lua'] == '5.5.1'
assert {case['name'] for case in report['cases']} == {'script.typed_call', 'script.event', 'script.load_unload'}
for case in report['cases']:
    assert case['operations_per_sample'] == 16 and case['verified_operations'] == 48
    assert len(case['samples_ns_per_operation']) == 3
    assert all(sample > 0 for sample in case['samples_ns_per_operation'])
    assert 0 < case['peak_lua_bytes'] <= 8 * 1048576
bad = subprocess.run([str(binary), '0', '3'], capture_output=True, text=True, timeout=30)
assert bad.returncode != 0
print('PASS verified Lua typed-call, event and startup benchmark workloads')
