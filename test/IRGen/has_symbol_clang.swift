// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-irgen %s -I %t -I %S/Inputs/has_symbol -module-name test | %FileCheck %s

// UNSUPPORTED: OS=windows-msvc

@_weakLinked import has_symbol_helper_clang

public func testClangDecls() {
  // CHECK: %{{[0-9]+}} = call i1 @"$sSo9clangFuncyys5Int32VFTwS"()
  if #_hasSymbol(clangFunc(_:)) {}
}

// --- clangFunc(_:) ---
// CHECK: define linkonce_odr hidden i1 @"$sSo9clangFuncyys5Int32VFTwS"() #1 {
// CHECK:   ret i1 icmp ne (ptr @clangFunc, ptr null)
