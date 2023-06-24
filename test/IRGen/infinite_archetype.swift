// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir

// REQUIRES: CPU=i386 || CPU=x86_64

protocol Fooable {
  associatedtype Foo
}

// CHECK: define hidden swiftcc void @"$s18infinite_archetype3foo{{[_0-9a-zA-Z]*}}F"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.opaque* noalias nocapture %1, %swift.type* %T, i8** %T.Fooable)
func foo<T: Fooable>(x: T) -> T where T == T.Foo { return x }
