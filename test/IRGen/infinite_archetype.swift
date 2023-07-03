// RUN: %target-swift-frontend -primary-file %s -emit-ir | %FileCheck %s

// REQUIRES: CPU=i386 || CPU=x86_64

protocol Fooable {
  associatedtype Foo
}

// CHECK: define hidden swiftcc void @"$s18infinite_archetype3foo{{[_0-9a-zA-Z]*}}F"(ptr noalias nocapture sret({{.*}}) %0, ptr noalias nocapture %1, ptr %T, ptr %T.Fooable)
func foo<T: Fooable>(x: T) -> T where T == T.Foo { return x }
