// RUN: %target-swift-frontend -emit-ir %s -disable-availability-checking | %FileCheck %s -DINT=i%target-ptrsize

public struct G<T> {}

public struct GG<each T> {
  var elements: (repeat G<each T>)

  public func doStuff(input: repeat each T) {
    _ = (repeat each input)
  }
}

// CHECK-LABEL: define{{( dllexport)?}}{{( protected)?}} swiftcc void @"$s28variadic_generic_fulfillment2GGV7doStuff5inputyxxQp_tF"(%swift.opaque** noalias nocapture %0, %swift.type* %"GG<repeat each T>", %T28variadic_generic_fulfillment2GGV* noalias nocapture swiftself %1)
// CHECK: [[METADATA:%.*]] = bitcast %swift.type* %"GG<repeat each T>" to %swift.type***
// CHECK: [[T_PTR:%.*]] = getelementptr inbounds %swift.type**, %swift.type*** [[METADATA]]
// CHECK: [[T:%.*]] = load %swift.type**, %swift.type*** [[T_PTR]]

// CHECK: [[INDEX:%.*]] = phi [[INT]] [ 0, %entry ], [ {{%.*}}, {{%.*}} ]

// Make sure we mask off the LSB since we have an on-heap pack here.

// CHECK: [[T_ADDR:%.*]] = ptrtoint %swift.type** [[T]] to [[INT]]
// CHECK-NEXT: [[T_ADDR2:%.*]] = and [[INT]] [[T_ADDR]], -2
// CHECK-NEXT: [[T2:%.*]] = inttoptr [[INT]] [[T_ADDR2]] to %swift.type**
// CHECK-NEXT: [[T_ELT_PTR:%.*]] = getelementptr inbounds %swift.type*, %swift.type** [[T2]], [[INT]] [[INDEX]]
// CHECK-NEXT: [[T_ELT:%.*]] = load %swift.type*, %swift.type** [[T_ELT_PTR]]

// CHECK: ret void
