// RUN: %target-run-simple-swift(-enable-experimental-feature RawLayout) | %FileCheck %s
// REQUIRES: executable_test

@_rawLayout(like: T)
struct Cell<T>: ~Copyable {}

struct Foo<T>: ~Copyable {
  let cell = Cell<T>()
  let myValue = 123
}

func something<T>(_ x: borrowing Foo<T>) -> Int {
  x.myValue
}

// CHECK: 123
print(something(Foo<Bool>()))

// CHECK: 123
print(something(Foo<Int>()))
