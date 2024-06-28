// RUN: %target-run-simple-swift(-enable-experimental-feature Embedded -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop) | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu

public struct FooError: Error {}

public class PrintingClass {
    init() { print("PrintingClass.init") }
    deinit { print("PrintingClass.deinit") }
}

public class Foo {
    var a: PrintingClass
    var b: PrintingClass
    init?(shouldFail: Bool) {
        a = PrintingClass()
        if shouldFail { return nil }
        b = PrintingClass()
    }
}

public class Bar: Foo {
  var value: Int = 17
}

public class Wibble: Bar {
  var c: PrintingClass = .init()
}

_ = Wibble(shouldFail: true)
print("OK 1")
// CHECK: PrintingClass.init
// CHECK: PrintingClass.init
// CHECK: PrintingClass.deinit
// CHECK: PrintingClass.deinit
// CHECK: OK 1

_ = Wibble(shouldFail: false)
print("OK 2")
// CHECK: PrintingClass.init
// CHECK: PrintingClass.init
// CHECK: PrintingClass.init
// CHECK: PrintingClass.deinit
// CHECK: PrintingClass.deinit
// CHECK: PrintingClass.deinit
// CHECK: OK 2

public class Base {
    var baseMember: PrintingClass
    init?(shouldFail: Bool) {
        if shouldFail { return nil }
        baseMember = PrintingClass()
    }
}

class SubClass: Base {
    var subClassMember: PrintingClass = PrintingClass()
    override init?(shouldFail: Bool) {
        super.init(shouldFail: shouldFail)
    }
}

_ = SubClass(shouldFail: true)
print("OK 3")
// CHECK: PrintingClass.init
// CHECK: PrintingClass.deinit
// CHECK: OK 3

_ = SubClass(shouldFail: false)
print("OK 4")
// CHECK: PrintingClass.init
// CHECK: PrintingClass.init
// CHECK: PrintingClass.deinit
// CHECK: PrintingClass.deinit
// CHECK: OK 4
