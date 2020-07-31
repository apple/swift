// RUN: %target-build-swift -O %s -module-name=test -Xfrontend -sil-verify-all -emit-sil | %FileCheck %s

// RUN: %empty-directory(%t) 
// RUN: %target-build-swift -O -module-name=test %s -o %t/a.out
// RUN: %target-run %t/a.out | %FileCheck %s -check-prefix=CHECK-OUTPUT

// REQUIRES: executable_test,swift_stdlib_no_asserts

#if _runtime(_ObjC)
import Foundation
#endif

struct Outer {
  struct Inner { }
}

// More types are tested in test/stdlib/TypeName.swift and
// test/stdlib/TypeNameInterpolation.swift

// CHECK-LABEL: sil [noinline] @$s4test0A21TypeNameInterpolationSSyF
// CHECK-NOT: apply
// CHECK-NOT: bb1
// CHECK: } // end sil function '$s4test0A21TypeNameInterpolationSSyF'
@inline(never)
public func testTypeNameInterpolation() -> String {
  return "-\(Outer.Inner.self)+"
}

// CHECK-LABEL: sil [noinline] @$s4test0A25FoldCompleteInterpolationSSyF 
// CHECK-NOT: apply
// CHECK-NOT: bb1
// CHECK: } // end sil function '$s4test0A25FoldCompleteInterpolationSSyF'
@inline(never)
public func testFoldCompleteInterpolation() -> String {
  let s = "is"
  return "-\([Int].self) \(s) \("cool")+"
}

// CHECK-LABEL: sil [noinline] @$s4test0A19UnqualifiedTypeNameSSyF 
// CHECK-NOT: apply
// CHECK-NOT: bb1
// CHECK: } // end sil function '$s4test0A19UnqualifiedTypeNameSSyF'
@inline(never)
public func testUnqualifiedTypeName() -> String {
  return _typeName(Outer.Inner.self, qualified: false)
}

// CHECK-LABEL: sil [noinline] @$s4test0A17QualifiedTypeNameSSyF 
// CHECK-NOT: apply
// CHECK-NOT: bb1
// CHECK: } // end sil function '$s4test0A17QualifiedTypeNameSSyF'
@inline(never)
public func testQualifiedTypeName() -> String {
  return _typeName(Outer.Inner.self, qualified: true)
}

// CHECK-LABEL: sil [noinline] @$s4test0A20UnqualifiedLocalTypeSSyF
// CHECK-NOT: apply
// CHECK-NOT: bb1
// CHECK: } // end sil function '$s4test0A20UnqualifiedLocalTypeSSyF'
@inline(never)
public func testUnqualifiedLocalType() -> String {
  struct LocalStruct { }
  return _typeName(LocalStruct.self, qualified: false)
}

// CHECK-LABEL: sil [noinline] @$s4test0A18QualifiedLocalTypeSSyF
// CHECK:    [[F:%[0-9]+]] = function_ref @$ss9_typeName_9qualifiedSSypXp_SbtF
// CHECK:    apply [[F]]
// CHECK: } // end sil function '$s4test0A18QualifiedLocalTypeSSyF'
@inline(never)
public func testQualifiedLocalType() -> String {
  struct LocalStruct { }
  return _typeName(LocalStruct.self, qualified: true)
}

#if _runtime(_ObjC)
@inline(never)
public func testObjcClassName(qualified: Bool) -> String {
  return _typeName(NSObject.self, qualified: qualified)
}
#endif

@inline(never)
func printEmbeeded(_ s: String) {
  print("<\(s)>")
}

// CHECK-OUTPUT: <-Inner+>
printEmbeeded(testTypeNameInterpolation())

// CHECK-OUTPUT: <-Array<Int> is cool+>
printEmbeeded(testFoldCompleteInterpolation())

// CHECK-OUTPUT: <Inner>
printEmbeeded(testUnqualifiedTypeName())

// CHECK-OUTPUT: <test.Outer.Inner>
printEmbeeded(testQualifiedTypeName())

// CHECK-OUTPUT: <LocalStruct>
printEmbeeded(testUnqualifiedLocalType())

// CHECK-OUTPUT: <test.(unknown context at {{.*}}).LocalStruct>
printEmbeeded(testQualifiedLocalType())

#if _runtime(_ObjC)
// CHECK-OUTPUT: <NSObject>
printEmbeeded(testObjcClassName(qualified: false))
// CHECK-OUTPUT: <NSObject>
printEmbeeded(testObjcClassName(qualified: true))
#endif

