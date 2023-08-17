
// RUN: %target-swift-emit-silgen -parse-as-library %s | %FileCheck %s

enum Boolish {
  case falsy
  case truthy
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum13Boolish_casesyyF
func Boolish_cases() {
  // CHECK:       [[BOOLISH:%[0-9]+]] = metatype $@thin Boolish.Type
  // CHECK-NEXT:  [[FALSY:%[0-9]+]] = enum $Boolish, #Boolish.falsy!enumelt
  _ = Boolish.falsy

  // CHECK-NEXT:  [[BOOLISH:%[0-9]+]] = metatype $@thin Boolish.Type
  // CHECK-NEXT:  [[TRUTHY:%[0-9]+]] = enum $Boolish, #Boolish.truthy!enumelt
  _ = Boolish.truthy
}

enum Optionable {
  case nought
  case mere(Int)
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum16Optionable_casesyySiF
func Optionable_cases(_ x: Int) {

  // CHECK:       [[METATYPE:%.*]] = metatype $@thin Optionable.Type
  // CHECK:       [[FN:%.*]] = function_ref @$s4enum16Optionable_casesyySiFAA0B0OSicADmcfu_
  _ = Optionable.mere

  // CHECK:  [[METATYPE:%.*]] = metatype $@thin Optionable.Type
  // CHECK:  [[RES:%.*]] = enum $Optionable, #Optionable.mere!enumelt, %0 : $Int
  _ = Optionable.mere(x)
}

// CHECK-LABEL: sil private [ossa] @$s4enum16Optionable_casesyySiFAA0B0OSicADmcfu_ : $@convention(thin) (@thin Optionable.Type) -> @owned @callee_guaranteed (Int) -> Optionable
// CHECK-LABEL: sil private [ossa] @$s4enum16Optionable_casesyySiFAA0B0OSicADmcfu_ADSicfu0_ : $@convention(thin) (Int, @thin Optionable.Type) -> Optionable {

protocol P {}
struct S : P {}

enum AddressOnly {
  case nought
  case mere(P)
  case phantom(S)
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum17AddressOnly_casesyyAA1SVF
func AddressOnly_cases(_ s: S) {

  // CHECK:       [[METATYPE:%.*]] = metatype $@thin AddressOnly.Type
  // CHECK:       [[FN:%.*]] = function_ref @$s4enum17AddressOnly_casesyyAA1SVFAA0bC0OAA1P_pcAFmcfu_
  // CHECK-NEXT:  [[CTOR:%.*]] = apply [[FN]]([[METATYPE]])
  // CHECK-NEXT:  destroy_value [[CTOR]]
  _ = AddressOnly.mere

  // CHECK-NEXT:  [[METATYPE:%.*]] = metatype $@thin AddressOnly.Type
  // CHECK-NEXT:  [[NOUGHT:%.*]] = alloc_stack $AddressOnly
  // CHECK-NEXT:  inject_enum_addr [[NOUGHT]]
  // CHECK-NEXT:  destroy_addr [[NOUGHT]]
  // CHECK-NEXT:  dealloc_stack [[NOUGHT]]
  _ = AddressOnly.nought

  // CHECK-NEXT:  [[METATYPE:%.*]] = metatype $@thin AddressOnly.Type
  // CHECK-NEXT:  [[P_BUF:%.*]] = alloc_stack $any P
  // CHECK-NEXT:  [[PAYLOAD_ADDR:%.*]] = init_existential_addr [[P_BUF]]
  // CHECK-NEXT:  store %0 to [trivial] [[PAYLOAD_ADDR]]
  // CHECK-NEXT:  [[MERE:%.*]] = alloc_stack $AddressOnly
  // CHECK-NEXT:  [[PAYLOAD:%.*]] = init_enum_data_addr [[MERE]]
  // CHECK-NEXT:  copy_addr [take] [[P_BUF]] to [init] [[PAYLOAD]] : $*any P
  // CHECK-NEXT:  inject_enum_addr [[MERE]]
  // CHECK-NEXT:  destroy_addr [[MERE]]
  // CHECK-NEXT:  dealloc_stack [[MERE]]
  // CHECK-NEXT:  dealloc_stack [[P_BUF]] : $*any P
  _ = AddressOnly.mere(s)

  // Address-only enum vs loadable payload

  // CHECK-NEXT:  [[METATYPE:%.*]] = metatype $@thin AddressOnly.Type
  // CHECK-NEXT:  [[PHANTOM:%.*]] = alloc_stack $AddressOnly
  // CHECK-NEXT:  [[PAYLOAD:%.*]] = init_enum_data_addr [[PHANTOM]] : $*AddressOnly, #AddressOnly.phantom!enumelt
  // CHECK-NEXT:  store %0 to [trivial] [[PAYLOAD]]
  // CHECK-NEXT:  inject_enum_addr [[PHANTOM]] : $*AddressOnly, #AddressOnly.phantom!enumelt
  // CHECK-NEXT:  destroy_addr [[PHANTOM]]
  // CHECK-NEXT:  dealloc_stack [[PHANTOM]]

  _ = AddressOnly.phantom(s)
  // CHECK:       return
}

enum PolyOptionable<T> {
  case nought
  case mere(T)
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum20PolyOptionable_casesyyxlF
func PolyOptionable_cases<T>(_ t: T) {

// CHECK:         [[METATYPE:%.*]] = metatype $@thin PolyOptionable<T>.Type
// CHECK-NEXT:    [[NOUGHT:%.*]] = alloc_stack $PolyOptionable<T>
// CHECK-NEXT:    inject_enum_addr [[NOUGHT]]
// CHECK-NEXT:    destroy_addr [[NOUGHT]]
// CHECK-NEXT:    dealloc_stack [[NOUGHT]]
  _ = PolyOptionable<T>.nought

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin PolyOptionable<T>.Type
// CHECK-NEXT:    [[T_BUF:%.*]] = alloc_stack $T
// CHECK-NEXT:    copy_addr %0 to [init] [[T_BUF]]
// CHECK-NEXT:    [[MERE:%.*]] = alloc_stack $PolyOptionable<T>
// CHECK-NEXT:    [[PAYLOAD:%.*]] = init_enum_data_addr [[MERE]]
// CHECK-NEXT:    copy_addr [take] [[T_BUF]] to [init] [[PAYLOAD]] : $*T
// CHECK-NEXT:    inject_enum_addr [[MERE]]
// CHECK-NEXT:    destroy_addr [[MERE]]
// CHECK-NEXT:    dealloc_stack [[MERE]]
// CHECK-NEXT:    dealloc_stack [[T_BUF]] : $*T
  _ = PolyOptionable<T>.mere(t)

// CHECK-NOT:    destroy_addr %0
// CHECK:         return

}

// The substituted type is loadable and trivial here

// CHECK-LABEL: sil hidden [ossa] @$s4enum32PolyOptionable_specialized_casesyySiF
func PolyOptionable_specialized_cases(_ t: Int) {

// CHECK:         [[METATYPE:%.*]] = metatype $@thin PolyOptionable<Int>.Type
// CHECK-NEXT:    [[NOUGHT:%.*]] = enum $PolyOptionable<Int>, #PolyOptionable.nought!enumelt
  _ = PolyOptionable<Int>.nought

// CHECK-NEXT:    [[METATYPE:%.*]] = metatype $@thin PolyOptionable<Int>.Type
// CHECK-NEXT:    [[NOUGHT:%.*]] = enum $PolyOptionable<Int>, #PolyOptionable.mere!enumelt, %0
  _ = PolyOptionable<Int>.mere(t)

// CHECK:         return

}


// Regression test for a bug where temporary allocations created as a result of
// tuple implosion were not deallocated in enum constructors.
struct String { var ptr: AnyObject }

enum Foo { case a(P, String) }

func Foo_cases() {
  _ = Foo.a
}

enum Indirect<T> {
  indirect case payload((T, other: T))
  case none
}
// CHECK-LABEL: sil{{.*}} @{{.*}}makeIndirectEnum{{.*}} : $@convention(thin) <T> (@in_guaranteed T) -> @owned Indirect<T>
// CHECK: [[BOX:%.*]] = alloc_box $<τ_0_0> { var (τ_0_0, other: τ_0_0) } <T>
// CHECK: enum $Indirect<T>, #Indirect.payload!enumelt, [[BOX]] : $<τ_0_0> { var (τ_0_0, other: τ_0_0) } <T>
func makeIndirectEnum<T>(_ payload: T) -> Indirect<T> {
  return Indirect.payload((payload, other: payload))
}

// https://github.com/apple/swift/issues/52118

enum TrailingClosureConcrete {
  case label(fn: () -> Int)
  case noLabel(() -> Int)
  case twoElementsLabel(x: Int, fn: () -> Int)
  case twoElementsNoLabel(_ x: Int, _ fn: () -> Int)
}

func useTrailingClosureConcrete() {
  _ = TrailingClosureConcrete.label { 0 }
  _ = TrailingClosureConcrete.noLabel { 0 }
  _ = TrailingClosureConcrete.twoElementsLabel(x: 0) { 0 }
  _ = TrailingClosureConcrete.twoElementsNoLabel(0) { 0 }
}

enum TrailingClosureGeneric<T> {
  case label(fn: () -> T)
  case noLabel(() -> T)
  case twoElementsLabel(x: T, fn: () -> T)
  case twoElementsNoLabel(_ x: T, _ fn: () -> T)
}

func useTrailingClosureGeneric<T>(t: T) {
  _ = TrailingClosureGeneric<T>.label { t }
  _ = TrailingClosureGeneric<T>.noLabel { t }
  _ = TrailingClosureGeneric<T>.twoElementsLabel(x: t) { t }
  _ = TrailingClosureGeneric<T>.twoElementsNoLabel(t) { t }
}

// https://github.com/apple/swift/issues/50338

enum OneOrTwo {
  case one
  case two
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum18matchOptionalEnum13baryAA8OneOrTwoOSg_tF : $@convention(thin) (Optional<OneOrTwo>) -> () {
// CHECK: bb0(%0 : $Optional<OneOrTwo>):
// CHECK-NEXT:  debug_value %0 : $Optional<OneOrTwo>, let, name "bar", argno 1
// CHECK-NEXT:  switch_enum %0 : $Optional<OneOrTwo>, case #Optional.some!enumelt: bb1, default bb4
// CHECK: bb1([[PHI_ARG:%.*]] : $OneOrTwo):
// CHECK-NEXT:  switch_enum [[PHI_ARG]] : $OneOrTwo, case #OneOrTwo.one!enumelt: bb2, case #OneOrTwo.two!enumelt: bb3
func matchOptionalEnum1(bar: OneOrTwo?) {
  switch bar {
  case .one: print("one")
  case .two?: print("two")
  default: print("default")
  }
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum18matchOptionalEnum23baryAA8OneOrTwoOSgSg_tF : $@convention(thin) (Optional<Optional<OneOrTwo>>) -> () {
// CHECK: bb0(%0 : $Optional<Optional<OneOrTwo>>):
// CHECK-NEXT: debug_value %0 : $Optional<Optional<OneOrTwo>>, let, name "bar", argno 1
// CHECK-NEXT: switch_enum %0 : $Optional<Optional<OneOrTwo>>, case #Optional.none!enumelt: bb1, default bb2
func matchOptionalEnum2(bar: OneOrTwo??) {
  switch bar {
  case .none: print("none")
  default: print("default")
  }
}

enum OneTwoOrNone {
    case one
    case two

    var none: Self {
        .one
    }
}

// CHECK-LABEL: sil hidden [ossa] @$s4enum18matchOptionalEnum33baryAA12OneTwoOrNoneOSg_tF : $@convention(thin) (Optional<OneTwoOrNone>) -> () {
// CHECK: bb0(%0 : $Optional<OneTwoOrNone>):
// CHECK-NEXT:  debug_value %0 : $Optional<OneTwoOrNone>, let, name "bar", argno 1
// CHECK-NEXT:  switch_enum %0 : $Optional<OneTwoOrNone>, case #Optional.some!enumelt: bb1, case #Optional.none!enumelt: bb4
// CHECK: bb1([[PHI_ARG:%.*]] : $OneTwoOrNone):
// CHECK-NEXT:  switch_enum [[PHI_ARG]] : $OneTwoOrNone, case #OneTwoOrNone.one!enumelt: bb2, case #OneTwoOrNone.two!enumelt: bb3
func matchOptionalEnum3(bar: OneTwoOrNone?) {
  switch bar {
  case .one: print("one")
  case .two?: print("two")
  case .none: print("none")
  default: print("default")
  }
}

// Make sure that we handle enum, tuple initialization composed
// correctly. Previously, we leaked down a failure path due to us misusing
// scopes.
enum rdar81817725 {
    case localAddress
    case setOption(Int, Any)

    static func takeAny(_:Any) -> Bool { return true }

    static func testSwitchCleanup(syscall: rdar81817725, expectedLevel: Int,
                                  valueMatcher: (Any) -> Bool)
      throws -> Bool {
        if case .setOption(expectedLevel, let value) = syscall {
            return rdar81817725.takeAny(value)
        } else {
            return false
        }
    }
}
