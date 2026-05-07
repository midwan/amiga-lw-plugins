#!/bin/sh
set -eu

src="${1:-src/pbr/pbr.c}"

fail() {
	printf '%s\n' "$1" >&2
	exit 1
}

grep -Eq 'static[[:space:]]+int[[:space:]]+blur_trace_depth' "$src" ||
	fail "missing static blur recursion depth guard"

grep -Eq 'blur_trace_depth[[:space:]]*==[[:space:]]*0' "$src" ||
	fail "blur path is not gated to primary shader evaluation"

awk '
	/blur_trace_depth\+\+/ { enter = NR }
	/\(\*sa->rayTrace\)\(pos, dir, col\)/ { ray = NR }
	/blur_trace_depth--/ { leave = NR }
	END {
		if (!enter || !ray || !leave || !(enter < ray && ray < leave))
			exit 1
	}
' "$src" || fail "blur recursion guard does not wrap rayTrace calls"
