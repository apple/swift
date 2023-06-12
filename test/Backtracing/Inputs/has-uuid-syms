#!/bin/bash
#
#  Test if the symbols for a given binary are installed, and if so return
#  the path to them.
#

set -e

if [[ $# -ne 1 ]]; then
    cat >2 <<EOF
usage: uuid-syms <binary>

Check whether a given binary has symbols installed in /usr/lib/debug/.build-id,
and if it does, return the path to them.
EOF
    exit 1
fi

input=$1
if ! uuid=$(readelf -n "$input" | awk '/Build ID:/ { print $3 }'); then
    echo "uuid-syms: $input has no uuid"
    exit 1
fi

prefix=${uuid:0:2}
suffix=${uuid:2}
dbgpath="/usr/lib/debug/.build-id/$prefix/$suffix.debug"
if [[ -f "$dbgpath" ]]; then
    echo "$dbgpath"
    exit 0
fi

exit 1

