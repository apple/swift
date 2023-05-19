// REQUIRES: OS=macosx

// RUN: %empty-directory(%t)
// RUN: split-file %s %t

//# Prepare dummy Xcode.app
// RUN: mkdir -p %t/Xcode.app/Contents/Develoer
// RUN: mkdir -p %t/Xcode.app/Contents/Developer/usr/bin
// RUN: mkdir -p %t/Xcode.app/Contents/Developer/usr/lib/swift/host/plugins
// RUN: mkdir -p %t/Xcode.app/Contents/Developer/usr/local/lib/swift/host/plugins
// RUN: touch %t/Xcode.app/Contents/Developer/usr/bin/swiftc
// RUN: touch %t/Xcode.app/Contents/Developer/usr/bin/swift-plugin-server
// RUN: chmod +x %t/Xcode.app/Contents/Developer/usr/bin/swiftc
// RUN: chmod +x %t/Xcode.app/Contents/Developer/usr/bin/swift-plugin-server

//# Prepare dummy 'xcode-select' and 'xcrun'
// RUN: mkdir -p %t/usr/bin
// RUN: sed 's;TMPDIR;%t;' %t/xcode-select > %t/usr/bin/xcode-select
// RUN: sed 's;TMPDIR;%t;' %t/xcrun > %t/usr/bin/xcrun
// RUN: chmod +x %t/usr/bin/xcode-select
// RUN: chmod +x %t/usr/bin/xcrun 

// RUN: env PATH=%t/usr/bin %swift_driver_plain -### %t/test.swift | %FileCheck %s

// CHECK: -plugin-path BUILD_DIR/lib/swift/host/plugins
// CHECK-SAME: -plugin-path BUILD_DIR/local/lib/swift/host/plugins
// CHECK-SAME: -external-plugin-path BUILD_DIR/{{[^#]+}}/Xcode.app/Contents/Developer/usr/lib/swift/host/plugins#BUILD_DIR/{{[^#]+}}/Xcode.app/Contents/Developer/usr/bin/swift-plugin-server
// CHECK-SAME: -external-plugin-path BUILD_DIR/{{[^#]+}}/Xcode.app/Contents/Developer/usr/local/lib/swift/host/plugins#BUILD_DIR/{{[^#]+}}/Xcode.app/Contents/Developer/usr/bin/swift-plugin-server

//--- xcrun
#!/bin/sh
echo TMPDIR/Xcode.app/Contents/Developer/usr/bin/swiftc

//--- xcode-select
#!/bin/sh
echo TMPDIR/Xcode.app/Contents/Developer

//--- test.swift
print(1)
