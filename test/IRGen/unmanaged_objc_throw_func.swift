// RUN: %target-swift-frontend -emit-ir -enable-copy-propagation -enable-lexical-lifetimes=false %s | %FileCheck %s
// REQUIRES: objc_interop
// REQUIRES: optimized_stdlib

import Foundation

// https://github.com/apple/swift/issues/51538

@objc protocol P {
  func returnUnmanagedCFArray() throws -> Unmanaged<CFArray>
}

// CHECK-LABEL: define hidden swiftcc ptr @"$s25unmanaged_objc_throw_func1CC22returnUnmanagedCFArrays0F0VySo0G3RefaGyKF"
@objc class C: NSObject, P {
  func returnUnmanagedCFArray() throws -> Unmanaged<CFArray> {
    // CHECK: %[[T0:.+]] = call swiftcc { ptr, ptr } @"$ss27_allocateUninitializedArrayySayxG_BptBwlF"(i{{32|64}} 1, ptr @"$sSiN")
    // CHECK-NEXT: %[[T1:.+]] = extractvalue { ptr, ptr } %[[T0]], 0
    // CHECK-NEXT: %[[T2:.+]] = extractvalue { ptr, ptr } %[[T0]], 1
    // CHECK-NEXT: %._value = getelementptr inbounds %TSi, ptr %[[T2]], i32 0, i32 0
    // CHECK:      %[[T7:.+]] = call swiftcc ptr @"$ss27_finalizeUninitializedArrayySayxGABnlF"(ptr %[[T1]], ptr @"$sSiN")
    // CHECK:      %[[T4:.+]] = call swiftcc ptr @"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF"(ptr %[[T7]], ptr @"$sSiN")
    // CHECK-NEXT: store ptr %[[T4]]
    // CHECK-NEXT: call void @swift_bridgeObjectRelease(ptr %{{[0-9]+}}) #{{[0-9]+}}
    // CHECK-NEXT: call void @llvm.objc.release(ptr %[[T4]])
    // CHECK-NEXT: ret ptr %[[T4]]
    let arr = [1] as CFArray
    return Unmanaged.passUnretained(arr) 
  } 
}

// CHECK: %[[T0:.+]] = call swiftcc ptr @"$s25unmanaged_objc_throw_func1CC22returnUnmanagedCFArrays0F0VySo0G3RefaGyKF"
// CHECK-NEXT: %[[T2:.+]] = load ptr, ptr %swifterror, align {{[0-9]+}}
// CHECK-NEXT: %[[T3:.+]] = icmp ne ptr %[[T2]], null
// CHECK-NEXT: br i1 %[[T3]], label %[[L1:.+]], label %[[L2:.+]]

// CHECK: [[L2]]:                                     ; preds = %entry
// CHECK-NEXT: %[[T4:.+]] = phi ptr [ %[[T0]], %entry ]
// CHECK-NEXT: %[[T5:.+]] = ptrtoint ptr %[[T4]] to i{{32|64}}
// CHECK-NEXT: br label %[[L3:.+]]

// CHECK: [[L1]]:                                     ; preds = %entry
// CHECK-NEXT: %[[T6:.+]] = phi ptr [ %[[T2]], %entry ]
// CHECK-NEXT: store ptr null, ptr %swifterror, align {{[0-9]+}}
// CHECK-NEXT: %[[T7:.+]] = icmp eq i{{32|64}} %{{.+}}, 0
// CHECK-NEXT: br i1 %[[T7]], label %[[L4:.+]], label %[[L5:.+]]

// CHECK: [[L5]]:                                     ; preds = %[[L1]]
// CHECK-NEXT: %[[T8:.+]] = inttoptr i{{32|64}} %{{.+}} to ptr
// CHECK-NEXT: br label %[[L6:.+]]

// CHECK: [[L6]]:                                     ; preds = %[[L5]]
// CHECK-NEXT: %[[T9:.+]] = phi ptr [ %[[T8]], %[[L5]] ]
// CHECK-NEXT: %[[T10:.+]] = call swiftcc ptr @"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF"(ptr %[[T6]]) #{{[0-9]+}}
// CHECK: call swiftcc void @"$sSA7pointeexvs"(ptr noalias nocapture %{{.+}}, ptr %[[T9]], ptr %{{.+}}) #{{[0-9]+}}
// CHECK: call void @swift_errorRelease(ptr %[[T6]]) #{{[0-9]+}}
// CHECK-NEXT: br label %[[L7:.+]]

// CHECK: [[L4]]:                                     ; preds = %[[L1]]
// CHECK-NEXT: call void @swift_errorRelease(ptr %[[T6]]) #{{[0-9]+}}
// CHECK-NEXT: br label %[[L7]]

// CHECK: [[L7]]:                                     ; preds = %[[L6]], %[[L4]]
// CHECK-NEXT: br label %[[L3]]

// CHECK: [[L3]]:                                     ; preds = %[[L2]], %[[L7]]
// CHECK-NEXT: %[[T12:.+]] = phi i{{32|64}} [ 0, %[[L7]] ], [ %[[T5]], %[[L2]] ]
// CHECK-NEXT: call void @llvm.objc.release(ptr %{{.+}})
// CHECK-NEXT: %[[T14:.+]] = inttoptr i{{32|64}} %[[T12]] to ptr
// CHECK-NEXT: ret ptr %[[T14]]
