
// RUN: %target-swift-frontend -module-name devirt_protocol_method_invocations -enable-spec-devirt -O -Xllvm -sil-disable-pass=ExistentialSpecializer -emit-sil %s | %FileCheck %s

// REQUIRES: swift_in_compiler

protocol PPP {
    func f()
}

protocol QQQ : PPP {
}

protocol RRR : QQQ {
}

struct S : RRR {}

extension QQQ {
    @_optimize(none)
    func f() {}
}

// Test that all witness_method instructions are devirtualized.
// This test used to crash the compiler because it uses inherited conformances.
// CHECK-LABEL: sil @$s34devirt_protocol_method_invocations24testInheritedConformanceyyF : $@convention(thin) () -> ()
// CHECK-NOT: witness_method
// CHECK-NOT: class_method
// CHECK: apply
// CHECK: // end sil function '$s34devirt_protocol_method_invocations24testInheritedConformanceyyF'
public func testInheritedConformance() {
    (S() as QQQ).f()
}

// Test that a witness_method instruction using an indirectly-inherited conformance
// is devirtualized.
//
// This test used to crash the compiler because it uses inherited conformances.
// CHECK-LABEL: sil @$s34devirt_protocol_method_invocations34testIndirectlyInheritedConformanceyyF : $@convention(thin) () -> ()
// CHECK-NOT: witness_method
// CHECK: apply
// CHECK: // end sil function '$s34devirt_protocol_method_invocations34testIndirectlyInheritedConformanceyyF'
public func testIndirectlyInheritedConformance() {
  (S() as RRR).f()
}


public protocol Foo { 
  func foo(_ x:Int) -> Int
}

public extension Foo {
  func boo(_ x:Int) -> Int32 {
    return 2222 + Int32(x)
  }

  func getSelf() -> Self {
    return self
  }
}

var gg = 1111

open class C : Foo {
  @inline(never)
  open func foo(_ x:Int) -> Int {
    gg += 1
    return gg + x
  }
}

@_transparent
func callfoo(_ f: Foo) -> Int {
  return f.foo(2) + f.foo(2)
}

@_transparent
func callboo(_ f: Foo) -> Int32 {
  return f.boo(2) + f.boo(2)
}

@_transparent
func callGetSelf(_ f: Foo) -> Foo {
  return f.getSelf()
}

// Check that methods returning Self are not devirtualized and do not crash the compiler.
// CHECK-LABEL: sil [noinline] {{.*}}@$s34devirt_protocol_method_invocations05test_a1_b11_extension_C33_invocation_with_self_return_typeyAA3Foo_pAA1CCF
// CHECK: init_existential_addr
// CHECK: open_existential_addr
// CHECK: return
@inline(never)
public func test_devirt_protocol_extension_method_invocation_with_self_return_type(_ c: C) -> Foo {
  return callGetSelf(c)
}

// Check that calls to f.foo() get devirtualized and are not invoked
// via the expensive witness_method instruction.
// To achieve that the information about a concrete type C should
// be propagated from init_existential_addr into witness_method and 
// apply instructions.

// CHECK-LABEL: sil [noinline] @$s34devirt_protocol_method_invocations05test_a1_b1_C11_invocationySiAA1CCF
// CHECK-NOT: witness_method
// CHECK: checked_cast
// CHECK-NOT: checked_cast
// CHECK: bb1(
// CHECK-NOT: checked_cast
// CHECK: return
// CHECK: bb2(
// CHECK-NOT: checked_cast
// CHECK: function_ref
// CHECK: apply
// CHECK: apply
// CHECK: br bb1(
// CHECK: bb3
// CHECK-NOT: checked_cast
// CHECK: apply
// CHECK: apply
// CHECK: br bb1(

// Check that calls of a method boo() from the protocol extension
// get devirtualized and are not invoked via the expensive witness_method instruction
// or by passing an existential as a parameter.
// To achieve that the information about a concrete type C should
// be propagated from init_existential_addr into apply instructions.
// In fact, the call is expected to be inlined and then constant-folded
// into a single integer constant.

// CHECK-LABEL: sil [noinline] {{.*}}@$s34devirt_protocol_method_invocations05test_a1_b11_extension_C11_invocationys5Int32VAA1CCF
// CHECK-NOT: checked_cast
// CHECK-NOT: open_existential
// CHECK-NOT: witness_method
// CHECK-NOT: apply
// CHECK: integer_literal
// CHECK: return

// CHECK: sil @$s34devirt_protocol_method_invocations12test24114020SiyF
// CHECK:   [[T0:%.*]] = integer_literal $Builtin.Int{{.*}}, 1
// CHECK:   [[T1:%.*]] = struct $Int ([[T0]] : $Builtin.Int{{.*}})
// CHECK:   return [[T1]]

// CHECK: sil @$s34devirt_protocol_method_invocations14testExMetatypeSiyF
// CHECK:   [[T0:%.*]] = integer_literal
// CHECK:   [[T2:%.*]] = struct $Int ([[T0]] : {{.*}})
// CHECK:   return [[T2]] : $Int

@inline(never)
public func test_devirt_protocol_method_invocation(_ c: C) -> Int {
  return callfoo(c)
}

@inline(never)
public func test_devirt_protocol_extension_method_invocation(_ c: C) -> Int32 {
  return callboo(c)
}


// Make sure that we are not crashing with an assertion due to specialization
// of methods with the Self return type as an argument.
// rdar://20868966
protocol Proto {
  func f() -> Self
}

class CC : Proto {
  func f() -> Self { return self }
}

func callDynamicSelfExistential(_ p: Proto) {
  p.f()
}

public func testSelfReturnType() {
  callDynamicSelfExistential(CC())
}


// Make sure that we are not crashing with an assertion due to specialization
// of methods with the Self return type.
// rdar://20955745.
protocol CP : class { func f() -> Self }
func callDynamicSelfClassExistential(_ cp: CP) { cp.f() }
class PP : CP {
  func f() -> Self { return self }
}

callDynamicSelfClassExistential(PP())

// Make sure we handle indirect conformances.
// rdar://24114020
protocol Base {
  var x: Int { get }
}
protocol Derived : Base {
}
struct SimpleBase : Derived {
  var x: Int
}
public func test24114020() -> Int {
  let base: Derived = SimpleBase(x: 1)
  return base.x
}

protocol StaticP {
  static var size: Int { get }
}
struct HasStatic<T> : StaticP {
  static var size: Int { return MemoryLayout<T>.size }
}
public func testExMetatype() -> Int {
  let type: StaticP.Type = HasStatic<Int>.self
  return type.size
}

// rdar://32288618
public func testExMetatypeVar() -> Int {
  var type: StaticP.Type = HasStatic<Int>.self
  return type.size
}

// IRGen used to crash on the testPropagationOfConcreteTypeIntoExistential method.
// rdar://26286278

protocol MathP {
  var sum: Int32 { get nonmutating set }
  func done()
}

extension MathP {
  @inline(never)
  func plus() -> Self {
    sum += 1
    return self
  }

  @inline(never)
  func minus() {
    sum -= 1
    if sum == 0 {
      done()
    }
  }
}

protocol MathA : MathP {}

public final class V {
  var a: MathA

  init(a: MathA) {
    self.a = a
  }
}

// Check that all witness_method invocations are devirtualized.
// CHECK-LABEL: sil [noinline] {{.*}}@$s34devirt_protocol_method_invocations44testPropagationOfConcreteTypeIntoExistential1v1xyAA1VC_s5Int32VtF
// CHECK-NOT: witness_method
// CHECK-NOT: class_method
// CHECK: return
@inline(never)
public func testPropagationOfConcreteTypeIntoExistential(v: V, x: Int32) {
  let y = v.a.plus()
  defer {
    y.minus()
  }
}

// Check that we don't attempt to cast an opened type to a concrete
// type inferred via ProtocolConformanceAnalysis if the type requires
// reabstraction when erased by an existential.
protocol ReabstractedP {
  func f()
}
extension Optional : ReabstractedP {
  func f() {}
}

// CHECK-LABEL: sil hidden [noinline] {{.*}}@$s34devirt_protocol_method_invocations23testReabstractedWitnessyyAA0F1P_pF : $@convention(thin) (@in_guaranteed any ReabstractedP) -> () {
// CHECK: bb0(%0 : $*any ReabstractedP):
// CHECK: [[OPEN:%.*]] = open_existential_addr immutable_access %0 : $*any ReabstractedP to $*@opened([[ID:.*]], any ReabstractedP) Self
// CHECK: [[WM:%.*]] = witness_method $@opened([[ID]], any ReabstractedP) Self, #ReabstractedP.f : <Self where Self : ReabstractedP> (Self) -> () -> (), [[OPEN]] : $*@opened([[ID]], any ReabstractedP) Self : $@convention(witness_method: ReabstractedP) <τ_0_0 where τ_0_0 : ReabstractedP> (@in_guaranteed τ_0_0) -> ()
// CHECK: apply [[WM]]<@opened([[ID]], any ReabstractedP) Self>([[OPEN]]) : $@convention(witness_method: ReabstractedP) <τ_0_0 where τ_0_0 : ReabstractedP> (@in_guaranteed τ_0_0) -> ()
// CHECK-LABEL: } // end sil function '$s34devirt_protocol_method_invocations23testReabstractedWitnessyyAA0F1P_pF'
@inline(never)
func testReabstractedWitness(_ f: ReabstractedP) {
  f.f()
}

public func testReabstracted(f: Optional<()->()>) {
  testReabstractedWitness(f)
}


// Test that we don't devirtualize calls to protocol requirements with
// covariant `Self`-rooted type parameters nested inside a collection type;
// the devirtualizer doesn't know how to handle these yet.
protocol CovariantSelfInCollection {
  associatedtype Assoc

  func self1() -> Array<Self>
  func self2() -> Dictionary<String, Self>
  func self3(_: (Self...) -> Void)
  func self4(_: (Array<(Dictionary<String, String>, Self)>) -> Void)

  func assoc1() -> Array<Assoc>
  func assoc2() -> Dictionary<String, Assoc>
  func assoc3(_: (Assoc...) -> Void)
  func assoc4(_: (Array<(Dictionary<String, String>, Assoc)>) -> Void)
}
struct CovariantSelfInCollectionImpl: CovariantSelfInCollection {
  typealias Assoc = Bool

  func self1() -> Array<Self> { [self] }
  func self2() -> Dictionary<String, Self> { [#file : self] }
  func self3(_: (Self...) -> Void) {}
  func self4(_: (Array<(Dictionary<String, String>, Self)>) -> Void) {}

  func assoc1() -> Array<Assoc> { [true] }
  func assoc2() -> Dictionary<String, Assoc> { [#file : true] }
  func assoc3(_: (Assoc...) -> Void) {}
  func assoc4(_: (Array<(Dictionary<String, String>, Assoc)>) -> Void) {}
}
// CHECK-LABEL: sil @$s34devirt_protocol_method_invocations12testNoDevirtyyF
//
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.self1
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.self2
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.self3
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.self4
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.assoc1
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.assoc2
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.assoc3
// CHECK: witness_method $CovariantSelfInCollectionImpl, #CovariantSelfInCollection.assoc4
// CHECK: end sil function '$s34devirt_protocol_method_invocations12testNoDevirtyyF'
public func testNoDevirt() {
  let p: any CovariantSelfInCollection = CovariantSelfInCollectionImpl()

  _ = p.self1()
  _ = p.self2()
  p.self3 { _ in }
  p.self4 { _ in }

  _ = p.assoc1()
  _ = p.assoc2()
  p.assoc3 { _ in }
  p.assoc4 { _ in }
}
