// RUN: %target-swift-frontend -emit-sil -verify %s

import _Differentiation

// https://github.com/apple/swift/issues/55745
// Test protocol witness thunk for `@differentiable` protocol requirement, where
// the required method has a non-wrt `inout` parameter.

protocol Proto {
  // expected-error @+1 {{cannot differentiate void function 'method(x:y:)'}}
  @differentiable(reverse, wrt: x)
  func method(x: Float, y: inout Float)
}

struct Struct: Proto {
  // expected-error @+1 {{cannot differentiate void function 'method(x:y:)'}}  
  @differentiable(reverse, wrt: x)
  func method(x: Float, y: inout Float) {
    y = y * x
  }
}

// Original crash:
// Assertion failed: (!array.empty() && "claiming next from empty array!"), function claimNext, file /Users/danielzheng/swift-build/swift/lib/SILGen/SILGenPoly.cpp, line 112.
// Stack dump:
// ...
// 1.	Swift version 5.3-dev (LLVM f8bd914aadc2e7b, Swift ba9c433c81d51ea)
// 2.	While evaluating request ASTLoweringRequest(Lowering AST to SIL for module main)
// 3.	While generating SIL witness table protocol conformance to 'SR_13305_Protocol' (at sr-13305.swift:7:1) for type 'SR_13305_Struct' (declared at [sr-13305.swift:12:1 - line:17:1] RangeText="struct SR_13305_Struct: SR_13305_Protocol {
//   @differentiable(reverse, wrt: x)
//   func method(x: Float, y: inout Float) {
//     y = y * x
//   }
// ")
// 4.	While generating protocol witness thunk SIL function "@AD__$s4main15SR_13305_StructVAA0B15_13305_ProtocolA2aDP6method1x1yySf_SfztFTW_jvp_SUU".
//  for 'method(x:y:)' (at sr-13305.swift:14:3)
// 5.	While emitting reabstraction thunk in SIL function "@$sSfIegy_S2fIegyd_TR".
// ...
// 7  swift-frontend           0x0000000100fe80ad swift::SILResultInfo const& claimNext<swift::SILResultInfo>(llvm::ArrayRef<swift::SILResultInfo>&) + 93
// 8  swift-frontend           0x0000000100fe6cc0 (anonymous namespace)::ResultPlanner::claimNextInnerResult((anonymous namespace)::ResultPlanner::PlanData&) + 32
