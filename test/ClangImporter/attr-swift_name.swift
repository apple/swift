// RUN: %empty-directory(%t.mcp)
// RUN: not %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/custom-modules -Xcc -w -typecheck %s -module-cache-path %t.mcp 2>&1 | %FileCheck %s

// REQUIRES: objc_interop

// CHECK-NOT: warning:

import ObjCIRExtras

func test(_ i: Int) {
  let t = SwiftNameTest()
  
  t.theMethod(number: i)

  _ = t.renamedSomeProp
  _ = type(of: t).renamedClassProp

  _ = circularFriends(_:_:)

  // We only see these five warnings because Clang can catch the other invalid
  // cases, and marks the attribute as invalid ahead of time.
  
  // CHECK: warning: too few parameters in swift_name attribute (expected 2; got 1)
  // CHECK: + (instancetype)g:(id)x outParam:(int *)foo SWIFT_NAME(init(g:));
  // CHECK-NOT: warning:
  // CHECK: note: please report this issue to the owners of 'ObjCIRExtras'
  // CHECK-NOT: warning:

  // CHECK: warning: cycle detected while resolving 'CircularName' in swift_name attribute for 'CircularName'
  // CHECK: SWIFT_NAME(CircularName.Inner) @interface CircularName : NSObject @end
  // CHECK-NOT: {{warning|note}}:
  // CHECK: note: please report this issue to the owners of 'ObjCIRExtras'
  // CHECK-NOT: warning:

  // CHECK: warning: cycle detected while resolving 'MutuallyCircularNameB' in swift_name attribute for 'MutuallyCircularNameA'
  // CHECK: SWIFT_NAME(MutuallyCircularNameB.Inner) @interface MutuallyCircularNameA : NSObject @end
  // CHECK-NOT: {{warning|note}}:
  // CHECK: note: while resolving 'MutuallyCircularNameA' in swift_name attribute for 'MutuallyCircularNameB'
  // CHECK: SWIFT_NAME(MutuallyCircularNameA.Inner) @interface MutuallyCircularNameB : NSObject @end
  // CHECK-NOT: {{warning|note}}:
  // CHECK: note: please report this issue to the owners of 'ObjCIRExtras'
  // CHECK-NOT: {{warning|note}}:
  // CHECK: SWIFT_NAME(MutuallyCircularNameB.Inner) @interface MutuallyCircularNameA : NSObject @end
}
