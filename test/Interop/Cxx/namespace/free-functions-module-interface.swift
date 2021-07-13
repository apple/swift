// RUN: %target-swift-ide-test -print-module -module-to-print=FreeFunctions -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s

// CHECK-NOT: extension
// CHECK: extension FunctionsNS1 {
// CHECK:   static func basicFunctionTopLevel() -> UnsafePointer<CChar>!
// CHECK:   static func forwardDeclared() -> UnsafePointer<CChar>!
// CHECK:   static func definedOutOfLine() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS1.FunctionsNS2 {
// CHECK:   static func basicFunctionSecondLevel() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS1.FunctionsNS2.FunctionsNS3 {
// CHECK:   static func basicFunctionLowestLevel() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS1 {
// CHECK:   static func definedInDefs() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS1 {
// CHECK:   static func sameNameInChild() -> UnsafePointer<CChar>!
// CHECK:   static func sameNameInSibling() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS1.FunctionsNS2 {
// CHECK:   static func sameNameInChild() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension FunctionsNS4 {
// CHECK:   static func sameNameInSibling() -> UnsafePointer<CChar>!
// CHECK: }
// CHECK-NOT: extension
