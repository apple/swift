// RUN: %target-swift-frontend -emit-silgen -verify %s | %FileCheck %s -check-prefix=CHECK-AST
// RUN: %target-swift-frontend -emit-silgen -verify %s | %FileCheck %s -check-prefix=CHECK-SIL

//===----------------------------------------------------------------------===//
// Normal types
//===----------------------------------------------------------------------===//

@_silgen_name("foo")
@differentiable(vjp: dfoo)
public func foo(_ x: Float, _ y: Float) -> Float {
  return 1
}

// CHECK-SIL-LABEL: sil [differentiable source 0 wrt 0, 1 vjp @$s3fooTZp0_1r0] [ossa] @foo

@_silgen_name("dfoo")
public func dfoo(_ x: Float, _ y: Float) -> (Float, (Float) -> (Float, Float)) {
  return (foo(x, y), { _ in (1, 1) })
}

// CHECK-SIL-LABEL: sil [ossa] @dfoo

//===----------------------------------------------------------------------===//
// Indirect returns
//===----------------------------------------------------------------------===//

@_silgen_name("foo_indir_ret")
@differentiable(vjp: dfoo_indir_ret)
public func foo_indir_ret<T: Differentiable>(_ x: Float, _ y: T) -> T {
  return y
}

// CHECK-SIL-LABEL: sil [differentiable source 0 wrt 0, 1 vjp @$s13foo_indir_retTZp0_1r0] [ossa] @foo_indir_ret
// CHECK-SIL: bb0(%0 : $*T, %1 : $Float, %2 : $*T):

@_silgen_name("dfoo_indir_ret")
public func dfoo_indir_ret<T: Differentiable>(_ x: Float, _ y: T) -> (T, (T.TangentVector) -> (Float, T.TangentVector)) {
  return (y, { v in (x, v) })
}

//===----------------------------------------------------------------------===//
// JVP
//===----------------------------------------------------------------------===//

@_silgen_name("hasjvp")
@differentiable(jvp: dhasjvp)
public func hasjvp(_ x: Float, _ y: Float) -> Float {
  return 1
}

// CHECK-SIL-LABEL: sil [differentiable source 0 wrt 0, 1 jvp @$s6hasjvpTzp0_1r0] [ossa] @hasjvp

@_silgen_name("dhasjvp")
public func dhasjvp(_ x: Float, _ y: Float) -> (Float, (Float, Float) -> Float) {
  return (1, { _, _ in 1 })
}

// CHECK-SIL-LABEL: sil [ossa] @dhasjvp

//===----------------------------------------------------------------------===//
// VJP
//===----------------------------------------------------------------------===//

@inlinable
@_silgen_name("hasvjp")
@differentiable(vjp: dhasvjp)
public func hasvjp(_ x: Float, _ y: Float) -> Float {
  return 1
}

// CHECK-SIL-LABEL: sil [serialized] [differentiable source 0 wrt 0, 1 vjp @$s6hasvjpTZp0_1r0] [ossa] @hasvjp

@_silgen_name("dhasvjp")
public func dhasvjp(_ x: Float, _ y: Float) -> (Float, (Float) -> (Float, Float)) {
  return (1, { _ in (1, 1) })
}

// CHECK-SIL-LABEL: sil [ossa] @dhasvjp

//===----------------------------------------------------------------------===//
// Computed property
//===----------------------------------------------------------------------===//

struct DiffComputedProp : Differentiable & AdditiveArithmetic {
  @differentiable(wrt: (self), jvp: computedPropJVP, vjp: computedPropVJP)
  var computedProp: Float {
    return 0
  }

  @_silgen_name("computedPropJVP")
  func computedPropJVP() -> (Float, (DiffComputedProp) -> Float) {
    fatalError("unimplemented")
  }

  @_silgen_name("computedPropVJP")
  func computedPropVJP() -> (Float, (Float) -> DiffComputedProp) {
    fatalError("unimplemented")
  }
}

// Check that `@differentiable` attribute is transferred from computed property
// storage declaration to getter accessor.

// CHECK-AST: struct DiffComputedProp : AdditiveArithmetic & Differentiable {
// CHECK-AST-NEXT:   var computedProp: Float { get }
// CHECK-AST: }

// CHECK-SIL-LABEL: // DiffComputedProp.computedProp.getter
// CHECK-SIL-NEXT: sil hidden [differentiable source 0 wrt 0 jvp @$s26differentiable_attr_silgen16DiffComputedPropV08computedF0SfvgTzp0r0 vjp @$s26differentiable_attr_silgen16DiffComputedPropV08computedF0SfvgTZp0r0] [ossa] @$s26differentiable_attr_silgen16DiffComputedPropV08computedF0Sfvg

public struct MyLayer: Differentiable {
  @differentiable
  var x: Float = 10
}

// CHECK-SIL-LABEL: initialization expression of MyLayer.x
// CHECK-SIL-NEXT: sil [transparent] [ossa] @$s26differentiable_attr_silgen7MyLayerV1xSfvpfi : $@convention(thin) () -> Float
