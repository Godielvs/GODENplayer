"""Opt-in external API smoke test with one generated, non-personal test PNG.

This is not a full player/Discord-client test. Anonymous uploads are public and
cannot be deleted by this tool. It accepts no artwork, account or file input.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import time
import urllib.error
import urllib.parse
import urllib.request
import zlib


class NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, req, fp, code, msg, headers, newurl):
        return None


def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))


def test_image():
    rows = b''.join(b'\0' + bytes((16, 120, 160, 255)) * 32 for _ in range(32))
    return (b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>2I5B', 32, 32, 8, 6, 0, 0, 0))
            + chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def run(report, reuse_origin=None):
    png = test_image()
    value = dict(schema='goden.catbox-network-smoke.v1', passed=False,
                 input='generated 32x32 solid color; no personal data or original image metadata',
                 inputSha256=hashlib.sha256(png).hexdigest(), uploadAttempts=0 if reuse_origin else 1,
                 playerWorkerTested=False, discordClientTested=False)

    def save():
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')

    opener = urllib.request.build_opener(NoRedirect)
    boundary = 'GodenPublicNetworkSmoke'
    body = (f'--{boundary}\r\nContent-Disposition: form-data; name="reqtype"\r\n\r\nfileupload\r\n'
            f'--{boundary}\r\nContent-Disposition: form-data; name="fileToUpload"; filename="test.png"\r\n'
            'Content-Type: image/png\r\n\r\n').encode() + png + f'\r\n--{boundary}--\r\n'.encode()
    try:
        origin = reuse_origin
        if not origin:
            request = urllib.request.Request('https://catbox.moe/user/api.php', body,
                                            {'Content-Type': 'multipart/form-data; boundary=' + boundary})
            with opener.open(request, timeout=12) as response:
                origin = response.read(1025).decode('ascii').strip()
        if not re.fullmatch(r'https://files\.catbox\.moe/[a-zA-Z0-9]{6}\.png', origin):
            raise ValueError('Unexpected upload response; no follow-up request sent.')
        value['originUrl'] = origin
        save()  # Retain the returned URL even if subsequent delivery fails.
        for attempt in range(3):
            try:
                with opener.open(origin, timeout=8) as response:
                    original = response.read(1024 * 1024 + 1)
                if original != png:
                    raise ValueError('Origin differs from the generated PNG.')
                value['originExact'] = True
                break
            except OSError as error:
                value['originError'] = type(error).__name__ + ': ' + str(error)
                if attempt < 2:
                    time.sleep(attempt + 1)
        # Check delivery independently: a transient origin read is not proof
        # that the compatibility proxy or the stored object has failed.
        delivery = 'https://wsrv.nl/?url=' + urllib.parse.quote(origin, safe='') + '&output=png&maxage=1y'
        with opener.open(delivery, timeout=12) as response:
            delivered = response.read(1024 * 1024 + 1)
        if not 24 <= len(delivered) <= 1024 * 1024 or delivered[:8] != b'\x89PNG\r\n\x1a\n':
            raise ValueError('Delivery did not return a bounded PNG.')
        if struct.unpack('>II', delivered[16:24]) != (32, 32):
            raise ValueError('Unexpected delivery dimensions.')
        value.update(passed=value.get('originExact', False), compatibilityDeliveryPng=True)
    except (OSError, ValueError, UnicodeError) as error:
        value['error'] = type(error).__name__ + ': ' + str(error)
    save()
    print('Catbox API + compatibility delivery: ' + ('PASS' if value['passed'] else 'NOT VERIFIED'))
    return 0 if value['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--allow-public-test-upload', action='store_true', required=True)
    parser.add_argument('--report', type=Path, required=True)
    parser.add_argument('--reuse-origin', help='Read a previous test object; do not upload again.')
    args = parser.parse_args()
    raise SystemExit(run(args.report, args.reuse_origin))
