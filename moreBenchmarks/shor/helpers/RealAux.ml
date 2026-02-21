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

open Summation

(** val coq_R_is_monoid : float coq_Monoid **)

let coq_R_is_monoid =
  { coq_Gzero = (Z.to_float Z.zero); coq_Gplus = ( +. ) }
