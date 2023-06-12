// Test that Objective-C types passed to a C++ constructor are bridged
// correctly.

// RUN: %target-swift-ide-test(mock-sdk: %clang-importer-sdk) -print-module -module-to-print=ConstructorsObjC -I %S/Inputs/ -source-filename=x -enable-experimental-cxx-interop | %FileCheck %s

// REQUIRES: objc_interop

// CHECK:      struct ConstructorWithNSArrayParam {
// CHECK-NEXT:   init(_ array: [Any]!)
// CHECK-NEXT:   @available(*, deprecated, message
// CHECK-NEXT:   init()
// CHECK-NEXT: }
