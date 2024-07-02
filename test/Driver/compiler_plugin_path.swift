// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s 2>^1 | %FileCheck %s

// REQUIRES: OS=macosx || OS=linux-gnu

// CHECK: -plugin-path {{[^ ]+}}/lib/swift/host/plugins
// CHECK-SAME: -plugin-path {{[^ ]+}}/local/lib/swift/host/plugins
