// RUN: %target-swift-frontend -emit-ir %s -module-name x | %FileCheck %s

// rdar://problem/40078863 - witness signatures get adjusted and the
// ProtocolConformances accompanying them did not, resulting in an extra witness
// table parameter.

protocol Foo {
    func bar()
}
extension Foo {
    func bar() {}
}
class Box<T> {}
extension Box: Foo where T: Foo {}
// CHECK-LABEL: define internal swiftcc void @"$s1x3BoxCyqd__GAA3FooA2aEP3baryyFTW"(ptr noalias nocapture swiftself dereferenceable({{[48]}}) %0, ptr %Self, ptr %SelfWitnessTable)
