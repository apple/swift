// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -target %target-cpu-apple-macosx10.50 -emit-module -emit-module-path %t/weak_import_availability_helper.swiftmodule -parse-as-library %S/Inputs/weak_import_availability_helper.swift -enable-library-evolution
//
// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir | %FileCheck %s --check-prefixes=CHECK,CHECK-OLD
// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir -target %target-cpu-apple-macosx10.50 | %FileCheck %s --check-prefixes=CHECK,CHECK-NEW
// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir -target %target-cpu-apple-macosx10.60 | %FileCheck %s --check-prefixes=CHECK,CHECK-NEW

// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir -target %target-cpu-apple-macosx10.50 -weak-link-at-target | %FileCheck %s --check-prefixes=CHECK,CHECK-OLD
// RUN: %target-swift-frontend -primary-file %s -I %t -emit-ir -target %target-cpu-apple-macosx10.60 -weak-link-at-target | %FileCheck %s --check-prefixes=CHECK,CHECK-NEW

// REQUIRES: OS=macosx

import weak_import_availability_helper

public func useConditionallyAvailableCase(e: AlwaysAvailableEnum) {
  switch e {
    case .alwaysAvailableCase: break
    case .conditionallyAvailableCase: break
  }
}

// CHECK-OLD-LABEL: @"$s31weak_import_availability_helper19AlwaysAvailableEnumO013conditionallyF4CaseyA2CmFWC" = extern_weak constant i32
// CHECK-NEW-LABEL: @"$s31weak_import_availability_helper19AlwaysAvailableEnumO013conditionallyF4CaseyA2CmFWC" = external constant i32

func useConformance<T : AlwaysAvailableProtocol>(_: T.Type) {}

@available(macOS 10.50, *)
public func useConditionallyAvailableConformance() {
  useConformance(AlwaysAvailableStruct.self)
}

// FIXME: We reference the witness table directly -- that's a bug since the module is resilient. Should reference the
// conformance descriptor instead.

// CHECK-OLD-LABEL: @"$s31weak_import_availability_helper21AlwaysAvailableStructVAA0eF8ProtocolAAWP" = extern_weak global ptr
// CHECK-NEW-LABEL: @"$s31weak_import_availability_helper21AlwaysAvailableStructVAA0eF8ProtocolAAWP" = external global ptr

@available(macOS, unavailable)
func useUnavailableConformance<T : UnavailableProtocol>(_: T.Type) {}

@available(macOS, unavailable)
public func useUnavailableConformance() {
  useUnavailableConformance(AlwaysAvailableStruct.self)
}

// CHECK-LABEL: @"$s31weak_import_availability_helper21AlwaysAvailableStructVAA19UnavailableProtocolAAWP" = extern_weak global ptr, align 8

@available(macOS 10.50, *)
public func callConditionallyAvailableFunction() {
  conditionallyAvailableFunction()
}

// CHECK-OLD-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper30conditionallyAvailableFunctionyyF"()
// CHECK-NEW-LABEL: declare swiftcc void @"$s31weak_import_availability_helper30conditionallyAvailableFunctionyyF"()

@available(macOS, unavailable)
public func callUnavailableFunction() {
  unavailableFunction()
}

// CHECK-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper19unavailableFunctionyyF"()

@available(macOS 10.50, *)
public func useConditionallyAvailableGlobal() {
  _ = conditionallyAvailableGlobal
  conditionallyAvailableGlobal = 0
  conditionallyAvailableGlobal += 1
}

// CHECK-OLD-LABEL: declare extern_weak swiftcc i64 @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivg"()
// CHECK-NEW-LABEL: declare swiftcc i64 @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivg"()

// CHECK-OLD-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivs"(i64)
// CHECK-NEW-LABEL: declare swiftcc void @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivs"(i64)

// CHECK-OLD-LABEL: declare extern_weak swiftcc { ptr, ptr } @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivM"(ptr noalias dereferenceable(32))
// CHECK-NEW-LABEL: declare swiftcc { ptr, ptr } @"$s31weak_import_availability_helper28conditionallyAvailableGlobalSivM"(ptr noalias dereferenceable(32))

@available(macOS, unavailable)
public func useUnavailableGlobal() {
  _ = unavailableGlobal
  unavailableGlobal = 0
  unavailableGlobal += 1
}

// CHECK-LABEL: declare extern_weak swiftcc i64 @"$s31weak_import_availability_helper17unavailableGlobalSivg"()
// CHECK-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper17unavailableGlobalSivs"(i64)
// CHECK-LABEL: declare extern_weak swiftcc { ptr, ptr } @"$s31weak_import_availability_helper17unavailableGlobalSivM"(ptr noalias dereferenceable(32))

func blackHole<T>(_: T) {}

@available(macOS 10.50, *)
public func useConditionallyAvailableStruct() {
  blackHole(ConditionallyAvailableStruct.self)
}

// CHECK-OLD-LABEL: declare extern_weak swiftcc %swift.metadata_response @"$s31weak_import_availability_helper28ConditionallyAvailableStructVMa"(i64)
// CHECK-NEW-LABEL: declare swiftcc %swift.metadata_response @"$s31weak_import_availability_helper28ConditionallyAvailableStructVMa"(i64)

@available(macOS 10.50, *)
public func useNestedConditionallyAvailableStruct() {
  blackHole(ConditionallyAvailableStruct.NestedStruct.self)
}

// CHECK-OLD-LABEL: declare extern_weak swiftcc %swift.metadata_response @"$s31weak_import_availability_helper28ConditionallyAvailableStructV06NestedG0VMa"(i64)
// CHECK-NEW-LABEL: declare swiftcc %swift.metadata_response @"$s31weak_import_availability_helper28ConditionallyAvailableStructV06NestedG0VMa"(i64)

@available(macOS 10.50, *)
public func useConditionallyAvailableMethod(s: ConditionallyAvailableStruct) {
  s.conditionallyAvailableMethod()
}

// CHECK-OLD-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper28ConditionallyAvailableStructV013conditionallyF6MethodyyF"(ptr noalias nocapture swiftself)
// CHECK-NEW-LABEL: declare swiftcc void @"$s31weak_import_availability_helper28ConditionallyAvailableStructV013conditionallyF6MethodyyF"(ptr noalias nocapture swiftself)

@available(macOS, unavailable)
public func useUnavailableStruct() {
  blackHole(UnvailableStruct.self)
}

// CHECK-LABEL: declare extern_weak swiftcc %swift.metadata_response @"$s31weak_import_availability_helper16UnvailableStructVMa"(i64)

@available(macOS, unavailable)
public func useUnavailableMethod(s: UnvailableStruct) {
  s.unavailableMethod()
}

// CHECK-LABEL: declare extern_weak swiftcc void @"$s31weak_import_availability_helper16UnvailableStructV17unavailableMethodyyF"(ptr noalias nocapture swiftself)
