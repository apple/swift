// RUN: %target-swift-frontend %s -Onone -emit-ir -g -o - -parse-as-library -module-name a | %FileCheck %s
public protocol P {}
public class C : P {}
public struct S {}
public extension S {
  subscript<T>(_ val: T, _ type : T.Type = T.self) -> T? { return nil }
}

public func f() {
  S()[0]
}
// CHECK: !DISubprogram(name: "deinit"
// CHECK: !DISubprogram(name: "init"
// CHECK: !DISubprogram(name: "subscript
