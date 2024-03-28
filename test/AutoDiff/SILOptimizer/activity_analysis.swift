// TODO: re-enable the boolean-literal-folding pass and fix the test accordingly
// RUN: %target-swift-emit-sil -Xllvm -sil-disable-pass=boolean-literal-folding -verify -Xllvm -debug-only=differentiation %s  2>&1 | %FileCheck %s
// REQUIRES: asserts

import _Differentiation

// Check that `@noDerivative` struct projections have "NONE" activity.

struct HasNoDerivativeProperty: Differentiable {
  var x: Float
  @noDerivative var y: Float
}
@differentiable(reverse)
func testNoDerivativeStructProjection(_ s: HasNoDerivativeProperty) -> Float {
  var tmp = s
  tmp.y = tmp.x
  return tmp.x
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testNoDerivativeStructProjection{{.*}} at parameter indices (0) and result indices (0):
// CHECK: [ACTIVE] %0 = argument of bb0 : $HasNoDerivativeProperty
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $HasNoDerivativeProperty, var, name "tmp"
// CHECK: [ACTIVE]   %4 = begin_access [read] [static] %2 : $*HasNoDerivativeProperty
// CHECK: [ACTIVE]   %5 = struct_element_addr %4 : $*HasNoDerivativeProperty, #HasNoDerivativeProperty.x
// CHECK: [VARIED]   %6 = load [trivial] %5 : $*Float
// CHECK: [ACTIVE]   %8 = begin_access [modify] [static] %2 : $*HasNoDerivativeProperty
// CHECK: [NONE]   %9 = struct_element_addr %8 : $*HasNoDerivativeProperty, #HasNoDerivativeProperty.y
// CHECK: [ACTIVE]   %12 = begin_access [read] [static] %2 : $*HasNoDerivativeProperty
// CHECK: [ACTIVE]   %13 = struct_element_addr %12 : $*HasNoDerivativeProperty, #HasNoDerivativeProperty.x
// CHECK: [ACTIVE]   %14 = load [trivial] %13 : $*Float

// Check that non-differentiable `tuple_element_addr` projections are non-varied.

@differentiable(reverse where T : Differentiable)
func testNondifferentiableTupleElementAddr<T>(_ x: T) -> T {
  var tuple = (1, 1, (x, 1), 1)
  tuple.0 = 1
  tuple.2.0 = x
  tuple.3 = 1
  return tuple.2.0
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testNondifferentiableTupleElementAddr{{.*}} at parameter indices (0) and result indices (0):
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*T
// CHECK: [ACTIVE]   %3 = alloc_stack [lexical] [var_decl] $(Int, Int, (T, Int), Int), var, name "tuple"
// CHECK: [USEFUL]   %4 = tuple_element_addr %3 : $*(Int, Int, (T, Int), Int), 0
// CHECK: [USEFUL]   %5 = tuple_element_addr %3 : $*(Int, Int, (T, Int), Int), 1
// CHECK: [ACTIVE]   %6 = tuple_element_addr %3 : $*(Int, Int, (T, Int), Int), 2
// CHECK: [USEFUL]   %7 = tuple_element_addr %3 : $*(Int, Int, (T, Int), Int), 3
// CHECK: [ACTIVE]   %18 = tuple_element_addr %6 : $*(T, Int), 0
// CHECK: [USEFUL]   %19 = tuple_element_addr %6 : $*(T, Int), 1
// CHECK: [ACTIVE]   %35 = begin_access [modify] [static] %3 : $*(Int, Int, (T, Int), Int)
// CHECK: [USEFUL]   %36 = tuple_element_addr %35 : $*(Int, Int, (T, Int), Int), 0
// CHECK: [ACTIVE]   %41 = begin_access [modify] [static] %3 : $*(Int, Int, (T, Int), Int)
// CHECK: [ACTIVE]   %42 = tuple_element_addr %41 : $*(Int, Int, (T, Int), Int), 2
// CHECK: [ACTIVE]   %43 = tuple_element_addr %42 : $*(T, Int), 0
// CHECK: [ACTIVE]   %51 = begin_access [modify] [static] %3 : $*(Int, Int, (T, Int), Int)
// CHECK: [USEFUL]   %52 = tuple_element_addr %51 : $*(Int, Int, (T, Int), Int), 3
// CHECK: [ACTIVE]   %55 = begin_access [read] [static] %3 : $*(Int, Int, (T, Int), Int)
// CHECK: [ACTIVE]   %56 = tuple_element_addr %55 : $*(Int, Int, (T, Int), Int), 2
// CHECK: [ACTIVE]   %57 = tuple_element_addr %56 : $*(T, Int), 0

// TF-781: check active local address + nested conditionals.

@differentiable(reverse, wrt: x)
func TF_781(_ x: Float, _ y: Float) -> Float {
  var result = y
  if true {
    if true {
      result = result * x // check activity of `result` and this `apply`
    }
  }
  return result
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}TF_781{{.*}} at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [USEFUL] %1 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %4 = alloc_stack [var_decl] $Float, var, name "result"
// CHECK: [ACTIVE]   %19 = begin_access [read] [static] %4 : $*Float
// CHECK: [ACTIVE]   %20 = load [trivial] %19 : $*Float
// CHECK: [ACTIVE]   %23 = apply %22(%20, %0, %18) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %24 = begin_access [modify] [static] %4 : $*Float
// CHECK: [ACTIVE]   %31 = begin_access [read] [static] %4 : $*Float
// CHECK: [ACTIVE]   %32 = load [trivial] %31 : $*Float

// TF-954: check nested conditionals and addresses.

@differentiable(reverse)
func TF_954(_ x: Float) -> Float {
  var outer = x
  outerIf: if true {
    var inner = outer
    inner = inner * x // check activity of this `apply`
    if false {
      break outerIf
    }
    outer = inner
  }
  return outer
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}TF_954{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Float, var, name "outer"
// CHECK: bb1:
// CHECK: [ACTIVE]   %10 = alloc_stack [var_decl] $Float, var, name "inner"
// CHECK: [ACTIVE]   %11 = begin_access [read] [static] %2 : $*Float
// CHECK: [USEFUL]   %14 = metatype $@thin Float.Type
// CHECK: [ACTIVE]   %15 = begin_access [read] [static] %10 : $*Float
// CHECK: [ACTIVE]   %16 = load [trivial] %15 : $*Float
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK:   %18 = function_ref @$sSf1moiyS2f_SftFZ : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %19 = apply %18(%16, %0, %14) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %20 = begin_access [modify] [static] %10 : $*Float
// CHECK: bb3:
// CHECK: [ACTIVE]   %31 = begin_access [read] [static] %10 : $*Float
// CHECK: [ACTIVE]   %32 = load [trivial] %31 : $*Float
// CHECK: [ACTIVE]   %34 = begin_access [modify] [static] %2 : $*Float
// CHECK: bb5:
// CHECK: [ACTIVE]   %40 = begin_access [read] [static] %2 : $*Float
// CHECK: [ACTIVE]   %41 = load [trivial] %40 : $*Float

//===----------------------------------------------------------------------===//
// Branching cast instructions
//===----------------------------------------------------------------------===//

@differentiable(reverse)
func checked_cast_branch(_ x: Float) -> Float {
  // expected-warning @+1 {{'is' test is always true}}
  if Int.self is Any.Type {
    return x + x
  }
  return x * x
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}checked_cast_branch{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [NONE]   %2 = metatype $@thin Int.Type
// CHECK: [NONE]   %3 = metatype $@thick Int.Type
// CHECK: bb1:
// CHECK: [NONE] %5 = argument of bb1 : $@thick any Any.Type
// CHECK: [NONE]   %6 = integer_literal $Builtin.Int1, -1
// CHECK: bb2:
// CHECK: [NONE] %8 = argument of bb2 : $@thick Int.Type
// CHECK: [NONE]   %9 = integer_literal $Builtin.Int1, 0
// CHECK: bb3:
// CHECK: [NONE] %11 = argument of bb3 : $Builtin.Int1
// CHECK: [NONE]   %12 = metatype $@thin Bool.Type
// CHECK: [NONE]   // function_ref Bool.init(_builtinBooleanLiteral:)
// CHECK: [NONE]   %14 = apply %13(%11, %12) : $@convention(method) (Builtin.Int1, @thin Bool.Type) -> Bool
// CHECK: [NONE]   %15 = struct_extract %14 : $Bool, #Bool._value
// CHECK: bb4:
// CHECK: [USEFUL]   %17 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref static Float.+ infix(_:_:)
// CHECK: [ACTIVE]   %19 = apply %18(%0, %0, %17) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: bb5:
// CHECK: [USEFUL]   %21 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK: [ACTIVE]   %23 = apply %22(%0, %0, %21) : $@convention(method) (Float, Float, @thin Float.Type) -> Float

// CHECK-LABEL: sil hidden [ossa] @${{.*}}checked_cast_branch{{.*}} : $@convention(thin) (Float) -> Float {
// CHECK:   checked_cast_br Int.Type in %3 : $@thick Int.Type to any Any.Type, bb1, bb2
// CHECK: }

@differentiable(reverse)
func checked_cast_addr_nonactive_result<T: Differentiable>(_ x: T) -> T {
  if let _ = x as? Float {
    // Do nothing with `y: Float?` value.
  }
  return x
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}checked_cast_addr_nonactive_result{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*T
// CHECK: [VARIED]   %3 = alloc_stack $T
// CHECK: [VARIED]   %5 = alloc_stack $Float
// CHECK: bb1:
// CHECK: [VARIED]   %7 = load [trivial] %5 : $*Float
// CHECK: [VARIED]   %8 = enum $Optional<Float>, #Optional.some!enumelt, %7 : $Float
// CHECK: bb2:
// CHECK: [NONE]   %11 = enum $Optional<Float>, #Optional.none!enumelt
// CHECK: bb3:
// CHECK: [VARIED] %14 = argument of bb3 : $Optional<Float>
// CHECK: bb4:
// CHECK: bb5:
// CHECK: [VARIED] %18 = argument of bb5 : $Float
// CHECK: bb6:
// CHECK: [NONE]   %22 = tuple ()

// CHECK-LABEL: sil hidden [ossa] @${{.*}}checked_cast_addr_nonactive_result{{.*}} : $@convention(thin) <T where T : Differentiable> (@in_guaranteed T) -> @out T {
// CHECK:   checked_cast_addr_br take_always T in %3 : $*T to Float in %5 : $*Float, bb1, bb2
// CHECK: }

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func checked_cast_addr_active_result<T: Differentiable>(x: T) -> T {
  // expected-note @+1 {{expression is not differentiable}}
  if let y = x as? Float {
    // Use `y: Float?` value in an active way.
    return y as! T
  }
  return x
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}checked_cast_addr_active_result{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*T
// CHECK: [ACTIVE]   %3 = alloc_stack $T
// CHECK: [ACTIVE]   %5 = alloc_stack $Float
// CHECK: bb1:
// CHECK: [ACTIVE]   %7 = load [trivial] %5 : $*Float
// CHECK: [ACTIVE]   %8 = enum $Optional<Float>, #Optional.some!enumelt, %7 : $Float
// CHECK: bb2:
// CHECK: [USEFUL]   %11 = enum $Optional<Float>, #Optional.none!enumelt
// CHECK: bb3:
// CHECK: [ACTIVE] %14 = argument of bb3 : $Optional<Float>
// CHECK: bb4:
// CHECK: [ACTIVE] %16 = argument of bb4 : $Float
// CHECK: [ACTIVE]   %19 = alloc_stack $Float
// CHECK: bb5:
// CHECK: bb6:
// CHECK: [NONE]   %27 = tuple ()

// CHECK-LABEL: sil hidden [ossa] @${{.*}}checked_cast_addr_active_result{{.*}} : $@convention(thin) <T where T : Differentiable> (@in_guaranteed T) -> @out T {
// CHECK:   checked_cast_addr_br take_always T in %3 : $*T to Float in %5 : $*Float, bb1, bb2
// CHECK: }

//===----------------------------------------------------------------------===//
// Array literal differentiation
//===----------------------------------------------------------------------===//

// Check `array.uninitialized_intrinsic` applications.

@differentiable(reverse)
func testArrayUninitializedIntrinsic(_ x: Float, _ y: Float) -> [Float] {
  return [x, y]
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsic{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE] %1 = argument of bb0 : $Float
// CHECK: [USEFUL]   %4 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %6 = apply %5<Float>(%4) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%7**, %8) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED] (%7, **%8**) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED]   %9 = mark_dependence %8 : $Builtin.RawPointer on %7 : $Array<Float>
// CHECK: [ACTIVE]   %10 = pointer_to_address %9 : $Builtin.RawPointer to [strict] $*Float
// CHECK: [VARIED]   %12 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %13 = index_addr %10 : $*Float, %12 : $Builtin.Word
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %16 = apply %15<Float>(%7) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>

@differentiable(reverse where T: Differentiable)
func testArrayUninitializedIntrinsicGeneric<T>(_ x: T, _ y: T) -> [T] {
  return [x, y]
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsicGeneric{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*T
// CHECK: [USEFUL]   %4 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %6 = apply %5<T>(%4) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%7**, %8) = destructure_tuple %6 : $(Array<T>, Builtin.RawPointer)
// CHECK: [VARIED] (%7, **%8**) = destructure_tuple %6 : $(Array<T>, Builtin.RawPointer)
// CHECK: [VARIED]   %9 = mark_dependence %8 : $Builtin.RawPointer on %7 : $Array<T>
// CHECK: [ACTIVE]   %10 = pointer_to_address %9 : $Builtin.RawPointer to [strict] $*T
// CHECK: [VARIED]   %12 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %13 = index_addr %10 : $*T, %12 : $Builtin.Word
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %16 = apply %15<T>(%7) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>

// TF-952: Test array literal initialized from an address (e.g. `var`).
@differentiable(reverse)
func testArrayUninitializedIntrinsicAddress(_ x: Float, _ y: Float) -> [Float] {
  var result = x
  result = result * y
  return [result, result]
}
// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsicAddress{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE] %1 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %4 = alloc_stack [var_decl] $Float, var, name "result"
// CHECK: [ACTIVE]   %7 = begin_access [read] [static] %4 : $*Float
// CHECK: [ACTIVE]   %8 = load [trivial] %7 : $*Float
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK: [ACTIVE]   %11 = apply %10(%8, %1, %6) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %12 = begin_access [modify] [static] %4 : $*Float
// CHECK: [USEFUL]   %15 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %17 = apply %16<Float>(%15) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%18**, %19) = destructure_tuple %17 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED] (%18, **%19**) = destructure_tuple %17 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED]   %20 = mark_dependence %19 : $Builtin.RawPointer on %18 : $Array<Float>
// CHECK: [ACTIVE]   %21 = pointer_to_address %20 : $Builtin.RawPointer to [strict] $*Float
// CHECK: [ACTIVE]   %22 = begin_access [read] [static] %4 : $*Float
// CHECK: [VARIED]   %25 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %26 = index_addr %21 : $*Float, %25 : $Builtin.Word
// CHECK: [ACTIVE]   %27 = begin_access [read] [static] %4 : $*Float
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %31 = apply %30<Float>(%18) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>
// TF-952: Test array literal initialized with `apply` direct results.
@differentiable(reverse)
func testArrayUninitializedIntrinsicFunctionResult(_ x: Float, _ y: Float) -> [Float] {
  return [x * y, x * y]
}
// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsicFunctionResult{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE] %1 = argument of bb0 : $Float
// CHECK: [USEFUL]   %4 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %6 = apply %5<Float>(%4) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%7**, %8) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED] (%7, **%8**) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED]   %9 = mark_dependence %8 : $Builtin.RawPointer on %7 : $Array<Float>
// CHECK: [ACTIVE]   %10 = pointer_to_address %9 : $Builtin.RawPointer to [strict] $*Float
// CHECK: [USEFUL]   %11 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK: [ACTIVE]   %13 = apply %12(%0, %1, %11) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [VARIED]   %15 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %16 = index_addr %10 : $*Float, %15 : $Builtin.Word
// CHECK: [USEFUL]   %17 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK: [ACTIVE]   %19 = apply %18(%0, %1, %17) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %22 = apply %21<Float>(%7) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>

// TF-975: Test nested array literals.
@differentiable(reverse)
func testArrayUninitializedIntrinsicNested(_ x: Float, _ y: Float) -> [Float] {
  let array = [x, y]
  return [array[0], array[1]]
}
// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsicNested{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE] %1 = argument of bb0 : $Float
// CHECK: [USEFUL]   %4 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %6 = apply %5<Float>(%4) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%7**, %8) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED] (%7, **%8**) = destructure_tuple %6 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED]   %9 = mark_dependence %8 : $Builtin.RawPointer on %7 : $Array<Float>
// CHECK: [ACTIVE]   %10 = pointer_to_address %9 : $Builtin.RawPointer to [strict] $*Float
// CHECK: [VARIED]   %12 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %13 = index_addr %10 : $*Float, %12 : $Builtin.Word
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK:   %15 = function_ref @$ss27_finalizeUninitializedArrayySayxGABnlF : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>
// CHECK: [ACTIVE]   %16 = apply %15<Float>(%7) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>
// CHECK: [USEFUL]   %19 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK:   %20 = function_ref @$ss27_allocateUninitializedArrayySayxG_BptBwlF : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE]   %21 = apply %20<Float>(%19) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%22**, %23) = destructure_tuple %21 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED] (%22, **%23**) = destructure_tuple %21 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [VARIED]   %24 = mark_dependence %23 : $Builtin.RawPointer on %22 : $Array<Float>
// CHECK: [ACTIVE]   %25 = pointer_to_address %24 : $Builtin.RawPointer to [strict] $*Float
// CHECK: [USEFUL]   %26 = integer_literal $Builtin.IntLiteral, 0
// CHECK: [USEFUL]   %27 = metatype $@thin Int.Type
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %29 = apply %28(%26, %27) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK: [NONE]   // function_ref Array.subscript.getter
// CHECK: [NONE]   %31 = apply %30<Float>(%25, %29, %17) : $@convention(method) <τ_0_0> (Int, @guaranteed Array<τ_0_0>) -> @out τ_0_0
// CHECK: [VARIED]   %32 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %33 = index_addr %25 : $*Float, %32 : $Builtin.Word
// CHECK: [USEFUL]   %34 = integer_literal $Builtin.IntLiteral, 1
// CHECK: [USEFUL]   %35 = metatype $@thin Int.Type
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %37 = apply %36(%34, %35) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK: [NONE]   // function_ref Array.subscript.getter
// CHECK: [NONE]   %39 = apply %38<Float>(%33, %37, %17) : $@convention(method) <τ_0_0> (Int, @guaranteed Array<τ_0_0>) -> @out τ_0_0
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %41 = apply %40<Float>(%22) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>

// TF-978: Test array literal initialized with `apply` indirect results.
struct Wrapper<T: Differentiable>: Differentiable {
  var value: T
}
@differentiable(reverse)
func testArrayUninitializedIntrinsicApplyIndirectResult<T>(_ x: T, _ y: T) -> [Wrapper<T>] {
  return [Wrapper(value: x), Wrapper(value: y)]
}
// CHECK-LABEL: [AD] Activity info for ${{.*}}testArrayUninitializedIntrinsicApplyIndirectResult{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*T
// CHECK: [USEFUL]   %4 = integer_literal $Builtin.Word, 2
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %6 = apply %5<Wrapper<T>>(%4) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [ACTIVE] (**%7**, %8) = destructure_tuple %6 : $(Array<Wrapper<T>>, Builtin.RawPointer)
// CHECK: [VARIED] (%7, **%8**) = destructure_tuple %6 : $(Array<Wrapper<T>>, Builtin.RawPointer)
// CHECK: [VARIED]   %9 = mark_dependence %8 : $Builtin.RawPointer on %7 : $Array<Wrapper<T>>
// CHECK: [ACTIVE]   %10 = pointer_to_address %9 : $Builtin.RawPointer to [strict] $*Wrapper<T>
// CHECK: [USEFUL]   %11 = metatype $@thin Wrapper<T>.Type
// CHECK: [ACTIVE]   %12 = alloc_stack $T
// CHECK: [NONE]   // function_ref Wrapper.init(value:)
// CHECK: [NONE]   %15 = apply %14<T>(%10, %12, %11) : $@convention(method) <τ_0_0 where τ_0_0 : Differentiable> (@in τ_0_0, @thin Wrapper<τ_0_0>.Type) -> @out Wrapper<τ_0_0>
// CHECK: [VARIED]   %17 = integer_literal $Builtin.Word, 1
// CHECK: [ACTIVE]   %18 = index_addr %10 : $*Wrapper<T>, %17 : $Builtin.Word
// CHECK: [USEFUL]   %19 = metatype $@thin Wrapper<T>.Type
// CHECK: [ACTIVE]   %20 = alloc_stack $T
// CHECK: [NONE]   // function_ref Wrapper.init(value:)
// CHECK: [NONE]   %23 = apply %22<T>(%18, %20, %19) : $@convention(method) <τ_0_0 where τ_0_0 : Differentiable> (@in τ_0_0, @thin Wrapper<τ_0_0>.Type) -> @out Wrapper<τ_0_0>
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [ACTIVE]   %26 = apply %25<Wrapper<T>>(%7) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0>

//===----------------------------------------------------------------------===//
// `inout` argument differentiation
//===----------------------------------------------------------------------===//

struct Mut: Differentiable {}
extension Mut {
  @differentiable(reverse, wrt: (self, x))
  mutating func mutatingMethod(_ x: Mut) {}
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}3MutV14mutatingMethodyyACF at parameter indices (0, 1) and result indices (0)
// CHECK: [VARIED] %0 = argument of bb0 : $Mut
// CHECK: [ACTIVE] %1 = argument of bb0 : $*Mut

@differentiable(reverse, wrt: (nonactive, x))
func nonActiveInoutArg(_ nonactive: inout Mut, _ x: Mut) {
  nonactive.mutatingMethod(x)
  nonactive = x
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}17nonActiveInoutArgyyAA3MutVz_ADtF at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*Mut
// CHECK: [ACTIVE] %1 = argument of bb0 : $Mut
// CHECK: [ACTIVE]   %4 = begin_access [modify] [static] %0 : $*Mut
// CHECK: [NONE]   // function_ref Mut.mutatingMethod(_:)
// CHECK: [NONE]   %6 = apply %5(%1, %4) : $@convention(method) (Mut, @inout Mut) -> ()
// CHECK: [ACTIVE]   %8 = begin_access [modify] [static] %0 : $*Mut

@differentiable(reverse, wrt: x)
func activeInoutArgMutatingMethod(_ x: Mut) -> Mut {
  var result = x
  result.mutatingMethod(result)
  return result
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}28activeInoutArgMutatingMethodyAA3MutVADF at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Mut
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Mut, var, name "result"
// CHECK: [ACTIVE]   %4 = begin_access [read] [static] %2 : $*Mut
// CHECK: [ACTIVE]   %5 = load [trivial] %4 : $*Mut
// CHECK: [ACTIVE]   %7 = begin_access [modify] [static] %2 : $*Mut
// CHECK: [NONE]   // function_ref Mut.mutatingMethod(_:)
// CHECK: [NONE]   %9 = apply %8(%5, %7) : $@convention(method) (Mut, @inout Mut) -> ()
// CHECK: [ACTIVE]   %11 = begin_access [read] [static] %2 : $*Mut
// CHECK: [ACTIVE]   %12 = load [trivial] %11 : $*Mut

@differentiable(reverse, wrt: (nonactive, x))
func activeInoutArgMutatingMethodVar(_ nonactive: inout Mut, _ x: Mut) {
  var result = nonactive
  result.mutatingMethod(x)
  nonactive = result
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}31activeInoutArgMutatingMethodVaryyAA3MutVz_ADtF at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*Mut
// CHECK: [ACTIVE] %1 = argument of bb0 : $Mut
// CHECK: [ACTIVE]   %4 = alloc_stack [var_decl] $Mut, var, name "result"
// CHECK: [ACTIVE]   %5 = begin_access [read] [static] %0 : $*Mut
// CHECK: [ACTIVE]   %8 = begin_access [modify] [static] %4 : $*Mut
// CHECK: [NONE]   // function_ref Mut.mutatingMethod(_:)
// CHECK:   %9 = function_ref @${{.*}}3MutV14mutatingMethodyyACF : $@convention(method) (Mut, @inout Mut) -> ()
// CHECK: [NONE]   %10 = apply %9(%1, %8) : $@convention(method) (Mut, @inout Mut) -> ()
// CHECK: [ACTIVE]   %12 = begin_access [read] [static] %4 : $*Mut
// CHECK: [ACTIVE]   %13 = load [trivial] %12 : $*Mut
// CHECK: [ACTIVE]   %15 = begin_access [modify] [static] %0 : $*Mut
// CHECK: [NONE]   %19 = tuple ()

@differentiable(reverse, wrt: (nonactive, x))
func activeInoutArgMutatingMethodTuple(_ nonactive: inout Mut, _ x: Mut) {
  var result = (nonactive, x)
  result.0.mutatingMethod(result.0)
  nonactive = result.0
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}33activeInoutArgMutatingMethodTupleyyAA3MutVz_ADtF at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*Mut
// CHECK: [ACTIVE] %1 = argument of bb0 : $Mut
// CHECK: [ACTIVE]   %4 = alloc_stack [var_decl] $(Mut, Mut), var, name "result"
// CHECK: [ACTIVE]   %5 = tuple_element_addr %4 : $*(Mut, Mut), 0
// CHECK: [ACTIVE]   %6 = tuple_element_addr %4 : $*(Mut, Mut), 1
// CHECK: [ACTIVE]   %7 = begin_access [read] [static] %0 : $*Mut
// CHECK: [ACTIVE]   %11 = begin_access [read] [static] %4 : $*(Mut, Mut)
// CHECK: [ACTIVE]   %12 = tuple_element_addr %11 : $*(Mut, Mut), 0
// CHECK: [ACTIVE]   %13 = load [trivial] %12 : $*Mut
// CHECK: [ACTIVE]   %15 = begin_access [modify] [static] %4 : $*(Mut, Mut)
// CHECK: [ACTIVE]   %16 = tuple_element_addr %15 : $*(Mut, Mut), 0
// CHECK: [NONE]   // function_ref Mut.mutatingMethod(_:)
// CHECK: [NONE]   %18 = apply %17(%13, %16) : $@convention(method) (Mut, @inout Mut) -> ()
// CHECK: [ACTIVE]   %20 = begin_access [read] [static] %4 : $*(Mut, Mut)
// CHECK: [ACTIVE]   %21 = tuple_element_addr %20 : $*(Mut, Mut), 0
// CHECK: [ACTIVE]   %22 = load [trivial] %21 : $*Mut
// CHECK: [ACTIVE]   %24 = begin_access [modify] [static] %0 : $*Mut

// Check `inout` arguments.

@differentiable(reverse)
func activeInoutArg(_ x: inout Float) -> Float {
  var result = x
  result += x
  return result
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}activeInoutArg{{.*}} at parameter indices (0) and result indices (0, 1)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*Float
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Float, var, name "result"
// CHECK: [ACTIVE]   %10 = begin_access [modify] [static] %2 : $*Float
// CHECK: [NONE]   // function_ref static Float.+= infix(_:_:)
// CHECK: [NONE]   %12 = apply %11(%10, %8, %6) : $@convention(method) (@inout Float, Float, @thin Float.Type) -> ()
// CHECK: [ACTIVE]   %14 = begin_access [read] [static] %2 : $*Float
// CHECK: [ACTIVE]   %15 = load [trivial] %14 : $*Float

@differentiable(reverse)
func activeInoutArgNonactiveInitialResult(_ x: inout Float) -> Float {
  var result: Float = 1
  result += x
  return result
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}activeInoutArgNonactiveInitialResult{{.*}} at parameter indices (0) and result indices (0, 1)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*Float
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Float, var, name "result"
// CHECK: [NONE]   // function_ref Float.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %6 = apply %5(%3, %4) : $@convention(method) (Builtin.IntLiteral, @thin Float.Type) -> Float
// CHECK: [USEFUL]   %8 = metatype $@thin Float.Type
// CHECK: [ACTIVE]   %12 = begin_access [modify] [static] %2 : $*Float
// CHECK: [NONE]   // function_ref static Float.+= infix(_:_:)
// CHECK: [NONE]   %14 = apply %13(%12, %10, %8) : $@convention(method) (@inout Float, Float, @thin Float.Type) -> ()
// CHECK: [ACTIVE]   %16 = begin_access [read] [static] %2 : $*Float
// CHECK: [ACTIVE]   %17 = load [trivial] %16 : $*Float

//===----------------------------------------------------------------------===//
// Throwing function differentiation (`try_apply`)
//===----------------------------------------------------------------------===//

// TF-433: Test `try_apply`.

func rethrowing(_ x: () throws -> Void) rethrows -> Void {}

@differentiable(reverse)
func testTryApply(_ x: Float) -> Float {
  rethrowing({})
  return x
}

// TF-433: differentiation diagnoses `try_apply` before activity info is printed.
// CHECK-LABEL: [AD] Activity info for ${{.*}}testTryApply{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [NONE]   // function_ref closure #1 in testTryApply(_:)
// CHECK: [NONE]   %3 = thin_to_thick_function %2 : $@convention(thin) () -> @error any Error to $@noescape @callee_guaranteed () -> @error any Error
// CHECK: [NONE]   // function_ref rethrowing(_:)
// CHECK: bb1:
// CHECK: [NONE] %6 = argument of bb1 : $()
// CHECK: bb2:
// CHECK: [NONE] %8 = argument of bb2 : $any Error

//===----------------------------------------------------------------------===//
// Coroutine differentiation (`begin_apply`)
//===----------------------------------------------------------------------===//

struct HasCoroutineReadAccessors: Differentiable {
  var stored: Float
  var computed: Float {
    // `_read` is a coroutine: `(Self) -> () -> ()`.
    _read { yield stored }
  }
}

struct HasCoroutineModifyAccessors: Differentiable {
  var stored: Float
  var computed: Float {
    get { stored }
    // `_modify` is a coroutine: `(inout Self) -> () -> ()`.
    _modify { yield &stored }
  }
}


// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func testAccessorCoroutinesRead(_ x: HasCoroutineReadAccessors) -> Float {
  // We do not support differentiation of _read accessors
  // expected-note @+1 {{cannot differentiate through a direct yield result}}
  return x.computed
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testAccessorCoroutinesRead{{.*}} at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $HasCoroutineReadAccessors
// CHECK: [NONE]   // function_ref HasCoroutineReadAccessors.computed.read
// CHECK:   %2 = function_ref @$s17activity_analysis25HasCoroutineReadAccessorsV8computedSfvr : $@yield_once @convention(method) (HasCoroutineReadAccessors) -> @yields Float
// CHECK: [ACTIVE] (**%3**, %4) = begin_apply %2(%0) : $@yield_once @convention(method) (HasCoroutineReadAccessors) -> @yields Float
// CHECK: [VARIED] (%3, **%4**) = begin_apply %2(%0) : $@yield_once @convention(method) (HasCoroutineReadAccessors) -> @yields Float
// CHECK: [VARIED]   %5 = end_apply %4 as $()

@differentiable(reverse)
func testAccessorCoroutinesModify(_ x: HasCoroutineModifyAccessors) -> Float {
  var x = x
  x.computed *= x.computed
  return x.computed
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testAccessorCoroutinesModify{{.*}} at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $HasCoroutineModifyAccessors
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $HasCoroutineModifyAccessors
// CHECK: [USEFUL]   %4 = metatype $@thin Float.Type
// CHECK: [ACTIVE]   %5 = begin_access [read] [static] %2 : $*HasCoroutineModifyAccessors
// CHECK: [ACTIVE]   %6 = load [trivial] %5 : $*HasCoroutineModifyAccessors
// CHECK: [NONE]   // function_ref HasCoroutineModifyAccessors.computed.getter
// CHECK:   %7 = function_ref @$s17activity_analysis27HasCoroutineModifyAccessorsV8computedSfvg : $@convention(method) (HasCoroutineModifyAccessors) -> Float
// CHECK: [ACTIVE]   %8 = apply %7(%6) : $@convention(method) (HasCoroutineModifyAccessors) -> Float
// CHECK: [ACTIVE]   %10 = begin_access [modify] [static] %2 : $*HasCoroutineModifyAccessors
// CHECK: [NONE]   // function_ref HasCoroutineModifyAccessors.computed.modify
// CHECK:   %11 = function_ref @$s17activity_analysis27HasCoroutineModifyAccessorsV8computedSfvM : $@yield_once @convention(method) (@inout HasCoroutineModifyAccessors) -> @yields @inout Float
// CHECK: [ACTIVE] (**%12**, %13) = begin_apply %11(%10) : $@yield_once @convention(method) (@inout HasCoroutineModifyAccessors) -> @yields @inout Float
// CHECK: [VARIED] (%12, **%13**) = begin_apply %11(%10) : $@yield_once @convention(method) (@inout HasCoroutineModifyAccessors) -> @yields @inout Float
// CHECK: [NONE]   // function_ref static Float.*= infix(_:_:)
// CHECK:  %14 = function_ref @$sSf2meoiyySfz_SftFZ : $@convention(method) (@inout Float, Float, @thin Float.Type) -> ()
// CHECK: [NONE]   %15 = apply %14(%12, %8, %4) : $@convention(method) (@inout Float, Float, @thin Float.Type) -> ()
// CHECK: [VARIED]   %16 = end_apply %13 as $()
// CHECK: [ACTIVE]   %18 = begin_access [read] [static] %2 : $*HasCoroutineModifyAccessors
// CHECK: [ACTIVE]   %19 = load [trivial] %18 : $*HasCoroutineModifyAccessors
// CHECK: [NONE]   // function_ref HasCoroutineModifyAccessors.computed.getter
// CHECK:   %20 = function_ref @$s17activity_analysis27HasCoroutineModifyAccessorsV8computedSfvg : $@convention(method) (HasCoroutineModifyAccessors) -> Float
// CHECK: [ACTIVE]   %21 = apply %20(%19) : $@convention(method) (HasCoroutineModifyAccessors) -> Float

// TF-1078: Test `begin_apply` active `inout` argument.
// `Array.subscript.modify` is the applied coroutine.

@differentiable(reverse)
func testBeginApplyActiveInoutArgument(array: [Float], x: Float) -> Float {
  var array = array
  // Array subscript assignment below calls `Array.subscript.modify`.
  // expected-error @+2 {{expression is not differentiable}}  
  // expected-note @+1 {{cannot differentiate functions that have not been marked '@differentiable' and that are defined in other files}}
  array[0] = x
  return array[0]
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testBeginApplyActiveInoutArgument{{.*}} at parameter indices (0, 1) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Array<Float>
// CHECK: [ACTIVE] %1 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %4 = alloc_stack [var_decl] $Array<Float>, var, name "array"
// CHECK: [ACTIVE]   %5 = copy_value %0 : $Array<Float>
// CHECK: [USEFUL]   %7 = integer_literal $Builtin.IntLiteral, 0
// CHECK: [USEFUL]   %8 = metatype $@thin Int.Type
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %10 = apply %9(%7, %8) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK: [ACTIVE]   %11 = begin_access [modify] [static] %4 : $*Array<Float>
// CHECK: [NONE]   // function_ref Array.subscript.modify
// CHECK: [ACTIVE] (**%13**, %14) = begin_apply %12<Float>(%10, %11) : $@yield_once @convention(method) <τ_0_0> (Int, @inout Array<τ_0_0>) -> @yields @inout τ_0_0
// CHECK: [VARIED] (%13, **%14**) = begin_apply %12<Float>(%10, %11) : $@yield_once @convention(method) <τ_0_0> (Int, @inout Array<τ_0_0>) -> @yields @inout τ_0_0
// CHECK: [USEFUL]   %18 = integer_literal $Builtin.IntLiteral, 0
// CHECK: [USEFUL]   %19 = metatype $@thin Int.Type
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %21 = apply %20(%18, %19) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int
// CHECK: [ACTIVE]   %22 = begin_access [read] [static] %4 : $*Array<Float>
// CHECK: [ACTIVE]   %23 = load_borrow %22 : $*Array<Float>
// CHECK: [ACTIVE]   %24 = alloc_stack $Float
// CHECK: [NONE]   // function_ref Array.subscript.getter
// CHECK: [NONE]   %26 = apply %25<Float>(%24, %21, %23) : $@convention(method) <τ_0_0> (Int, @guaranteed Array<τ_0_0>) -> @out τ_0_0
// CHECK: [ACTIVE]   %27 = load [trivial] %24 : $*Float

// TF-1115: Test `begin_apply` active `inout` argument with non-active initial result.

@differentiable(reverse)
func testBeginApplyActiveButInitiallyNonactiveInoutArgument(x: Float) -> Float {
  // `var array` is initially non-active.
  var array: [Float] = [0]
  // Array subscript assignment below calls `Array.subscript.modify`.
  // expected-error @+2 {{expression is not differentiable}}  
  // expected-note @+1 {{cannot differentiate functions that have not been marked '@differentiable' and that are defined in other files}}
  array[0] = x
  return array[0]
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testBeginApplyActiveButInitiallyNonactiveInoutArgument{{.*}} at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Array<Float>, var, name "array"
// CHECK: [USEFUL]   %3 = integer_literal $Builtin.Word, 1
// CHECK: [NONE]   // function_ref _allocateUninitializedArray<A>(_:)
// CHECK: [USEFUL]   %5 = apply %4<Float>(%3) : $@convention(thin) <τ_0_0> (Builtin.Word) -> (@owned Array<τ_0_0>, Builtin.RawPointer)
// CHECK: [USEFUL] (**%6**, %7) = destructure_tuple %5 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [NONE] (%6, **%7**) = destructure_tuple %5 : $(Array<Float>, Builtin.RawPointer)
// CHECK: [NONE]   %8 = mark_dependence %7 : $Builtin.RawPointer on %6 : $Array<Float> // user: %9
// CHECK: [USEFUL]   %9 = pointer_to_address %8 : $Builtin.RawPointer to [strict] $*Float // user: %14
// CHECK: [USEFUL]   %10 = integer_literal $Builtin.IntLiteral, 0    // user: %13
// CHECK: [USEFUL]   %11 = metatype $@thin Float.Type                // user: %13
// CHECK: [NONE]   // function_ref Float.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %13 = apply %12(%10, %11) : $@convention(method) (Builtin.IntLiteral, @thin Float.Type) -> Float // user: %14
// CHECK: [NONE]   // function_ref _finalizeUninitializedArray<A>(_:)
// CHECK: [USEFUL]   %16 = apply %15<Float>(%6) : $@convention(thin) <τ_0_0> (@owned Array<τ_0_0>) -> @owned Array<τ_0_0> // user: %17
// CHECK: [USEFUL]   %18 = integer_literal $Builtin.IntLiteral, 0    // user: %21
// CHECK: [USEFUL]   %19 = metatype $@thin Int.Type                  // user: %21
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %21 = apply %20(%18, %19) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int // user: %24
// CHECK: [ACTIVE]   %22 = begin_access [modify] [static] %2 : $*Array<Float> // users: %28, %24
// CHECK: [NONE]   // function_ref Array.subscript.modify
// CHECK: [ACTIVE] (**%24**, %25) = begin_apply %23<Float>(%21, %22) : $@yield_once @convention(method) <τ_0_0> (Int, @inout Array<τ_0_0>) -> @yields @inout τ_0_0 // user: %26
// CHECK: [VARIED] (%24, **%25**) = begin_apply %23<Float>(%21, %22) : $@yield_once @convention(method) <τ_0_0> (Int, @inout Array<τ_0_0>) -> @yields @inout τ_0_0 // user: %27
// CHECK: [USEFUL]   %29 = integer_literal $Builtin.IntLiteral, 0    // user: %32
// CHECK: [USEFUL]   %30 = metatype $@thin Int.Type                  // user: %32
// CHECK: [NONE]   // function_ref Int.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %32 = apply %31(%29, %30) : $@convention(method) (Builtin.IntLiteral, @thin Int.Type) -> Int // user: %37
// CHECK: [ACTIVE]   %33 = begin_access [read] [static] %2 : $*Array<Float> // users: %40, %34
// CHECK: [ACTIVE]   %34 = load_borrow %33 : $*Array<Float>          // users: %39, %37
// CHECK: [ACTIVE]   %35 = alloc_stack $Float                        // users: %41, %38, %37
// CHECK: [NONE]   // function_ref Array.subscript.getter
// CHECK: [NONE]   %37 = apply %36<Float>(%35, %32, %34) : $@convention(method) <τ_0_0> (Int, @guaranteed Array<τ_0_0>) -> @out τ_0_0
// CHECK: [ACTIVE]   %38 = load [trivial] %35 : $*Float              // user: %44

//===----------------------------------------------------------------------===//
// Class differentiation
//===----------------------------------------------------------------------===//

class C: Differentiable {
  @differentiable(reverse)
  var float: Float

  init(_ float: Float) {
    self.float = float
  }

  @differentiable(reverse)
  func method(_ x: Float) -> Float {
    x * float
  }

// CHECK-LABEL: [AD] Activity info for ${{.*}}1CC6methodyS2fF at parameter indices (0, 1) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE] %1 = argument of bb0 : $C
// CHECK: [USEFUL]   %4 = metatype $@thin Float.Type
// CHECK: [VARIED]   %5 = class_method %1 : $C, #C.float!getter : (C) -> () -> Float, $@convention(method) (@guaranteed C) -> Float
// CHECK: [ACTIVE]   %6 = apply %5(%1) : $@convention(method) (@guaranteed C) -> Float
// CHECK: [NONE]   // function_ref static Float.* infix(_:_:)
// CHECK:   %7 = function_ref @$sSf1moiyS2f_SftFZ : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %8 = apply %7(%0, %6, %4) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
}

// TF-1176: Test class property `modify` accessor.
@differentiable(reverse)
func testClassModifyAccessor(_ c: inout C) {
  c.float *= c.float
}

// FIXME(TF-1176): Some values are incorrectly not marked as active: `%16`, etc.
// CHECK-LABEL: [AD] Activity info for ${{.*}}testClassModifyAccessor{{.*}} at parameter indices (0) and result indices (0)
// CHECK: [ACTIVE] %0 = argument of bb0 : $*C
// CHECK: [NONE]   %2 = metatype $@thin Float.Type
// CHECK: [ACTIVE]   %3 = begin_access [read] [static] %0 : $*C
// CHECK: [VARIED]   %4 = load [copy] %3 : $*C
// CHECK: [ACTIVE]   %6 = begin_access [read] [static] %0 : $*C
// CHECK: [VARIED]   %7 = load [copy] %6 : $*C
// CHECK: [VARIED]   %9 = class_method %7 : $C, #C.float!getter : (C) -> () -> Float, $@convention(method) (@guaranteed C) -> Float
// CHECK: [VARIED]   %10 = apply %9(%7) : $@convention(method) (@guaranteed C) -> Float
// CHECK: [VARIED]   %12 = class_method %4 : $C, #C.float!modify : (C) -> () -> (), $@yield_once @convention(method) (@guaranteed C) -> @yields @inout Float
// CHECK: [VARIED] (**%13**, %14) = begin_apply %12(%4) : $@yield_once @convention(method) (@guaranteed C) -> @yields @inout Float
// CHECK: [VARIED] (%13, **%14**) = begin_apply %12(%4) : $@yield_once @convention(method) (@guaranteed C) -> @yields @inout Float
// CHECK: [NONE]   // function_ref static Float.*= infix(_:_:)
// CHECK: [NONE]   %16 = apply %15(%13, %10, %2) : $@convention(method) (@inout Float, Float, @thin Float.Type) -> ()

//===----------------------------------------------------------------------===//
// Enum differentiation
//===----------------------------------------------------------------------===//

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse)
// expected-note @+1 {{when differentiating this function definition}}
func testActiveOptional(_ x: Float) -> Float {
  var maybe: Float? = 10
  // expected-note @+1 {{expression is not differentiable}}
  maybe = x
  return maybe!
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testActiveOptional{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $Float
// CHECK: [ACTIVE]   %2 = alloc_stack [var_decl] $Optional<Float>, var, name "maybe"
// CHECK: [USEFUL]   %3 = integer_literal $Builtin.IntLiteral, 10
// CHECK: [USEFUL]   %4 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref Float.init(_builtinIntegerLiteral:)
// CHECK: [USEFUL]   %6 = apply %5(%3, %4) : $@convention(method) (Builtin.IntLiteral, @thin Float.Type) -> Float
// CHECK: [USEFUL]   %7 = enum $Optional<Float>, #Optional.some!enumelt, %6 : $Float
// CHECK: [ACTIVE]   %9 = enum $Optional<Float>, #Optional.some!enumelt, %0 : $Float
// CHECK: [ACTIVE]   %10 = begin_access [modify] [static] %2 : $*Optional<Float>
// CHECK: [ACTIVE]   %13 = begin_access [read] [static] %2 : $*Optional<Float>
// CHECK: [ACTIVE]   %14 = load [trivial] %13 : $*Optional<Float>
// CHECK: bb1:
// CHECK: [NONE]   // function_ref _diagnoseUnexpectedNilOptional(_filenameStart:_filenameLength:_filenameIsASCII:_line:_isImplicitUnwrap:)
// CHECK: [NONE]   %24 = apply %23(%17, %18, %19, %20, %22) : $@convention(thin) (Builtin.RawPointer, Builtin.Word, Builtin.Int1, Builtin.Word, Builtin.Int1) -> ()
// CHECK: bb2:
// CHECK: [ACTIVE] %26 = argument of bb2 : $Float

enum DirectEnum: Differentiable & AdditiveArithmetic {
  case case0
  case case1(Float)
  case case2(Float, Float)

  typealias TangentVector = Self

  static var zero: Self { fatalError() }
  static func +(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
  static func -(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
}

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse, wrt: e)
// expected-note @+2 {{when differentiating this function definition}}
// expected-note @+1 {{differentiating enum values is not yet supported}}
func testActiveEnumValue(_ e: DirectEnum, _ x: Float) -> Float {
  switch e {
  case .case0: return x
  case let .case1(y1): return y1
  case let .case2(y1, y2): return y1 + y2
  }
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testActiveEnumValue{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $DirectEnum
// CHECK: [USEFUL] %1 = argument of bb0 : $Float
// CHECK: bb1:
// CHECK: bb2:
// CHECK: [ACTIVE] %6 = argument of bb2 : $Float
// CHECK: bb3:
// CHECK: [ACTIVE] %9 = argument of bb3 : $(Float, Float)
// CHECK: [ACTIVE] (**%10**, %11) = destructure_tuple %9 : $(Float, Float)
// CHECK: [ACTIVE] (%10, **%11**) = destructure_tuple %9 : $(Float, Float)
// CHECK: [USEFUL]   %14 = metatype $@thin Float.Type
// CHECK: [NONE]   // function_ref static Float.+ infix(_:_:)
// CHECK:   %15 = function_ref @$sSf1poiyS2f_SftFZ : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: [ACTIVE]   %16 = apply %15(%10, %11, %14) : $@convention(method) (Float, Float, @thin Float.Type) -> Float
// CHECK: bb4:
// CHECK: [ACTIVE] %18 = argument of bb4 : $Float

enum IndirectEnum<T: Differentiable>: Differentiable & AdditiveArithmetic {
  case case1(T)
  case case2(Float, T)

  typealias TangentVector = Self

  static func ==(_ lhs: Self, _ rhs: Self) -> Bool { fatalError() }
  static var zero: Self { fatalError() }
  static func +(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
  static func -(_ lhs: Self, _ rhs: Self) -> Self { fatalError() }
}

// expected-error @+1 {{function is not differentiable}}
@differentiable(reverse, wrt: e)
// expected-note @+2 {{when differentiating this function definition}}
// expected-note @+1 {{differentiating enum values is not yet supported}}
func testActiveEnumAddr<T>(_ e: IndirectEnum<T>) -> T {
  switch e {
  case let .case1(y1): return y1
  case let .case2(_, y2): return y2
  }
}

// CHECK-LABEL: [AD] Activity info for ${{.*}}testActiveEnumAddr{{.*}} at parameter indices (0) and result indices (0)
// CHECK: bb0:
// CHECK: [ACTIVE] %0 = argument of bb0 : $*T
// CHECK: [ACTIVE] %1 = argument of bb0 : $*IndirectEnum<T>
// CHECK: [ACTIVE]   %3 = alloc_stack $IndirectEnum<T>
// CHECK: bb1:
// CHECK: [ACTIVE]   %6 = unchecked_take_enum_data_addr %3 : $*IndirectEnum<T>, #IndirectEnum.case1!enumelt
// CHECK: [ACTIVE]   %7 = alloc_stack [lexical] [var_decl] $T, let, name "y1"
// CHECK: bb2:
// CHECK: [ACTIVE] {{.*}} = unchecked_take_enum_data_addr {{.*}} : $*IndirectEnum<T>, #IndirectEnum.case2!enumelt
// CHECK: [ACTIVE] {{.*}} = tuple_element_addr {{.*}} : $*(Float, T), 0
// CHECK: [VARIED] {{.*}} = load [trivial] {{.*}} : $*Float
// CHECK: [ACTIVE] {{.*}} = tuple_element_addr {{.*}} : $*(Float, T), 1
// CHECK: [ACTIVE] {{.*}} = alloc_stack [lexical] [var_decl] $T, let, name "y2"
// CHECK: bb3:
// CHECK: [NONE]   {{.*}} = tuple ()
