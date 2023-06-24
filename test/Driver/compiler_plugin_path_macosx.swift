// RUN: %swiftc_driver -driver-print-jobs -target x86_64-apple-macosx10.9 %s 2>^1 | %FileCheck %s
// REQUIRES: OS=macosx

// CHECK: -external-plugin-path
// CHECK-SAME: .sdk/usr/lib/swift/host/plugins#
// CHECK-SAME: .sdk/usr/bin/swift-plugin-server

// CHECK-SAME: -external-plugin-path
// CHECK-SAME: .sdk/usr/local/lib/swift/host/plugins#
// CHECK-SAME: .sdk/usr/bin/swift-plugin-server

// CHECK-SAME: -external-plugin-path
// CHECK-SAME: MacOSX.platform/Developer/usr/lib/swift/host/plugins#
// CHECK-SAME: MacOSX.platform/Developer/usr/bin/swift-plugin-server

// CHECK-SAME: -external-plugin-path
// CHECK-SAME: MacOSX.platform/Developer/usr/local/lib/swift/host/plugins#
// CHECK-SAME: MacOSX.platform/Developer/usr/bin/swift-plugin-server

// CHECK-SAME: -plugin-path
// CHECK-SAME: {{(/|\\\\)}}lib{{(/|\\\\)}}swift{{(/|\\\\)}}host{{(/|\\\\)}}plugins

// CHECK-SAME: -plugin-path
// CHECK-SAME: {{(/|\\\\)}}local{{(/|\\\\)}}lib{{(/|\\\\)}}swift{{(/|\\\\)}}host{{(/|\\\\)}}plugins
