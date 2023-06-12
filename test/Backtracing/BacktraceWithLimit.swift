// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -o %t/BacktraceWithLimit
// RUN: %target-codesign %t/BacktraceWithLimit
// RUN: %target-run %t/BacktraceWithLimit | %FileCheck %s

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

import _Backtracing

func doFrames(_ count: Int) {
  if count <= 0 {
    let backtrace = try! Backtrace.capture(limit: 10, top: 0)

    print(backtrace)
  } else {
    doFrames(count - 1)
  }
}

@main
struct BacktraceWithLimit {
  static func main() {
    // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 1{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 2{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 3{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 4{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 5{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 6{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 7{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 8{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 9{{[ \t]+}}...
    doFrames(1000)

    print("")

    // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 1{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 2{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 3{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 4{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    doFrames(5)
  }
}
