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

open ContFrac
open Datatypes
open Nat

(** val coq_OF_post_step : Z.t -> Z.t -> Z.t -> Z.t **)

let coq_OF_post_step step o m =
  snd
    (coq_ContinuedFraction step o
      (PeanoNat.Nat.pow (Z.succ (Z.succ Z.zero)) m))

(** val coq_OF_post' : Z.t -> Z.t -> Z.t -> Z.t -> Z.t -> Z.t **)

let rec coq_OF_post' step a n o m =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> Z.zero)
    (fun step' ->
    let pre = coq_OF_post' step' a n o m in
    if Z.equal pre Z.zero
    then if Z.equal (Z.powm a (coq_OF_post_step step' o m) n) (Z.succ Z.zero)
         then coq_OF_post_step step' o m
         else Z.zero
    else pre)
    step

(** val coq_OF_post : Z.t -> Z.t -> Z.t -> Z.t -> Z.t **)

let coq_OF_post a n o m =
  coq_OF_post'
    (add (mul (Z.succ (Z.succ Z.zero)) m) (Z.succ (Z.succ Z.zero))) a n o m
