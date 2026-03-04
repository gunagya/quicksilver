#!/bin/bash
# Increase OCaml stack size
export OCAMLRUNPARAM='l=1000M'  # 1000MB stack (default ~1MB)
eval $(opam env)
dune exec ./modmult.exe -- "$@"
