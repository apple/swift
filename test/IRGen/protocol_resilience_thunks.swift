// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_protocol.swiftmodule -module-name=resilient_protocol %S/../Inputs/resilient_protocol.swift
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-cpu %s
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %s

import resilient_protocol

// Method descriptors

// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsVoid1xySb_tFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 6)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsBoolSbyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 7)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP10returnsAnyypyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 8)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP12throwingFuncyyKFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 9)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11genericFuncyqd__qd__lFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 10)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvgTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 11)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvsTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 12)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvMTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>, ptr @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 13)

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks26callResilientWitnessMethodyyx010resilient_A00E12BaseProtocolRzlF"(ptr noalias nocapture %0, ptr %T, ptr %T.ResilientBaseProtocol)
// CHECK: call swiftcc {{.*}} @"$s18resilient_protocol21ResilientBaseProtocolP11requirementSiyFTj"(ptr noalias nocapture swiftself %0, ptr %T, ptr %T.ResilientBaseProtocol)
// CHECK: ret void
public func callResilientWitnessMethod<T : ResilientBaseProtocol>(_ value: T) {
  _ = value.requirement()
}

public protocol MyResilientProtocol {
  func returnsVoid(x: Bool)

  func returnsBool() -> Bool
  func returnsAny() -> Any

  func throwingFunc() throws

  func genericFunc<T>(_: T) -> T

  var property: Bool { get set }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsVoid1xySb_tFTj"(i1 %0, ptr noalias nocapture swiftself %1, ptr %2, ptr %3)
// CHECK:      [[WITNESS_GEP:%.*]] = getelementptr inbounds ptr, ptr %3, i32 1
// CHECK:      [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_GEP]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_GEP]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[WITNESS]](i1 %0, ptr noalias nocapture swiftself %1, ptr %2, ptr %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsBoolSbyFTj"(ptr noalias nocapture swiftself %0, ptr %1, ptr %2)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %2, i32 2
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc i1 [[WITNESS]](ptr noalias nocapture swiftself %0, ptr %1, ptr %2)
// CHECK-NEXT: ret i1 [[RESULT]]

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP10returnsAnyypyFTj"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture swiftself %1, ptr %2, ptr %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %3, i32 3
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[WITNESS]](ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture swiftself %1, ptr %2, ptr %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP12throwingFuncyyKFTj"(ptr noalias nocapture swiftself %0, ptr{{( noalias nocapture( swifterror)? dereferenceable\(.\))?}} %1, ptr %2, ptr %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %3, i32 4
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[WITNESS]](ptr noalias nocapture swiftself %0, ptr{{( noalias nocapture( swifterror)? dereferenceable\(.\))?}} %1, ptr %2, ptr %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP11genericFuncyqd__qd__lFTj"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr %2, ptr noalias nocapture swiftself %3, ptr %4, ptr %5)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %5, i32 5
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[WITNESS]](ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr %2, ptr noalias nocapture swiftself %3, ptr %4, ptr %5)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvgTj"(ptr noalias nocapture swiftself %0, ptr %1, ptr %2)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %2, i32 6
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc i1 [[WITNESS]](ptr noalias nocapture swiftself %0, ptr %1, ptr %2)
// CHECK-NEXT: ret i1 [[RESULT]]

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvsTj"(i1 %0, ptr nocapture swiftself %1, ptr %2, ptr %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %3, i32 7
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[WITNESS]](i1 %0, ptr nocapture swiftself %1, ptr %2, ptr %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc { ptr, ptr } @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvMTj"(ptr noalias dereferenceable({{16|32}}) %0, ptr nocapture swiftself %1, ptr %2, ptr %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds ptr, ptr %3, i32 8
// CHECK-NEXT: [[WITNESS:%.*]] = load ptr, ptr [[WITNESS_ADDR]]
// CHECK-arm64e-NEXT: ptrtoint ptr [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc { ptr, ptr } [[WITNESS]](ptr noalias dereferenceable({{16|32}}) %0, ptr nocapture swiftself %1, ptr %2, ptr %3)
// CHECK-NEXT: ret { ptr, ptr } [[RESULT]]
