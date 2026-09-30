#!/usr/bin/env python3
"""Extract and disassemble the actual gfx942 code objects in a HIP executable."""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def capture(binary, llvm_bin, output):
    # objdump --offloading writes extracted bundles beside its input. Copy the
    # executable so inspection never creates files in a build or frozen snapshot.
    output.mkdir(parents=True, mode=0o700, exist_ok=False)
    copied = output / 'input'
    shutil.copy2(binary, copied)
    report = {'recorded_utc': datetime.now(timezone.utc).isoformat(), 'passed': False,
              'binary_sha256': sha(copied), 'binary': str(binary),
              'commands': [], 'code_objects': [], 'failure': None}

    def run(command, name):
        command = list(map(str, command))
        result = subprocess.run(command, cwd=output, capture_output=True, timeout=120)
        stdout, stderr = output / (name + '.out'), output / (name + '.err')
        stdout.write_bytes(result.stdout)
        stderr.write_bytes(result.stderr)
        report['commands'].append({'argv': command, 'exit_code': result.returncode,
                                   'stdout': stdout.name, 'stdout_sha256': sha(stdout),
                                   'stderr': stderr.name, 'stderr_sha256': sha(stderr)})
        if result.returncode:
            raise RuntimeError(f'command failed: inspect {stderr}')
        return result.stdout.decode()

    try:
        objdump = llvm_bin / 'llvm-objdump'
        run([objdump, '--version'], 'tool-version')
        run([objdump, '--offloading', copied], 'extract')
        bundles = sorted(output.glob('input.*hip*-amdgcn-*-gfx942*'))
        if not bundles:
            raise RuntimeError('no embedded gfx942 code objects; check the selected binary')
        for index, bundle in enumerate(bundles):
            assembly = run([objdump, '--disassemble', '--mcpu=gfx942', bundle], f'isa-{index}')
            # ELF notes contain per-kernel VGPR/SGPR and private-segment sizes;
            # these are compiler allocations, not measured occupancy counters.
            run([llvm_bin / 'llvm-readobj', '--notes', bundle], f'notes-{index}')
            if 'file format elf64-amdgpu' not in assembly or not re.search(r'\bs_endpgm\b', assembly):
                raise RuntimeError(f'missing AMDGPU kernel disassembly: {bundle.name}')
            report['code_objects'].append({'file': bundle.name, 'sha256': sha(bundle),
                'bytes': bundle.stat().st_size,
                'carry_instructions': len(re.findall(r'\bv_addc_co_u32', assembly)),
                'borrow_instructions': len(re.findall(r'\bv_subb_co_u32', assembly)),
                'multiply_add_instructions': len(re.findall(r'\bv_mad_u64_u32', assembly))})
        report['passed'] = True
    except (OSError, RuntimeError, subprocess.SubprocessError) as error:
        report['failure'] = str(error)
    finally:
        copied.unlink()
        (output / 'report.json').write_text(json.dumps(report, indent=2) + '\n')
    return report['passed']


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--llvm-bin', type=Path, required=True, help='AMD LLVM tool directory')
    parser.add_argument('--output-dir', type=Path, required=True, help='new external directory')
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    output = args.output_dir.resolve()
    if output == root or root in output.parents or output.exists():
        parser.error('output-dir must be new and outside the checkout')
    passed = capture(args.binary.resolve(), args.llvm_bin.resolve(), output)
    print(f'ISA capture {"passed" if passed else "FAILED"}: {output / "report.json"}')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())
