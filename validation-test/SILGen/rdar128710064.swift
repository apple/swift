// RUN: %target-swift-emit-silgen %s | %FileCheck %s
// RUN: %target-swift-emit-sil %s -verify

struct NC : ~Copyable {}

func loadClosure(_ umrp: UnsafeMutableRawPointer) {
  typealias Enumerator = (borrowing NC) -> Void
  let body = umrp.load(as: Enumerator.self)
  _ = body
}

// CHECK-LABEL: sil {{.*}} @$s13rdar1287100642NCVytIegnr_ACIegg_TR : {{.*}} {
// CHECK:       bb0(
// CHECK-SAME:      [[NC:%[^,]+]] :
// CHECK-SAME:    , [[CLOSURE:%[^,]+]] :
// CHECK-SAME:  ):
// CHECK:         [[NC_INDIRECT_ADDR:%[^,]+]] = alloc_stack $NC
// CHECK:         [[NC_INDIRECT_CHECK:%[^,]+]] = mark_unresolved_non_copyable_value [consumable_and_assignable] [[NC_INDIRECT_ADDR]]
// CHECK:         [[NC_INDIRECT:%[^,]+]] = store_borrow [[NC]] to [[NC_INDIRECT_CHECK]]
// CHECK:         [[OUT:%[^,]+]] = alloc_stack $()
// CHECK:         apply [[CLOSURE]]([[OUT]], [[NC_INDIRECT]])
// CHECK:         dealloc_stack [[OUT]] : $*()
// CHECK:         end_borrow [[NC_INDIRECT]]
// CHECK:         dealloc_stack [[NC_INDIRECT_ADDR]]
// CHECK-LABEL: } // end sil function '$s13rdar1287100642NCVytIegnr_ACIegg_TR'
