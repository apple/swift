// RUN: %target-build-swift -enable-conforming-protocol-extensions %s -o %t.out
// RUN: %target-run %t.out | %FileCheck %s

public extension FixedWidthInteger: ExpressibleByUnicodeScalarLiteral {
    @_transparent
  init(unicodeScalarLiteral value: Unicode.Scalar) {
    self = Self(value.value)
  }
  func foo() -> String {
    return "Foo!"
  }
}

let a: [Int16] = ["a", "b", "c", "d", "e"]
let b: [UInt32] = ["a", "b", "c", "d", "e"]

protocol Q {
  var bar: Double { get }
  func foo2() -> String
}
public protocol P {
}
public extension P: Q {
  var bar: Double { return -888 }
  func foo2() -> String {
    return "Foo2 \(self)!"
  }
}
class C {}
struct S {
  var a = 99
}
public protocol P2 : P {}
public extension P2 {}

 extension C: P2 {}
 extension S: P2 {}

//public extension Numeric: P2 {}
public extension FixedWidthInteger: P2 {}

// CHECK: Foo2 main.C!
print(C().foo2())
// CHECK: Foo2 S(a: 99)!
print(S().foo2())
// CHECK: Foo2 99!
print(Int8(99).foo2())
// CHECK: Foo2 99!
//extension UInt32: P2 {}
print(UInt32(99).foo2())
// CHECK: Foo!
print(Int8(99).foo())
// CHECK: Foo!
print(Int32(99).foo())
// CHECK: Foo!
print(UInt32(99).foo())

// CHECK: [97, 98, 99, 100, 101]
print(a)
// CHECK: [97, 98, 99, 100, 101]
print(b)
// CHECK: ["Foo2 97!", "Foo2 98!", "Foo2 99!", "Foo2 100!", "Foo2 101!"]
print(a.map {$0.foo2()})
// CHECK: ["Foo!", "Foo!", "Foo!", "Foo!", "Foo!"]
print(b.map {$0.foo()})
// CHECK: ["Foo2 97!", "Foo2 98!", "Foo2 99!", "Foo2 100!", "Foo2 101!"]
print(b.map {$0.foo2()})

public func use<T>(_ value: T) -> T
  where T : FixedWidthInteger {
    print(value.foo2())
    return value + "1" // ← Used ExpressibleByUnicodeScalarLiteral
}

// CHECK: 50
print(use(1))

let u = UInt32(99)

// CHECK: 148
print(use(u))

func aaa(_ b: P2) {
  print(b.foo2())
}

aaa(u)

//aaa(88.0)
//
//print(99.0.foo2())

let v = b.map { $0 as P2 }.map { $0.foo2() }
// CHECK: ["Foo2 97!", "Foo2 98!", "Foo2 99!", "Foo2 100!", "Foo2 101!"]
print(v)

// CHECK: -888.0
print(99.bar)

import Foundation

public protocol CC {
}

public struct AA: CC {
    var a = 9
    var b = 8
}

public extension CC: Codable {
}

// CHECK: {"a":9,"b":8}
print(String(data: try! JSONEncoder().encode(AA()), encoding: .utf8) ?? "?")
