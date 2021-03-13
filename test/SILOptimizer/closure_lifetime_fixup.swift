// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend %S/../Inputs/resilient_struct.swift -enable-library-evolution -emit-module -emit-module-path %t/resilient_struct.swiftmodule
// RUN: %target-swift-frontend %S/../Inputs/resilient_enum.swift -I %t -enable-library-evolution -emit-module -emit-module-path %t/resilient_enum.swiftmodule
// RUN: %target-swift-frontend %s -sil-verify-all -emit-sil -disable-copy-propagation -I %t -o - | %FileCheck %s --check-prefix=CHECK --check-prefix=%target-os

// Using -disable-copy-propagation to pattern match against older SIL
// output. At least until -enable-copy-propagation has been around
// long enough in the same form to be worth rewriting CHECK lines.

import resilient_struct
import resilient_enum

func use_closure(_ c : () -> () ) {
  c()
}

func use_closureGeneric<T>(_ c : () -> T ) {
  _ = c()
}

public class C {
  func doIt() {}
  func returnInt() -> Int { return 0 }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup10testSimple1cyAA1CC_tF : $@convention(thin) (@guaranteed C) -> () {
// CHECK: bb0([[ARG:%.*]] : $C):
// CHECK: [[F:%.*]]  = function_ref @$s22closure_lifetime_fixup10testSimple1cyAA1CC_tFyyXEfU_
// CHECK-NEXT:  strong_retain [[ARG]] : $C
// CHECK-NEXT:  [[PA:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[F]]([[ARG]]) : $@convention(thin) (@guaranteed C) -> ()
// CHECK-NEXT:  [[CL:%.*]] = mark_dependence [[PA]] : $@noescape @callee_guaranteed () -> () on [[ARG]] : $C
// CHECK-NEXT:  // function_ref use_closure(_:)
// CHECK-NEXT:  [[F2:%.*]] = function_ref @$s22closure_lifetime_fixup04use_A0yyyyXEF : $@convention(thin) (@noescape @callee_guaranteed () -> ()) -> ()
// CHECK-NEXT:  apply [[F2]]([[CL]]) : $@convention(thin) (@noescape @callee_guaranteed () -> ()) -> ()
// CHECK-NEXT:  dealloc_stack [[PA]] : $@noescape @callee_guaranteed () -> ()
// CHECK-NEXT:  strong_release [[ARG]] : $C
// CHECK-NEXT:  tuple ()
// CHECK-NEXT:  return {{.*}} : $()
public func testSimple(c: C) {
  use_closure { c.doIt() }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup11testGeneric1cyAA1CC_tF : $@convention(thin) (@guaranteed C) -> () {
// CHECK:bb0([[ARG:%.*]] : $C):
// CHECK:  [[F:%.*]] = function_ref @$s22closure_lifetime_fixup11testGeneric1cyAA1CC_tFSiyXEfU_ : $@convention(thin) (@guaranteed C) -> Int
// CHECK-NEXT:  strong_retain [[ARG]] : $C
// CHECK-NEXT:  [[PA:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[F]]([[ARG]]) :
// CHECK-NEXT:  [[MD:%.*]] = mark_dependence [[PA]] : $@noescape @callee_guaranteed () -> Int on [[ARG]] : $C
// CHECK-NEXT:  // function_ref thunk for @callee_guaranteed () -> (@unowned Int)
// CHECK-NEXT:  [[F:%.*]] = function_ref @$sSiIgd_SiIegr_TR : $@convention(thin) (@noescape @callee_guaranteed () -> Int) -> @out Int
// CHECK-NEXT:  [[PA2:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[F]]([[MD]]) : $@convention(thin) (@noescape @callee_guaranteed () -> Int) -> @out Int
// CHECK-NEXT:  [[CF2:%.*]] = convert_function [[PA2]]
// CHECK-NEXT:  // function_ref use_closureGeneric<A>(_:)
// CHECK-NEXT:  [[F2:%.*]] = function_ref @$s22closure_lifetime_fixup04use_A7GenericyyxyXElF :
// CHECK-NEXT:  apply [[F2]]<Int>([[CF2]]) :
// CHECK-NEXT:  dealloc_stack [[PA2]]
// CHECK-NEXT:  dealloc_stack [[PA]]
// CHECK-NEXT:  strong_release [[ARG]]
// CHECK-NEXT:  tuple ()
// CHECK-NEXT:  return {{.*}} : $()

public func testGeneric(c: C) {
  use_closureGeneric { return c.returnInt() }
}

public protocol P {
  associatedtype Element
  subscript<U>(a: (Element) -> U, b: (U) -> Element) -> U { get set }
}

// Make sure we keep the closure alive up until after the write back.

// CHECK-LABEL: sil @$s22closure_lifetime_fixup10testModify1pyxz_tAA1PRzSS7ElementRtzlF : $@convention(thin) <T where T : P, T.Element == String> (@inout T) -> () {
// CHECK: bb0
// CHECK:  [[PA1:%.*]] = partial_apply [callee_guaranteed]
// CHECK:  [[CF1:%.*]] = convert_function [[PA1]]
// CHECK:  [[CVT1:%.*]] = convert_escape_to_noescape [[CF1]]
// CHECK:  [[PA2:%.*]] = partial_apply [callee_guaranteed]
// CHECK:  [[CF2:%.*]] = convert_function [[PA2]]
// CHECK:  [[CVT2:%.*]] = convert_escape_to_noescape [[CF2]]
// CHECK:  [[W:%.*]] = witness_method $T, #P.subscript!modify
// CHECK:  ([[BUFFER:%.*]], [[TOKEN:%.*]]) = begin_apply [[W]]<T, Int>([[CVT1]], [[CVT2]], {{.*}})
// CHECK:  end_apply [[TOKEN]]
// CHECK:  strong_release [[CF1]]
// CHECK:  strong_release [[CF2]]
public func testModify<T : P>(p: inout T) where T.Element == String {
  p[{Int($0)!}, {String($0)}] += 1
}

public func dontCrash<In, Out>(test: Bool, body: @escaping ((In) -> Out, In) -> Out ) -> (In) -> Out {
  var result: ((In) -> Out)!
  result = { (x: In) in
    if test {
      return body(result, x)
    }
    let r = body(result, x)
    return r
  }
  return result
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup28to_stack_of_convert_function1pySvSg_tF
// CHECK:  [[FN:%.*]] = function_ref @$s22closure_lifetime_fixup28to_stack_of_convert_function1pySvSg_tFSSSvcfu_ : $@convention(thin) (UnsafeMutableRawPointer) -> @owned String
// CHECK:  [[PA:%.*]] = thin_to_thick_function [[FN]]
// CHECK:  [[CVT:%.*]] = convert_function [[PA]]
// CHECK:  [[CVT2:%.*]] = convert_escape_to_noescape [[CVT]]
// CHECK:  [[REABSTRACT:%.*]] = function_ref @$sSvSSs5Error_pIgyozo_SvSSsAA_pIegnrzo_TR
// CHECK:  [[PA2:%.*]] = partial_apply [callee_guaranteed] [on_stack] [[REABSTRACT]]([[CVT2]])
// CHECK:  [[CF2:%.*]] = convert_function [[PA2]]
// CHECK:  [[MAP:%.*]] = function_ref @$sSq3mapyqd__Sgqd__xKXEKlF
// CHECK:  try_apply [[MAP]]<UnsafeMutableRawPointer, String>({{.*}}, [[CF2]], {{.*}})
public func to_stack_of_convert_function(p: UnsafeMutableRawPointer?) {
  _ = p.map(String.init(describing:))
}


public func no_dealloc_stack_before_unreachable(_ message: String, fileName: StaticString = #file, lineNumber: Int = #line) -> Never  {
  Swift.fatalError(message, file: fileName, line: UInt(lineNumber))
}

// Make sure closures are allocated on the stack.
func useClosure(_ c: () -> ()) {
}
func useClosureThrowing(_ c: () throws -> ()) throws {
}
func useGenericClosure<T, V>(_ c : (T) -> V) {
}
func useGenericClosureThrowing<T, V>(_ c: (T) throws -> V) throws {
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup12captureClass1c1dyAA1CC_AFtKF
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup10useClosureyyyyXEF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup18useClosureThrowingyyyyKXEKF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$sIg_ytytIegnr_TR
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup17useGenericClosureyyq_xXEr0_lF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$ss5Error_pIgzo_ytytsAA_pIegnrzo_TR
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup25useGenericClosureThrowingyyq_xKXEKr0_l
public func captureClass(c: C, d: C) throws {
  useClosure {
    c.doIt()
    d.doIt()
  }

  try useClosureThrowing {
    c.doIt()
    d.doIt()
  }

  useGenericClosure {
    c.doIt()
    d.doIt()
  }

  try useGenericClosureThrowing {
    c.doIt()
    d.doIt()
  }
}

public protocol DoIt {
  func doIt()
}
// CHECK-LABEL: sil @$s22closure_lifetime_fixup14captureGeneric1c1dyx_q_tKAA4DoItRzAaER_r0_lF
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup10useClosureyyyyXEF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup18useClosureThrowingyyyyKXEKF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$sIg_ytytIegnr_TR
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup17useGenericClosureyyq_xXEr0_lF

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$ss5Error_pIgzo_ytytsAA_pIegnrzo_TR
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup25useGenericClosureThrowingyyq_xKXEKr0_lF
public func captureGeneric<C :DoIt,D: DoIt>(c: C, d: D) throws {
  useClosure {
    c.doIt()
    d.doIt()
  }

  try useClosureThrowing {
    c.doIt()
    d.doIt()
  }

  useGenericClosure {
    c.doIt()
    d.doIt()
  }

  try useGenericClosureThrowing {
    c.doIt()
    d.doIt()
  }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup14captureClosure1c1d1tyq_xXE_q_xXExtKr0_lF
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  function_ref @$s22closure_lifetime_fixup10useClosureyyyyXEF

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  function_ref @$s22closure_lifetime_fixup18useClosureThrowingyyyyKXEKF


// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$sIg_ytytIegnr_TR
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: function_ref @$s22closure_lifetime_fixup17useGenericClosureyyq_xXEr0_lF

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  function_ref @$ss5Error_pIgzo_ytytsAA_pIegnrzo_TR
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  function_ref @$s22closure_lifetime_fixup25useGenericClosureThrowingyyq_xKXEKr0_lF
public func captureClosure<T, V>(c : (T) ->V, d: (T) -> V, t: T) throws {
  useClosure {
    c(t)
    d(t)
  }

  try useClosureThrowing {
    c(t)
    d(t)
  }

  useGenericClosure {
    c(t)
    d(t)
  }

  try useGenericClosureThrowing {
    c(t)
    d(t)
  }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup16captureResilient
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

public func captureResilient(c: Size) throws {
  useClosure {
    c.method()
  }

  try useClosureThrowing {
    c.method()
  }

  useGenericClosure {
    c.method()
  }

  try useGenericClosureThrowing {
    c.method()
  }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup10capturePOD1cy16resilient_struct5PointV_tKF
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply
public func capturePOD(c: Point) throws {
  useClosure {
    c.method()
  }

  try useClosureThrowing {
    c.method()
  }

  useGenericClosure {
    c.method()
  }

  try useGenericClosureThrowing {
    c.method()
  }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup11captureEnum
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply
public func captureEnum(c: FullyFixedLayout, d: CustomColor) throws {
  useClosure {
     _ = c
     _ = d
  }

  try useClosureThrowing {
     _ = c
     _ = d
  }

  useGenericClosure {
     _ = c
     _ = d
  }

  try useGenericClosureThrowing {
     _ = c
     _ = d
  }
}

public protocol EmptyP {}

public struct AddressOnlyStruct : EmptyP {}

public enum AddressOnlyEnum {
  case nought
  case mere(EmptyP)
  case phantom(AddressOnlyStruct)
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup22captureAddressOnlyEnum
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply
public func captureAddressOnlyEnum(c: AddressOnlyEnum, d: AddressOnlyEnum) throws {
  useClosure {
     _ = c
     _ = d
  }

  try useClosureThrowing {
     _ = c
     _ = d
  }

  useGenericClosure {
     _ = c
     _ = d
  }

  try useGenericClosureThrowing {
     _ = c
     _ = d
  }
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup15captureProtocol
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply

// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply

// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply
public func captureProtocol(c: EmptyP, d: EmptyP) throws {
  useClosure {
     _ = c
     _ = d
  }

  try useClosureThrowing {
     _ = c
     _ = d
  }

  useGenericClosure { () -> Int in
     _ = c
     _ = d
     return 0
  }

  try useGenericClosureThrowing { () -> Int in
     _ = c
     _ = d
     return 0
  }
}

public protocol Q {
  associatedtype Element
  func test<U>(_ c: (Element) -> U) throws
}

// CHECK-LABEL: sil @$s22closure_lifetime_fixup0A18WithAssociatedType1c1eyx_7ElementQztKAA1QRzlF
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: try_apply
public func closureWithAssociatedType<C : Q> (c: C, e: C.Element) throws {
  try c.test( { _ in _ = e })
}


public class F<T> {
   func test<V>(_ c: (V) throws -> T) {}
   let t : T? = nil
}

// CHECK-LABEL: s22closure_lifetime_fixup22testClosureMethodParam1fyAA1FCyxG_tKlF
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK:  partial_apply [callee_guaranteed] [on_stack]
// CHECK: apply
public func testClosureMethodParam<T>(f: F<T>) throws {
  try f.test { return f.t! }
}
