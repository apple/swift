// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

// Test derivative function vtable entries for `@differentiable` class members:
// - Methods.
// - Accessors (from properties and subscripts).
// - Initializers.

import _Differentiation

// Dummy `Differentiable`-conforming type.
struct DummyTangentVector: Differentiable & AdditiveArithmetic {
  // FIXME(TF-648): Dummy to make `Super.TangentVector` be nontrivial.
  var _nontrivial: [Float] = []

  static var zero: Self { Self() }
  static func + (_: Self, _: Self) -> Self { Self() }
  static func - (_: Self, _: Self) -> Self { Self() }
  typealias TangentVector = Self
}

class Super: Differentiable {
  typealias TangentVector = DummyTangentVector
  func move(along _: TangentVector) {}

  var base: Float
  // FIXME(TF-648): Dummy to make `Super.TangentVector` be nontrivial.
  var _nontrivial: [Float] = []

  init(base: Float) {
    self.base = base
  }

  @differentiable(wrt: x)
  func method(_ x: Float, _ y: Float) -> Float {
    return x
  }

  @differentiable(wrt: x where T: Differentiable)
  func genericMethod<T>(_ x: T, _ y: T) -> T {
    return x
  }

  @differentiable
  var property: Float { base }

  @differentiable(wrt: x)
  subscript(_ x: Float, _ y: Float) -> Float {
    return x
  }
}

class Sub: Super {
  override init(base: Float) {
    super.init(base: base)
  }

  // Override JVP for `method` wrt `x`.
  @derivative(of: method, wrt: x)
  @derivative(of: subscript, wrt: x)
  final func jvpMethod(_ x: Float, _ y: Float) -> (value: Float, differential: (Float) -> Float) {
    fatalError()
  }
  // Override VJP for `method` wrt `x`.
  @derivative(of: method, wrt: x)
  @derivative(of: subscript, wrt: x)
  final func vjpMethod(_ x: Float, _ y: Float) -> (value: Float, pullback: (Float) -> (Float)) {
    fatalError()
  }

  // Override derivatives for `method` wrt `x`.
  // FIXME(TF-1203): This `@differentiable` attribute should not be necessary to
  // override derivatives. Fix `derivativeFunctionRequiresNewVTableEntry` to
  // account for derived declaration `@derivative` attributes.
  @differentiable(wrt: x)
  // Add new derivatives for `method` wrt `(x, y)`.
  @differentiable(wrt: (x, y))
  override func method(_ x: Float, _ y: Float) -> Float {
    return x
  }

  // Override derivatives for `property` wrt `self`.
  @differentiable
  override var property: Float { base }
  @derivative(of: property)
  final func vjpProperty() -> (value: Float, pullback: (Float) -> TangentVector) {
    fatalError()
  }

  // Override derivatives for `subscript` wrt `x`.
  @differentiable(wrt: x)
  override subscript(_ x: Float, _ y: Float) -> Float {
    return x
  }
}

class SubSub: Sub {}

// Check vtable entry thunks.

// CHECK-LABEL: sil hidden [transparent] [thunk] [ossa] @AD__$s6vtable5SuperC6methodyS2f_SftFTJfSUUpSr_vtable_entry_thunk : $@convention(method) (Float, Float, @guaranteed Super) -> (Float, @owned @callee_owned (Float) -> Float) {
// CHECK: bb0(%0 : $Float, %1 : $Float, %2 : @guaranteed $Super):
// CHECK:   %3 = function_ref @$s6vtable5SuperC6methodyS2f_SftF : $@convention(method) (Float, Float, @guaranteed Super) -> Float
// CHECK:   %4 = differentiable_function [parameters 0] [results 0] %3 : $@convention(method) (Float, Float, @guaranteed Super) -> Float
// CHECK:   %5 = differentiable_function_extract [jvp] %4 : $@differentiable @convention(method) (Float, @noDerivative Float, @noDerivative @guaranteed Super) -> Float
// CHECK:   %6 = apply %5(%0, %1, %2) : $@convention(method) (Float, Float, @guaranteed Super) -> (Float, @owned @callee_owned (Float) -> Float)
// CHECK:   return %6 : $(Float, @callee_owned (Float) -> Float)
// CHECK: }

// Check vtable entries: new vs `[override]` vs `[inherited]` entries.

// CHECK-LABEL: sil_vtable Super {
// CHECK:   #Super.method: (Super) -> (Float, Float) -> Float : @$s6vtable5SuperC6methodyS2f_SftF
// CHECK:   #Super.method!jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable5SuperC6methodyS2f_SftFTJfSUUpSr_vtable_entry_thunk
// CHECK:   #Super.method!vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable5SuperC6methodyS2f_SftFTJrSUUpSr_vtable_entry_thunk
// CHECK:   #Super.genericMethod: <T> (Super) -> (T, T) -> T : @$s6vtable5SuperC13genericMethodyxx_xtlF
// CHECK:   #Super.genericMethod!jvp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJfSUUpSr_vtable_entry_thunk
// CHECK:   #Super.genericMethod!vjp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJrSUUpSr_vtable_entry_thunk
// CHECK:   #Super.property!getter: (Super) -> () -> Float : @$s6vtable5SuperC8propertySfvg
// CHECK:   #Super.property!getter.jvp.S: (Super) -> () -> Float : @AD__$s6vtable5SuperC8propertySfvgTJfSpSr_vtable_entry_thunk
// CHECK:   #Super.property!getter.vjp.S: (Super) -> () -> Float : @AD__$s6vtable5SuperC8propertySfvgTJrSpSr_vtable_entry_thunk
// CHECK:   #Super.subscript!getter: (Super) -> (Float, Float) -> Float : @$s6vtable5SuperCyS2f_Sftcig
// CHECK:   #Super.subscript!getter.jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable5SuperCyS2f_SftcigTJfSUUpSr_vtable_entry_thunk
// CHECK:   #Super.subscript!getter.vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable5SuperCyS2f_SftcigTJrSUUpSr_vtable_entry_thunk
// CHECK: }

// CHECK-LABEL: sil_vtable Sub {
// CHECK:   #Super.method: (Super) -> (Float, Float) -> Float : @$s6vtable3SubC6methodyS2f_SftF [override]
// CHECK:   #Super.method!jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFAA5SuperCADyS2f_SftFTV_jvp [override]
// CHECK:   #Super.method!vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFAA5SuperCADyS2f_SftFTV_vjp [override]
// CHECK:   #Super.genericMethod: <T> (Super) -> (T, T) -> T : @$s6vtable5SuperC13genericMethodyxx_xtlF [inherited]
// CHECK:   #Super.genericMethod!jvp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJfSUUpSr_vtable_entry_thunk [inherited]
// CHECK:   #Super.genericMethod!vjp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJrSUUpSr_vtable_entry_thunk [inherited]
// CHECK:   #Super.property!getter: (Super) -> () -> Float : @$s6vtable3SubC8propertySfvg [override]
// CHECK:   #Super.property!getter.jvp.S: (Super) -> () -> Float : @AD__$s6vtable3SubC8propertySfvgAA5SuperCADSfvgTV_jvp [override]
// CHECK:   #Super.property!getter.vjp.S: (Super) -> () -> Float : @AD__$s6vtable3SubC8propertySfvgAA5SuperCADSfvgTV_vjp [override]
// CHECK:   #Super.subscript!getter: (Super) -> (Float, Float) -> Float : @$s6vtable3SubCyS2f_Sftcig [override]
// CHECK:   #Super.subscript!getter.jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubCyS2f_SftcigAA5SuperCyS2f_SftcigTV_jvp [override]
// CHECK:   #Super.subscript!getter.vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubCyS2f_SftcigAA5SuperCyS2f_SftcigTV_vjp [override]
// CHECK:   #Sub.method!jvp.SSU: (Sub) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFTJfSSUpSr_vtable_entry_thunk
// CHECK:   #Sub.method!vjp.SSU: (Sub) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFTJrSSUpSr_vtable_entry_thunk
// CHECK: }

// CHECK-LABEL: sil_vtable SubSub {
// CHECK:   #Super.method: (Super) -> (Float, Float) -> Float : @$s6vtable3SubC6methodyS2f_SftF [inherited]
// CHECK:   #Super.method!jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFAA5SuperCADyS2f_SftFTV_jvp [inherited]
// CHECK:   #Super.method!vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFAA5SuperCADyS2f_SftFTV_vjp [inherited]
// CHECK:   #Super.genericMethod: <T> (Super) -> (T, T) -> T : @$s6vtable5SuperC13genericMethodyxx_xtlF [inherited]
// CHECK:   #Super.genericMethod!jvp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJfSUUpSr_vtable_entry_thunk [inherited]
// CHECK:   #Super.genericMethod!vjp.SUU.<T where T : Differentiable>: <T> (Super) -> (T, T) -> T : @AD__$s6vtable5SuperC13genericMethodyxx_xtlF16_Differentiation14DifferentiableRzlTJrSUUpSr_vtable_entry_thunk [inherited]
// CHECK:   #Super.property!getter: (Super) -> () -> Float : @$s6vtable3SubC8propertySfvg [inherited]
// CHECK:   #Super.property!getter.jvp.S: (Super) -> () -> Float : @AD__$s6vtable3SubC8propertySfvgAA5SuperCADSfvgTV_jvp [inherited]
// CHECK:   #Super.property!getter.vjp.S: (Super) -> () -> Float : @AD__$s6vtable3SubC8propertySfvgAA5SuperCADSfvgTV_vjp [inherited]
// CHECK:   #Super.subscript!getter: (Super) -> (Float, Float) -> Float : @$s6vtable3SubCyS2f_Sftcig [inherited]
// CHECK:   #Super.subscript!getter.jvp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubCyS2f_SftcigAA5SuperCyS2f_SftcigTV_jvp [inherited]
// CHECK:   #Super.subscript!getter.vjp.SUU: (Super) -> (Float, Float) -> Float : @AD__$s6vtable3SubCyS2f_SftcigAA5SuperCyS2f_SftcigTV_vjp [inherited]
// CHECK:   #Sub.method!jvp.SSU: (Sub) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFTJfSSUpSr_vtable_entry_thunk [inherited]
// CHECK:   #Sub.method!vjp.SSU: (Sub) -> (Float, Float) -> Float : @AD__$s6vtable3SubC6methodyS2f_SftFTJrSSUpSr_vtable_entry_thunk [inherited]
// CHECK: }
