"""Finalize an exact local candidate, without publication, player launch or promotion.

Inputs are untrusted receipts. Validate all bindings before creating fresh output
directories; publish only allowlisted, rehashed files and sanitized observations.
Runtime, performance and rights clearance are deliberately separate from integrity.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
from pathlib import Path
import re
import shutil
import tempfile
import zipfile

import public_release as release

SETUP = 'GODENplayer-Beta-Setup-x64.exe'
BUILD = 'GODENplayer-Beta-Setup-x64.manifest.json'
CORRESPONDING = 'GODENplayer-corresponding-sources.zip'
PUBLIC = 'GODENplayer-public-source.zip'
COMPONENTS = dict(zip(('fooNekoroxSha256', 'analysisWorkerSha256', 'crtRendererSha256',
                       'shaderRendererSha256',
                       'spectrumAnalyzerSha256', 'fooUiColumnsSha256'), release.BINARIES))
FILE_GATES = (
    'cleanInstall', 'legacyMarkerDidNotResetProfile', 'silentCoverConsentNotGranted',
    'upgradeProfileIdentical', 'rollbackRetained', 'rollbackRestorationByteIdentical',
    'completePayloadVerified', 'uninstallProfilePreserved', 'uninstallApplicationTreesRemoved',
    'officialInstallerSourcePreserved', 'officialHostUnmodified', 'optionalStatisticsOffline',
    'existingStatisticsProfilePreserved', 'productionUninstallRegistrationUnchanged',
    'observedFoobarStateUnchanged', 'officialRuntimePreserved', 'legacyHostUninstallerAbsent')
HEADLESS_GATES = ('failedProfileBackupRefusedUninstall', 'incompleteRecoveryRefused',
                  'failedPromotionPreservedLive', 'failedEvidenceCommitRestoredLive')
UI_GATES = ('cleanFirstBoot', 'noConfigurationDialogs', 'presenceEnabledByDefault',
            'rollbackRestorationBooted', 'activeUpgradeClosed',
            'legacyLongPathRollbackRemoved', 'runtimeWindowPerMonitorV2')
PENDING = (
    'Shader grants, separate Kerr-Newman contribution permissions and approved B icon similarity/rights',
    'Physical multi-monitor DPI, menus/dialogs and click accuracy',
    'Complete player visuals, audio/resume/seek, artwork/indexing and matched performance',
    'Extended live Presence/reconnection/network coverage beyond the listed generated-cover observations',
    'Fresh hosted CI; first-boot and active-upgrade coverage is limited to the exact transaction fields below',
    'Clean target OS/Windows N, ARM/Wine, HDR, physical pixel layouts and complete High Contrast/accessibility',
    'Arbitrary power-loss recovery and production mutation-message cases beyond the listed rejection observations')


def require(condition, message):
    if not condition:
        raise ValueError(message)


def sha(value, label):
    require(isinstance(value, str) and re.fullmatch(r'[a-fA-F0-9]{64}', value) is not None,
            'Invalid SHA256: ' + label)
    return value.lower()


def true(value, label):
    require(value is True, 'Required boolean outcome failed or missing: ' + label)


def receipt_snapshot(path):
    data = path.read_bytes()
    return json.loads(data.decode('utf-8-sig')), hashlib.sha256(data).hexdigest()


def observation(path, source_rows, source_hash, setup_hash):
    value, input_hash = receipt_snapshot(path)
    require(isinstance(value, dict), 'Observation must be a JSON object.')
    require(value.get('passed') is None or type(value['passed']) is bool,
            'Observation passed must be boolean or null.')
    bindings = []
    for field, expected, label in (
            ('sourceContentSha256', source_hash, 'candidate-source'),
            ('setupSha256', setup_hash, 'exact-installer')):
        if field in value:
            require(sha(value[field], field) == expected, 'Observation belongs to a different candidate.')
            bindings.append(label)
    if 'sources' in value:
        sources = value['sources']
        require(isinstance(sources, dict) and bool(sources), 'Observation source binding is empty.')
        for name, digest in sources.items():
            require(name in source_rows and sha(digest, 'observation source') == source_rows[name]['sha256'],
                    'Observation fixture sources changed.')
        bindings.append('listed-fixture-sources-only')
    if 'productionSourceSha256' in value:
        for field, name in (('productionSourceSha256', 'packaging/friend-installer/NekoRoX-Friend.iss'),
                            ('driverSha256', 'tools/test-statistics-download.py')):
            require(name in source_rows and sha(value.get(field), field) == source_rows[name]['sha256'],
                    'Statistics fixture sources changed.')
        bindings.append('statistics-procedure-and-driver-only')
    metrics = {}
    for key in ('exitCode', 'commonChainP99Milliseconds', 'creativeChainP99Milliseconds', 'files'):
        item = value.get(key)
        if type(item) in (int, float):
            require(float('-inf') < item < float('inf'), 'Nonfinite observation metric.')
            metrics[key] = item
    return dict(inputSha256=input_hash, passed=value.get('passed'),
                binding=bindings or ['informational-unbound'], metrics=metrics,
                qualifiesFullPlayer=False)


def preflight(installer_directory, release_manifest, transaction_evidence, observations):
    paths = {name: installer_directory / name for name in (SETUP, BUILD, CORRESPONDING, PUBLIC)}
    require(all(p.is_file() and not p.is_symlink() for p in paths.values()), 'Installer input is missing or linked.')
    expected = {name: release.digest(path) for name, path in paths.items()}
    expected_manifest = release.digest(release_manifest)
    build = release.read_json(paths[BUILD])
    manifest = release.read_json(release_manifest)
    transaction, transaction_hash = receipt_snapshot(transaction_evidence)
    require(build.get('schema') == 'goden.friend-installer-build.v3', 'Unsupported installer build schema.')
    require(manifest.get('schema') == 'goden.release-manifest.v2', 'Unsupported release manifest schema.')
    require(transaction.get('schema') == 'goden.installer-transaction-qa.v2', 'Unsupported transaction schema.')
    version = manifest['buildIdentity']['version']
    require(isinstance(version, str) and re.fullmatch(r'[A-Za-z0-9.+-]{1,80}', version), 'Unsafe version.')
    require(build.get('version') == version and build.get('setup') == SETUP, 'Installer version/name differs.')
    setup_hash = expected[SETUP]
    require(sha(build.get('setupSha256'), 'installer') == setup_hash, 'Installer bytes changed.')
    require(type(build.get('setupBytes')) is int and build['setupBytes'] == paths[SETUP].stat().st_size,
            'Installer size changed.')
    require(sha(build.get('releaseManifestSha256'), 'release manifest') == expected_manifest,
            'Installer belongs to a different release manifest.')
    require(sha(build.get('correspondingSourcesSha256'), 'corresponding archive') == expected[CORRESPONDING],
            'Installer corresponding source archive changed.')
    for flag in ('officialInstallerEmbeddedIntact', 'officialHostNotEmbedded'):
        true(build.get(flag), flag)
    require(build.get('hostExecutablePatched') is False and manifest.get('qaMutationHooks') is False,
            'Patched host or public QA mutation hooks are not allowed.')
    for field, component in COMPONENTS.items():
        require(sha(build['embeddedComponents'].get(field), field) == sha(manifest[component]['sha256'], component),
                'Installer component differs: ' + component)
    true(transaction.get('passed'), 'transaction passed')
    require(type(transaction.get('runtimeUiSkipped')) is bool, 'Missing runtime observation mode.')
    require(sha(transaction.get('setupSha256'), 'transaction installer') == setup_hash,
            'Transaction belongs to a different installer.')
    for flag in FILE_GATES:
        true(transaction.get(flag), flag)
    for flag in HEADLESS_GATES:
        require(flag in transaction and (transaction[flag] is None or type(transaction[flag]) is bool),
                'Invalid transaction outcome: ' + flag)
        if transaction['runtimeUiSkipped']:
            true(transaction[flag], flag)
        else:
            require(transaction[flag] is not False, 'Failed transaction safety outcome: ' + flag)
    for flag in UI_GATES:
        require(flag in transaction and (transaction[flag] is None or type(transaction[flag]) is bool),
                'Invalid runtime observation: ' + flag)
        if transaction['runtimeUiSkipped']:
            require(transaction[flag] is None, 'Headless report cannot claim runtime approval.')
        else:
            true(transaction[flag], flag)
    host_hash = sha(build.get('installedHostExpectedSha256'), 'expected host')
    for field in ('officialHostSourceSha256', 'installedHostSha256'):
        require(sha(transaction.get(field), field) == host_hash, 'Official host differs.')
    archives = {}
    for name, role in ((CORRESPONDING, True), (PUBLIC, False)):
        archives[name] = release.verify_archive(paths[name], corresponding=role, release_manifest=release_manifest)
    source_hash = sha(manifest['buildIdentity']['sourceContentSha256'], 'source fingerprint')
    source_rows = {r['path']: r for r in archives[PUBLIC]['files']}
    evidence = {}
    for name, path in observations.items():
        require(re.fullmatch(r'[a-z0-9-]{1,64}', name), 'Unsafe observation label.')
        evidence[name] = observation(path, source_rows, source_hash, setup_hash)
    sanitized_build = dict(schema='goden.verified-installer-build.v1', inputSha256=expected[BUILD],
                           version=version, setup=SETUP, setupBytes=build['setupBytes'], setupSha256=setup_hash,
                           releaseManifestSha256=expected_manifest,
                           correspondingSourcesSha256=expected[CORRESPONDING],
                           installedHostExpectedSha256=host_hash,
                           embeddedComponents={k: sha(build['embeddedComponents'][k], k) for k in COMPONENTS})
    sanitized_transaction = dict(schema=transaction['schema'], inputSha256=transaction_hash,
                                 passed=True, setupSha256=setup_hash, runtimeUiSkipped=transaction['runtimeUiSkipped'],
                                 **{k: transaction[k] for k in (*FILE_GATES, *HEADLESS_GATES, *UI_GATES)})
    require(all(release.digest(paths[name]) == digest for name, digest in expected.items()) and
            release.digest(release_manifest) == expected_manifest, 'Input changed during preflight.')
    return paths, archives, manifest, sanitized_build, sanitized_transaction, evidence, expected


def copy_verified(source, destination, expected):
    shutil.copyfile(source, destination)
    require(release.digest(destination) == expected, 'Copied artifact integrity mismatch.')


def extract_verified(archive_path, destination, rows):
    with zipfile.ZipFile(archive_path) as archive:
        # Paths/aliases/types were checked before staging; never use extractall.
        for name in archive.namelist():
            release.validate_archive_path(name)
            path = destination / name
            path.parent.mkdir(parents=True, exist_ok=True)
            data = archive.read(name)
            path.write_bytes(data)
            require(path.stat().st_size == len(data) and release.digest(path) == hashlib.sha256(data).hexdigest(),
                    'Exported archive member integrity mismatch.')
            if name != 'SOURCE-MANIFEST.json':
                expected = rows[name]
                require(path.stat().st_size == expected['bytes'] and release.digest(path) == expected['sha256'],
                        'Exported source integrity mismatch.')


def finalize(installer_directory, release_manifest, transaction_evidence, output_directory,
             source_export=None, observations=None):
    observations = observations or {}
    inputs = [installer_directory.resolve(), release_manifest.resolve(), transaction_evidence.resolve(),
              *(p.resolve() for p in observations.values())]
    outputs = [output_directory.resolve()]
    if source_export is not None:
        outputs.append(source_export.resolve())
    for output in outputs:
        require(not output.exists(), 'Refusing to overwrite an existing delivery/export.')
        for path in inputs:
            require(not (output == path or output.is_relative_to(path) or path.is_relative_to(output)),
                    'Output overlaps an input.')
    if len(outputs) == 2:
        require(not (outputs[0] == outputs[1] or outputs[0].is_relative_to(outputs[1]) or outputs[1].is_relative_to(outputs[0])),
                'Delivery and source export overlap.')
    paths, archives, manifest, build, transaction, evidence, expected = preflight(
        inputs[0], inputs[1], inputs[2], observations)
    # Capture expected hashes before any copying, including the external manifest.
    manifest_hash = build['releaseManifestSha256']
    stages = []
    published = []
    try:
        for output in outputs:
            output.parent.mkdir(parents=True, exist_ok=True)
            stages.append(Path(tempfile.mkdtemp(prefix='.goden-finalize-', dir=output.parent)))
        stage = stages[0]
        for name in (SETUP, CORRESPONDING, PUBLIC):
            copy_verified(paths[name], stage / name, expected[name])
        copy_verified(inputs[1], stage / 'RELEASE-MANIFEST.json', manifest_hash)
        release.write_json(stage / 'INSTALLER-BUILD-VERIFIED.json', build)
        rows = {r['path']: r for r in archives[PUBLIC]['files']}
        if source_export is not None:
            extract_verified(stage / PUBLIC, stages[1], rows)
        value = dict(schema='goden.public-preparation-delivery.v3',
                     createdUtc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                     version=manifest['buildIdentity']['version'],
                     status='local-candidate-runtime-performance-and-rights-pending',
                     sourceContentSha256=manifest['buildIdentity']['sourceContentSha256'],
                     exactInstaller=dict(file=SETUP, sha256=expected[SETUP], bytes=build['setupBytes']),
                     integrityVerified=True, installerTransaction=transaction, observations=evidence,
                     exportedSourceFiles=len(rows) if source_export is not None else None,
                     nativeBuildRepeated=False, playerLaunched=False, windowsInputControlUsed=False,
                     personalInstallationPromoted=False, githubPublicationPerformed=False,
                     performanceApproved=False, rightsCleared=False, pending=list(PENDING))
        release.write_json(stage / 'QUALIFICATION.json', value)
        lines = [f'# GODENplayer {value["version"]} â€” local candidate', '',
                 'Installer, both source archives, release manifest, six managed component hashes and',
                 'transaction evidence refer to the same candidate. Every delivered/exported file was rehashed.',
                 'This verifies integrity and the listed installer transactions, not full runtime or rights clearance.', '',
                 '## Observations', '']
        for name, item in evidence.items():
            outcome = {True: 'PASS', False: 'FAILED', None: 'not observed'}[item['passed']]
            lines.append(f'- {name}: {outcome}; binding: {", ".join(item["binding"])}. Input SHA256 `{item["inputSha256"]}`.')
        lines += ['', 'Reports without matching candidate identities are informational only. A failed DSP result',
                  'remains failed. No timing threshold, quality or refresh rate was changed; no real benchmark',
                  'or native build was rerun by finalization. See JSON for retained metrics. Headless runtime',
                  'observations remain null. A source fixture does not qualify the full player.', '',
                  '## Pending', '', *(f'- {item}.' for item in PENDING), '',
                  'Read LICENSE-SCOPE.md and docs/QUALIFICATION.md in the source archive before redistribution.',
                  'Keep corresponding sources beside the installer. SHA256SUMS.txt detects accidental changes;',
                  'it is not a cryptographic publisher signature. No promotion or publication was performed.', '']
        (stage / 'QUALIFICATION.md').write_text('\n'.join(lines), encoding='utf-8')
        (stage / 'READ-ME-FIRST.txt').write_text(
            'Local candidate only. Read QUALIFICATION.md and LICENSE-SCOPE.md in the sources.\n'
            'Runtime/performance approval and shader/Kerr-Newman/icon rights remain pending.\n'
            'Keep corresponding sources beside the installer. Verify SHA256SUMS.txt.\n', encoding='utf-8')
        for name in (CORRESPONDING, PUBLIC):
            (stage / Path(name).with_suffix('.sha256.txt')).write_text(expected[name] + '  ' + name + '\n', encoding='ascii')
        checksums = ''.join(release.digest(p) + '  ' + p.name + '\n' for p in sorted(stage.iterdir()))
        (stage / 'SHA256SUMS.txt').write_text(checksums, encoding='ascii')
        # Publish only newly staged directories; never merge into an existing output.
        for temporary, output in zip(stages, outputs):
            require(not output.exists(), 'Output appeared during finalization.')
            temporary.rename(output)
            published.append(output)
        return value
    except BaseException:
        # These are exact directories created in this invocation, never caller inputs.
        for output in reversed(published):
            shutil.rmtree(output)
        raise
    finally:
        for stage in stages:
            if stage.exists():
                shutil.rmtree(stage)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--installer-directory', type=Path, required=True)
    parser.add_argument('--release-manifest', type=Path, required=True)
    parser.add_argument('--transaction-evidence', type=Path, required=True)
    parser.add_argument('--output-directory', type=Path, required=True)
    parser.add_argument('--source-export', type=Path)
    parser.add_argument('--observation', action='append', default=[], metavar='NAME=JSON')
    args = parser.parse_args()
    observations = {}
    for item in args.observation:
        name, separator, path = item.partition('=')
        if not separator or not path or name in observations:
            parser.error('Use unique --observation NAME=JSON inputs.')
        observations[name] = Path(path)
    try:
        result = finalize(args.installer_directory, args.release_manifest, args.transaction_evidence,
                          args.output_directory, args.source_export, observations)
    except (ValueError, KeyError, TypeError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, 'Finalization refused: ' + str(error) + '\n')
    print('Local delivery verified; runtime/performance/rights remain pending. Installer SHA256: ' + result['exactInstaller']['sha256'])
