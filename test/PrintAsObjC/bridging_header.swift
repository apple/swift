// REQUIRES: objc_interop

// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-pch -o %t/bridging-header.pch %S/Inputs/bridging_header-Bridging-Header.h
// RUN: %target-swift-frontend(mock-sdk: -sdk %S/../Inputs/clang-importer-sdk -I %t) -emit-module -I %S/Inputs/custom-modules -import-objc-header %t/bridging-header.pch -import-underlying-module -o %t %s -disable-objc-attr-requires-foundation-module -emit-objc-header-path %t/bridging_header-Swift.h

// RUN: %FileCheck %s --input-file %t/bridging_header-Swift.h

// CHECK: bridging_header-Bridging-Header.h
// CHECK-NOT: bridging-header.pch

@objc class Test : NSObject, TestProto {
  var strongProp: Any?
}
