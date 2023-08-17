// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -module-name cast_folding_objc_generics -O -Xllvm -sil-disable-pass=FunctionSignatureOpts -Xllvm -sil-disable-pass=PerfInliner -emit-sil %s | %FileCheck %s

// REQUIRES: objc_interop

import objc_generics

// CHECK-LABEL: sil [noinline] {{.*}}@$s26cast_folding_objc_generics26testObjCGenericParamChangeySo12GenericClassCySo8NSStringCGADySo15NSMutableStringCGF : $@convention(thin) (@guaranteed GenericClass<NSMutableString>) -> @owned GenericClass<NSString> {
// CHECK:         upcast
// CHECK-NOT:     cond_fail
// CHECK: } // end sil function '$s26cast_folding_objc_generics26testObjCGenericParamChangeySo12GenericClassCySo8NSStringCGADySo15NSMutableStringCGF'
@inline(never)
public func testObjCGenericParamChange(_ a: GenericClass<NSMutableString>) -> GenericClass<NSString> {
  return a as! GenericClass<NSString>
}

// CHECK-LABEL: sil [noinline] @$s26cast_folding_objc_generics34testObjCGenericParamChangeSubclassySo07GenericJ0CySo8NSStringCGSo0K5ClassCySo15NSMutableStringCGF : $@convention(thin) (@guaranteed GenericClass<NSMutableString>) -> @owned GenericSubclass<NSString> {
// CHECK:         unconditional_checked_cast
// CHECK-NOT:     cond_fail
// CHECK: } // end sil function '$s26cast_folding_objc_generics34testObjCGenericParamChangeSubclassySo07GenericJ0CySo8NSStringCGSo0K5ClassCySo15NSMutableStringCGF'
@inline(never)
public func testObjCGenericParamChangeSubclass(_ a: GenericClass<NSMutableString>) -> GenericSubclass<NSString> {
  return a as! GenericSubclass<NSString>
}

// CHECK-LABEL: sil [noinline] {{.*}}@$s26cast_folding_objc_generics36testObjCGenericParamChangeSuperclassySo12GenericClassCySo8NSStringCGSo0K8SubclassCySo15NSMutableStringCGF : $@convention(thin) (@guaranteed GenericSubclass<NSMutableString>) -> @owned GenericClass<NSString> {
// CHECK:         upcast
// CHECK-NOT:     cond_fail
// CHECK: } // end sil function '$s26cast_folding_objc_generics36testObjCGenericParamChangeSuperclassySo12GenericClassCySo8NSStringCGSo0K8SubclassCySo15NSMutableStringCGF'
@inline(never)
public func testObjCGenericParamChangeSuperclass(_ a: GenericSubclass<NSMutableString>) -> GenericClass<NSString> {
  return a as! GenericClass<NSString>
}
