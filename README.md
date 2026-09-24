# Quicksilver / Trident artifact

This repository contains the compiler, simulator, benchmark inputs, and plotting
notebook used to evaluate Trident. The artifact entry point, `run_artifact.py`,
compiles and simulates the configurations consumed by
[`results/paper_plots.ipynb`](results/paper_plots.ipynb), extracts their results to
CSV, and optionally executes the notebook to produce the paper figures and tables.

## Requirements and setup

Run the commands below from the repository root. You need Python 3.10 or newer,
CMake 3.20.3 or newer, a C++23 compiler (for example GCC 13), and the development
headers/libraries for zlib and liblzma. On Debian/Ubuntu, the library packages are
`zlib1g-dev` and `liblzma-dev`; the Python environment also needs `venv` support.

```bash
python3 -m venv venv
source venv/bin/activate
python3 -m pip install -r requirements-artifact.txt
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCOMPILE_BINARY_GENERATION=OFF
cmake --build build --target qs_memory_scheduler yoked_simulator -j2
```

If the default C++ compiler is too old, select a suitable compiler during the
initial CMake configuration, for example with `-DCMAKE_CXX_COMPILER=g++-13`.
`COMPILE_BINARY_GENERATION=OFF` uses the provided raw benchmark binaries and avoids
the optional dependencies needed to generate them from their original sources.
The runner expects the two built executables under `build/`; it does not run CMake.
CSV extraction alone uses the Python standard library. The Python packages above
are required for `--plots` and interactive notebook use, including reading the
Numbers spreadsheets on Linux without Apple Numbers.

The artifact distribution must include these compressed input binaries under
`benchmarks/bin/` (or a directory supplied with `--binary-dir`):

| Benchmark label for `--benchmarks` | Raw binary |
| --- | --- |
| `bose_hubbard_q` | `BQ_bose_hubbard_q.xz` |
| `bose_hubbard_t` | `BQ_bose_hubbard_t.xz` |
| `ethylene_oxide_q_prepare` | `BQ_c2h4o_ethylene_oxide_q_prepare.xz` |
| `ethylene_oxide_q_select` | `BQ_c2h4o_ethylene_oxide_q_select.xz` |
| `ethylene_oxide_t` | `BQ_c2h4o_ethylene_oxide_t.xz` |
| `chromium_q_prepare` | `BQ_chromium_q_prepare.xz` |
| `chromium_q_select` | `BQ_chromium_q_select.xz` |
| `chromium_t` | `BQ_chromium_t.xz` |
| `shor_rsa24` | `shor_modmult_N16777259_a3_pow0.xz` |
| `grover_3sat` | `BQ_grover_3sat_schoning_1710.xz` |

These files total approximately 314 MB compressed and are ignored by Git. A Git
checkout alone does not supply them: include `benchmarks/bin/*.xz` in the artifact
archive. The runner checks for the selected inputs and does not download them.

## Reproduce the figures and tables

```bash
python3 run_artifact.py --plots
```

This runs all ten benchmarks, then extracts the nine required CSV files into
`results/` and executes the plotting notebook. Omitting `--plots` stops after CSV
extraction. The executed notebook is saved as `results/paper_plots.executed.ipynb`;
the source notebook is preserved. PDFs are written into the results directory by
default. Tables, including application fidelity and space overhead, appear in the
executed notebook. `artifact_results.json` records the extraction inputs and run
metadata alongside the CSVs.

The default is one benchmark at a time. `--jobs N` permits N benchmarks to run in
parallel; each benchmark executes its dependent compilation/simulation steps in
order. Use `--benchmarks LABEL [LABEL ...]` to select workloads from the table
above. The runner stops on a failed command or incomplete extraction instead of
silently accepting missing results.

To inspect the exact commands without running them or writing outputs:

```bash
python3 run_artifact.py --dry-run
```

## Validate using existing logs

If compilation and simulation have already completed:

```bash
python3 run_artifact.py --extract-only --plots
```

The default log root is `build/yoked_codes_run_all_workloads/logs`. A log root must
contain the existing `compile/` and `simulate/` subdirectories. To use an archived
log tree and keep newly extracted data separate:

```bash
python3 run_artifact.py --extract-only --log-dir /path/to/logs --results-dir results/validation --plots
```

`--extract-only` runs only the eight required extractor scripts and checks that the
selected benchmarks/configurations have the data consumed by the notebook. It
requires neither raw benchmark binaries nor the built compiler/simulator. The
notebook reads generated CSVs only from the selected results directory, so missing
files cannot be replaced silently by older repository CSVs.

## Quick compilation and simulation check

Use a small instruction budget to validate the pipeline before a full run:

```bash
python3 run_artifact.py --instructions 100 --benchmarks bose_hubbard_q --plots
python3 run_artifact.py --instructions 1000 --run-only
python3 run_artifact.py --instructions 1000 --extract-only --plots
```

`--instructions N` sets every simulation's instruction target to N. The existing
compiler and simulator need trace lookahead: second-pass compilation uses
N + 2,000,000 instructions and first-pass compilation uses N + 4,000,000. Keeping
this headroom avoids their end-of-file failure on tiny traces without modifying
the compiler or simulator. By default, these runs use
`build/artifact_smoke_N/` and `results/smoke_N/`, keeping full-run outputs separate.
The first command executes the complete pipeline for one benchmark. The second
runs compilation and simulation for all ten benchmarks without extraction; the
third extracts those short-run logs and makes plots.

Short runs validate execution and data flow; they do not reproduce the paper's
numerical results. Instruction limits are stopping targets, and a compiler or
simulator may finish a layer or operation beyond the requested count. Without
`--instructions`, the workload defaults are 100 million simulated instructions
and 250 million first-pass compilation instructions (200 million for
`bose_hubbard_t`). Second-pass compilation uses 2 million fewer instructions than
the corresponding first pass.

## Output locations and notebook use

| Option | Purpose and default |
| --- | --- |
| `--build-dir DIR` | Built `qs_memory_scheduler` and `yoked_simulator`; default `build/` |
| `--binary-dir DIR` | Raw compressed benchmark inputs; default `benchmarks/bin/` |
| `--run-dir DIR` | Compilation/simulation working outputs and logs; default `build/yoked_codes_run_all_workloads/`, or `build/artifact_smoke_N/` with an instruction limit |
| `--log-dir DIR` | Override the input log root for `--extract-only` |
| `--results-dir DIR` | CSVs and executed notebook; default `results/`, or `results/smoke_N/` with an instruction limit |
| `--plot-dir DIR` | PDF output directory; default the results directory |
| `--plot-timeout SECONDS` | Per-cell notebook execution timeout; default 600 |

Within the run directory, compiled traces are stored under `binaries/firstpass/`,
`binaries/secondpass/cache/`, and `binaries/secondpass/prefetch/`; command logs are
stored under `logs/compile/` and `logs/simulate/`.

Explicit output-directory options override the automatic short-run locations.
Existing files in the chosen output directories can be overwritten. There is no
resume mode; `--run-only` and `--extract-only` separate the two pipeline stages.

To explore the notebook interactively after extraction:

```bash
python3 -m jupyterlab results/paper_plots.ipynb
```

Select a kernel from the environment with `requirements-artifact.txt` installed.
The notebook discovers the repository from its working directory. Optional
environment variables `QUICKSILVER_RESULTS_DIR` and `QUICKSILVER_PLOT_DIR` select
the CSV input and PDF output directories. Set `QUICKSILVER_REPO_ROOT` as well if
the kernel starts outside the repository. `run_artifact.py --plots` configures
these paths automatically.

Run the artifact orchestration checks with:

```bash
python3 -m unittest discover -s tests -p 'test_artifact*.py'
```
