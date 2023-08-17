// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking %S/Inputs/specialize_opaque_type_archetypes_2.swift -module-name External -emit-module -emit-module-path %t/External.swiftmodule
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking %S/Inputs/specialize_opaque_type_archetypes_3.swift -enable-library-evolution -module-name External2 -emit-module -emit-module-path %t/External2.swiftmodule
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking %S/Inputs/specialize_opaque_type_archetypes_4.swift -I %t -enable-library-evolution -module-name External3 -emit-module -emit-module-path %t/External3.swiftmodule
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking %S/Inputs/specialize_opaque_type_archetypes_3.swift -I %t -enable-library-evolution -module-name External2 -Osize -Xllvm -sil-disable-pass=redundant-load-elimination -emit-module -o - | %target-sil-opt -module-name External2 | %FileCheck --check-prefix=RESILIENT %s
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking -I %t -module-name A -enforce-exclusivity=checked -Osize -Xllvm -sil-disable-pass=redundant-load-elimination -emit-sil -sil-verify-all %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize
// RUN: %target-swift-frontend -enable-copy-propagation=requested-passes-only -enable-lexical-lifetimes=false -disable-availability-checking -I %t -module-name A -enforce-exclusivity=checked -enable-library-evolution -Osize -Xllvm -sil-disable-pass=redundant-load-elimination -emit-sil -sil-verify-all %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize

// REQUIRES: swift_in_compiler

import External
import External2
import External3

public protocol P {
  func myValue() -> Int64
}

extension Int64: P {
  public func myValue() -> Int64 {
    return self
  }
}

@inline(never)
func useP<T: P> (_ t: T) {
  print(t)
}

public func bar(_ x: Int64) -> some P {
  return x
}

public func foo(_ x: Int64) -> some P {
  if x > 0 {
    return bar(x + 1)
  }

  return bar(x - 1)
}

@inline(never)
func getInt() -> Int64 {
  return 2
}

@inline(never)
func identity<T>(_ t: T) -> T {
  return t
}

// CHECK-LABEL: sil {{.*}}@$s1A10testFooBaryyxAA1PRzlF : $@convention(thin) <T where T : P> (@in_guaranteed T) -> () {
// CHECK: bb3([[FOOS_INT:%.*]] : $Builtin.Int64):
// CHECK:  [[FOO_RES:%.*]] = struct $Int64 ([[FOOS_INT]] : $Builtin.Int64)
// CHECK:  [[ID:%.*]] = function_ref @$s1A8identityyxxlFs5Int64V_Tg5 : $@convention(thin) (Int64) -> Int64
// CHECK:  [[ID_RES:%.*]] = apply [[ID]]([[FOO_RES]]) : $@convention(thin) (Int64) -> Int64
// CHECK:  [[USEP:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5 : $@convention(thin) (Int64) -> ()
// CHECK:  apply [[USEP]]([[ID_RES]]) : $@convention(thin) (Int64) -> ()
// CHECK:  apply [[USEP]]([[FOO_RES]]) : $@convention(thin) (Int64) -> ()

public func testFooBar<T:P>(_ t : T) {
  let x = foo(getInt())
  useP(identity(x))
  useP(x.myValue())
}

struct AddressOnly : P{
  var p : P = Int64(1)

  func myValue() -> Int64 {
    return p.myValue()
  }
}

public func addressOnlyFoo() -> some P {
  return AddressOnly()
}

// CHECK-LABEL: sil @$s1A21testAddressOnlyFoobaryyF
// CHECK-NOT: return type of
// CHECK: return
public func testAddressOnlyFoobar() {
  let x = addressOnlyFoo()
  let y = x
  useP(x.myValue())
  useP(y.myValue())
}

public protocol CP : class {
  func myValue() -> Int64
}

class C : CP {
  func myValue() -> Int64 {
    return 0
  }
}

public func returnC() -> some CP {
  return C()
}

// CHECK-LABEL: sil @$s1A4useCyyF
// CHECK: [[FUN:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5
// CHECK: [[INT:%.*]] = struct $Int64 (
// CHECK:  = apply [[FUN]]([[INT]])
public func useC() {
   let c = returnC()
   useP(c.myValue())
}

// CHECK-LABEL: sil @$s1A11useExternalyyF
// CHECK:  // function_ref Int64.myValue2()
// CHECK:  [[FUN:%.*]] = function_ref @$ss5Int64V8ExternalE8myValue2AByF
// CHECK:  apply [[FUN]]
public func useExternal() {
  let e = external()
  useP(e.myValue2())
}

// Call to a resilient function should not be specialized.

// CHECK-LABEL: sil @$s1A20useExternalResilientyyF
// CHECK:  [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0)
// CHECK:  [[FUN:%.*]] = function_ref @$s9External217externalResilientQryF : $@convention(thin) @substituted {{.*}} for <@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0) __>
// CHECK:  apply [[FUN]]([[RES]])
// CHECK:  witness_method
// CHECK:  return
public func useExternalResilient() {
  let e = externalResilient()
  useP(e.myValue3())
}

struct Container {
  var x : some P {
    get {
      return Int64(1)
    }
  }
}

// CHECK-LABEL: sil @$s1A11usePropertyyyF
// CHECK:  [[VAL:%.*]] = struct $Int64
// CHECK:  // function_ref specialized useP<A>(_:)
// CHECK:  [[FUN:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5
// CHECK:  apply [[FUN]]([[VAL]])
public func useProperty() {
   let p = Container().x
   useP(p.myValue())
}

protocol Q {
  associatedtype T
  func f() -> T
  associatedtype T2
  func g() -> T2
}

struct S : Q {
  func f()->some P { return Int64(1) }
  func g()->some CP { return C() }
}

struct Container2 {
  var member : S.T
  mutating func blah(_ x: S.T) { member = x }
}

class Container3 {
  @inline(__always)
  init(member : S.T) {
    self.member = member
  }
  var member : S.T
  func blah(_ x: S.T) { member = x }
}

struct Container4 {
  var member : S.T2
  mutating func blah(_ x: S.T2) { member = x }
}

struct Container5 {
  var member : (S.T2, S.T)
  mutating func blah(_ x: S.T2, _ y: S.T) { member = (x,y) }
}

struct Pair<T, V> {
  var first: T
  var second: V
}
// CHECK-LABEL: sil @$s1A10storedPropyyF
// CHECK-NOT: apply
// CHECK: store
// CHECK-NOT: apply
// CHECK: store
// CHECK-NOT: apply
// CHECK: store
// CHECK-NOT: apply
// CHECK: return
public func storedProp() {
  var c = Container2(member: S().f())
  c.blah(S().f())
  var c2 = Container3(member: S().f())
  c2.blah(S().f())
  var c3 = Container4(member: S().g())
  c3.blah(S().g())
  var s = S()
  var c4 = Container5(member: (s.g(), s.f()))
  c4.blah(s.g(), s.f())
}

public func usePair() {
  var x = Pair(first: bar(1), second: returnC())
  useP(x.first.myValue())
  useP(x.second.myValue())
}


struct MyInt64 : ExternalP2 {
  var x = Int64(0)
  public func myValue3() -> Int64 {
    return  x + 3
  }
}

func nonResilient() -> some ExternalP2 {
  return MyInt64()
}

// CHECK-LABEL: sil @$s1A019usePairResilientNonC0yyF : $@convention(thin) () -> ()
// CHECK: alloc_stack $Pair<MyInt64, @_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0)
// CHECK: [[USEP:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5
// CHECK: [[FIRST_MYVALUE3:%.*]] = struct $Int64
// CHECK: apply [[USEP]]([[FIRST_MYVALUE3]])
// CHECK:  [[MYVALUE_WITNESS:%.*]] = witness_method $@_opaqueReturnTypeOf("$s9External217externalResilientQryF"
// CHECK:  [[SECOND_MYVALUE3:%.*]] = apply [[MYVALUE_WITNESS]]
// CHECK:  apply [[USEP]]([[SECOND_MYVALUE3]])
// CHECK: return

public func usePairResilientNonResilient() {
  var x = Pair(first: nonResilient(), second: externalResilient())
  useP(x.first.myValue3())
  useP(x.second.myValue3())
}

public protocol P3 {
  associatedtype AT
  func foo() -> AT
}

public struct Adapter<T: P3>: P3 {
  var inner: T
  public func foo() -> some P3 {
    return inner
  }
}

// Don't assert.
// CHECK-LABEL: sil {{.*}} @$s1A7AdapterVyxGAA2P3A2aEP3foo2ATQzyFTW
// CHECK:  [[F:%.*]] = function_ref @$s1A7AdapterV3fooQryF
// CHECK:  apply [[F]]<τ_0_0>(%0, %1) : $@convention(method) <τ_0_0 where τ_0_0 : P3> (@in_guaranteed Adapter<τ_0_0>) ->
extension P3 {
  public func foo() -> some P3 {
    return Adapter(inner: self)
  }
}

// We should specialize the opaque type because the resilient function is
// inlineable.

// CHECK-LABEL: sil @$s1A21useExternalResilient2yyF : $@convention(thin) () -> ()
// CHECK:   [[RES:%.*]] = alloc_stack $Int64
// CHECK:   [[FUN:%.*]] = function_ref @$s9External226inlinableExternalResilientQryF : $@convention(thin) @substituted {{.*}} for <Int64>
// CHECK:   apply [[FUN]]([[RES]])
// CHECK:   return
public func useExternalResilient2() {
  let e = inlinableExternalResilient()
  useP(e.myValue3())
}

// In this case we should only 'peel' one layer of opaque archetypes.
// CHECK-LABEL: sil @$s1A21useExternalResilient3yyF
// CHECK:  [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0)
// CHECK:  [[FUN:%.*]] = function_ref @$s9External3031inlinableExternalResilientCallsD0QryF : $@convention(thin) @substituted {{.*}} for <@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0) __>
// CHECK:  apply [[FUN]]([[RES]])
public func useExternalResilient3() {
  let e = inlinableExternalResilientCallsResilient()
  useP(e.myValue3())
}

// Check that we can look through two layers of inlinable resilient functions.
// CHECK-LABEL: sil @$s1A21useExternalResilient4yyF
// CHECK:   [[RES:%.*]] = alloc_stack $Int64
// CHECK:   [[FUN:%.*]] = function_ref @$s9External3040inlinableExternalResilientCallsInlinablecD0QryF : $@convention(thin) @substituted {{.*}} for <Int64>
// CHECK:   apply [[FUN]]([[RES]])
public func useExternalResilient4() {
  let e = inlinableExternalResilientCallsInlinableExternalResilient()
  useP(e.myValue3())
}

// CHECK-LABEL: sil @$s1A18testStoredPropertyyyF
// CHECK:   [[CONTAINER_INIT_FUN:%.*]] = function_ref @$s8External0A9ContainerVACycfC
// CHECK:   [[CONTAINER:%.*]] = apply [[CONTAINER_INIT_FUN]]
// CHECK:   [[RES:%.*]] = alloc_stack $Int64
// CHECK:   [[COMPUTED_PROP:%.*]] = function_ref @$s8External0A9ContainerV16computedPropertyQrvg
// CHECK:   apply [[COMPUTED_PROP]]([[RES]], [[CONTAINER]])
// CHECK:   [[MYVALUE:%.*]] = function_ref @$ss5Int64V8ExternalE8myValue2AByF : $@convention(method) (Int64) -> Int64
// CHECK:   apply [[MYVALUE]]
public func testStoredProperty() {
  let c = ExternalContainer()
  useP(c.computedProperty.myValue2())
}

// CHECK-LABEL: sil @$s1A21testResilientPropertyyyF
// CHECK:   [[CONTAINER:%.*]] = alloc_stack $ResilientContainer
// CHECK:   [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External218ResilientContainerV16computedPropertyQrvp", 0)
// CHECK:   [[FUN:%.*]] = function_ref @$s9External218ResilientContainerV16computedPropertyQrvg
// CHECK:   apply [[FUN]]([[RES]], [[CONTAINER]])
public func testResilientProperty() {
  let r = ResilientContainer()
  useP(r.computedProperty.myValue3())
}

// CHECK-LABEL: sil @$s1A30testResilientInlinablePropertyyyF
// CHECK:  [[CONTAINER:%.*]] = alloc_stack $ResilientContainer
// CHECK:  [[RES:%.*]] = alloc_stack $Int64
// CHECK:  [[FUN:%.*]] = function_ref @$s9External218ResilientContainerV18inlineablePropertyQrvg
// CHECK:  apply [[FUN]]([[RES]], [[CONTAINER]])
public func testResilientInlinableProperty() {
  let r = ResilientContainer()
  useP(r.inlineableProperty.myValue3())
}

// CHECK-LABEL: sil @$s1A31testResilientInlinableProperty3yyF
// CHECK:  [[CONTAINER:%.*]] = alloc_stack $ResilientContainer
// CHECK:  [[RES:%.*]] = alloc_stack $Int64
// CHECK:  [[FUN:%.*]] = function_ref @$s9External218ResilientContainerV19inlineableProperty2Qrvg
// CHECK:  apply [[FUN]]([[RES]], [[CONTAINER]])
public func testResilientInlinableProperty3() {
  let r = ResilientContainer()
  useP(r.inlineableProperty2.myValue3())
}

// CHECK-LABEL: sil @$s1A22testResilientProperty2yyF
// CHECK:  [[CONTAINER:%.*]] = alloc_stack $ResilientContainer2
// CHECK:  [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External319ResilientContainer2V16computedPropertyQrvp", 0)
// CHECK:  [[FUN:%.*]] = function_ref @$s9External319ResilientContainer2V16computedPropertyQrvg
// CHECK:  apply [[FUN]]([[RES]], [[CONTAINER]])
public func testResilientProperty2() {
  let r = ResilientContainer2()
  useP(r.computedProperty.myValue3())
}

// The inlinable property recursively calls an resilient property 'peel' one layer of opaque archetypes.
// CHECK-LABEL: sil @$s1A31testResilientInlinableProperty2yyF
// CHECK:  [[CONTAINER:%.*]] = alloc_stack $ResilientContainer2
// CHECK:  [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External218ResilientContainerV16computedPropertyQrvp", 0)
// CHECK:  [[FUN:%.*]] = function_ref @$s9External319ResilientContainer2V18inlineablePropertyQrvg
// CHECK:  apply [[FUN]]([[RES]], [[CONTAINER]])
public func testResilientInlinableProperty2() {
  let r = ResilientContainer2()
  useP(r.inlineableProperty.myValue3())
}

// CHECK-LABEL: sil @$s1A035testResilientInlinablePropertyCallsbC0yyF : $@convention(thin) () -> () {
// CHECK:   [[CONTAINTER:%.*]] = alloc_stack $ResilientContainer2
// CHECK:   [[RES:%.*]] = alloc_stack $Int64
// CHECK:   [[FUN:%.*]] = function_ref @$s9External319ResilientContainer2V023inlineablePropertyCallsB10InlineableQrvg
// CHECK:  apply [[FUN]]([[RES]], [[CONTAINTER]])
public func testResilientInlinablePropertyCallsResilientInlinable() {
  let r = ResilientContainer2()
  useP(r.inlineablePropertyCallsResilientInlineable.myValue3())
}

// RESILIENT-LABEL: sil {{.*}}@$s9External218ResilientContainerV33genericEagerMoveInlineableContextyyxlFSi_Tgq5 : {{.*}}{
// RESILIENT:       {{bb[0-9]+}}({{%[^,]+}} : $Int, {{%[^,]+}} : @_eagerMove $
// RESILIENT-LABEL: } // end sil function '$s9External218ResilientContainerV33genericEagerMoveInlineableContextyyxlFSi_Tgq5'

// RESILIENT-LABEL: sil [serialized] [canonical] @$s9External218ResilientContainerV33genericEagerMoveInlineableContextyyxlF : {{.*}} {
// RESILIENT:       {{bb[0-9]+}}({{%[^,]+}} : $*T, {{%[^,]+}} : @_eagerMove $
// RESILIENT-LABEL: } // end sil function '$s9External218ResilientContainerV33genericEagerMoveInlineableContextyyxlF'

// RESILIENT-LABEL: sil [serialized] [canonical] @$s9External218ResilientContainerV26eagerMoveInlineableContextyyF : $@convention(method) (@in_guaranteed ResilientContainer) -> () {
// RESILIENT:       {{bb[0-9]+}}({{%[^,]+}} : @_eagerMove $
// RESILIENT-LABEL: } // end sil function '$s9External218ResilientContainerV26eagerMoveInlineableContextyyF'

// RESILIENT-LABEL: sil [serialized] [canonical] @$s9External218ResilientContainerV17inlineableContextyyF
// RESILIENT:  [[RES:%.*]] = alloc_stack $@_opaqueReturnTypeOf("$s9External218ResilientContainerV16computedPropertyQrvp", 0)
// RESILIENT:  [[FUN:%.*]] = function_ref @$s9External218ResilientContainerV16computedPropertyQrvg
// RESILIENT:  apply [[FUN]]([[RES]], %0)

public protocol P4 {
  associatedtype AT
  func foo(_ x: Int64) -> AT
  func test()
}

struct PA : P4 {
  func foo(_ x: Int64)  -> some P {
    return Int64(x)
  }
}

public class K {}

public protocol P4EM {
  associatedtype AT
  func foo(@_eagerMove _ x: K) -> AT
}

struct PAEM : P4EM {
  func foo(@_eagerMove _ x: K)  -> some P {
    return 5 as Int64
  }
}

// CHECK-LABEL: sil private [transparent] [thunk] @$s1A2PAVAA2P4A2aDP4testyyFTW
// CHECK:   [[V:%.*]] = load %0 : $*PA
// CHECK:   [[F:%.*]] = function_ref @$s1A2PAV4testyyF
// CHECK:   apply [[F]]([[V]])

// CHECK-64-LABEL: sil hidden @$s1A2PAV4testyyF : $@convention(method) (PA) -> ()
// CHECK-64:   [[V:%.*]] = integer_literal $Builtin.Int64, 5
// CHECK-64:   [[I:%.*]] = struct $Int64 ([[V]] : $Builtin.Int64)
// CHECK-64:   [[F:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5
// CHECK-64:   apply [[F]]([[I]]) : $@convention(thin) (Int64) -> ()
// CHECK-64:   apply [[F]]([[I]]) : $@convention(thin) (Int64) -> ()

@inline(never)
func testIt<T>(cl: (Int64) throws -> T) {
 do {
   print(try cl(5))
 } catch (_) {}
}

// CHECK-LABEL: sil shared [noinline] @$s1A16testPartialApplyyyxAA2P4RzlFAA2PAV_Tg5
// CHECK:  [[PA:%.*]] = alloc_stack $PA
// CHECK:  store %0 to [[PA]] : $*PA
// CHECK:  [[F:%.*]] = function_ref @$s1A16testPartialApplyyyxAA2P4RzlF2ATQzs5Int64Vcxcfu_AeGcfu0_AA2PAV_TG5 : $@convention(thin) (Int64, @in_guaranteed PA) -> @out Int64
// CHECK:  [[C:%.*]] = partial_apply [callee_guaranteed] [[F]]([[PA]]) : $@convention(thin) (Int64, @in_guaranteed PA) -> @out Int64
// CHECK:  convert_function [[C]] : $@callee_guaranteed (Int64) -> @out Int64 to $@callee_guaranteed @substituted <τ_0_0> (Int64) -> (@out τ_0_0, @error any Error) for <Int64>
@inline(never)
func testPartialApply<T: P4>(_ t: T) {
  let fun = t.foo
  testIt(cl: fun)
  print(fun(5))
}

public func testPartialApply() {
  testPartialApply(PA())
}

// CHECK-LABEL: sil shared [noinline] @$s1A25testPartialApplyEagerMoveyyxAA4P4EMRzlFAA4PAEMV_Tg5 : {{.*}}{
// CHECK:       {{bb[0-9]+}}({{%[^,]+}} : @_eagerMove $
// CHECK-LABEL: } // end sil function '$s1A25testPartialApplyEagerMoveyyxAA4P4EMRzlFAA4PAEMV_Tg5'

@inline(never)
func testItEagerMove<T>(cl: (K) throws -> T) {
 do {
   print(try cl(K()))
 } catch (_) {}
}

@inline(never)
func testPartialApplyEagerMove<T: P4EM>(@_eagerMove _ t: T) {
  let fun = t.foo
  testItEagerMove(cl: fun)
  print(fun(K()))
}

public func testPartialApplyEagerMove() {
  testPartialApplyEagerMove(PAEM())
}

struct Trivial<T> {
  var x : Int64
}

func createTrivial<T>(_ t: T) -> Trivial<T> {
  return Trivial<T>(x: 1)
}

// CHECK: sil @$s1A11testTrivialyyF : $@convention(thin) () -> ()
// CHECK:   %[[LITERAL:.+]] = integer_literal $Builtin.Int64, 1
// CHECK:   %[[STRUCT:.+]] = struct $Int64 (%[[LITERAL]] : $Builtin.Int64)
// CHECK:   %[[FUNC:.+]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5 : $@convention(thin) (Int64) -> ()
// CHECK:   apply %[[FUNC]](%[[STRUCT]])
public func testTrivial() {
   let s = bar(10)
   let t = createTrivial(s)
   useP(t.x)
}

func createTuple<T>(_ t: T) -> (T,T) {
  return (t, t)
}

// CHECK: sil @$s1A9testTupleyyF
// CHECK:  [[I:%.*]] = integer_literal $Builtin.Int64, 10
// CHECK:  [[I2:%.*]] = struct $Int64 ([[I]] : $Builtin.Int64)
// CHECK:  [[F:%.*]] = function_ref @$s1A4usePyyxAA1PRzlFs5Int64V_Tg5 : $@convention(thin) (Int64) -> ()
// CHECK:  apply [[F]]([[I2]]) : $@convention(thin) (Int64) -> ()
// CHECK:  apply [[F]]([[I2]]) : $@convention(thin) (Int64) -> ()
public func testTuple() {
  let s = bar(10)
  let t = createTuple(s)
  useP(t.0)
  useP(t.1)
}

extension PA {
  func test() {
    var p = (foo, foo)
    useP(p.0(5))
    useP(p.1(5))
  }
}

public struct Foo {
  var id : Int = 0
  var p : Int64 = 1
}

struct Test : RandomAccessCollection {
    struct Index : Comparable, Hashable {
        var identifier: AnyHashable?
        var offset: Int

        static func < (lhs: Index, rhs: Index) -> Bool {
            return lhs.offset < rhs.offset
        }

        func hash(into hasher: inout Hasher) {
            hasher.combine(identifier)
            hasher.combine(offset)
        }
    }

    let foos: [Foo]
    let ids: [AnyHashable]

    init(foos: [Foo]) {
        self.foos = foos
        self.ids = foos.map { $0.id }
    }

    func _index(atOffset n: Int) -> Index {
        return Index(identifier: ids.isEmpty ? nil : ids[n], offset: n)
    }

    var startIndex: Index {
        return _index(atOffset: 0)
    }

    var endIndex: Index {
        return Index(identifier: nil, offset: ids.endIndex)
    }

    func index(after i: Index) -> Index {
        return _index(atOffset: i.offset + 1)
    }

    func index(before i: Index) -> Index {
        return _index(atOffset: i.offset - 1)
    }

    func distance(from start: Index, to end: Index) -> Int {
        return end.offset - start.offset
    }

    func index(_ i: Index, offsetBy n: Int) -> Index {
        return _index(atOffset: i.offset + n)
    }

   subscript(i: Index) -> some P {
        return foos[i.offset].p
    }
}

@inline(never)
func useAbstractFunction<T: P>(_ fn: (Int64) -> T) {}

public func testThinToThick() {
  useAbstractFunction(bar)
}

// CHECK-LABEL: sil @$s1A19rdar56410009_normalyyF
public func rdar56410009_normal() {
  // CHECK: [[EXTERNAL_RESILIENT_WRAPPER:%.+]] = function_ref @$s9External224externalResilientWrapperyQrxAA10ExternalP2RzlF
  // CHECK: = apply [[EXTERNAL_RESILIENT_WRAPPER]]<@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0) __>({{%.+}}, {{%.+}}) : $@convention(thin) <τ_0_0 where τ_0_0 : ExternalP2> (@in_guaranteed τ_0_0) -> @out @_opaqueReturnTypeOf("$s9External224externalResilientWrapperyQrxAA10ExternalP2RzlF", 0) __<τ_0_0>
  _ = externalResilientWrapper(externalResilient())
} // CHECK: end sil function '$s1A19rdar56410009_normalyyF'

// CHECK-LABEL: sil @$s1A25rdar56410009_inlinedInneryyF
public func rdar56410009_inlinedInner() {
  // CHECK: [[EXTERNAL_RESILIENT_WRAPPER:%.+]] = function_ref @$s9External224externalResilientWrapperyQrxAA10ExternalP2RzlF
  // CHECK: = apply [[EXTERNAL_RESILIENT_WRAPPER]]<Int64>({{%.+}}, {{%.+}}) : $@convention(thin) <τ_0_0 where τ_0_0 : ExternalP2> (@in_guaranteed τ_0_0) -> @out @_opaqueReturnTypeOf("$s9External224externalResilientWrapperyQrxAA10ExternalP2RzlF", 0) __<τ_0_0>
  _ = externalResilientWrapper(inlinableExternalResilient())
} // CHECK: end sil function '$s1A25rdar56410009_inlinedInneryyF'

// CHECK-LABEL: sil @$s1A25rdar56410009_inlinedOuteryyF
public func rdar56410009_inlinedOuter() {
  // CHECK: [[INLINABLE_EXTERNAL_RESILIENT_WRAPPER:%.+]] = function_ref @$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFAA08externalD0QryFQOyQo__Tg5
  // CHECK: = apply [[INLINABLE_EXTERNAL_RESILIENT_WRAPPER]]({{%.+}}, {{%.+}}) : $@convention(thin) (@in_guaranteed @_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0) __) -> @out WrapperP2<@_opaqueReturnTypeOf("$s9External217externalResilientQryF"
  _ = inlinableExternalResilientWrapper(externalResilient())
} // CHECK: end sil function '$s1A25rdar56410009_inlinedOuteryyF'

// Specialized from above
// CHECK-LABEL: sil shared [noinline] @$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFAA08externalD0QryFQOyQo__Tg5
// CHECK: [[WRAPPER_INIT:%.+]] = function_ref @$s9External29WrapperP2VyACyxGxcfC
// CHECK: = apply [[WRAPPER_INIT]]<@_opaqueReturnTypeOf("$s9External217externalResilientQryF", 0) __>({{%.+}}, {{%.+}}, {{%.+}}) : $@convention(method) <τ_0_0 where τ_0_0 : ExternalP2> (@in τ_0_0, @thin WrapperP2<τ_0_0>.Type) -> @out WrapperP2<τ_0_0>
// CHECK: end sil function '$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFAA08externalD0QryFQOyQo__Tg5'

// Specialized from below
// CHECK-LABEL: sil shared [noinline] @$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFs5Int64V_Tg5
// CHECK: [[WRAPPER_INIT:%.+]] = function_ref @$s9External29WrapperP2VyACyxGxcfC
// CHECK: = apply [[WRAPPER_INIT]]<Int64>({{%.+}}, {{%.+}}, {{%.+}}) : $@convention(method) <τ_0_0 where τ_0_0 : ExternalP2> (@in τ_0_0, @thin WrapperP2<τ_0_0>.Type) -> @out WrapperP2<τ_0_0>
// CHECK: end sil function '$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFs5Int64V_Tg5'

// CHECK-LABEL: sil @$s1A24rdar56410009_inlinedBothyyF
public func rdar56410009_inlinedBoth() {
  // CHECK: [[INLINABLE_EXTERNAL_RESILIENT_WRAPPER:%.+]] = function_ref @$s9External233inlinableExternalResilientWrapperyQrxAA0C2P2RzlFs5Int64V_Tg5
  // CHECK: = apply [[INLINABLE_EXTERNAL_RESILIENT_WRAPPER]]({{%.+}}, {{%.+}}) : $@convention(thin) (Int64) -> @out WrapperP2<Int64>
  _ = inlinableExternalResilientWrapper(inlinableExternalResilient())
} // CHECK:  end sil function '$s1A24rdar56410009_inlinedBothyyF'
