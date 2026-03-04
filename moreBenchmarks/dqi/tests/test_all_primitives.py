#!/usr/bin/env python3
"""
Compile every gen_dqi_qasm.py primitive at a small instance and report results.

Run from any directory:
    python moreBenchmarks/dqi/tests/test_all_primitives.py
"""
import os
import subprocess
import sys
import time

_HERE = os.path.dirname(os.path.abspath(__file__))
_DQI_DIR = os.path.dirname(_HERE)
GEN_SCRIPT = os.path.join(_DQI_DIR, 'gen_dqi_qasm.py')
OUT_DIR = os.path.join(_DQI_DIR, 'out', 'test')

# (primitive, extra_args)
# Small instances: b=3 (GF(2^3)), m=7, n=4, k=1
CASES = [
    # DQI circuit-level primitives
    ('prepare_sds',    ['--m', '7',  '--n', '4']),
    ('qrom',           ['--m', '8']),
    ('rs_egcd_zalka',  ['--m', '7',  '--n', '4', '--b', '3']),
    ('rs_egcd_dialog', ['--m', '7',  '--n', '4', '--b', '3']),
    ('full_dqi',       ['--m', '3',  '--n', '2', '--b', '3']),
    # Gate-level primitives (opi_gate_cost_table.py keep() leaves)
    ('and',            []),
    ('toffoli',        []),
    ('cswap',          ['--b', '3']),
    ('mcx',            ['--n', '3']),
    ('addK_ctrl',      ['--b', '4',  '--k', '1']),
    ('addK_adj_ctrl',  ['--b', '4',  '--k', '1']),
    ('gf2_add',        ['--b', '3']),
    ('gf2_add_ctrl',   ['--b', '3']),
    ('gf2_mul_opt',    ['--b', '3']),
    ('gf2_mul_mbuc',   ['--b', '3']),
    ('synthesize_lr',  ['--b', '3']),
]


def _outfile(primitive, extra_args):
    """Derive a test-specific output path."""
    tag = primitive
    for flag, val in zip(extra_args[::2], extra_args[1::2]):
        tag += f'_{flag.lstrip("-")}{val}'
    return os.path.join(OUT_DIR, f'dqi_{tag}.qasm')


def run_case(primitive, extra_args, timeout=300):
    outfile = _outfile(primitive, extra_args)
    cmd = [
        sys.executable, GEN_SCRIPT,
        '--primitive', primitive,
        '--out', outfile,
    ] + extra_args
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
    elapsed = time.time() - t0
    ok = (
        proc.returncode == 0
        and os.path.exists(outfile)
        and os.path.getsize(outfile) > 0
    )
    lines = 0
    if ok:
        with open(outfile) as f:
            lines = sum(1 for _ in f)
    return ok, elapsed, lines, proc.stdout, proc.stderr


def main():
    os.makedirs(OUT_DIR, exist_ok=True)
    print(f'Output directory: {OUT_DIR}')
    print(f'Running {len(CASES)} primitives...\n')

    results = []
    for primitive, extra_args in CASES:
        label = primitive + (' ' + ' '.join(extra_args) if extra_args else '')
        print(f'  {label}...', end='', flush=True)
        try:
            ok, elapsed, lines, stdout, stderr = run_case(primitive, extra_args)
            status = 'PASS' if ok else 'FAIL'
            print(f' {status} ({elapsed:.1f}s, {lines:,} lines)')
            results.append((primitive, extra_args, status, elapsed, lines, stderr))
        except subprocess.TimeoutExpired:
            print(' TIMEOUT')
            results.append((primitive, extra_args, 'TIMEOUT', 300, 0, ''))
        except Exception as exc:
            print(f' ERROR')
            results.append((primitive, extra_args, 'ERROR', 0, 0, str(exc)))

    # Summary table
    print()
    print('=' * 65)
    print(f'{"Primitive":<22} {"Args":<22} {"Status":<8} {"s":>5} {"Lines":>8}')
    print('-' * 65)
    for primitive, extra_args, status, elapsed, lines, _ in results:
        args_str = ' '.join(extra_args)
        print(f'{primitive:<22} {args_str:<22} {status:<8} {elapsed:>4.1f}s {lines:>8,}')
    print('=' * 65)

    failures = [(p, a, s, e) for p, a, s, _, _, e in results if s != 'PASS']
    if failures:
        print(f'\n{len(failures)} failure(s):')
        for primitive, _, status, stderr in failures:
            print(f'\n  [{status}] {primitive}')
            if stderr:
                for line in stderr.strip().splitlines()[-8:]:
                    print(f'    {line}')
    else:
        print('\nAll primitives passed.')

    return 1 if failures else 0


if __name__ == '__main__':
    sys.exit(main())
