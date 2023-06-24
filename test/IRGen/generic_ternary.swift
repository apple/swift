// RUN: %target-swift-frontend %use_no_opaque_pointers -primary-file %s -emit-ir | %FileCheck %s
// RUN: %target-swift-frontend -primary-file %s -emit-ir

// REQUIRES: CPU=i386 || CPU=x86_64

// <rdar://problem/13793646>
struct OptionalStreamAdaptor<T : IteratorProtocol> {
  // CHECK: define hidden swiftcc void @"$s15generic_ternary21OptionalStreamAdaptorV4next{{[_0-9a-zA-Z]*}}F"(%swift.opaque* noalias nocapture sret({{.*}}) %0, %swift.type* %"OptionalStreamAdaptor<T>", %T15generic_ternary21OptionalStreamAdaptorV* nocapture swiftself dereferenceable({{.*}}) %1)
  mutating
  func next() -> Optional<T.Element> {
    return x[0].next()
  }
  var x: [T]
}
