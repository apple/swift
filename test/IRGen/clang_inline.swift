// RUN: %empty-directory(%t)
// RUN: %build-irgen-test-overlays
// RUN: %target-swift-frontend -enable-objc-interop -sdk %S/Inputs -primary-file %s -O -disable-sil-perf-optzns -disable-llvm-optzns -emit-ir -Xcc -fstack-protector -I %t | %FileCheck %s

// RUN: %empty-directory(%t/Empty.framework/Modules/Empty.swiftmodule)
// RUN: %target-swift-frontend -emit-module-path %t/Empty.framework/Modules/Empty.swiftmodule/%target-swiftmodule-name %S/../Inputs/empty.swift -module-name Empty -I %t
// RUN: %target-swift-frontend -sdk %S/Inputs -primary-file %s -I %t -F %t -DIMPORT_EMPTY -O -disable-sil-perf-optzns -disable-llvm-optzns -emit-ir -Xcc -fstack-protector -enable-objc-interop | %FileCheck %s

// REQUIRES: CPU=i386 || CPU=x86_64
// REQUIRES: objc_interop

#if IMPORT_EMPTY
  import Empty
#endif

import gizmo

// CHECK-LABEL: define hidden swiftcc i64 @"$s12clang_inline16CallStaticInlineC10ReturnZeros5Int64VyF"(ptr swiftself %0) {{.*}} {
class CallStaticInline {
  func ReturnZero() -> Int64 { return Int64(zero()) }
}

// CHECK-LABEL: define internal i32 @zero()
// CHECK-SAME:         [[INLINEHINT_SSP_UWTABLE:#[0-9]+]] {

// CHECK-LABEL: define hidden swiftcc i64 @"$s12clang_inline17CallStaticInline2C10ReturnZeros5Int64VyF"(ptr swiftself %0) {{.*}} {
class CallStaticInline2 {
  func ReturnZero() -> Int64 { return Int64(wrappedZero()) }
}

// CHECK-LABEL: define internal i32 @wrappedZero()
// CHECK-SAME:         [[INLINEHINT_SSP_UWTABLE:#[0-9]+]] {

// CHECK-LABEL: define hidden swiftcc i32 @"$s12clang_inline10testExterns5Int32VyF"() {{.*}} {
func testExtern() -> CInt {
  return wrappedGetInt()
}

// CHECK-LABEL: define internal i32 @wrappedGetInt()
// CHECK-SAME:         [[INLINEHINT_SSP_UWTABLE:#[0-9]+]] {

// CHECK-LABEL: define hidden swiftcc i32 @"$s12clang_inline16testAlwaysInlines5Int32VyF"() {{.*}} {
func testAlwaysInline() -> CInt {
  return alwaysInlineNumber()
}

// CHECK-LABEL: define internal i32 @alwaysInlineNumber()
// CHECK-SAME:         [[ALWAYS_INLINE:#[0-9]+]] {

// CHECK-LABEL: define hidden swiftcc i32 @"$s12clang_inline20testInlineRedeclareds5Int32VyF"() {{.*}} {
func testInlineRedeclared() -> CInt {
  return zeroRedeclared()
}

// CHECK-LABEL: define internal i32 @zeroRedeclared() #{{[0-9]+}} {

// CHECK-LABEL: define hidden swiftcc i32 @"$s12clang_inline27testInlineRedeclaredWrappeds5Int32VyF"() {{.*}} {
func testInlineRedeclaredWrapped() -> CInt {
  return wrappedZeroRedeclared()
}

// CHECK-LABEL: define internal i32 @wrappedZeroRedeclared() #{{[0-9]+}} {

// CHECK-LABEL: define hidden swiftcc i32 @"$s12clang_inline22testStaticButNotInlines5Int32VyF"() {{.*}} {
func testStaticButNotInline() -> CInt {
  return staticButNotInline()
}

// CHECK-LABEL: define internal i32 @staticButNotInline() #{{[0-9]+}} {

// CHECK-LABEL: define internal i32 @innerZero()
// CHECK-SAME:         [[INNER_ZERO_ATTR:#[0-9]+]] {
// CHECK-LABEL: declare i32 @getInt()
// CHECK-SAME:         [[GET_INT_ATTR:#[0-9]+]]

// CHECK-DAG: attributes [[INLINEHINT_SSP_UWTABLE]] = { inlinehint optsize ssp {{.*}}}
// CHECK-DAG: attributes [[ALWAYS_INLINE]] = { alwaysinline nounwind optsize ssp
// CHECK-DAG: attributes [[INNER_ZERO_ATTR]] = { inlinehint nounwind optsize ssp
// CHECK-DAG: attributes [[GET_INT_ATTR]] = {
