// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -emit-module-path %t/has_symbol_helper.swiftmodule -parse-as-library %S/Inputs/has_symbol/has_symbol_helper.swift -enable-library-evolution -disable-availability-checking
// RUN: %target-swift-frontend -emit-irgen %s -I %t -I %S/Inputs/has_symbol -module-name test | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc

@_weakLinked import has_symbol_helper

// Match the target word size so that we can use it throughout the test
// CHECK: %swift.type = type { [[WORD:i32|i64]] }

public func testGlobalFunctions() {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper8function4withySi_tFTwS"()
  if #_hasSymbol(function(with:)) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper12throwingFuncyyKFTwS"()
  if #_hasSymbol(throwingFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper11genericFuncyyxAA1PRzlFTwS"()
  if #_hasSymbol(genericFunc(_:) as (S) -> Void) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper20funcWithOpaqueResultQryFTwS"()
  if #_hasSymbol(funcWithOpaqueResult) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper9cdeclFuncyyFTwS"()
  if #_hasSymbol(cdeclFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper19forwardDeclaredFuncyyFTwS"()
  if #_hasSymbol(forwardDeclaredFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper11dynamicFuncyyFTwS"()
  if #_hasSymbol(dynamicFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper15replacementFuncyyFTwS"()
  if #_hasSymbol(replacementFunc) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFTwS"()
  if #_hasSymbol(dynamicFuncOpaqueResult) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFTwS"()
  if #_hasSymbol(replacementFuncOpaqueResult) {}
}

// --- function(with:) ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper8function4withySi_tFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper8function4withySi_tF", ptr null)

// --- throwingFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper12throwingFuncyyKFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper12throwingFuncyyKF", ptr null)

// --- genericFunc(_:) ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper11genericFuncyyxAA1PRzlFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper11genericFuncyyxAA1PRzlF", ptr null)

// --- funcWithOpaqueResult() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper20funcWithOpaqueResultQryFTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper20funcWithOpaqueResultQryFQOMQ", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper20funcWithOpaqueResultQryF", ptr null))

// --- cdeclFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper9cdeclFuncyyFTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper9cdeclFuncyyF", ptr null), i1 icmp ne (ptr @cdecl_func, ptr null))

// --- forwardDeclaredFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper19forwardDeclaredFuncyyFTwS"()
// CHECK:   ret i1 icmp ne (ptr @forward_declared_func, ptr null)

// --- dynamicFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper11dynamicFuncyyFTwS"()
// CHECK:   ret i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper11dynamicFuncyyF", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper11dynamicFuncyyFTX", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper11dynamicFuncyyFTx", ptr null))

// --- replacementFunc() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper15replacementFuncyyFTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper15replacementFuncyyF", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper15replacementFuncyyFTX", ptr null))

// --- dynamicFuncOpaqueResult() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFTwS"()
// CHECK:   ret i1 and (i1 and (i1 and (i1 and (i1 and (i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFQOMQ", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFQOMg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFQOMh", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFQOMg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFQOMg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryF", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFTX", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper23dynamicFuncOpaqueResultQryFTx", ptr null))

// --- replacementFuncOpaqueResult() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFTwS"()
// CHECK:   ret i1 and (i1 and (i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFQOMQ", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFQOMg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFQOMg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper27replacementFuncOpaqueResultQryF", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper27replacementFuncOpaqueResultQryFTX", ptr null))

public func testVars() {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper6globalSivpTwS"()
  if #_hasSymbol(global) {}
}

// --- global ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper6globalSivpTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper6globalSivg", ptr null)

public func testClass(_ c: C) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1CCACycfcTwS"()
  if #_hasSymbol(C.init) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1CC6method4withySi_tFTwS"()
  if #_hasSymbol(c.method(with:)) {}
}

// --- C.init() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1CCACycfcTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1CCACycfc", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1CCACycfC", ptr null))

// --- C.method(with:) ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1CC6method4withySi_tFTwS"()
// CHECK:   ret i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1CC6method4withySi_tFTj", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1CC6method4withySi_tFTq", ptr null))

public func testStruct(_ s: S) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1SV6memberSivpTwS"()
  if #_hasSymbol(s.member) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1SV6method4withySi_tFTwS"()
  if #_hasSymbol(s.method(with:)) {}
}

// --- S.member ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1SV6memberSivpTwS"()
// CHECK:   ret i1 and (i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1SV6memberSivpMV", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1SV6memberSivg", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper1SV6memberSivs", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper1SV6memberSivM", ptr null))

// --- S.method(with:) ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1SV6method4withySi_tFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper1SV6method4withySi_tF", ptr null)

public func testEnum(_ e: E) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1EO9basicCaseyA2CmFTwS"()
  if #_hasSymbol(E.basicCase) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1EO11payloadCaseyAcA1SVcACmFTwS"()
  if #_hasSymbol(E.payloadCase) {}
}

// --- E.basicCase ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1EO9basicCaseyA2CmFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper1EO9basicCaseyA2CmFWC", ptr null)

// --- E.payloadCase(_:) ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1EO11payloadCaseyAcA1SVcACmFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper1EO11payloadCaseyAcA1SVcACmFWC", ptr null)

public func testOpaqueParameter<T: P>(_ p: T) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1PP11requirementyyFTwS"()
  if #_hasSymbol(p.requirement) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1PP26requirementWithDefaultImplyyFTwS"()
  if #_hasSymbol(p.requirementWithDefaultImpl) {}
}

// --- P.requirement() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1PP11requirementyyFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper1PP11requirementyyF", ptr null)

// --- P.requirementWithDefaultImpl() ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1PP26requirementWithDefaultImplyyFTwS"()
// CHECK:   ret i1 icmp ne (ptr @"$s17has_symbol_helper1PP26requirementWithDefaultImplyyF", ptr null)

public func testExistentialParameter(_ p: any P) {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1PP11requirementyyFTwS"()
  if #_hasSymbol(p.requirement) {}

  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1PP26requirementWithDefaultImplyyFTwS"()
  if #_hasSymbol(p.requirementWithDefaultImpl) {}
}

public func testMetatypes() {
  // CHECK: %{{[0-9]+}} = call i1 @"$s17has_symbol_helper1SVTwS"()
  if #_hasSymbol(S.self) {}
}

// --- S.self ---
// CHECK: define linkonce_odr hidden i1 @"$s17has_symbol_helper1SVTwS"()
// CHECK:   ret i1 and (i1 and (i1 icmp ne (ptr @"$s17has_symbol_helper1SVMn", ptr null), i1 icmp ne (ptr @"$s17has_symbol_helper1SVN", ptr null)), i1 icmp ne (ptr @"$s17has_symbol_helper1SVMa", ptr null))
