// RUN: %target-swift-frontend -emit-sil -verify -Xllvm -debug-only=differentiation 2>&1 %s | %FileCheck %s -check-prefix=CHECK-DATA-STRUCTURES
// RUN: %target-swift-frontend -emit-sil -verify -Xllvm -sil-print-after=differentiation -o /dev/null 2>&1 %s | %FileCheck %s -check-prefix=CHECK-SIL
// REQUIRES: asserts

// TODO: Add FileCheck tests.

import _Differentiation

//===----------------------------------------------------------------------===//
// Conditionals
//===----------------------------------------------------------------------===//

@differentiable
@_silgen_name("cond")
func cond(_ x: Float) -> Float {
  if x > 0 {
    return x + x
  }
  return x - x
}

// CHECK-DATA-STRUCTURES: struct _AD__cond_bb0__PB__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: struct _AD__cond_bb1__PB__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   var predecessor: _AD__cond_bb1__Pred__src_0_wrt_0
// CHECK-DATA-STRUCTURES:   var pullback_0: (Float) -> (Float, Float)
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: struct _AD__cond_bb2__PB__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   var predecessor: _AD__cond_bb2__Pred__src_0_wrt_0
// CHECK-DATA-STRUCTURES:   var pullback_1: (Float) -> (Float, Float)
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: struct _AD__cond_bb3__PB__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   var predecessor: _AD__cond_bb3__Pred__src_0_wrt_0
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: enum _AD__cond_bb0__Pred__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: enum _AD__cond_bb1__Pred__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   case bb0(_AD__cond_bb0__PB__src_0_wrt_0)
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: enum _AD__cond_bb2__Pred__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   case bb0(_AD__cond_bb0__PB__src_0_wrt_0)
// CHECK-DATA-STRUCTURES: }
// CHECK-DATA-STRUCTURES: enum _AD__cond_bb3__Pred__src_0_wrt_0 {
// CHECK-DATA-STRUCTURES:   case bb2(_AD__cond_bb2__PB__src_0_wrt_0)
// CHECK-DATA-STRUCTURES:   case bb1(_AD__cond_bb1__PB__src_0_wrt_0)
// CHECK-DATA-STRUCTURES: }

// CHECK-SIL-LABEL: sil hidden [ossa] @condTJrSpSr : $@convention(thin) (Float) -> (Float, @owned @callee_owned (Float) -> Float) {
// CHECK-SIL: bb0([[INPUT_ARG:%.*]] : $Float):
// CHECK-SIL:   [[BB0_PB_STRUCT:%.*]] = struct $_AD__cond_bb0__PB__src_0_wrt_0 ()
// CHECK-SIL:   cond_br {{%.*}}, bb1, bb2

// CHECK-SIL: bb1:
// CHECK-SIL:   [[BB1_PRED:%.*]] = enum $_AD__cond_bb1__Pred__src_0_wrt_0, #_AD__cond_bb1__Pred__src_0_wrt_0.bb0!enumelt, [[BB0_PB_STRUCT]]
// CHECK-SIL:   [[BB1_PB_STRUCT:%.*]] = struct $_AD__cond_bb1__PB__src_0_wrt_0
// CHECK-SIL:   [[BB3_PRED_PRED1:%.*]] = enum $_AD__cond_bb3__Pred__src_0_wrt_0, #_AD__cond_bb3__Pred__src_0_wrt_0.bb1!enumelt, [[BB1_PB_STRUCT]]
// CHECK-SIL:   br bb3({{.*}} : $Float, [[BB3_PRED_PRED1]] : $_AD__cond_bb3__Pred__src_0_wrt_0)

// CHECK-SIL: bb2:
// CHECK-SIL:   [[BB2_PRED:%.*]] = enum $_AD__cond_bb2__Pred__src_0_wrt_0, #_AD__cond_bb2__Pred__src_0_wrt_0.bb0!enumelt, [[BB0_PB_STRUCT]]
// CHECK-SIL:   [[BB2_PB_STRUCT:%.*]] = struct $_AD__cond_bb2__PB__src_0_wrt_0
// CHECK-SIL:   [[BB3_PRED_PRED2:%.*]] = enum $_AD__cond_bb3__Pred__src_0_wrt_0, #_AD__cond_bb3__Pred__src_0_wrt_0.bb2!enumelt, [[BB2_PB_STRUCT]]
// CHECK-SIL:   br bb3({{.*}} : $Float, [[BB3_PRED_PRED2]] : $_AD__cond_bb3__Pred__src_0_wrt_0)

// CHECK-SIL: bb3([[ORIG_RES:%.*]] : $Float, [[BB3_PRED_ARG:%.*]] : @owned $_AD__cond_bb3__Pred__src_0_wrt_0)
// CHECK-SIL:   [[BB3_PB_STRUCT:%.*]] = struct $_AD__cond_bb3__PB__src_0_wrt_0
// CHECK-SIL:   [[PULLBACK_REF:%.*]] = function_ref @condTJpSpSr
// CHECK-SIL:   [[PB:%.*]] = partial_apply [[PULLBACK_REF]]([[BB3_PB_STRUCT]])
// CHECK-SIL:   [[VJP_RESULT:%.*]] = tuple ([[ORIG_RES]] : $Float, [[PB]] : $@callee_owned (Float) -> Float)
// CHECK-SIL:   return [[VJP_RESULT]]


// CHECK-SIL-LABEL: sil private [ossa] @condTJpSpSr : $@convention(thin) (Float, @owned _AD__cond_bb3__PB__src_0_wrt_0) -> Float {
// CHECK-SIL: bb0([[SEED:%.*]] : $Float, [[BB3_PB_STRUCT:%.*]] : @owned $_AD__cond_bb3__PB__src_0_wrt_0):
// CHECK-SIL:   [[BB3_PRED:%.*]] = destructure_struct [[BB3_PB_STRUCT]] : $_AD__cond_bb3__PB__src_0_wrt_0
// CHECK-SIL:   switch_enum [[BB3_PRED]] : $_AD__cond_bb3__Pred__src_0_wrt_0, case #_AD__cond_bb3__Pred__src_0_wrt_0.bb2!enumelt: bb1, case #_AD__cond_bb3__Pred__src_0_wrt_0.bb1!enumelt: bb3

// CHECK-SIL: bb1([[BB3_PRED2_TRAMP_PB_STRUCT:%.*]] : @owned $_AD__cond_bb2__PB__src_0_wrt_0):
// CHECK-SIL:   br bb2({{%.*}} : $Float, {{%.*}}: $Float, [[BB3_PRED2_TRAMP_PB_STRUCT]] : $_AD__cond_bb2__PB__src_0_wrt_0)

// CHECK-SIL: bb2({{%.*}} : $Float, {{%.*}} : $Float, [[BB2_PB_STRUCT:%.*]] : @owned $_AD__cond_bb2__PB__src_0_wrt_0):
// CHECK-SIL:   ([[BB2_PRED:%.*]], [[BB2_PB:%.*]]) = destructure_struct [[BB2_PB_STRUCT]]
// CHECK-SIL:   [[BB2_ADJVALS:%.*]] = apply [[BB2_PB]]([[SEED]]) : $@callee_owned (Float) -> (Float, Float)
// CHECK-SIL:   switch_enum [[BB2_PRED]] : $_AD__cond_bb2__Pred__src_0_wrt_0, case #_AD__cond_bb2__Pred__src_0_wrt_0.bb0!enumelt: bb6

// CHECK-SIL: bb3([[BB3_PRED1_TRAMP_PB_STRUCT:%.*]] : @owned $_AD__cond_bb1__PB__src_0_wrt_0):
// CHECK-SIL:   br bb4({{%.*}} : $Float, {{%.*}}: $Float, [[BB3_PRED1_TRAMP_PB_STRUCT]] : $_AD__cond_bb1__PB__src_0_wrt_0)

// CHECK-SIL: bb4({{%.*}} : $Float, {{%.*}} : $Float, [[BB1_PB_STRUCT:%.*]] : @owned $_AD__cond_bb1__PB__src_0_wrt_0):
// CHECK-SIL:   ([[BB1_PRED:%.*]], [[BB1_PB:%.*]]) = destructure_struct [[BB1_PB_STRUCT]]
// CHECK-SIL:   [[BB1_ADJVALS:%.*]] = apply [[BB1_PB]]([[SEED]]) : $@callee_owned (Float) -> (Float, Float)
// CHECK-SIL:   switch_enum [[BB1_PRED]] : $_AD__cond_bb1__Pred__src_0_wrt_0, case #_AD__cond_bb1__Pred__src_0_wrt_0.bb0!enumelt: bb5

// CHECK-SIL: bb5([[BB1_PRED0_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   br bb7({{%.*}} : $Float, [[BB1_PRED0_TRAMP_PB_STRUCT]] : $_AD__cond_bb0__PB__src_0_wrt_0)

// CHECK-SIL: bb6([[BB2_PRED0_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   br bb7({{%.*}} : $Float, [[BB2_PRED0_TRAMP_PB_STRUCT]] : $_AD__cond_bb0__PB__src_0_wrt_0)

// CHECK-SIL: bb7({{%.*}} : $Float, [[BB0_PB_STRUCT:%.*]] : $_AD__cond_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   return {{%.*}} : $Float

@differentiable
@_silgen_name("nested_cond")
func nested_cond(_ x: Float, _ y: Float) -> Float {
  if x > 0 {
    if y > 10 {
      return x * y
    } else {
      return x + y
    }
  }
  return y - x
}

@differentiable
@_silgen_name("nested_cond_generic")
func nested_cond_generic<T : Differentiable & FloatingPoint>(_ x: T, _ y: T) -> T {
  if x > 0 {
    if y > 10 {
      return y
    } else {
      return x
    }
  }
  return y
}

@differentiable
@_silgen_name("loop_generic")
func loop_generic<T : Differentiable & FloatingPoint>(_ x: T) -> T {
  var result = x
  for _ in 1..<3 {
    var y = x
    for _ in 1..<3 {
      result = y
      y = result
    }
  }
  return result
}

// Test `switch_enum`.

enum Enum {
  case a(Float)
  case b(Float, Float)
}
@differentiable
@_silgen_name("enum_notactive")
func enum_notactive(_ e: Enum, _ x: Float) -> Float {
  switch e {
  case let .a(a): return x * a
  case let .b(b1, b2): return x * b1 * b2
  }
}

// CHECK-SIL-LABEL: sil hidden [ossa] @enum_notactiveTJrUSpSr : $@convention(thin) (Enum, Float) -> (Float, @owned @callee_owned (Float) -> Float) {
// CHECK-SIL: bb0([[ENUM_ARG:%.*]] : $Enum, [[X_ARG:%.*]] : $Float):
// CHECK-SIL:   [[BB0_PB_STRUCT:%.*]] = struct $_AD__enum_notactive_bb0__PB__src_0_wrt_1 ()
// CHECK-SIL:   switch_enum [[ENUM_ARG]] : $Enum, case #Enum.a!enumelt: bb1, case #Enum.b!enumelt: bb2

// CHECK-SIL: bb1([[ENUM_A:%.*]] : $Float):
// CHECK-SIL:   [[BB1_PRED_PRED0:%.*]] = enum $_AD__enum_notactive_bb1__Pred__src_0_wrt_1, #_AD__enum_notactive_bb1__Pred__src_0_wrt_1.bb0!enumelt, [[BB0_PB_STRUCT]] : $_AD__enum_notactive_bb0__PB__src_0_wrt_1
// CHECK-SIL:   [[BB1_PB_STRUCT:%.*]] = struct $_AD__enum_notactive_bb1__PB__src_0_wrt_1 ({{.*}})
// CHECK-SIL:   [[BB3_PRED_PRED1:%.*]] = enum $_AD__enum_notactive_bb3__Pred__src_0_wrt_1, #_AD__enum_notactive_bb3__Pred__src_0_wrt_1.bb1!enumelt, [[BB1_PB_STRUCT]] : $_AD__enum_notactive_bb1__PB__src_0_wrt_1
// CHECK-SIL:   br bb3({{.*}} : $Float, [[BB3_PRED_PRED1]] : $_AD__enum_notactive_bb3__Pred__src_0_wrt_1)

// CHECK-SIL: bb2([[ENUM_B:%.*]] : $(Float, Float)):
// CHECK-SIL:   [[BB2_PRED_PRED0:%.*]] = enum $_AD__enum_notactive_bb2__Pred__src_0_wrt_1, #_AD__enum_notactive_bb2__Pred__src_0_wrt_1.bb0!enumelt, [[BB0_PB_STRUCT]] : $_AD__enum_notactive_bb0__PB__src_0_wrt_1
// CHECK-SIL:   [[BB2_PB_STRUCT:%.*]] = struct $_AD__enum_notactive_bb2__PB__src_0_wrt_1 ({{.*}})
// CHECK-SIL:   [[BB3_PRED_PRED2:%.*]] = enum $_AD__enum_notactive_bb3__Pred__src_0_wrt_1, #_AD__enum_notactive_bb3__Pred__src_0_wrt_1.bb2!enumelt, [[BB2_PB_STRUCT]] : $_AD__enum_notactive_bb2__PB__src_0_wrt_1
// CHECK-SIL:   br bb3({{.*}} : $Float, [[BB3_PRED_PRED2]] : $_AD__enum_notactive_bb3__Pred__src_0_wrt_1)

// CHECK-SIL: bb3([[ORIG_RES:%.*]] : $Float, [[BB3_PRED_ARG:%.*]] : @owned $_AD__enum_notactive_bb3__Pred__src_0_wrt_1)
// CHECK-SIL:   [[BB3_PB_STRUCT:%.*]] = struct $_AD__enum_notactive_bb3__PB__src_0_wrt_1
// CHECK-SIL:   [[PULLBACK_REF:%.*]] = function_ref @enum_notactiveTJpUSpSr
// CHECK-SIL:   [[PB:%.*]] = partial_apply [callee_guaranteed] [[PULLBACK_REF]]([[BB3_PB_STRUCT]])
// CHECK-SIL:   [[VJP_RESULT:%.*]] = tuple ([[ORIG_RES]] : $Float, [[PB]] : $@callee_guaranteed (Float) -> Float)
// CHECK-SIL:   return [[VJP_RESULT]]

// Test `switch_enum_addr`.

// Clone of `enum Optional<Wrapped>`.
enum AddressOnlyEnum<T> {
  case some(T)
  case none
}
@differentiable
@_silgen_name("enum_addr_notactive")
func enum_addr_notactive<T>(_ e: AddressOnlyEnum<T>, _ x: Float) -> Float {
  switch e {
  case .none: break
  case .some: break
  }
  return x
}

// CHECK-SIL-LABEL: sil hidden [ossa] @enum_addr_notactivelTJrUSpSr : $@convention(thin) <τ_0_0> (@in_guaranteed AddressOnlyEnum<τ_0_0>, Float) -> (Float, @owned @callee_guaranteed (Float) -> Float) {
// CHECK-SIL: bb0([[ENUM_ARG:%.*]] : $*AddressOnlyEnum<τ_0_0>, [[X_ARG:%.*]] : $Float):
// CHECK-SIL:   [[ENUM_ADDR:%.*]] = alloc_stack $AddressOnlyEnum<τ_0_0>
// CHECK-SIL:   copy_addr [[ENUM_ARG]] to [initialization] [[ENUM_ADDR]] : $*AddressOnlyEnum<τ_0_0>
// CHECK-SIL:   [[BB0_PB_STRUCT:%.*]] = struct $_AD__enum_addr_notactive_bb0__PB__src_0_wrt_1_l<τ_0_0> ()
// CHECK-SIL:   switch_enum_addr [[ENUM_ADDR]] : $*AddressOnlyEnum<τ_0_0>, case #AddressOnlyEnum.none!enumelt: bb1, case #AddressOnlyEnum.some!enumelt: bb2

// CHECK-SIL: bb1:
// CHECK-SIL:   [[BB1_PRED_PRED0:%.*]] = enum $_AD__enum_addr_notactive_bb1__Pred__src_0_wrt_1_l<τ_0_0>, #_AD__enum_addr_notactive_bb1__Pred__src_0_wrt_1_l.bb0!enumelt, [[BB0_PB_STRUCT]] : $_AD__enum_addr_notactive_bb0__PB__src_0_wrt_1_l<τ_0_0>
// CHECK-SIL:   dealloc_stack [[ENUM_ADDR]] : $*AddressOnlyEnum<τ_0_0>
// CHECK-SIL:   [[BB1_PB_STRUCT:%.*]] = struct $_AD__enum_addr_notactive_bb1__PB__src_0_wrt_1_l<τ_0_0> ([[BB1_PRED_PRED0]] : $_AD__enum_addr_notactive_bb1__Pred__src_0_wrt_1_l<τ_0_0>)
// CHECK-SIL:   [[BB3_PRED_PRED1:%.*]] = enum $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>, #_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l.bb1!enumelt, [[BB1_PB_STRUCT]] : $_AD__enum_addr_notactive_bb1__PB__src_0_wrt_1_l<τ_0_0>
// CHECK-SIL:   br bb3([[BB3_PRED_PRED1]] : $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>)

// CHECK-SIL: bb2:
// CHECK-SIL:   [[BB2_PRED_PRED0:%.*]] = enum $_AD__enum_addr_notactive_bb2__Pred__src_0_wrt_1_l<τ_0_0>, #_AD__enum_addr_notactive_bb2__Pred__src_0_wrt_1_l.bb0!enumelt, [[BB0_PB_STRUCT]] : $_AD__enum_addr_notactive_bb0__PB__src_0_wrt_1_l<τ_0_0>
// CHECK-SIL:   [[ENUM_DATA:%.*]] = unchecked_take_enum_data_addr [[ENUM_ADDR]] : $*AddressOnlyEnum<τ_0_0>, #AddressOnlyEnum.some!enumelt
// CHECK-SIL:   destroy_addr [[ENUM_DATA]] : $*τ_0_0
// CHECK-SIL:   dealloc_stack [[ENUM_ADDR]] : $*AddressOnlyEnum<τ_0_0>
// CHECK-SIL:   [[BB2_PB_STRUCT:%.*]] = struct $_AD__enum_addr_notactive_bb2__PB__src_0_wrt_1_l<τ_0_0> ([[BB2_PRED_PRED0]] : $_AD__enum_addr_notactive_bb2__Pred__src_0_wrt_1_l<τ_0_0>)
// CHECK-SIL:   [[BB3_PRED_PRED2:%.*]] = enum $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>, #_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l.bb2!enumelt, [[BB2_PB_STRUCT]] : $_AD__enum_addr_notactive_bb2__PB__src_0_wrt_1_l<τ_0_0>
// CHECK-SIL:   br bb3([[BB3_PRED_PRED2]] : $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>)

// CHECK-SIL: bb3([[BB3_PRED_ARG:%.*]] : $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>):
// CHECK-SIL:   [[BB3_PB_STRUCT:%.*]] = struct $_AD__enum_addr_notactive_bb3__PB__src_0_wrt_1_l<τ_0_0> ([[BB3_PRED_ARG]] : $_AD__enum_addr_notactive_bb3__Pred__src_0_wrt_1_l<τ_0_0>)

// CHECK-SIL:   [[PB_FNREF:%.*]] = function_ref @enum_addr_notactivelTJpUSpSr : $@convention(thin) <τ_0_0> (Float, @owned _AD__enum_addr_notactive_bb3__PB__src_0_wrt_1_l<τ_0_0>) -> Float
// CHECK-SIL:   [[PB:%.*]] = partial_apply [callee_guaranteed] [[PB_FNREF]]<τ_0_0>([[BB3_PB_STRUCT]]) : $@convention(thin) <τ_0_0> (Float, @owned _AD__enum_addr_notactive_bb3__PB__src_0_wrt_1_l<τ_0_0>) -> Float
// CHECK-SIL:   [[VJP_RESULT:%.*]] = tuple ([[X_ARG]] : $Float, [[PB]] : $@callee_guaranteed (Float) -> Float)
// CHECK-SIL:   return [[VJP_RESULT]] : $(Float, @callee_guaranteed (Float) -> Float)

// Test control flow + tuple buffer.
// Verify that pullback buffers are not allocated for address projections.

@differentiable
@_silgen_name("cond_tuple_var")
func cond_tuple_var(_ x: Float) -> Float {
  // expected-warning @+1 {{variable 'y' was never mutated; consider changing to 'let' constant}}
  var y = (x, x)
  if x > 0 {
    return y.0
  }
  return y.1
}

// CHECK-SIL-LABEL: sil private [ossa] @cond_tuple_varTJpSpSr : $@convention(thin) (Float, @owned _AD__cond_tuple_var_bb3__PB__src_0_wrt_0) -> Float {
// CHECK-SIL: bb0([[SEED:%.*]] : $Float, [[BB3_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb3__PB__src_0_wrt_0):
// CHECK-SIL:   [[BB3_PRED:%.*]] = destructure_struct [[BB3_PB_STRUCT]] : $_AD__cond_tuple_var_bb3__PB__src_0_wrt_0
// CHECK-SIL:   copy_addr {{%.*}} to {{%.*}} : $*(Float, Float)
// CHECK-SIL-NOT:   copy_addr {{%.*}} to {{%.*}} : $*Float
// CHECK-SIL:   switch_enum [[BB3_PRED]] : $_AD__cond_tuple_var_bb3__Pred__src_0_wrt_0, case #_AD__cond_tuple_var_bb3__Pred__src_0_wrt_0.bb2!enumelt: bb1, case #_AD__cond_tuple_var_bb3__Pred__src_0_wrt_0.bb1!enumelt: bb3

// CHECK-SIL: bb1([[BB3_PRED2_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb2__PB__src_0_wrt_0):
// CHECK-SIL:   br bb2({{%.*}} : $Float, {{%.*}} : $Float, [[BB3_PRED2_TRAMP_PB_STRUCT]] : $_AD__cond_tuple_var_bb2__PB__src_0_wrt_0)

// CHECK-SIL: bb2({{%.*}} : $Float, {{%.*}} : $Float, [[BB2_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb2__PB__src_0_wrt_0):
// CHECK-SIL:   [[BB2_PRED:%.*]] = destructure_struct [[BB2_PB_STRUCT]]
// CHECK-SIL:   copy_addr {{%.*}} to {{%.*}} : $*(Float, Float)
// CHECK-SIL-NOT:   copy_addr {{%.*}} to {{%.*}} : $*Float
// CHECK-SIL:   switch_enum [[BB2_PRED]] : $_AD__cond_tuple_var_bb2__Pred__src_0_wrt_0, case #_AD__cond_tuple_var_bb2__Pred__src_0_wrt_0.bb0!enumelt: bb6

// CHECK-SIL: bb3([[BB3_PRED1_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb1__PB__src_0_wrt_0):
// CHECK-SIL:   br bb4({{%.*}} : $Float, {{%.*}} : $Float, [[BB3_PRED1_TRAMP_PB_STRUCT]] : $_AD__cond_tuple_var_bb1__PB__src_0_wrt_0)

// CHECK-SIL: bb4({{%.*}} : $Float, {{%.*}} : $Float, [[BB1_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb1__PB__src_0_wrt_0):
// CHECK-SIL:   [[BB1_PRED:%.*]] = destructure_struct [[BB1_PB_STRUCT]]
// CHECK-SIL:   copy_addr {{%.*}} to {{%.*}} : $*(Float, Float)
// CHECK-SIL-NOT:   copy_addr {{%.*}} to {{%.*}} : $*Float
// CHECK-SIL:   switch_enum [[BB1_PRED]] : $_AD__cond_tuple_var_bb1__Pred__src_0_wrt_0, case #_AD__cond_tuple_var_bb1__Pred__src_0_wrt_0.bb0!enumelt: bb5

// CHECK-SIL: bb5([[BB1_PRED0_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   br bb7({{%.*}} : $Float, [[BB1_PRED0_TRAMP_PB_STRUCT]] : $_AD__cond_tuple_var_bb0__PB__src_0_wrt_0)

// CHECK-SIL: bb6([[BB2_PRED0_TRAMP_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   br bb7({{%.*}} : $Float, [[BB2_PRED0_TRAMP_PB_STRUCT]] : $_AD__cond_tuple_var_bb0__PB__src_0_wrt_0)

// CHECK-SIL: bb7({{%.*}} : $Float, [[BB0_PB_STRUCT:%.*]] : $_AD__cond_tuple_var_bb0__PB__src_0_wrt_0):
// CHECK-SIL:   return {{%.*}} : $Float
