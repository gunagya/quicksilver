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


module Nat =
 struct
  (** val pred : Z.t -> Z.t **)

  let pred = (fun a -> Z.max (Z.pred a) Z.zero)

  (** val sub : Z.t -> Z.t -> Z.t **)

  let rec sub = (fun a b -> Z.max (Z.sub a b) Z.zero)

  (** val pow : Z.t -> Z.t -> Z.t **)

  let rec pow = (fun a b -> Z.pow a (Z.to_int b))

  (** val divmod : Z.t -> Z.t -> Z.t -> Z.t -> Z.t * Z.t **)

  let rec divmod x y q u =
    (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
      (fun _ -> (q, u))
      (fun x' ->
      (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
        (fun _ -> divmod x' y (Z.succ q) y)
        (fun u' -> divmod x' y q u')
        u)
      x

  (** val log2_iter : Z.t -> Z.t -> Z.t -> Z.t -> Z.t **)

  let rec log2_iter k p q r =
    (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
      (fun _ -> p)
      (fun k' ->
      (fun fO fS n -> if Z.equal n Z.zero then fO () else fS (Z.pred n))
        (fun _ -> log2_iter k' (Z.succ p) (Z.succ q) q)
        (fun r' -> log2_iter k' p (Z.succ q) r')
        r)
      k

  (** val log2 : Z.t -> Z.t **)

  let log2 = fun n -> (Z.of_int (Z.log2 n))
 end
