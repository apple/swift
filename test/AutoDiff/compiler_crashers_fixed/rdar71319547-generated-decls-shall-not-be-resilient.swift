// RUN: %target-build-swift -enable-library-evolution %s
// RUN: %target-build-swift -O -enable-library-evolution %s
// RUN: %target-build-swift -O -g -enable-library-evolution %s

// rdar://71319547

import _Differentiation

// Assertion failed: (mainPullbackStruct->getType() == pbStructLoweredType), function run, file swift/lib/SILOptimizer/Differentiation/PullbackCloner.cpp, line 1899.
// Stack dump:
// 1.	Swift version 5.3-dev (LLVM 618cb952e0f199a, Swift d74c261f098665c)
// 2.	While evaluating request ExecuteSILPipelineRequest(Run pipelines { Mandatory Diagnostic Passes + Enabling Optimization Passes } on SIL for main.main)
// 3.	While running pass #17 SILModuleTransform "Differentiation".
// 4.	While processing // differentiability witness for foo(_:)
// sil_differentiability_witness [serialized] [reverse] [parameters 0] [results 0] @$s4main3fooyS2fF : $@convention(thin) (Float) -> Float {
// }
@differentiable(reverse, wrt: x)
public func i_have_a_pullback_struct(_ x: Float) -> Float {
  return x
}

// Assertion failed: (v->getType().isObject()), function operator(), file swift/lib/SIL/Utils/ValueUtils.cpp, line 22.
// Stack dump:
// 1.	Swift version 5.3-dev (LLVM 618cb952e0f199a, Swift d74c261f098665c)
// 2.	While evaluating request ExecuteSILPipelineRequest(Run pipelines { Mandatory Diagnostic Passes + Enabling Optimization Passes } on SIL for main.main)
// 3.	While running pass #24 SILModuleTransform "Differentiation".
// 4.	While processing // differentiability witness for i_have_a_branching_trace_enum(_:)
// sil_differentiability_witness [serialized] [reverse] [parameters 0] [results 0] @$s4main29i_have_a_branching_trace_enumyS2fF : $@convention(thin) (Float) -> Float {
// }
@differentiable(reverse, wrt: x)
public func i_have_a_branching_trace_enum(_ x: Float) -> Float {
  if true {
    return x
  } else {
    return x.squareRoot()
  }
}
