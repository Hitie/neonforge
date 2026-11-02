"""Run the real-DLL benchmark and compare identical standalone workloads."""
import argparse
import datetime
import hashlib
import json
import platform
import subprocess
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compare(report, baseline):
    for field in ['schema_version', 'iterations', 'samples', 'configuration', 'compiler_msvc', 'clock', 'environment']:
        if report[field] != baseline[field]:
            raise ValueError('incomparable benchmark field: ' + field)
    if report['artifacts']['benchmark_sha256'] != baseline['artifacts']['benchmark_sha256']:
        raise ValueError('comparison requires the same benchmark executable')
    old = {case['name']: case for case in baseline['cases']}
    if len(old) != len(report['cases']):
        raise ValueError('comparison requires the same scenario set')
    result = []
    for case in report['cases']:
        previous = old[case['name']]
        for field in ['operation', 'matching_listeners', 'unrelated_listeners', 'operations_per_sample', 'verified_callbacks_per_sample', 'cleanup_verified']:
            if case[field] != previous[field]:
                raise ValueError('incomparable workload: ' + case['name'] + '/' + field)
        before, after = previous['median_ns_per_operation'], case['median_ns_per_operation']
        if before <= 0 or after <= 0:
            raise ValueError('benchmark timings must be positive')
        result.append({'name': case['name'], 'baseline_median_ns': before, 'candidate_median_ns': after, 'speedup': before / after})
    return {'baseline_runtime_sha256': baseline['artifacts']['runtime_sha256'], 'cases': result}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--runtime', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--baseline', type=Path)
    parser.add_argument('--iterations', type=int, default=5000)
    parser.add_argument('--samples', type=int, default=7)
    args = parser.parse_args()
    try:
        executable, runtime = args.executable.resolve(strict=True), args.runtime.resolve(strict=True)
        if args.output.resolve() in {executable, runtime} or (args.baseline and args.output.resolve() == args.baseline.resolve()):
            raise ValueError('output must not overwrite benchmark inputs')
        baseline = json.loads(args.baseline.read_text(encoding='utf-8')) if args.baseline else None
        artifacts = {'benchmark_file': executable.name, 'benchmark_sha256': sha256(executable), 'runtime_file': runtime.name, 'runtime_sha256': sha256(runtime)}
        args.output.parent.mkdir(parents=True, exist_ok=True)
        result = subprocess.run([str(executable), str(runtime), str(args.output.parent.resolve() / 'benchmark-data'), str(args.iterations), str(args.samples)], capture_output=True, text=True, encoding='utf-8', timeout=600)
        if result.returncode:
            raise ValueError(result.stderr.strip() or 'native benchmark failed')
        if artifacts['runtime_sha256'] != sha256(runtime) or artifacts['benchmark_sha256'] != sha256(executable):
            raise ValueError('benchmark input changed during the run')
        report = json.loads(result.stdout)
        report['artifacts'] = artifacts
        report['environment'] = {'system': platform.system(), 'release': platform.release(), 'machine': platform.machine(), 'processor': platform.processor()}
        report['recorded_at_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
        report['scope'] = 'standalone C ABI microbenchmarks; no game or competitor comparison'
        if baseline is not None:
            report['comparison'] = compare(report, baseline)
        args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
        print(json.dumps({'output': args.output.name, 'cases': len(report['cases']), 'runtime_sha256': artifacts['runtime_sha256']}))
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.TimeoutExpired) as error:
        print('NEONFORGE benchmark: ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
