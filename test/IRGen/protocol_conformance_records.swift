// RUN: %target-swift-frontend -primary-file %s -emit-ir -enable-resilience -enable-source-import -I %S/../Inputs | %FileCheck %s
// RUN: %target-swift-frontend %s -emit-ir -num-threads 8 -enable-resilience -enable-source-import -I %S/../Inputs | %FileCheck %s

import resilient_struct

public protocol Runcible {
  func runce()
}

// CHECK-LABEL: @"\01l_protocol_conformances" = private constant [

// CHECK:         %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[RUNCIBLE:@_T028protocol_conformance_records8RuncibleMp]]
// -- type metadata
// CHECK-SAME:           @_T028protocol_conformance_records15NativeValueTypeVMn
// -- witness table
// CHECK-SAME:           @_T028protocol_conformance_records15NativeValueTypeVAA8RuncibleAAWP
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         },
public struct NativeValueType: Runcible {
  public func runce() {}
}

// CHECK-SAME:         %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[RUNCIBLE]]
// -- class metadata
// CHECK-SAME:           @_T028protocol_conformance_records15NativeClassTypeCMn
// -- witness table
// CHECK-SAME:           @_T028protocol_conformance_records15NativeClassTypeCAA8RuncibleAAWP
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         },
public class NativeClassType: Runcible {
  public func runce() {}
}

// CHECK:         %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[RUNCIBLE]]
// -- nominal type descriptor
// CHECK-SAME:           @_T028protocol_conformance_records17NativeGenericTypeVMn
// -- witness table
// CHECK-SAME:           @_T028protocol_conformance_records17NativeGenericTypeVyxGAA8RuncibleAAWP
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         },
public struct NativeGenericType<T>: Runcible {
  public func runce() {}
}

// CHECK-SAME:         %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[RUNCIBLE]]
// -- type metadata
// CHECK-SAME:           @got._T0SiMn
// -- witness table
// CHECK-SAME:           @_T0Si28protocol_conformance_records8RuncibleAAWP
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         }
extension Int: Runcible {
  public func runce() {}
}

// For a resilient struct, reference the NominalTypeDescriptor

// CHECK-SAME:         %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[RUNCIBLE]]
// -- nominal type descriptor
// CHECK-SAME:           @got._T016resilient_struct4SizeVMn
// -- witness table
// CHECK-SAME:           @_T016resilient_struct4SizeV28protocol_conformance_records8RuncibleADWP
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         }

extension Size: Runcible {
  public func runce() {}
}

// TODO: conformances that need lazy initialization
public protocol Spoon { }

// Conditional conformances
// CHECK: %swift.protocol_conformance {
// -- protocol descriptor
// CHECK-SAME:           [[SPOON:@_T028protocol_conformance_records5SpoonMp]]
// -- nominal type descriptor
// CHECK-SAME:           @_T028protocol_conformance_records17NativeGenericTypeVMn
// -- witness table accessor
// CHECK-SAME:           i32 add{{.*}}@_T028protocol_conformance_records17NativeGenericTypeVyxGAA5SpoonA2aERzlWa{{.*}}i32 2),
// -- reserved
// CHECK-SAME:           i32 0
// CHECK-SAME:         }
extension NativeGenericType : Spoon where T: Spoon {
  public func runce() {}
}
