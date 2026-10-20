"""Exercise the production staging copy in a silent disposable Inno fixture.

No player, production registry, settings or live installation is used. A path
under a regular file deterministically fails copying; this is not a disk-full
simulation. Keep the original app and profile byte-identical after failure.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT = Path(__file__).resolve().parents[1]


def run(iscc):
    path = ROOT / 'packaging/friend-installer/NekoRoX-Friend.iss'
    source = path.read_text(encoding='utf-8-sig')
    procedures = []
    for name in ('Quote', 'SeedStagingFromLive'):
        matches = re.findall(r'^function ' + name + r'\([\s\S]*?^end;', source, re.M)
        if len(matches) != 1:
            raise ValueError('Staging extraction boundary changed: ' + name)
        procedures.extend(matches)
    output = ROOT / 'artifacts' / ('installer-staging-copy-' + uuid.uuid4().hex)
    output.mkdir(parents=True)
    header = f'''[Setup]
AppId=GodenStagingCopyFixture-{uuid.uuid4().hex}
AppName=Goden staging copy fixture
AppVersion=1
DefaultDirName={{tmp}}\\GodenStagingFixture
CreateAppDir=no
Uninstallable=no
CreateUninstallRegKey=no
UsePreviousAppDir=no
UsePreviousLanguage=no
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={output}
OutputBaseFilename=staging-copy
[Code]
'''
    tests = r'''
procedure Require(const Condition: Boolean; const Message: String);
begin
  if not Condition then RaiseException(Message);
end;
procedure CurStepChanged(Step: TSetupStep);
var
  Base, Live, Next, Failure, Hash, Blocked, ProfileHash: String;
begin
  if Step <> ssPostInstall then exit;
  Base := ExpandConstant('{src}\fixtures João 日本語');
  Live := Base + '\app'; Next := Base + '\app.next';
  Require(ForceDirectories(Live + '\profile\crash reports'), 'create source');
  Require(SaveStringToFile(Live + '\foobar2000.exe', 'not executable - approved live sentinel', False), 'write live sentinel');
  Require(SaveStringToFile(Live + '\profile\config.sqlite', 'profile sentinel', False), 'write profile sentinel');
  Require(SaveStringToFile(Live + '\profile\crash reports\private.dmp', 'not for staging', False), 'write excluded crash');
  Require(SaveStringToFile(Live + '\profile\running', 'not for staging', False), 'write excluded marker');
  Hash := GetSHA256OfFile(Live + '\foobar2000.exe');
  ProfileHash := GetSHA256OfFile(Live + '\profile\config.sqlite');
  Failure := 'stale failure';
  Require(SeedStagingFromLive(Base + '\missing', Next, Failure) and (Failure = ''), 'absent live app failed');
  Require(not DirExists(Next), 'absent source created staging');
  Require(SeedStagingFromLive(Live, Next, Failure) and (Failure = ''), 'valid copy failed');
  Require(GetSHA256OfFile(Next + '\foobar2000.exe') = Hash, 'binary copy differs');
  Require(GetSHA256OfFile(Next + '\profile\config.sqlite') = ProfileHash, 'profile copy differs');
  Require(not FileExists(Next + '\profile\crash reports\private.dmp'), 'crash report copied');
  Require(not FileExists(Next + '\profile\running'), 'running marker copied');
  Require(SeedStagingFromLive(Live, Next, Failure) and (Failure = ''), 'unchanged copy failed');
  Blocked := Base + '\regular-file';
  Require(SaveStringToFile(Blocked, 'destination obstacle', False), 'write obstacle');
  Require(not SeedStagingFromLive(Live, Blocked + '\app.next', Failure), 'invalid destination accepted');
  Require(Pos('Robocopy exit code ', Failure) = 1, 'copy failure code lost');
  Require(GetSHA256OfFile(Live + '\foobar2000.exe') = Hash, 'failed copy changed live app');
  Require(GetSHA256OfFile(Live + '\profile\config.sqlite') = ProfileHash, 'failed copy changed profile');
  Require(not DirExists(Base + '\app.previous'), 'failed copy moved live app');
  Require(not FileExists(Base + '\install-recovery.ini'), 'failed copy created commit evidence');
  Require(SaveStringToFile(ExpandConstant('{src}\passed.txt'), 'staging-copy passed', False), 'save result');
end;
'''
    fixture = output / 'staging-copy.iss'
    fixture.write_text(header + '\n'.join(procedures) + tests, encoding='utf-8-sig')
    with (output / 'build.log').open('w') as log:
        subprocess.run([str(iscc), '/Q', str(fixture)], stdout=log, stderr=subprocess.STDOUT,
                       timeout=120, check=True)
    log = output / 'runtime.log'
    subprocess.run([str(output / 'staging-copy.exe'), '/VERYSILENT', '/SUPPRESSMSGBOXES',
                    '/NORESTART', '/LOG=' + str(log)], timeout=120, check=True)
    if (output / 'passed.txt').read_text() != 'staging-copy passed':
        raise ValueError('Staging fixture did not complete')
    logged = log.read_text(encoding='utf-8-sig')
    for phrase in ('Staging copy failed: Robocopy exit code ', 'Staging source:', '; destination:'):
        if phrase not in logged:
            raise ValueError('Copy diagnosis missing from log')
    report = {'schema': 'goden.installer-staging-copy.v1', 'passed': True,
              'cases': ['absent-source', 'copy-with-exclusions', 'unchanged-copy', 'invalid-destination'],
              'liveAndProfilePreserved': True, 'actualDiskFullTested': False,
              'runtimeUiTested': False, 'productionRegistryTouched': False,
              'sources': {path.relative_to(ROOT).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()}}
    (output / 'result.json').write_text(json.dumps(report, indent=2) + '\n')
    print('Installer staging copy PASS: live/profile preserved and copy error diagnosed')
    print(output / 'result.json')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iscc', type=Path, required=True)
    run(parser.parse_args().iscc.resolve())
