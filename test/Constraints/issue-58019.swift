// RUN: %target-swift-frontend -dump-ast %s | %FileCheck %s

// https://github.com/apple/swift/issues/58019

func fetch() {
  // CHECK: open_existential_expr implicit type='Void'
  // CHECK: opaque_value_expr implicit type='any MyError'
  // CHECK-NOT: type='SryMap<$T{{.*}}>.Failure'
  sryMap { return "" }
  .napError{ $0.abc() }
}

func sryMap<String>(_ transform: () -> String) -> SryMap<String> {
  fatalError()
}

protocol MyError {}
extension MyError {
  func abc() -> Void { }
}

protocol MyProto {
  associatedtype Failure
}
extension MyProto {
  func napError(_ transform: (Self.Failure) -> Void) {}
}

struct SryMap<Output> : MyProto {
  typealias Failure = MyError
}
