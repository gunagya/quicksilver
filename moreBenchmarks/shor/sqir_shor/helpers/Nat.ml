(*
  Important note:
  ---------------

  This file is derived from the SQIR repository (https://github.com/inQWIRE/SQIR)
  and is subject to the terms of the MIT License found at:
  https://github.com/inQWIRE/SQIR/blob/main/LICENSE.md

  It is part of the code from the paper:

  Hietala, K., Rand, R., Hung, S.H., Wu, X., & Hicks, M. (2021).
  A verified optimizer for Quantum circuits.
  Proceedings of the ACM on Programming Languages, 5(POPL), 1–29.
*)


(** val add : Z.t -> Z.t -> Z.t **)

let rec add n m =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> m)
    (fun p -> Z.succ (add p m))
    n

(** val mul : Z.t -> Z.t -> Z.t **)

let rec mul n m =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> Z.zero)
    (fun p -> add m (mul p m))
    n

(** val sub : Z.t -> Z.t -> Z.t **)

let rec sub n m =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> n)
    (fun k ->
    (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
      (fun _ -> n)
      (fun l -> sub k l)
      m)
    n

(** val max : Z.t -> Z.t -> Z.t **)

let rec max n m =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> m)
    (fun n' ->
    (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
      (fun _ -> n)
      (fun m' -> Z.succ (max n' m'))
      m)
    n
