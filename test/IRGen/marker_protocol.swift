// RUN: %target-swift-frontend -primary-file %s -emit-ir -o - | %FileCheck %s

// Marker protocols should have no ABI impact at all, so this source file checks
// for the absence of symbols related to marker protocols.

// CHECK-NOT: $s15marker_protocol1PP
// CHECK-NOT: $s15marker_protocol1PMp

// REQUIRES: PTRSIZE=64

@_marker public protocol P { }

extension Int: P { }
extension Array: P where Element: P { }

// No mention of the marker protocol for runtime type instantiation.
// CHECK-LABEL: @"$sSS_15marker_protocol1P_ptMD" =
// CHECK-SAME: @"symbolic SS_ypt"

// CHECK-LABEL: @"$s15marker_protocol1QMp" = {{(dllexport |protected )?}}constant
// CHECK-SAME: i32 trunc{{.*}}s15marker_protocolMXM{{.*}}s15marker_protocol1QMp
// CHECK-SAME: i32 0, i32 5, i32 0
public protocol Q: P {
  func f()
  func g()
  func h()
  func i()
  func j()
}

protocol R { }

@_marker protocol S: AnyObject { }

// Note: no mention of marker protocols here.
// CHECK-LABEL: @"$s15marker_protocol10HasMarkersVMF" =
// CHECK-SAME: @"symbolic yp"
// CHECK-SAME: @"symbolic ______p 15marker_protocol1QP"
// CHECK-SAME: @"symbolic ______p 15marker_protocol1RP"
// CHECK-SAME: @"symbolic yXl"
struct HasMarkers {
  var field1: P
  var field2: P & Q
  var field3: P & R
  var field4: S
}

// Note: no mention of marker protocols when forming a dictionary.
// CHECK-LABEL: define{{.*}}@"$s15marker_protocol0A12InDictionaryypyF"
// CHECK: call ptr @__swift_instantiateConcreteTypeFromMangledName({{.*}} @"$sSS_15marker_protocol1P_ptMD")
public func markerInDictionary() -> Any {
  let dict: [String: P] = ["answer" : 42]
  return dict
}

// Note: no witness tables
// CHECK: swiftcc void @"$s15marker_protocol7genericyyxAA1PRzlF"(ptr noalias %0, ptr %T)
public func generic<T: P>(_: T) { }

public struct GenericType<T: Hashable & P> { }

// CHECK-LABEL: @"$s15marker_protocol11testGeneric1i5arrayySi_SaySiGtF"(
public func testGeneric(i: Int, array: [Int]) {
  generic(i)
  generic(array)
  // CHECK: __swift_instantiateConcreteTypeFromMangledName{{.*}}$s15marker_protocol11GenericTypeVySaySiGGmMD
  print(GenericType<[Int]>.self)
}

// Forming an existential involving a marker protocol would crash the compiler
protocol SelfConstrainedProtocol {
  static var shared: Self { get }
}

struct Foo: SelfConstrainedProtocol {
  let x: P
  static var shared: Self {
    Foo(x: 123)
  }
}

protocol P2 {
  associatedtype Foo
}

// CHECK: define{{.*}}$s15marker_protocol3fooyy3FooQz_xtAA1PRzAA2P2RzlF
func foo<T: P & P2>(_: T.Foo, _: T) { }

class C {}

let v1 = (any C & P).self
let v2 = C.self

// CHECK-LABEL: define hidden swiftcc void @"$s15marker_protocol23testProtocolCompositionyyF"()
// CHECK: [[V1:%.*]] = call ptr @__swift_instantiateConcreteTypeFromMangledName(ptr @"$s15marker_protocol1P_AA1CCXcMD")
// CHECK: [[V2:%.*]] = load ptr, ptr @"$s15marker_protocol2v2AA1CCmvp"
func testProtocolComposition() {
  print(v1 == v2)
}
