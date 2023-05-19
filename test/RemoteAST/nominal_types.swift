// RUN: %target-swift-remoteast-test -enable-anonymous-context-mangled-names %s | %FileCheck %s

// REQUIRES: swift-remoteast-test

@_silgen_name("printMetadataType")
func printType(_: Any.Type)

@_silgen_name("stopRemoteAST")
func stopRemoteAST()

printType(Int.self)
// CHECK: Int

struct A {}
printType(A.self)
// CHECK: A

struct B {
  struct Foo {}
}
printType(B.Foo.self)
// CHECK: B.Foo

extension B {
  struct Bar {}
}
printType(B.Bar.self)
// CHECK: B.Bar

class C {
}
printType(C.self)
// CHECK: C

class D : C {
}
printType(D.self)
// CHECK: D

class E {
  struct Foo {}
}
printType(E.Foo.self)
// CHECK: E.Foo

struct F {
  class Foo {}
}
printType(F.Foo.self)
// CHECK: F.Foo

enum G {
  case Gwhatever

  struct Foo {}
  struct Bar<T> {}
}
printType(G.self)
// CHECK: G
printType(G.Foo.self)
// CHECK: G.Foo
printType(G.Bar<A>.self)
// CHECK: G.Bar<A>

struct H<T, U> {
  struct Foo {}
  struct Bar<V, W> {}
}
printType(H<A,A>.self)
// CHECK: H<A, A>
printType(H<B.Foo, H<B, A>>.self)
// CHECK: H<B.Foo, H<B, A>>
printType(H<B, B>.Foo.self)
// CHECK: H<B, B>.Foo
printType(H<A, B>.Bar<B, A>.self)
// CHECK: H<A, B>.Bar<B, A>

class I<T> {}
printType(I<Int>.self)
// CHECK: I<Int>

protocol J {}

printType(J.self)
// CHECK: found type: J
printType(J.Protocol.self)
// FIXME: Should be (any J).Type
// CHECK: found type: J.Type
printType(J.Type.self)
// CHECK: found type: any J.Type

protocol K {}
typealias JK = J & K
typealias KJ = K & J
printType(JK.self)
// CHECK: found type: J & K
printType(KJ.self)
// CHECK: found type: J & K

struct L {
  private struct PrivateInner { }
  private struct PrivateInnerGeneric<T> { }

  static func testPrivate() {
    // CHECK: L.PrivateInner
    printType(L.PrivateInner.self)

    // CHECK: L.PrivateInnerGeneric<Int>
    printType(L.PrivateInnerGeneric<Int>.self)

    // CHECK: L.PrivateInnerGeneric<String>
    printType(L.PrivateInnerGeneric<String>.self)
  }
}
L.testPrivate()

struct M<T, U> {
  private struct Inner { }
  private struct InnerGeneric<V> { }

  static func testPrivate() {
    // CHECK: M<Int, String>.Inner
    printType(Inner.self)

    // CHECK: M<Int, String>.InnerGeneric<Double>
    printType(InnerGeneric<Double>.self)

    // CHECK: M<Int, String>.InnerGeneric<(String, Int)>
    printType(InnerGeneric<(U, T)>.self)
  }
}
M<Int, String>.testPrivate()

struct N {
  static func testPrivate() {
    struct LocalStruct {
      struct Inner { }
    }
    // CHECK: LocalStruct.Inner
    printType(LocalStruct.Inner.self)
  }
}
N.testPrivate()

stopRemoteAST()
