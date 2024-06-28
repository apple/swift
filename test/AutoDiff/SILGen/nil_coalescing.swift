// RUN: %target-swift-frontend -emit-sil -verify %s | %FileCheck %s

import _Differentiation

// CHECK: sil @test_nil_coalescing
// CHECK: bb0(%{{.*}} : $*T, %[[ARG_OPT:.*]] : $*Optional<T>, %[[ARG_PB:.*]] :
// CHECK:    $@noescape @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error any Error) for <T>):
// CHECK: %[[ALLOC_OPT:.*]] = alloc_stack [lexical] $Optional<T>
// CHECK: copy_addr %[[ARG_OPT]] to [init] %[[ALLOC_OPT]] : $*Optional<T>
// We'd need to check that ALLOC_OPT is an argument of switch_enum_addr below. However, this code
// is inlined from the standard library and therefore could have a sequence of copies in between
// depending whether we're compiling against debug or release stdlib
// CHECK: switch_enum_addr %{{.*}} : $*Optional<T>, case #Optional.some!enumelt: {{.*}}, case #Optional.none!enumelt: {{.*}}
// CHECK: try_apply %[[ARG_PB]](%{{.*}}) : $@noescape @callee_guaranteed @substituted <τ_0_0> () -> (@out τ_0_0, @error any Error) for <T>, normal {{.*}}, error {{.*}}
//
@_silgen_name("test_nil_coalescing")
@derivative(of: ??)
@usableFromInline
func nilCoalescing<T: Differentiable>(optional: T?, defaultValue: @autoclosure () throws -> T)
 rethrows -> (value: T, pullback: (T.TangentVector) -> Optional<T>.TangentVector)
{
 let hasValue = optional != nil
 let value = try optional ?? defaultValue()
 func pullback(_ v: T.TangentVector) -> Optional<T>.TangentVector {
   return hasValue ? .init(v) : .zero
 }
 return (value, pullback)
}
