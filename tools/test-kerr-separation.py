"""Deterministic standalone/IPC Kerr image comparison; never controls a desktop.

Fixtures use the documented Windows x64 LE ABI, not a compiler-private object.
Generated images contain only procedural inputs and may be shared for QA.
"""
from __future__ import annotations
import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess

RECORD = struct.Struct('<6Qd4f2I208f')
assert RECORD.size == 912
MAGIC = 0x46524747

def generate(path):
    records = []
    for n in range(64):
        music = n < 16 or n >= 30
        playing = (4 <= n < 44) or n >= 52
        visible = n >= 4 and n != 51
        work = n not in (12, 13, 51)
        seconds = n / 30 if n < 44 else 44 / 30 + (n - 44) * .8 if n < 52 else 12 + (n - 52) * .1
        width, height = (960, 640) if n < 54 else (1920, 1080) if n < 59 else (600, 600)
        flags = (1 if playing else 0) | (2 if visible else 0) | (32 if work else 0) | (4 if music else 0) | 8 | 16
        colors = [.85, .18, .08, 1, .14, .23, .7, 1, .32, .48, .21, 1, .03, .02, .04, 1]
        energy = [(.08 + ((i * 11 + n * 7) % 47) / 64) if n != 63 else 0 for i in range(64)]
        onsets = [1.6 if (i + n) % 17 == 0 else 0 for i in range(64)]
        activity = [((i + n) % 11 - 5) / 5 for i in range(64)]
        records.append(RECORD.pack(n+1, 0, 0 if n<16 else 1 if n<30 else 2, 1 if n<61 else 2,
                                   1 if n<61 else 2, n+1, seconds, width, height, 1.25, 1,
                                   flags, 48000, *(colors+energy+onsets+activity)))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(struct.pack('<4I', MAGIC, 1, RECORD.size, len(records))+b''.join(records))

def compare(left, right):
    files = sorted(left.glob('frame-*.bmp'))
    if not files or {p.name for p in files} != {p.name for p in right.glob('frame-*.bmp')}:
        raise ValueError('Missing or extra rendered frames')
    for p in files:
        if p.read_bytes() != (right / p.name).read_bytes():
            raise ValueError('Different pixels: '+p.name)
    return len(files)

def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--generate', type=Path)
    parser.add_argument('--renderer', type=Path)
    parser.add_argument('--host-probe', type=Path)
    parser.add_argument('--approved-reference', type=Path)
    parser.add_argument('--out', type=Path)
    args = parser.parse_args()
    if args.generate: generate(args.generate)
    if args.renderer:
        if not args.out: parser.error('--out required')
        args.out.mkdir(parents=True, exist_ok=False)
        fixture = args.out / 'values.bin'; generate(fixture)
        runs = {}
        programs = [('direct', args.renderer, []), ('client', args.renderer, ['--via-client'])]
        if args.host_probe: programs.append(('host', args.host_probe, []))
        if args.approved_reference: programs.append(('approved', args.approved_reference, []))
        for name, program, flags in programs:
            command = [str(program.resolve()), '--fixture', str(fixture.resolve()), '--out', str((args.out/name).resolve()), *flags]
            run = subprocess.run(command, capture_output=True, text=True, timeout=300)
            runs[name] = {'exitCode': run.returncode, 'output': run.stdout+run.stderr}
            if run.returncode: raise RuntimeError(name+': '+str(runs[name]))
        count = compare(args.out/'direct', args.out/'client')
        if args.host_probe: compare(args.out/'direct', args.out/'host')
        if args.approved_reference: compare(args.out/'direct', args.out/'approved')
        result = {'schema':'goden.kerr-separation.v1', 'passed':True, 'byteIdenticalBmpFrames':count,
                  'rendererSha256':hashlib.sha256(args.renderer.read_bytes()).hexdigest(),
                  'executables':{name:hashlib.sha256(program.read_bytes()).hexdigest() for name,program,_ in programs},
                  'cases':['Original','Music','static first image','temporary defer','signed activity','onsets > 1','pause slowdown','resume','hidden','resize','track change'],
                  'runs':runs, 'scope':'deterministic GPU pixels, not human aesthetic approval or full-player performance'}
        (args.out/'result.json').write_text(json.dumps(result, indent=2)+'\n')
        print(json.dumps(result, indent=2))
if __name__=='__main__': main()
