// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -disable-availability-checking -g %s -parse-as-library -module-name main -emit-ir | %FileCheck %s --check-prefix=CHECK-LL
// RUN: %target-build-swift  -Xfrontend -disable-availability-checking -g %s -parse-as-library -module-name main -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: swift_test_mode_optimize_none
// REQUIRES: concurrency
// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

import Swift
import _Concurrency

// CHECK-LL: @voidToInt64Tu =
// CHECK-LL: define hidden swift{{(tail)?}}cc void @voidToInt64(ptr swiftasync {{%[0-9]+}}) {{#[0-9]*}}
@_silgen_name("voidToInt64")
func voidToInt64() async -> Int64 { return 42 }

@_silgen_name("test_case")
func test_case() async {
  let int = await voidToInt64()
  print(int) // CHECK: 42
}

@main struct Main {
  static func main() async {
    await test_case()
  }
}
