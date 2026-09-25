#!/usr/bin/env bash
# A vertex-program pow() of a vector base must compute each lane from its own
# base lane (t_07866923).  vp_pow_vector_check.py EXECUTES the compiled
# container (vector and scalar units) and compares every lane with pow().
# usage: vp-pow-vector-test.sh <rsx-cg-compiler>
set -u
here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
compiler="${1:?usage: $0 <rsx-cg-compiler>}"
python3 "$here/vp_pow_vector_check.py" "$compiler"
