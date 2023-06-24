// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/has_symbol_helper.swiftmodule -parse-as-library %S/Inputs/has_symbol/has_symbol_helper.swift -enable-library-evolution -disable-availability-checking -DCONCURRENCY
// RUN: %target-swift-frontend -emit-irgen %s -I %t -module-name test | %FileCheck %s

// REQUIRES: concurrency
// UNSUPPORTED: OS=windows-msvc

@_weakLinked import has_symbol_helper

public func testGlobalFunctions() {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper9asyncFuncyyYaFTwS"()
  if #_hasSymbol(asyncFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper12isolatedFuncyyFTwS"()
  if #_hasSymbol(isolatedFunc) {}
}

// --- asyncFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper9asyncFuncyyYaFTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper9asyncFuncyyYaF", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper9asyncFuncyyYaFTu", ptr null))

// --- isolatedFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper12isolatedFuncyyFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper12isolatedFuncyyF", ptr null)

public func testActor(_ a: A) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1ACACycfcTwS"()
  if #_hasSymbol(A.init) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1AC11asyncMethodyyYaFTwS"()
  if #_hasSymbol(a.asyncMethod) {}

  // FIXME: Add support for actor isolated methods
}

// --- A.init() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1ACACycfcTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1ACACycfc", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1ACACycfC", ptr null))

// --- A.asyncMethod() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1AC11asyncMethodyyYaFTwS"()
// CHECK:   ret i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1AC11asyncMethodyyYaFTj", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1AC11asyncMethodyyYaFTjTu", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper1AC11asyncMethodyyYaFTq", ptr null))
