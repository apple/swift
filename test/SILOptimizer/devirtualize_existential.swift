// RUN: %target-swift-frontend %s -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test -emit-sil | %FileCheck %s

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -wmo -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -module-name=test %s -o %t/a.out
// RUN: %target-codesign %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT
// REQUIRES: executable_test

// Test conversions of return types.

public struct S1<ID> {
  var x: Int
}

protocol P1<ID> {
  associatedtype ID

  func get(x: ID) -> S1<ID>
}

struct Y1: P1 {
  func get(x: Int) -> S1<Int> {
    return S1(x: 27)
  }
}

public struct X1<ID> {
  let p: any P1<ID>
  
  // CHECK-LABEL: sil {{.*}} @$s4test2X1V6testit1i1xAA2S1VyxGx_xtF :
  // CHECK:         unchecked_trivial_bit_cast
  // CHECK:       } // end sil function '$s4test2X1V6testit1i1xAA2S1VyxGx_xtF'
  @_semantics("optimize.sil.specialize.generic.never")
  @inline(never)
  public func testit(i: ID, x: ID) -> S1<ID> {
    return p.get(x: x)
  }
}

public struct S2<ID> {
  var x: String
}

protocol P2<ID> {
  associatedtype ID

  func get(x: ID) -> S2<ID>
}

struct Y2: P2 {
  func get(x: Int) -> S2<Int> {
    return S2(x: "27")
  }
}

public struct X2<ID> {
  let p: any P2<ID>
  
  // CHECK-LABEL: sil {{.*}} @$s4test2X2V6testit1i1xAA2S2VyxGx_xtF :
  // CHECK:         unchecked_bitwise_cast
  // CHECK:       } // end sil function '$s4test2X2V6testit1i1xAA2S2VyxGx_xtF'
  @_semantics("optimize.sil.specialize.generic.never")
  @inline(never)
  public func testit(i: ID, x: ID) -> S2<ID> {
    return p.get(x: x)
  }
}


class C3<T> {}

public struct S3<ID> {
  var x: C3<ID>
}

protocol P3<ID> {
  associatedtype ID

  func get(x: ID) -> S3<ID>
}

struct Y3: P3 {
  func get(x: Int) -> S3<Int> {
    return S3(x: C3<Int>())
  }
}

public struct X3<ID> {
  let p: any P3<ID>
  
  // CHECK-LABEL: sil {{.*}} @$s4test2X3V6testit1i1xAA2S3VyxGx_xtF :
  // CHECK:         unchecked_bitwise_cast
  // CHECK:       } // end sil function '$s4test2X3V6testit1i1xAA2S3VyxGx_xtF'
  @_semantics("optimize.sil.specialize.generic.never")
  @inline(never)
  public func testit(i: ID, x: ID) -> S3<ID> {
    return p.get(x: x)
  }
}


public class C4<T> {}

protocol P4<ID> {
  associatedtype ID

  func get(x: ID) -> C4<ID>
}

struct Y4: P4 {
  func get(x: Int) -> C4<Int> {
    return C4()
  }
}

public struct X4<ID> {
  let p: any P4<ID>
  
  // CHECK-LABEL: sil {{.*}} @$s4test2X4V6testit1i1xAA2C4CyxGx_xtF :
  // CHECK:         unchecked_ref_cast
  // CHECK:       } // end sil function '$s4test2X4V6testit1i1xAA2C4CyxGx_xtF'
  @_semantics("optimize.sil.specialize.generic.never")
  @inline(never)
  public func testit(i: ID, x: ID) -> C4<ID> {
    return p.get(x: x)
  }
}


public struct S5<ID> {
  var x: (Int, C4<ID>)
}

protocol P5<ID> {
  associatedtype ID

  func get(x: ID) -> S5<ID>
}

struct Y5: P5 {
  func get(x: Int) -> S5<Int> {
    return S5(x: (27, C4<Int>()))
  }
}

public struct X5<ID> {
  let p: any P5<ID>
  
  // CHECK-LABEL: sil {{.*}} @$s4test2X5V6testit1i1xAA2S5VyxGx_xtF :
  // CHECK:         unchecked_bitwise_cast
  // CHECK:       } // end sil function '$s4test2X5V6testit1i1xAA2S5VyxGx_xtF'
  @_semantics("optimize.sil.specialize.generic.never")
  @inline(never)
  public func testit(i: ID, x: ID) -> S5<ID> {
    return p.get(x: x)
  }
}

// Basic test

protocol Pingable {
 func ping(_ x : Int);
}
class Foo : Pingable {
  func ping(_ x : Int) { _ = 1 }
}

// Everything gets devirtualized, inlined, and promoted to the stack.
//CHECK-LABEL: sil @$s4test17interesting_stuffyyF :
//CHECK-NOT:     init_existential_addr
//CHECK-NOT:     apply
//CHECK:       } // end sil function '$s4test17interesting_stuffyyF'
public func interesting_stuff() {
 let x : Pingable = Foo()
 x.ping(1)
}

// CHECK-OUTPUT: S1<Int>(x: 27)
print(X1<Int>(p: Y1()).testit(i: 1, x: 2))
// CHECK-OUTPUT: S2<Int>(x: "27")
print(X2<Int>(p: Y2()).testit(i: 1, x: 2))
// CHECK-OUTPUT: S3<Int>(x: test.C3<Swift.Int>)
print(X3<Int>(p: Y3()).testit(i: 1, x: 2))
// CHECK-OUTPUT: test.C4<Swift.Int>
print(X4<Int>(p: Y4()).testit(i: 1, x: 2))
// CHECK-OUTPUT: S5<Int>(x: (27, test.C4<Swift.Int>))
print(X5<Int>(p: Y5()).testit(i: 1, x: 2))

