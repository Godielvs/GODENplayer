"""Run the production single-root guard in an isolated Inno executable.

Only pure filesystem guards are extracted. No host, production registry,
shortcut, personal installation or application executable is touched.
"""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import uuid

ROOT=Path(__file__).resolve().parents[1]


def run(iscc):
    original=(ROOT/'packaging/friend-installer/NekoRoX-Friend.iss').read_text(encoding='utf-8-sig')
    procedures=[]
    for name in ('InstallRootsOverlap','QaTargetAllowed','HasExistingInstallAtRoot','ProductionRootConflict'):
        matches=re.findall(r'^function '+name+r'\([\s\S]*?^end;',original,re.M)
        if len(matches)!=1:
            raise ValueError('Installer root-policy extraction boundary changed')
        procedures.extend(matches)
    output=ROOT/'artifacts'/('installer-root-policy-'+uuid.uuid4().hex)
    output.mkdir(parents=True)
    header=f'''[Setup]
AppId=GodenRootPolicyFixture-{uuid.uuid4().hex}
AppName=Goden root policy fixture
AppVersion=1
DefaultDirName={{tmp}}\\GodenRootFixture
CreateAppDir=no
Uninstallable=no
CreateUninstallRegKey=no
UsePreviousAppDir=no
UsePreviousLanguage=no
PrivilegesRequired=lowest
DisableDirPage=yes
DisableProgramGroupPage=yes
OutputDir={output}
OutputBaseFilename=root-policy
[Code]
'''
    tests=r'''
procedure Require(const Condition: Boolean; const Message: String);
begin
  if not Condition then RaiseException(Message);
end;
procedure CurStepChanged(Step: TSetupStep);
var
  Base, First, Second, Missing, Plain, UninstallerOnly: String;
begin
  if Step <> ssPostInstall then exit;
  Base := ExpandConstant('{src}\fixtures João 日本語');
  First := Base + '\first'; Second := Base + '\second';
  Missing := Base + '\missing'; Plain := Base + '\plain';
  UninstallerOnly := Base + '\uninstaller-only';
  Require(ForceDirectories(First + '\app'), 'create live fixture');
  Require(SaveStringToFile(First + '\app\foobar2000.exe', 'not executable', False), 'write marker');
  Require(ForceDirectories(Plain), 'create unrelated directory');
  Require(ForceDirectories(UninstallerOnly), 'create uninstaller fixture');
  Require(SaveStringToFile(UninstallerOnly + '\unins000.exe', 'not executable', False), 'write uninstall marker');
  Require(ProductionRootConflict(First, First) = '', 'same-root upgrade refused');
  Require(ProductionRootConflict(First + '\', Uppercase(First)) = '', 'case/trailing slash mismatch');
  Require(ProductionRootConflict(First + '\..\first', First) = '', 'normalized root mismatch');
  Require(ProductionRootConflict(Second, First) <> '', 'second managed root allowed');
  Require(ProductionRootConflict(Second, UninstallerOnly) <> '', 'retained uninstall ownership ignored');
  Require(ProductionRootConflict(Second, Missing) = '', 'stale registration blocks fresh install');
  Require(ProductionRootConflict(Second, Plain) = '', 'unrelated folder treated as GODEN');
  Require(ProductionRootConflict(Second, '') = '', 'fresh install refused');
  Require(not QaTargetAllowed(First, Missing, First), 'QA accepted custom production root');
  Require(not QaTargetAllowed(First + '\child', Missing, First), 'QA accepted production child');
  Require(not QaTargetAllowed(Base, Missing, First), 'QA accepted production ancestor');
  Require(not QaTargetAllowed(Missing, Missing, ''), 'QA accepted default production root');
  Require(not QaTargetAllowed('', Missing, First), 'QA accepted implicit directory');
  Require(QaTargetAllowed(Second, Missing, First), 'QA refused disposable root');
  Require(QaTargetAllowed(First + '-fixture', Missing, First), 'QA confused adjacent path prefix');
  Require(SaveStringToFile(Plain + '\registry-ownership.ini', 'not personal data', False), 'write ownership fixture');
  Require(not QaTargetAllowed(Plain, Missing, First), 'QA accepted retained production ownership');
  Require(SaveStringToFile(ExpandConstant('{src}\passed.txt'), '16 guards passed', False), 'save result');
end;
'''
    source=output/'root-policy.iss'
    source.write_text(header+'\n'.join(procedures)+tests,encoding='utf-8-sig')
    with (output/'build.log').open('w') as log:
        subprocess.run([str(iscc),'/Q',str(source)],stdout=log,stderr=subprocess.STDOUT,check=True)
    subprocess.run([str(output/'root-policy.exe'),'/VERYSILENT','/SUPPRESSMSGBOXES','/NORESTART'],check=True)
    if (output/'passed.txt').read_text()!='16 guards passed':
        raise ValueError('Root policy fixture did not complete')
    report={'schema':'goden.installer-root-policy.v1','passed':True,'cases':16,
            'sources':{'packaging/friend-installer/NekoRoX-Friend.iss':hashlib.sha256((ROOT/'packaging/friend-installer/NekoRoX-Friend.iss').read_bytes()).hexdigest()},
            'productionRegistryTouched':False,'runtimeUiTested':False}
    (output/'result.json').write_text(json.dumps(report,indent=2)+'\n')
    print('Installer root policy PASS: 16 actual Inno/filesystem guard cases')
    return output


if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--iscc',type=Path,required=True)
    args=parser.parse_args()
    print(run(args.iscc))
