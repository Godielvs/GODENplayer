"""Offline adversarial tests of exact-candidate delivery; no installer execution."""
from __future__ import annotations
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock
import zipfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'tools'))
import public_release as release
spec = importlib.util.spec_from_file_location('finalizer', Path(__file__).resolve().parents[1] / 'tools/finalize-public-delivery.py')
finalizer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(finalizer)
audit_spec = importlib.util.spec_from_file_location('audit', Path(__file__).resolve().parents[1] / 'tools/audit-public-source.py')
audit = importlib.util.module_from_spec(audit_spec)
audit_spec.loader.exec_module(audit)


class DeliveryTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.inputs = self.root / 'inputs'
        self.inputs.mkdir()
        self.manifest = self.root / 'release.json'
        self.transaction = self.root / 'transaction.json'
        self.output = self.root / 'delivery'
        self.export = self.root / 'export'
        self.make()

    def make(self, public_extra=None):
        files = {name: b'source' for name in (
            'LICENSE', 'LICENSE-SCOPE.md', 'docs/BUILD.md', 'third_party/dependencies.lock.json',
            'src/component/foo_nekorox.vcxproj', 'tools/build-columns-ui.ps1',
            'src/shader-render/main.cpp', 'src/shader-render/frame_protocol.h',
            'src/shader-render/goden-shader-render.vcxproj', 'src/shader-render/COPYING.md',
            'LICENSES/NPGS-GPL-3.0.txt', 'LICENSES/NPGS-NOTICE.txt')}
        rows = [dict(path=k, bytes=len(v), sha256=hashlib.sha256(v).hexdigest()) for k, v in sorted(files.items())]
        source_hash = hashlib.sha256(json.dumps(rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        trees = {}
        for name, license_name in (('columns-ui', 'COPYING'), ('spectrum', 'LICENSE')):
            dependency_rows = [dict(path=f'corresponding-sources/{name}/{license_name}', bytes=7,
                                    sha256=hashlib.sha256(b'license').hexdigest())]
            trees[name] = hashlib.sha256(json.dumps(dependency_rows, sort_keys=True, separators=(',', ':')).encode()).hexdigest()
        manifest = dict(schema='goden.release-manifest.v2', dependencySourceTrees=trees,
                        buildIdentity=dict(version='1.0-test', sourceContentSha256=source_hash),
                        qaMutationHooks=False, **{key: dict(sha256='a'*64) for key in release.BINARIES})
        release.write_json(self.manifest, manifest)
        files['packaging/component-manifest.json'] = self.manifest.read_bytes()
        for role, name in ((False, finalizer.PUBLIC), (True, finalizer.CORRESPONDING)):
            members = dict(files)
            if role:
                members.update({'corresponding-sources/columns-ui/COPYING': b'license',
                                'corresponding-sources/spectrum/LICENSE': b'license'})
            if not role and public_extra:
                members[public_extra] = b'untrusted'
            rows = [dict(path=k, bytes=len(v), sha256=hashlib.sha256(v).hexdigest()) for k, v in sorted(members.items())]
            with zipfile.ZipFile(self.inputs / name, 'w') as archive:
                for k, v in members.items(): archive.writestr(k, v)
                archive.writestr('SOURCE-MANIFEST.json', json.dumps(dict(schema='goden.public-source-archive.v1',
                    correspondingSources=role, sourceContentSha256=source_hash, files=rows)))
        setup = self.inputs / finalizer.SETUP
        setup.write_bytes(b'NOT AN EXECUTABLE - offline fixture')
        build = dict(schema='goden.friend-installer-build.v3', version='1.0-test', setup=finalizer.SETUP,
                     setupSha256=release.digest(setup), setupBytes=setup.stat().st_size,
                     releaseManifestSha256=release.digest(self.manifest),
                     correspondingSourcesSha256=release.digest(self.inputs / finalizer.CORRESPONDING),
                     installedHostExpectedSha256='b'*64, officialInstallerEmbeddedIntact=True,
                     officialHostNotEmbedded=True, hostExecutablePatched=False,
                     embeddedComponents={key: 'a'*64 for key in finalizer.COMPONENTS})
        release.write_json(self.inputs / finalizer.BUILD, build)
        transaction = dict(schema='goden.installer-transaction-qa.v2', passed=True, runtimeUiSkipped=True,
                           setupSha256=build['setupSha256'], officialHostSourceSha256='b'*64, installedHostSha256='b'*64,
                           **{key: True for key in (*finalizer.FILE_GATES, *finalizer.HEADLESS_GATES)},
                           **{key: None for key in finalizer.UI_GATES})
        release.write_json(self.transaction, transaction)

    def run_delivery(self, observations=None):
        return finalizer.finalize(self.inputs, self.manifest, self.transaction, self.output, self.export, observations)

    def edit(self, path, function):
        value = release.read_json(path)
        function(value)
        release.write_json(path, value)

    def refuse(self):
        with self.assertRaises((ValueError, KeyError)):
            self.run_delivery()
        self.assertFalse(self.output.exists())
        self.assertFalse(self.export.exists())
        self.assertFalse(list(self.root.glob('.goden-finalize-*')))

    def test_matching_candidate_and_sanitized_failure(self):
        dsp = self.root / 'dsp.json'
        release.write_json(dsp, dict(passed=False, commonChainP99Milliseconds=0.4471,
                                    personalPath='SECRET PRIVATE VALUE', rawLog='secret'))
        result = self.run_delivery({'dsp': dsp})
        self.assertIsNone(result['installerTransaction']['runtimeWindowPerMonitorV2'])
        self.assertIs(result['observations']['dsp']['passed'], False)
        self.assertEqual(result['observations']['dsp']['binding'], ['informational-unbound'])
        self.assertIs(result['performanceApproved'], False)
        self.assertIs(result['rightsCleared'], False)
        self.assertNotIn('SECRET', (self.output / 'QUALIFICATION.json').read_text())
        self.assertEqual((self.export / 'LICENSE').read_bytes(), b'source')
        for line in (self.output / 'SHA256SUMS.txt').read_text().splitlines():
            digest, name = line.split('  ')
            self.assertEqual(release.digest(self.output / name), digest)

    def test_installer_tamper(self):
        (self.inputs / finalizer.SETUP).write_bytes(b'altered')
        self.refuse()

    def test_manifest_build_bindings(self):
        for key, value in (('setupBytes', 1), ('setupSha256', 'c'*64), ('version', 'other'),
                           ('releaseManifestSha256', 'd'*64), ('correspondingSourcesSha256', 'e'*64),
                           ('officialInstallerEmbeddedIntact', 'true'), ('hostExecutablePatched', 0),
                           ('installedHostExpectedSha256', 'c'*64), ('schema', 'old')):
            with self.subTest(key=key):
                self.make()
                self.edit(self.inputs / finalizer.BUILD, lambda d: d.update({key: value}))
                self.refuse()
        for component in finalizer.COMPONENTS:
            with self.subTest(component=component):
                self.make()
                self.edit(self.inputs / finalizer.BUILD, lambda d: d['embeddedComponents'].update({component: 'e'*64}))
                self.refuse()

    def test_transaction_bindings_and_boolean_types(self):
        for key, value in (('setupSha256', 'c'*64), ('installedHostSha256', 'c'*64), ('schema', 'old'),
                           ('runtimeUiSkipped', 1), ('runtimeWindowPerMonitorV2', True),
                           ('passed', 'true'), ('passed', 1), ('upgradeProfileIdentical', False),
                           ('failedProfileBackupRefusedUninstall', None)):
            with self.subTest(key=key, value=value):
                self.make()
                self.edit(self.transaction, lambda d: d.update({key: value}))
                self.refuse()
        self.make()
        self.edit(self.transaction, lambda d: d.pop('cleanFirstBoot'))
        self.refuse()

    def test_swapped_archives(self):
        corresponding = self.inputs / finalizer.CORRESPONDING
        public = self.inputs / finalizer.PUBLIC
        public.write_bytes(corresponding.read_bytes())
        self.refuse()
        self.make()
        corresponding.write_bytes(public.read_bytes())
        self.refuse()

    def test_public_unmanifested_and_injected_dependency(self):
        with zipfile.ZipFile(self.inputs / finalizer.PUBLIC, 'a') as archive:
            archive.writestr('unlisted', 'bad')
        self.refuse()
        self.make(public_extra='corresponding-sources/columns-ui/hidden')
        self.refuse()

    def test_release_swap(self):
        self.edit(self.manifest, lambda d: d.update(qaMutationHooks=True))
        self.refuse()

    def test_observation_identity_mismatch(self):
        receipt = self.root / 'observation.json'
        for value in (dict(passed=True, sourceContentSha256='f'*64),
                      dict(passed=True, sources={'LICENSE': 'a'*64}), dict(passed='true')):
            release.write_json(receipt, value)
            with self.assertRaises(ValueError): self.run_delivery({'test': receipt})
            self.assertFalse(self.output.exists())

    def test_copy_corruption_leaves_no_final_output(self):
        def corrupt(source, destination): destination.write_bytes(b'corrupted')
        with mock.patch.object(finalizer.shutil, 'copyfile', side_effect=corrupt):
            self.refuse()

    def test_export_corruption_leaves_no_final_output(self):
        write = Path.write_bytes
        for target in ('LICENSE', 'SOURCE-MANIFEST.json'):
            def corrupt(path, data):
                return write(path, b'corrupted' if path.name == target else data)
            with self.subTest(target=target), mock.patch.object(Path, 'write_bytes', corrupt):
                self.refuse()

    def test_interactive_failed_safety_gate(self):
        for flag in finalizer.HEADLESS_GATES:
            self.make()
            def fail(value):
                value.update(runtimeUiSkipped=False)
                value.update({key: True for key in finalizer.UI_GATES})
                value[flag] = False
            self.edit(self.transaction, fail)
            with self.subTest(flag=flag): self.refuse()

    def test_existing_and_overlapping_outputs_refused(self):
        self.output.mkdir()
        (self.output / 'keep').write_text('original')
        with self.assertRaises(ValueError): self.run_delivery()
        self.assertEqual((self.output / 'keep').read_text(), 'original')
        with self.assertRaises(ValueError):
            finalizer.finalize(self.inputs, self.manifest, self.transaction, self.inputs / 'nested')


class PrivacyTests(unittest.TestCase):
    def test_personal_media_defaults(self):
        absolute = 'Q:' + '\\private\\album'
        for declaration in ("[string]$FixtureAlbum = '"+absolute+"'", "param([string]$Track='"+absolute+"')"):
            self.assertEqual(audit.private_default_rules('tools/test.ps1', declaration.encode()), ['personal-media-default'])

    def test_generic_and_explicit_inputs_allowed(self):
        for text in ("[string]$AppRoot = (Join-Path $env:LOCALAPPDATA 'Programs\\GODENplayer')",
                     '[Parameter(Mandatory=$true)][string]$Track',
                     "[string]$WindowsDirectory='C:\\Windows'"):
            self.assertEqual(audit.private_default_rules('tools/test.ps1', text.encode()), [])


if __name__ == '__main__': unittest.main()
