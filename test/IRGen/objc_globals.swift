// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/abi %s -whole-module-optimization -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend(mock-sdk: %clang-importer-sdk) -I %S/Inputs/abi %s -emit-ir | %FileCheck %s
//
// REQUIRES: objc_interop

import gadget
import Foundation

@inline(never)
func blackHole<T>(_ t: T) { }

// CHECK-DAG: @"OBJC_CLASS_$_NSNumber" = external global %struct._class_t
// CHECK-DAG: @"OBJC_CLASS_$_NSString" = external global %struct._class_t
// CHECK-DAG: @"OBJC_CLASSLIST_REFERENCES_$_{{.*}}" = internal global ptr @"OBJC_CLASS_$_NSNumber", section "__DATA,__objc_classrefs,regular,no_dead_strip"
// CHECK-DAG: @"OBJC_CLASSLIST_REFERENCES_$_{{.*}}" = internal global ptr @"OBJC_CLASS_$_NSString", section "__DATA,__objc_classrefs,regular,no_dead_strip"

public func testLiterals() {
  blackHole(gadget.giveMeASelector())
  blackHole(gadget.giveMeANumber())
  blackHole(gadget.giveMeAMetaclass())
}

func fooInternal() {
  blackHole(NSString.self as AnyObject)
}

public func fooLazy() {
  fooInternal()
}

// CHECK-LABEL: define internal ptr @giveMeASelector()
// CHECK:         load ptr, ptr @OBJC_SELECTOR_REFERENCES_
// CHECK:         ret

// CHECK-LABEL: define internal ptr @giveMeANumber()
// CHECK:         [[CLASS:%.*]] = load ptr, ptr
// CHECK-DAG:         [[SELECTOR:%.*]] = load ptr, ptr @OBJC_SELECTOR_REFERENCES_.{{.*}}
// CHECK:         call {{.*}} @objc_msgSend
// CHECK:         ret

// CHECK-LABEL: define internal ptr @giveMeAMetaclass()
// CHECK:         [[CLASS:%.*]] = load ptr, ptr
// CHECK-DAG:         [[SELECTOR:%.*]] = load ptr, ptr @OBJC_SELECTOR_REFERENCES_
// CHECK:         call {{.*}} @objc_msgSend
// CHECK:         ret
