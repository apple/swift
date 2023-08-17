// RUN: %target-run-simple-swift(-Xfrontend -disable-availability-checking -parse-as-library)

// REQUIRES: executable_test
// REQUIRES: concurrency
// REQUIRES: objc_interop

// UNSUPPORTED: back_deployment_runtime
// UNSUPPORTED: use_os_stdlib

// Make sure the concurrency runtime tolerates dynamically-subclassed actors.

import ObjectiveC

actor Foo: NSObject {
  var x = 0

  func doit() async {
    x += 1
    try! await Task.sleep(nanoseconds: 1000)
    x += 1
  }
}

@main
enum Main {
  static func main() async {
    let FooSub = objc_allocateClassPair(Foo.self, "FooSub", 0) as! Foo.Type
    objc_registerClassPair(FooSub)
    let foosub = FooSub.init()
    await foosub.doit()
  }
}

