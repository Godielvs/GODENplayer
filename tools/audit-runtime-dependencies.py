"""Inspect PE imports without loading DLLs or consulting this PC's installed CRT.

Build/qualification tool only. An import must resolve in the delivered host or
to an explicit Windows 10 system/API-set contract. This cannot qualify an OS,
driver, decoder or CPU by itself. Media Foundation is an optional worker-only
Windows feature, and is reported separately for Windows N installations.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


WINDOWS_DLLS = set('''advapi32 avrt bcrypt cabinet cfgmgr32 comctl32 comdlg32
crypt32 cryptbase cryptnet d2d1 d3d11 d3dcompiler_47 dbghelp dcomp dinput8 dnsapi dsound
dwmapi dwrite dxgi dxguid gdi32 gdiplus hid imagehlp imm32 iphlpapi kernel32 kernelbase
mf mfcore mfplat mfreadwrite mfuuid mmdevapi msacm32 mscms msimg32 msvcrt ncrypt netapi32
normaliz ntdll ole32 oleacc oleaut32 opengl32 powrprof propsys psapi rpcrt4
secur32 setupapi shcore shell32 shlwapi sspicli user32 userenv usp10 uxtheme
urlmon version windowscodecs winhttp wininet winmm winspool wintrust wldap32 ws2_32
wtsapi32'''.split())
WINDOWS_DLLS = {name + '.dll' for name in WINDOWS_DLLS}
MEDIA_FOUNDATION = {'mf.dll', 'mfplat.dll', 'mfreadwrite.dll', 'mfcore.dll'}
# Reviewed contracts used by the pinned x64 payload, available at the 19041
# baseline. Prefix matching alone would incorrectly accept invented/new APIs.
WINDOWS_API_SETS = {
    'api-ms-win-core-path-l1-1-0.dll', 'api-ms-win-shcore-scaling-l1-1-1.dll',
    *('api-ms-win-crt-' + name + '-l1-1-0.dll' for name in (
        'convert', 'environment', 'filesystem', 'heap', 'locale', 'math',
        'runtime', 'stdio', 'string', 'time', 'utility')),
}
MANAGED = ('foo_nekorox.dll', 'nekorox-analysis.exe', 'goden-crt-render.exe', 'goden-shader-render.exe',
           'foo_vis_spectrum_analyzer.dll', 'foo_ui_columns.dll')


def pe_imports(path: Path):
    data = path.read_bytes()

    def read(fmt, offset):
        size = struct.calcsize(fmt)
        if offset < 0 or offset + size > len(data):
            raise ValueError('Truncated PE: ' + path.name)
        return struct.unpack_from(fmt, data, offset)

    if data[:2] != b'MZ':
        raise ValueError('Not a PE image: ' + path.name)
    header = read('<I', 60)[0]
    if data[header:header + 4] != b'PE\0\0':
        raise ValueError('Invalid PE signature: ' + path.name)
    machine, count = read('<HH', header + 4)
    optional_size = read('<H', header + 20)[0]
    optional = header + 24
    if machine != 0x8664 or read('<H', optional)[0] != 0x20b or count > 96:
        raise ValueError('Expected x64 PE32+ image: ' + path.name)
    directories = read('<I', optional + 108)[0]
    if optional_size < 112 or optional_size < 112 + min(directories, 16) * 8:
        raise ValueError('Invalid PE data directories: ' + path.name)
    image_base = read('<Q', optional + 24)[0]
    headers_size = read('<I', optional + 60)[0]
    sections = []
    for index in range(count):
        section = optional + optional_size + index * 40
        virtual_size, address, raw_size, raw_offset = read('<IIII', section + 8)
        sections.append((address, virtual_size, raw_size, raw_offset))

    def offset(rva, size=1):
        if 0 <= rva < headers_size and rva + size <= min(headers_size, len(data)):
            return rva
        for address, virtual_size, raw_size, raw_offset in sections:
            delta = rva - address
            if 0 <= delta < max(virtual_size, raw_size):
                if delta + size > raw_size or raw_offset + delta + size > len(data):
                    break
                return raw_offset + delta
        raise ValueError('Unmapped PE import RVA: ' + path.name)

    result = set()
    for directory, stride, name_at in ((1, 20, 12), (13, 32, 4)):
        if directories <= directory:
            continue
        rva, size = read('<II', optional + 112 + directory * 8)
        if rva == 0 and size == 0:
            continue
        if rva == 0 or size < stride:
            raise ValueError('Invalid PE import directory: ' + path.name)
        for index in range(min(size // stride, 4096)):
            entry = offset(rva + index * stride, stride)
            if not any(data[entry:entry + stride]):
                break
            name = read('<I', entry + name_at)[0]
            if directory == 13 and not (read('<I', entry)[0] & 1):
                name -= image_base
            start = offset(name)
            end = data.find(b'\0', start, min(start + 256, len(data)))
            if end < 0:
                raise ValueError('Unterminated PE DLL name: ' + path.name)
            text = data[start:end].decode('ascii').lower()
            if not re.fullmatch(r'[a-z0-9_.-]+\.dll', text):
                raise ValueError('Unsafe PE DLL name: ' + path.name)
            result.add(text)
        else:
            raise ValueError('Unterminated PE import table: ' + path.name)
    return sorted(result)


def audit(host: Path, managed: list[Path]):
    # Host's activation context provides its runtime assembly. Never satisfy a
    # missing MSVC DLL with one that happens to be installed in System32 here.
    locations = (host, host / 'runtime')
    bundled = {}
    for location in locations:
        if location.is_dir():
            for path in location.iterdir():
                if path.is_file() and path.suffix.lower() == '.dll':
                    bundled.setdefault(path.name.lower(), path)
    pending = [host / 'foobar2000.exe', *managed]
    seen = set()
    rows = []
    missing = []
    while pending:
        path = pending.pop()
        identity = path.resolve()
        if identity in seen:
            continue
        seen.add(identity)
        imports = []
        for name in pe_imports(path):
            # Managed inputs may live in a developer Release directory, whose
            # unrelated/stale DLLs are not delivered. Only the official host's
            # enumerated runtime is an import resolution source.
            local = bundled.get(name)
            if local is not None and local.is_file():
                resolution = 'bundled'
                pending.append(local)
            elif name in MEDIA_FOUNDATION:
                resolution = 'Windows Media Foundation (optional worker feature)'
                if path.name.lower() != 'nekorox-analysis.exe':
                    missing.append({'image': path.name, 'import': name,
                                    'reason': 'Media Foundation escaped the optional worker'})
            elif name in WINDOWS_DLLS or name in WINDOWS_API_SETS:
                resolution = 'Windows system/API-set contract'
            else:
                resolution = 'unresolved'
                missing.append({'image': path.name, 'import': name})
            imports.append({'name': name, 'resolution': resolution})
        rows.append({'image': path.name, 'sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
                     'imports': imports})
    return {'schema': 'goden.runtime-import-audit.v1', 'passed': not missing,
            'images': sorted(rows, key=lambda row: row['image'].lower()), 'missing': missing,
            'runtimeTested': False, 'consultedInstalledRedistributables': False}


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host', type=Path, required=True)
    parser.add_argument('--managed', type=Path, required=True, help='Flat Release output containing all six managed binaries')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    report = audit(args.host, [args.managed / name for name in MANAGED])
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + '\n', encoding='utf-8')
    print(json.dumps({'passed': report['passed'], 'images': len(report['images']), 'missing': report['missing']}))
    raise SystemExit(0 if report['passed'] else 1)
