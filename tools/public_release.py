"""Local public-source export, source correspondence, and clean staging.

No Git history, host binaries, profiles, network access or publication. Run from
any directory with Python 3.11+. Generated outputs belong under out/.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import stat
import subprocess
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[1]
ROOT_FILES = {
    '.editorconfig', '.gitignore', 'AGENTS.md', 'CHANGELOG.md', 'CONTRIBUTING.md',
    'Directory.Build.props', 'LICENSE', 'LICENSE-SCOPE.md', 'NekoRoX.sln', 'README.md',
}
SOURCE_DIRS = {'.github', 'assets', 'build', 'config', 'docs', 'LICENSES',
               'packaging', 'services', 'src', 'tests', 'third_party', 'tools', 'vendor'}
PRUNE = {'.git', '.vs', '.wrangler', '__pycache__', 'node_modules', 'CMakeFiles',
         'Debug', 'Release', 'x64', 'dist', 'out', 'artifacts', 'vcpkg_installed'}
BINARY_SUFFIXES = {'.obj', '.pdb', '.ilk', '.exp', '.dll', '.exe', '.lib',
                   '.log', '.tlog', '.pyc', '.suo', '.user', '.lastbuildstate',
                   '.pch', '.idb', '.recipe'}
IMPORT_LIBRARY = 'vendor/foobar2000/shared/shared-x64.lib'
BINARIES = {
    'coreBinary': 'foo_nekorox.dll', 'analysisWorkerBinary': 'nekorox-analysis.exe',
    'crtRendererBinary': 'goden-crt-render.exe',
    'shaderRendererBinary': 'goden-shader-render.exe',
    'spectrumAnalyzerBinary': 'foo_vis_spectrum_analyzer.dll',
    'columnsUiBinary': 'foo_ui_columns.dll',
}
DEPENDENCY_TREES = {
    'columns-ui': ('out/dependencies/columns-ui', 'columns-ui-build.json', 'foo_ui_columns.dll'),
    'spectrum': ('out/third_party/spectrum-analyzer/foo_vis_spectrum_analyzer', 'spectrum-analyzer-build.json', 'foo_vis_spectrum_analyzer.dll'),
}
DEPENDENCY_PREFIXES = {
    'columns-ui': ('corresponding-sources/columns-ui/', 'corresponding-sources/vcpkg/'),
    'spectrum': ('corresponding-sources/spectrum/', 'corresponding-sources/spectrum-columns-sdk/',
                 'corresponding-sources/official-sdk/', 'corresponding-sources/WTL/'),
}
SPECTRUM_ADDITIONS = ('NekoThemeProvider.h', 'GodenDawVisualSource.h', 'NekoRenderSampling.h',
                     'NekoAxisText.h', 'NekoBarBatch.h', 'Visuals/Spectrum/NekoDoubleExposure.h',
                     'Visuals/Spectrum/NekoVisualPalette.h')


def digest(path: Path) -> str:
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def read_json(path: Path):
    return json.loads(path.read_text(encoding='utf-8-sig'))


def write_json(path: Path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')


def record(path: Path, relative: str):
    return {'path': relative, 'bytes': path.stat().st_size, 'sha256': digest(path)}


def linked(path: Path) -> bool:
    """Include Windows junctions/reparse points, not only POSIX symlinks."""
    info = path.lstat()
    return stat.S_ISLNK(info.st_mode) or bool(getattr(info, 'st_file_attributes', 0) &
                                            getattr(stat, 'FILE_ATTRIBUTE_REPARSE_POINT', 0x400))


def private_name(name: str) -> bool:
    name = name.casefold()
    return name == '.git' or name.startswith(('.env', '.dev.vars'))


def assert_unlinked(path: Path):
    # os.walk(followlinks=False) does not protect a linked starting directory,
    # nor an intermediate junction above it. Refuse every existing component.
    for part in (path.absolute(), *path.absolute().parents):
        if part.exists() and linked(part):
            raise ValueError('Linked source root or ancestor refused.')


def contained_file(base: Path, name: str) -> Path:
    assert_unlinked(base)
    validate_archive_path(name)
    path = base
    for part in name.split('/'):
        path = path / part
        if linked(path):
            raise ValueError('Linked source path refused: ' + name)
    if not path.resolve().is_relative_to(base.resolve()) or not path.is_file():
        raise ValueError('Source path escapes its root or is not a file: ' + name)
    return path


def source_files():
    assert_unlinked(ROOT)
    for name in sorted(ROOT_FILES):
        if not (ROOT / name).is_file() or linked(ROOT / name):
            raise ValueError(f'Missing public root file: {name}')
        yield ROOT / name, name
    for name in sorted(SOURCE_DIRS):
        base = ROOT / name
        if not base.exists():
            continue
        if linked(base):
            raise ValueError('Linked public source root refused: ' + name)
        for directory, dirs, files in os.walk(base, followlinks=False):
            dirs[:] = sorted(d for d in dirs if d not in PRUNE and
                             not d.startswith(('release-', 'debug-')) and
                             not private_name(d) and not linked(Path(directory) / d))
            for name in sorted(files):
                path = Path(directory) / name
                rel = path.relative_to(ROOT).as_posix()
                if linked(path) or private_name(name):
                    continue
                if name.endswith('.FileListAbsolute.txt'):
                    continue
                if path.suffix.lower() in BINARY_SUFFIXES and rel != IMPORT_LIBRARY:
                    continue
                yield path, rel


def fingerprint(files):
    rows = sorted([record(p, rel) for p, rel in files if rel != 'packaging/component-manifest.json'], key=lambda x: x['path'])
    value = hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
    return value, rows


def manifest():
    identity = (ROOT / 'src/component/build_identity.h').read_text(encoding='utf-8-sig')
    version = re.search(r'kVersion\[\]\s*=\s*"([^"]+)"', identity).group(1)
    source_hash, _ = fingerprint(source_files())
    native = read_json(ROOT / 'out/Release/native-build-source.json')
    if native.get('schema') != 'goden.native-build-source.v1' or native.get('qaMutationHooks') is not False or native.get('sourceContentSha256') != source_hash:
        raise ValueError('Native build receipt is missing or sources changed after compilation; rebuild first.')
    for key in ('coreBinary', 'analysisWorkerBinary', 'crtRendererBinary', 'shaderRendererBinary'):
        if native.get('binaries', {}).get(key) != record(ROOT / 'out/Release' / BINARIES[key], BINARIES[key]):
            raise ValueError('Native binary changed after compilation: ' + key)
    value = {
        'schema': 'goden.release-manifest.v2', 'component': 'foo_nekorox',
        'buildIdentity': {'version': version, 'source': 'src/component/build_identity.h',
                          'releaseState': 'local-candidate-not-published',
                          'sourceContentSha256': source_hash,
                          'toolchain': 'MSVC v142 / SDK 19041; Columns UI v143 / SDK 26100'},
        'architecture': 'x86_64', 'minimumWindows': '10.0.19041',
        'minimumFoobar2000': '2.25.10', 'columnsUi': '3.6.0',
        'hostDisplayContract': {'hostExecutablePatched': False,
                                'provider': 'Columns UI bounded PMv2 window context'},
        'qaMutationHooks': False,
        'nativeBuildSource': native,
        'rightsStatus': 'third-party shader permissions and approved B icon similarity/rights remain pending; see LICENSE-SCOPE.md',
        'dependencyLockSha256': digest(ROOT / 'third_party/dependencies.lock.json'),
    }
    for key, file in BINARIES.items():
        value[key] = record(ROOT / 'out/Release' / file, file)
    for name, file in [('spectrumBuild', 'spectrum-analyzer-build.json'),
                       ('columnsUiBuild', 'columns-ui-build.json')]:
        receipt = ROOT / 'out/Release' / file
        value[name] = {'path': file, 'sha256': digest(receipt)}
    value['dependencySourceTrees'] = {}
    for name, (base, receipt_name, binary) in DEPENDENCY_TREES.items():
        current, _ = fingerprint(dependency_sources(name))
        receipt = read_json(ROOT / 'out/Release' / receipt_name)
        if receipt.get('sourceTreeSha256') != current or receipt.get('qualifiedBinarySha256') != digest(ROOT / 'out/Release' / binary):
            raise ValueError('Dependency sources/binary changed since its build receipt: ' + name)
        value['dependencySourceTrees'][name] = current
    write_json(ROOT / 'packaging/component-manifest.json', value)
    print(f'Generated manifest for {version} from six actual binaries.')


def staging(destination: Path):
    if destination.exists():
        raise ValueError('Refusing to overwrite an existing staging directory.')
    release = read_json(ROOT / 'packaging/component-manifest.json')
    seed = ROOT / 'packaging/clean-seed'
    inputs = []
    aliases = set()
    for item in read_json(seed / 'seed-manifest.json')['files']:
        src = contained_file(seed, item['path'])
        if digest(src) != item['sha256'].lower() or src.stat().st_size != item['bytes']:
            raise ValueError('Clean seed mismatch: ' + item['path'])
        rel = 'profile/' + item['path']
        inputs.append((src, rel, item['sha256'].lower(), item['bytes']))
    components = {'coreBinary': 'foo_nekorox', 'analysisWorkerBinary': 'foo_nekorox',
                  'crtRendererBinary': 'foo_nekorox', 'shaderRendererBinary': 'foo_nekorox', 'columnsUiBinary': 'foo_ui_columns',
                  'spectrumAnalyzerBinary': 'foo_vis_spectrum_analyzer'}
    for key, folder in components.items():
        name = BINARIES[key]
        src = ROOT / 'out/Release' / name
        if linked(src) or digest(src) != release[key]['sha256'].lower() or src.stat().st_size != release[key]['bytes']:
            raise ValueError('Built binary changed since manifest: ' + name)
        rel = f'profile/user-components-x64/{folder}/{name}'
        inputs.append((src, rel, release[key]['sha256'].lower(), release[key]['bytes']))
    for _, rel, _, _ in inputs:
        key = rel.casefold()
        if key in aliases or any(key.startswith(other + '/') or other.startswith(key + '/') for other in aliases):
            raise ValueError('Staging path alias: ' + rel)
        aliases.add(key)
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=destination.name + '.stage-', dir=destination.parent))
    try:
        files = []
        for src, rel, expected, size in inputs:
            dst = temporary / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(src, dst)
            row = record(dst, rel)
            if row['sha256'] != expected or row['bytes'] != size:
                raise ValueError('Staged copy changed during staging: ' + rel)
            files.append(row)
        write_json(temporary / 'staging-manifest.json', {
            'schema': 'goden.friend-staging.v1', 'cleanProfile': True,
            'nativeColumnsAccepted': True, 'seedManifestSha256': digest(seed / 'seed-manifest.json'),
            'files': files})
        # Windows rename refuses an existing destination. Never replace a
        # concurrently published staging tree, even an empty one.
        if destination.exists():
            raise ValueError('Staging destination appeared during preparation.')
        temporary.rename(destination)
    finally:
        if temporary.exists():
            shutil.rmtree(temporary)
    print('Staged seven clean settings and six freshly built components.')


def git_sources(base: Path, prefix: str, allowed_untracked=()):
    assert_unlinked(base)
    if not (base / '.git').exists():
        raise ValueError('Missing corresponding source checkout: ' + prefix)
    untracked = subprocess.run(['git', '-C', str(base), 'ls-files', '--others', '--exclude-standard', '-z'],
                               capture_output=True, check=True).stdout.decode('utf-8').split('\0')
    for name in untracked:
        if name and name not in allowed_untracked:
            raise ValueError('Untracked build input is not in the explicit source set: ' + name)
    result = subprocess.run(['git', '-C', str(base), 'ls-files', '--stage', '-z'],
                            capture_output=True, check=True).stdout
    for entry in result.decode('utf-8').split('\0'):
        if not entry:
            continue
        metadata, name = entry.split('\t', 1)
        mode, _, stage = metadata.split()
        if stage != '0':
            raise ValueError('Unmerged corresponding source: ' + name)
        if mode == '160000':
            yield from git_sources(base / name, prefix + '/' + name)
            continue
        path = base / name
        if any(private_name(part) for part in Path(name).parts):
            continue
        if path.is_file() and not any(linked(base.joinpath(*Path(name).parts[:i])) for i in range(1, len(Path(name).parts) + 1)):
            yield path, f'{prefix}/{name}'


def dependency_sources(name: str):
    base = ROOT / DEPENDENCY_TREES[name][0]
    tracked = list(git_sources(base, 'corresponding-sources/' + name,
                               SPECTRUM_ADDITIONS if name == 'spectrum' else ()))
    yield from tracked
    if name == 'spectrum':
        included = {rel for _, rel in tracked}
        for rel in SPECTRUM_ADDITIONS:
            archive_rel = 'corresponding-sources/spectrum/' + rel
            if archive_rel not in included:
                yield contained_file(base, rel), archive_rel
        yield contained_file(base.parent, 'Directory.Build.targets'), 'corresponding-sources/spectrum/effective/Directory.Build.targets'
        yield from git_sources(ROOT / 'out/third_party/spectrum-analyzer/columns_ui_sdk-source',
                               'corresponding-sources/spectrum-columns-sdk')
        yield from plain_sources(ROOT / 'out/dependencies/foobar2000-sdk/SDK-2025-03-07',
                                 'corresponding-sources/official-sdk')
        yield from plain_sources(ROOT / 'out/third_party/spectrum-analyzer/WTL10_10320',
                                 'corresponding-sources/WTL')
    else:
        blds = base / 'vcpkg_installed/x64-windows-static/vcpkg/blds'
        for package in ('cppwinrt', 'fmt', 'foonathan-lexy', 'ms-gsl', 'range-v3', 'wil'):
            candidates = list((blds / package / 'src').glob('*.clean'))
            if len(candidates) != 1:
                raise ValueError('Expected exactly one qualified vcpkg source: ' + package)
            yield from plain_sources(candidates[0], f'corresponding-sources/vcpkg/{package}')


def seal_dependency(name: str):
    base, receipt_name, binary = DEPENDENCY_TREES[name]
    receipt_path = ROOT / 'out/Release' / receipt_name
    value = read_json(receipt_path)
    expected = value.get('sha256') if name == 'columns-ui' else value['hashes']['dll']
    binary_hash = digest(ROOT / 'out/Release' / binary)
    if expected.lower() != binary_hash:
        raise ValueError('Build receipt does not match the produced dependency binary.')
    current, _ = fingerprint(dependency_sources(name))
    if value.get('sourceTreeSha256') != current:
        raise ValueError('Dependency sources changed after the pre-build snapshot: ' + name)
    value['qualifiedBinarySha256'] = binary_hash
    write_json(receipt_path, value)


def snapshot_build(destination: Path):
    value, _ = fingerprint(source_files())
    write_json(destination, {'sourceContentSha256': value})


def seal_native(snapshot: Path):
    before = read_json(snapshot)['sourceContentSha256']
    after, _ = fingerprint(source_files())
    if before != after:
        raise ValueError('Sources changed during native compilation; no build receipt was published.')
    write_json(ROOT / 'out/Release/native-build-source.json', {
        'schema': 'goden.native-build-source.v1', 'sourceContentSha256': after,
        'qaMutationHooks': False,
        'binaries': {key: record(ROOT / 'out/Release' / BINARIES[key], BINARIES[key])
                     for key in ('coreBinary', 'analysisWorkerBinary', 'crtRendererBinary', 'shaderRendererBinary')}})


def plain_sources(base: Path, prefix: str):
    assert_unlinked(base)
    if not base.is_dir() or linked(base):
        raise ValueError('Missing corresponding sources: ' + prefix)
    for directory, dirs, files in os.walk(base, followlinks=False):
        dirs[:] = sorted(d for d in dirs if d not in PRUNE and not private_name(d) and
                         not linked(Path(directory) / d))
        for name in sorted(files):
            path = Path(directory) / name
            if linked(path) or path.suffix.lower() in BINARY_SUFFIXES or private_name(name):
                continue
            yield path, prefix + '/' + path.relative_to(base).as_posix()


def archive(destination: Path, corresponding: bool):
    files = list(source_files())
    source_hash, _ = fingerprint(files)
    if corresponding:
        release = read_json(ROOT / 'packaging/component-manifest.json')
        if release['buildIdentity']['sourceContentSha256'] != source_hash:
            raise ValueError('Source changed since manifest; regenerate it after the final build.')
        for name in DEPENDENCY_TREES:
            files += list(dependency_sources(name))
    aliases, directories = set(), {}
    for rel in [name for _, name in files] + ['SOURCE-MANIFEST.json']:
        validate_archive_path(rel)
        alias = rel.casefold()
        if alias in aliases:
            raise ValueError('Duplicate Windows archive path alias: ' + rel)
        aliases.add(alias)
        parts = rel.split('/')
        for end in range(1, len(parts)):
            parent = '/'.join(parts[:end])
            key = parent.casefold()
            if key in directories and directories[key] != parent:
                raise ValueError('Conflicting Windows archive path alias: ' + rel)
            directories[key] = parent
    if aliases.intersection(directories):
        raise ValueError('Conflicting Windows archive file/directory alias.')
    checksum = destination.with_suffix('.sha256.txt')
    if destination.exists() or checksum.exists():
        raise ValueError('Refusing to overwrite an existing source archive or checksum.')
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=destination.name + '.archive-', dir=destination.parent))
    candidate = temporary / destination.name
    try:
        rows = write_archive(candidate, files, source_hash, corresponding)
        verify_archive(candidate, corresponding)
        sha256 = digest(candidate)
        candidate_checksum = temporary / checksum.name
        candidate_checksum.write_text(sha256 + '  ' + destination.name + '\n', encoding='ascii')
        if destination.exists() or checksum.exists():
            raise ValueError('Archive destination appeared during preparation.')
        candidate.rename(destination)
        try:
            candidate_checksum.rename(checksum)
        except BaseException:
            destination.unlink()
            raise
    finally:
        shutil.rmtree(temporary)
    print(f'Verified {len(rows)} archived files; SHA256 {sha256}')


def write_archive(destination: Path, files, source_hash: str, corresponding: bool):
    rows = []
    with zipfile.ZipFile(destination, 'w', zipfile.ZIP_DEFLATED, compresslevel=6) as out:
        for path, rel in sorted(files, key=lambda x: x[1]):
            data = path.read_bytes()
            # Deterministic container metadata. Preserve content exactly.
            info = zipfile.ZipInfo(rel, (2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            out.writestr(info, data)
            rows.append({'path': rel, 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest()})
        payload = {'schema': 'goden.public-source-archive.v1',
                   'sourceContentSha256': source_hash, 'correspondingSources': corresponding,
                   'files': rows}
        out.writestr('SOURCE-MANIFEST.json', json.dumps(payload, indent=2) + '\n')
    return rows


def verify_archive(path: Path, corresponding=True, release_manifest: Path | None = None):
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) != len({i.filename for i in entries}):
            raise ValueError('Duplicate archive member.')
        if len(entries) > 100000 or sum(i.file_size for i in entries) > 2 * 1024**3:
            raise ValueError('Source archive exceeds the qualification limits.')
        aliases = set()
        for entry in entries:
            validate_archive_path(entry.filename)
            alias = entry.filename.casefold()
            if alias in aliases:
                raise ValueError('Duplicate Windows archive path alias.')
            aliases.add(alias)
            kind = stat.S_IFMT(entry.external_attr >> 16)
            if entry.is_dir() or kind not in (0, stat.S_IFREG):
                raise ValueError('Archive members must be regular files.')
        directories = {}
        for entry in entries:
            parts = entry.filename.split('/')
            for end in range(1, len(parts)):
                parent = '/'.join(parts[:end])
                alias = parent.casefold()
                if alias in aliases or (alias in directories and directories[alias] != parent):
                    raise ValueError('Conflicting Windows archive path alias.')
                directories[alias] = parent
        value = json.loads(archive.read('SOURCE-MANIFEST.json'))
        if value.get('schema') != 'goden.public-source-archive.v1':
            raise ValueError('Unsupported source archive schema.')
        if type(value.get('correspondingSources')) is not bool or value['correspondingSources'] != corresponding:
            raise ValueError('Archive role does not match the requested source archive.')
        if not isinstance(value.get('files'), list):
            raise ValueError('Source archive file manifest is missing.')
        required = {'LICENSE', 'LICENSE-SCOPE.md', 'docs/BUILD.md', 'third_party/dependencies.lock.json',
                    'src/component/foo_nekorox.vcxproj', 'tools/build-columns-ui.ps1',
                    'src/shader-render/main.cpp', 'src/shader-render/frame_protocol.h',
                    'src/shader-render/goden-shader-render.vcxproj', 'src/shader-render/COPYING.md',
                    'LICENSES/NPGS-GPL-3.0.txt', 'LICENSES/NPGS-NOTICE.txt'}
        names = set(archive.namelist())
        if not required <= names:
            raise ValueError('Source archive is incomplete: ' + str(required - names))
        if corresponding:
            for name in ('corresponding-sources/columns-ui/COPYING',
                         'corresponding-sources/spectrum/LICENSE'):
                if name not in names:
                    raise ValueError('Missing dependency sources: ' + name)
        manifested = set()
        for item in value['files']:
            if not isinstance(item, dict) or set(item) != {'path', 'bytes', 'sha256'}:
                raise ValueError('Invalid source archive file record.')
            name = item['path']
            validate_archive_path(name)
            if name in manifested or name == 'SOURCE-MANIFEST.json':
                raise ValueError('Duplicate source manifest record.')
            manifested.add(name)
            if type(item['bytes']) is not int or item['bytes'] < 0 or not isinstance(item['sha256'], str) or not re.fullmatch('[0-9a-f]{64}', item['sha256']):
                raise ValueError('Invalid source archive size or digest.')
            if name.startswith('corresponding-sources/'):
                allowed = ('columns-ui/', 'spectrum/', 'spectrum-columns-sdk/', 'official-sdk/', 'WTL/',
                           'vcpkg/cppwinrt/', 'vcpkg/fmt/', 'vcpkg/foonathan-lexy/',
                           'vcpkg/ms-gsl/', 'vcpkg/range-v3/', 'vcpkg/wil/')
                if not corresponding or not name[len('corresponding-sources/'):].startswith(allowed):
                    raise ValueError('Unexpected corresponding source path.')
            data = archive.read(name)
            if len(data) != item['bytes'] or hashlib.sha256(data).hexdigest() != item['sha256']:
                raise ValueError('Source archive integrity mismatch: ' + name)
        if names != {i['path'] for i in value['files']} | {'SOURCE-MANIFEST.json'}:
            raise ValueError('Unmanifested archive content.')
        rows = sorted([i for i in value['files'] if not i['path'].startswith('corresponding-sources/') and
                       i['path'] != 'packaging/component-manifest.json'], key=lambda x: x['path'])
        actual = hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        if actual != value['sourceContentSha256']:
            raise ValueError('Archive source fingerprint mismatch.')
        release = json.loads(archive.read('packaging/component-manifest.json'))
        if release.get('schema') != 'goden.release-manifest.v2':
            raise ValueError('Unsupported embedded release manifest schema.')
        trees = release.get('dependencySourceTrees')
        if (not isinstance(trees, dict) or set(trees) != set(DEPENDENCY_TREES) or
                any(not isinstance(v, str) or not re.fullmatch('[0-9a-f]{64}', v) for v in trees.values())):
            raise ValueError('Release manifest must bind both dependency source trees.')
        if release['buildIdentity']['sourceContentSha256'] != actual:
            raise ValueError('Archive release manifest does not match its sources.')
        if release_manifest is not None and archive.read('packaging/component-manifest.json') != release_manifest.read_bytes():
            raise ValueError('Sources belong to a different release manifest.')
        if corresponding:
            for name, expected in trees.items():
                tree = sorted([r for r in value['files'] if r['path'].startswith(DEPENDENCY_PREFIXES[name])], key=lambda r: r['path'])
                actual_tree = hashlib.sha256(json.dumps(tree, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
                if not tree or actual_tree != expected:
                    raise ValueError('Corresponding dependency sources differ from the build receipt: ' + name)
        return value


def validate_archive_path(name):
    """Accept portable regular-file paths with no Windows extraction aliases."""
    if not isinstance(name, str) or not name or '\\' in name or any(ord(c) < 32 for c in name):
        raise ValueError('Unsafe archive path.')
    for part in name.split('/'):
        stem = part.split('.')[0].upper()
        if (not part or part in ('.', '..') or part.endswith(('.', ' ')) or
                any(c in part for c in ':<>"|?*') or
                stem in {'CON', 'PRN', 'AUX', 'NUL', 'CONIN$', 'CONOUT$'} or
                re.fullmatch(r'(COM|LPT)[1-9\u00b9\u00b2\u00b3]', stem)):
            raise ValueError('Unsafe archive path.')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('manifest')
    p = sub.add_parser('snapshot-build'); p.add_argument('destination', type=Path)
    p = sub.add_parser('seal-native'); p.add_argument('snapshot', type=Path)
    p = sub.add_parser('dependency-fingerprint'); p.add_argument('name', choices=DEPENDENCY_TREES)
    p = sub.add_parser('seal-dependency'); p.add_argument('name', choices=DEPENDENCY_TREES)
    p = sub.add_parser('stage'); p.add_argument('destination', type=Path)
    p = sub.add_parser('archive'); p.add_argument('destination', type=Path); p.add_argument('--corresponding', action='store_true')
    p = sub.add_parser('verify'); p.add_argument('archive', type=Path); p.add_argument('--release-manifest', type=Path); p.add_argument('--public', action='store_true')
    args = parser.parse_args()
    if args.command == 'manifest': manifest()
    elif args.command == 'snapshot-build': snapshot_build(args.destination)
    elif args.command == 'seal-native': seal_native(args.snapshot)
    elif args.command == 'dependency-fingerprint':
        print(fingerprint(dependency_sources(args.name))[0])
    elif args.command == 'seal-dependency': seal_dependency(args.name)
    elif args.command == 'stage': staging(args.destination.resolve())
    elif args.command == 'archive': archive(args.destination.resolve(), args.corresponding)
    elif args.command == 'verify': verify_archive(args.archive.resolve(), corresponding=not args.public, release_manifest=args.release_manifest)
