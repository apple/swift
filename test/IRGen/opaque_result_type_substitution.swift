// RUN: %target-swift-frontend -disable-type-layout -enable-library-evolution -disable-availability-checking -emit-ir -primary-file %s | %FileCheck %s

public protocol E {}

public struct Pair<T, V> : E {
  var fst : T
  var snd : V

  public init(_ f: T, _ s: V) {
    self.fst = f
    self.snd = s
  }

  public func foobar() -> some E {
    return self
  }
}

@inlinable
public func usePair<T, V>(_ t: T, _ v: V) {
  var x = Pair(t, v)
  let q = x.foobar()
  let u = x.foobar()
  let p = Pair(q, u)
  print(p)
}

// CHECK-LABEL: define{{.*}} swiftcc void @"$s31opaque_result_type_substitution7usePairyyx_q_tr0_lF"({{.*}}, ptr %T, ptr %V)
// CHECK:  [[PAIR_TV:%.*]] = call swiftcc %swift.metadata_response @"$s31opaque_result_type_substitution4PairVMa"({{.*}}, ptr %T, ptr %V)
// CHECK:  [[MD:%.*]] = extractvalue %swift.metadata_response [[PAIR_TV]], 0
// CHECK:  [[PAIR_OPAQUE:%.*]] = call swiftcc %swift.metadata_response @"$s31opaque_result_type_substitution4PairVMa"({{.*}}, ptr [[MD]], ptr [[MD]])
// CHECK:  [[MD2:%.*]] = extractvalue %swift.metadata_response [[PAIR_OPAQUE]], 0
// CHECK:  call ptr @"$s31opaque_result_type_substitution4PairVyACyxq_GADGr0_lWOh"({{.*}}, ptr {{.*}}, ptr [[MD2]])
