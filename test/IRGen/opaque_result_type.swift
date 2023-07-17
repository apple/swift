// RUN: %empty-directory(%t)
// RUN: %{python} %utils/chex.py < %s > %t/opaque_result_type.swift
// RUN: %target-swift-frontend -enable-experimental-named-opaque-types -enable-implicit-dynamic -disable-availability-checking -emit-ir %t/opaque_result_type.swift | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-NODEBUG %t/opaque_result_type.swift

// rdar://76863553
// UNSUPPORTED: OS=watchos && CPU=x86_64

public protocol O {
  func bar()
}
public protocol O2 {
  func baz()
}

protocol P {
  associatedtype A: O

  func poo() -> A
}
protocol Q: AnyObject {
  associatedtype B: O, O2

  func qoo() -> B
}

extension Int: O, O2 {
  public func bar() {}
  public func baz() {}
}

@_marker protocol Marker { }
extension Int: Marker { }

extension String: P {
  // CHECK-LABEL: @"$sSS18opaque_result_typeE3pooQryFQOMQ" = {{.*}}constant <{ {{.*}} }> <{
  // -- header: opaque type context (0x4), generic (0x80), unique (0x40), two entries (0x2_0000)
  // CHECK-SAME:         <i32 0x2_00c4>,
  // -- parent context: module, or anon context for function
  // CHECK-SAME:         @"$s18opaque_result_typeMXM"
  // -- mangled underlying type
  // CHECK-SAME:         @"symbolic Si"
  // -- conformance to O
  // CHECK-SAME:         @"get_witness_table Si18opaque_result_type1OHpyHC
  // CHECK-SAME:  }>
  func poo() -> some O {
    return 0
  }

  // CHECK-LABEL: @"$sSS18opaque_result_typeE4propQrvpQOMQ" = {{.*}}constant
  public var prop: some O {
    return 0
  }

  // CHECK-LABEL: @"$sSS18opaque_result_typeEQrycipQOMQ" = {{.*}}constant
  public subscript() -> some O {
    return 0
  }
}

// CHECK-LABEL: @"$s18opaque_result_type10globalPropQrvpQOMQ" = {{.*}}constant
public var globalProp: some O {
  return 0
}

public class C: P, Q, Marker {
  // CHECK-LABEL: @"$s18opaque_result_type1CC3pooQryFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
  // -- header: opaque type context (0x4), generic (0x80), unique (0x40), two entries (0x2_0000)
  // CHECK-SAME:         <i32 0x2_00c4>
  // -- parent context: module, or anon context for function
  // CHECK-SAME:         @"$s18opaque_result_typeMXM"
  // -- mangled underlying type
  // CHECK-SAME:         @"symbolic Si"
  // -- conformance to O
  // CHECK-SAME:         @"get_witness_table Si18opaque_result_type1OHpyHC
  // CHECK-SAME:  }>
  func poo() -> some O {
    return 0
  }

  // CHECK-LABEL: @"$s18opaque_result_type1CC3qooQryFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
  // -- header: opaque type context (0x4), generic (0x80), unique (0x40), three entries (0x3_0000)
  // CHECK-SAME:         <i32 0x3_00c4>
  // -- parent context: module, or anon context for function
  // CHECK-SAME:         @"$s18opaque_result_typeMXM"
  // -- mangled underlying type
  // CHECK-SAME:         @"symbolic Si"
  // -- conformance to O
  // CHECK-SAME:         @"get_witness_table Si18opaque_result_type1OHpyHC
  // -- conformance to O2
  // CHECK-SAME:         @"get_witness_table Si18opaque_result_type2O2HpyHC
  // CHECK-SAME:  }>
  func qoo() -> some O & O2 {
    return 0
  }
}

// CHECK-LABEL: @"$s18opaque_result_type3foo1xQrSS_tFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
// -- header: opaque type context (0x4), generic (0x80), unique (0x40), two entries (0x2_0000)
// CHECK-SAME:         <i32 0x2_00c4>
// -- parent context: module, or anon context for function
// CHECK-SAME:         @"$s18opaque_result_typeMXM"
// -- mangled underlying type
// CHECK-SAME:         @"symbolic SS"
// -- conformance to P
// CHECK-SAME:         @"get_witness_table SS18opaque_result_type1PHpyHC
// CHECK-SAME:  }>
func foo(x: String) -> some P {
  return x
}

// CHECK-LABEL: @"$s18opaque_result_type3bar1yQrAA1CC_tFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
// -- header: opaque type context (0x4), generic (0x80), unique (0x40), two entries (0x2_0000)
// CHECK-SAME:         <i32 0x2_00c4>
// -- parent context: module, or anon context for function
// CHECK-SAME:         @"$s18opaque_result_typeMXM"
// -- mangled underlying type
// CHECK-SAME:         @"symbolic _____ 18opaque_result_type1CC"
// -- conformance to Q
// CHECK-SAME:         @"get_witness_table 18opaque_result_type1CCAA1QHPyHC
// CHECK-SAME:  }>
func bar(y: C) -> some Q {
  return y
}

// CHECK-LABEL: @"$s18opaque_result_type3baz1zQrx_tAA1PRzAA1QRzlFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
// -- header: opaque type context (0x4), generic (0x80), unique (0x40), three entries (0x3_0000)
// CHECK-SAME:         <i32 0x3_00c4>
// -- parent context: anon context for function
// CHECK-SAME:         @"$s18opaque_result_type3baz1zQrx_tAA1PRzAA1QRzlFMXX"
// -- mangled underlying type
// CHECK-SAME:         @"symbolic x"
// -- conformance to P
// CHECK-SAME:         @"get_witness_table 18opaque_result_type1PRzAA1QRzlxAaB
// -- conformance to Q
// CHECK-SAME:         @"get_witness_table 18opaque_result_type1PRzAA1QRzlxAaC
// CHECK-SAME:  }>
func baz<T: P & Q>(z: T) -> some P & Q {
  return z
}

// CHECK-LABEL: @"$s18opaque_result_type4fizz1zQrx_tAA6MarkerRzAA1PRzAA1QRzlFQOMQ" = {{.*}} constant <{ {{.*}} }> <{
// -- header: opaque type context (0x4), generic (0x80), unique (0x40), three entries (0x3_0000)
// CHECK-SAME:         <i32 0x3_00c4>
// -- parent context: anon context for function
// CHECK-SAME:         @"$s18opaque_result_type4fizz1zQrx_tAA6MarkerRzAA1PRzAA1QRzlFMXX"
// -- mangled underlying type
// CHECK-SAME:         @"symbolic x"
// -- conformance to P
// CHECK-SAME:         @"get_witness_table 18opaque_result_type6MarkerRzAA1PRzAA1QRzlxAaCHD2_
// -- conformance to Q
// CHECK-SAME:         @"get_witness_table 18opaque_result_type6MarkerRzAA1PRzAA1QRzlxAaDHD3_
// CHECK-SAME:  }>
func fizz<T: P & Q & Marker>(z: T) -> some P & Q & Marker {
  return z
}

func bauble<T: P & Q & Marker, U: Q>(z: T, u: U) -> [(some P & Q & Marker, (some Q)?)] {
  return [(z, u)]
}

// Ensure the local type's opaque descriptor gets emitted.
// CHECK-LABEL: @"$s18opaque_result_type11localOpaqueQryF0D0L_QryFQOMQ" = 
func localOpaque() -> some P {
  func local() -> some P {
    return "local"
  }

  return local()
}

public func useFoo(x: String, y: C) {
  let p = foo(x: x)
  let pa = p.poo()
  pa.bar()

  let q = bar(y: y)
  let qb = q.qoo()
  qb.bar()
  qb.baz()

  let pq = baz(z: y)
  let pqa = pq.poo()
  pqa.bar()
  let pqb = pq.qoo()
  pqb.bar()
  pqb.baz()

  let _ = bauble(z: y, u: y)
}

// CHECK-LABEL: define {{.*}} @"$s18opaque_result_type6bauble1z1uSayQr_QR_SgtGx_q_tAA6MarkerRzAA1PRzAA1QRzAaIR_r0_lF"

// CHECK-LABEL: define {{.*}} @"$s18opaque_result_type6useFoo1x1yySS_AA1CCtF"
// CHECK: [[OPAQUE:%.*]] = call {{.*}} @"$s18opaque_result_type3baz1zQrx_tAA1PRzAA1QRzlFQOMg"
// CHECK: [[CONFORMANCE:%.*]] = call swiftcc ptr @swift_getOpaqueTypeConformance{{2?}}(ptr {{.*}}, ptr [[OPAQUE]], [[WORD:i32|i64]] 1)
// CHECK: [[TYPE:%.*]] = call {{.*}} @__swift_instantiateConcreteTypeFromMangledName{{.*}}({{.*}} @"$s18opaque_result_type3baz1zQrx_tAA1PRzAA1QRzlFQOyAA1CCQo_MD")
// CHECK: call swiftcc ptr @swift_getAssociatedConformanceWitness(ptr [[CONFORMANCE]], ptr [[TYPE]]

// Make sure we can mangle named opaque result types
struct Boom<T: P> {
  var prop1: Int = 5
  var prop2: <U, V> (U, V) = ("hello", 5)
}

// CHECK-LABEL: define {{.*}} @"$s18opaque_result_type9gimmeBoomypyF
// CHECK: call swiftcc void @"$s18opaque_result_type4BoomV5prop15prop2ACyxGSi_AcEQr_QR_tvpQOyx_Qo__AcEQr_QR_tvpQOyx_Qo0_ttcfcfA0_"
public func gimmeBoom() -> Any {
  Boom<String>(prop1: 5)
}

// CHECK-LABEL: define {{.*}} @"$sSS18opaque_result_type1PAA1AAaBP_AA1OPWT"
// CHECK: [[OPAQUE:%.*]] = call {{.*}} @"$sSS18opaque_result_typeE3pooQryFQOMg"
// CHECK: call swiftcc ptr @swift_getOpaqueTypeConformance{{2?}}(ptr {{.*}}, ptr [[OPAQUE]], [[WORD]] 1)

// rdar://problem/49585457
protocol R {
  associatedtype A: R
  func getA() -> A
}

struct Wrapper<T: R>: R {
  var wrapped: T
  
  func getA() -> some R {
    return wrapped.getA()
  }
}

struct X<T: R, U: R>: R {
  var t: T
  var u: U

  func getA() -> some R {
    return Wrapper(wrapped: u)
  }
}

var globalOProp: some O = 0

public struct OpaqueProps {
  static var staticOProp: some O = 0
  var instanceOProp: some O = 0
}

// Make sure we don't recurse indefinitely on recursive enums.
public enum RecursiveEnum {
  case a(Int)
  case b(String)
  indirect case c(RecursiveEnum)
}

public enum EnumWithTupleWithOpaqueField {
  case a(Int)
  case b((OpaqueProps, String))
}

// rdar://86800325 - Make sure we don't crash on result builders.
@resultBuilder
struct Builder {
  static func buildBlock(_: Any...) -> Int { 5 }
}

protocol P2 {
  associatedtype A: P2

  @Builder var builder: A { get }
}

extension Int: P2 {
  var builder: some P2 { 5 }
}

struct UseBuilder: P2 {
  var builder: some P2 {
    let extractedExpr: some P2 = 5
  }
}
