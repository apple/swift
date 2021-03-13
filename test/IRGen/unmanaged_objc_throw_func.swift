// RUN: %target-swift-frontend -emit-ir %s | %FileCheck %s
// REQUIRES: objc_interop
// REQUIRES: optimized_stdlib

import Foundation

@objc protocol SR_9035_P {
  func returnUnmanagedCFArray() throws -> Unmanaged<CFArray>
}

// CHECK-LABEL: define hidden swiftcc %TSo10CFArrayRefa* @"$s25unmanaged_objc_throw_func9SR_9035_CC22returnUnmanagedCFArrays0G0VySo0H3RefaGyKF"
@objc class SR_9035_C: NSObject, SR_9035_P {
  func returnUnmanagedCFArray() throws -> Unmanaged<CFArray> {
    // CHECK: %[[T0:.+]] = call swiftcc { %swift.bridge*, i8* } @"$ss27_allocateUninitializedArrayySayxG_BptBwlF"(i{{32|64}} 1, %swift.type* @"$sSiN")
    // CHECK-NEXT: %[[T1:.+]] = extractvalue { %swift.bridge*, i8* } %[[T0]], 0
    // CHECK-NEXT: %[[T2:.+]] = extractvalue { %swift.bridge*, i8* } %[[T0]], 1
    // CHECK-NEXT: %[[T3:.+]] = bitcast i8* %[[T2]] to %TSi*
    // CHECK-NEXT: %._value = getelementptr inbounds %TSi, %TSi* %[[T3]], i32 0, i32 0
    // CHECK:      %[[T7:.+]] = call swiftcc %swift.bridge* @"$ss27_finalizeUninitializedArrayySayxGABnlF"(%swift.bridge* %[[T1]], %swift.type* @"$sSiN")
    // CHECK:      %[[T4:.+]] = call swiftcc %TSo7NSArrayC* @"$sSa10FoundationE19_bridgeToObjectiveCSo7NSArrayCyF"(%swift.bridge* %[[T7]], %swift.type* @"$sSiN")
    // CHECK-NEXT: %[[T5:.+]] = bitcast %TSo7NSArrayC* %[[T4]] to %TSo10CFArrayRefa*
    // CHECK-NEXT: store %TSo10CFArrayRefa* %[[T5]]
    // CHECK-NEXT: call void @swift_bridgeObjectRelease(%swift.bridge* %{{[0-9]+}}) #{{[0-9]+}}
    // CHECK-NEXT: %[[T6:.+]] = bitcast %TSo10CFArrayRefa* %[[T5]] to i8*
    // CHECK-NEXT: call void @llvm.objc.release(i8* %[[T6]])
    // CHECK-NEXT: ret %TSo10CFArrayRefa* %[[T5]]
    let arr = [1] as CFArray
    return Unmanaged.passUnretained(arr) 
  } 
}

// CHECK: %[[T0:.+]] = call swiftcc %TSo10CFArrayRefa* @"$s25unmanaged_objc_throw_func9SR_9035_CC22returnUnmanagedCFArrays0G0VySo0H3RefaGyKF"
// CHECK-NEXT: %[[T2:.+]] = load %swift.error*, %swift.error** %swifterror, align {{[0-9]+}}
// CHECK-NEXT: %[[T3:.+]] = icmp ne %swift.error* %[[T2]], null
// CHECK-NEXT: br i1 %[[T3]], label %[[L1:.+]], label %[[L2:.+]]

// CHECK: [[L2]]:                                     ; preds = %entry
// CHECK-NEXT: %[[T4:.+]] = phi %TSo10CFArrayRefa* [ %[[T0]], %entry ]
// CHECK-NEXT: %[[T4a:.+]] = bitcast %T25unmanaged_objc_throw_func9SR_9035_CC* %{{.+}} to i8*
// CHECK-NEXT: call void @llvm.objc.release(i8* %[[T4a]])
// CHECK-NEXT: %[[T5:.+]] = ptrtoint %TSo10CFArrayRefa* %[[T4]] to i{{32|64}}
// CHECK-NEXT: br label %[[L3:.+]]

// CHECK: [[L1]]:                                     ; preds = %entry
// CHECK-NEXT: %[[T6:.+]] = phi %swift.error* [ %[[T2]], %entry ]
// CHECK-NEXT: store %swift.error* null, %swift.error** %swifterror, align {{[0-9]+}}
// CHECK-NEXT: %[[T6a:.+]] = bitcast %T25unmanaged_objc_throw_func9SR_9035_CC* %{{.+}} to i8*
// CHECK-NEXT: call void @llvm.objc.release(i8* %[[T6a]])
// CHECK-NEXT: %[[T7:.+]] = icmp eq i{{32|64}} %{{.+}}, 0
// CHECK-NEXT: br i1 %[[T7]], label %[[L4:.+]], label %[[L5:.+]]

// CHECK: [[L5]]:                                     ; preds = %[[L1]]
// CHECK-NEXT: %[[T8:.+]] = inttoptr i{{32|64}} %{{.+}} to i8*
// CHECK-NEXT: br label %[[L6:.+]]

// CHECK: [[L6]]:                                     ; preds = %[[L5]]
// CHECK-NEXT: %[[T9:.+]] = phi i8* [ %[[T8]], %[[L5]] ]
// CHECK-NEXT: %[[T10:.+]] = call swiftcc %TSo7NSErrorC* @"$s10Foundation22_convertErrorToNSErrorySo0E0Cs0C0_pF"(%swift.error* %[[T6]]) #{{[0-9]+}}
// CHECK: call swiftcc void @"$sSA7pointeexvs"(%swift.opaque* noalias nocapture %{{.+}}, i8* %[[T9]], %swift.type* %{{.+}}) #{{[0-9]+}}
// CHECK-NEXT: %[[T11:.+]] = bitcast %TSo7NSErrorCSg* %{{.+}} to i8*
// CHECK: call void @swift_errorRelease(%swift.error* %[[T6]]) #{{[0-9]+}}
// CHECK-NEXT: br label %[[L7:.+]]

// CHECK: [[L4]]:                                     ; preds = %[[L1]]
// CHECK-NEXT: call void @swift_errorRelease(%swift.error* %[[T6]]) #{{[0-9]+}}
// CHECK-NEXT: br label %[[L7]]

// CHECK: [[L7]]:                                     ; preds = %[[L6]], %[[L4]]
// CHECK-NEXT: br label %[[L3]]

// CHECK: [[L3]]:                                     ; preds = %[[L2]], %[[L7]]
// CHECK-NEXT: %[[T12:.+]] = phi i{{32|64}} [ 0, %[[L7]] ], [ %[[T5]], %[[L2]] ]
// CHECK-NEXT: %[[T14:.+]] = inttoptr i{{32|64}} %[[T12]] to %struct.__CFArray*
// CHECK-NEXT: ret %struct.__CFArray* %[[T14]]
