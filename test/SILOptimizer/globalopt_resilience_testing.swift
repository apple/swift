// RUN: %target-swift-frontend -emit-sil -O -enable-library-evolution -primary-file %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-sil -O -enable-library-evolution -enable-testing -primary-file %s | %FileCheck %s --check-prefix=CHECK-TESTING

// REQUIRES: swift_in_compiler

// If a global variable with a resilient type has public linkage, we have to
// allocate a buffer for it even if the type has a fixed size in its
// defining module.
//
// There are two cases where this can occur:
//
// - An internal property is defined in a resilient module built with
//   -enable-testing
//
// - A public property is defined to have the @_fixed_layout attribute

public struct Wrapper {
  var x: Int32

  static let usefulConstant = Wrapper(x: 321)
}

// CHECK-LABEL: sil_global hidden [let] @$s28globalopt_resilience_testing7WrapperV14usefulConstantACvpZ : $Wrapper = {
// CHECK-NEXT:   %0 = integer_literal $Builtin.Int32, 321
// CHECK-NEXT:   %1 = struct $Int32 (%0 : $Builtin.Int32)
// CHECK-NEXT:   %initval = struct $Wrapper (%1 : $Int32)
// CHECK-NEXT: }

// CHECK-TESTING-LABEL: sil_global [let] @$s28globalopt_resilience_testing7WrapperV14usefulConstantACvpZ : $Wrapper{{$}}
