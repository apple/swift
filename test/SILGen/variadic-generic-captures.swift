// RUN: %target-swift-emit-silgen %s | %FileCheck %s

func noEscape(_ fn: () -> ()) {
  fn()
}

// CHECK-LABEL: sil hidden [ossa] @$s4main19formNoEscapeClosure1xyxxQp_tRvzlF : $@convention(thin) <each T> (@pack_guaranteed Pack{repeat each T}) -> () {
// CHECK: [[TUPLE:%.*]] = alloc_stack $(repeat each T)

// CHECK: [[IDX:%.*]] = dynamic_pack_index %8 of $Pack{repeat each T}
// CHECK: open_pack_element [[IDX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid "[[UUID:.*]]"
// CHECK: [[PACK_ELT:%.*]] = pack_element_get [[IDX]] of %0 : $*Pack{repeat each T} as $*@pack_element("[[UUID]]") each T
// CHECK: [[TUPLE_ADDR:%.*]] = tuple_pack_element_addr [[IDX]] of [[TUPLE]] : $*(repeat each T) as $*@pack_element("[[UUID]]") each T
// CHECK: copy_addr [[PACK_ELT]] to [init] [[TUPLE_ADDR]] : $*@pack_element("[[UUID]]") each T

func formNoEscapeClosure<each T>(x: repeat each T) {
  noEscape {
    let _ = (repeat print(each x))
  }
}

// CHECK-LABEL: sil private [ossa] @$s4main19formNoEscapeClosure1xyxxQp_tRvzlFyyXEfU_ : $@convention(thin) <each T> (@in_guaranteed (repeat each T)) -> () {
// CHECK: bb0(%0 : @closureCapture $*(repeat each T)):
// CHECK: [[PACK:%.*]] = alloc_pack $Pack{repeat each T}

// CHECK: [[IDX:%.*]] = dynamic_pack_index {{%.*}} of $Pack{repeat each T}
// CHECK: open_pack_element [[IDX]] of <each T> at <Pack{repeat each T}>, shape $each T, uuid "[[UUID:.*]]"
// CHECK: [[TUPLE_ELT:%.*]] = tuple_pack_element_addr [[IDX]] of %0 : $*(repeat each T) as $*@pack_element("[[UUID]]") each T
// CHECK: pack_element_set [[TUPLE_ELT]] : $*@pack_element("[[UUID]]") each T into [[IDX]] of [[PACK]] : $*Pack{repeat each T}

func formEscapingClosure<each T>(x: repeat each T) -> () -> () {
  return {
    let _ = (repeat print(each x))
  }
}

