// RUN: %empty-directory(%t)

// RUN: %target-build-swift -emit-executable %s -g -o %t/variadic_nominal_types -emit-module -Xfrontend -disable-availability-checking

// RUN: sed -ne '/\/\/ *DEMANGLE-TYPE: /s/\/\/ *DEMANGLE-TYPE: *//p' < %s > %t/input
// RUN: %lldb-moduleimport-test-with-sdk %t/variadic_nominal_types -type-from-mangled=%t/input | %FileCheck %s --check-prefix=CHECK-TYPE

struct Variadic<each T, U> {
  struct Inner<each V, W> {}
}

extension Variadic where repeat each T: Equatable {
  struct Constrained {}
}

func blackHole(_: Any...) {}

do {
  let x0 = Variadic<Float>()
  let x1 = Variadic<Int, Float>()
  let x2 = Variadic<Int, String, Float>()
  let x3 = Variadic<Int, String, Float>.Inner<Character, Bool, Double>()

  blackHole(x0, x1, x2, x3)
}

do {
  let x0 = Variadic<Float>.Constrained()
  let x1 = Variadic<Int, Float>.Constrained()
  let x2 = Variadic<Int, String, Float>.Constrained()

  blackHole(x0, x1, x2)
}

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVyyQPSfGD
// CHECK-TYPE: Variadic<Float>

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVySi_QPSfGD
// CHECK-TYPE: Variadic<Int, Float>

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVySi_SSQPSfGD
// CHECK-TYPE: Variadic<Int, String, Float>

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicV5InnerVySi_SSQPSf_SJ_SbQPSdGD
// CHECK-TYPE: Variadic<Int, String, Float>.Inner<Character, Bool, Double>

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVAASQRzrlE11ConstrainedVyyQPSf_GD
// CHECK-TYPE: Variadic<Float>.Constrained

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVAASQRzrlE11ConstrainedVySi_QPSf_GD
// CHECK-TYPE: Variadic<Int, Float>.Constrained

// DEMANGLE-TYPE: $s22variadic_nominal_types8VariadicVAASQRzrlE11ConstrainedVySi_SSQPSf_GD
// CHECK-TYPE: Variadic<Int, String, Float>.Constrained
