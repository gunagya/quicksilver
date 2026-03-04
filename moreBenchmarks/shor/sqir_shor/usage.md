# Shor's Algorithm Circuit Extractors

This directory contains two OCaml scripts for extracting representative subcircuits from SQIR's formally verified implementation of Shor's factoring algorithm.

## Scripts

### 1. `modmult.ml` - Single Modular Multiplication Extractor
Generates a single modular multiplication circuit: `a^(2^power) * x mod N`

### 2. `modexp.ml` - Full QPE Circuit Extractor
Generates complete Quantum Phase Estimation with all m controlled modular multiplications plus inverse QFT.

## Usage

### 1. Install Dependencies

```bash
# Install OCaml package manager (macOS)
brew install opam

# Initialize opam
opam init --disable-sandboxing -y
eval $(opam env)

# Install build tools
opam install dune zarith -y
```

### 2. Build

```bash
cd quicksilver/moreBenchmarks
eval $(opam env)
dune build
```

### 3. Run

```bash
# Single modular multiplication
dune exec ./modmult.exe -- -N 15 -a 7

# Full QPE circuit
dune exec ./modexp.exe -- -N 15 -a 7
```

## Usage

### Single Modular Multiplication (modmult.ml)

```bash
# Generate single modmult for N=15, a=7, power=0 (computes 7^1 mod 15)
dune exec ./modmult.exe -- -N 15 -a 7

# Generate for different power (computes 7^2 mod 15)
dune exec ./modmult.exe -- -N 15 -a 7 --power 1

# Generate for N=21
dune exec ./modmult.exe -- -N 21 -a 5

# Bigger examples

# RSA-17, works in like 1 min with a 500MB stack
dune exec ./modmult.exe -- -N 65537 -a 3 --power 0

# RSA-24, works in like 1.5 mins with a 1000MB stack
dune exec ./modmult.exe -- -N 16777259 -a 3 --power 0
```

**Output:** `shor_modmult_N{N}_a{a}_pow{power}.qasm`

**Parameters:**
- `-N int` : Modulus (must be > 1 and coprime with a)
- `-a int` : Base (must be 0 < a < N and coprime with N)
- `--power int` : Power parameter (computes a^(2^power) mod N), default: 0
- `--bitwidth int` : Generate representative structure for specified bitwidth

The bitwidth flag, e.g. 
```bash
dune exec ./modmult.exe -- --bitwidth 2048
```
will generate an N=15 circuit with 2048-bit statistics. It won't write the actual qasm file. 


### Increasing the OCaml stack size

```bash
cd quicksilver/moreBenchmarks/shor && cat << 'EOF' > run_with_large_stack.sh
#!/bin/bash
# Increase OCaml stack size
export OCAMLRUNPARAM='l=500M'  # 500MB stack (default ~1MB)
eval $(opam env)
dune exec ./modmult.exe -- "$@"
EOF
chmod +x run_with_large_stack.sh
```

For something like RSA-256, you could try something like
```bash
export OCAMLRUNPARAM='l=10G,h=30G'  # 10GB stack, 30GB heap
ulimit -s unlimited                  # Remove OS stack limit
ulimit -v unlimited                  # Remove virtual memory limit
```

but might be wiser to try a streaming approach which incrementally writes gates, consuming constant memory. 

### Full QPE Circuit (modexp.ml)

```bash
# Generate full QPE for N=15 (m=8 controlled modmults)
dune exec ./modexp.exe -- -N 15 -a 7

# Generate full QPE for N=21 (m=9 controlled modmults)
dune exec ./modexp.exe -- -N 21 -a 5

# Generate full QPE for N=35 (m=11 controlled modmults)
dune exec ./modexp.exe -- -N 35 -a 3
```

**Output:** `shor_qpe_N{N}_a{a}.qasm`

**Parameters:**
- `-N int` : Modulus (must be > 1 and coprime with a)
- `-a int` : Base (must be 0 < a < N and coprime with N)

Practical limit is N < 100. 

### SQIR Library Modules

All SQIR library modules are included in `helpers/` directory:

**Core modules:**
- `ExtractionGateSet.ml` - Gate definitions and QASM conversion
- `ExtrShor.ml` - Shor's algorithm circuit components
- `Run.ml` - QASM file I/O
- `RCIR.ml` - Reversible circuit intermediate representation
- `ModMult.ml` - Modular multiplication circuits

**Supporting modules:**
- `Nat.ml`, `PeanoNat.ml` - Natural number operations
- `Datatypes.ml` - Basic data structures
- `List0.ml` - List operations
- `DiscreteProb.ml` - Discrete probability
- `ContFrac.ml` - Continued fractions
- `Main.ml`, `Shor.ml` - Main algorithm components
- `RealAux.ml`, `Summation.ml` - Auxiliary functions

## Works cited

These scripts are derived from the SQIR project:
- **Repository:** https://github.com/inQWIRE/SQIR
- **License:** MIT License
- **Paper:** Hietala, K., Rand, R., Hung, S.H., Wu, X., & Hicks, M. (2021). A verified optimizer for Quantum circuits. Proceedings of the ACM on Programming Languages, 5(POPL), 1–29.