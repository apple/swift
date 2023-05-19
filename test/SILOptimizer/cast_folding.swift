// RUN: %target-swift-frontend -O -emit-sil %s | %FileCheck %s

// We want to check two things here:
// - Correctness
// - That certain "is" checks are eliminated based on static analysis at compile-time
//
// In ideal world, all those testNN functions should be simplified down to a single basic block
// which returns either true or false, i.e. all type checks should folded statically.

public protocol P {}
public protocol R {}

protocol Q: P {}

class A: P {}
class AA: A {}

class X {}

class B: P {}

private struct S: P {}

struct T: Q {}

private struct U {}

public protocol CP1: class {}

public protocol CP2: class {}

// Class D implements only one of class protocols
// and it cannot be extended elsewhere as it is private
private class D: CP1 {}

private final class F: CP1 {}

// Class E implements both class protocols at once
class E: CP1, CP2 {}

func cast0<T>(_ a: T) -> Bool {
  // Succeeds if T is A
  return type(of: A()) is T.Type
}


func cast1<T>(_ a: T) -> Bool {
  // Succeeds if T is A
  return type(of: (A() as AnyObject)) is T.Type
}

func cast2<T>(_ a: T) -> Bool {
  // Succeeds if T is A
  let ao: AnyObject = A() as AnyObject
  return type(of: ao) is T.Type
}


func cast3(_ p: AnyObject) -> Bool {
  // Always fails
  return type(of: p) is AnyObject.Protocol
}

func cast4(_ p: AnyObject) -> Bool {
  return type(of: p) is A.Type
}

func cast5(_ t: AnyObject.Type) -> Bool {
  // Succeeds if t is B.self
  return t is B.Type
}

func cast6<T>(_ t: T) -> Bool {
  // Always fails
  return AnyObject.self is T.Type
}

func cast7<T>(_ t: T.Type) -> Bool {
  // Succeeds if t is AnyObject.self
  return t is AnyObject.Protocol
}


func cast8<T>(_ a: T) -> Bool {
  // Succeeds if T is A
  return type(of: (A() as P)) is T.Type
}

func cast9<T>(_ a: T) -> Bool {
  // Succeeds if T is A
  let ao: P = A() as P
  return type(of: ao) is T.Type
}


func cast10(_ p: P) -> Bool {
  return type(of: p) is P.Protocol
}

func cast11(_ p: P) -> Bool {
  // Succeeds if p is of type A
  return type(of: p) is A.Type
}

func cast12(_ t: P.Type) -> Bool {
  return t is B.Type
}


func cast13<T>(_ t: T) -> Bool {
  // Succeeds if T is P
  return P.self is T.Type
}


func cast14<T>(_ t: T.Type) -> Bool {
  // Succeeds if p is P.self
  return t is P.Protocol
}

func cast15<T>(_ t: T) -> Bool {
  // Succeeds if T is P
  return P.self is T.Type
}

func cast16<T>(_ t: T) -> Bool {
  // Succeeds if T is P
  return T.self is P.Protocol
}


func cast17<T>(_ t: T) -> Bool {
  // Succeeds if T is AnyObject
  return AnyObject.self is T.Type
}

func cast18<T>(_ t: T) -> Bool {
  // Succeeds if T is AnyObject
  return T.self is AnyObject.Protocol
}

func cast20<T>(_ t: T) -> Bool {
  // Succeeds if T is P
  return T.self is P.Type
}

func cast21<T>(_ t: T) -> Bool {
  // Succeeds if T is P
  return T.self is P.Protocol
}

func cast22(_ existential: P.Type) -> Bool {
  // Succeeds if existential is S.self
  return existential is S.Type
}

func cast23(_ concrete: P.Protocol) -> Bool {
  // Always fails
  return concrete is S.Type
}


func cast24(_ existential: P.Type) -> Bool {
  // Succeeds if existential is Q.self
  return existential is Q.Type
}


func cast25(_ concrete: P.Protocol) -> Bool {
  // Always fails, because P != Q
  return concrete is Q.Type
}

func cast26(_ existential: Q.Type) -> Bool {
  // Succeeds always, because Q: P
  return existential is P.Type
}

func cast27(_ existential: CP1.Type) -> Bool {
  // Fails always, because existential is a class
  // and it cannot be struct
  return existential is S.Type
}

func cast28(_ existential: CP1.Type) -> Bool {
  // Succeeds if existential conforms to CP1 and CP2
  return existential is CP2.Type
}

func cast29(_ o: Any) -> Bool {
  // Succeeds if o is P.Type
  return o is P.Type
}

func cast30(_ o: AnyObject) -> Bool {
  // Succeeds if o is P.Type
  return o is P.Type
}

func cast32(_ p: P) -> Bool {
  // Fails always, because a metatype cannot conform to a protocol
  return p is P.Type
}

func cast33(_ p: P) -> Bool {
  // Same as above, but non-existential metatype
  return p is X.Type
}

func cast38<T>(_ t: T) -> Bool {
  return t is (Int, Int)
}

func cast39<T>(_ t: T) -> Bool {
  return t is (x: Int, y: Int)
}

func cast40<T>(_ t: T) -> Bool {
  return t is (x: Int, y: A)
}

func cast41<T>(_ t: T, _ mt: T.Type) -> Bool {
	let typ = type(of: t as Any)
	return typ is AnyClass
}

func cast42(_ p: P) -> Bool {
  return type(of: p as Any) is AnyClass
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test0SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test0() -> Bool {
  return cast0(A())
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test1() -> Bool {
  return cast1(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test2() -> Bool {
  return cast2(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test3SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test3() -> Bool {
  return cast3(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test4SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test4() -> Bool {
  return cast4(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test5_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test5_1() -> Bool {
    return cast5(B.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test5_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test5_2() -> Bool {
    return cast5(AnyObject.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test6_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test6_1() -> Bool {
    return cast6(B.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test6_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test6_2() -> Bool {
    return cast6(AnyObject.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test7_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test7_1() -> Bool {
    return cast7(B.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding7test7_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test7_2() -> Bool {
    return cast7(AnyObject.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test8SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test8() -> Bool {
  return cast8(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding5test9SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test9() -> Bool {
  return cast9(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test10SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test10() -> Bool {
  return cast10(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test11SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test11() -> Bool {
  return cast11(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test12SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test12() -> Bool {
    return cast12(A.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test13_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test13_1() -> Bool {
    return cast13(A.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test13_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test13_2() -> Bool {
    return cast13(P.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test13_3SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test13_3() -> Bool {
    return cast13(A() as P)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test14_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test14_1() -> Bool {
    return cast14(A.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test14_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test14_2() -> Bool {
    return cast14(P.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test15_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test15_1() -> Bool {
    return cast15(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test15_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test15_2() -> Bool {
    return cast15(A() as P)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test16_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test16_1() -> Bool {
    return cast16(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test16_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test16_2() -> Bool {
    return cast16(A() as P)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test17_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test17_1() -> Bool {
    return cast17(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test17_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test17_2() -> Bool {
    return cast17(A() as AnyObject)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test18_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test18_1() -> Bool {
    return cast18(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test18_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test18_2() -> Bool {
    return cast18(A() as AnyObject)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test19SbyF : $@convention(thin) () -> Bool
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test19SbyF'
@inline(never)
func test19() -> Bool {
    let t: Any.Type = type(of: 1 as Any)
    return t is Int.Type
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test20_1SbyF : $@convention(thin) () -> Bool
@inline(never)
func test20_1() -> Bool {
    return cast20(S.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test20_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test20_2() -> Bool {
    return cast20(U())
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test21_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test21_1() -> Bool {
    return cast21(S.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test21_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test21_2() -> Bool {
    return cast21(A() as P)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test22_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test22_1() -> Bool {
    return cast22(T.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test22_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test22_2() -> Bool {
    return cast22(S.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test23SbyF : $@convention(thin) () -> Bool
// CHECK:         [[I0:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK:         [[B:%.*]] = struct $Bool ([[I0]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test23SbyF'
@inline(never)
func test23() -> Bool {
    return cast23(P.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test24_1SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test24_1() -> Bool {
    return cast24(T.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test24_2SbyF : $@convention(thin) () -> Bool
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test24_2() -> Bool {
    return cast24(S.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test25SbyF : $@convention(thin) () -> Bool
// CHECK:         [[I0:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK:         [[B:%.*]] = struct $Bool ([[I0]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test25SbyF'
@inline(never)
func test25() -> Bool {
    return cast25(P.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test26SbyF
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test26() -> Bool {
    return cast26(T.self)
}


// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test27SbyF
// CHECK:         [[I0:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK:         [[B:%.*]] = struct $Bool ([[I0]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test27SbyF'
@inline(never)
func test27() -> Bool {
    return cast27(D.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test28_1SbyF
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test28_1() -> Bool {
    return cast28(D.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test28_2SbyF
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test28_2() -> Bool {
    return cast28(E.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding8test28_3SbyF
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
func test28_3() -> Bool {
    return cast28(F.self)
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test29SbyF
// CHECK: bb0
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test29() -> Bool {
    return cast29(X())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test30SbyF
// CHECK: bb0
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test30() -> Bool {
    return cast30(X())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test32SbyF
// CHECK: bb0
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test32() -> Bool {
    // We don't actually fold this right now, but at least make sure it
    // doesn't crash
    return cast32(A())
}

// CHECK-LABEL: sil hidden [noinline] @$s12cast_folding6test33SbyF
// CHECK: bb0
// CHECK: checked_cast
// CHECK: return
@inline(never)
func test33() -> Bool {
    // Ditto...
    return cast33(A())
}


protocol PP {
  func foo() -> Int
}

public class CC : PP {
  func foo() -> Int { return 0 }
}

public class DD : PP {
  func foo() -> Int { return 1 }
}

// Check that the body of the function
// contains a trap followed by unreachable
// and no code afterwards.
// CHECK-LABEL: sil {{.*}}@$s12cast_folding7getAsDDyAA0E0CAA2CCCF
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }
public func getAsDD(_ c: CC) -> DD {
  return c as! DD
}

// Check that the body of the function
// contains a trap followed by unreachable
// and no code afterwards.
// CHECK-LABEL: sil {{.*}}@$s12cast_folding7callFooySiAA2CCCF
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }
public func callFoo(_ c: CC) -> Int {
  return (c as! DD).foo()
}


@inline(never)
func callFooGeneric<T : PP>(_ c: T) -> Int {
  return (c as! DD).foo()
}

// Check that the inlined version of callFooGeneric contains only a trap
// followed by unreachable and no code afterwards
// CHECK-LABEL: sil [noinline] {{.*}}@$s12cast_folding16callForGenericCCyyAA0F0CF
// CHECK: [[ONE:%[0-9]+]] = integer_literal $Builtin.Int1, -1
// CHECK: cond_fail [[ONE]] : $Builtin.Int1, "failed cast"
// CHECK-NEXT: unreachable
// CHECK-NEXT: }
@inline(never)
public func callForGenericCC(_ c: CC) {
  callFoo(c)
}

// Check that this conversion does not crash a compiler.
@inline(never)
public func test34() {
  if let _ = A.self as? P {
    print("A: dice")
  } else {
    print("A: no dice")
  }
}


// Check that this conversion does not crash a compiler.
@inline(never)
public func test35() {
  if let _ = X.self as? P {
    print("X: dice")
  } else {
    print("X: no dice")
  }
}

// Check that we do not eliminate casts from AnyHashable to a type that
// implements Hashable.
// CHECK-LABEL: sil [noinline] @$s12cast_folding6test36{{[_0-9a-zA-Z]*}}F
// CHECK: checked_cast_addr_br take_always AnyHashable in {{.*}} to Int
@inline(never)
public func test36(ah: AnyHashable) {
  if let _ = ah as? Int {
    print("success")
  } else {
    print("failure")
  }
}

// Check that we do not eliminate casts to AnyHashable from an opaque type
// that might implement Hashable.
// CHECK-LABEL: sil [noinline] @$s12cast_folding6test37{{[_0-9a-zA-Z]*}}F
// CHECK: checked_cast_addr_br take_always T in {{.*}} to AnyHashable
@inline(never)
public func test37<T>(ah: T) {
  if let _ = ah as? AnyHashable {
    print("success")
  } else {
    print("failure")
  }
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test38a{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test38a() -> Bool {
  return cast38((1, 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test38b{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test38b() -> Bool {
  return cast38((x: 1, y: 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test38c{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test38c() -> Bool {
  return cast38((z: 1, y: 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test39a{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test39a() -> Bool {
  return cast39((1, 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test39b{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, -1
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test39b() -> Bool {
  return cast39((x: 1, y: 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test39c{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test39c() -> Bool {
  return cast39((z: 1, y: 2))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test39d{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test39d() -> Bool {
  return cast39((1, 2, 3))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test40a{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// FIXME: Would love to fold this to just "true"
// CHECK-NOT: return:
// CHECK: unconditional_checked_cast_addr
@inline(never)
public func test40a() -> Bool {
  return cast40((1, A()))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test40b{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// FIXME: Would love to fold this to just "true"
// CHECK-NOT: return:
// CHECK: unconditional_checked_cast_addr
@inline(never)
public func test40b() -> Bool {
  return cast40((1, AA()))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test40c{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NEXT: %0 = integer_literal $Builtin.Int1, 0
// CHECK-NEXT: %1 = struct $Bool
// CHECK-NEXT: return %1
@inline(never)
public func test40c() -> Bool {
  return cast40((1, S()))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding7test40d{{[_0-9a-zA-Z]*}}F
// CHECK: bb0
// CHECK-NOT: return
// CHECK: checked_cast_addr_br take_always (Int, Any) in
@inline(never)
public func test40d(_ a: Any) -> Bool {
  return cast40((1, a))
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test41SbyF
// CHECK: bb0
// FIXME: Would love to fold this to just "true"
// CHECK-NOT: return:
// CHECK: checked_cast_br
// CHECK: //{{.*}}$s12cast_folding6test41{{.*}}F
@inline(never)
public func test41() -> Bool {
    return cast41(A(), P.self)
}

// CHECK-LABEL: sil [noinline] {{.*}}@$s12cast_folding6test42{{.*}}F
// CHECK: bb0
// CHECK-NOT: return:
// CHECK: checked_cast
// CHECK: //{{.*}}$s12cast_folding6test42{{.*}}F
@inline(never)
public func test42(_ p: P) -> Bool {
    return cast42(p)
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test43SbyF
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test43SbyF'
@inline(never)
public func test43() -> Bool {
  return P.self is Any.Type
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test44SbyF
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test44SbyF'
@inline(never)
public func test44() -> Bool {
  return Any.self is Any.Type
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test45SbyF
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test45SbyF'
@inline(never)
public func test45() -> Bool {
  return (P & R).self is Any.Type
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test46SbyF
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test46SbyF'
@inline(never)
public func test46() -> Bool {
  return AnyObject.self is Any.Type
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test47SbyF
// CHECK:         [[I1:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK:         [[B:%.*]] = struct $Bool ([[I1]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test47SbyF'
@inline(never)
public func test47() -> Bool {
  return Any.Type.self is Any.Type
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding6test48SbyF
// CHECK:         [[I0:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK:         [[B:%.*]] = struct $Bool ([[I0]]
// CHECK:         return [[B]]
// CHECK:       } // end sil function '$s12cast_folding6test48SbyF'
@inline(never)
public func test48() -> Bool {
  return Any.Type.self is Any.Type.Type
}

func cast<U, V>(_ u: U.Type) -> V? {
  return u as? V
}

// CHECK-LABEL: sil [noinline] @{{.*}}testCastAnyObjectProtocolTo{{.*}}Type
// CHECK: %0 = enum $Optional{{.*}}, #Optional.none!enumelt
// CHECK-NEXT: return %0
@inline(never)
public func testCastAnyObjectProtocolToAnyObjectType() -> AnyObject.Type? {
  return cast(AnyObject.self)
}

// CHECK-LABEL: sil [noinline] @$s12cast_folding020testCastProtocolTypee2ToefF0AA1P_pXpXpSgyF :
// CHECK:         [[E:%.*]] = enum $Optional{{.*}}, #Optional.none!enumelt
// CHECK:         return [[E]]
// CHECK:       } // end sil function '$s12cast_folding020testCastProtocolTypee2ToefF0AA1P_pXpXpSgyF'
@inline(never)
public func testCastProtocolTypeProtocolToProtocolTypeType() -> P.Type.Type? {
  return P.Type.self as? P.Type.Type
}

protocol PForOptional {
}

extension Optional: PForOptional {
}

func testCastToPForOptional<T>(_ t: T) -> Bool {
  if let _ = t as? PForOptional {
    return true
  }
  return false
}

// CHECK-LABEL: // testCastToPForOptionalSuccess()
// CHECK: [[RES:%.*]] = integer_literal $Builtin.Int1, -1
// CHECK: [[RET:%.*]] = struct $Bool ([[RES]] : $Builtin.Int1)
// CHECK: return [[RET]] : $Bool
@inline(never)
public func testCastToPForOptionalSuccess() -> Bool {
  let t: Int? = 42
  return testCastToPForOptional(t)
}

// CHECK-LABEL: // testCastToPForOptionalFailure()
// CHECK: [[RES:%.*]] = integer_literal $Builtin.Int1, 0
// CHECK: [[RET:%.*]] = struct $Bool ([[RES]] : $Builtin.Int1)
// CHECK: return [[RET]] : $Bool
@inline(never)
public func testCastToPForOptionalFailure() -> Bool {
  let t: Int = 42
  return testCastToPForOptional(t)
}

print("test0=\(test0())")

print("test1=\(test1())")

print("test2=\(test2())")

print("test3=\(test3())")

print("test4=\(test4())")

print("test5_1=\(test5_1())")

print("test5_2=\(test5_2())")

print("test6_1=\(test6_1())")

print("test6_2=\(test6_2())")

print("test7_1=\(test7_1())")

print("test7_2=\(test7_2())")


print("test8=\(test8())")

print("test9=\(test9())")

print("test10=\(test10())")

print("test11=\(test11())")

print("test12=\(test12())")

print("test13_1=\(test13_1())")

print("test13_2=\(test13_2())")

print("test13_3=\(test13_3())")

print("test14_1=\(test14_1())")

print("test14_2=\(test14_2())")

print("test15_1=\(test15_1())")

print("test15_2=\(test15_2())")

print("test16_1=\(test16_1())")

print("test16_2=\(test16_2())")


print("test17_1=\(test17_1())")

print("test17_2=\(test17_2())")

print("test18_1=\(test18_1())")

print("test18_2=\(test18_2())")

print("test19=\(test19())")

print("test20_1=\(test20_1())")

print("test20_2=\(test20_2())")

print("test21_1=\(test21_1())")

print("test21_2=\(test21_2())")

print("test22_1=\(test22_1())")

print("test22_2=\(test22_2())")

print("test23=\(test23())")

print("test24_1=\(test24_1())")

print("test24_2=\(test24_2())")

print("test25=\(test25())")

print("test26=\(test26())")

print("test27=\(test27())")

print("test28_1=\(test28_1())")

print("test28_2=\(test28_2())")

print("test28_3=\(test28_3))")

print("test29=\(test29())")

print("test30=\(test30())")

print("test32=\(test32())")

print("test33=\(test33())")
