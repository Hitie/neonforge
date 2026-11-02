"""Verify pinned, unmodified embedded dependency source before distribution."""
import argparse
import hashlib
import json
from pathlib import Path


def verify(root):
    directory = Path(root) / 'third_party/lua'
    provenance = json.loads((directory / 'UPSTREAM.json').read_text())
    for name, expected in provenance['source_files_sha256'].items():
        path = directory / name
        if path.is_symlink() or not path.is_file() or hashlib.sha256(path.read_bytes()).hexdigest() != expected:
            raise ValueError('Lua source differs from the pinned archive: ' + name)
    expected = set(provenance['source_files_sha256'])
    actual = {path.name for path in directory.iterdir() if path.suffix in {'.c', '.h', '.hpp'}}
    if actual != expected:
        raise ValueError('Unexpected/missing Lua source file')
    if not (directory / 'LICENSE.txt').is_file():
        raise ValueError('Lua license missing')
    return len(expected)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-dir', type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args()
    print('Verified', verify(args.source_dir), 'pinned Lua source/header files')
