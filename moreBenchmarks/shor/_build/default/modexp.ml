(*
  Script to generate a complete Quantum Phase Estimation (QPE) circuit with all m
  controlled modular exponentiations from Shor's factoring algorithm. This includes
  m controlled modular multiplications (where m = log₂(2N²)) followed by an inverse
  Quantum Fourier Transform, representing the complete phase estimation component
  of Shor's algorithm.

  Important note:
  ---------------

  This file is derived from the SQIR repository (https://github.com/inQWIRE/SQIR)
  and is subject to the terms of the MIT License found at:
  https://github.com/inQWIRE/SQIR/blob/main/LICENSE.md

  It is part of the code from the paper:

  Hietala, K., Rand, R., Hung, S.H., Wu, X., & Hicks, M. (2021).
  A verified optimizer for Quantum circuits.
  Proceedings of the ACM on Programming Languages, 5(POPL), 1–29.

  Modifications to the original code:

  1. Created standalone QPE extraction utility using coq_QPE function. Does not include intial X gate or classical post-processing. Does include:
     - Hadamard gates on m output qubits
     - m controlled modular multiplications for powers 0 to m-1
     - Inverse QFT on output qubits

  2. Added command-line argument parsing with parameters:
     - Modulus N parameter (-N)
     - Base a parameter (-a)

  3. Implemented gate counting and analysis functionality including:
     - Total gate count across all m controlled modular multiplications
     - Average gates per modular multiplication
     - QPE structure statistics (m, data qubits, ancilla qubits)

  Dependencies:
  -------------
  - OCaml compiler (tested with 5.4.0)
  - SQIR library modules: ExtractionGateSet, ExtrShor, Run
  - zarith library (arbitrary precision integers)
  - dune build system

  Build instructions:
  -------------------
  Place in SQIR/examples/shor/extraction/ directory and build with:
    dune build

  Usage:
  ------
  # Full QPE for N=15 (m=8 controlled modmults)
  dune exec ./modexp.exe -- -N 15 -a 7

  # Full QPE for N=21 (m=9 controlled modmults)
  dune exec ./modexp.exe -- -N 21 -a 5

  # Full QPE for N=35 (m=11 controlled modmults)
  dune exec ./modexp.exe -- -N 35 -a 3

  Note: Practical limit is N < 100 due to circuit generation time and size.
        For larger N, use modmult.ml to generate representative single modmult.
*)

open Printf
open ExtractionGateSet
open ExtrShor
open Run

(* Extract full QPE circuit for Shor's algorithm *)
(* This includes all m modular multiplications - the complete phase estimation *)

let rec count_gates_aux (u : coq_U ucom) acc =
  match u with
  | Coq_useq (u1, u2) -> count_gates_aux u1 (count_gates_aux u2 acc)
  | Coq_uapp (_, _, _) -> 1 + acc
let count_gates u = count_gates_aux u 0

(* Generate full QPE circuit with all m modular exponentiations *)
let gen_full_qpe_circuit n a =
  let n_qubits = modmult_data_nqs n in
  let ainv = Z.invert a n in
  let m = shor_output_nqs n in  (* m = log2(2 * N^2) - number of QPE output qubits *)
  (* Create function that generates modmult for each power i *)
  let f i = modmult_circuit a ainv n n_qubits i in
  (* Generate full QPE circuit *)
  let circuit = coq_QPE m f in
  circuit

(* light argument parsing *)
let n = ref 0
let a = ref 0
let usage = "usage: " ^ Sys.argv.(0) ^ " -N int -a int"
let speclist = [
    ("-N", Arg.Set_int n, ": modulus N");
    ("-a", Arg.Set_int a, ": base a (must be coprime to N)")
  ]
let () =
  Arg.parse
    speclist
    (fun x -> raise (Arg.Bad ("Bad argument : " ^ x)))
    usage;

if (!n <= 1) then
  printf "ERROR: Requires 1 < N\n%!"
else if (!a <= 0 || !n <= !a) then
  printf "ERROR: Requires 0 < a < N\n%!"
else if (Z.gcd (Z.of_int !a) (Z.of_int !n) > Z.one) then
  printf "ERROR: Requires a, N coprime\n%!"
else (
  let n_z = Z.of_int !n in
  let a_z = Z.of_int !a in

  printf "Generating full QPE circuit for N=%d, a=%d\n%!" !n !a;

  let m = Z.to_int (shor_output_nqs n_z) in
  let n_qubits_data = Z.to_int (modmult_data_nqs n_z) in
  let n_qubits_anc = Z.to_int (modmult_anc_nqs n_z) in

  printf "QPE structure:\n";
  printf "  QPE output qubits (m): %d\n" m;
  printf "  Data qubits: %d\n" n_qubits_data;
  printf "  Ancilla qubits: %d\n" n_qubits_anc;
  printf "  Number of controlled modmults: %d\n" m;
  printf "\nGenerating circuit (this may take a while for large N)...\n%!";

  let circuit = gen_full_qpe_circuit n_z a_z in
  let total_qubits = Z.to_int (shor_nqs n_z) in
  let gate_count = count_gates circuit in

  printf "\nCircuit statistics:\n";
  printf "  Total qubits: %d\n" total_qubits;
  printf "  Total gates: %d\n" gate_count;
  printf "  Gates per modmult (avg): %d\n" (gate_count / m);

  let filename = Printf.sprintf "out/shor_qpe_N%d_a%d.qasm" !n !a in
  write_qasm_file filename circuit total_qubits;
  printf "\nWritten to %s\n" filename
)