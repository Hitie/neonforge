"""Reproducible, explicit exports of original source and the standalone SDK."""
import argparse
import hashlib
import json
import stat
import struct
import zipfile
from pathlib import Path

VERSION = '0.1.2-dev.1'
ROOT_FILES = {'.clang-format', '.gitignore', 'CMakeLists.txt', 'build.ps1', 'README.md', 'CHANGELOG.md', 'LICENSE', 'CONTRIBUTING.md', 'SECURITY.md', '.github/CODEOWNERS', 'benchmarks/results/baseline.json', 'benchmarks/results/candidate.json'}
DIRECTORIES = {
    'src': {'.cpp', '.hpp', '.h'},
    'include': {'.h', '.hpp'},
    'examples': {'.c', '.cpp', '.h', '.hpp', '.json'},
    'tests': {'.c', '.cpp', '.h', '.hpp', '.py'},
    'tools': {'.py'},
    'docs': {'.md'},
    'schemas': {'.json'},
    'adapters': {'.json'},
    '.github': {'.yml', '.yaml', '.md'},
    'assets': {'.svg'},
    'benchmarks': {'.cpp'},
}


class PackageError(RuntimeError):
    pass


def _regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
        raise PackageError('Export requires a regular file: ' + str(path))


def _export_path(root, path):
    try:
        relative = path.relative_to(root)
    except ValueError as error:
        raise PackageError('Export path is outside the selected source root') from error
    current = root
    for part in [None, *relative.parts]:
        if part is not None:
            current /= part
        info = current.lstat()
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise PackageError('Linked source path refused: ' + str(current))
    if not path.resolve().is_relative_to(root.resolve()):
        raise PackageError('Resolved export path escapes the source root')


def source_files(root):
    root = Path(root).absolute()
    _export_path(root, root)
    selected = []
    for name in ROOT_FILES:
        path = root / name
        if path.exists():
            _export_path(root, path)
            _regular(path)
            selected.append(path)
    for folder, extensions in DIRECTORIES.items():
        directory = root / folder
        try:
            directory.lstat()
        except FileNotFoundError:
            continue
        def walk(current):
            _export_path(root, current)
            if not current.is_dir():
                raise PackageError('Expected an original source directory: ' + str(current))
            for path in sorted(current.iterdir()):
                if path.name in {'__pycache__', '.git', '.vs'}:
                    continue
                _export_path(root, path)
                if path.is_dir():
                    walk(path)
                elif path.suffix in extensions:
                    _regular(path)
                    selected.append(path)
        walk(directory)
    return sorted(selected, key=lambda path: path.relative_to(root).as_posix())


def _native(path):
    if not path.is_file():
        raise PackageError('Required build artifact missing: ' + str(path))
    _regular(path)
    data = path.read_bytes()
    try:
        offset = struct.unpack_from('<I', data, 0x3c)[0]
        machine = struct.unpack_from('<H', data, offset + 4)[0]
        optional = struct.unpack_from('<H', data, offset + 24)[0]
        if data[:2] != b'MZ' or data[offset:offset + 4] != b'PE\0\0' or machine != 0x8664 or optional != 0x20b:
            raise ValueError('not a Windows x64 PE image')
    except (struct.error, ValueError) as error:
        raise PackageError('Invalid native build artifact: ' + str(path)) from error
    return data


def _zip(path, prefix, entries):
    with zipfile.ZipFile(path, 'w', compression=zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
        for name, data in sorted(entries.items()):
            item = zipfile.ZipInfo(prefix + '/' + name, (2026, 11, 2, 0, 0, 0))
            item.compress_type = zipfile.ZIP_DEFLATED
            item.external_attr = 0o100644 << 16
            archive.writestr(item, data)
    with zipfile.ZipFile(path) as archive:
        if archive.testzip() is not None:
            raise PackageError('Archive integrity check failed')
    return {'name': path.name, 'bytes': path.stat().st_size, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest()}


def _checksums(entries):
    return ''.join(hashlib.sha256(data).hexdigest() + '  ' + name + '\n' for name, data in sorted(entries.items())).encode('utf-8')


def build_distributions(source, build, output):
    source, build, output = Path(source).resolve(), Path(build).resolve(), Path(output).resolve()
    binaries = {name: _native(build / name) for name in ['neonforge.exe', 'NeonForge.Core.dll', 'nf_example.dll', 'nf_consumer.dll', 'nf_winmm_probe.exe', 'nf_benchmark.exe', 'nf_typed_example.exe']}
    import_library = build / 'NeonForge.Core.lib'
    _regular(import_library)
    library_data = import_library.read_bytes()
    if not library_data.startswith(b'!<arch>\n'):
        raise PackageError('Invalid native import library: ' + str(import_library))
    original = {}
    for path in source_files(source):
        _export_path(source, path)
        original[path.relative_to(source).as_posix()] = path.read_bytes()
    for required in ['LICENSE', 'include/neonforge/api.h', 'examples/manifests/provider.json', 'examples/manifests/consumer.json', 'docs/status.md']:
        if required not in original:
            raise PackageError('Required source file missing: ' + required)
    sdk = {
        'runtime/neonforge.exe': binaries['neonforge.exe'],
        'runtime/NeonForge.Core.dll': binaries['NeonForge.Core.dll'],
        'runtime/nf_typed_example.exe': binaries['nf_typed_example.exe'],
        'lib/NeonForge.Core.lib': library_data,
        'examples/mods/org.neonforge.example/plugin.dll': binaries['nf_example.dll'],
        'examples/mods/org.neonforge.consumer/plugin.dll': binaries['nf_consumer.dll'],
        'examples/mods/org.neonforge.example/manifest.json': original['examples/manifests/provider.json'],
        'examples/mods/org.neonforge.consumer/manifest.json': original['examples/manifests/consumer.json'],
        'tools/nf_winmm_probe.exe': binaries['nf_winmm_probe.exe'],
        'tools/nf_benchmark.exe': binaries['nf_benchmark.exe'],
        'LICENSE': original['LICENSE'],
    }
    for name, data in original.items():
        if name.startswith('include/'):
            sdk[name] = data
        elif name.startswith('examples/native/'):
            sdk[name] = data
        elif name.startswith(('docs/', 'schemas/', 'adapters/', 'benchmarks/results/')) or name in {'tools/forge.py', 'tools/benchmark.py'}:
            sdk[name] = data
    sdk['README.md'] = f'''# NEONFORGE Core {VERSION} — standalone SDK

One foundation. Build Night City.

This is a development SDK, not an installed Cyberpunk mod or a replacement for the six reference frameworks. Engine hooks, Lua, TweakDB, resources, the compiler and game UI remain unavailable. Do not copy this package over your game's loaders.

Extract the complete directory and double-click Run-Demo.cmd. The original C and C++ plugins exchange a service, receive an event, execute a deferred task and persist their results under data/. No game installation is required. Windows x64 and the Microsoft Visual C++ x64 runtime are required; Python is needed only for tools/forge.py.

Run-Typed-Demo.cmd demonstrates signature lookup and typed native invocation from C, preserving its result under data/typed-demo/. See docs/typed-calls.md for ownership, limits and build instructions using lib/NeonForge.Core.lib.

Commands:

    runtime\\neonforge.exe validate examples\\mods
    runtime\\neonforge.exe run examples\\mods data --ticks 3

SDK headers are under include/ and example source is under examples/native/. Set NEONFORGE_SDK to this extracted package root when building a generated project. API/status/tool documentation is under docs/. The complete source project is supplied in the separate source archive and at https://github.com/Hitie/neonforge . The repository's existing GPLv3 license is preserved.

SHA256SUMS.txt verifies every other file in this package. Standalone tests do not establish in-game functionality or performance.
'''.encode('utf-8')
    sdk['Run-Demo.cmd'] = b'@echo off\r\nsetlocal\r\npushd "%~dp0"\r\n"runtime\\neonforge.exe" run "examples\\mods" "data" --ticks 3\r\nset "nf_result=%ERRORLEVEL%"\r\necho.\r\nif not "%~1"=="--no-pause" pause\r\npopd\r\nexit /b %nf_result%\r\n'
    sdk['Run-Typed-Demo.cmd'] = b'@echo off\r\nsetlocal\r\npushd "%~dp0"\r\n"runtime\\nf_typed_example.exe"\r\nset "nf_result=%ERRORLEVEL%"\r\necho.\r\nif not "%~1"=="--no-pause" pause\r\npopd\r\nexit /b %nf_result%\r\n'
    original['SHA256SUMS.txt'] = _checksums(original)
    sdk['SHA256SUMS.txt'] = _checksums(sdk)
    output.mkdir(parents=True, exist_ok=True)
    result = {
        'version': VERSION,
        'status': 'standalone development SDK; game services unavailable',
        'source': _zip(output / f'NEONFORGE-Core-{VERSION}-Source.zip', f'NEONFORGE-Core-{VERSION}-Source', original),
        'sdk': _zip(output / f'NEONFORGE-Core-{VERSION}-Windows-x64-SDK.zip', f'NEONFORGE-Core-{VERSION}-SDK', sdk),
        'source_files': len(original) - 1,
        'sdk_files': len(sdk) - 1,
    }
    (output / 'SHA256SUMS.txt').write_text(''.join(result[key]['sha256'] + '  ' + result[key]['name'] + '\n' for key in ['source', 'sdk']), encoding='utf-8')
    (output / 'build-manifest.json').write_text(json.dumps(result, indent=2) + '\n', encoding='utf-8')
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument('--build-dir', type=Path, required=True)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    try:
        print(json.dumps(build_distributions(args.source_dir, args.build_dir, args.output_dir), indent=2))
        return 0
    except (OSError, PackageError) as error:
        print('NEONFORGE packaging: ' + str(error))
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
