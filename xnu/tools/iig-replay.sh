#!/bin/bash
# iig-replay: stand-in for Xcode's iig (xnu's makefiles run $IIG). Xcode 16's iig emits
# IORPC.kernelContent, which xnu-8796 lacks, so this copies the Xcode 15 outputs pregenerated
# in xnu/iig-pregen/ instead, after checking the .iig still matches SHA256SUMS.iig.
# IIG_PREGEN overrides the directory.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
pregen=${IIG_PREGEN:-$here/../iig-pregen}

def= header= impl=
while [ $# -gt 0 ]; do
	case $1 in
	--) break ;;
	--version | --help)
		echo "iig-replay (pregenerated DriverKit-286 outputs)"
		exit 0 ;;
	--def=*) def=${1#--def=} ;;
	--header=*) header=${1#--header=} ;;
	--impl=*) impl=${1#--impl=} ;;
	--def) def=${2:?"$1 needs a value"}; shift ;;
	--header) header=${2:?"$1 needs a value"}; shift ;;
	--impl) impl=${2:?"$1 needs a value"}; shift ;;
	--framework-name | --deployment-target) : "${2:?"$1 needs a value"}"; shift ;;
	esac
	shift
done

[ -n "$def" ] || { echo "iig-replay: --def is required" >&2; exit 2; }
name=$(basename "$def" .iig)

missing=
if [ -n "$header" ] && [ ! -f "$pregen/$name.h" ]; then missing=1; fi
if [ -n "$impl" ] && [ ! -f "$pregen/$name.iig.cpp" ]; then missing=1; fi
if [ -n "$missing" ]; then
	echo "iig-replay: no pregenerated output for $name (did an .iig change? regenerate xnu/iig-pregen on macos-14)" >&2
	exit 1
fi

sums=$pregen/SHA256SUMS.iig
if [ -f "$sums" ]; then
	want=$(awk -v f="$name.iig" '$2 == f { print $1 }' "$sums")
	if [ -n "$want" ]; then
		if command -v shasum >/dev/null 2>&1; then
			got=$(shasum -a 256 "$def" | cut -d' ' -f1)
		else
			got=$(sha256sum "$def" | cut -d' ' -f1)
		fi
		if [ "$got" != "$want" ]; then
			echo "iig-replay: $name.iig differs from the one the pregenerated output came from" >&2
			exit 1
		fi
	fi
fi

if [ -n "$header" ]; then
	mkdir -p "$(dirname "$header")"
	cp "$pregen/$name.h" "$header"
fi
if [ -n "$impl" ]; then
	mkdir -p "$(dirname "$impl")"
	cp "$pregen/$name.iig.cpp" "$impl"
fi
