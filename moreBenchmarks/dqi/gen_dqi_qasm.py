
import argparse
import os
import sys

# qualtran_compiler is installed via pip
# Derive the bundled dqi path from the package install location.
import qualtran_to_qasm as _qc
_DQI_SRC = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(_qc.__file__))), 'dqi', 'src')
sys.path.insert(0, os.path.abspath(_DQI_SRC))

from qualtran_to_qasm.compiler import bloq_to_qasm


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _qgf(b):
    from qualtran import QGF
    return QGF(2, b)

def _require(args, *names):
    for name in names:
        if getattr(args, name) is None:
            raise ValueError(f'--{name} is required for this primitive')


# ---------------------------------------------------------------------------
# Bloq factory functions
# ---------------------------------------------------------------------------

# ---- DQI circuit-level primitives ----------------------------------------

def make_prepare_sds(args):
    from dqi.sparse_dicke_states.prepare_sparse_dicke_state_bloq import PrepareSparseDickeState
    _require(args, 'm', 'n')
    m = args.m
    k = args.n // 2
    print(f'Building PrepareSparseDickeState(m={m}, k={k})')
    return PrepareSparseDickeState(m=m, k=k)


def make_qrom(args):
    import math
    import numpy as np
    from qualtran.bloqs.data_loading.qrom import QROM
    _require(args, 'm')
    m = args.m
    target_bitsize = args.target_bitsize
    sel_bitsize = math.ceil(math.log2(m)) if m > 1 else 1
    rng = np.random.default_rng(seed=42)
    data = rng.integers(0, 2**target_bitsize, size=m, dtype=np.int64)
    print(f'Building QROM(sel_bitsizes=({sel_bitsize},), target_bitsizes=({target_bitsize},), m={m})')
    return QROM([data], selection_bitsizes=(sel_bitsize,), target_bitsizes=(target_bitsize,))


def make_rs_egcd_zalka(args):
    """
    Compiles _PolyEEAZalkaImpl, the concrete decomposable core of
    RSCodeEGCDZalkaEEA.

    RSCodeEGCDZalkaEEA call graph:
      _PolyEEAZalkaImpl  — n_eea_steps iterations of _PolyEEAZalkaImplStep;
                           compiled here as the primary (and only) output.
      ConstructOrPhaseErrors — stub (build_call_graph only, no qubit wiring);
                           appears as // [unresolved: ...] comments in output.
    """
    _require(args, 'm', 'n', 'b')
    from qualtran import QGF
    from dqi.rs_codes.rs_code_egcd_bloqs_zalka_eea import RSCodeEGCDZalkaEEA

    m, n, b = args.m, args.n, args.b
    dtype = QGF(2, b)
    parent = RSCodeEGCDZalkaEEA(m=m, n=n, dtype=dtype)
    bloq = parent.zalka_eea_bloq
    print(f'Building _PolyEEAZalkaImpl (core of RSCodeEGCDZalkaEEA, m={m}, n={n}, b={b})')
    print(f'  n_eea_steps={parent.n_eea_steps}, poly_len={n+3}')
    print(f'  Expected stubs: ~1.26% at m=7 scale (IsEquals/LessThanK/GreaterThanK/UpdateFlag)')
    return bloq


def make_rs_egcd_dialog(args):
    """
    See qualtranCompiler/dqi/README.md for how the delta control flow was handled.
    """
    _require(args, 'm', 'n', 'b')
    from qualtran import QGF
    from dqi.rs_codes.rs_code_egcd_bloqs import RSCodeEGCD

    m, n, b = args.m, args.n, args.b
    dtype = QGF(2, b)
    print(f'Building RSCodeEGCD(m={m}, n={n}, dtype=GF(2^{b}))')
    return RSCodeEGCD(m=m, n=n, dtype=dtype)


# ---- Gate-level primitives (keep() leaves from opi_gate_cost_table.py) ---

def make_and_gate(args):
    """
    And gate: Qualtran's Toffoli equivalent via Clifford+T decomposition.
    """
    from qualtran.bloqs.mcmt.and_bloq import And
    print('Building And(cv1=1, cv2=1)')
    return And(cv1=1, cv2=1)


def make_toffoli(args):
    """
    Toffoli gate.
    """
    from qualtran.bloqs.basic_gates import Toffoli
    print('Building Toffoli()')
    return Toffoli()


def make_cswap(args):
    """
    CSWAP (controlled-SWAP) from dqi.basic_gates.
    --b sets the register data type width: dtype=QUInt(b).
    """
    _require(args, 'b')
    from qualtran import QUInt
    from dqi.basic_gates.basic_gates import CSWAP
    b = args.b
    print(f'Building CSWAP(dtype=QUInt({b}))')
    return CSWAP(dtype=QUInt(b))


def make_mcx(args):
    """
    MCX (multi-controlled X) from dqi.basic_gates.
    --n sets the number of controls.
    """
    _require(args, 'n')
    from dqi.basic_gates.basic_gates import MCX
    n_ctrl = args.n
    print(f'Building MCX(cvs=(1,)*{n_ctrl})')
    return MCX(cvs=(1,) * n_ctrl)


def make_addK_ctrl(args):
    """
    Controlled AddK: adds classical constant k to a QUInt(b) register, controlled.
    --b sets register bitsize, --k sets the constant.
    """
    _require(args, 'b', 'k')
    from qualtran import QUInt
    from qualtran.bloqs.arithmetic import AddK
    b, k = args.b, args.k
    print(f'Building AddK(dtype=QUInt({b}), k={k}).controlled()')
    return AddK(dtype=QUInt(b), k=k).controlled()


def make_addK_adj_ctrl(args):
    """
    Controlled adjoint AddK: uncomputes AddK(k), controlled.
    --b sets register bitsize, --k sets the constant.
    """
    _require(args, 'b', 'k')
    from qualtran import QUInt
    from qualtran.bloqs.arithmetic import AddK
    b, k = args.b, args.k
    print(f'Building AddK(dtype=QUInt({b}), k={k}).adjoint().controlled()')
    return AddK(dtype=QUInt(b), k=k).adjoint().controlled()


def make_gf2_add(args):
    """
    GF2Addition: XOR-based addition over GF(2^b).
    --b sets the field size exponent.
    Compilation has 4 CX gates per b-bit register.
    """
    _require(args, 'b')
    from qualtran.bloqs.gf_arithmetic import GF2Addition
    b = args.b
    print(f'Building GF2Addition(qgf=QGF(2, {b}))')
    return GF2Addition(qgf=_qgf(b))


def make_gf2_add_ctrl(args):
    """
    Controlled GF2Addition over GF(2^b).
    --b sets the field size exponent.
    """
    _require(args, 'b')
    from qualtran.bloqs.gf_arithmetic import GF2Addition
    b = args.b
    print(f'Building GF2Addition(qgf=QGF(2, {b})).controlled()')
    return GF2Addition(qgf=_qgf(b)).controlled()


def make_gf2_mul_opt(args):
    """
    GF2MulViaOptimizedPolyMul: DQI-optimized Karatsuba GF(2^b) multiplier.
    --b sets the field size exponent.
    Compilation forward + adjoint verified at b=3,6,8,12.
    """
    _require(args, 'b')
    from dqi.gf_arithmetic.gf_multiplication import GF2MulViaOptimizedPolyMul
    b = args.b
    print(f'Building GF2MulViaOptimizedPolyMul(dtype=QGF(2, {b}))')
    return GF2MulViaOptimizedPolyMul(dtype=_qgf(b))


def make_gf2_mul_mbuc(args):
    """
    GF2MulMBUCOpt: MBUC (matrix-based uncompute) GF(2^b) multiplier, DQI variant.
    --b sets the field size exponent.
    Compilation requires _assume_measure_x_fixed_outcome
    and _allow_classical_wires patches in compiler.py).
    """
    _require(args, 'b')
    from dqi.gf_arithmetic.gf_multiplication import GF2MulMBUCOpt
    b = args.b
    print(f'Building GF2MulMBUCOpt(qgf=QGF(2, {b}))')
    return GF2MulMBUCOpt(qgf=_qgf(b))


def make_synthesize_lr(args):
    """
    SynthesizeLRCircuit: synthesizes the L/R reduction circuit for GF(2^b) multiplication.
    --b sets the field size exponent (determines reduction matrix dimensions b×b).
    """
    _require(args, 'b')
    from qualtran.bloqs.gf_arithmetic.gf2_multiplication import GF2MulMBUC, SynthesizeLRCircuit
    b = args.b
    matrix = GF2MulMBUC(qgf=_qgf(b)).reduction_matrix_q
    print(f'Building SynthesizeLRCircuit(matrix shape={matrix.shape})')
    return SynthesizeLRCircuit(matrix=matrix)


_FACTORIES = {
    # DQI circuit-level primitives
    'prepare_sds':    make_prepare_sds,
    'qrom':           make_qrom,
    'rs_egcd_zalka':  make_rs_egcd_zalka,
    'rs_egcd_dialog': make_rs_egcd_dialog,
    'full_dqi':       make_full_dqi,
    # Gate-level primitives (opi_gate_cost_table.py keep() leaves)
    'and':            make_and_gate,
    'toffoli':        make_toffoli,
    'cswap':          make_cswap,
    'mcx':            make_mcx,
    'addK_ctrl':      make_addK_ctrl,
    'addK_adj_ctrl':  make_addK_adj_ctrl,
    'gf2_add':        make_gf2_add,
    'gf2_add_ctrl':   make_gf2_add_ctrl,
    'gf2_mul_opt':    make_gf2_mul_opt,
    'gf2_mul_mbuc':   make_gf2_mul_mbuc,
    'synthesize_lr':  make_synthesize_lr,
}


# ---------------------------------------------------------------------------
# Argument parsing
# ---------------------------------------------------------------------------

def build_parser():
    parser = argparse.ArgumentParser(
        description='Generate QASM 2.0 circuits for DQI primitives',
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument(
        '--primitive',
        type=str,
        required=True,
        choices=list(_FACTORIES.keys()),
        metavar='PRIMITIVE',
        help=(
            'Which circuit to compile. Choices: '
            + ', '.join(_FACTORIES.keys())
        ),
    )
    # DQI-scale parameters (required by RS/full-DQI/prepare_sds primitives)
    parser.add_argument('--m', type=int, default=None,
                        help='Number of constraints / RS codeword length')
    parser.add_argument('--n', type=int, default=None,
                        help='Number of variables / syndrome length; '
                             'also used as control count for mcx')
    parser.add_argument('--b', type=int, default=None,
                        help='Field size exponent GF(2^b); also used as '
                             'register bitsize for non-GF primitives')
    # Gate-level parameters
    parser.add_argument('--k', type=int, default=None,
                        help='Classical constant for addK_ctrl / addK_adj_ctrl')
    parser.add_argument('--target_bitsize', type=int, default=1,
                        help='QROM output register width in bits (default: 1). '
                             'Paper scale: --m 4095 --target_bitsize 73')
    # EEA variant
    parser.add_argument(
        '--algo',
        type=str,
        default='zalka',
        choices=['zalka', 'dialog'],
        help='EEA variant for RS decoder primitives (default: zalka)',
    )
    parser.add_argument('--out', type=str, default=None,
                        help='Output QASM file path (default: out/dqi_<primitive>[_params].qasm)')
    parser.add_argument('--quiet', action='store_true',
                        help='Suppress progress output')
    return parser


def _default_outfile(args):
    """Build a descriptive default output filename from the primitive and its params."""
    parts = [args.primitive]
    if args.m is not None:
        parts.append(f'm{args.m}')
    if args.n is not None:
        parts.append(f'n{args.n}')
    if args.b is not None:
        parts.append(f'b{args.b}')
    if args.k is not None:
        parts.append(f'k{args.k}')
    if args.target_bitsize != 1:
        parts.append(f't{args.target_bitsize}')
    stem = '_'.join(parts)
    out_dir = os.path.join(os.path.dirname(__file__), 'out')
    return os.path.join(out_dir, f'dqi_{stem}.qasm')


# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------

def main():
    parser = build_parser()
    args = parser.parse_args()

    if args.out is None:
        args.out = _default_outfile(args)

    bloq = _FACTORIES[args.primitive](args)

    verbose = not args.quiet
    os.makedirs(os.path.dirname(os.path.abspath(args.out)), exist_ok=True)
    qasm = bloq_to_qasm(bloq, outfile=args.out, verbose=verbose)

    if verbose:
        lines = qasm.count('\n')
        print(f'\nDone. {lines} lines written to {args.out}')


if __name__ == '__main__':
    main()
