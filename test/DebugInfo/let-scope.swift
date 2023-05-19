// RUN: %target-swift-frontend -g -emit-sil %s -parse-as-library -module-name a | %FileCheck %s
func use<T>(_ t: T) {}
public func f(value: (Int, Int)) {
  let (x, y) = value
  // CHECK: debug_value {{.*}}let, name "x", {{.*}}, scope [[LET:[0-9]+]]
  // CHECK: debug_value {{.*}}let, name "y", {{.*}}, scope [[LET]]
  use((x,y))
}
