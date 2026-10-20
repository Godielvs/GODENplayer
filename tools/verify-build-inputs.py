"""Offline, fail-closed verification of vendored inputs and the pinned patch series."""
from __future__ import annotations
import argparse
import hashlib
import json
import sys
if sys.version_info < (3, 11):
    raise SystemExit('Build verification requires Python 3.11 or newer; select the documented Python executable.')
from pathlib import Path
from public_release import ROOT, contained_file, digest, read_json


def verify(root: Path):
    lock = read_json(root / 'third_party/dependencies.lock.json')
    checked = 0
    for dependency in lock['dependencies']:
        if 'inventory' in dependency:
            inventory = read_json(contained_file(root, dependency['inventory']))['files']
            content = hashlib.sha256(json.dumps(inventory, sort_keys=True,
                separators=(',', ':')).encode()).hexdigest()
            if content != dependency['contentSha256']:
                raise ValueError('Dependency inventory identity mismatch: ' + dependency['name'])
            if len({row['path'].casefold() for row in inventory}) != len(inventory):
                raise ValueError('Dependency inventory path alias: ' + dependency['name'])
            for row in inventory:
                if digest(contained_file(root, row['path'])) != row['sha256']:
                    raise ValueError('Vendored input drift: ' + row['path'])
                checked += 1
        for row in dependency.get('patches', []) + dependency.get('files', []):
            if digest(contained_file(root, row['path'])) != row['sha256']:
                raise ValueError('Pinned dependency input drift: ' + row['path'])
            checked += 1
    return checked


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    args = parser.parse_args()
    print(f'Verified {verify(args.root)} pinned build input hashes; no network or compilation.')
