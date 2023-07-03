// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend -emit-module -enable-library-evolution -emit-module-path=%t/resilient_class_thunks.swiftmodule -module-name=resilient_class_thunks %S/../Inputs/resilient_class_thunks.swift
// RUN: %target-swift-frontend -I %t -emit-ir %s | %FileCheck %s --check-prefix=CHECK --check-prefix=CHECK-%target-ptrsize -DINT=i%target-ptrsize
// RUN: %target-swift-frontend -I %t -emit-ir -O %s

import resilient_class_thunks

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s23class_resilience_thunks21testDispatchThunkBase1b1ty010resilient_a1_C00G0CyxG_xtlF"(ptr %0, ptr noalias nocapture %1)
public func testDispatchThunkBase<T>(b: Base<T>, t: T) {

  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC6takesTyyxFTj"(ptr noalias nocapture {{%.*}}, ptr swiftself %0)
  b.takesT(t)

  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC8takesIntyySiFTj"([[INT]] 0, ptr swiftself %0)
  b.takesInt(0)

  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC14takesReferenceyyAA6ObjectCFTj"(ptr {{%.*}}, ptr swiftself %0)
  b.takesReference(Object())

  // CHECK: ret void
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s23class_resilience_thunks24testDispatchThunkDerived1dy010resilient_a1_C00G0C_tF"(ptr %0)
public func testDispatchThunkDerived(d: Derived) {

  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC6takesTyyxFTj"(ptr noalias nocapture {{%.*}}, ptr swiftself {{%.*}})
  d.takesT(0)

  // CHECK: call swiftcc void @"$s22resilient_class_thunks7DerivedC8takesIntyySiSgFTj"([[INT]] 0, i8 1, ptr swiftself %0)
  d.takesInt(nil)

  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC14takesReferenceyyAA6ObjectCFTj"(ptr null, ptr swiftself {{%.*}})
  d.takesReference(nil)

  // CHECK: ret void
}

// We had a bug where if a non-resilient class overrides a method from a
// resilient class, calling the override would directly access the vtable
// entry of the resilient class, instead of going through the dispatch
// thunk.

open class MyDerived : Base<Int> {
  // Override has different formal type but is ABI-compatible
  open override func takesReference(_: Object) {}
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s23class_resilience_thunks27testDispatchThunkMyOverride1d1oyAA0G7DerivedC_010resilient_a1_C06ObjectCtF"(ptr %0, ptr %1)
public func testDispatchThunkMyOverride(d: MyDerived, o: Object) {
  // CHECK: call swiftcc void @"$s22resilient_class_thunks4BaseC14takesReferenceyyAA6ObjectCFTj"
  d.takesReference(o)

  // CHECK: ret void
}

public func testDispatchThunkCast(d: Derived) {
  _ = d.returnsSuperclass()
}
