// RUN: %target-swift-frontend -O -sil-verify-all -emit-sil -Xllvm '-sil-inline-never-functions=$sSa6appendyy' -Xllvm -sil-inline-never-function='$sSa6append10contentsOfyqd__n_t7ElementQyd__RszSTRd__lFSi_SaySiGTg5' %s | %FileCheck %s
// REQUIRES: swift_stdlib_no_asserts,optimized_stdlib

// This is an end-to-end test of the Array.append(contentsOf:) ->
// Array.append(Element) optimization.
//
// To check that the optimization produces the expected
// Array.append(Element) calls, the CHECK lines match those call
// sites. The optimizer may subsequently inline Array.append(Element),
// which is good, but to keep the test simple and specific to the
// optimization, the RUN line prevents inlining Array.append(Element).
// Likewise, negative tests check for the existence of
// Array.append(contentsOf:), so don't inline those either.

// CHECK-LABEL: sil {{.*}}@{{.*}}testInt
// CHECK-NOT: apply
// CHECK:        [[F:%[0-9]+]] = function_ref @$sSa6appendyyxnFSi_Tg5
// CHECK-NOT: apply
// CHECK:        apply [[F]]
// CHECK-NEXT:   tuple
// CHECK-NEXT:   return
public func testInt(_ a: inout [Int]) {
  a += [1]
}

// CHECK-LABEL: sil {{.*}}@{{.*}}testThreeInts
// CHECK-DAG:    [[F:%[0-9]+]] = function_ref @$sSa6appendyyxnFSi_Tg5
// CHECK-DAG:    apply [[F]]
// CHECK-DAG:    apply [[F]]
// CHECK-DAG:    apply [[F]]
// CHECK:      } // end sil function '{{.*}}testThreeInts{{.*}}'
public func testThreeInts(_ a: inout [Int]) {
  a += [1, 2, 3]
}

// CHECK-LABEL: sil {{.*}}@{{.*}}testTooManyInts
// CHECK-NOT: apply
// CHECK:        [[F:%[0-9]+]] = function_ref  @${{.*append.*contentsOf.*}}
// CHECK-NOT: apply
// CHECK:        apply [[F]]
// CHECK-NOT: apply
// CHECK:        return
public func testTooManyInts(_ a: inout [Int]) {
  a += [1, 2, 3, 4, 5, 6, 7]
}

// CHECK-LABEL: sil {{.*}}@{{.*}}testString
// CHECK-NOT: apply
// CHECK:        [[F:%[0-9]+]] = function_ref @$sSa6appendyyxnFSS_Tg5
// CHECK-NOT: apply
// CHECK:        apply [[F]]
// CHECK-NOT: apply
// CHECK:        tuple
// CHECK-NEXT:   return
public func testString(_ a: inout [String], s: String) {
  a += [s]
}

// This is not supported yet. Just check that we don't crash on this.`
public func dontPropagateContiguousArray(_ a: inout ContiguousArray<UInt8>) {
  a += [4]
}

// Check if the specialized Array.append<A>(contentsOf:) is reasonably optimized for Array<Int>.

// CHECK-LABEL: sil shared {{.*}}@$sSa6append10contentsOfyqd__n_t7ElementQyd__RszSTRd__lFSi_SaySiGTg5

// There should only be a single call to _createNewBuffer or reserveCapacityForAppend/reserveCapacityImpl.

// CHECK-NOT: apply
// CHECK: [[F:%[0-9]+]] = function_ref @{{.*(_consumeAndCreateNew|reserveCapacity).*}}
// CHECK: apply [[F]]
// CHECK-NOT: apply

// CHECK-LABEL: } // end sil function '$sSa6append10contentsOfyqd__n_t7ElementQyd__RszSTRd__lFSi_SaySiGTg5

