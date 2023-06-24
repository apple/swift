// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -module-name main %s -emit-ir | %FileCheck %s --check-prefix=CHECK-CACHE
// RUN: %target-swift-frontend -module-name main %s -emit-ir -disable-preallocated-instantiation-caches | %FileCheck %s --check-prefix=CHECK-NOCACHE


public class Generic<T> {
  public func m1(t: T) -> T { return t }
  public func m2(t: T) -> T { return t }
}

protocol MyProtocol {
  associatedtype T
  func foo() -> T
}

public struct MyStruct<T>: MyProtocol {
  func foo() -> T { fatalError() }
}

// "metadata instantiation cache for protocol conformance descriptor for main.MyStruct<A> : main.MyProtocol in main"
// CHECK-CACHE: @"$s4main8MyStructVyxGAA0B8ProtocolAAMcMK" = internal global [{{.*}} x ptr] zeroinitializer
// CHECK-CACHE: @"$s4main8MyStructVyxGAA0B8ProtocolAAMc" = {{.*}} @"$s4main8MyStructVyxGAA0B8ProtocolAAMcMK" {{.*}}
// CHECK-NOCACHE-NOT: @"$s4main8MyStructVyxGAA0B8ProtocolAAMcMK"

// "type metadata instantiation cache for main.Generic"
// CHECK-CACHE: @"$s4main7GenericCMI" = internal global [{{.*}} x ptr] zeroinitializer
// CHECK-CACHE: @"$s4main7GenericCMn" = {{.*}} @"$s4main7GenericCMI" {{.*}}
// CHECK-NOCACHE-NOT: @"$s4main7GenericCMI"

// "type metadata instantiation cache for main.MyStruct"
// CHECK-CACHE: @"$s4main8MyStructVMI" = internal global [{{.*}} x ptr] zeroinitializer
// CHECK-CACHE: @"$s4main8MyStructVMn" = {{.*}} @"$s4main8MyStructVMI" {{.*}} 
// CHECK-NOCACHE-NOT: @"$s4main8MyStructVMI"
