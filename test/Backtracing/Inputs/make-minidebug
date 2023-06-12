#!/bin/bash
#
#  Generate MiniDebugInfo data (in the '.gnu_debugdata' section)
#

set -e

# Check we have the required arguments
if [[ $# -ne 2 ]]; then
    cat >2 <<EOF
usage: make-minidebug <input> <output>

Extract debug information from <input> and generate a new version, <output>,
containing a compressed '.gnu_debugdata' section.

Requires xz be installed on your machine.
EOF
    exit 1
fi

# Grab the arguments
input=$1
output=$2

# Check for the tools we need
for tool in nm comm objcopy xz strip; do
    if ! $(which $tool >/dev/null); then
        echo "make-minidebug: $tool not installed."
        exit 1
    fi
done

# Create a temporary directory to work in
tempdir=$(mktemp -d -t tmp.XXXXXXXXXXX)
function cleanup {
    rm -rf "$tempdir"
}
trap cleanup EXIT

# Actually construct the MiniDebug section

nm -D "$input" --format=posix --defined-only \
    | awk '{ print $1 }' | sort > "$tempdir/dynsyms"
nm "$input" --format=posix --defined-only \
    | awk '$2 ~ /[TtD]/ { print $1 }' \
    | sort > "$tempdir/funcsyms"

comm -13 "$tempdir/dynsyms" "$tempdir/funcsyms" > "$tempdir/keepsyms"

objcopy --only-keep-debug \
        --remove-section .gdb_index \
        --remove-section .comment \
        --keep-symbols="$tempdir/keepsyms" \
        "$input" "$tempdir/minidbginfo"

xz "$tempdir/minidbginfo"

objcopy -S --remove-section .comment \
        --add-section .gnu_debugdata="$tempdir/minidbginfo.xz" \
        "$input" "$output"
