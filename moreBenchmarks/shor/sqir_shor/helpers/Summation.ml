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


type 'g coq_Monoid = { coq_Gzero : 'g; coq_Gplus : ('g -> 'g -> 'g) }

(** val big_sum : 'a1 coq_Monoid -> (Z.t -> 'a1) -> Z.t -> 'a1 **)

let rec big_sum h f n =
  (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
    (fun _ -> h.coq_Gzero)
    (fun n' -> h.coq_Gplus (big_sum h f n') (f n'))
    n
