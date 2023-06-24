// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -static -enable-library-evolution -emit-module-path=%t/resilient_protocol.swiftmodule -module-name=resilient_protocol %S/../Inputs/resilient_protocol.swift
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution %s | %FileCheck %s -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -enable-library-evolution -O %s

import resilient_protocol

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}}  swiftcc void @"$s22conformance_resilience14useConformanceyyx18resilient_protocol22OtherResilientProtocolRzlF"(ptr noalias nocapture %0, ptr %T, ptr %T.OtherResilientProtocol)
public func useConformance<T : OtherResilientProtocol>(_: T) {}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22conformance_resilience14getConformanceyy18resilient_protocol7WrapperVyxGlF"(ptr noalias nocapture %0, ptr %T)
public func getConformance<T>(_ w: Wrapper<T>) {
  // CHECK: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s18resilient_protocol7WrapperVMa"([[INT]] 0, ptr %T)
  // CHECK: [[META:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
  // CHECK: [[WTABLE:%.*]] = call ptr @swift_getWitnessTable(ptr @"$s18resilient_protocol7WrapperVyxGAA22OtherResilientProtocolAAMc", ptr [[META]], ptr undef)
  // CHECK: call swiftcc void @"$s22conformance_resilience14useConformanceyyx18resilient_protocol22OtherResilientProtocolRzlF"(ptr noalias nocapture %0, ptr [[META]], ptr [[WTABLE]])
  // CHECK: ret void
  useConformance(w)
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s22conformance_resilience14getConformanceyy18resilient_protocol15ConcreteWrapperVF"(ptr noalias nocapture %0)
public func getConformance(_ w: ConcreteWrapper) {
  // CHECK: [[RESPONSE:%.*]] = call swiftcc %swift.metadata_response @"$s18resilient_protocol15ConcreteWrapperVMa"([[INT]] 0)
  // CHECK: [[META:%.*]] = extractvalue %swift.metadata_response [[RESPONSE]], 0
  // CHECK: call swiftcc void @"$s22conformance_resilience14useConformanceyyx18resilient_protocol22OtherResilientProtocolRzlF"(ptr noalias nocapture %0, ptr [[META]], ptr @"$s18resilient_protocol15ConcreteWrapperVAA22OtherResilientProtocolAAWP")
  // CHECK: ret void
  useConformance(w)
}
