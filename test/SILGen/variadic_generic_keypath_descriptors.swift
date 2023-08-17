// RUN: %target-swift-emit-silgen -enable-library-evolution -disable-availability-checking %s | %FileCheck %s

// rdar://problem/112474421

public struct G<each T> {
   public var property1: (repeat each T) -> ()
   public var property2: ((repeat each T)) -> ()
   public var property3: (G<repeat each T>) -> ()
}

// We don't attempt to emit a keypath descriptor for property1 and property2,
// and we shouldn't crash while emitting a keypath descriptor for property3.

// CHECK-NOT: sil_property #G.property1<each τ_0_0>
// CHECK-NOT: sil_property #G.property2<each τ_0_0>
// CHECK-LABEL: sil_property #G.property3<each τ_0_0>