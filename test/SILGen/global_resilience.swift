// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module -parse-as-library -enable-library-evolution -emit-module-path=%t/resilient_global.swiftmodule -module-name=resilient_global %S/../Inputs/resilient_global.swift
// RUN: %target-swift-emit-silgen -I %t -enable-library-evolution -parse-as-library %s | %FileCheck %s
// RUN: %target-swift-emit-sil -I %t -O -enable-library-evolution -parse-as-library %s | %FileCheck --check-prefix=CHECK-OPT %s

// REQUIRES: swift_in_compiler

import resilient_global

public struct MyEmptyStruct {}

// CHECK-LABEL: sil_global private @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvp : $MyEmptyStruct

// CHECK-OPT-LABEL: sil_global private @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvp : $MyEmptyStruct = {
// CHECK-OPT-LABEL:   %initval = struct $MyEmptyStruct ()
// CHECK-OPT-LABEL: }

public var myEmptyGlobal = MyEmptyStruct()

// CHECK-LABEL: sil_global @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVvp : $MyEmptyStruct

@_fixed_layout public var myFixedLayoutGlobal = MyEmptyStruct()

// Mutable addressor for resilient global

// CHECK-LABEL: sil hidden [global_init] [ossa] @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvau : $@convention(thin) () -> Builtin.RawPointer
// CHECK:         global_addr @$s17global_resilience13myEmptyGlobalAA02MyD6StructVv
// CHECK:         return

// Synthesized accessors for our resilient global variable

// CHECK-LABEL: sil [ossa] @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvg
// CHECK:         function_ref @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvau
// CHECK:         return

// CHECK-LABEL: sil [ossa] @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvs
// CHECK:         function_ref @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvau
// CHECK:         return

// CHECK-LABEL: sil [ossa] @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvM
// CHECK:         function_ref @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvau
// CHECK:         begin_access [modify] [dynamic]
// CHECK:         yield
// CHECK:         end_access

// Mutable addressor for fixed-layout global

// CHECK-LABEL: sil private [global_init_once_fn] [ossa] @{{.*}}WZ
// CHECK:         alloc_global @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVv
// CHECK:         return

// CHECK-LABEL: sil [global_init] [ossa] @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVvau
// CHECK:         function_ref @{{.*}}WZ
// CHECK:         global_addr @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVv
// CHECK:         return

// CHECK-OPT-LABEL: sil private [global_init_once_fn] {{.*}}@{{.*}}WZ
// CHECK-OPT:     alloc_global @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVv
// CHECK-OPT:     return

// CHECK-OPT-LABEL: sil [global_init] @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVvau
// CHECK-OPT:     function_ref @{{.*}}WZ
// CHECK-OPT:     global_addr @$s17global_resilience19myFixedLayoutGlobalAA13MyEmptyStructVvp
// CHECK-OPT:     return

// Accessing resilient global from our resilience domain --
// call the addressor directly

// CHECK-LABEL: sil [ossa] @$s17global_resilience16getMyEmptyGlobalAA0dE6StructVyF
// CHECK:         function_ref @$s17global_resilience13myEmptyGlobalAA02MyD6StructVvau
// CHECK:         return
public func getMyEmptyGlobal() -> MyEmptyStruct {
  return myEmptyGlobal
}

// Accessing resilient global from a different resilience domain --
// access it with accessors

// CHECK-LABEL: sil [ossa] @$s17global_resilience14getEmptyGlobal010resilient_A00D15ResilientStructVyF
// CHECK:         function_ref @$s16resilient_global11emptyGlobalAA20EmptyResilientStructVvg
// CHECK:         return
public func getEmptyGlobal() -> EmptyResilientStruct {
  return emptyGlobal
}

// CHECK-LABEL: sil [ossa] @$s17global_resilience17modifyEmptyGlobalyyF
// CHECK:         [[MODIFY:%.*]] = function_ref @$s16resilient_global11emptyGlobalAA20EmptyResilientStructVvM
// CHECK-NEXT:    ([[ADDR:%.*]], [[TOKEN:%.*]]) = begin_apply [[MODIFY]]()
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[FN:%.*]] = function_ref @$s16resilient_global20EmptyResilientStructV6mutateyyF
// CHECK-NEXT:    apply [[FN]]([[ADDR]])
// CHECK-NEXT:    end_apply [[TOKEN]]
// CHECK-NEXT:    tuple
// CHECK-NEXT:    return
public func modifyEmptyGlobal() {
  emptyGlobal.mutate()
}

// Accessing fixed-layout global from a different resilience domain --
// call the addressor directly

// CHECK-LABEL: sil [ossa] @$s17global_resilience20getFixedLayoutGlobal010resilient_A020EmptyResilientStructVyF
// CHECK:         function_ref @$s16resilient_global17fixedLayoutGlobalAA20EmptyResilientStructVvau
// CHECK:         return
public func getFixedLayoutGlobal() -> EmptyResilientStruct {
  return fixedLayoutGlobal
}
