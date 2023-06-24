// RUN: %target-swift-frontend -emit-ir -disable-objc-interop %s | %FileCheck %s

// Note: -disable-objc-interop is used to give consistent results on Darwin
// and Linux, avoiding differences like %swift.refcounted -vs- %objc_object,
// etc.

public class SelfCasts {
  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc ptr @"$s17dynamic_self_cast9SelfCastsC02toD0yACXDACFZ"(ptr %0, ptr swiftself %1)
  // CHECK: call ptr @swift_dynamicCastClassUnconditional(ptr %0, ptr %1, ptr null, i32 0, i32 0)
  // CHECK: ret
  public static func toSelf(_ s: SelfCasts) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc ptr @"$s17dynamic_self_cast9SelfCastsC09genericToD0yACXDxlFZ"(ptr noalias nocapture %0, ptr %T, ptr swiftself %1)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %T, ptr %1, {{.*}})
  // CHECK: ret
  public static func genericToSelf<T>(_ s: T) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc ptr @"$s17dynamic_self_cast9SelfCastsC014classGenericToD0yACXDxRlzClFZ"(ptr %0, ptr %T, ptr swiftself %1)
  // CHECK: call ptr @swift_dynamicCastClassUnconditional(ptr %0, ptr %1, ptr null, i32 0, i32 0)
  // CHECK: ret
  public static func classGenericToSelf<T : AnyObject>(_ s: T) -> Self {
    return s as! Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc void @"$s17dynamic_self_cast9SelfCastsC011genericFromD0xylFZ"(ptr noalias nocapture sret({{.*}}) %0, ptr %T, ptr swiftself %1)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %1, ptr %T, {{.*}})
  // CHECK: ret
  public static func genericFromSelf<T>() -> T {
    let s = Self()
    return s as! T
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc ptr @"$s17dynamic_self_cast9SelfCastsC016classGenericFromD0xyRlzClFZ"(ptr %T, ptr swiftself %0)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %0, ptr %T, {{.*}})
  // CHECK: ret
  public static func classGenericFromSelf<T : AnyObject>() -> T {
    let s = Self()
    return s as! T
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc {{i32|i64}} @"$s17dynamic_self_cast9SelfCastsC02toD11ConditionalyACXDSgACFZ"(ptr %0, ptr swiftself %1)
  // CHECK: call ptr @swift_dynamicCastClass(ptr %0, ptr %1)
  // CHECK: ret
  public static func toSelfConditional(_ s: SelfCasts) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc {{i32|i64}} @"$s17dynamic_self_cast9SelfCastsC09genericToD11ConditionalyACXDSgxlFZ"(ptr noalias nocapture %0, ptr %T, ptr swiftself %1)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %T, ptr %1, {{.*}})
  // CHECK: ret
  public static func genericToSelfConditional<T>(_ s: T) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc {{i32|i64}} @"$s17dynamic_self_cast9SelfCastsC014classGenericToD11ConditionalyACXDSgxRlzClFZ"(ptr %0, ptr %T, ptr swiftself %1)
  // CHECK: call ptr @swift_dynamicCastClass(ptr %0, ptr %1)
  // CHECK: ret
  public static func classGenericToSelfConditional<T : AnyObject>(_ s: T) -> Self? {
    return s as? Self
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc void @"$s17dynamic_self_cast9SelfCastsC011genericFromD11ConditionalxSgylFZ"(ptr noalias nocapture sret({{.*}}) %0, ptr %T, ptr swiftself %1)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %1, ptr %T, {{.*}})
  // CHECK: ret
  public static func genericFromSelfConditional<T>() -> T? {
    let s = Self()
    return s as? T
  }

  // CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swiftcc {{i32|i64}} @"$s17dynamic_self_cast9SelfCastsC016classGenericFromD11ConditionalxSgyRlzClFZ"(ptr %T, ptr swiftself %0)
  // CHECK: call zeroext i1 @swift_dynamicCast(ptr {{%.*}}, ptr {{%.*}}, ptr %0, ptr %T, {{.*}})
  // CHECK: ret
  public static func classGenericFromSelfConditional<T : AnyObject>() -> T? {
    let s = Self()
    return s as? T
  }

  public required init() {}
}
