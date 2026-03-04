# DQI QASM Benchmark generator usage

`gen_dqi_qasm.py` compiles Qualtran Bloqs from the DQI circuit into QASM 2.0 files.
It covers the full set of primitives tracked in the `opi_gate_cost_table.py` keep() leaves,
plus some DQI sub-programs.

---

## Setup

### 1. Get the branch

```bash
git clone https://github.com/gunagya/quicksilver.git
cd quicksilver
git checkout kdbenchmarks
```

### 2. Install qualtranCompiler

`qualtranCompiler` contains the Qualtran→QASM compiler and the bundled `dqi` source.
Install it from GitHub into a Python 3.12+ virtual environment:

```bash
python -m venv venv
source venv/bin/activate

pip install "qualtranCompiler @ git+ssh://git@github.com/KabirDubey/qualtranCompiler.git"
```

This should install all dependencies automatically (cirq-core, qualtran, galois, numpy, …).

### 3. Run

```bash
cd moreBenchmarks/dqi
python gen_dqi_qasm.py --primitive <name> [params...]
```

Output files are written to `moreBenchmarks/dqi/out/` by default.
Pass `--out path/to/file.qasm` to override.

---

## Primitives

### Gate-level primitives (opi_gate_cost_table.py keep() leaves)

These are the terminal bloqs used by `opi_gate_cost_table.py` in the `dqi` resource estimation paper for resource counting.

| Primitive | Bloq | Required args | Example |
|---|---|---|---|
| `and` | `And(cv1=1, cv2=1)` | none | `--primitive and` |
| `toffoli` | `Toffoli()` | none | `--primitive toffoli` |
| `cswap` | `CSWAP(QUInt(b))` | `--b` | `--primitive cswap --b 12` |
| `mcx` | `MCX(cvs=(1,)*n)` | `--n` | `--primitive mcx --n 4` |
| `addK_ctrl` | `AddK(QUInt(b), k).controlled()` | `--b --k` | `--primitive addK_ctrl --b 12 --k 3` |
| `addK_adj_ctrl` | `AddK(QUInt(b), k).adjoint().controlled()` | `--b --k` | `--primitive addK_adj_ctrl --b 12 --k 3` |
| `gf2_add` | `GF2Addition(QGF(2,b))` | `--b` | `--primitive gf2_add --b 12` |
| `gf2_add_ctrl` | `GF2Addition(QGF(2,b)).controlled()` | `--b` | `--primitive gf2_add_ctrl --b 12` |
| `gf2_mul_opt` | `GF2MulViaOptimizedPolyMul(QGF(2,b))` | `--b` | `--primitive gf2_mul_opt --b 12` |
| `gf2_mul_mbuc` | `GF2MulMBUCOpt(QGF(2,b))` | `--b` | `--primitive gf2_mul_mbuc --b 12` |
| `synthesize_lr` | `SynthesizeLRCircuit(reduction_matrix_q)` | `--b` | `--primitive synthesize_lr --b 12` |
| `qrom` | `QROM(data, sel_bits, target_bits)` | `--m` | `--primitive qrom --m 4095` |
| `qrom` (paper scale) | QROM with 73-bit output | `--m --target_bitsize` | `--primitive qrom --m 4095 --target_bitsize 73` |

### DQI circuit-level primitives

| Primitive | Bloq | Required args | Notes |
|---|---|---|---|
| `prepare_sds` | `PrepareSparseDickeState(m, n//2)` | `--m --n` | 
| `rs_egcd_zalka` | `_PolyEEAZalkaImpl` (EEA core) | `--m --n --b` | `ConstructOrPhaseErrors` is unresolved |
| `rs_egcd_dialog` | `RSCodeEGCD` | `--m --n --b` | Conservative: OmegaMinusU delta always satisfied |

---

## Parameters

| Flag | Type | Default | Description |
|---|---|---|---|
| `--primitive` | str | required | Primitive name from the table above |
| `--m` | int | — | Codeword/constraint length (e.g. 4095) |
| `--n` | int | — | Syndrome length or MCX control count |
| `--b` | int | — | GF(2^b) field exponent / register bitsize |
| `--k` | int | — | Classical constant for AddK variants |
| `--target_bitsize` | int | 1 | QROM output width in bits |
| `--algo` | str | zalka | EEA variant: `zalka` or `dialog` |
| `--out` | str | auto | Override output file path |
| `--quiet` | flag | off | Suppress progress output |

---

## Usage examples

```bash
# Gate-level: no parameters needed
python gen_dqi_qasm.py --primitive and
python gen_dqi_qasm.py --primitive toffoli

# Gate-level: GF(2^12) arithmetic (paper field size)
python gen_dqi_qasm.py --primitive gf2_add       --b 12
python gen_dqi_qasm.py --primitive gf2_add_ctrl  --b 12
python gen_dqi_qasm.py --primitive gf2_mul_opt   --b 12
python gen_dqi_qasm.py --primitive gf2_mul_mbuc  --b 12
python gen_dqi_qasm.py --primitive synthesize_lr --b 12

# Gate-level: controlled integer arithmetic
python gen_dqi_qasm.py --primitive addK_ctrl     --b 12 --k 1
python gen_dqi_qasm.py --primitive addK_adj_ctrl --b 12 --k 1

# DQI sub-circuits — paper scale (prepare_sds and qrom are feasible)
python gen_dqi_qasm.py --primitive prepare_sds  --m 4095 --n 70 --b 12
python gen_dqi_qasm.py --primitive qrom         --m 4095 --n 70 --b 12
```

---

### What unresolved stubs mean

The compiler replaces each unresolvable gate with a QASM comment:

```
// [unresolved: BloqAsCirqGate(IsEquals(...))]
```

The surrounding QASM is valid and executable; only these specific operations are missing.
The stub rate is an upper bound on circuit correctness degradation: real-world impact
depends on whether the unresolved gates appear in the critical path.
