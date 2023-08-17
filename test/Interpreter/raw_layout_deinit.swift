// RUN: %empty-directory(%t)
// RUN: %target-build-swift -enable-experimental-feature RawLayout %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s
// REQUIRES: executable_test

@_rawLayout(size: 16, alignment: 16)
struct Foo: ~Copyable {
  init(_ value: Int) {
    print("Foo.init(\(value))")
  }

  deinit {
    print("Foo.deinit")
  }

  func bar() {
    print("Foo.bar()")
  }
}

func test() {
  let foo = Foo(42)
  foo.bar()
}

print("-- start")
test()
print("-- done")

// CHECK:      -- start
// CHECK-NEXT: Foo.init(42)
// CHECK-NEXT: Foo.bar()
// CHECK-NEXT: Foo.deinit
// CHECK-NEXT: -- done
