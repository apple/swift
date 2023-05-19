// RUN: %target-swift-emit-silgen -disable-availability-checking %s | %FileCheck %s

// rdar://107459964
// rdar://107478603

public struct G<Value> {
  public let id: Int
}

public struct Holder<each T> {
  public let values: (repeat G<each T>)
}

// CHECK-LABEL: sil {{.*}}@$s4main21testInstanceVarAccessyAA1GVyxGAA6HolderVyx_QPGlF :
// CHECK:       [[T0:%.*]] = struct_extract %0 : $Holder<X>, #Holder.values
// CHECK-NEXT:  return [[T0]] : $G<X>
public func testInstanceVarAccess<X>(_ holder: Holder<X>) -> G<X> {
  return holder.values
}

extension Holder {
  public static var allValues: (repeat G<each T>) {
    return (repeat G<each T>(id: 0))
  }
}

// CHECK-LABEL: sil {{.*}}@$s4main19testStaticVarAccessAA1GVyxGylF :
// CHECK:       [[METATYPE:%.*]] = metatype $@thin Holder<X>.Type
// CHECK-NEXT:  [[PACK:%.*]] = alloc_pack $Pack{G<X>}
// CHECK-NEXT:  [[TEMP:%.*]] = alloc_stack $G<X>
// CHECK-NEXT:  [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{G<X>}
// CHECK-NEXT:  pack_element_set [[TEMP]] : $*G<X> into [[INDEX]] of [[PACK]] :
// CHECK-NEXT:  // function_ref
// CHECK-NEXT:  [[FN:%.*]] = function_ref @$s4main6HolderV9allValuesAA1GVyxGxQp_tvgZ : $@convention(method) <each τ_0_0> (@thin Holder<repeat each τ_0_0>.Type) -> @pack_out Pack{repeat G<each τ_0_0>}
// CHECK-NEXT:  apply [[FN]]<Pack{X}>([[PACK]], [[METATYPE]])
// CHECK-NEXT:  [[T0:%.*]] = load [trivial] [[TEMP]] : $*G<X>
// CHECK-NEXT:  dealloc_stack [[TEMP]]
// CHECK-NEXT:  dealloc_pack [[PACK]]
// CHECK-NEXT:  return [[T0]] : $G<X>
public func testStaticVarAccess<X>() -> G<X> {
  return Holder<X>.allValues
}

extension Holder {
  static func takeExpansion(arg: (repeat G<each T>)) {}
  static func takePartial(arg: (Int, repeat G<each T>)) {}
}

// CHECK-LABEL: sil {{.*}}@$s4main23testArgPassingExpansion3argyAA1GVyxG_tlF :
// CHECK:       [[METATYPE:%.*]] = metatype $@thin Holder<X>.Type
// CHECK-NEXT:  [[PACK:%.*]] = alloc_pack $Pack{G<X>}
// CHECK-NEXT:  [[TEMP:%.*]] = alloc_stack $G<X>
// CHECK-NEXT:  store %0 to [trivial] [[TEMP]] : $*G<X>
// CHECK-NEXT:  [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{G<X>}
// CHECK-NEXT:  pack_element_set [[TEMP]] : $*G<X> into [[INDEX]] of [[PACK]] :
// CHECK-NEXT:  // function_ref
// CHECK-NEXT:  [[FN:%.*]] = function_ref @$s4main6HolderV13takeExpansion3argyAA1GVyxGxQp_t_tFZ : $@convention(method) <each τ_0_0> (@pack_guaranteed Pack{repeat G<each τ_0_0>}, @thin Holder<repeat each τ_0_0>.Type) -> ()
// CHECK-NEXT:  apply [[FN]]<Pack{X}>([[PACK]], [[METATYPE]])
// CHECK-NEXT:  dealloc_stack [[TEMP]]
// CHECK-NEXT:  dealloc_pack [[PACK]]
public func testArgPassingExpansion<X>(arg: G<X>) {
  Holder<X>.takeExpansion(arg: arg)
}

// CHECK-LABEL: sil {{.*}}@$s4main21testArgPassingPartialyyF :
// CHECK:       [[METATYPE:%.*]] = metatype $@thin Holder<>.Type
// CHECK:       [[PACK:%.*]] = alloc_pack $Pack{}
// CHECK-NEXT:  // function_ref
// CHECK-NEXT:  [[FN:%.*]] = function_ref @$s4main6HolderV11takePartial3argySi_AA1GVyxGxQpt_tFZ : $@convention(method) <each τ_0_0> (Int, @pack_guaranteed Pack{repeat G<each τ_0_0>}, @thin Holder<repeat each τ_0_0>.Type) -> ()
// CHECK-NEXT:  apply [[FN]]<Pack{}>({{.*}}, [[PACK]], [[METATYPE]])
// CHECK-NEXT:  dealloc_pack [[PACK]]
public func testArgPassingPartial() {
  Holder< >.takePartial(arg: 0)
}

func makeTuple<each T>(_ t: repeat each T) -> (repeat each T) {
  return (repeat each t)
}

// rdar://107972801
// CHECK-LABEL: sil {{.*}}@$s4main7makeOneyxxlF : $@convention(thin) <T> (@in_guaranteed T) -> @out T {
// CHECK:       bb0(%0 : $*T, %1 : $*T):
// CHECK:         [[RET_PACK:%.*]] = alloc_pack $Pack{T}
// CHECK-NEXT:    [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{T}
// CHECK-NEXT:    pack_element_set %0 : $*T into [[INDEX]] of [[RET_PACK]] :
// CHECK-NEXT:    [[ARG_PACK:%.*]] = alloc_pack $Pack{T}
// CHECK-NEXT:    [[TEMP:%.*]] = alloc_stack $T
// CHECK-NEXT:    copy_addr %1 to [init] [[TEMP]] : $*T
// CHECK-NEXT:    [[INDEX:%.*]] = scalar_pack_index 0 of $Pack{T}
// CHECK-NEXT:    pack_element_set [[TEMP]] : $*T into [[INDEX]] of [[ARG_PACK]] :
// CHECK-NEXT:    // function_ref
// CHECK-NEXT:    [[FN:%.*]] = function_ref @$s4main9makeTupleyxxQp_txxQpRvzlF : $@convention(thin) <each τ_0_0> (@pack_guaranteed Pack{repeat each τ_0_0}) -> @pack_out Pack{repeat each τ_0_0}
// CHECK-NEXT:    apply [[FN]]<Pack{T}>([[RET_PACK]], [[ARG_PACK]])
// CHECK-NEXT:    destroy_addr [[TEMP]] : $*T
// CHECK-NEXT:    dealloc_stack [[TEMP]] : $*T
// CHECK-NEXT:    dealloc_pack [[ARG_PACK]] : $*Pack{T}
// CHECK-NEXT:    dealloc_pack [[RET_PACK]] : $*Pack{T}
public func makeOne<T>(_ t: T) -> T {
  return makeTuple(t)
}
