#!/usr/bin/env python3
"""Host-only audit of the external CUDA prototype; does not certify GPU execution.

Requires g++ and the reference checkout. Returns 1 when arithmetic defects are
detected. CUDA qualifiers are removed by a temporary header; arithmetic function
bodies are compiled unchanged. All generated files stay in a temporary directory
apart from the explicitly requested JSON report.
"""
import argparse
import hashlib
import json
from pathlib import Path
import random
import subprocess
import tempfile


HARNESS = r'''
#include <cstdio>
#include <iostream>
#include <string>
#include "secp256k1.cuh"
static void print(const uint256_t& x) {
    for (int i = 7; i >= 0; --i) std::printf("%08x", x.limbs[i]);
    std::printf("\n");
}
int main() {
    Point g{}, separate{}, aliased{};
    set256FromConst(&g.x, SECP256K1_GX);
    set256FromConst(&g.y, SECP256K1_GY);
    g.z.limbs[0] = 1;
    aliased = g;
    pointDouble(&separate, &g);
    pointDouble(&aliased, &aliased);
    print(separate.z);
    print(aliased.z);
    std::string ah, bh;
    while (std::cin >> ah >> bh) {
        uint256_t a{}, b{}, r{};
        for (int i = 0; i < 8; ++i) {
            a.limbs[7-i] = std::stoul(ah.substr(8*i, 8), nullptr, 16);
            b.limbs[7-i] = std::stoul(bh.substr(8*i, 8), nullptr, 16);
        }
        modMul(&r, &a, &b);
        print(r);
    }
}
'''


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--report', type=Path, required=True)
    args = parser.parse_args()
    reference = args.reference.resolve(strict=True)
    header = reference / 'cuda/secp256k1.cuh'
    kernel = reference / 'cuda/bsgs_kernel.cu'
    p = 2**256 - 2**32 - 977
    values = [0, 1, 2, p-1, p-2, p-3, 2**32-1, 2**128-1, 2**255]
    pairs = [(a, b) for a in values for b in values]
    edge_count = len(pairs)
    seed = 20260929
    rng = random.Random(seed)
    pairs += [(rng.randrange(p), rng.randrange(p)) for _ in range(1000)]
    with tempfile.TemporaryDirectory(prefix='keyhunt-reference-audit-') as folder:
        temp = Path(folder)
        (temp / 'cuda_runtime.h').write_text(
            '#define __device__\n#define __constant__\n'
            '#define __forceinline__ inline\n')
        source = temp / 'probe.cpp'
        source.write_text(HARNESS)
        binary = temp / 'probe'
        subprocess.run(['g++', '-O2', '-std=c++17', '-I', str(temp),
                        '-I', str(header.parent), str(source), '-o', str(binary)],
                       check=True, timeout=60)
        result = subprocess.run([str(binary)],
                                input=''.join(f'{a:064x} {b:064x}\n' for a, b in pairs),
                                text=True, capture_output=True, check=True, timeout=30)
    lines = result.stdout.splitlines()
    if len(lines) != len(pairs) + 2:
        raise RuntimeError('Unexpected arithmetic probe output')
    failures = []
    for index, ((a, b), actual) in enumerate(zip(pairs, lines[2:])):
        if int(actual, 16) != a*b % p:
            failures.append({'case_index': index,
                             'kind': 'edge' if index < edge_count else 'seeded',
                             'a': f'{a:064x}', 'b': f'{b:064x}',
                             'expected': f'{a*b % p:064x}', 'actual': actual})
    gy = int('483ada7726a3c4655da4fbfc0e1108a8fd17b448a68554199c47d08ffb10d4b8', 16)
    expected_z = f'{2*gy % p:064x}'
    double_failure = lines[0] != expected_z or lines[1] != expected_z
    report = {
        'schema_version': 1,
        'scope': 'Host-only arithmetic probe; no GPU compilation or execution',
        'reference_commit': subprocess.check_output(
            ['git', '-C', str(reference), 'rev-parse', 'HEAD'], text=True).strip(),
        'reference_header_sha256': hashlib.sha256(header.read_bytes()).hexdigest(),
        'reference_kernel_sha256': hashlib.sha256(kernel.read_bytes()).hexdigest(),
        'harness_sha256': hashlib.sha256(HARNESS.encode()).hexdigest(),
        'compiler': subprocess.check_output(['g++', '--version'], text=True).splitlines()[0],
        'compiler_flags': ['-O2', '-std=c++17'],
        'seed': seed,
        'field_multiplication_cases': len(pairs),
        'edge_cases': edge_count,
        'seeded_cases': len(pairs) - edge_count,
        'failed_field_multiplication_cases': len(failures),
        'failed_edge_cases': sum(f['kind'] == 'edge' for f in failures),
        'failed_seeded_cases': sum(f['kind'] == 'seeded' for f in failures),
        'first_failed_examples': failures[:8],
        'double_g_expected_z': expected_z,
        'double_g_z_separate': lines[0],
        'double_g_z_in_place': lines[1],
        'double_g_failed': double_failure,
        'source_derived_inversion_counts': {'squarings': 256, 'multiplies': (p-2).bit_count()},
        'source_derived_bloom_512_mib_bit_count_uint32': ((512 * 1024**2) * 8) & (2**32 - 1),
    }
    args.report.write_text(json.dumps(report, indent=2) + '\n')
    print(f'Multiplication mismatches: {len(failures)}/{len(pairs)}; '
          f'doubling mismatch: {double_failure}')
    print(f'Report: {args.report}')
    return 1 if failures or double_failure else 0


if __name__ == '__main__':
    raise SystemExit(main())
