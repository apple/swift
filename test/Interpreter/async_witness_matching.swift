// RUN: %empty-directory(%t)
// RUN: %target-build-swift  -Xfrontend -disable-availability-checking %s -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// Ensures the more exact witness from S is chosen
// to fulfill the requirement from P.
// See: https://github.com/apple/swift/issues/60318

protocol P {
  func foo() async -> String
}

struct S: P {
  func foo() -> String { "plain" }
  func foo() async -> String { "async" }
}

let p: P = S()
let ans = await p.foo()
print(ans)
// CHECK: async
