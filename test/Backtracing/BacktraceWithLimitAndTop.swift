// RUN: %empty-directory(%t)
// RUN: %target-build-swift %s -parse-as-library -Onone -o %t/BacktraceWithLimitAndTop
// RUN: %target-codesign %t/BacktraceWithLimitAndTop
// RUN: %target-run %t/BacktraceWithLimitAndTop | %FileCheck %s

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime
// REQUIRES: executable_test
// REQUIRES: backtracing
// REQUIRES: OS=macosx || OS=linux-gnu

import _Backtracing

func doFrames(_ count: Int, limit: Int, top: Int) {
  if count <= 0 {
    let backtrace = try! Backtrace.capture(limit: limit, top: top)

    print(backtrace)
  } else {
    doFrames(count - 1, limit: limit, top: top)
  }
}

@main
struct BacktraceWithTop {
  static func main() {
    // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 1{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 2{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 3{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 4{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 5{{[ \t]+}}...
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    doFrames(1000, limit: 10, top: 4)

    print("")

    // CHECK:      0{{[ \t]+}}...
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: {{[0-9]+[ \t]+}}0x{{[0-9a-f]+}} [ra]
    doFrames(1000, limit: 10, top: 10)

    print("")

    // CHECK:      0{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 1{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 2{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 3{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 4{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    // CHECK-NEXT: 5{{[ \t]+}}0x{{[0-9a-f]+}} [ra]
    doFrames(6, limit: 30, top: 4)
  }
}
