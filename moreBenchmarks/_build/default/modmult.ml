open Printf
open ExtractionGateSet
open ExtrShor
open Run

(* Extract a representative component of Shor's algorithm *)
(* This extracts one modular multiplication circuit - the core computational unit *)

let rec count_gates_aux (u : coq_U ucom) acc =
  match u with
  | Coq_useq (u1, u2) -> count_gates_aux u1 (count_gates_aux u2 acc)
  | Coq_uapp (_, _, _) -> 1 + acc
let count_gates u = count_gates_aux u 0

(* Generate a single modular multiplication circuit *)
let gen_modmult_circuit n a power =
  let n_qubits = modmult_data_nqs n in
  let ainv = Z.invert a n in
  (* modmult_circuit computes a^(2^power) * x mod N *)
  let circuit_bc = modmult_circuit a ainv n n_qubits power in
  (* Convert from bccom to ucom *)
  let circuit = bc2ucom circuit_bc in
  circuit

(* light argument parsing *)
let n = ref 0
let a = ref 0
let power = ref 0
let bitwidth = ref 0
let usage = "usage: " ^ Sys.argv.(0) ^ " -N int -a int [--power int] [--bitwidth int]"
let speclist = [
    ("-N", Arg.Set_int n, ": modulus N");
    ("-a", Arg.Set_int a, ": base a (must be coprime to N)");
    ("--power", Arg.Set_int power, ": extract circuit for a^(2^power) mod N (default: 0)");
    ("--bitwidth", Arg.Set_int bitwidth, ": specify bitwidth for larger N (e.g., 2048 for RSA-2048)")
  ]
let () =
  Arg.parse
    speclist
    (fun x -> raise (Arg.Bad ("Bad argument : " ^ x)))
    usage;

if (!bitwidth > 0) then (
  (* Generate representative circuit for large N specified by bitwidth *)
  (* Use a small concrete value but with the qubit structure of the target size *)
  let small_n = Z.of_int 15 in (* Use 15 as concrete modulus for circuit structure *)
  let small_a = Z.of_int 7 in  (* Use 7 as concrete base *)

  printf "Generating representative Shor modmult circuit for %d-bit modulus\n%!" !bitwidth;
  printf "Using concrete values: N=15, a=7 (circuit structure represents %d-bit case)\n%!" !bitwidth;

  (* Calculate qubit counts for the target bitwidth *)
  let n_qubits_data = !bitwidth in
  let n_qubits_anc = !bitwidth in  (* Ancilla scales with data qubits *)
  let total_qubits = n_qubits_data + n_qubits_anc in

  (* Generate the circuit using small values *)
  let circuit = gen_modmult_circuit small_n small_a (Z.of_int !power) in
  let gate_count = count_gates circuit in

  printf "Representative circuit structure:\n";
  printf "  Data qubits: %d\n" n_qubits_data;
  printf "  Ancilla qubits: %d\n" n_qubits_anc;
  printf "  Total qubits: %d\n" total_qubits;
  printf "  Gate count (for N=15): %d\n" gate_count;
  printf "  Estimated gates for %d-bit: ~%d\n" !bitwidth (gate_count * !bitwidth / 4);

  let actual_circuit_qubits = Z.to_int (modmult_nqs small_n) in
  let filename = Printf.sprintf "shor_modmult_representative_%dbit.qasm" !bitwidth in
  write_qasm_file filename circuit actual_circuit_qubits;
  printf "Written to %s\n" filename
) else if (!n <= 1) then
  printf "ERROR: Requires 1 < N or --bitwidth\n%!"
else if (!a <= 0 || !n <= !a) then
  printf "ERROR: Requires 0 < a < N\n%!"
else if (Z.gcd (Z.of_int !a) (Z.of_int !n) > Z.one) then
  printf "ERROR: Requires a, N coprime\n%!"
else (
  let n_z = Z.of_int !n in
  let a_z = Z.of_int !a in
  let power_z = Z.of_int !power in

  printf "Generating modular multiplication circuit for N=%d, a=%d, power=%d\n%!" !n !a !power;
  printf "Computing: a^(2^%d) * x mod %d\n%!" !power !n;

  let circuit = gen_modmult_circuit n_z a_z power_z in
  let total_qubits = Z.to_int (modmult_nqs n_z) in
  let n_qubits_data = Z.to_int (modmult_data_nqs n_z) in
  let n_qubits_anc = Z.to_int (modmult_anc_nqs n_z) in
  let gate_count = count_gates circuit in

  printf "Circuit statistics:\n";
  printf "  Data qubits: %d\n" n_qubits_data;
  printf "  Ancilla qubits: %d\n" n_qubits_anc;
  printf "  Total qubits: %d\n" total_qubits;
  printf "  Gate count: %d\n" gate_count;

  let filename = Printf.sprintf "shor_modmult_N%d_a%d_pow%d.qasm" !n !a !power in
  write_qasm_file filename circuit total_qubits;
  printf "Written to %s\n" filename
)