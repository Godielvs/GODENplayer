"""Run the production Inno Statistics procedure against adversarial local HTTP.

Only the fixed URL is substituted in a separate test executable. The actual
Inno downloader, SHA-256 pins, extraction, copy/rename and optional-component
failure handler are retained. No player/host, production installer, registry entry or external
service is launched. All outputs stay under a new repository artifacts folder.
"""
from __future__ import annotations

import argparse
import hashlib
import http.server
import json
import os
from pathlib import Path
import socket
import subprocess
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
PIN = '4e47c44b8624544ef618fc02999c1fbbc212534507888a871625b83e949e4014'
DLL_PIN = '68718dbf463e8dc1402261f11b0cdc39bd6e5291971deaa8b91ecd4af9cd7466'


def sha(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def run(iscc: Path, package: Path):
    if os.name != 'nt':
        raise ValueError('The Inno runtime test requires Windows.')
    if sha(package) != PIN:
        raise ValueError('Positive-control package is not the pinned official archive.')
    output = ROOT / 'artifacts' / ('statistics-download-' + uuid.uuid4().hex)
    output.mkdir(parents=True)
    original = (ROOT / 'packaging/friend-installer/NekoRoX-Friend.iss').read_text(encoding='utf-8-sig')
    source_sha = hashlib.sha256(original.encode()).hexdigest()
    start = original.index('function StatisticsProgress(')
    end = original.index('procedure WriteCoverPrivacyChoice;', start)
    procedure = original[start:end]
    hash_start = original.index('function PayloadHashMatches(')
    hash_end = original.index('procedure RestoreFailedCommit(', hash_start)
    hash_helper = original[hash_start:hash_end]
    official = 'https://www.foobar2000.org/getcomponent/31048989d7d7873a52cb371510188ac4/foo_playcount.fb2k-component'
    if procedure.count(official) != 1 or PIN.upper() not in procedure or DLL_PIN.upper() not in procedure:
        raise ValueError('Statistics procedure/pins changed; review fixture transport seam.')
    valid = package.read_bytes()
    state = {'mode': '', 'requests': 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *_):
            pass

        def do_GET(self):
            state['requests'] += 1
            mode = state['mode']
            status = 503 if mode == 'unavailable' else 200
            if mode == 'invalid-content':
                body = b'<html>not a component</html>'
            elif mode == 'tampered-archive':
                body = valid[:-1] + bytes([valid[-1] ^ 1])
            elif mode == 'oversized':
                body = b'x' * (4 * 1024 * 1024 + 1)
            elif mode == 'unavailable':
                body = b'service unavailable'
            else:
                body = valid
            self.send_response(status)
            self.send_header('Content-Type', 'application/octet-stream')
            self.send_header('Content-Length', str(len(body)))
            self.end_headers()
            try:
                self.wfile.write(body[:100] if mode == 'interrupted' else body)
                self.wfile.flush()
                if mode == 'interrupted':
                    self.connection.shutdown(socket.SHUT_RDWR)
            except (BrokenPipeError, ConnectionResetError, OSError):
                pass

    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    server_thread = threading.Thread(target=server.serve_forever, daemon=True)
    server_thread.start()
    endpoint = f'http://127.0.0.1:{server.server_port}/statistics'
    source = f'''[Setup]
AppId=GodenStatisticsFixture-{uuid.uuid4().hex}
AppName=Goden Statistics runtime fixture
AppVersion=1
DefaultDirName={{tmp}}\\GodenStatisticsFixture
CreateAppDir=no
Uninstallable=no
CreateUninstallRegKey=no
UsePreviousAppDir=no
UsePreviousLanguage=no
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={output}
OutputBaseFilename=statistics-fixture
Compression=none
ArchiveExtraction=full
[Code]
var StagingApp: String; StatisticsPage: TInputOptionWizardPage;
{hash_helper}
{procedure.replace(official, endpoint)}
procedure InitializeWizard;
begin
  StatisticsPage := CreateInputOptionPage(wpWelcome, 'Statistics fixture', '', '', False, False);
  StatisticsPage.Add('Download pinned component');
  StatisticsPage.Values[0] := True;
end;
procedure CurStepChanged(CurStep: TSetupStep);
begin
  if CurStep = ssPostInstall then
  begin
    StagingApp := ExpandFileName(ExpandConstant('{{param:QA_ROOT}}'));
    if CompareText(Copy(StagingApp, 1, Length('{output}\\')), '{output}\\') <> 0 then
      RaiseException('Fixture root must stay inside the generated artifacts directory');
    ForceDirectories(StagingApp);
    TryInstallStatistics;
    if not SaveStringToFile(AddBackslash(StagingApp) + 'fixture-completed.txt', 'complete', False) then
      RaiseException('Fixture evidence could not be saved');
  end;
end;
'''
    script = output / 'statistics-fixture.iss'
    script.write_text(source, encoding='utf-8-sig')
    value = dict(schema='goden.statistics-inno-runtime.v1', passed=False,
                 productionSourceSha256=sha(ROOT / 'packaging/friend-installer/NekoRoX-Friend.iss'),
                 driverSha256=sha(Path(__file__)), generatedScriptSha256=sha(script),
                 compilerSha256=sha(iscc), officialPackageSha256=sha(package),
                 procedureSha256=hashlib.sha256(procedure.encode()).hexdigest(),
                 transport='loopback URL substitution only; real Inno downloader/hash/extractor and production procedure',
                 productionInstallerTested=False, playerLaunched=False, cases=[])
    startup = subprocess.STARTUPINFO()
    startup.dwFlags |= subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    try:
        compiled = subprocess.run([str(iscc), '/Qp', str(script)], capture_output=True,
                                  timeout=120, startupinfo=startup)
        (output / 'compile.log').write_bytes(compiled.stdout + compiled.stderr)
        if compiled.returncode:
            raise ValueError('Inno fixture compilation failed; see compile.log')
        executable = output / 'statistics-fixture.exe'
        value['fixtureSha256'] = sha(executable)
        for mode in ['invalid-content', 'tampered-archive', 'oversized', 'unavailable',
                     'interrupted', 'valid', 'existing', 'offline']:
            state.update(mode=mode, requests=0)
            case_root = output / mode
            target = case_root / 'profile/user-components-x64/foo_playcount/foo_playcount.dll'
            if mode == 'existing':
                target.parent.mkdir(parents=True)
                target.write_bytes(b'preserve existing user component')
            log = output / (mode + '.log')
            command = [str(executable), '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART',
                       '/SP-', f'/LOG={log}', f'/QA_ROOT={case_root}']
            if mode == 'offline':
                command.append('/STATISTICS_OFFLINE=1')
            result = subprocess.run(command, capture_output=True, timeout=75, startupinfo=startup)
            present = target.is_file()
            expected_hash = (DLL_PIN if mode == 'valid' else
                             hashlib.sha256(b'preserve existing user component').hexdigest() if mode == 'existing' else None)
            passed = (result.returncode == 0 and (case_root / 'fixture-completed.txt').is_file()
                      and present == (expected_hash is not None)
                      and (not present or sha(target) == expected_hash)
                      and not target.with_suffix('.dll.next').exists()
                      and (state['requests'] == 0 if mode in ('existing', 'offline') else state['requests'] >= 1))
            if mode not in ('valid', 'existing', 'offline'):
                passed = passed and 'Optional Playback Statistics unavailable:' in log.read_text(encoding='utf-8-sig')
            if mode == 'oversized':
                log_text = log.read_text(encoding='utf-8-sig')
                passed = passed and 'Download aborted' in log_text and 'Verifying temporary file' not in log_text
            value['cases'].append(dict(name=mode, passed=passed, exitCode=result.returncode,
                                       requests=state['requests'], installed=present,
                                       installedSha256=sha(target) if present else None))
            print(('PASS ' if passed else 'FAIL ') + 'Statistics Inno runtime: ' + mode, flush=True)
            if not passed:
                raise ValueError('Statistics runtime fixture failed: ' + mode)
        if source_sha != hashlib.sha256((ROOT / 'packaging/friend-installer/NekoRoX-Friend.iss').read_text(encoding='utf-8-sig').encode()).hexdigest():
            raise ValueError('Production source changed during qualification.')
        value['passed'] = True
    except (ValueError, OSError, subprocess.TimeoutExpired) as error:
        value['error'] = str(error)
    finally:
        server.shutdown()
        server.server_close()
        (output / 'result.json').write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    print('Statistics fixture report: ' + str(output / 'result.json'))
    return 0 if value['passed'] else 1


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iscc', type=Path, required=True)
    parser.add_argument('--package', type=Path, required=True)
    args = parser.parse_args()
    raise SystemExit(run(args.iscc.resolve(), args.package.resolve()))
