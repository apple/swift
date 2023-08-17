// RUN: %empty-directory(%t)
// RUN: %target-build-swift -O -Xfrontend -disable-availability-checking %s -parse-as-library -module-name main -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: objc_interop
// REQUIRES: concurrency
// REQUIRES: executable_test
// REQUIRES: concurrency_runtime

// rdar://108260399
// REQUIRES: optimized_stdlib

import Foundation

@main struct M {
  @TaskLocal static var v: UUID = UUID()
  static func test(_ t: UUID) async {
    await Self.$v.withValue(t) {
      await Task.sleep(1)
      print(Self.$v.get())
    }
  }
  static func main() async {
    // CHECK: before
    print("before")
    await test(UUID())
    // CHECK: after
    print("after")
  }
}
