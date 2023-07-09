// RUN: %target-swift-frontend -primary-file %s -emit-ir -disable-objc-attr-requires-foundation-module -enable-objc-interop | %FileCheck %s -DINT=i%target-ptrsize

protocol A {}

protocol B {
  associatedtype AA: A
  func foo()
}

@objc protocol O {}

protocol C {
  associatedtype OO: O
  func foo()
}

struct SA: A {}
struct SB: B {
  typealias AA = SA
  func foo() {}
}
// CHECK-LABEL: @"$s34witness_table_objc_associated_type2SBVAA1BAAWP" = hidden global [4 x ptr] [
// CHECK:         {{(associated conformance 34witness_table_objc_associated_type2SBVAA1BAA2AAAaDP_AA1A|.ptrauth)}}
// CHECK:         ptr {{.*}}@"$s34witness_table_objc_associated_type2SBVAA1BA2aDP3fooyyFTW{{(\.ptrauth)?}}"
// CHECK:       ]

class CO: O {}
struct SO: C {
  typealias OO = CO
  func foo() {}
}
// CHECK-LABEL: @"$s34witness_table_objc_associated_type2SOVAA1CAAWP" = hidden global [3 x ptr] [
// CHECK:         ptr {{.*}}@"$s34witness_table_objc_associated_type2SOVAA1CA2aDP3fooyyFTW{{(\.ptrauth)?}}"
// CHECK:       ]

// CHECK-LABEL: define hidden swiftcc void @"$s34witness_table_objc_associated_type0A25OffsetAfterAssociatedTypeyyxAA1BRzlF"(ptr noalias nocapture %0, ptr %T, ptr %T.B)
func witnessOffsetAfterAssociatedType<T: B>(_ x: T) {
  // CHECK:         [[FOO_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.B, i32 3
  // CHECK:         [[FOO:%.*]] = load {{.*}} [[FOO_ADDR]]
  // CHECK:         call swiftcc void [[FOO]]
  x.foo()
}

// CHECK-LABEL: define hidden swiftcc void @"$s34witness_table_objc_associated_type0A29OffsetAfterAssociatedTypeObjCyyxAA1CRzlF"(ptr noalias nocapture %0, ptr %T, ptr %T.C) {{.*}} {
func witnessOffsetAfterAssociatedTypeObjC<T: C>(_ x: T) {
  // CHECK:         [[FOO_ADDR:%.*]] = getelementptr inbounds ptr, ptr %T.C, i32 2
  // CHECK:         [[FOO:%.*]] = load {{.*}} [[FOO_ADDR]]
  // CHECK:         call swiftcc void [[FOO]]
  x.foo()
}
