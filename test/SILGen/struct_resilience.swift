// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_struct.swiftmodule %S/../Inputs/resilient_struct.swift
// RUN: %target-swift-emit-silgen -I %t -enable-library-evolution %s | %FileCheck %s

import resilient_struct

// Resilient structs from outside our resilience domain are always address-only

// CHECK-LABEL: sil hidden [ossa] @$s17struct_resilience26functionWithResilientTypes_1f010resilient_A04SizeVAF_A2FXEtF : $@convention(thin) (@in_guaranteed Size, @guaranteed @noescape @callee_guaranteed (@in_guaranteed Size) -> @out Size) -> @out Size
// CHECK:       bb0(%0 : $*Size, %1 : $*Size, %2 : @guaranteed $@noescape @callee_guaranteed (@in_guaranteed Size) -> @out Size):
func functionWithResilientTypes(_ s: Size, f: (Size) -> Size) -> Size {

  // Stored properties of resilient structs from outside our resilience
  // domain are accessed through accessors

// CHECK:         copy_addr %1 to [init] [[OTHER_SIZE_BOX:%[0-9]*]] : $*Size
  var s2 = s

// CHECK:         [[GETTER:%.*]] = function_ref @$s16resilient_struct4SizeV1wSivg : $@convention(method) (@in_guaranteed Size) -> Int
// CHECK:         [[RESULT:%.*]] = apply [[GETTER]](%1)
// CHECK:         [[WRITE:%.*]] = begin_access [modify] [unknown] [[OTHER_SIZE_BOX]] : $*Size
// CHECK:         [[SETTER:%.*]] = function_ref @$s16resilient_struct4SizeV1wSivs : $@convention(method) (Int, @inout Size) -> ()
// CHECK:         apply [[SETTER]]([[RESULT]], [[WRITE]])
  s2.w = s.w

// CHECK:         [[FN:%.*]] = function_ref @$s16resilient_struct4SizeV1hSivg : $@convention(method) (@in_guaranteed Size) -> Int
// CHECK:         [[RESULT:%.*]] = apply [[FN]](%1)
  _ = s.h

// CHECK:         apply {{.*}}(%0, %1)
// CHECK:         return
  return f(s)
}

// Use modify for inout access of properties in resilient structs
// from a different resilience domain

public func inoutFunc(_ x: inout Int) {}

// CHECK-LABEL: sil hidden [ossa] @$s17struct_resilience18resilientInOutTestyy0c1_A04SizeVzF : $@convention(thin) (@inout Size) -> ()

func resilientInOutTest(_ s: inout Size) {

// CHECK:         function_ref @$s16resilient_struct4SizeV1wSivM
// CHECK:         function_ref @$s17struct_resilience9inoutFuncyySizF

  inoutFunc(&s.w)

// CHECK: return
}

// Fixed-layout structs may be trivial or loadable

// CHECK-LABEL: sil hidden [ossa] @$s17struct_resilience28functionWithFixedLayoutTypes_1f010resilient_A05PointVAF_A2FXEtF : $@convention(thin) (Point, @guaranteed @noescape @callee_guaranteed (Point) -> Point) -> Point
// CHECK:       bb0(%0 : $Point, %1 : @guaranteed $@noescape @callee_guaranteed (Point) -> Point):
func functionWithFixedLayoutTypes(_ p: Point, f: (Point) -> Point) -> Point {

  // Stored properties of fixed layout structs are accessed directly
  var p2 = p

// CHECK:         [[RESULT:%.*]] = struct_extract %0 : $Point, #Point.x
// CHECK:         [[DEST:%.*]] = struct_element_addr [[POINT_BOX:%[0-9]*]] : $*Point, #Point.x
// CHECK:         assign [[RESULT]] to [[DEST]] : $*Int
  p2.x = p.x

// CHECK:         [[RESULT:%.*]] = struct_extract %0 : $Point, #Point.y
  _ = p.y

// CHECK:         [[NEW_POINT:%.*]] = apply {{.*}}(%0)
// CHECK:         return [[NEW_POINT]]
  return f(p)
}

// Fixed-layout struct with resilient stored properties is still address-only

// CHECK-LABEL: sil hidden [ossa] @$s17struct_resilience39functionWithFixedLayoutOfResilientTypes_1f010resilient_A09RectangleVAF_A2FXEtF : $@convention(thin) (@in_guaranteed Rectangle, @guaranteed @noescape @callee_guaranteed (@in_guaranteed Rectangle) -> @out Rectangle) -> @out Rectangle
// CHECK:        bb0(%0 : $*Rectangle, %1 : $*Rectangle, %2 : @guaranteed $@noescape @callee_guaranteed (@in_guaranteed Rectangle) -> @out Rectangle):
func functionWithFixedLayoutOfResilientTypes(_ r: Rectangle, f: (Rectangle) -> Rectangle) -> Rectangle {
  return f(r)
}

// Make sure we generate getters and setters for stored properties of
// resilient structs

public struct MySize {

  // Static computed property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV10expirationSivgZ : $@convention(method) (@thin MySize.Type) -> Int
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV10expirationSivsZ : $@convention(method) (Int, @thin MySize.Type) -> ()
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV10expirationSivMZ : $@yield_once @convention(method) (@thin MySize.Type) -> @yields @inout Int
  public static var expiration: Int {
    get { return copyright + 70 }
    set { copyright = newValue - 70 }
  }

  // Instance computed property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1dSivg : $@convention(method) (@in_guaranteed MySize) -> Int
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1dSivs : $@convention(method) (Int, @inout MySize) -> ()
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1dSivM : $@yield_once @convention(method) (@inout MySize) -> @yields @inout Int
  public var d: Int {
    get { return 0 }
    set { }
  }

  // Instance stored property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1wSivg : $@convention(method) (@in_guaranteed MySize) -> Int
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1wSivs : $@convention(method) (Int, @inout MySize) -> ()
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1wSivM : $@yield_once @convention(method) (@inout MySize) -> @yields @inout Int
  public var w: Int

  // Read-only instance stored property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1hSivg : $@convention(method) (@in_guaranteed MySize) -> Int
  public let h: Int

  // Weak property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV1iyXlSgvg : $@convention(method) (@in_guaranteed MySize) -> @owned Optional<AnyObject>
  public weak var i: AnyObject?

  // Static stored property

// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV9copyrightSivgZ : $@convention(method) (@thin MySize.Type) -> Int
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV9copyrightSivsZ : $@convention(method) (Int, @thin MySize.Type) -> ()
// CHECK-LABEL: sil [ossa] @$s17struct_resilience6MySizeV9copyrightSivMZ : $@yield_once @convention(method) (@thin MySize.Type) -> @yields @inout Int
  public static var copyright: Int = 0
}

// CHECK-LABEL: sil [ossa] @$s17struct_resilience28functionWithMyResilientTypes_1fAA0E4SizeVAE_A2EXEtF : $@convention(thin) (@in_guaranteed MySize, @guaranteed @noescape @callee_guaranteed (@in_guaranteed MySize) -> @out MySize) -> @out MySize
public func functionWithMyResilientTypes(_ s: MySize, f: (MySize) -> MySize) -> MySize {

  // Stored properties of resilient structs from inside our resilience
  // domain are accessed directly

// CHECK:         copy_addr %1 to [init] [[SIZE_BOX:%[0-9]*]] : $*MySize
  var s2 = s

// CHECK:         [[SRC_ADDR:%.*]] = struct_element_addr %1 : $*MySize, #MySize.w
// CHECK:         [[SRC:%.*]] = load [trivial] [[SRC_ADDR]] : $*Int
// CHECK:         [[WRITE:%.*]] = begin_access [modify] [unknown] [[SIZE_BOX]] : $*MySize
// CHECK:         [[DEST_ADDR:%.*]] = struct_element_addr [[WRITE]] : $*MySize, #MySize.w
// CHECK:         assign [[SRC]] to [[DEST_ADDR]] : $*Int
  s2.w = s.w

// CHECK:         [[RESULT_ADDR:%.*]] = struct_element_addr %1 : $*MySize, #MySize.h
// CHECK:         [[RESULT:%.*]] = load [trivial] [[RESULT_ADDR]] : $*Int
  _ = s.h

// CHECK:         apply {{.*}}(%0, %1)
// CHECK:         return
  return f(s)
}

// CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s17struct_resilience25publicTransparentFunctionySiAA6MySizeVF : $@convention(thin) (@in_guaranteed MySize) -> Int
@_transparent public func publicTransparentFunction(_ s: MySize) -> Int {

  // Since the body of a public transparent function might be inlined into
  // other resilience domains, we have to use accessors

// CHECK:         [[GETTER:%.*]] = function_ref @$s17struct_resilience6MySizeV1wSivg
// CHECK-NEXT:    [[RESULT:%.*]] = apply [[GETTER]](%0)
// CHECK-NEXT:    return [[RESULT]]
  return s.w
}

// CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s17struct_resilience30publicTransparentLocalFunctionySiycAA6MySizeVF : $@convention(thin) (@in_guaranteed MySize) -> @owned @callee_guaranteed () -> Int
@_transparent public func publicTransparentLocalFunction(_ s: MySize) -> () -> Int {

// CHECK-LABEL: sil shared [serialized] [ossa] @$s17struct_resilience30publicTransparentLocalFunctionySiycAA6MySizeVFSiycfU_ : $@convention(thin) (@in_guaranteed MySize) -> Int
// CHECK: function_ref @$s17struct_resilience6MySizeV1wSivg : $@convention(method) (@in_guaranteed MySize) -> Int
// CHECK: return {{.*}} : $Int

  return { s.w }

}

// CHECK-LABEL: sil hidden [transparent] [ossa] @$s17struct_resilience27internalTransparentFunctionySiAA6MySizeVF : $@convention(thin) (@in_guaranteed MySize) -> Int
// CHECK: bb0([[ARG:%.*]] : $*MySize):
@_transparent func internalTransparentFunction(_ s: MySize) -> Int {

  // The body of an internal transparent function will not be inlined into
  // other resilience domains, so we can access storage directly

// CHECK:         [[W_ADDR:%.*]] = struct_element_addr [[ARG]] : $*MySize, #MySize.w
// CHECK-NEXT:    [[RESULT:%.*]] = load [trivial] [[W_ADDR]] : $*Int
// CHECK-NEXT:    return [[RESULT]]
  return s.w
}

// CHECK-LABEL: sil [serialized] [ossa] @$s17struct_resilience23publicInlinableFunctionySiAA6MySizeVF : $@convention(thin) (@in_guaranteed MySize) -> Int
@inlinable public func publicInlinableFunction(_ s: MySize) -> Int {

  // Since the body of a public transparent function might be inlined into
  // other resilience domains, we have to use accessors

// CHECK:         [[GETTER:%.*]] = function_ref @$s17struct_resilience6MySizeV1wSivg
// CHECK-NEXT:    [[RESULT:%.*]] = apply [[GETTER]](%0)
// CHECK-NEXT:    return [[RESULT]]
  return s.w

}

// Make sure that @usableFromInline entities can be resilient

@usableFromInline struct VersionedResilientStruct {
  @usableFromInline let x: Int
  @usableFromInline let y: Int

  @usableFromInline init(x: Int, y: Int) {
    self.x = x
    self.y = y
  }

  // Non-inlinable initializer, assigns to self -- treated as a root initializer

  // CHECK-LABEL: sil [ossa] @$s17struct_resilience24VersionedResilientStructV5otherA2C_tcfC : $@convention(method) (@in VersionedResilientStruct, @thin VersionedResilientStruct.Type) -> @out VersionedResilientStruct
  // CHECK:      [[SELF_BOX:%.*]] = alloc_box ${ var VersionedResilientStruct }
  // CHECK-NEXT: [[SELF_UNINIT:%.*]] = mark_uninitialized [rootself] [[SELF_BOX]]
  // CHECK:      return
  @usableFromInline init(other: VersionedResilientStruct) {
    self = other
  }

  // Inlinable initializer, assigns to self -- treated as a delegating initializer

  // CHECK-LABEL: sil [serialized] [ossa] @$s17struct_resilience24VersionedResilientStructV6other2A2C_tcfC : $@convention(method) (@in VersionedResilientStruct, @thin VersionedResilientStruct.Type) -> @out VersionedResilientStruct
  // CHECK:      [[SELF_BOX:%.*]] = alloc_box ${ var VersionedResilientStruct }
  // CHECK-NEXT: [[SELF_UNINIT:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]]
  // CHECK:      return
  @usableFromInline @inlinable init(other2: VersionedResilientStruct) {
    self = other2
  }
}

// CHECK-LABEL: sil [transparent] [serialized] [ossa] @$s17struct_resilience27useVersionedResilientStructyAA0deF0VADF : $@convention(thin) (@in_guaranteed VersionedResilientStruct) -> @out VersionedResilientStruct
@usableFromInline
@_transparent func useVersionedResilientStruct(_ s: VersionedResilientStruct)
    -> VersionedResilientStruct {
  // CHECK:       function_ref @$s17struct_resilience24VersionedResilientStructV1ySivg
  // CHECK:       function_ref @$s17struct_resilience24VersionedResilientStructV1xSivg
  // CHECK:       function_ref @$s17struct_resilience24VersionedResilientStructV1x1yACSi_SitcfC

  return VersionedResilientStruct(x: s.y, y: s.x)

  // CHECK:       return
}

// CHECK-LABEL: sil [serialized] [ossa] @$s17struct_resilience18inlinableInoutTestyyAA6MySizeVzF : $@convention(thin) (@inout MySize) -> ()
@inlinable public func inlinableInoutTest(_ s: inout MySize) {
  // Inlinable functions can be inlined in other resilience domains.
  //
  // Make sure we use modify for an inout access of a resilient struct
  // property inside an inlinable function.

  // CHECK:       function_ref @$s17struct_resilience6MySizeV1wSivM
  inoutFunc(&s.w)

  // CHECK:       return
}

// Initializers for resilient structs
extension Size {

  // CHECK-LABEL: sil hidden [ossa] @$s16resilient_struct4SizeV0B11_resilienceE5otherA2C_tcfC : $@convention(method) (@in Size, @thin Size.Type) -> @out Size
  // CHECK:      [[SELF_BOX:%.*]] = alloc_box ${ var Size }
  // CHECK-NEXT: [[SELF_UNINIT:%.*]] = mark_uninitialized [delegatingself] [[SELF_BOX]] : ${ var Size }
  // CHECK:      return
  init(other: Size) {
    self = other
  }
}
