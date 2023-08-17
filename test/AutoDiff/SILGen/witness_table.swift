// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

// Test derivative function witness table entries for `@differentiable` protocol requirements.

import _Differentiation

protocol Protocol: Differentiable {
  @differentiable(reverse, wrt: (self, x, y))
  @differentiable(reverse, wrt: x)
  func method(_ x: Float, _ y: Double) -> Float

  @differentiable(reverse)
  var property: Float { get set }

  @differentiable(reverse, wrt: (self, x))
  subscript(_ x: Float, _ y: Float) -> Float { get set }
}

// Dummy `Differentiable`-conforming type.
struct DummyTangentVector: Differentiable & AdditiveArithmetic {
  static var zero: Self { Self() }
  static func + (_: Self, _: Self) -> Self { Self() }
  static func - (_: Self, _: Self) -> Self { Self() }
  typealias TangentVector = Self
}

struct Struct: Protocol {
  typealias TangentVector = DummyTangentVector
  mutating func move(by _: TangentVector) {}

  @differentiable(reverse, wrt: (self, x, y))
  @differentiable(reverse, wrt: x)
  func method(_ x: Float, _ y: Double) -> Float {
    return x
  }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}method{{.*}}_jvp_SUU : $@convention(witness_method: Protocol) (Float, Double, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed (Float) -> Float) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}method{{.*}} : $@convention(method) (Float, Double, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_FN]]
  // CHECK: [[JVP_FN:%.*]] = differentiable_function_extract [jvp] [[DIFF_FN]]
  // CHECK: apply [[JVP_FN]]
  // CHECK: }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}method{{.*}}_vjp_SUU : $@convention(witness_method: Protocol) (Float, Double, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed (Float) -> Float) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}method{{.*}} : $@convention(method) (Float, Double, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_FN]]
  // CHECK: [[VJP_FN:%.*]] = differentiable_function_extract [vjp] [[DIFF_FN]]
  // CHECK: apply [[VJP_FN]]
  // CHECK: }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}method{{.*}}_jvp_SSS : $@convention(witness_method: Protocol) (Float, Double, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float, Double, @in_guaranteed τ_0_0) -> Float for <DummyTangentVector>) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}method{{.*}} : $@convention(method) (Float, Double, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0 1 2] [results 0] [[ORIG_FN]]
  // CHECK: [[JVP_FN:%.*]] = differentiable_function_extract [jvp] [[DIFF_FN]]
  // CHECK: apply [[JVP_FN]]
  // CHECK: }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}method{{.*}}_vjp_SSS : $@convention(witness_method: Protocol) (Float, Double, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float) -> (Float, Double, @out τ_0_0) for <DummyTangentVector>) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}method{{.*}} : $@convention(method) (Float, Double, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0 1 2] [results 0] [[ORIG_FN]]
  // CHECK: [[VJP_FN:%.*]] = differentiable_function_extract [vjp] [[DIFF_FN]]
  // CHECK: apply [[VJP_FN]]
  // CHECK: }

  @differentiable(reverse)
  var property: Float {
    get { 1 }
    set {}
  }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}property{{.*}}_jvp_S : $@convention(witness_method: Protocol) (@in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (@in_guaranteed τ_0_0) -> Float for <DummyTangentVector>) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}property{{.*}} : $@convention(method) (Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_FN]]
  // CHECK: [[JVP_FN:%.*]] = differentiable_function_extract [jvp] [[DIFF_FN]]
  // CHECK: apply [[JVP_FN]]
  // CHECK: }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__${{.*}}property{{.*}}_vjp_S : $@convention(witness_method: Protocol) (@in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float) -> @out τ_0_0 for <DummyTangentVector>) {
  // CHECK: [[ORIG_FN:%.*]] = function_ref {{.*}}property{{.*}} : $@convention(method) (Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0] [results 0] [[ORIG_FN]]
  // CHECK: [[VJP_FN:%.*]] = differentiable_function_extract [vjp] [[DIFF_FN]]
  // CHECK: apply [[VJP_FN]]
  // CHECK: }

  @differentiable(reverse, wrt: (self, x))
  subscript(_ x: Float, _ y: Float) -> Float {
    get { x }
    set {}
  }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcigTW_jvp_SUS : $@convention(witness_method: Protocol) (Float, Float, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float, @in_guaranteed τ_0_0) -> Float for <DummyTangentVector>)
  // CHECK: [[ORIG_FN:%.*]] = function_ref @$s13witness_table6StructVyS2f_Sftcig : $@convention(method) (Float, Float, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0 2] [results 0] [[ORIG_FN]]
  // CHECK: [[JVP_FN:%.*]] = differentiable_function_extract [jvp] [[DIFF_FN]]
  // CHECK: apply [[JVP_FN]]
  // CHECK: }

  // CHECK-LABEL: sil private [transparent] [thunk] [ossa] @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcigTW_vjp_SUS : $@convention(witness_method: Protocol) (Float, Float, @in_guaranteed Struct) -> (Float, @owned @callee_guaranteed @substituted <τ_0_0> (Float) -> (Float, @out τ_0_0) for <DummyTangentVector>)
  // CHECK: [[ORIG_FN:%.*]] = function_ref @$s13witness_table6StructVyS2f_Sftcig : $@convention(method) (Float, Float, Struct) -> Float
  // CHECK: [[DIFF_FN:%.*]] = differentiable_function [parameters 0 2] [results 0] [[ORIG_FN]]
  // CHECK: [[VJP_FN:%.*]] = differentiable_function_extract [vjp] [[DIFF_FN]]
  // CHECK: apply [[VJP_FN]]
  // CHECK: }
}

// CHECK-LABEL: sil_witness_table hidden Struct: Protocol module witness_table {
// CHECK-NEXT:   base_protocol Differentiable: Struct: Differentiable module witness_table
// CHECK-NEXT:   method #Protocol.method: <Self where Self : Protocol> (Self) -> (Float, Double) -> Float : @$s13witness_table6StructVAA8ProtocolA2aDP6methodyS2f_SdtFTW
// CHECK-NEXT:   method #Protocol.method!jvp.SUU.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Double) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP6methodyS2f_SdtFTW_jvp_SUU
// CHECK-NEXT:   method #Protocol.method!vjp.SUU.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Double) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP6methodyS2f_SdtFTW_vjp_SUU
// CHECK-NEXT:   method #Protocol.method!jvp.SSS.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Double) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP6methodyS2f_SdtFTW_jvp_SSS
// CHECK-NEXT:   method #Protocol.method!vjp.SSS.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Double) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP6methodyS2f_SdtFTW_vjp_SSS
// CHECK-NEXT:   method #Protocol.property!getter: <Self where Self : Protocol> (Self) -> () -> Float : @$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvgTW
// CHECK-NEXT:   method #Protocol.property!getter.jvp.S.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> () -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvgTW_jvp_S
// CHECK-NEXT:   method #Protocol.property!getter.vjp.S.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> () -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvgTW_vjp_S
// CHECK-NEXT:   method #Protocol.property!setter: <Self where Self : Protocol> (inout Self) -> (Float) -> () : @$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvsTW
// CHECK-NEXT:   method #Protocol.property!setter.jvp.SS.<Self where Self : Protocol>: <Self where Self : Protocol> (inout Self) -> (Float) -> () : @AD__$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvsTW_jvp_SS
// CHECK-NEXT:   method #Protocol.property!setter.vjp.SS.<Self where Self : Protocol>: <Self where Self : Protocol> (inout Self) -> (Float) -> () : @AD__$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvsTW_vjp_SS
// CHECK-NEXT:   method #Protocol.property!modify: <Self where Self : Protocol> (inout Self) -> () -> () : @$s13witness_table6StructVAA8ProtocolA2aDP8propertySfvMTW
// CHECK-NEXT:   method #Protocol.subscript!getter: <Self where Self : Protocol> (Self) -> (Float, Float) -> Float : @$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcigTW
// CHECK-NEXT:   method #Protocol.subscript!getter.jvp.SUS.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Float) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcigTW_jvp_SUS
// CHECK-NEXT:   method #Protocol.subscript!getter.vjp.SUS.<Self where Self : Protocol>: <Self where Self : Protocol> (Self) -> (Float, Float) -> Float : @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcigTW_vjp_SUS
// CHECK-NEXT:   method #Protocol.subscript!setter: <Self where Self : Protocol> (inout Self) -> (Float, Float, Float) -> () : @$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcisTW
// CHECK-NEXT:   method #Protocol.subscript!setter.jvp.USUS.<Self where Self : Protocol>: <Self where Self : Protocol> (inout Self) -> (Float, Float, Float) -> () : @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcisTW_jvp_USUS
// CHECK-NEXT:   method #Protocol.subscript!setter.vjp.USUS.<Self where Self : Protocol>: <Self where Self : Protocol> (inout Self) -> (Float, Float, Float) -> () : @AD__$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftcisTW_vjp_USUS
// CHECK-NEXT:   method #Protocol.subscript!modify: <Self where Self : Protocol> (inout Self) -> (Float, Float) -> () : @$s13witness_table6StructVAA8ProtocolA2aDPyS2f_SftciMTW
// CHECK: }
