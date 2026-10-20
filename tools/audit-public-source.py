"""Inspect the exact export allowlist; never print matched private values.

Pass optional private markers through --forbid-file OUTSIDE the public tree,
one UTF-8 marker per line. The report contains only paths/rule identifiers.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import re
import public_release


def private_default_rules(relative, data):
    """Focused default-value audit, not a general secret scanner.

    Explicit test arguments and generic OS/application locations are allowed.
    Never return matched values, which may contain personal library names.
    """
    if not relative.endswith('.ps1'):
        return []
    text = data.decode('utf-8-sig', errors='replace')
    media = r'(?:Track|FirstTrack|SecondTrack|FixtureAlbum|MediaRoot)'
    absolute_default = rf'(?im)^\s*(?:\[[^\]\r\n]+\]\s*)*\${media}\s*=\s*[\x22\x27][A-Za-z]:[\\/]'
    # Compact param(...) scripts may put multiple declarations on one line.
    compact_default = rf'(?i)\[string\]\s*\${media}\s*=\s*[\x22\x27][A-Za-z]:[\\/]'
    desktop_fallback = r'(?i)Join-Path\s+\$env:USERPROFILE\s+[\x22\x27]Desktop[\\/]'
    findings = []
    if re.search(absolute_default, text) or re.search(compact_default, text):
        findings.append('personal-media-default')
    if re.search(desktop_fallback, text):
        findings.append('personal-desktop-fallback')
    return findings


def audit(markers):
    failures = []
    count = 0
    rows = []
    rules = [('private-key', re.compile(rb'-----BEGIN (?:RSA |EC |DSA |OPENSSH |ENCRYPTED )?PRIVATE KEY-----'))]
    for path, relative in public_release.source_files():
        count += 1
        data = path.read_bytes()
        if relative != 'packaging/component-manifest.json':
            rows.append(dict(path=relative, bytes=len(data), sha256=hashlib.sha256(data).hexdigest()))
        failures.extend(dict(path=relative, rule=name) for name in private_default_rules(relative, data))
        for name, pattern in rules:
            if pattern.search(data): failures.append(dict(path=relative, rule=name))
        for index, marker in enumerate(markers):
            if any(marker.encode(encoding) in data for encoding in ('utf-8', 'utf-16-le')):
                failures.append(dict(path=relative, rule=f'private-marker-{index + 1}'))
        if '.git' in Path(relative).parts or path.name.startswith(('.env', '.dev.vars')):
            failures.append(dict(path=relative, rule='private-state'))
    fingerprint = hashlib.sha256(json.dumps(sorted(rows, key=lambda r: r['path']), sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return dict(schema='goden.public-source-audit.v1', sourceContentSha256=fingerprint,
                files=count, passed=not failures, failures=failures)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--forbid-file', type=Path)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    markers = []
    if args.forbid_file:
        path = args.forbid_file.resolve()
        if path.is_relative_to(public_release.ROOT.resolve()):
            parser.error('Private markers must remain outside the public source tree.')
        markers = [line for line in path.read_text(encoding='utf-8-sig').splitlines() if line]
    value = audit(markers)
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print(f"Public export audit: {value['files']} files, {len(value['failures'])} findings.")
    raise SystemExit(0 if value['passed'] else 1)
