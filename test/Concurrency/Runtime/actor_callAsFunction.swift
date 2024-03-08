// RUN: %target-run-simple-swift( -Xfrontend -disable-availability-checking -parse-as-library) | %FileCheck %s

// REQUIRES: executable_test
// REQUIRES: concurrency

// REQUIRES: concurrency_runtime
// UNSUPPORTED: back_deployment_runtime

enum Maybe<T: Sendable>: Sendable {
  case just(T)
  case nothing
}

@available(SwiftStdlib 5.1, *)
actor Printer {
  var numPrints: Int = 0
  
  func callAsFunction<T>(_ x: Maybe<T>) async {
    switch x {
    case .nothing: genericPrinter("nothing", counter: &numPrints)
    case .just(let t): genericPrinter(t, counter: &numPrints)
    }
  }
  
  func callAsFunction(_ x: String) {
    genericPrinter(x, counter: &numPrints)
  }
  
  func callAsFunction(_ x: Int) async throws {
    genericPrinter(x, counter: &numPrints)
  }
  
  @MainActor func callAsFunction(writingTo count: inout Maybe<Int>) async {
    count = .just(await numPrints)
  }
  
  private func genericPrinter<T>(_ x: T, counter: inout Int) {
    print(x)
    counter += 1
  }
}

@available(SwiftStdlib 5.1, *)
@main struct Main {
  static func main() async {
    let p = Printer()
    var maybe: Maybe<Int> = .nothing
    
    await p(maybe)
    await p("cat")
    try! await p(2)
    await p("cat")
    
    
    await p(writingTo: &maybe)
    await p(maybe)
  }
}

// CHECK: nothing
// CHECK: cat
// CHECK: 2
// CHECK: cat
// CHECK: 4

