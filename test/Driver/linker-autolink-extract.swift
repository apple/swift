// RUN: %swiftc_driver -driver-print-jobs -target x86_64-unknown-linux-gnu -g %s | %FileCheck -check-prefix DEBUG_LINUX %s

// REQUIRES: autolink-extract
// UNSUPPORTED: freebsd

// DEBUG_LINUX: bin/swift
// DEBUG_LINUX-NEXT: bin/swift-autolink-extract
// DEBUG_LINUX-NEXT: bin/swift
// DEBUG_LINUX-NEXT: bin/swift-frontend -modulewrap
// DEBUG_LINUX-NEXT: bin/clang{{"? }}
// DEBUG_LINUX: -o main
// DEBUG_LINUX-NOT: dsymutil

