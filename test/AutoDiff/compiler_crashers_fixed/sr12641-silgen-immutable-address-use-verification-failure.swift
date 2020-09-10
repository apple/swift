// RUN: %target-swift-frontend -enable-resilience -emit-sil -verify %s
// REQUIRES: asserts
// REQUIRES: swift_stable_abi

// SR-12641: SILGen verification error regarding `ImmutableAddressUseVerifier` and AutoDiff-generated code.

import _Differentiation

public struct Resilient: Differentiable {
  var x: Float
}

public class Class: Differentiable {
  var x: Resilient
  init(_ x: Resilient) {
    self.x = x
  }
}

public func f(_ c: Class) -> Resilient {
  return Resilient(x: 0)
}

_ = pullback(at: Class(Resilient(x: 10)), in: f)

// swift/lib/SIL/Verifier/SILVerifier.cpp:456: bool (anonymous namespace)::ImmutableAddressUseVerifier::isConsumingOrMutatingArgumentConvention(swift::SILArgumentConvention): Assertion `conv.isIndirectConvention() && "Expect an indirect convention"' failed.
// Stack dump:
// ...
// 1.      Swift version 5.3-dev (LLVM be43a34c3c, Swift 6d5b2f5220)
// 2.      While evaluating request SILGenWholeModuleRequest(SIL Generation for module main)
// 3.      While verifying SIL function "@$s4main5ClassC13TangentVectorVAA9ResilientVADVIeggr_AeHIegnr_TR".
// ...
//  #8 0x00000000011e7a3e (anonymous namespace)::ImmutableAddressUseVerifier::isConsumingOrMutatingApplyUse(swift::Operand*)
//  #9 0x00000000011e6add (anonymous namespace)::ImmutableAddressUseVerifier::isMutatingOrConsuming(swift::SILValue)
// #10 0x00000000011ce0b4 (anonymous namespace)::SILVerifier::visitSILBasicBlock(swift::SILBasicBlock*)

// Related crasher discovered while fixing SR-12641.

class LoadableOriginal<T: Differentiable>: Differentiable {
  var x: T
  init(_ x: T) { self.x = x }
}

@differentiable
func loadableOriginal<T: AdditiveArithmetic>(_ loadable: LoadableOriginal<T>) -> T {
  return T.zero
}

// swift/include/swift/SIL/TypeLowering.h:845: swift::SILType swift::Lowering::TypeConverter::getLoweredLoadableType(swift::Type, swift::TypeExpansionContext, swift::SILModule &): Assertion `(ti.isLoadable() || !SILModuleConventions(M).useLoweredAddresses()) && "unexpected address-only type"' failed.
// Stack dump:
// ...
// 2.      While evaluating request ExecuteSILPipelineRequest(Run pipelines { Guaranteed Passes } on SIL for main.main)
// 3.      While running pass #153 SILModuleTransform "Differentiation".
// 4.      While processing // differentiability witness for loadableOriginal<A>(_:)
// sil_differentiability_witness hidden [parameters 0] [results 0] <T where T : AdditiveArithmetic, T : Differentiable> @$s4main16loadableOriginalyxAA08LoadableC0CyxGs18AdditiveArithmeticRz16_Differentiation14DifferentiableRzlF : $@convention(thin) <T where T : Additive
// Arithmetic, T : Differentiable> (@guaranteed LoadableOriginal<T>) -> @out T {
// }
//
//  on SIL function "@$s4main16loadableOriginalyxAA08LoadableC0CyxGs18AdditiveArithmeticRz16_Differentiation14DifferentiableRzlF".
//  for 'loadableOriginal(_:)'
// 5.      While generating VJP for SIL function "@$s4main16loadableOriginalyxAA08LoadableC0CyxGs18AdditiveArithmeticRz16_Differentiation14DifferentiableRzlF".
//  for 'loadableOriginal(_:)'
// 6.      While generating pullback for SIL function "@$s4main16loadableOriginalyxAA08LoadableC0CyxGs18AdditiveArithmeticRz16_Differentiation14DifferentiableRzlF".
//  for 'loadableOriginal(_:)'
// ...
//  #9 0x0000000000f83fbb swift::autodiff::PullbackEmitter::emitZeroDirect(swift::CanType, swift::SILLocation)
// #10 0x0000000000f8248b swift::autodiff::PullbackEmitter::emitZeroDerivativesForNonvariedResult(swift::SILValue)
// #11 0x0000000000f7fcae swift::autodiff::PullbackEmitter::run()
// #12 0x0000000000f3fba4 swift::autodiff::VJPEmitter::run()
// #13 0x0000000000eb1669 (anonymous namespace)::DifferentiationTransformer::canonicalizeDifferentiabilityWitness(swift::SILFunction*, swift::SILDifferentiabilityWitness*, swift::autodiff::DifferentiationInvoker, swift::IsSerialized_t)
// #14 0x0000000000eaea5e (anonymous namespace)::Differentiation::run()
