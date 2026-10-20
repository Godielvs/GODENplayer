"""PE parser and deployment negatives; never load fixture code or a system DLL."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('runtime_audit', Path(__file__).resolve().parents[1] / 'tools/audit-runtime-dependencies.py')
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


def image(name=None, delay=False):
    data = bytearray(2048)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 60, 128)
    data[128:132] = b'PE\0\0'
    struct.pack_into('<HH', data, 132, 0x8664, 1)
    struct.pack_into('<H', data, 148, 240)
    struct.pack_into('<H', data, 152, 0x20b)
    struct.pack_into('<Q', data, 176, 0x140000000)
    struct.pack_into('<I', data, 212, 512)
    struct.pack_into('<I', data, 260, 16)
    struct.pack_into('<IIII', data, 400, 1536, 4096, 1536, 512)
    if name:
        directory = 13 if delay else 1
        struct.pack_into('<II', data, 264 + directory * 8, 4096, 64 if delay else 40)
        if delay:
            struct.pack_into('<II', data, 512, 1, 4352)
        else:
            struct.pack_into('<I', data, 524, 4352)
        encoded = name.encode('ascii') + b'\0'
        data[768:768 + len(encoded)] = encoded
    return data


class Imports(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, data):
        target = self.root / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        return target

    def test_normal_and_delay(self):
        for delay in (False, True):
            path = self.write('test.dll', image('KERNEL32.dll', delay))
            self.assertEqual(audit.pe_imports(path), ['kernel32.dll'])

    def test_rejects_wrong_architecture(self):
        data = image();struct.pack_into('<H', data, 132, 0x14c)
        with self.assertRaises(ValueError):
            audit.pe_imports(self.write('test.dll', data))

    def test_rejects_truncation(self):
        for length in (0, 2, 64, 150, 420, 520):
            with self.assertRaises(ValueError):
                audit.pe_imports(self.write('test.dll', image('kernel32.dll')[:length]))

    def test_rejects_unsafe_names(self):
        for name in ('../escape.dll', 'C:\\escape.dll', 'not-a-dll'):
            with self.assertRaises(ValueError):
                audit.pe_imports(self.write('test.dll', image(name)))

    def test_rejects_unmapped_name(self):
        data=image('kernel32.dll');struct.pack_into('<I', data, 524, 0xfffffff0)
        with self.assertRaises(ValueError):
            audit.pe_imports(self.write('test.dll', data))

    def test_missing_crt_not_satisfied_by_developer_pc(self):
        host=self.root / 'host';self.write('host/foobar2000.exe', image())
        managed=self.write('plugin.dll', image('msvcp140.dll'))
        self.assertFalse(audit.audit(host, [managed])['passed'])
        self.write('host/msvcp140.dll', image('kernel32.dll'))
        result=audit.audit(host, [managed])
        self.assertTrue(result['passed'])
        self.assertEqual(len(result['images']), 3)

    def test_optional_media_stays_worker_only(self):
        host=self.root / 'host';self.write('host/foobar2000.exe', image())
        worker=self.write('nekorox-analysis.exe', image('mfplat.dll'))
        self.assertTrue(audit.audit(host, [worker])['passed'])
        component=self.write('plugin.dll', image('mfplat.dll'))
        self.assertFalse(audit.audit(host, [component])['passed'])

    def test_unknown_delay_dependency_refused(self):
        host=self.root / 'host';self.write('host/foobar2000.exe', image())
        plugin=self.write('plugin.dll', image('unshipped.dll', True))
        result=audit.audit(host, [plugin])
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing'][0]['import'], 'unshipped.dll')

    def test_unshipped_release_sibling_does_not_hide_dependency(self):
        host=self.root / 'host';self.write('host/foobar2000.exe', image())
        plugin=self.write('Release/plugin.dll', image('unshipped.dll'))
        self.write('Release/unshipped.dll', image('kernel32.dll'))
        result=audit.audit(host, [plugin])
        self.assertFalse(result['passed'])
        self.assertEqual(result['missing'][0]['import'], 'unshipped.dll')
        self.assertEqual(len(result['images']), 2)

    def test_unknown_api_set_is_not_implicitly_supported(self):
        host=self.root / 'host';self.write('host/foobar2000.exe', image())
        for name in ('api-ms-win-nonexistent-l1-1-0.dll', 'ext-ms-win-invented-l1-1-0.dll'):
            plugin=self.write('plugin.dll', image(name))
            self.assertFalse(audit.audit(host, [plugin])['passed'])
        plugin=self.write('plugin.dll', image('api-ms-win-core-path-l1-1-0.dll'))
        self.assertTrue(audit.audit(host, [plugin])['passed'])


if __name__ == '__main__':
    unittest.main()
