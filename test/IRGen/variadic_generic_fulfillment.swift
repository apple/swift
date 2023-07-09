// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking | %FileCheck %s -DINT=i%target-ptrsize

public struct G<T> {}

public struct GG<each T> {
  var elements: (repeat G<each T>)

  public func doStuff(input: repeat each T) {
    _ = (repeat each input)
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s28variadic_generic_fulfillment2GGV7doStuff5inputyxxQp_tF"(ptr noalias nocapture %0, ptr %"GG<repeat each T>", ptr noalias nocapture swiftself %1)
// CHECK: [[T_PTR:%.*]] = getelementptr inbounds ptr, ptr %"GG<repeat each T>"
// CHECK: [[T:%.*]] = load ptr, ptr [[T_PTR]]

// CHECK: [[INDEX:%.*]] = phi [[INT]] [ 0, %entry ], [ {{%.*}}, {{%.*}} ]

// Make sure we mask off the LSB since we have an on-heap pack here.

// CHECK: [[T_ADDR:%.*]] = ptrtoint ptr [[T]] to [[INT]]
// CHECK-NEXT: [[T_ADDR2:%.*]] = and [[INT]] [[T_ADDR]], -2
// CHECK-NEXT: [[T2:%.*]] = inttoptr [[INT]] [[T_ADDR2]] to ptr
// CHECK-NEXT: [[T_ELT_PTR:%.*]] = getelementptr inbounds ptr, ptr [[T2]], [[INT]] [[INDEX]]
// CHECK-NEXT: [[T_ELT:%.*]] = load ptr, ptr [[T_ELT_PTR]]

// CHECK: ret void
