// RUN: %empty-directory(%t)
// RUN: %target-swift-frontend -emit-module  -disable-availability-checking -enable-library-evolution -emit-module-path=%t/resilient_class.swiftmodule -module-name=resilient_class %S/Inputs/resilient_class.swift
// RUN: %target-swift-frontend -I %t -emit-ir  -disable-availability-checking -enable-library-evolution %s | %FileCheck -check-prefix CHECK -check-prefix CHECK-%target-cpu -check-prefix CHECK-%target-import-type %s
// REQUIRES: concurrency

import resilient_class

open class MyBaseClass<T> {
  var value: T

  open func wait() async -> Int {
    return 0
  }

  open func wait() async -> T {
    return value
  }

  open func waitThrows() async throws -> Int {
    return 0
  }

  open func waitThrows() async throws -> T {
    return value
  }

  // FIXME
  // open func waitGeneric<T>(_: T) async -> T
  // open func waitGenericThrows<T>(_: T) async throws -> T

  public init(_ value: T) {
    self.value = value
  }
}

// CHECK-LABEL: @"$s16class_resilience11MyBaseClassC4waitxyYaFTjTu" = {{(dllexport )?}}{{(protected )?}}global %swift.async_func_pointer

// CHECK-LABEL: @"$s16class_resilience11MyBaseClassCMn" = {{(dllexport )?}}{{(protected )?}}constant
// CHECK-SAME: ptr @"$s16class_resilience11MyBaseClassC4waitxyYaFTu"

// CHECK-LABEL: @"$s16class_resilience9MyDerivedCMn" = hidden constant
// CHECK-SAME: ptr @"$s16class_resilience9MyDerivedC4waitSiyYaF010resilient_A09BaseClassCADxyYaFTVTu"

// CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swift{{(tail)?}}cc void @"$s16class_resilience14callsAwaitableyx010resilient_A09BaseClassCyxGYalF"(ptr noalias nocapture %0, ptr swiftasync %1{{.*}})
// CHECK-DIRECT: ptr @"$s15resilient_class9BaseClassC4waitxyYaFTjTu"
// CHECK-INDIRECT: [[LOAD:%[0-9]+]] = load ptr, ptr inttoptr (i64 and (i64 add (i64 ptrtoint (ptr @"\01__imp_$s15resilient_class9BaseClassC4waitxyYaFTjTu" to i64), i64 1), i64 -2) to ptr), align {{4|8}}
// CHECK-INDIRECT-NEXT: select i1 icmp eq (i64 and (i64 add (i64 ptrtoint (ptr @"\01__imp_$s15resilient_class9BaseClassC4waitxyYaFTjTu" to i64), i64 1), i64 1), i64 0),
// CHECK-INDIRECT-SAME: ptr inttoptr (i64 add (i64 ptrtoint (ptr @"\01__imp_$s15resilient_class9BaseClassC4waitxyYaFTjTu" to i64), i64 1) to ptr),
// CHECK-INDIRECT-SAME: ptr [[LOAD]]
// CHECK: ret void
public func callsAwaitable<T>(_ c: BaseClass<T>) async -> T {
  return await c.wait()
}

// CHECK-LABEL: define {{(dllexport )?}}{{(protected )?}}swift{{(tail)?}}cc void @"$s16class_resilience11MyBaseClassC4waitxyYaFTj"(ptr noalias nocapture %0, ptr swiftasync %1, ptr swiftself %2) {{#([0-9]+)}} {

class MyDerived : BaseClass<Int> {
  override func wait() async -> Int {
    return await super.wait()
  }
}
