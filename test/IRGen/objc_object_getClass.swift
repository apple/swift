// RUN: %target-swift-frontend -emit-ir -primary-file %s -module-name A | %FileCheck %s

// REQUIRES: CPU=armv7k
// REQUIRES: OS=watchos

import Foundation

func getAClass(managedObjectClass : AnyClass) -> AnyClass{
  let metaClass: AnyClass = object_getClass(managedObjectClass)!
  return metaClass
}

public class ObjCSubclass : NSObject {
  public final var field: Int32 = 0
}

func test(_ o: ObjCSubclass) {
  o.field = 10
}

// CHECK-DAG: declare ptr @object_getClass(ptr{{.*}})
// CHECK-DAG: call ptr @object_getClass(ptr %{{.*}})
