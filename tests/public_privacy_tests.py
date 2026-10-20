"""Offline privacy and dependency-drift rejection; no private profile is read."""
import importlib.util
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'tools'))
import public_release as release
import importlib
inputs = importlib.import_module('verify-build-inputs')
spec = importlib.util.spec_from_file_location('audit', ROOT / 'tools/audit-public-source.py')
audit = importlib.util.module_from_spec(spec); spec.loader.exec_module(audit)


class PrivacyTests(unittest.TestCase):
    def test_private_key_formats(self):
        with tempfile.TemporaryDirectory() as temp:
            file = Path(temp) / 'source.txt'
            for prefix in ('', 'RSA ', 'EC ', 'OPENSSH ', 'DSA ', 'ENCRYPTED '):
                file.write_text('-----BEGIN ' + prefix + 'PRIVATE KEY' + '-----')
                with patch.object(release, 'source_files', return_value=[(file, 'source.txt')]):
                    self.assertEqual(audit.audit([])['failures'], [{'path':'source.txt','rule':'private-key'}])

    def test_sanitizer_reports_never_echo_input_paths_or_markers(self):
        with tempfile.TemporaryDirectory(prefix='goden-privacy-') as temp:
            root = Path(temp); profile = root / 'PrivateMarkerProfile'
            layout = profile / 'nekorox/presets/layout.fcl'; layout.parent.mkdir(parents=True)
            shutil.copyfile(ROOT / 'packaging/clean-seed/nekorox/presets/layout.fcl', layout)
            report = root / 'report.json'
            command = ['node', str(ROOT / 'tools/sanitize-friend-profile.js'), str(profile), str(report), str(root / 'PrivateMarkerSource')]
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            text = result.stdout + result.stderr + report.read_text()
            for marker in (str(root), 'PrivateMarkerProfile', 'PrivateMarkerSource'):
                self.assertNotIn(marker, text)
            (profile / 'untrusted.dat').write_bytes(('C:' + '\\Users\\' + 'PrivateMarkerLeak\\file').encode())
            result = subprocess.run(command, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn('PrivateMarkerLeak', result.stdout + result.stderr)
            self.assertNotIn(str(root), result.stdout + result.stderr)

    def test_vendored_or_patch_mutation_refused_before_build(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp); source = root / 'vendor/example/a.h'
            source.parent.mkdir(parents=True); source.write_bytes(b'qualified')
            rows = [{'path':'vendor/example/a.h','sha256':release.digest(source)}]
            inventory = root / 'third_party/inventory.json'
            release.write_json(inventory, {'files':rows})
            content = __import__('hashlib').sha256(json.dumps(rows, sort_keys=True, separators=(',',':')).encode()).hexdigest()
            patchfile = root / 'third_party/example.patch'; patchfile.write_bytes(b'patch')
            release.write_json(root / 'third_party/dependencies.lock.json', {'dependencies':[
                {'name':'fixture','inventory':'third_party/inventory.json','contentSha256':content,
                 'patches':[{'path':'third_party/example.patch','sha256':release.digest(patchfile)}]}]})
            self.assertEqual(inputs.verify(root), 2)
            source.write_bytes(b'drift')
            with self.assertRaisesRegex(ValueError, 'Vendored input drift'): inputs.verify(root)
            source.write_bytes(b'qualified'); patchfile.write_bytes(b'drift')
            with self.assertRaisesRegex(ValueError, 'Pinned dependency input drift'): inputs.verify(root)


if __name__ == '__main__': unittest.main()
