// RUN: %target-swift-frontend %s -emit-ir | %FileCheck %s

public protocol A {}

public class AC : A{}

public class CVC<A1: AC> where A1: A {
  // CHECK-LABEL: define{{.*}} @"$s21superclass_constraint3CVCCACyxGycfc"
  public init() {
    // CHECK: [[A:%.*]] = alloca ptr
    // CHECK-NOT: ret ptr
    // CHECK: store ptr %0, ptr [[A]]
    // CHECK: ret ptr
    var a = self
  }
}
