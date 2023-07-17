// RUN: %target-swift-ide-test -print-module -module-to-print=Fields -I %S/Inputs -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// CHECK:      struct HasThreeFields {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(a: Int32, b: Int32, c: Int32)
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct DerivedWithSameField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct DerivedWithOneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var d: Int32
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct HasOneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(e: Int32)
// CHECK-NEXT:   var e: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct DerivedFromAll {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var f: Int32
// CHECK-NEXT:   var e: Int32
// CHECK-NEXT:   var d: Int32
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct OneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   init(value: Int32)
// CHECK-NEXT:   var value: Int32
// CHECK-NEXT: }
// CHECK-NEXT: struct DerivedFromOneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var value: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct NonTrivial {
// CHECK-NEXT:   init()
// CHECK-NEXT: }

// CHECK-NEXT: struct NonTrivialHasThreeFields {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct NonTrivialDerivedWithOneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var d: Int32
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct NonTrivialHasOneField {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var e: Int32
// CHECK-NEXT: }

// CHECK-NEXT: struct NonTrivialDerivedFromAll {
// CHECK-NEXT:   init()
// CHECK-NEXT:   var f: Int32
// CHECK-NEXT:   var e: Int32
// CHECK-NEXT:   var d: Int32
// CHECK-NEXT:   var a: Int32
// CHECK-NEXT:   var b: Int32
// CHECK-NEXT:   var c: Int32
// CHECK-NEXT: }
