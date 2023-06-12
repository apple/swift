#!/bin/bash
#
#  Strip a binary and generate a separate .dbg that gets referred to using
#  .gnu_debuglink.
#

set -e

# Check we have the required arguments
if [[ $# -ne 3 ]]; then
    cat >2 <<EOF
usage: make-debuglink <original> <stripped> <debug>

Take <original>, which contains symbols, strip it to create <stripped>, and
extract the debug information into a separate <debug> file, adding a
'.gnu_debuglink' section to <stripped> that points at it.
EOF
    exit 1
fi

# Grab the arguments
original=$1
stripped=$2
debug=$3

# Create a temporary directory to work in
tempdir=$(mktemp -d -t tmp.XXXXXXXXXXX)
function cleanup {
    rm -rf "$tempdir"
}
trap cleanup EXIT

# Construct the debug file
nm -D "$original" --format=posix --defined-only \
    | awk '{ print $1 }' | sort > "$tempdir/dynsyms"
nm "$original" --format=posix --defined-only \
    | awk '$2 ~ /[TtD]/ { print $1 }' \
    | sort > "$tempdir/funcsyms"

comm -13 "$tempdir/dynsyms" "$tempdir/funcsyms" > "$tempdir/keepsyms"

objcopy --only-keep-debug \
        --remove-section .gdb_index \
        --remove-section .comment \
        --keep-symbols="$tempdir/keepsyms" \
        "$original" "$debug"

objcopy -S --add-gnu-debuglink="$debug" "$original" "$stripped"
