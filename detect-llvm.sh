#!/usr/bin/env bash
# Requires: LLVM 7.0 installed in some known location.
# Usage: ./detect-llvm.sh, or sourced.

set -e

# XXX: Currently we stick to LLVM 7.

test_llvm7_prefix()
{
	for pfx in "/usr/lib/llvm-7" "/opt/llvm70" "/opt/llvm70" \
		"/usr/local" "/usr"; do
		if [[ -x "$pfx/bin/llvm-config" ]]; then
			if [[ $("$pfx/bin/llvm-config" --version) =~ 7\..+ ]]; then
				LLVM_PREFIX="$("$pfx/bin/llvm-config" --prefix)"
				LLVM_BINDIR="$("$pfx/bin/llvm-config" --bindir)"
				return
			fi
		fi
	done
}

test_llvm7_prefix
echo 'Found LLVM 7 prefix:' "$LLVM_PREFIX"
echo 'LLVM version:' "$("$LLVM_BINDIR/llvm-config" --version)"

