"""Offline negative tests for the distribution's source/binary binding."""
from __future__ import annotations

import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
import stat
import subprocess
import unittest
import warnings
import zipfile
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('public_release', Path(__file__).resolve().parents[1] / 'tools/public_release.py')
release = importlib.util.module_from_spec(spec)
spec.loader.exec_module(release)


class ArchiveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.files = {name: b'source' for name in (
            'LICENSE', 'LICENSE-SCOPE.md', 'docs/BUILD.md', 'third_party/dependencies.lock.json',
            'src/component/foo_nekorox.vcxproj', 'tools/build-columns-ui.ps1',
            'src/shader-render/main.cpp', 'src/shader-render/frame_protocol.h',
            'src/shader-render/goden-shader-render.vcxproj', 'src/shader-render/COPYING.md',
            'LICENSES/NPGS-GPL-3.0.txt', 'LICENSES/NPGS-NOTICE.txt',
            'corresponding-sources/columns-ui/COPYING', 'corresponding-sources/spectrum/LICENSE')}

    def make(self, *, extra=None, tamper=False, duplicate=False, corresponding=True, symlink=False):
        files = dict(self.files)
        if not corresponding:
            files = {k: v for k, v in files.items() if not k.startswith('corresponding-sources/')}
        if extra: files[extra] = b'untrusted'
        rows = [dict(path=k, bytes=len(v), sha256=hashlib.sha256(v).hexdigest()) for k, v in sorted(files.items())]
        source_rows = [r for r in rows if not r['path'].startswith('corresponding-sources/')]
        fingerprint = hashlib.sha256(json.dumps(source_rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        trees = {}
        for name in ('columns-ui', 'spectrum'):
            dependency_rows = [r for r in rows if r['path'].startswith(release.DEPENDENCY_PREFIXES[name])]
            trees[name] = hashlib.sha256(json.dumps(dependency_rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        manifest = json.dumps({'schema': 'goden.release-manifest.v2', 'dependencySourceTrees': trees,
                               'buildIdentity': {'sourceContentSha256': fingerprint}}).encode()
        self.manifest = self.root / 'release.json'
        self.manifest.write_bytes(manifest)
        files['packaging/component-manifest.json'] = manifest
        rows.append(dict(path='packaging/component-manifest.json', bytes=len(manifest), sha256=hashlib.sha256(manifest).hexdigest()))
        value = dict(schema='goden.public-source-archive.v1', correspondingSources=corresponding, sourceContentSha256=fingerprint, files=rows)
        path = self.root / 'sources.zip'
        with zipfile.ZipFile(path, 'w') as out:
            for name, data in files.items():
                entry = zipfile.ZipInfo(name)
                if symlink and name == 'LICENSE':
                    entry.create_system = 3
                    entry.external_attr = (stat.S_IFLNK | 0o777) << 16
                out.writestr(entry, b'changed' if tamper and name == 'LICENSE' else data)
            out.writestr('SOURCE-MANIFEST.json', json.dumps(value))
            if duplicate:
                with warnings.catch_warnings():
                    warnings.simplefilter('ignore', UserWarning)
                    out.writestr('LICENSE', files['LICENSE'])
        return path

    def test_matching_release(self):
        release.verify_archive(self.make(), release_manifest=self.manifest)

    def test_old_release_rejected(self):
        path = self.make()
        self.manifest.write_text('{"another":"release"}', encoding='utf-8')
        with self.assertRaisesRegex(ValueError, 'different release'): release.verify_archive(path, release_manifest=self.manifest)

    def test_tampered_member(self):
        with self.assertRaisesRegex(ValueError, 'integrity mismatch'): release.verify_archive(self.make(tamper=True))

    def test_duplicate_member(self):
        with self.assertRaisesRegex(ValueError, 'Duplicate'): release.verify_archive(self.make(duplicate=True))

    def test_unsafe_paths(self):
        for name in ('../escape', '/absolute', 'C:/outside', 'nested/../escape', r'nested\escape',
                     'nested//escape', 'nested/./escape', 'CON.txt', 'nested/trailing.',
                     'nested/trailing ', 'AUX', 'com1.txt', 'file:stream', 'bad\x01name'):
            with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'Unsafe'): release.verify_archive(self.make(extra=name))

    def test_unmanifested_member(self):
        path = self.make()
        with zipfile.ZipFile(path, 'a') as out: out.writestr('unlisted', 'bad')
        with self.assertRaisesRegex(ValueError, 'Unmanifested'): release.verify_archive(path)

    def test_missing_corresponding_sources(self):
        del self.files['corresponding-sources/spectrum/LICENSE']
        with self.assertRaisesRegex(ValueError, 'Missing dependency'): release.verify_archive(self.make())

    def test_public_release_binding(self):
        path = self.make(corresponding=False)
        release.verify_archive(path, corresponding=False, release_manifest=self.manifest)
        self.manifest.write_bytes(b'{}')
        with self.assertRaisesRegex(ValueError, 'different release'):
            release.verify_archive(path, corresponding=False, release_manifest=self.manifest)

    def test_wrong_archive_role(self):
        for role in (True, False):
            with self.subTest(role=role), self.assertRaisesRegex(ValueError, 'role'):
                release.verify_archive(self.make(corresponding=role), corresponding=not role)

    def test_public_ignored_dependency_prefix(self):
        with self.assertRaisesRegex(ValueError, 'Unexpected corresponding'):
            release.verify_archive(self.make(corresponding=False, extra='corresponding-sources/columns-ui/hidden'), corresponding=False)

    def test_unknown_dependency_prefix(self):
        with self.assertRaisesRegex(ValueError, 'Unexpected corresponding'):
            release.verify_archive(self.make(extra='corresponding-sources/unreviewed/hidden'))

    def test_windows_case_alias(self):
        with self.assertRaisesRegex(ValueError, 'alias'):
            release.verify_archive(self.make(extra='license'))

    def test_symlink(self):
        with self.assertRaisesRegex(ValueError, 'regular files'):
            release.verify_archive(self.make(symlink=True))

    def test_file_directory_alias(self):
        for first, second in (('tree', 'tree/child'), ('Tree/a', 'tree/b')):
            self.files[first] = b'file'
            with self.subTest(first=first), self.assertRaisesRegex(ValueError, 'alias'):
                release.verify_archive(self.make(extra=second))
            del self.files[first]

    def test_missing_dependency_binding(self):
        path = self.make()
        # Rebuild a self-consistent container whose release manifest omits the binding.
        with zipfile.ZipFile(path) as archive:
            members = {name: archive.read(name) for name in archive.namelist()}
        manifest = json.loads(members['packaging/component-manifest.json'])
        manifest.pop('dependencySourceTrees')
        members['packaging/component-manifest.json'] = json.dumps(manifest).encode()
        value = json.loads(members['SOURCE-MANIFEST.json'])
        for row in value['files']:
            if row['path'] == 'packaging/component-manifest.json':
                row.update(bytes=len(members[row['path']]), sha256=hashlib.sha256(members[row['path']]).hexdigest())
        members['SOURCE-MANIFEST.json'] = json.dumps(value).encode()
        with zipfile.ZipFile(path, 'w') as archive:
            for name, data in members.items(): archive.writestr(name, data)
        with self.assertRaisesRegex(ValueError, 'both dependency'):
            release.verify_archive(path)


class SourceAndStagingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.scope = patch.object(release, 'ROOT', self.root)
        self.scope.start(); self.addCleanup(self.scope.stop)
        self.seed = self.root / 'packaging/clean-seed'
        self.seed.mkdir(parents=True)
        self.destination = self.root / 'stage'
        self.rows = []
        for name in ('first', 'last'):
            path = self.seed / name; path.write_bytes(name.encode())
            self.rows.append(release.record(path, name))
        self.manifest = {}
        for key, name in release.BINARIES.items():
            path = self.root / 'out/Release' / name
            path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(name.encode())
            self.manifest[key] = release.record(path, name)
        release.write_json(self.root / 'packaging/component-manifest.json', self.manifest)

    def stage(self):
        release.write_json(self.seed / 'seed-manifest.json', {'files': self.rows})
        release.staging(self.destination)

    def test_stage_success(self):
        self.stage()
        self.assertEqual((self.destination / 'profile/first').read_bytes(), b'first')

    def test_stage_traversal_preserves_outside(self):
        sentinel = self.root / 'packaging/sentinel'; sentinel.write_bytes(b'private')
        self.rows.append(release.record(sentinel, '../sentinel'))
        with self.assertRaisesRegex(ValueError, 'Unsafe'): self.stage()
        self.assertEqual(sentinel.read_bytes(), b'private')
        self.assertFalse(self.destination.exists())

    def test_bad_late_input_leaves_no_partial_tree(self):
        self.rows[-1]['sha256'] = '0' * 64
        with self.assertRaisesRegex(ValueError, 'mismatch'): self.stage()
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.root.glob('stage.stage-*')), [])

    def test_corrupt_copy_cleans_temporary(self):
        def corrupt(src, dst): Path(dst).write_bytes(b'corrupted')
        with patch.object(release.shutil, 'copyfile', corrupt), self.assertRaisesRegex(ValueError, 'changed during staging'):
            self.stage()
        self.assertFalse(self.destination.exists())
        self.assertEqual(list(self.root.glob('stage.stage-*')), [])

    def test_source_walk_excludes_links_and_private_state(self):
        tree = self.root / 'sources'; tree.mkdir()
        (tree / 'ordinary').write_bytes(b'public')
        (tree / '.env.local').write_bytes(b'secret')
        (tree / '.ENV.upper').write_bytes(b'secret')
        (tree / '.Dev.vars.local').write_bytes(b'secret')
        outside = self.root / 'private'; outside.write_bytes(b'outside')
        try:
            (tree / 'linked-file').symlink_to(outside)
            (tree / 'linked-dir').symlink_to(self.seed, target_is_directory=True)
        except OSError as error:
            self.skipTest('Symlink creation unavailable: ' + str(error.winerror))
        self.assertEqual([rel for _, rel in release.plain_sources(tree, 'source')], ['source/ordinary'])
        with self.assertRaisesRegex(ValueError, 'Linked'): release.contained_file(tree, 'linked-file')
        with self.assertRaisesRegex(ValueError, 'Linked'): list(release.plain_sources(tree / 'linked-dir', 'source'))
        with self.assertRaisesRegex(ValueError, 'Linked'): release.contained_file(tree / 'linked-dir', 'first')

    def test_dependency_cannot_be_resealed_after_source_edit(self):
        path = self.root / 'source'; path.write_bytes(b'before')
        before, _ = release.fingerprint([(path, 'source')])
        binary = self.root / 'out/Release/foo_ui_columns.dll'
        receipt = self.root / 'out/Release/columns-ui-build.json'
        release.write_json(receipt, {'sha256': release.digest(binary), 'sourceTreeSha256': before})
        path.write_bytes(b'after')
        with patch.object(release, 'dependency_sources', return_value=[(path, 'source')]), self.assertRaisesRegex(ValueError, 'pre-build snapshot'):
            release.seal_dependency('columns-ui')
        self.assertEqual(release.read_json(receipt)['sourceTreeSha256'], before)

    def test_native_cannot_be_sealed_after_source_edit(self):
        path = self.root / 'source'; path.write_bytes(b'before')
        snapshot = self.root / 'before.json'
        with patch.object(release, 'source_files', return_value=[(path, 'source')]):
            release.snapshot_build(snapshot)
            path.write_bytes(b'after')
            with self.assertRaisesRegex(ValueError, 'during native compilation'): release.seal_native(snapshot)
        self.assertFalse((self.root / 'out/Release/native-build-source.json').exists())

    def test_untracked_header_requires_explicit_corresponding_source(self):
        tree = self.root / 'checkout'; tree.mkdir()
        subprocess.run(['git', 'init', '-q', str(tree)], check=True)
        (tree / 'project.vcxproj').write_text('<ClInclude Include="added.h"/>')
        subprocess.run(['git', '-C', str(tree), 'add', 'project.vcxproj'], check=True)
        (tree / 'added.h').write_text('first version')
        with self.assertRaisesRegex(ValueError, 'Untracked build input'):
            list(release.git_sources(tree, 'source'))
        def inputs():
            yield from release.git_sources(tree, 'source', ('added.h',))
            yield release.contained_file(tree, 'added.h'), 'source/added.h'
        before = release.fingerprint(inputs())[0]
        (tree / 'added.h').write_text('second version')
        self.assertNotEqual(before, release.fingerprint(inputs())[0])

    def test_untracked_resource_is_refused(self):
        tree = self.root / 'resource-checkout'; tree.mkdir()
        subprocess.run(['git', 'init', '-q', str(tree)], check=True)
        (tree / 'project.vcxproj').write_text('<ResourceCompile Include="added.rc"/>')
        subprocess.run(['git', '-C', str(tree), 'add', 'project.vcxproj'], check=True)
        (tree / 'added.rc').write_text('1 ICON "icon.ico"')
        with self.assertRaisesRegex(ValueError, 'Untracked build input'):
            list(release.git_sources(tree, 'source'))

    def test_untracked_input_in_submodule_is_refused(self):
        tree = self.root / 'parent'; child = tree / 'child'; child.mkdir(parents=True)
        for base in (tree, child):
            subprocess.run(['git', 'init', '-q', str(base)], check=True)
        (child / 'tracked.h').write_text('tracked')
        subprocess.run(['git', '-C', str(child), 'add', 'tracked.h'], check=True)
        subprocess.run(['git', '-C', str(child), '-c', 'user.name=Fixture', '-c',
                        'user.email=fixture@example.invalid', 'commit', '-qm', 'fixture'], check=True)
        commit = subprocess.run(['git', '-C', str(child), 'rev-parse', 'HEAD'],
                                check=True, capture_output=True, text=True).stdout.strip()
        subprocess.run(['git', '-C', str(tree), 'update-index', '--add', '--cacheinfo',
                        '160000,' + commit + ',child'], check=True)
        self.assertEqual(len(list(release.git_sources(tree, 'source'))), 1)
        (child / 'added.h').write_text('untracked')
        with self.assertRaisesRegex(ValueError, 'Untracked build input'):
            list(release.git_sources(tree, 'source'))

    def test_archive_collision_preserves_existing_archive_and_checksum(self):
        a = self.root / 'a'; a.write_bytes(b'a')
        target = self.root / 'kept.zip'; target.write_bytes(b'archive sentinel')
        checksum = target.with_suffix('.sha256.txt'); checksum.write_bytes(b'checksum sentinel')
        for names in (('name', 'name'), ('Name', 'name'), ('Tree/a', 'tree/b'), ('node', 'node/a')):
            with self.subTest(names=names), patch.object(release, 'source_files',
                    return_value=[(a, name) for name in names]), self.assertRaisesRegex(ValueError, 'alias'):
                release.archive(target, False)
            self.assertEqual(target.read_bytes(), b'archive sentinel')
            self.assertEqual(checksum.read_bytes(), b'checksum sentinel')
        self.assertEqual(list(self.root.glob('*.archive-*')), [])

    def test_failed_archive_verification_publishes_nothing(self):
        a = self.root / 'a'; a.write_bytes(b'a')
        target = self.root / 'rejected.zip'
        with patch.object(release, 'source_files', return_value=[(a, 'ordinary')]), self.assertRaisesRegex(ValueError, 'incomplete'):
            release.archive(target, False)
        self.assertFalse(target.exists())
        self.assertFalse(target.with_suffix('.sha256.txt').exists())
        self.assertEqual(list(self.root.glob('*.archive-*')), [])

    def test_checksum_publication_failure_removes_new_archive(self):
        a = self.root / 'a'; a.write_bytes(b'a')
        target = self.root / 'rejected.zip'
        rename = Path.rename
        def fail_checksum(path, destination):
            if path.name.endswith('.sha256.txt'):
                raise OSError('injected checksum publication failure')
            return rename(path, destination)
        with patch.object(release, 'source_files', return_value=[(a, 'ordinary')]), \
                patch.object(release, 'verify_archive'), patch.object(Path, 'rename', fail_checksum), \
                self.assertRaisesRegex(OSError, 'injected'):
            release.archive(target, False)
        self.assertFalse(target.exists())
        self.assertFalse(target.with_suffix('.sha256.txt').exists())
        self.assertEqual(list(self.root.glob('*.archive-*')), [])


if __name__ == '__main__': unittest.main()
