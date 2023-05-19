// RUN: %target-swift-frontend -dump-ast %s | %FileCheck %s

// https://github.com/apple/swift/issues/60806

struct Foo<T> {
  init(_: (T) -> Void) {}
}

protocol Bar {}

enum Baz: Bar {
  case someCase(Int)
}

enum NonBarBaz {
  case someCase(Int)
}

let _: Foo<Bar> = Foo<Bar> { (a: Bar) -> Void in
  switch a {
  // CHECK: (pattern_is type='any Bar' value_cast Baz
  // CHECK-NEXT: (pattern_enum_element type='Baz' Baz.someCase
  case let .someCase(value) as Baz:
    print(value)
  // expected-warning@-1 {{cast from 'any Bar' to unrelated type 'NonBarBaz' always fails}}
  case let .someCase(value) as NonBarBaz:
    print(value)
  default:
      break
  }
}
