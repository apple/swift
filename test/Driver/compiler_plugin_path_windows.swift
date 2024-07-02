// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s 2>^1 | %FileCheck %s

// REQUIRES: OS=windows

// CHECK: -plugin-path {{[^ ]+}}\bin
