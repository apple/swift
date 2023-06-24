// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_protocol.swiftmodule -module-name=resilient_protocol %S/../Inputs/resilient_protocol.swift
// RUN: %target-swift-frontend %use_no_opaque_pointers -I %t -emit-ir -enable-library-evolution %s | %FileCheck --check-prefix=CHECK --check-prefix=CHECK-%target-cpu %s
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution %s
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %s

// CHECK: %swift.type = type { [[INT:i32|i64]] }

import resilient_protocol

// Method descriptors

// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsVoid1xySb_tFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 6)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsBoolSbyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 7)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP10returnsAnyypyFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 8)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP12throwingFuncyyKFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 9)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP11genericFuncyqd__qd__lFTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 10)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvgTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 11)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvsTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 12)
// CHECK-LABEL: @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvMTq" ={{( dllexport)?}}{{( protected)?}} alias %swift.protocol_requirement, getelementptr inbounds (<{{.*}}>* @"$s26protocol_resilience_thunks19MyResilientProtocolMp", i32 0, i32 13)

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks26callResilientWitnessMethodyyx010resilient_A00E12BaseProtocolRzlF"(%swift.opaque* noalias nocapture %0, %swift.type* %T, i8** %T.ResilientBaseProtocol)
// CHECK: call swiftcc [[INT]] @"$s18resilient_protocol21ResilientBaseProtocolP11requirementSiyFTj"(%swift.opaque* noalias nocapture swiftself %0, %swift.type* %T, i8** %T.ResilientBaseProtocol)
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

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsVoid1xySb_tFTj"(i1 %0, %swift.opaque* noalias nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK:      [[WITNESS_GEP:%.*]] = getelementptr inbounds i8*, i8** %3, i32 1
// CHECK:      [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_GEP]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to void (i1, %swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_GEP]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[FN]](i1 %0, %swift.opaque* noalias nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s26protocol_resilience_thunks19MyResilientProtocolP11returnsBoolSbyFTj"(%swift.opaque* noalias nocapture swiftself %0, %swift.type* %1, i8** %2)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %2, i32 2
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to i1 (%swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc i1 [[FN]](%swift.opaque* noalias nocapture swiftself %0, %swift.type* %1, i8** %2)
// CHECK-NEXT: ret i1 [[RESULT]]

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP10returnsAnyypyFTj"(%Any* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %3, i32 3
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to void (%Any*, %swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[FN]](%Any* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP12throwingFuncyyKFTj"(%swift.opaque* noalias nocapture swiftself %0, %swift.error**{{( noalias nocapture( swifterror)? dereferenceable\(.\))?}} %1, %swift.type* %2, i8** %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %3, i32 4
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to void (%swift.opaque*, %swift.error**, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[FN]](%swift.opaque* noalias nocapture swiftself %0, %swift.error**{{( noalias nocapture( swifterror)? dereferenceable\(.\))?}} %1, %swift.type* %2, i8** %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP11genericFuncyqd__qd__lFTj"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture %1, %swift.type* %2, %swift.opaque* noalias nocapture swiftself %3, %swift.type* %4, i8** %5)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %5, i32 5
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to void (%swift.opaque*, %swift.opaque*, %swift.type*, %swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[FN]](%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture %1, %swift.type* %2, %swift.opaque* noalias nocapture swiftself %3, %swift.type* %4, i8** %5)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc i1 @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvgTj"(%swift.opaque* noalias nocapture swiftself %0, %swift.type* %1, i8** %2)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %2, i32 6
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to i1 (%swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc i1 %5(%swift.opaque* noalias nocapture swiftself %0, %swift.type* %1, i8** %2)
// CHECK-NEXT: ret i1 [[RESULT]]

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvsTj"(i1 %0, %swift.opaque* nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %3, i32 7
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to void (i1, %swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: call swiftcc void [[FN]](i1 %0, %swift.opaque* nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK-NEXT: ret void

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc { i8*, %TSb* } @"$s26protocol_resilience_thunks19MyResilientProtocolP8propertySbvMTj"(i8* noalias dereferenceable({{16|32}}) %0, %swift.opaque* nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK:      [[WITNESS_ADDR:%.*]] = getelementptr inbounds i8*, i8** %3, i32 8
// CHECK-NEXT: [[WITNESS:%.*]] = load i8*, i8** [[WITNESS_ADDR]]
// CHECK-NEXT: [[FN:%.*]] = bitcast i8* [[WITNESS]] to { i8*, %TSb* } (i8*, %swift.opaque*, %swift.type*, i8**)*
// CHECK-arm64e-NEXT: ptrtoint i8** [[WITNESS_ADDR]] to i64
// CHECK-arm64e-NEXT: call i64 @llvm.ptrauth.blend
// CHECK-NEXT: [[RESULT:%.*]] = call swiftcc { i8*, %TSb* } [[FN]](i8* noalias dereferenceable({{16|32}}) %0, %swift.opaque* nocapture swiftself %1, %swift.type* %2, i8** %3)
// CHECK-NEXT: ret { i8*, %TSb* } [[RESULT]]
