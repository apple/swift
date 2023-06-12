// RUN: %target-swift-emit-silgen %s | %FileCheck %s
// RUN: %target-swift-frontend -emit-sil -verify %s

// rdar://109161396

public protocol P {}

@_moveOnly
public struct M {
   private var x: P
   var other: CInt { 0 }

   var otherMoveOnly: M {
       _read {
           yield self
       }
   }

   @_silgen_name("no")
   init()
}

// CHECK-LABEL: sil [ossa] @${{.*}}4test3mut
// CHECK: [[CHECK:%.*]] = mark_must_check [consumable_and_assignable] %0
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[CHECK]]
// CHECK: [[RESULT:%.*]] = apply {{.*}}([[ACCESS]])
// CHECK; end_access [[ACCESS]]
// CHECK: return [[RESULT]]
public func test(mut: inout M) -> CInt {
  return mut.other
}

// CHECK-LABEL: sil [ossa] @${{.*}}4test6borrow
// CHECK: [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] %0
// CHECK: [[RESULT:%.*]] = apply {{.*}}([[CHECK]])
// CHECK: return [[RESULT]]
public func test(borrow: borrowing M) -> CInt {
  return borrow.other
}

// CHECK-LABEL: sil [ossa] @${{.*}}4test7consume
// CHECK: [[BOX:%.*]] = project_box
// CHECK: [[ACCESS:%.*]] = begin_access [read] [unknown] [[BOX]]
// CHECK: [[CHECK:%.*]] = mark_must_check [no_consume_or_assign] [[ACCESS]]
// CHECK: [[RESULT:%.*]] = apply {{.*}}([[CHECK]])
// CHECK; end_access [[ACCESS]]
// CHECK: return [[RESULT]]
public func test(consume: consuming M) -> CInt {
  return consume.other
}

// CHECK-LABEL: sil [ossa] @${{.*}}4test3own
// CHECK: [[CHECK:%.*]] = mark_must_check [consumable_and_assignable] %0
// CHECK: [[RESULT:%.*]] = apply {{.*}}([[CHECK]])
// CHECK: return [[RESULT]]
public func test(own: __owned M) -> CInt {
  return own.other
}

func use(_: CInt, andMutate _: inout M) {}
func use(_: CInt, andConsume _: consuming M) {}
func borrow(_: borrowing M, andMutate _: inout M) {}
func borrow(_: borrowing M, andConsume _: consuming M) {}

public func testNoInterferenceGet(mut: inout M, extra: consuming M) {
    // This should not cause exclusivity interference, since the result of
    // the getter can have an independent lifetime from the borrow.
    use(mut.other, andMutate: &mut)
    use(mut.other, andConsume: mut)
    mut = extra
}

public func testInterferenceRead(mut: inout M, extra: consuming M) {
    // This should cause exclusivity interference, since in order to borrow
    // the yielded result from the `_read`, we need to keep the borrow of
    // the base going.
    borrow(mut.otherMoveOnly, andMutate: &mut) // expected-error{{}} expected-note{{}}
    borrow(mut.otherMoveOnly, andConsume: mut) // expected-error{{}} expected-note{{}}
    mut = extra
}
