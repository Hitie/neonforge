import json,subprocess,sys
from pathlib import Path
p=Path(sys.argv[1]).resolve()/'nf_winmm_probe.exe'
assert p.exists(),'Windows forwarding-qualification probe is not implemented'
r=subprocess.run([str(p)],capture_output=True,text=True,encoding='utf-8',timeout=20)
assert r.returncode==0,r.stderr
d=json.loads(r.stdout)
assert d['named_exports']>0 and d['resolved_names']==d['named_exports'] and d['unresolved_exports']==0,d
assert d['timer_caps_ok'] and d['timer_delta_ms']<1000,d
assert d['game_bootstrap_verified'] is False and d['engine_adapter_ready'] is False,d
print('PASS Windows system-library name/ordinal resolution and representative calls; game bootstrap remains unverified')
