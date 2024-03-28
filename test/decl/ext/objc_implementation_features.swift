// REQUIRES: objc_interop

// RUN: not %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -typecheck %s -import-objc-header %S/Inputs/objc_implementation.h 2>&1 | %FileCheck --check-prefixes NO,CHECK %s
// RUN: not %target-swift-frontend(mock-sdk: %clang-importer-sdk-nosource) -typecheck %s -import-objc-header %S/Inputs/objc_implementation.h -enable-experimental-feature ObjCImplementation 2>&1 | %FileCheck --check-prefixes YES,CHECK %s

// NO-NOT: objc_implementation_features.swift:[[@LINE+2]]:{{[0-9]+}}: warning: '@_objcImplementation' is deprecated; use '@implementation' instead
// YES-DAG: objc_implementation_features.swift:[[@LINE+1]]:{{[0-9]+}}: warning: '@_objcImplementation' is deprecated; use '@implementation' instead
@_objcImplementation(EmptyCategory) extension ObjCClass {}

// CHECK-DAG: objc_implementation_features.swift:[[@LINE+2]]:{{[0-9]+}}: warning: extension for main class interface should provide implementation for instance method 'subclassMethod(fromHeader1:)'; this will become an error after adopting '@implementation'
// CHECK-NOT: objc_implementation_features.swift:[[@LINE+1]]:{{[0-9]+}}: warning: '@_objcImplementation' is deprecated; use '@implementation' instead
@_objcImplementation extension ObjCSubclass {}

// CHECK-DAG: objc_implementation_features.swift:[[@LINE+3]]:{{[0-9]+}}: error: extension for main class interface should provide implementation for initializer 'init()'{{$}}
// NO-DAG: objc_implementation_features.swift:[[@LINE+2]]:{{[0-9]+}}: error: 'implementation' attribute is only valid when experimental feature ObjCImplementation is enabled
// YES-NOT: objc_implementation_features.swift:[[@LINE+1]]:{{[0-9]+}}: error: 'implementation' attribute is only valid when experimental feature ObjCImplementation is enabled
@implementation extension ObjCBasicInitClass {}
