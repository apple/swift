// REQUIRES: objc_interop
// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/clang-module-cache
// RUN: mkdir -p %t/Frameworks
// RUN: mkdir -p %t/Frameworks/E.framework/
// RUN: mkdir -p %t/Frameworks/E.framework/Modules
// RUN: mkdir -p %t/Frameworks/E.framework/Modules/E.swiftmodule

// RUN: echo "---" > %t/blocklist.yml
// RUN: echo "action:" >> %t/blocklist.yml

// Copy over the interface
// RUN: cp %S/Inputs/Swift/E.swiftinterface %t/Frameworks/E.framework/Modules/E.swiftmodule/%module-target-triple.swiftinterface


// Run the scan
// RUN: %target-swift-frontend -scan-dependencies %s -o %t/deps.json -F %t/Frameworks/ -sdk %t -blocklist-file %t/blocklist.yml
// RUN: %validate-json %t/deps.json | %FileCheck %s

import E

// CHECK: "-blocklist-file"
// CHECK-NEXT: {{.*}}blocklist.yml"
