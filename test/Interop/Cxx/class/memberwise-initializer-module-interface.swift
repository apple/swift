// RUN: %target-swift-ide-test -print-module -module-to-print=MemberwiseInitializer -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK:      struct StructPrivateOnly {
// CHECK-NEXT:   init()
// CHECK-NEXT: }
// CHECK-NEXT: struct StructPublicOnly {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct StructEmptyPrivateSection {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct StructPublicAndPrivate {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct StructWithUnimportedMemberFunction {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassPrivateOnly {
// CHECK-NEXT:   init()
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassPublicOnly {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassEmptyPublicSection {
// CHECK-NEXT:   init()
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassPrivateAndPublic {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassWithUnimportedMemberFunction {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassWithTemplatedFunction {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct ClassWithTemplatedUsingDecl {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(varPublic: Int32)
// CHECK-NEXT:   var varPublic: Int32
// CHECK-NEXT: }
