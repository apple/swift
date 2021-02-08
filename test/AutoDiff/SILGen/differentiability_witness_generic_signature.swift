// RUN: %target-swift-emit-silgen -verify -module-name main %s | %FileCheck %s
// RUN: %target-swift-emit-sil -verify -module-name main %s

// NOTE(SR-11950): SILParser crashes for SILGen round-trip.

// This file tests:
// - The "derivative generic signature" of `@differentiable` and `@derivative`
//   attributes.
// - The generic signature of lowered SIL differentiability witnesses.

// Context:
// - For `@differentiable` attributes: the derivative generic signature is
//   resolved from the original declaration's generic signature and additional
//   `where` clause requirements.
// - For `@derivative` attributes: the derivative generic signature is the
//   attributed declaration's generic signature.

import _Differentiation

//===----------------------------------------------------------------------===//
// Same-type requirements
//===----------------------------------------------------------------------===//

// Test original declaration with a generic signature and derivative generic
// signature where all generic parameters are concrete (i.e. bound to concrete
// types via same-type requirements).

struct AllConcrete<T>: Differentiable {}

extension AllConcrete {
  //   Original generic signature: `<T>`
  // Derivative generic signature: `<T where T == Float>`
  //    Witness generic signature: `<T where T == Float>`
  @_silgen_name("allconcrete_where_gensig_constrained")
  @differentiable(where T == Float)
  func whereClauseGenericSignatureConstrained() -> AllConcrete {
    return self
  }
}
extension AllConcrete where T == Float {
  @derivative(of: whereClauseGenericSignatureConstrained)
  func jvpWhereClauseGenericSignatureConstrained() -> (
    value: AllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (whereClauseGenericSignatureConstrained(), { $0 })
  }
}

// CHECK-LABEL: // differentiability witness for allconcrete_where_gensig_constrained
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] <T where T == Float> @allconcrete_where_gensig_constrained : $@convention(method) <T> (AllConcrete<T>) -> AllConcrete<T> {
// CHECK-NEXT:   jvp: @allconcrete_where_gensig_constrainedSfRszlTJfSpSr : $@convention(method) (AllConcrete<Float>) -> (AllConcrete<Float>, @owned @callee_owned (AllConcrete<Float>.TangentVector) -> AllConcrete<Float>.TangentVector)
// CHECK-NEXT: }

// If a `@differentiable` or `@derivative` attribute satisfies two conditions:
// 1. The derivative generic signature is equal to the original generic signature.
// 2. The derivative generic signature has *all concrete* generic parameters.
//
// Then the attribute should be lowered to a SIL differentiability witness with
// *no* derivative generic signature.

extension AllConcrete where T == Float {
  //   Original generic signature: `<T where T == Float>`
  // Derivative generic signature: `<T where T == Float>`
  //    Witness generic signature: none
  @_silgen_name("allconcrete_original_gensig")
  @differentiable
  func originalGenericSignature() -> AllConcrete {
    return self
  }

  @derivative(of: originalGenericSignature)
  func jvpOriginalGenericSignature() -> (
    value: AllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (originalGenericSignature(), { $0 })
  }

// CHECK-LABEL: // differentiability witness for allconcrete_original_gensig
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] @allconcrete_original_gensig : $@convention(method) (AllConcrete<Float>) -> AllConcrete<Float> {
// CHECK-NEXT:   jvp: @allconcrete_original_gensigTJfSpSr : $@convention(method) (AllConcrete<Float>) -> (AllConcrete<Float>, @owned @callee_owned (AllConcrete<Float>.TangentVector) -> AllConcrete<Float>.TangentVector)
// CHECK-NEXT: }

  //   Original generic signature: `<T where T == Float>`
  // Derivative generic signature: `<T where T == Float>` (explicit `where` clause)
  //    Witness generic signature: none
  @_silgen_name("allconcrete_where_gensig")
  @differentiable(where T == Float)
  func whereClauseGenericSignature() -> AllConcrete {
    return self
  }

  @derivative(of: whereClauseGenericSignature)
  func jvpWhereClauseGenericSignature() -> (
    value: AllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (whereClauseGenericSignature(), { $0 })
  }

// CHECK-LABEL: // differentiability witness for allconcrete_where_gensig
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] @allconcrete_where_gensig : $@convention(method) (AllConcrete<Float>) -> AllConcrete<Float> {
// CHECK-NEXT:   jvp: @allconcrete_where_gensigTJfSpSr : $@convention(method) (AllConcrete<Float>) -> (AllConcrete<Float>, @owned @callee_owned (AllConcrete<Float>.TangentVector) -> AllConcrete<Float>.TangentVector)
// CHECK-NEXT: }
}

// Test original declaration with a generic signature and derivative generic
// signature where *not* all generic parameters are concrete.
// types via same-type requirements).

struct NotAllConcrete<T, U>: Differentiable {}

extension NotAllConcrete {
  //   Original generic signature: `<T, U>`
  // Derivative generic signature: `<T, U where T == Float>`
  //    Witness generic signature: `<T, U where T == Float>` (not all concrete)
  @_silgen_name("notallconcrete_where_gensig_constrained")
  @differentiable(where T == Float)
  func whereClauseGenericSignatureConstrained() -> NotAllConcrete {
    return self
  }
}
extension NotAllConcrete where T == Float {
  @derivative(of: whereClauseGenericSignatureConstrained)
  func jvpWhereClauseGenericSignatureConstrained() -> (
    value: NotAllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (whereClauseGenericSignatureConstrained(), { $0 })
  }
}

// CHECK-LABEL: // differentiability witness for notallconcrete_where_gensig_constrained
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] <T, U where T == Float> @notallconcrete_where_gensig_constrained : $@convention(method) <T, U> (NotAllConcrete<T, U>) -> NotAllConcrete<T, U> {
// CHECK-NEXT:   jvp: @notallconcrete_where_gensig_constrainedSfRszr0_lTJfSpSr : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 == Float> (NotAllConcrete<Float, τ_0_1>) -> (NotAllConcrete<Float, τ_0_1>, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (τ_0_0) -> τ_0_1 for <NotAllConcrete<Float, τ_0_1>.TangentVector, NotAllConcrete<Float, τ_0_1>.TangentVector>)
// CHECK-NEXT: }

extension NotAllConcrete where T == Float {
  //   Original generic signature: `<T, U where T == Float>`
  // Derivative generic signature: `<T, U where T == Float>`
  //    Witness generic signature: `<T, U where T == Float>` (not all concrete)
  @_silgen_name("notallconcrete_original_gensig")
  @differentiable
  func originalGenericSignature() -> NotAllConcrete {
    return self
  }

  @derivative(of: originalGenericSignature)
  func jvpOriginalGenericSignature() -> (
    value: NotAllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (originalGenericSignature(), { $0 })
  }

// CHECK-LABEL: // differentiability witness for notallconcrete_original_gensig
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] <T, U where T == Float> @notallconcrete_original_gensig : $@convention(method) <T, U where T == Float> (NotAllConcrete<Float, U>) -> NotAllConcrete<Float, U> {
// CHECK-NEXT:   jvp: @notallconcrete_original_gensigSfRszr0_lTJfSpSr : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 == Float> (NotAllConcrete<Float, τ_0_1>) -> (NotAllConcrete<Float, τ_0_1>, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (τ_0_0) -> τ_0_1 for <NotAllConcrete<Float, τ_0_1>.TangentVector, NotAllConcrete<Float, τ_0_1>.TangentVector>)
// CHECK-NEXT: }

  //   Original generic signature: `<T, U where T == Float>`
  // Derivative generic signature: `<T, U where T == Float>` (explicit `where` clause)
  //    Witness generic signature: `<T, U where T == Float>` (not all concrete)
  @_silgen_name("notallconcrete_where_gensig")
  @differentiable(where T == Float)
  func whereClauseGenericSignature() -> NotAllConcrete {
    return self
  }

  @derivative(of: whereClauseGenericSignature)
  func jvpWhereClauseGenericSignature() -> (
    value: NotAllConcrete, differential: (TangentVector) -> TangentVector
  ) {
    (whereClauseGenericSignature(), { $0 })
  }

// CHECK-LABEL: // differentiability witness for notallconcrete_where_gensig
// CHECK-NEXT: sil_differentiability_witness hidden [parameters 0] [results 0] <T, U where T == Float> @notallconcrete_where_gensig : $@convention(method) <T, U where T == Float> (NotAllConcrete<Float, U>) -> NotAllConcrete<Float, U> {
// CHECK-NEXT:   jvp: @notallconcrete_where_gensigSfRszr0_lTJfSpSr : $@convention(method) <τ_0_0, τ_0_1 where τ_0_0 == Float> (NotAllConcrete<Float, τ_0_1>) -> (NotAllConcrete<Float, τ_0_1>, @owned @callee_owned @substituted <τ_0_0, τ_0_1> (τ_0_0) -> τ_0_1 for <NotAllConcrete<Float, τ_0_1>.TangentVector, NotAllConcrete<Float, τ_0_1>.TangentVector>)
// CHECK-NEXT: }
}
