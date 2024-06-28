// RUN: %target-swift-frontend -module-name access_marker_verify -enable-verify-exclusivity -enforce-exclusivity=checked -emit-silgen -swift-version 4 -parse-as-library %s | %FileCheck %s
// RUN: %target-swift-frontend -module-name access_marker_verify -enable-verify-exclusivity -enforce-exclusivity=checked -Onone -emit-sil -swift-version 4 -parse-as-library %s -o /dev/null
// RUN: %target-swift-frontend -module-name access_marker_verify -enable-verify-exclusivity -enforce-exclusivity=checked -O -emit-sil -swift-version 4 -parse-as-library %s -o /dev/null
// REQUIRES: asserts

// Test the combination of SILGen + DiagnoseStaticExclusivity with verification.
//
// This is a collection of tests that cover SILGen cases that require special
// handling for exclusivity verification. SILGen must generate access markers,
// possibly unenforced, to satisfy the verification run during the
// DiagnoseStaticExclusivity pass.
//
// These cases are mostly covered by existing SILGen tests, but need to be
// repeated here to ensure they are run with exclusivity verification enabled.
import SwiftShims

protocol P {}
struct StructP : P {}

protocol PBar {
  func bar()
}

class BaseClass  {
  init() {}
}
class SubClass : BaseClass {
  override required init() {}
}

enum E {
case V(Int)
}

func takesClosure(_ f: () -> ()) {
  f()
}

// --- struct initialization.
struct StructOfInt {
  var i: Int

  init() {
    i = 1
  }

  mutating func changeMe() {
    i = 3
  }
}
// The verifier ignores the load of the self box.
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify11StructOfIntVACycfC : $@convention(method) (@thin StructOfInt.Type) -> StructOfInt {
// CHECK: bb0(%0 : $@thin StructOfInt.Type):
// CHECK:   [[BOX:%.*]] = alloc_box ${ var StructOfInt }, var, name "self"
// CHECK:   [[UNINIT:%.*]] = mark_uninitialized [rootself] [[BOX]] : ${ var StructOfInt }
// CHECK:   [[LIFETIME:%.*]] = begin_borrow [var_decl] [[UNINIT]]
// CHECK:   [[PROJ:%.*]] = project_box [[LIFETIME]] : ${ var StructOfInt }, 0
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJ]] : $*StructOfInt
// CHECK:   [[ADR:%.*]] = struct_element_addr [[ACCESS]] : $*StructOfInt, #StructOfInt.i
// CHECK:   assign %{{.*}} to [[ADR]] : $*Int
// CHECK:   end_access [[ACCESS]] : $*StructOfInt
// CHECK-NOT: begin_access
// CHECK:   [[VAL:%.*]] = load [trivial] [[PROJ]] : $*StructOfInt
// CHECK:   destroy_value [[UNINIT]] : ${ var StructOfInt }
// CHECK:   return [[VAL]] : $StructOfInt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify11StructOfIntVACycfC'

// --- class initialization.
class SuperHasInt {
  var i: Int

  init() {
    i = 3
  }
}

class SubHasInt : SuperHasInt {
  var j: Int
  
  override init() {
    j = 4
    super.init()
  }

  init(x: Int) {
    j = x
    super.init()
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify11SuperHasIntCACycfc : $@convention(method) (@owned SuperHasInt) -> @owned SuperHasInt {
// CHECK:   bb0(%0 : @owned $SuperHasInt):
// CHECK:   [[UNINIT:%.*]] = mark_uninitialized [rootself] %0 : $SuperHasInt
// CHECK:   [[BORROW:%.*]] = begin_borrow [[UNINIT]] : $SuperHasInt
// CHECK:   [[ADR:%.*]] = ref_element_addr [[BORROW]] : $SuperHasInt, #SuperHasInt.i
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [dynamic] [[ADR]] : $*Int
// CHECK:   assign %{{.*}} to [[ACCESS]] : $*Int
// CHECK:   end_access [[ACCESS]] : $*Int
// CHECK:   end_borrow [[BORROW]] : $SuperHasInt
// CHECK-NOT: begin_access
// CHECK:   [[VAL:%.*]] = copy_value [[UNINIT]] : $SuperHasInt
// CHECK:   destroy_value [[UNINIT]] : $SuperHasInt
// CHECK:   return [[VAL]] : $SuperHasInt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify11SuperHasIntCACycfc'

// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify9SubHasIntCACycfc : $@convention(method) (@owned SubHasInt) -> @owned SubHasInt {
// CHECK: bb0(%0 : @owned $SubHasInt):
// CHECK:   [[BOX:%.*]] = alloc_box ${ var SubHasInt }, let, name "self"
// CHECK:   [[UNINIT:%.*]] = mark_uninitialized [derivedself] [[BOX]] : ${ var SubHasInt }
// CHECK:   [[UNINIT_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[UNINIT]]
// CHECK:   [[PROJ:%.*]] = project_box [[UNINIT_LIFETIME]] : ${ var SubHasInt }, 0
// CHECK-NOT: begin_access
// CHECK:   store %0 to [init] [[PROJ]] : $*SubHasInt
// CHECK-NOT: begin_access
// CHECK:   [[BORROW:%.*]] = load_borrow [[PROJ]] : $*SubHasInt
// CHECK:   [[ADR:%.*]] = ref_element_addr [[BORROW]] : $SubHasInt, #SubHasInt.j
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [dynamic] [[ADR]] : $*Int
// CHECK:   assign %{{.*}} to [[ACCESS]] : $*Int
// CHECK:   end_access [[ACCESS]] : $*Int
// CHECK:   end_borrow [[BORROW]] : $SubHasInt
// CHECK-NOT: begin_access
// CHECK:   load [take] [[PROJ]] : $*SubHasInt
// CHECK:   upcast %{{.*}} : $SubHasInt to $SuperHasInt
// CHECK:   function_ref @$s20access_marker_verify11SuperHasIntCACycfc : $@convention(method) (@owned SuperHasInt) -> @owned SuperHasInt
// CHECK:   apply
// CHECK:   unchecked_ref_cast %{{.*}} : $SuperHasInt to $SubHasInt
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [init] [[PROJ]] : $*SubHasInt
// CHECK:   [[VAL:%.*]] = load [copy] [[PROJ]] : $*SubHasInt
// CHECK:   end_borrow [[UNINIT_LIFETIME]]
// CHECK:   destroy_value [[UNINIT]] : ${ var SubHasInt }
// CHECK:   return [[VAL]] : $SubHasInt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify9SubHasIntCACycfc'

// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify9SubHasIntC1xACSi_tcfc : $@convention(method) (Int, @owned SubHasInt) -> @owned SubHasInt {
// CHECK: bb0(%0 : $Int, %1 : @owned $SubHasInt):
// CHECK:   [[BOX:%.*]] = alloc_box ${ var SubHasInt }, let, name "self"
// CHECK:   [[UNINIT:%.*]] = mark_uninitialized [derivedself] [[BOX]] : ${ var SubHasInt }
// CHECK:   [[UNINIT_LIFETIME:%[^,]+]] = begin_borrow [lexical] [var_decl] [[UNINIT]]
// CHECK:   [[PROJ:%.*]] = project_box [[UNINIT_LIFETIME]] : ${ var SubHasInt }, 0
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [init] [[PROJ]] : $*SubHasInt
// CHECK-NOT: begin_access
// CHECK:   [[BORROW:%.*]] = load_borrow [[PROJ]] : $*SubHasInt
// CHECK:   [[ADR:%.*]] = ref_element_addr [[BORROW]] : $SubHasInt, #SubHasInt.j
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [dynamic] [[ADR]] : $*Int
// CHECK:   assign %0 to [[ACCESS]] : $*Int
// CHECK:   end_access [[ACCESS]] : $*Int
// CHECK:   end_borrow [[BORROW]] : $SubHasInt
// CHECK-NOT: begin_access
// CHECK:   load [take] [[PROJ]] : $*SubHasInt
// CHECK:   upcast %{{.*}} : $SubHasInt to $SuperHasInt
// CHECK:   function_ref @$s20access_marker_verify11SuperHasIntCACycfc : $@convention(method) (@owned SuperHasInt) -> @owned SuperHasInt
// CHECK:   apply
// CHECK:   unchecked_ref_cast %{{.*}} : $SuperHasInt to $SubHasInt
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [init] [[PROJ]] : $*SubHasInt
// CHECK:   [[VAL:%.*]] = load [copy] [[PROJ]] : $*SubHasInt
// CHECK:   end_borrow [[UNINIT_LIFETIME]]
// CHECK:   destroy_value [[UNINIT]] : ${ var SubHasInt }
// CHECK:   return [[VAL]] : $SubHasInt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify9SubHasIntC1xACSi_tcfc'

// --- access `let` property.
class LetClass {
  let x = 3
}

// FIXME: should be a [unknown] access.
//
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify10testGetLet1cSiAA0F5ClassC_tF : $@convention(thin) (@guaranteed LetClass) -> Int {
// CHECK: bb0(%0 : @guaranteed $LetClass):
// CHECK:   ref_element_addr
// CHECK:   load [trivial]
// CHECK:   return
// CHECK-LABEL: } // end sil function '$s20access_marker_verify10testGetLet1cSiAA0F5ClassC_tF'
func testGetLet(c: LetClass) -> Int {
  return c.x
}

// --- initialize let property and superclass.
struct IntWrapper {
  var x: Int
}

final class SubWrapper : BaseClass {
  let val: IntWrapper

  init(_ val: IntWrapper) {
    self.val = val
    super.init()
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify10SubWrapperCyAcA03IntE0Vcfc : $@convention(method) (IntWrapper, @owned SubWrapper) -> @owned SubWrapper {
// CHECK: bb0(%0 : $IntWrapper, %1 : @owned $SubWrapper):
// CHECK:   alloc_box ${ var SubWrapper }, let, name "self"
// CHECK:   mark_uninitialized [derivedself]
// CHECK:   project_box
// CHECK-NOT: begin_access
// CHECK:   store %1 to [init]
// CHECK-NOT: begin_access
// CHECK:   load_borrow
// CHECK:   ref_element_addr
// CHECK:   assign %0 to
// CHECK:   end_borrow
// CHECK-NOT: begin_access
// CHECK:   load [take]
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [init]
// CHECK:   [[VAL:%.*]] = load [copy]
// CHECK:   destroy_value %{{.*}} : ${ var SubWrapper }
// CHECK:   return [[VAL]] : $SubWrapper
// CHECK-LABEL: } // end sil function '$s20access_marker_verify10SubWrapperCyAcA03IntE0Vcfc'

// --- captured local.
func testCaptureLocal() -> ()->() {
  var x = 1
  let f = { x = 3 }
  _ = x
  return f
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify16testCaptureLocalyycyF : $@convention(thin) () -> @owned @callee_guaranteed () -> () {
// CHECK: bb0:
// CHECK:   [[BOX:%.*]] = alloc_box ${ var Int }, var, name "x"
// CHECK:   [[LIFETIME:%.*]] = begin_borrow [var_decl] [[BOX]]
// CHECK:   [[PROJ:%.*]] = project_box [[LIFETIME]]
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[PROJ]] : $*Int
// CHECK:   store %{{.*}} to [trivial] [[ACCESS]]
// CHECK:   end_access
// CHECK:   [[CAPTURE:%.*]] = copy_value [[LIFETIME]] : ${ var Int }
// CHECK:   partial_apply [callee_guaranteed] %{{.*}}([[CAPTURE]]) : $@convention(thin) (@guaranteed { var Int }) -> ()
// CHECK:   begin_access [read] [unknown] [[PROJ]]
// CHECK:   [[VAL:%.*]] = load [trivial]
// CHECK:   end_access
// CHECK-NOT: begin_access
// CHECK:   return {{.*}} : $@callee_guaranteed () -> ()
// CHECK-LABEL: } // end sil function '$s20access_marker_verify16testCaptureLocalyycyF'

// --- mutating struct.
func testModifyS(_ arg: StructOfInt) -> StructOfInt {
  var lhs: StructOfInt
  lhs = arg
  lhs.changeMe()
  return lhs
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify11testModifySyAA11StructOfIntVADF : $@convention(thin) (StructOfInt) -> StructOfInt {
// CHECK: bb0(%0 : $StructOfInt):
// CHECK:   alloc_box ${ var StructOfInt }, var, name "lhs"
// CHECK:   mark_uninitialized [var]
// CHECK:   project_box
// CHECK:   begin_access [modify] [unknown]
// CHECK:   assign
// CHECK:   end_access
// CHECK:   begin_access [modify] [unknown]
// CHECK:   function_ref @$s20access_marker_verify11StructOfIntV8changeMeyyF : $@convention(method) (@inout StructOfInt) -> ()
// CHECK:   apply
// CHECK:   end_access
// CHECK:   begin_access [read] [unknown]
// CHECK:   load [trivial]
// CHECK:   end_access
// CHECK-LABEL: } // end sil function '$s20access_marker_verify11testModifySyAA11StructOfIntVADF'

// --- initialize LValue.
protocol HasIntGetter {
  var x: Int { get }
}
func testInitLValue(p: HasIntGetter) -> Int {
  var x = p.x
  return x
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify14testInitLValue1pSiAA12HasIntGetter_p_tF : $@convention(thin) (@in_guaranteed any HasIntGetter) -> Int {
// CHECK: bb0(%0 : $*any HasIntGetter):
// CHECK:   alloc_box ${ var Int }, var, name "x"
// CHECK:   [[PROJ:%.*]] = project_box
// CHECK:   [[OPENED:%.*]] = open_existential_addr immutable_access %0
// CHECK:   witness_method $@opened
// CHECK:   [[VALUE:%.*]] = apply %{{.*}}<@opened("{{.*}}", any HasIntGetter) Self>([[OPENED]])
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[PROJ]] : $*Int
// CHECK:   store [[VALUE]] to [trivial] [[ACCESS]] : $*Int
// CHECK:   end_access [[ACCESS]]
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJ]]
// CHECK:   [[RELOAD:%.*]] = load [trivial] [[ACCESS]]
// CHECK:   end_access [[ACCESS]]
// CHECK:   return [[RELOAD]]
// CHECK-LABEL: } // end sil function '$s20access_marker_verify14testInitLValue1pSiAA12HasIntGetter_p_tF'

// --- initialize let.
func testCopyS(_ arg: StructOfInt) -> StructOfInt {
  let lhs: StructOfInt
  lhs = arg
  return lhs
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify9testCopySyAA11StructOfIntVADF : $@convention(thin) (StructOfInt) -> StructOfInt {
// CHECK: bb0(%0 : $StructOfInt):
// CHECK:   alloc_stack [var_decl] $StructOfInt, let, name "lhs"
// CHECK:   [[UNINIT:%.*]] = mark_uninitialized [var]
// CHECK-NOT: begin_access
// CHECK:   assign %0 to [[UNINIT]] : $*StructOfInt
// CHECK-NOT: begin_access
// CHECK:   %5 = load [trivial] [[UNINIT]] : $*StructOfInt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify9testCopySyAA11StructOfIntVADF'

// --- local var init (single buffer).
func testLocalVarInit(_ arg: StructOfInt) -> Int {
  var lhs = arg
  return lhs.i
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify16testLocalVarInitySiAA11StructOfIntVF : $@convention(thin) (StructOfInt) -> Int {
// CHECK: bb0(%0 : $StructOfInt):
// CHECK:   alloc_box ${ var StructOfInt }, var, name "lhs"
// CHECK:   [[BOX:%.*]] = project_box
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[BOX]]
// CHECK:   store %0 to [trivial] [[ACCESS]]
// CHECK:   end_access
// CHECK-LABEL: } // end sil function '$s20access_marker_verify16testLocalVarInitySiAA11StructOfIntVF'

// --- init generic enum
enum GenericEnum<T> {
case V(T)

  init?(t: T) {
    self = .V(t)
  }
}

func testInitGenericEnum<T>(t: T) -> GenericEnum<T>? {
  return GenericEnum(t: t)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify11GenericEnumO1tACyxGSgx_tcfC : $@convention(method) <T> (@in T, @thin GenericEnum<T>.Type) -> @out Optional<GenericEnum<T>> {
// CHECK: bb0(%0 : $*Optional<GenericEnum<T>>, %1 : $*T, %2 : $@thin GenericEnum<T>.Type):
// CHECK:   alloc_box $<τ_0_0> { var GenericEnum<τ_0_0> } <T>, var, name "self"
// CHECK:   mark_uninitialized [delegatingself] %3 : $<τ_0_0> { var GenericEnum<τ_0_0> } <T>
// CHECK:   [[PROJ:%.*]] = project_box
// CHECK:   [[STK:%.*]] = alloc_stack $GenericEnum<T>
// CHECK:   [[ENUMDATAADDR:%.*]] = init_enum_data_addr [[STK]]
// CHECK:   copy_addr %1 to [init] [[ENUMDATAADDR]] : $*T
// CHECK:   inject_enum_addr
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unknown] [[PROJ]]
// CHECK:   copy_addr [take] %{{.*}} to [[ACCESS]] : $*GenericEnum<T>
// CHECK:   end_access [[ACCESS]] : $*GenericEnum<T>
// CHECK:   [[ADR2:%.*]] = init_enum_data_addr %0
// CHECK-NOT: begin_access
// CHECK:   copy_addr %{{.*}} to [init] [[ADR2]] : $*GenericEnum<T>
// CHECK:   inject_enum_addr %0 : $*Optional<GenericEnum<T>>, #Optional.some!enumelt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify11GenericEnumO1tACyxGSgx_tcfC'

// -- initialize indirect enum.
enum IndirectEnum {
  indirect case V(Int)
}

func testIndirectEnum() -> IndirectEnum {
  return IndirectEnum.V(3)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify16testIndirectEnumAA0eF0OyF : $@convention(thin) () -> @owned IndirectEnum {
// CHECK: bb0:
// CHECK:   alloc_box ${ var Int }
// CHECK:   [[PROJ:%.*]] = project_box
// CHECK:   apply
// CHECK:   [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[PROJ]]
// CHECK:   store %{{.*}} to [trivial] [[ACCESS]] : $*Int
// CHECK:   end_access
// CHECK:   enum $IndirectEnum, #IndirectEnum.V!enumelt
// CHECK:   return
// CHECK-LABEL: } // end sil function '$s20access_marker_verify16testIndirectEnumAA0eF0OyF'

// -- indirect enum with getter.
enum IntEnum {
  indirect case int(Int)

  var getValue: Int {
    switch self {
    case .int(let x): return x
    }
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify7IntEnumO8getValueSivg : $@convention(method) (@guaranteed IntEnum) -> Int {
// CHECK: bb0(%0 : @guaranteed $IntEnum):
// CHECK:   switch_enum %{{.*}} : $IntEnum, case #IntEnum.int!enumelt: bb1
// CHECK: bb1(%{{.*}} : @guaranteed ${ var Int }):
// CHECK:   [[PROJ:%.*]] = project_box
// CHECK-NOT: begin_access
// CHECK:   load [trivial] [[PROJ]] : $*Int
// CHECK-LABEL: } // end sil function '$s20access_marker_verify7IntEnumO8getValueSivg'

// Also test SILGenPattern for address-only enums with indirect loadable payloads.
enum IntTEnum<T> {
  indirect case int(Int)
  case other(T)

  var getValue: Int {
    switch self {
    case .int(let x): return x
    case .other: return 0
    }
  }
}
// IntTEnum.getValue.getter
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify8IntTEnumO8getValueSivg : $@convention(method) <T> (@in_guaranteed IntTEnum<T>) -> Int {
// CHECK: bb0(%0 : $*IntTEnum<T>):
// CHECK:   switch_enum_addr %{{.*}} : $*IntTEnum<T>, case #IntTEnum.int!enumelt: bb1, case #IntTEnum.other!enumelt: bb2
// CHECK: bb1:
// CHECK:   [[UTEDA:%.*]] = unchecked_take_enum_data_addr %{{.*}} : $*IntTEnum<T>, #IntTEnum.int!enumelt
// CHECK-NOT: begin_access
// CHECK:   [[BOX:%.*]] = load [take] [[UTEDA]] : $*<τ_0_0> { var Int } <T>
// CHECK:   [[PROJ:%.*]] = project_box [[BOX]] : $<τ_0_0> { var Int } <T>, 0
// CHECK-NOT: begin_access
// CHECK:   load [trivial] [[PROJ]] : $*Int
// CHECK-LABEL: } // end sil function '$s20access_marker_verify8IntTEnumO8getValueSivg'

// -- indirect enum reference.
enum RefEnum {
  indirect case ref(BaseClass)

  var getValue: BaseClass {
    switch self {
    case .ref(let c): return c
    }
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify7RefEnumO8getValueAA9BaseClassCvg : $@convention(method) (@guaranteed RefEnum) -> @owned BaseClass {
// CHECK: bb0(%0 : @guaranteed $RefEnum):
// CHECK:   switch_enum %{{.*}} : $RefEnum, case #RefEnum.ref!enumelt: bb1
// CHECK: bb1(%{{.*}} : @guaranteed ${ var BaseClass }):
// CHECK:   [[PROJ:%.*]] = project_box %{{.*}} : ${ var BaseClass }, 0
// CHECK-NOT: begin_access
// CHECK:   load_borrow [[PROJ]] : $*BaseClass
// CHECK-LABEL: } // end sil function '$s20access_marker_verify7RefEnumO8getValueAA9BaseClassCvg'

// --- indirect enum pattern.
func testEnumPattern(ie: IndirectEnum) -> Bool {
  guard case .V(let kind) = ie else {
    return false
  }
  _ = kind
  return true
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify15testEnumPattern2ieSbAA08IndirectE0O_tF : $@convention(thin) (@guaranteed IndirectEnum) -> Bool {
// CHECK: bb0(%0 : @guaranteed $IndirectEnum):
// CHECK:   switch_enum %{{.*}} : $IndirectEnum, case #IndirectEnum.V!enumelt: [[BBV:bb.*]], default bb
// CHECK: [[BBV]](%{{.*}} : @owned ${ var Int }):
// CHECK:   [[PROJ:%.*]] = project_box
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unsafe] [[PROJ]]
// CHECK:   load [trivial] [[ACCESS]] : $*Int
// CHECK-LABEL: } // end sil function '$s20access_marker_verify15testEnumPattern2ieSbAA08IndirectE0O_tF'

// --- enum LValue.
struct StructOfEnum {
  var e: E
  var f: E
}
func enumLValueHelper(_: inout E, _: inout E) {}

// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify14testEnumLValue1syAA08StructOfE0Vz_tF : $@convention(thin) (@inout StructOfEnum) -> () {
// CHECK: bb0(%0 : $*StructOfEnum):
// CHECK:   begin_access [modify] [unknown] %0 : $*StructOfEnum
// CHECK:   struct_element_addr %2 : $*StructOfEnum, #StructOfEnum.e
// CHECK:   begin_access [modify] [unknown] %0 : $*StructOfEnum
// CHECK:   struct_element_addr %4 : $*StructOfEnum, #StructOfEnum.f
// CHECK:   function_ref @$s20access_marker_verify16enumLValueHelperyyAA1EOz_ADztF : $@convention(thin) (@inout E, @inout E) -> ()
// CHECK:   apply %6(%3, %5) : $@convention(thin) (@inout E, @inout E) -> ()
// CHECK:   end_access %4 : $*StructOfEnum
// CHECK:   end_access %2 : $*StructOfEnum
// CHECK:   %10 = tuple ()
// CHECK:   return %10 : $()
// CHECK-LABEL: } // end sil function '$s20access_marker_verify14testEnumLValue1syAA08StructOfE0Vz_tF'
func testEnumLValue(s: inout StructOfEnum) {
  enumLValueHelper(&s.e, &s.f)
}

struct MyDict<K: Hashable, V> {
  let s = "" // make this non-trivial
  subscript(key: K) -> V? {
    get { return nil }
    set {}
  }
}

// --- Optional access.
func accessOptionalArray(_ dict : MyDict<Int, [Int]>) {
  var dict = dict
  dict[1]?.append(2)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify0A13OptionalArrayyyAA6MyDictVySiSaySiGGF : $@convention(thin) (@guaranteed MyDict<Int, Array<Int>>) -> () {
// CHECK: bb0(%0 : @guaranteed $MyDict<Int, Array<Int>>):
// CHECK:   alloc_box ${ var MyDict<Int, Array<Int>> }, var, name "dict"
// CHECK:   [[PROJ:%.*]] = project_box
// ----- initialize the box.
// CHECK:   [[INITACCESS:%.*]] = begin_access [modify] [unsafe] [[PROJ]]
// CHECK:   store %{{.*}} to [init] [[INITACCESS]]
// CHECK:   end_access [[INITACCESS]]
// ----- begin formal access for MyDict.subscript.setter
// CHECK:   [[BOXACCESS:%.*]] = begin_access [modify] [unknown] [[PROJ]]
// CHECK:   [[TEMP:%.*]] = alloc_stack $Optional<Array<Int>>
// CHECK:   load_borrow [[BOXACCESS]] : $*MyDict<Int, Array<Int>>
// ----- Initialize some trivial temporaries.
// CHECK:   alloc_stack $Int
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [trivial]
// ----- Call MyDict.subscript.getter.
// CHECK-NOT: begin_access
// CHECK:   apply %{{.*}}<Int, [Int]>
// ----- access the temporary array result of the getter
// CHECK:   [[TEMPACCESS:%.*]] = begin_access [modify] [unsafe] [[TEMP]]
// CHECK:   [[HAS_VALUE:%.*]] = select_enum_addr [[TEMPACCESS]]
// CHECK:   cond_br [[HAS_VALUE]], [[TRUEBB:bb[0-9]+]], [[FALSEBB:bb[0-9]+]]
//
// CHECK: [[TRUEBB]]:
// CHECK-NOT: begin_access
// CHECK:   [[TEMPARRAYADR:%.*]] = unchecked_take_enum_data_addr [[TEMPACCESS]] : $*Optional<Array<Int>>, #Optional.some!enumelt
// ----- call Array.append
// CHECK:   alloc_stack $Int
// CHECK:   store %{{.*}} to [trivial]
// CHECK:   function_ref @$sSa6appendyyxnF : $@convention(method) <τ_0_0> (@in τ_0_0, @inout Array<τ_0_0>) -> ()
// CHECK:   apply %{{.*}}<Int>(%{{.*}}, [[TEMPARRAYADR]]) : $@convention(method) <τ_0_0> (@in τ_0_0, @inout Array<τ_0_0>) -> ()
// CHECK:   [[TEMPARRAYVAL:%.*]] = load [take] [[TEMPACCESS]] : $*Optional<Array<Int>>
// CHECK:   [[ARRAYCOPY:%.*]] = alloc_stack $Optional<Array<Int>>
// CHECK:   store [[TEMPARRAYVAL]] to [init] [[ARRAYCOPY]] : $*Optional<Array<Int>>
// CHECK:   alloc_stack $Int
// CHECK:   store %{{.*}} to [trivial]
// ----- call MyDict.subscript.setter
// CHECK: apply %{{.*}}<Int, [Int]>([[ARRAYCOPY]], %{{.*}}, [[BOXACCESS]]) : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@in Optional<τ_0_1>, @in τ_0_0, @inout MyDict<τ_0_0, τ_0_1>) -> ()
// CHECK:   br [[RETBB:bb[0-9]+]]
//
// CHECK: [[FALSEBB]]:
// CHECK:   [[TEMPARRAY:%.*]] = load [copy] [[TEMPACCESS]]
// CHECK:   [[WRITEBACK:%.*]] = alloc_stack $Optional<Array<Int>>
// CHECK-NOT: begin_access
// CHECK:   store [[TEMPARRAY]] to [init] [[WRITEBACK]]
// CHECK:   [[TEMP3:%.*]] = alloc_stack $Int
// CHECK-NOT: begin_access
// CHECK:   store %{{.*}} to [trivial] [[TEMP3]] : $*Int
// Call MyDict.subscript.setter
// CHECK:   apply %{{.*}}<Int, [Int]>([[WRITEBACK]], [[TEMP3]], [[BOXACCESS]]) : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 : Hashable> (@in Optional<τ_0_1>, @in τ_0_0, @inout MyDict<τ_0_0, τ_0_1>) -> ()
// CHECK:   end_access [[TEMPACCESS]] : $*Optional<Array<Int>>
// CHECK:   end_access [[BOXACCESS]] : $*MyDict<Int, Array<Int>>
// CHECK:   br [[RETBB]]
//
// CHECK-LABEL: } // end sil function '$s20access_marker_verify0A13OptionalArrayyyAA6MyDictVySiSaySiGGF'

// --- Optional map.
enum OptionalWithMap<Wrapped> {
  case none

  case some(Wrapped)

  init(_ some: Wrapped) { self = .some(some) }

  func map<U>(
    _ transform: (Wrapped) throws -> U
  ) rethrows -> U? {
    switch self {
    case .some(let y):
      return .some(try transform(y))
    case .none:
      return .none
    }
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify15OptionalWithMapO3mapyqd__Sgqd__xKXEKlF :
// CHECK: [[STK:%.]] = alloc_stack $OptionalWithMap<Wrapped>
// CHECK-NOT: begin_access
// CHECK: copy_addr %2 to [init] [[STK]] : $*OptionalWithMap<Wrapped>
// CHECK: switch_enum_addr [[STK]] : $*OptionalWithMap<Wrapped>, case #OptionalWithMap.some!enumelt: [[BBSOME:bb.*]], case #OptionalWithMap.none!enumelt: bb
//
// CHECK: [[BBSOME]]:
// CHECK-NOT: begin_access
// CHECK: [[ADR:%.*]] = unchecked_take_enum_data_addr [[STK]]
// CHECK: alloc_stack [lexical] [var_decl] $Wrapped, let, name "y"
// CHECK-NOT: begin_access
// CHECK: copy_addr [take] [[ADR]] to [init]
// ----- call transform.
// CHECK: try_apply
// CHECK-LABEL: } // end sil function '$s20access_marker_verify15OptionalWithMapO3mapyqd__Sgqd__xKXEKlF'

// --- delegating initializer.
struct DelegatingInit {
  var i: Int
  init(i: Int) {
    self.i = i
  }
  init() {
    self.init(i: 4)
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify14DelegatingInitV1iACSi_tcfC : $@convention(method) (Int, @thin DelegatingInit.Type) -> DelegatingInit {
// CHECK: bb0(%0 : $Int, %1 : $@thin DelegatingInit.Type):
// CHECK:   alloc_box ${ var DelegatingInit }, var, name "self"
// CHECK:   mark_uninitialized [rootself] %2 : ${ var DelegatingInit }
// CHECK:   [[BOX:%.*]] = project_box
// CHECK:   begin_access [modify] [unknown]
// CHECK:   struct_element_addr
// CHECK:   assign
// CHECK:   end_access
// CHECK-NOT: begin_access
// CHECK:   load [trivial] [[BOX]] : $*DelegatingInit
// CHECK:   destroy_value
// CHECK:   return {{%[^,]+}} : $DelegatingInit
// CHECK-LABEL: } // end sil function '$s20access_marker_verify14DelegatingInitV1iACSi_tcfC'

// --- addressor.
func testAddressor(p: UnsafePointer<Int>) -> Int {
  return p.pointee
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify13testAddressor1pSiSPySiG_tF : $@convention(thin) (UnsafePointer<Int>) -> Int {
// CHECK: bb0(%0 : $UnsafePointer<Int>):
// CHECK:   apply
// CHECK:   struct_extract
// CHECK:   [[ADR:%.*]] = pointer_to_address
// CHECK:   [[ACCESS:%.*]] = begin_access [read] [unsafe] [[ADR]] : $*Int
// CHECK:   load [trivial] [[ACCESS]] : $*Int
// CHECK:   return
// CHECK-LABEL: } // end sil function '$s20access_marker_verify13testAddressor1pSiSPySiG_tF'

// --- shims.
func testShims() -> UInt32 {
  return _SwiftKeyPathBufferHeader_SizeMask
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify9testShimss6UInt32VyF : $@convention(thin) () -> UInt32 {
// CHECK: bb0:
// CHECK:   [[GA:%.*]] = global_addr @_SwiftKeyPathBufferHeader_SizeMask : $*UInt32
// CHECK-NOT: begin_access
// CHECK:   load [trivial] [[GA]] : $*UInt32
// CHECK:   return
// CHECK-LABEL: } // end sil function '$s20access_marker_verify9testShimss6UInt32VyF'

// --- global variable initialization.
var globalString1 = "⓪" // start non-empty
// CHECK-LABEL: sil private [global_init_once_fn] [ossa] @{{.*}}WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK: alloc_global @$s20access_marker_verify13globalString1SSvp
// CHECK: [[GA:%.*]] = global_addr @$s20access_marker_verify13globalString1SSvp : $*String
// CHECK: apply
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[GA]] : $*String
// CHECK: store %{{.*}} to [init] [[ACCESS]] : $*String
// CHECK: end_access
// CHECK-LABEL: } // end sil function '{{.*}}WZ'

var globalString2 = globalString1
// CHECK-LABEL: sil private [global_init_once_fn] [ossa] @{{.*}}WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK: alloc_global @$s20access_marker_verify13globalString2SSvp
// CHECK: [[GA:%.*]] = global_addr @$s20access_marker_verify13globalString2SSvp : $*String
// CHECK: apply
// CHECK: [[PTR:%.*]] = pointer_to_address
// CHECK: [[ACCESS:%.*]] = begin_access [read] [dynamic] [[PTR]] : $*String
// CHECK: [[INIT:%.*]] = begin_access [modify] [unsafe] [[GA]] : $*String
// CHECK: copy_addr [[ACCESS]] to [init] [[INIT]] : $*String
// CHECK: end_access [[INIT]] : $*String
// CHECK: end_access [[ACCESS]] : $*String
// CHECK-NOT: end_access
// CHECK-LABEL: } // end sil function '{{.*}}WZ'


// --- getter.
struct GenericStructWithGetter<T> {
  var t: T
  var val: Int

  struct Value {
    internal var val: Int
    
    internal init(_ val: Int) { self.val = val }
  }
  var value : Value {
    get { return Value(val) }
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify23GenericStructWithGetterV5valueAC5ValueVyx_Gvg : $@convention(method) <T> (@in_guaranteed GenericStructWithGetter<T>) -> GenericStructWithGetter<T>.Value {
// CHECK: bb0(%0 : $*GenericStructWithGetter<T>):
// CHECK:   [[ADR:%.*]] = struct_element_addr %0 : $*GenericStructWithGetter<T>, #GenericStructWithGetter.val
// CHECK-NOT: begin_access
// CHECK:   load [trivial] [[ADR]] : $*Int
// CHECK:   apply
// CHECK-LABEL: } // end sil function '$s20access_marker_verify23GenericStructWithGetterV5valueAC5ValueVyx_Gvg'

// --- setter.
struct StructWithSetter {
  var _val: Int

  internal var val: Int {
    get {
      return _val
    }
    set {
      _val = newValue
    }
  }

  mutating func inc(incVal: Int) {
    val += incVal
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify16StructWithSetterV3inc0G3ValySi_tF : $@convention(method) (Int, @inout StructWithSetter) -> () {
// CHECK: bb0(%0 : $Int, %1 : $*StructWithSetter):
// CHECK: [[FORMALACCESS:%.*]] = begin_access [modify] [unknown] %1
// CHECK: alloc_stack $Int
// CHECK: load [trivial] [[FORMALACCESS]] : $*StructWithSetter
// CHECK: [[GETTER:%.*]] = function_ref @$s20access_marker_verify16StructWithSetterV3valSivg
// CHECK: apply [[GETTER]]
// CHECK: begin_access [modify] [unsafe]
// CHECK: store %{{.*}} to [trivial]
// CHECK: end_access
// CHECK: begin_access [modify] [unsafe]
// CHECK: [[INC:%.*]] = function_ref @$sSi2peoiyySiz_SitFZ
// CHECK: apply [[INC]]
// CHECK: load [trivial] %13 : $*Int
// CHECK: [[SETTER:%.*]] = function_ref @$s20access_marker_verify16StructWithSetterV3valSivs
// CHECK: apply [[SETTER]]
// CHECK: end_access
// CHECK: end_access [[FORMALACCESS]]
// CHECK-LABEL: } // end sil function '$s20access_marker_verify16StructWithSetterV3inc0G3ValySi_tF'

// --- lazy inout.
func increment(_ x: inout Int) { x += 1 }

final class LazyFinalClassProperty {
  lazy var cat: Int = 5
}

func inoutWriteOfLazyFinalClassProperty(l: inout LazyFinalClassProperty) {
  increment(&l.cat)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify34inoutWriteOfLazyFinalClassProperty1lyAA0ghiJ0Cz_tF : $@convention(thin) (@inout LazyFinalClassProperty) -> () {
// CHECK: bb0(%0 : $*LazyFinalClassProperty):
// CHECK:   [[FORMALACCESS:%.*]] = begin_access [read] [unknown] %0 : $*LazyFinalClassProperty
// CHECK:   load [copy] [[FORMALACCESS]] : $*LazyFinalClassProperty
// CHECK:   end_access [[FORMALACCESS]] : $*LazyFinalClassProperty
// CHECK:   // function_ref LazyFinalClassProperty.cat.getter
// CHECK:   [[GETTER:%.*]] = function_ref @$s20access_marker_verify22LazyFinalClassPropertyC3catSivg
// CHECK:   apply [[GETTER]]
// CHECK:   begin_access [modify] [unsafe]
// CHECK:   store %{{.*}} to [trivial]
// CHECK:   end_access
// CHECK:   [[TEMPACCESS:%.*]] = begin_access [modify] [unsafe] %5 : $*Int
// CHECK:   [[INC:%.*]] = function_ref @$s20access_marker_verify9incrementyySizF : $@convention(thin) (@inout Int) -> ()
// CHECK:   apply [[INC]]([[TEMPACCESS]]) : $@convention(thin) (@inout Int) -> ()
// CHECK:   load [trivial] [[TEMPACCESS]] : $*Int
// CHECK:   [[SETTER:%.*]] = function_ref @$s20access_marker_verify22LazyFinalClassPropertyC3catSivs
// CHECK:   apply [[SETTER]]
// CHECK:   end_access [[TEMPACCESS]] : $*Int
// CHECK-LABEL: } // end sil function '$s20access_marker_verify34inoutWriteOfLazyFinalClassProperty1lyAA0ghiJ0Cz_tF'

// --- lazy getter.
func inoutAccessOfLazyFinalClassProperty(
  l: inout LazyFinalClassProperty
) -> Int {
  return l.cat
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify35inoutAccessOfLazyFinalClassProperty1lSiAA0ghiJ0Cz_tF : $@convention(thin) (@inout LazyFinalClassProperty) -> Int {
// CHECK: bb0(%0 : $*LazyFinalClassProperty):
// CHECK:   begin_access [read] [unknown] %0
// CHECK:   load [copy]
// CHECK:   end_access
// CHECK:   [[GETTER:%.*]] = function_ref @$s20access_marker_verify22LazyFinalClassPropertyC3catSivg : $@convention(method) (@guaranteed LazyFinalClassProperty) -> Int
// CHECK:   apply [[GETTER]]
// CHECK-LABEL: } // end sil function '$s20access_marker_verify35inoutAccessOfLazyFinalClassProperty1lSiAA0ghiJ0Cz_tF'

// --- polymorphic getter
protocol Abstractable {
  associatedtype Result
  var storedFunction: () -> Result { get set }
}

class C : Abstractable {
  var storedFunction: () -> Int = { 0 }
}
// CHECK-LABEL: sil private [transparent] [thunk] [ossa] @$s20access_marker_verify1CCAA12AbstractableA2aDP14storedFunction6ResultQzycvMTW :
// CHECK:      bb0(%0 : $*C):
// CHECK-NEXT:   [[SELF:%.*]] = load_borrow %0 : $*C
// CHECK-NEXT:   [[MODIFY:%.*]] = class_method [[SELF]] : $C, #C.storedFunction!modify
// CHECK-NEXT:   ([[ADDR:%.*]], [[TOKEN:%.*]]) = begin_apply [[MODIFY]]([[SELF]])
// CHECK-NEXT:   [[TEMP:%.*]] = alloc_stack
// CHECK-NEXT:   [[OLD_FN:%.*]] = load [take] [[ADDR]]
// CHECK-NEXT:   // function_ref thunk
// CHECK-NEXT:   [[THUNK:%.*]] = function_ref @$sSiIegd_SiIegr_TR
// CHECK-NEXT:   [[THUNKED_OLD_FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[OLD_FN]])
// CHECK-NEXT:   [[CONVERTED_OLD_FN:%.*]] = convert_function [[THUNKED_OLD_FN]]
// CHECK-NEXT:   [[TEMP_ACCESS:%.*]] = begin_access [modify] [unsafe] [[TEMP]] :
// CHECK-NEXT:   store [[CONVERTED_OLD_FN]] to [init] [[TEMP_ACCESS]] :
// CHECK-NEXT:   end_access [[TEMP_ACCESS]]
// CHECK-NEXT:   yield [[TEMP]] : {{.*}}, resume bb1, unwind bb2

// CHECK:      bb1:
// CHECK-NEXT:   [[NEW_FN:%.*]] = load [take] [[TEMP]]
// CHECK-NEXT:   [[NEW_FN_CONV:%.*]] = convert_function [[NEW_FN]]
// CHECK-NEXT:   // function_ref thunk
// CHECK-NEXT:   [[THUNK:%.*]] = function_ref @$sSiIegr_SiIegd_TR
// CHECK-NEXT:   [[THUNKED_NEW_FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[NEW_FN_CONV]])
// CHECK-NEXT:   store [[THUNKED_NEW_FN]] to [init] [[ADDR]] :
// CHECK-NEXT:   dealloc_stack [[TEMP]]
// CHECK-NEXT:   end_apply [[TOKEN]]
// CHECK-NEXT:   [[TUPLE:%.*]] = tuple ()
// CHECK-NEXT:   end_borrow [[SELF]] : $C
// CHECK-NEXT:   return [[TUPLE]]

// CHECK:      bb2:
// CHECK-NEXT:   [[NEW_FN:%.*]] = load [take] [[TEMP]]
// CHECK-NEXT:   [[NEW_FN_CONV:%.*]] = convert_function [[NEW_FN]]
// CHECK-NEXT:   // function_ref thunk
// CHECK-NEXT:   [[THUNK:%.*]] = function_ref @$sSiIegr_SiIegd_TR
// CHECK-NEXT:   [[THUNKED_NEW_FN:%.*]] = partial_apply [callee_guaranteed] [[THUNK]]([[NEW_FN_CONV]])
// CHECK-NEXT:   store [[THUNKED_NEW_FN]] to [init] [[ADDR]] :
// CHECK-NEXT:   dealloc_stack [[TEMP]]
// CHECK-NEXT:   abort_apply [[TOKEN]]
// CHECK-NEXT:   end_borrow [[SELF]] : $C
// CHECK-NEXT:   unwind

// --- writeback address-only.
var addressOnly: P {
  get {
    return StructP()
  }
  set {}
}

func takesInoutP(x: inout P) {}

func testWriteback() {
  takesInoutP(x: &addressOnly)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify13testWritebackyyF : $@convention(thin) () -> () {
// CHECK: bb0:
// CHECK:   %0 = alloc_stack $any P
// CHECK: [[GETTER:%.*]] = apply
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unsafe] %0 : $*any P
// Call takesInoutP
// CHECK: apply %{{.*}}([[ACCESS]]) : $@convention(thin) (@inout any P) -> ()
// Call addressOnly.setter
// CHECK: apply %{{.*}}([[ACCESS]]) : $@convention(thin) (@in any P) -> ()
// CHECK: end_access [[ACCESS]] : $*any P
// CHECK-LABEL: } // end sil function '$s20access_marker_verify13testWritebackyyF'

// --- writeback temp.
struct MutableStorage {
  mutating func push() {}
}

class Container {
  var storage: MutableStorage

  init() {
    storage = MutableStorage()
  }

  func testWritebackTemp() {
    self.storage.push()
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify9ContainerC17testWritebackTempyyF : $@convention(method) (@guaranteed Container) -> () {
// CHECK: bb0(%0 : @guaranteed $Container):
// call storage.materializeForSet
// CHECK: [[MODIFY:%.*]] = class_method %0 : $Container, #Container.storage!modify
// CHECK: begin_apply [[MODIFY]]
// call MutableStorage.push()
// CHECK: apply %{{.*}}(%{{.*}}) : $@convention(method) (@inout MutableStorage) -> ()
// CHECK: end_apply
// CHECK-LABEL: } // end sil function '$s20access_marker_verify9ContainerC17testWritebackTempyyF'

// --- return mixed tuple
protocol HasClassGetter {
  var c: BaseClass { get }
}
func testMixedTuple(p: HasClassGetter) -> (BaseClass, Any) {
  return (p.c, p.c)
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify14testMixedTuple1pAA9BaseClassC_yptAA03HasH6Getter_p_tF : $@convention(thin) (@in_guaranteed any HasClassGetter) -> (@owned BaseClass, @out Any) {
// CHECK: bb0(%0 : $*Any, %1 : $*any HasClassGetter):
// CHECK: [[P1:%.*]] = open_existential_addr immutable_access %1 : $*any HasClassGetter to $*@opened
// CHECK: [[OUTC:%.*]] = apply %{{.*}}<@opened("{{.*}}", any HasClassGetter) Self>([[P1]])
// CHECK: [[P2:%.*]] = open_existential_addr immutable_access %1 : $*any HasClassGetter to $*@opened
// CHECK: [[R2:%.*]] = apply %{{.*}}<@opened("{{.*}}", any HasClassGetter) Self>([[P2]])
// CHECK: [[OUTANY:%.*]] = init_existential_addr %0 : $*Any, $BaseClass
// CHECK: store [[R2]] to [init] [[OUTANY]] : $*BaseClass
// CHECK: return [[OUTC]] : $BaseClass
// CHECK-LABEL: } // end sil function '$s20access_marker_verify14testMixedTuple1pAA9BaseClassC_yptAA03HasH6Getter_p_tF'

// --- existential cast.
internal protocol CanCast {
  func unbox<T : Hashable>() -> T?
}

internal struct CanCastStruct<Base : Hashable> : CanCast {
  internal var base: Base

  internal func unbox<T : Hashable>() -> T? {
    return (self as CanCast as? CanCastStruct<T>)?.base
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify13CanCastStructV5unboxqd__SgySHRd__lF : $@convention(method) <Base where Base : Hashable><T where T : Hashable> (@in_guaranteed CanCastStruct<Base>) -> @out Optional<T> {
// CHECK: bb0(%0 : $*Optional<T>, %1 : $*CanCastStruct<Base>):
// CHECK: [[OUT_ENUM:%.*3]] = init_enum_data_addr %0 : $*Optional<T>, #Optional.some!enumelt
// CHECK: [[TEMP_SUB:%.*]] = alloc_stack $Optional<CanCastStruct<T>>
// CHECK: [[TEMP_BASE:%.*]] = alloc_stack $any CanCast
// CHECK: [[TEMP_BASE_ADR:%.*]] = init_existential_addr [[TEMP_BASE]] : $*any CanCast, $CanCastStruct<Base>
// CHECK-NOT: begin_access
// CHECK: copy_addr %1 to [init] [[TEMP_BASE_ADR]] : $*CanCastStruct<Base>
// CHECK-NOT: begin_access
// CHECK: [[TEMP_SUB_ADR:%.*]] = init_enum_data_addr [[TEMP_SUB]] : $*Optional<CanCastStruct<T>>, #Optional.some!enumelt
// CHECK-NOT: begin_access
// CHECK: checked_cast_addr_br take_always any CanCast in [[TEMP_BASE]] : $*any CanCast to CanCastStruct<T> in [[TEMP_SUB_ADR]] : $*CanCastStruct<T>
// CHECK-NOT: begin_access
// CHECK: [[TEMP_DATA:%.*]] = unchecked_take_enum_data_addr [[TEMP_SUB]] : $*Optional<CanCastStruct<T>>, #Optional.some!enumelt
// CHECK-NOT: begin_access
// CHECK: [[BASE_ADR:%.*]] = struct_element_addr [[TEMP_DATA]] : $*CanCastStruct<T>, #CanCastStruct.base
// CHECK: copy_addr [[BASE_ADR]] to [init] [[OUT_ENUM]] : $*T
// CHECK-NOT: begin_access
// CHECK: inject_enum_addr %0 : $*Optional<T>, #Optional.some!enumelt
// CHECK-LABEL: } // end sil function '$s20access_marker_verify13CanCastStructV5unboxqd__SgySHRd__lF'

// --- open existential
protocol Q : PBar {}

func testOpenExistential(p: PBar) {
  let q0 = p as? Q
  if q0 != nil, let q = q0 {
    q.bar()
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify19testOpenExistential1pyAA4PBar_p_tF : $@convention(thin) (@in_guaranteed any PBar) -> () {
// CHECK: bb0(%0 : $*any PBar):
// CHECK: [[Q0:%.*]] = alloc_stack [lexical] [var_decl] $Optional<any Q>, let, name "q0"
// CHECK: [[PBAR:%.*]] = alloc_stack $any PBar
// CHECK-NOT: begin_access
// CHECK: copy_addr %0 to [init] [[PBAR]] : $*any PBar
// CHECK-NOT: begin_access
// CHECK: [[Q0_DATA:%.*]] = init_enum_data_addr [[Q0]] : $*Optional<any Q>, #Optional.some!enumelt
// CHECK-NOT: begin_access
// CHECK: checked_cast_addr_br take_always any PBar in [[PBAR]] : $*any PBar to any Q in [[Q0_DATA]] : $*any Q, bb1, bb2
// CHECK-NOT: begin_access
// CHECK: inject_enum_addr [[Q0]] : $*Optional<any Q>, #Optional.some!enumelt
// CHECK-NOT: begin_access
// CHECK: apply %{{.*}}<any Q>([[Q0]], {{.*}}) : $@convention(method) <τ_0_0 where τ_0_0 : ~Copyable> (@in_guaranteed Optional<τ_0_0>, _OptionalNilComparisonType, @thin Optional<τ_0_0>.Type) -> Bool
// CHECK: [[Q:%.*]] = alloc_stack [lexical] [var_decl] $any Q, let, name "q"
// CHECK: [[OPT_Q:%.*]] = alloc_stack $Optional<any Q>
// CHECK-NOT: begin_access
// CHECK: copy_addr [[Q0]] to [init] [[OPT_Q]] : $*Optional<any Q>
// CHECK-NOT: begin_access
// CHECK: switch_enum_addr [[OPT_Q]] : $*Optional<any Q>, case #Optional.some!enumelt: bb
// CHECK-NOT: begin_access
// CHECK: [[OPT_Q_ADR:%.*]] = unchecked_take_enum_data_addr [[OPT_Q]] : $*Optional<any Q>, #Optional.some!enumelt
// CHECK-NOT: begin_access
// CHECK: copy_addr [take] [[OPT_Q_ADR]] to [init] [[Q]] : $*any Q
// CHECK-NOT: begin_access
// CHECK: [[Q_ADR:%.*]] = open_existential_addr immutable_access [[Q]] : $*any Q to $*@opened("{{.*}}", any Q) Self
// CHECK: witness_method $@opened("{{.*}}", any Q) Self, #PBar.bar
// CHECK: apply %{{.*}}<@opened("{{.*}}", any Q) Self>([[Q_ADR]])
// CHECK-LABEL: } // end sil function '$s20access_marker_verify19testOpenExistential1pyAA4PBar_p_tF'

// --- local existential
func getP() -> P {
  struct S : P {}
  return S()
}

func testLocalExistential() {
  var p = getP()
  takesClosure { p = getP() }
  _ = p
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify20testLocalExistentialyyF : $@convention(thin) () -> () {
// CHECK: alloc_box ${ var any P }, var, name "p"
// CHECK: [[PROJ:%.*]] = project_box %{{.*}} : ${ var any P }, 0
// CHECK-NOT: begin_access
// CHECK: apply %{{.*}}([[PROJ]]) : $@convention(thin) () -> @out any P
// CHECK-NOT: begin_access
// CHECK: partial_apply [callee_guaranteed] %{{.*}}([[PROJ]]) : $@convention(thin) (@inout_aliasable any P) -> ()
// CHECK-NOT: begin_access
// CHECK: apply
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[PROJ]] : $*any P
// CHECK: [[COPY:%.*]] = alloc_stack $any P
// CHECK-NOT: begin_access
// CHECK: copy_addr [[ACCESS]] to [init] [[COPY]] : $*any P
// CHECK: end_access
// CHECK-NOT: begin_access
// CHECK-LABEL: } // end sil function '$s20access_marker_verify20testLocalExistentialyyF'

// --- address-only argument.
protocol UsesSelf {
  func bar(_: Self)
}

extension UsesSelf {
  static func testSelf(a: Self, b: Self) {
    a.bar(b)
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify8UsesSelfPAAE04testE01a1byx_xtFZ : $@convention(method) <Self where Self : UsesSelf> (@in_guaranteed Self, @in_guaranteed Self, @thick Self.Type) -> () {
// CHECK: bb0(%0 : $*Self, %1 : $*Self, %2 : $@thick Self.Type):
// CHECK: apply %{{.*}}<Self>(%1, %0) : $@convention(witness_method: UsesSelf) <τ_0_0 where τ_0_0 : UsesSelf> (@in_guaranteed τ_0_0, @in_guaranteed τ_0_0) -> ()
// CHECK-LABEL: } // end sil function '$s20access_marker_verify8UsesSelfPAAE04testE01a1byx_xtFZ'

// --- autoclosure
struct StructWithLayout {
  internal init() {
    assert(MemoryLayout.size(ofValue: self) >= 0)
  }
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify16StructWithLayoutVACycfC : $@convention(method) (@thin StructWithLayout.Type) -> StructWithLayout {
// CHECK: bb0(%0 : $@thin StructWithLayout.Type):
// CHECK: alloc_box ${ var StructWithLayout }, var, name "self"
// CHECK: mark_uninitialized [rootself] %{{.*}} : ${ var StructWithLayout }
// CHECK: [[PROJ:%.*]] = project_box %{{.*}} : ${ var StructWithLayout }, 0
// CHECK: [[PA:%.*]] = partial_apply [callee_guaranteed] %{{.*}}([[PROJ]]) : $@convention(thin) (@inout_aliasable StructWithLayout) -> Bool
// CHECK: [[CLOSURE:%.*]] = convert_escape_to_noescape [not_guaranteed] [[PA]] : $@callee_guaranteed () -> Bool to $@noescape @callee_guaranteed () -> Bool
// call StaticString.init
// CHECK: apply
// call UInt.init(_builtinIntegerLiteral:)
// CHECK: apply
// call default argument
// call _sanityCheck(_:_:file:line:)
// CHECK: apply %{{.*}}([[CLOSURE]], {{.*}})
// CHECK: load [trivial] [[PROJ]] : $*StructWithLayout
// CHECK-LABEL: } // end sil function '$s20access_marker_verify16StructWithLayoutVACycfC'

// --- pointer_to_address
// Verification should ignore this case.
func testPointerInit(x: Int, y: UnsafeMutablePointer<Int>) {
  y.pointee = x
}
// CHECK-LABEL: sil hidden [ossa] @$s20access_marker_verify15testPointerInit1x1yySi_SpySiGtF : $@convention(thin) (Int, UnsafeMutablePointer<Int>) -> () {
// CHECK: bb0(%0 : $Int, %1 : $UnsafeMutablePointer<Int>):
// call addressor
// CHECK: [[POINTEE:%.*]] = apply %{{.*}}<Int>(%1) : $@convention(method) <τ_0_0 where τ_0_0 : ~Copyable> (UnsafeMutablePointer<τ_0_0>) -> UnsafeMutablePointer<τ_0_0>
// CHECK: [[RAWPTR:%.*]] = struct_extract [[POINTEE]] : $UnsafeMutablePointer<Int>, #UnsafeMutablePointer._rawValue
// CHECK: [[ADR:%.*]] = pointer_to_address [[RAWPTR]] : $Builtin.RawPointer to [strict] $*Int
// CHECK: [[ACCESS:%.*]] = begin_access [modify] [unsafe] [[ADR]] : $*Int
// CHECK: assign %0 to [[ACCESS]] : $*Int
// CHECK-LABEL: } // end sil function '$s20access_marker_verify15testPointerInit1x1yySi_SpySiGtF'

// Verification should ignore the address operand of init_existential_addr.
class testInitExistentialGlobal {
  static var testProperty: P = StructP()
}
// CHECK-LABEL: sil private [global_init_once_fn] [ossa] @{{.*}}WZ : $@convention(c) (Builtin.RawPointer) -> () {
// CHECK:   alloc_global @$s20access_marker_verify25testInitExistentialGlobalC0D8PropertyAA1P_pvpZ
// CHECK:   [[GADR:%.*]] = global_addr @$s20access_marker_verify25testInitExistentialGlobalC0D8PropertyAA1P_pvpZ : $*any P
// CHECK:   %{{.*}} = apply %{{.*}}({{.*}}) : $@convention(method) (@thin StructP.Type) -> StructP
// CHECK:   [[EADR:%.*]] = init_existential_addr [[GADR]] : $*any P, $StructP
// CHECK:   store %{{.*}} to [trivial] [[EADR]] : $*StructP
// CHECK-LABEL: } // end sil function '{{.*}}WZ

public enum SomeError: Swift.Error {
    case error
}

// Verification should ignore addresses produced by project_existential_box.
public func testInitBox() throws {
    throw SomeError.error
}
// CHECK-LABEL: sil [ossa] @$s20access_marker_verify11testInitBoxyyKF : $@convention(thin) () -> @error any Error {
// CHECK: [[BOXALLOC:%.*]] = alloc_existential_box $any Error, $SomeError
// CHECK: [[PROJ:%.*]] = project_existential_box $SomeError in [[BOXALLOC]] : $any Error
// CHECK: store [[BOXALLOC]] to [init] [[BOXBUF:%.*]] :
// CHECK: store %{{.*}} to [trivial] [[PROJ]] : $*SomeError
// CHECK: [[BOXALLOC2:%.*]] = load [take] [[BOXBUF]]
// CHECK: throw [[BOXALLOC2]] : $any Error
// CHECK-LABEL: } // end sil function '$s20access_marker_verify11testInitBoxyyKF'

public final class HasStaticProp {
  public static let empty: HasStaticProp = HasStaticProp()
}

// A global addressor produces an unenforced RawPointer. This looks
// like an Unidentified access with no access marker. Ensure that
// verification doesn't assert.
public func getStaticProp() -> HasStaticProp {
  return .empty
}

// CHECK-LABEL: sil [ossa] @$s20access_marker_verify13getStaticPropAA03HaseF0CyF : $@convention(thin) () -> @owned HasStaticProp {
// function_ref HasStaticProp.empty.unsafeMutableAddressor
// CHECK: [[F:%.*]] = function_ref @$s20access_marker_verify13HasStaticPropC5emptyACvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK: [[RP:%.*]] = apply [[F]]() : $@convention(thin) () -> Builtin.RawPointer
// CHECK: [[ADR:%.*]] = pointer_to_address [[RP]] : $Builtin.RawPointer to [strict] $*HasStaticProp
// CHECK: load [copy] [[ADR]] : $*HasStaticProp
// CHECK-LABEL: } // end sil function '$s20access_marker_verify13getStaticPropAA03HaseF0CyF'

// ===-----------------------------------------------------------------------===
// Test computeMemBehavior with a non-address value. In this case, the
// address for an access of StreamClass.buffer is compared with the
// ContiguousArrayStorageBase value.
//
// <rdar://60046018> assert: (v->getType().isAddress()) in getAccessedAddress.

public final class StreamClass {
    private var buffer: [UInt8]

    public init() {
        self.buffer = []
    }
}

// CHECK-LABEL: sil [ossa] @$s20access_marker_verify25testNonAddressMemBehavioryySiF : $@convention(thin) (Int) -> () {
// The relevant SIL instructions for this test don't appear until the string interpolation is inlined.
// Just make sure it does not assert.
// CHECK-LABEL: } // end sil function '$s20access_marker_verify25testNonAddressMemBehavioryySiF'
public func testNonAddressMemBehavior(_ N: Int) {
  let listOfStrings: [String] = (0..<10).map { "This is the number: \($0)!\n" }
  let stream = StreamClass()
}
