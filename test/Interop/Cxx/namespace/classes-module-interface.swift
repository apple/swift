// RUN: %target-swift-ide-test -print-module -module-to-print=Classes -I %S/Inputs -source-filename=x -enable-cxx-interop | %FileCheck %s

// CHECK-NOT: extension
// CHECK: extension ClassesNS1 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK:   struct ForwardDeclaredStruct {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension ClassesNS1.ClassesNS2 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK:   struct ForwardDeclaredStruct {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK: }
// CHECK-NOT: extension

// CHECK: extension ClassesNS3 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:     mutating func basicMember() -> UnsafePointer<CChar>!
// CHECK:   }
// CHECK: }
// CHECK-NOT: extension

// CHECK: typealias GlobalAliasToNS1 = ClassesNS1
// CHECK: extension ClassesNS4 {
// CHECK:   typealias AliasToGlobalNS1 = ClassesNS1
// CHECK:   typealias AliasToGlobalNS2 = ClassesNS1.ClassesNS2
// CHECK:   typealias AliasToInnerNS5 = ClassesNS4.ClassesNS5
// CHECK:   typealias AliasToNS2 = ClassesNS1.ClassesNS2
// CHECK:   typealias AliasChainToNS1 = ClassesNS1
// CHECK:   typealias AliasChainToNS2 = ClassesNS1.ClassesNS2
// CHECK: }
// CHECK: extension ClassesNS4.ClassesNS5 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:   }
// CHECK: }
// CHECK: extension ClassesNS5 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:   }
// CHECK:   typealias AliasToAnotherNS5 = ClassesNS4.ClassesNS5
// CHECK:   typealias AliasToGlobalNS5 = ClassesNS5
// CHECK:   typealias AliasToLocalNS5 = ClassesNS5.ClassesNS5
// CHECK:   typealias AliasToNS5 = ClassesNS5.ClassesNS5
// CHECK: }
// CHECK: extension ClassesNS5.ClassesNS5 {
// CHECK:   struct BasicStruct {
// CHECK:     init()
// CHECK:   }
// CHECK:   typealias AliasToNS5NS5 = ClassesNS5.ClassesNS5
// CHECK: }
// CHECK-NOT: extension
