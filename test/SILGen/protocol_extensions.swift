
// RUN: %target-swift-emit-silgen -module-name protocol_extensions -disable-objc-attr-requires-foundation-module -enable-objc-interop %s | %FileCheck %s

public protocol P1 {
  func reqP1a()
  subscript(i: Int) -> Int { get set }
}

struct Box {
  var number: Int
}

extension P1 {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions2P1PAAE6extP1a{{[_0-9a-zA-Z]*}}F : $@convention(method) <Self where Self : P1> (@in_guaranteed Self) -> () {
  // CHECK: bb0([[SELF:%[0-9]+]] : $*Self):
  func extP1a() {
    // CHECK: [[WITNESS:%[0-9]+]] = witness_method $Self, #P1.reqP1a : {{.*}} : $@convention(witness_method: P1) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> ()
    // CHECK-NEXT: apply [[WITNESS]]<Self>([[SELF]]) : $@convention(witness_method: P1) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> ()
    reqP1a()
    // CHECK: return
  }

  // CHECK-LABEL: sil [ossa] @$s19protocol_extensions2P1PAAE6extP1b{{[_0-9a-zA-Z]*}}F : $@convention(method) <Self where Self : P1> (@in_guaranteed Self) -> () {
  // CHECK: bb0([[SELF:%[0-9]+]] : $*Self):
  public func extP1b() {
    // CHECK: [[FN:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE6extP1a{{[_0-9a-zA-Z]*}}F : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> ()
    // CHECK-NEXT: apply [[FN]]<Self>([[SELF]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> ()
    extP1a()
    // CHECK: return
  }

  subscript(i: Int) -> Int {
    // modify can do static dispatch to peer accessors (tested later, in the emission of the concrete conformance)
    get {
      return 0
    }
    set {}
  }

  func callSubscript() -> Int {
    // But here we have to do a witness method call:

    // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions2P1PAAE13callSubscript{{[_0-9a-zA-Z]*}}F
    // CHECK: bb0(%0 : $*Self):
    // CHECK: witness_method $Self, #P1.subscript!getter
    // CHECK: return
    return self[0]
  }

  static var staticReadOnlyProperty: Int {
    return 0
  }

  static var staticReadWrite1: Int {
    get { return 0 }
    set { }
  }

  static var staticReadWrite2: Box {
    get { return Box(number: 0) }
    set { }
  }
}

// ----------------------------------------------------------------------------
// Using protocol extension members with concrete types
// ----------------------------------------------------------------------------
class C : P1 {
  func reqP1a() { }
}

//   (modify test from above)
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s19protocol_extensions1CCAA2P1A2aDPyS2iciMTW
// CHECK: bb0(%0 : $Int, %1 : $*τ_0_0):
// CHECK: function_ref @$s19protocol_extensions2P1PAAEyS2icig
// CHECK: return

class D : C { }

struct S : P1 {
  func reqP1a() { }
}

struct G<T> : P1 {
  func reqP1a() { }
}

struct MetaHolder {
  var d: D.Type = D.self
  var s: S.Type = S.self
}

struct GenericMetaHolder<T> {
  var g: G<T>.Type = G<T>.self
}

func inout_func(_ n: inout Int) {}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions5testD_2dd1dyAA10MetaHolderV_AA1DCmAHtF : $@convention(thin) (MetaHolder, @thick D.Type, @guaranteed D) -> ()
// CHECK: bb0([[M:%[0-9]+]] : $MetaHolder, [[DD:%[0-9]+]] : $@thick D.Type, [[D:%[0-9]+]] : @guaranteed $D):
func testD(_ m: MetaHolder, dd: D.Type, d: D) {
  // CHECK: [[D2:%[0-9]+]] = alloc_box ${ var D }
  // CHECK: [[D2L:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[D2]]
  // CHECK: [[RESULT:%.*]] = project_box [[D2L]]
  // CHECK: [[MATERIALIZED_BORROWED_D:%[0-9]+]] = alloc_stack $D
  // CHECK: [[SB:%.*]] = store_borrow [[D]] to [[MATERIALIZED_BORROWED_D]]
  // CHECK: [[FN:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE11returnsSelf{{[_0-9a-zA-Z]*}}F
  // CHECK: apply [[FN]]<D>([[RESULT]], [[SB]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> @out τ_0_0
  // CHECK: end_borrow
  // CHECK-NEXT: dealloc_stack [[MATERIALIZED_BORROWED_D]]
  var d2: D = d.returnsSelf()

  // CHECK: metatype $@thick D.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = D.staticReadOnlyProperty

  // CHECK: metatype $@thick D.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  D.staticReadWrite1 = 1

  // CHECK: metatype $@thick D.Type
  // CHECK: alloc_stack $Int
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  D.staticReadWrite1 += 1

  // CHECK: metatype $@thick D.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  D.staticReadWrite2 = Box(number: 2)

  // CHECK: metatype $@thick D.Type
  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  D.staticReadWrite2.number += 5

  // CHECK: metatype $@thick D.Type
  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&D.staticReadWrite2.number)

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = dd.staticReadOnlyProperty

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  dd.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  dd.staticReadWrite1 += 1

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  dd.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  dd.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&dd.staticReadWrite2.number)

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = m.d.staticReadOnlyProperty

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  m.d.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  m.d.staticReadWrite1 += 1

  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  m.d.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  m.d.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&m.d.staticReadWrite2.number)

  // CHECK: return
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions5testS_2ssyAA10MetaHolderV_AA1SVmtF
func testS(_ m: MetaHolder, ss: S.Type) {
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = S.staticReadOnlyProperty

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  S.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  S.staticReadWrite1 += 1

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  S.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  S.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&S.staticReadWrite2.number)

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = ss.staticReadOnlyProperty

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  ss.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  ss.staticReadWrite1 += 1

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  ss.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  ss.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&ss.staticReadWrite2.number)

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = m.s.staticReadOnlyProperty

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  m.s.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  m.s.staticReadWrite1 += 1

  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  m.s.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  m.s.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick S.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&m.s.staticReadWrite2.number)

  // CHECK: return
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions5testG{{[_0-9a-zA-Z]*}}F
func testG<T>(_ m: GenericMetaHolder<T>, gg: G<T>.Type) {
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = G<T>.staticReadOnlyProperty

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  G<T>.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  G<T>.staticReadWrite1 += 1

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  G<T>.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  G<T>.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&G<T>.staticReadWrite2.number)

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = gg.staticReadOnlyProperty

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  gg.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  gg.staticReadWrite1 += 1

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  gg.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  gg.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&gg.staticReadWrite2.number)

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE22staticReadOnlyPropertySivgZ
  let _ = m.g.staticReadOnlyProperty

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  m.g.staticReadWrite1 = 1

  // CHECK: alloc_stack $Int
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite1SivsZ
  // CHECK: dealloc_stack
  m.g.staticReadWrite1 += 1

  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  m.g.staticReadWrite2 = Box(number: 2)

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  m.g.staticReadWrite2.number += 5

  // CHECK: alloc_stack $Box
  // CHECK: metatype $@thick G<T>.Type
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvgZ
  // CHECK: store
  // CHECK: function_ref @$s19protocol_extensions10inout_funcyySizF
  // CHECK: load
  // CHECK: function_ref @$s19protocol_extensions2P1PAAE16staticReadWrite2AA3BoxVvsZ
  // CHECK: dealloc_stack
  inout_func(&m.g.staticReadWrite2.number)

  // CHECK: return
}

// ----------------------------------------------------------------------------
// Using protocol extension members with existentials
// ----------------------------------------------------------------------------
extension P1 {
  func f1() { }

  subscript (i: Int64) -> Bool {
    get { return true }
  }

  var prop: Bool {
    get { return true }
  }

  func returnsSelf() -> Self { return self }

  var prop2: Bool {
    get { return true }
    set { }
  }

  subscript (b: Bool) -> Bool {
    get { return b }
    set { }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions17testExistentials1{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[P:%[0-9]+]] : $*any P1, [[B:%[0-9]+]] : $Bool, [[I:%[0-9]+]] : $Int64):
func testExistentials1(_ p1: P1, b: Bool, i: Int64) {
  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[F1:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE2f1{{[_0-9a-zA-Z]*}}F
  // CHECK: apply [[F1]]<@opened([[UUID]], any P1) Self>([[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> ()
  p1.f1()

  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[GETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAEySbs5Int64Vcig
  // CHECK: apply [[GETTER]]<@opened([[UUID]], any P1) Self>([[I]], [[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Int64, @in_guaranteed τ_0_0) -> Bool
  // CHECK: store{{.*}} : $*Bool
  var b2 = p1[i]

  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[GETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE4propSbvg
  // CHECK: apply [[GETTER]]<@opened([[UUID]], any P1) Self>([[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> Bool
  // CHECK: store{{.*}} : $*Bool
  var b3 = p1.prop
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions17testExistentials2{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[P:%[0-9]+]] : $*any P1):
func testExistentials2(_ p1: P1) {
  // CHECK: [[P1A:%[0-9]+]] = alloc_box ${ var any P1 }
  // CHECK: [[P1AL:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[P1A]]
  // CHECK: [[PB:%.*]] = project_box [[P1AL]]
  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[FN:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE11returnsSelf{{[_0-9a-zA-Z]*}}F
  // CHECK: [[P1AINIT:%[0-9]+]] = init_existential_addr [[PB]] : $*any P1, $@opened([[UUID2:".*"]], any P1) Self
  // CHECK: apply [[FN]]<@opened([[UUID]], any P1) Self>([[P1AINIT]], [[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> @out τ_0_0
  var p1a: P1 = p1.returnsSelf()
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions23testExistentialsGetters{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[P:%[0-9]+]] : $*any P1):
func testExistentialsGetters(_ p1: P1) {
  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[FN:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE5prop2Sbvg
  // CHECK: [[B:%[0-9]+]] = apply [[FN]]<@opened([[UUID]], any P1) Self>([[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (@in_guaranteed τ_0_0) -> Bool
  let b: Bool = p1.prop2

  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr immutable_access [[P]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[GETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAEyS2bcig
  // CHECK: apply [[GETTER]]<@opened([[UUID]], any P1) Self>([[B]], [[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Bool, @in_guaranteed τ_0_0) -> Bool
  let b2: Bool = p1[b]
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions22testExistentialSetters{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[P:%[0-9]+]] : $*any P1, [[B:%[0-9]+]] : $Bool):
func testExistentialSetters(_ p1: P1, b: Bool) {
  var p1 = p1
  // CHECK: [[PBOX:%[0-9]+]] = alloc_box ${ var any P1 }
  // CHECK: [[PLIFETIME:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[PBOX]]
  // CHECK: [[PBP:%[0-9]+]] = project_box [[PLIFETIME]]
  // CHECK-NEXT: copy_addr [[P]] to [init] [[PBP]] : $*any P1
  // CHECK: [[WRITE:%.*]] = begin_access [modify] [unknown] [[PBP]]
  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr mutable_access [[WRITE]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[GETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE5prop2Sbvs
  // CHECK: apply [[GETTER]]<@opened([[UUID]], any P1) Self>([[B]], [[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Bool, @inout τ_0_0) -> ()
  // CHECK-NOT: deinit_existential_addr
  p1.prop2 = b

  // CHECK: [[WRITE:%.*]] = begin_access [modify] [unknown] [[PBP]]
  // CHECK: [[POPENED:%[0-9]+]] = open_existential_addr mutable_access [[WRITE]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[SUBSETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAEyS2bcis
  // CHECK: apply [[SUBSETTER]]<@opened([[UUID]], any P1) Self>([[B]], [[B]], [[POPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Bool, Bool, @inout τ_0_0) -> ()
  // CHECK-NOT: deinit_existential_addr [[PB]] : $*any P1
  p1[b] = b

  // CHECK: return
}

struct HasAP1 {
  var p1: P1

  var someP1: P1 {
    get { return p1 }
    set { p1 = newValue }
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions29testLogicalExistentialSetters{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[HASP1:%[0-9]+]] : $*HasAP1, [[B:%[0-9]+]] : $Bool)
func testLogicalExistentialSetters(_ hasAP1: HasAP1, _ b: Bool) {
  var hasAP1 = hasAP1
  // CHECK: [[HASP1_BOX:%[0-9]+]] = alloc_box ${ var HasAP1 }
  // CHECK: [[HASP1_LIFETIME:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[HASP1_BOX]]
  // CHECK: [[PBHASP1:%[0-9]+]] = project_box [[HASP1_LIFETIME]]
  // CHECK-NEXT: copy_addr [[HASP1]] to [init] [[PBHASP1]] : $*HasAP1
  // CHECK: [[WRITE:%.*]] = begin_access [modify] [unknown] [[PBHASP1]]
  // CHECK: [[P1_COPY:%[0-9]+]] = alloc_stack $any P1
  // CHECK: [[SOMEP1_GETTER:%[0-9]+]] = function_ref @$s19protocol_extensions6HasAP1V6someP1AA0F0_pvg : $@convention(method) (@in_guaranteed HasAP1) -> @out any P1
  // CHECK: [[RESULT:%[0-9]+]] = apply [[SOMEP1_GETTER]]([[P1_COPY]], [[WRITE]]) : $@convention(method) (@in_guaranteed HasAP1) -> @out any P1
  // CHECK: [[P1_OPENED:%[0-9]+]] = open_existential_addr mutable_access [[P1_COPY]] : $*any P1 to $*@opened([[UUID:".*"]], any P1) Self
  // CHECK: [[PROP2_SETTER:%[0-9]+]] = function_ref @$s19protocol_extensions2P1PAAE5prop2Sbvs : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Bool, @inout τ_0_0) -> ()
  // CHECK: apply [[PROP2_SETTER]]<@opened([[UUID]], any P1) Self>([[B]], [[P1_OPENED]]) : $@convention(method) <τ_0_0 where τ_0_0 : P1> (Bool, @inout τ_0_0) -> ()
  // CHECK: [[SOMEP1_SETTER:%[0-9]+]] = function_ref @$s19protocol_extensions6HasAP1V6someP1AA0F0_pvs : $@convention(method) (@in any P1, @inout HasAP1) -> ()
  // CHECK: apply [[SOMEP1_SETTER]]([[P1_COPY]], [[WRITE]]) : $@convention(method) (@in any P1, @inout HasAP1) -> ()
  // CHECK-NOT: deinit_existential_addr
  hasAP1.someP1.prop2 = b
  // CHECK: return
}

func plusOneP1() -> P1 {}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions38test_open_existential_semantics_opaque{{[_0-9a-zA-Z]*}}F
func test_open_existential_semantics_opaque(_ guaranteed: P1,
                                            immediate: P1) {
  var immediate = immediate
  // CHECK: [[IMMEDIATE_BOX:%.*]] = alloc_box ${ var any P1 }
  // CHECK: [[IMMEDIATE_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[IMMEDIATE_BOX]]
  // CHECK: [[PB:%.*]] = project_box [[IMMEDIATE_LIFETIME]]
  // CHECK: [[VALUE:%.*]] = open_existential_addr immutable_access %0
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])

  guaranteed.f1()
  
  // -- Need a guaranteed copy because it's immutable
  // CHECK: [[READ:%.*]] = begin_access [read] [unknown] [[PB]]
  // CHECK: copy_addr [[READ]] to [init] [[IMMEDIATE:%.*]] :
  // CHECK: [[VALUE:%.*]] = open_existential_addr immutable_access [[IMMEDIATE]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // -- Can consume the value from our own copy
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK: destroy_addr [[IMMEDIATE]]
  // CHECK: dealloc_stack [[IMMEDIATE]]
  immediate.f1()

  // CHECK: [[PLUS_ONE:%.*]] = alloc_stack $any P1
  // CHECK: [[VALUE:%.*]] = open_existential_addr immutable_access [[PLUS_ONE]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // -- Can consume the value from our own copy
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK: destroy_addr [[PLUS_ONE]]
  // CHECK: dealloc_stack [[PLUS_ONE]]
  plusOneP1().f1()
}

protocol CP1: class {}

extension CP1 {
  func f1() { }
}

func plusOneCP1() -> CP1 {}

// CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions37test_open_existential_semantics_class{{[_0-9a-zA-Z]*}}F
// CHECK: bb0([[ARG0:%.*]] : @guaranteed $any CP1, [[ARG1:%.*]] : @guaranteed $any CP1):
func test_open_existential_semantics_class(_ guaranteed: CP1,
                                           immediate: CP1) {
  var immediate = immediate
  // CHECK: [[IMMEDIATE_BOX:%.*]] = alloc_box ${ var any CP1 }
  // CHECK: [[IMMEDIATE_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[IMMEDIATE_BOX]]
  // CHECK: [[PB:%.*]] = project_box [[IMMEDIATE_LIFETIME]]

  // CHECK-NOT: copy_value [[ARG0]]
  // CHECK: [[VALUE:%.*]] = open_existential_ref [[ARG0]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK-NOT: destroy_value [[VALUE]]
  // CHECK-NOT: destroy_value [[ARG0]]
  guaranteed.f1()

  // CHECK: [[READ:%.*]] = begin_access [read] [unknown] [[PB]]
  // CHECK: [[IMMEDIATE:%.*]] = load [copy] [[READ]]
  // CHECK: [[VALUE:%.*]] = open_existential_ref [[IMMEDIATE]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK: destroy_value [[VALUE]]
  // CHECK-NOT: destroy_value [[IMMEDIATE]]
  immediate.f1()

  // CHECK: [[F:%.*]] = function_ref {{.*}}plusOneCP1
  // CHECK: [[PLUS_ONE:%.*]] = apply [[F]]()
  // CHECK: [[VALUE:%.*]] = open_existential_ref [[PLUS_ONE]]
  // CHECK: [[METHOD:%.*]] = function_ref
  // CHECK: apply [[METHOD]]<{{.*}}>([[VALUE]])
  // CHECK: destroy_value [[VALUE]]
  // CHECK-NOT: destroy_value [[PLUS_ONE]]
  plusOneCP1().f1()
}
// CHECK: } // end sil function '$s19protocol_extensions37test_open_existential_semantics_class{{[_0-9a-zA-Z]*}}F'

protocol InitRequirement {
  init(c: C)
}

extension InitRequirement {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions15InitRequirementPAAE1dxAA1DC_tcfC : $@convention(method) <Self where Self : InitRequirement> (@owned D, @thick Self.Type) -> @out Self
  // CHECK:       bb0([[OUT:%.*]] : $*Self, [[ARG:%.*]] : @owned $D, [[SELF_TYPE:%.*]] : $@thick Self.Type):
  init(d: D) {
    // CHECK:      [[SELF_BOX:%.*]] = alloc_box
    // CHECK-NEXT: [[UNINIT_SELF:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
    // CHECK-NEXT: [[SELF_BOX_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[UNINIT_SELF]]
    // CHECK-NEXT: [[SELF_BOX_ADDR:%.*]] = project_box [[SELF_BOX_LIFETIME]]
    // CHECK:      [[SELF_BOX:%.*]] = alloc_stack $Self
    // CHECK-NEXT: [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK-NEXT: [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK-NEXT: [[ARG_COPY_CAST:%.*]] = upcast [[ARG_COPY]]
    // CHECK:      [[DELEGATEE:%.*]] = witness_method $Self, #InitRequirement.init!allocator : {{.*}} : $@convention(witness_method: InitRequirement) <τ_0_0 where τ_0_0 : InitRequirement> (@owned C, @thick τ_0_0.Type) -> @out τ_0_0
    // CHECK-NEXT: apply [[DELEGATEE]]<Self>([[SELF_BOX]], [[ARG_COPY_CAST]], [[SELF_TYPE]])
    // CHECK-NEXT: end_borrow [[BORROWED_ARG]]
    // CHECK-NEXT: copy_addr [take] [[SELF_BOX]] to [[SELF_BOX_ADDR]]
    // CHECK-NEXT: dealloc_stack [[SELF_BOX]]
    // CHECK-NEXT: copy_addr [[SELF_BOX_ADDR]] to [init] [[OUT]]
    // CHECK-NEXT: destroy_value [[ARG]]
    // CHECK-NEXT: end_borrow [[SELF_BOX_LIFETIME]]
    // CHECK-NEXT: destroy_value [[UNINIT_SELF]]
    // CHECK:      return
    self.init(c: d)
  }

  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions15InitRequirementPAAE2d2xAA1DC_tcfC : $@convention(method)
  // CHECK:       bb0([[OUT:%.*]] : $*Self, [[ARG:%.*]] : @owned $D, [[SELF_TYPE:%.*]] : $@thick Self.Type):
  init(d2: D) {
    // CHECK:      [[SELF_BOX:%.*]] = alloc_box
    // CHECK-NEXT: [[UNINIT_SELF:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
    // CHECK-NEXT: [[SELF_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[UNINIT_SELF]]
    // CHECK-NEXT: [[SELF_BOX_ADDR:%.*]] = project_box [[SELF_LIFETIME]]
    // CHECK:      [[SELF_BOX:%.*]] = alloc_stack $Self
    // CHECK-NEXT: [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK-NEXT: [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:      [[DELEGATEE:%.*]] = function_ref @$s19protocol_extensions15InitRequirementPAAE1dxAA1DC_tcfC :
    // CHECK-NEXT: apply [[DELEGATEE]]<Self>([[SELF_BOX]], [[ARG_COPY]], [[SELF_TYPE]])
    // CHECK-NEXT: end_borrow [[BORROWED_ARG]]
    // CHECK-NEXT: copy_addr [take] [[SELF_BOX]] to [[SELF_BOX_ADDR]]
    // CHECK-NEXT: dealloc_stack [[SELF_BOX]]
    // CHECK-NEXT: copy_addr [[SELF_BOX_ADDR]] to [init] [[OUT]]
    // CHECK-NEXT: destroy_value [[ARG]]
    // CHECK-NEXT: end_borrow [[SELF_LIFETIME]]
    // CHECK-NEXT: destroy_value [[UNINIT_SELF]]
    // CHECK: } // end sil function '$s19protocol_extensions15InitRequirementPAAE2d2xAA1DC_tcfC'
    self.init(d: d2)
  }

  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions15InitRequirementPAAE2c2xAA1CC_tcfC  : $@convention(method)
  // CHECK:       bb0([[OUT:%.*]] : $*Self, [[ARG:%.*]] : @owned $C, [[SELF_TYPE:%.*]] : $@thick Self.Type):
  init(c2: C) {
    // CHECK:      [[SELF_BOX:%.*]] = alloc_box
    // CHECK-NEXT: [[UNINIT_SELF:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
    // CHECK-NEXT: [[SELF_LIFETIME:%.*]] = begin_borrow [lexical] [var_decl] [[UNINIT_SELF]]
    // CHECK-NEXT: [[SELF_BOX_ADDR:%.*]] = project_box [[SELF_LIFETIME]]
    // CHECK:      [[SELF_BOX:%.*]] = alloc_stack $Self
    // CHECK-NEXT: [[SELF_TYPE:%.*]] = metatype $@thick Self.Type
    // CHECK-NEXT: [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
    // CHECK-NEXT: [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
    // CHECK:      [[DELEGATEE:%.*]] = witness_method $Self, #InitRequirement.init!allocator
    // CHECK-NEXT: apply [[DELEGATEE]]<Self>([[SELF_BOX]], [[ARG_COPY]], [[SELF_TYPE]])
    // CHECK-NEXT: end_borrow [[BORROWED_ARG]]
    // CHECK-NEXT: [[ACCESS:%.*]] = begin_access [modify] [unknown] [[SELF_BOX_ADDR]]
    // CHECK-NEXT: copy_addr [take] [[SELF_BOX]] to [[ACCESS]]
    // CHECK-NEXT: end_access [[ACCESS]]
    // CHECK-NEXT: dealloc_stack [[SELF_BOX]]
    // CHECK-NEXT: copy_addr [[SELF_BOX_ADDR]] to [init] [[OUT]]
    // CHECK-NEXT: destroy_value [[ARG]]
    // CHECK-NEXT: end_borrow [[SELF_LIFETIME]]
    // CHECK-NEXT: destroy_value [[UNINIT_SELF]]
    // CHECK:      return
    self = Self(c: c2)
  }
}

protocol ClassInitRequirement: class {
  init(c: C)
}

extension ClassInitRequirement {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions20ClassInitRequirementPAAE{{[_0-9a-zA-Z]*}}fC : $@convention(method) <Self where Self : ClassInitRequirement> (@owned D, @thick Self.Type) -> @owned Self
  // CHECK:       bb0([[ARG:%.*]] : @owned $D, [[SELF_TYPE:%.*]] : $@thick Self.Type):
  // CHECK:         [[BORROWED_ARG:%.*]] = begin_borrow [[ARG]]
  // CHECK:         [[ARG_COPY:%.*]] = copy_value [[BORROWED_ARG]]
  // CHECK:         [[ARG_COPY_CAST:%.*]] = upcast [[ARG_COPY]]
  // CHECK:         [[DELEGATEE:%.*]] = witness_method $Self, #ClassInitRequirement.init!allocator : {{.*}} : $@convention(witness_method: ClassInitRequirement) <τ_0_0 where τ_0_0 : ClassInitRequirement> (@owned C, @thick τ_0_0.Type) -> @owned τ_0_0
  // CHECK:         apply [[DELEGATEE]]<Self>([[ARG_COPY_CAST]], [[SELF_TYPE]])
  // CHECK:         end_borrow [[BORROWED_ARG]]
  
  // CHECK: } // end sil function '$s19protocol_extensions20ClassInitRequirementPAAE{{[_0-9a-zA-Z]*}}fC'
  init(d: D) {
    self.init(c: d)
  }
}

@objc class OC {}
@objc class OD: OC {}

@objc protocol ObjCInitRequirement {
  init(c: OC, d: OC)
}

func foo(_ t: ObjCInitRequirement.Type, c: OC) -> ObjCInitRequirement {
  return t.init(c: OC(), d: OC())
}

extension ObjCInitRequirement {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions19ObjCInitRequirementPAAE{{[_0-9a-zA-Z]*}}fC : $@convention(method) <Self where Self : ObjCInitRequirement> (@owned OD, @thick Self.Type) -> @owned Self
  // CHECK:       bb0([[ARG:%.*]] : @owned $OD, [[SELF_TYPE:%.*]] : $@thick Self.Type):
  // CHECK:         [[OBJC_SELF_TYPE:%.*]] = thick_to_objc_metatype [[SELF_TYPE]]
  // CHECK:         [[SELF:%.*]] = alloc_ref_dynamic [objc] [[OBJC_SELF_TYPE]] : $@objc_metatype Self.Type, $Self
  // CHECK:         [[BORROWED_ARG_1:%.*]] = begin_borrow [[ARG]]
  // CHECK:         [[BORROWED_ARG_1_UPCAST:%.*]] = upcast [[BORROWED_ARG_1]]
  // CHECK:         [[BORROWED_ARG_2:%.*]] = begin_borrow [[ARG]]
  // CHECK:         [[BORROWED_ARG_2_UPCAST:%.*]] = upcast [[BORROWED_ARG_2]]
  // CHECK:         [[WITNESS:%.*]] = objc_method [[SELF]] : $Self, #ObjCInitRequirement.init!initializer.foreign : {{.*}}, $@convention(objc_method) <τ_0_0 where τ_0_0 : ObjCInitRequirement> (OC, OC, @owned τ_0_0) -> @owned τ_0_0
  // CHECK:         apply [[WITNESS]]<Self>([[BORROWED_ARG_1_UPCAST]], [[BORROWED_ARG_2_UPCAST]], [[SELF]])
  // CHECK:         end_borrow [[BORROWED_ARG_1]]
  // CHECK: } // end sil function '$s19protocol_extensions19ObjCInitRequirementPAAE{{[_0-9a-zA-Z]*}}fC'
  init(d: OD) {
    self.init(c: d, d: d)
  }
}

// rdar://problem/21370992 - delegation from an initializer in a
// protocol extension to an @objc initializer in a class.
class ObjCInitClass {
  @objc required init() { }
}

protocol ProtoDelegatesToObjC { }

extension ProtoDelegatesToObjC where Self : ObjCInitClass {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions20ProtoDelegatesToObjCPA2A0F10CInitClassC{{[_0-9a-zA-Z]*}}fC
  // CHECK: bb0([[STR:%[0-9]+]] : @owned $String, [[SELF_META:%[0-9]+]] : $@thick Self.Type):
  init(string: String) {
    // CHECK:   [[SELF_BOX:%[0-9]+]] = alloc_box $<τ_0_0 where τ_0_0 : ObjCInitClass, τ_0_0 : ProtoDelegatesToObjC> { var τ_0_0 } <Self>
    // CHECK:   [[MARKED_SELF_BOX:%[0-9]+]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
    // CHECK:   [[SELF_LIFETIME:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[MARKED_SELF_BOX]]
    // CHECK:   [[PB_SELF_BOX:%.*]] = project_box [[SELF_LIFETIME]]
    // CHECK:   [[SELF_META_C:%[0-9]+]] = upcast [[SELF_META]] : $@thick Self.Type to $@thick ObjCInitClass.Type
    // CHECK:   [[OBJC_INIT:%[0-9]+]] = class_method [[SELF_META_C]] : $@thick ObjCInitClass.Type, #ObjCInitClass.init!allocator
    // CHECK:   [[SELF_RESULT:%[0-9]+]] = apply [[OBJC_INIT]]([[SELF_META_C]])
    // CHECK:   [[SELF_RESULT_AS_SELF:%[0-9]+]] = unchecked_ref_cast [[SELF_RESULT]] : $ObjCInitClass to $Self
    // CHECK:   assign [[SELF_RESULT_AS_SELF]] to [[PB_SELF_BOX]] : $*Self
    self.init()
  }
}

// Delegating from an initializer in a protocol extension where Self
// has a superclass to a required initializer of that class.
class RequiredInitClass {
  required init() { }
}

protocol ProtoDelegatesToRequired { }

extension ProtoDelegatesToRequired where Self : RequiredInitClass {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions24ProtoDelegatesToRequiredPA2A0F9InitClassC{{[_0-9a-zA-Z]*}}fC 
  // CHECK: bb0([[STR:%[0-9]+]] : @owned $String, [[SELF_META:%[0-9]+]] : $@thick Self.Type):
  init(string: String) {
  // CHECK:   [[SELF_BOX:%[0-9]+]] = alloc_box $<τ_0_0 where τ_0_0 : RequiredInitClass, τ_0_0 : ProtoDelegatesToRequired> { var τ_0_0 } <Self>
  // CHECK:   [[MARKED_SELF_BOX:%[0-9]+]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
  // CHECK:   [[SELF_LIFETIME:%[0-9]+]] = begin_borrow [lexical] [var_decl] [[MARKED_SELF_BOX]]
  // CHECK:   [[PB_SELF_BOX:%.*]] = project_box [[SELF_LIFETIME]]
  // CHECK:   [[SELF_META_AS_CLASS_META:%[0-9]+]] = upcast [[SELF_META]] : $@thick Self.Type to $@thick RequiredInitClass.Type
  // CHECK:   [[INIT:%[0-9]+]] = class_method [[SELF_META_AS_CLASS_META]] : $@thick RequiredInitClass.Type, #RequiredInitClass.init!allocator : (RequiredInitClass.Type) -> () -> RequiredInitClass, $@convention(method) (@thick RequiredInitClass.Type) -> @owned RequiredInitClass
  // CHECK:   [[SELF_RESULT:%[0-9]+]] = apply [[INIT]]([[SELF_META_AS_CLASS_META]]) : $@convention(method) (@thick RequiredInitClass.Type) -> @owned RequiredInitClass
  // CHECK:   [[SELF_RESULT_AS_SELF:%[0-9]+]] = unchecked_ref_cast [[SELF_RESULT]] : $RequiredInitClass to $Self
  // CHECK:   assign [[SELF_RESULT_AS_SELF]] to [[PB_SELF_BOX]] : $*Self
    self.init()
  }
}

// ----------------------------------------------------------------------------
// Default implementations via protocol extensions
// ----------------------------------------------------------------------------

protocol P2 {
  associatedtype A
  func f1(_ a: A)
  func f2(_ a: A)
  var x: A { get }
}

extension P2 {
  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions2P2PAAE2f1{{[_0-9a-zA-Z]*}}F
  // CHECK: witness_method $Self, #P2.f2 :
  // CHECK: function_ref @$s19protocol_extensions2P2PAAE2f3{{[_0-9a-zA-Z]*}}F
  // CHECK: return
  func f1(_ a: A) {
    f2(a)
    f3(a)
  }

  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions2P2PAAE2f2{{[_0-9a-zA-Z]*}}F
  // CHECK: witness_method $Self, #P2.f1 :
  // CHECK: function_ref @$s19protocol_extensions2P2PAAE2f3{{[_0-9a-zA-Z]*}}F
  // CHECK: return
  func f2(_ a: A) {
    f1(a)
    f3(a)
  }

  func f3(_ a: A) {}

  // CHECK-LABEL: sil hidden [ossa] @$s19protocol_extensions2P2PAAE2f4{{[_0-9a-zA-Z]*}}F
  // CHECK: witness_method $Self, #P2.f1 :
  // CHECK: witness_method $Self, #P2.f2 :
  // CHECK: return
  func f4() {
    f1(x)
    f2(x)
  }
}
