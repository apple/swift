// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -o %t -module-name Lib -I %S/Inputs/custom-modules %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules | %FileCheck %s

// RUN: %target-swift-ide-test -source-filename=x -print-module -module-to-print Lib -I %t -I %S/Inputs/custom-modules -Xcc -DBAD > %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY %s < %t.txt
// RUN: %FileCheck -check-prefix CHECK-RECOVERY-NEGATIVE %s < %t.txt

// REQUIRES: objc_interop

import TypeRemovalObjC

// CHECK-DAG: let simple: AnyObject?
// CHECK-RECOVERY-DAG: let simple: AnyObject?
public let simple: AnyObject? = nil

// CHECK-DAG: let someObject: Base?
// CHECK-RECOVERY-NEGATIVE-NOT: let someObject:
public let someObject: Base? = nil
// CHECK-DAG: let someGenericObject: Generic<AnyObject>?
// CHECK-RECOVERY-NEGATIVE-NOT: let someGenericObject:
public let someGenericObject: Generic<AnyObject>? = nil
// CHECK-DAG: let someProtoValue: (any SomeProto)?
// CHECK-RECOVERY-NEGATIVE-NOT: let someProtoValue:
public let someProtoValue: SomeProto? = nil
// CHECK-DAG: let someProtoType: (any SomeProto.Type)?
// CHECK-RECOVERY-NEGATIVE-NOT: let someProtoType:
public let someProtoType: SomeProto.Type? = nil
// CHECK-DAG: let someProtoCompositionValue: (any AProto & SomeProto)?
// CHECK-RECOVERY-NEGATIVE-NOT: let someProtoCompositionValue:
public let someProtoCompositionValue: (AProto & SomeProto)? = nil
// CHECK-DAG: let someProtoCompositionValue2: (any SomeProto & ZProto)?
// CHECK-RECOVERY-NEGATIVE-NOT: let someProtoCompositionValue2:
public let someProtoCompositionValue2: (SomeProto & ZProto)? = nil

// CHECK-DAG: let someTypedefValue: SomeTypedef
// CHECK-RECOVERY-DAG: let someTypedefValue: Int64
public let someTypedefValue: SomeTypedef = 0
// CHECK-DAG: let someTypedefOptValue: SomeTypedef?
// CHECK-RECOVERY-DAG: let someTypedefOptValue: Int64?
public let someTypedefOptValue: SomeTypedef? = nil

// CHECK-DAG: unowned var someUnownedObject: @sil_unowned Base
// CHECK-RECOVERY-NEGATIVE-NOT: var someUnownedObject:
public unowned var someUnownedObject: Base = Base()
// CHECK-DAG: unowned(unsafe) var someUnownedUnsafeObject: @sil_unmanaged Base
// CHECK-RECOVERY-NEGATIVE-NOT: var someUnownedUnsafeObject:
public unowned(unsafe) var someUnownedUnsafeObject: Base = Base()
// CHECK-DAG: weak var someWeakObject: @sil_weak Base
// CHECK-RECOVERY-NEGATIVE-NOT: var someWeakObject:
public weak var someWeakObject: Base? = nil

// CHECK-DAG: struct GenericStruct<T>
// CHECK-RECOVERY-DAG: struct GenericStruct<T>
struct GenericStruct<T> {}

// CHECK-DAG: extension GenericStruct where T : SomeProto
// CHECK-RECOVERY-NEGATIVE-NOT: extension GenericStruct{{.*}}SomeProto
extension GenericStruct where T: SomeProto {
  // CHECK-DAG: func someOperation
  // CHECK-RECOVERY-NEGATIVE-NOT: someOperation
  func someOperation() {}
}
