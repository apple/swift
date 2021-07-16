// RUN: %empty-directory(%t)

// RUN: %target-build-swift-dylib(%t/%target-library-name(resilient_async)) -Xfrontend -enable-experimental-async-main -enable-library-evolution %S/Inputs/resilient_async.swift -emit-module -emit-module-path %t/resilient_async.swiftmodule -module-name resilient_async
// RUN: %target-codesign %t/%target-library-name(resilient_async)

// RUN: %target-build-swift -Xfrontend -enable-experimental-async-main -parse-as-library %s -lresilient_async -I %t -L %t -o %t/main %target-rpath(%t)
// RUN: %target-codesign %t/main

// Introduce a defaulted protocol method.
// RUN: %target-build-swift-dylib(%t/%target-library-name(resilient_async)) -Xfrontend -enable-experimental-async-main -enable-library-evolution %S/Inputs/resilient_async2.swift -emit-module -emit-module-path %t/resilient_async.swiftmodule -module-name resilient_async
// RUN: %target-codesign %t/%target-library-name(resilient_async)

// RUN: %target-run %t/main %t/%target-library-name(resilient_async)

// REQUIRES: executable_test
// REQUIRES: concurrency
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

import resilient_async

class Impl : Problem {}

@main struct Main {
  static func main() async {
      let i = Impl()
      // This used to crash.
      let r = await callGenericWitness(i)
      assert(r == 1)
  }
}
