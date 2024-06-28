// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %s -parse-as-library -enable-experimental-feature Embedded -c -o %t/main.o
// RUN: %target-clang %t/main.o -o %t/a.out -dead_strip
// RUN: %target-run %t/a.out | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu

class MyClass {
  init() { print("MyClass.init") }
  deinit { print("MyClass.deinit") }
  func foo() { print("MyClass.foo") }
}

class MySubClass: MyClass {
  var x = 27

  override init() { print("MySubClass.init") }
  deinit { print("MySubClass.deinit") }
  override func foo() { print("MySubClass.foo") }

  func printX() {
    print(x)
  }
}

class MySubSubClass: MySubClass {
  override init() { print("MySubSubClass.init") }
  deinit { print("MySubSubClass.deinit") }
  override func foo() { print("MySubSubClass.foo") }
}

class OtherSubClass: MyClass {}

func testCasting(_ title: StaticString, _ c: MyClass) {
  print(title, terminator: "")
  if let s = c as? MySubClass {
    s.printX()
  } else {
    print("-")
  }
}

public class DynamicSelfClass {
  public static let ds = DynamicSelfClass()
  public static let i: Int = 42
  var x: Int

  public init() {
    self.x = Self.i
  }
}

@main
struct Main {
  static var o: (MyClass?, MyClass?, MyClass?) = (nil, nil, nil)

  static func main() {
    print("1") // CHECK: 1
    o.0 = MyClass()
    // CHECK: MyClass.init
    print("")

    print("2") // CHECK: 2
    o.1 = MySubClass()
    // CHECK: MySubClass.init
    // CHECK: MyClass.init
    print("")

    print("3") // CHECK: 3
    o.2 = MySubSubClass()
    // CHECK: MySubSubClass.init
    // CHECK: MySubClass.init
    // CHECK: MyClass.init
    print("")

    print("4") // CHECK: 4
    o.0!.foo()
    o.1!.foo()
    o.2!.foo()
    // CHECK: MyClass.foo
    // CHECK: MySubClass.foo
    // CHECK: MySubSubClass.foo
    print("")
    
    print("5") // CHECK: 5
    o.0 = nil
    // CHECK: MyClass.deinit
    o.1 = nil
    // CHECK: MySubClass.deinit
    // CHECK: MyClass.deinit
    o.2 = nil
    // CHECK: MySubSubClass.deinit
    // CHECK: MySubClass.deinit
    // CHECK: MyClass.deinit
    print("")

    // CHECK: base: -
    testCasting("base: ", MyClass())
    // CHECK: sub: 27
    testCasting("sub: ", MySubClass())
    // CHECK: subsub: 27
    testCasting("subsub: ", MySubSubClass())
    // CHECK: other: -
    testCasting("other: ", OtherSubClass())

    // CHECK: 42
    print(DynamicSelfClass.ds.x)
  }
}
