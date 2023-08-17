// RUN: %target-swift-frontend -emit-silgen %s | %FileCheck %s

public class SelfCasts {
  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC02toD0yACXDACFZ : $@convention(method) (@guaranteed SelfCasts, @thick SelfCasts.Type) -> @owned SelfCasts {
  // CHECK: unconditional_checked_cast {{.*}} : $SelfCasts to @dynamic_self SelfCasts
  // CHECK: }
  public static func toSelf(_ s: SelfCasts) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC09genericToD0yACXDxlFZ : $@convention(method) <T> (@in_guaranteed T, @thick SelfCasts.Type) -> @owned SelfCasts {
  // CHECK: unconditional_checked_cast_addr T in {{.*}} : $*T to @dynamic_self SelfCasts in {{.*}} : $*SelfCasts
  // CHECK: }
  public static func genericToSelf<T>(_ s: T) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC014classGenericToD0yACXDxRlzClFZ : $@convention(method) <T where T : AnyObject> (@guaranteed T, @thick SelfCasts.Type) -> @owned SelfCasts {
  // CHECK: unconditional_checked_cast {{.*}} : $T to @dynamic_self SelfCasts
  // CHECK: }
  public static func classGenericToSelf<T : AnyObject>(_ s: T) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC011genericFromD0xylFZ : $@convention(method) <T> (@thick SelfCasts.Type) -> @out T {
  // CHECK: unconditional_checked_cast_addr @dynamic_self SelfCasts in {{.*}} : $*SelfCasts to T in {{.*}} : $*T
  // CHECK: }
  public static func genericFromSelf<T>() -> T {
    let s = Self()
    return s as! T
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC016classGenericFromD0xyRlzClFZ : $@convention(method) <T where T : AnyObject> (@thick SelfCasts.Type) -> @owned T
  // CHECK: unconditional_checked_cast_addr @dynamic_self SelfCasts in {{.*}} : $*SelfCasts to T in {{.*}} : $*T
  // CHECK: }
  public static func classGenericFromSelf<T : AnyObject>() -> T {
    let s = Self()
    return s as! T
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC02toD11ConditionalyACXDSgACFZ : $@convention(method) (@guaranteed SelfCasts, @thick SelfCasts.Type) -> @owned Optional<SelfCasts> {
  // CHECK: checked_cast_br SelfCasts in {{.*}} : $SelfCasts to @dynamic_self SelfCasts
  // CHECK: }
  public static func toSelfConditional(_ s: SelfCasts) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC09genericToD11ConditionalyACXDSgxlFZ : $@convention(method) <T> (@in_guaranteed T, @thick SelfCasts.Type) -> @owned Optional<SelfCasts> {
  // CHECK: checked_cast_addr_br take_always T in {{.*}} : $*T to @dynamic_self SelfCasts in {{.*}} : $*SelfCasts
  // CHECK: }
  public static func genericToSelfConditional<T>(_ s: T) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC014classGenericToD11ConditionalyACXDSgxRlzClFZ : $@convention(method) <T where T : AnyObject> (@guaranteed T, @thick SelfCasts.Type) -> @owned Optional<SelfCasts> {
  // CHECK: checked_cast_br T in {{.*}} : $T to @dynamic_self SelfCasts
  // CHECK: }
  public static func classGenericToSelfConditional<T : AnyObject>(_ s: T) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC011genericFromD11ConditionalxSgylFZ : $@convention(method) <T> (@thick SelfCasts.Type) -> @out Optional<T> {
  // CHECK: checked_cast_addr_br take_always @dynamic_self SelfCasts in {{.*}} : $*SelfCasts to T in {{.*}} : $*T
  // CHECK: }
  public static func genericFromSelfConditional<T>() -> T? {
    let s = Self()
    return s as? T
  }

  // CHECK-LABEL: sil [ossa] @$s17dynamic_self_cast9SelfCastsC016classGenericFromD11ConditionalxSgyRlzClFZ : $@convention(method) <T where T : AnyObject> (@thick SelfCasts.Type) -> @owned Optional<T> {
  // CHECK: checked_cast_addr_br take_always @dynamic_self SelfCasts in {{.*}} : $*SelfCasts to T in {{.*}} : $*T
  // CHECK: }
  public static func classGenericFromSelfConditional<T : AnyObject>() -> T? {
    let s = Self()
    return s as? T
  }

  public required init() {}
}
