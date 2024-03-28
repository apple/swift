// RUN: %target-swift-emit-silgen -enable-experimental-feature BorrowingSwitch %s | %FileCheck %s

struct Inner: ~Copyable {}

struct Outer: ~Copyable {
    var storedInner: Inner

    var readInner: Inner {
        _read { fatalError() }
    }

    var getInner: Inner {
        get { fatalError() }
    }
}

func use(_: borrowing Outer) {}
func use(_: borrowing Inner) {}

func temporary() -> Outer { fatalError() }

// CHECK-LABEL: sil {{.*}}@{{.*}}11borrowParam
// CHECK:  = mark_unresolved_non_copyable_value [no_consume_or_assign]
func borrowParam(x: borrowing Outer) {
    // CHECK: [[MARK:%.*]] = mark_unresolved_non_copyable_value [strict] [no_consume_or_assign]
    // CHECK: [[BORROW:%.*]] = begin_borrow [[MARK]]
    switch x {
    case let y:
        // CHECK: apply {{.*}}([[BORROW]])
        use(y)
    }
    // CHECK: end_borrow [[BORROW]]

    
    // CHECK: [[BORROW_OUTER:%.*]] = begin_borrow {{.*}} : $Outer
    // CHECK: [[BORROW_INNER:%.*]] = struct_extract [[BORROW_OUTER]]
    // CHECK: [[BORROW_FIX:%.*]] = begin_borrow [fixed] [[BORROW_INNER]]
    // CHECK: [[COPY_INNER:%.*]] = copy_value [[BORROW_FIX]]
    // CHECK: [[MARK:%.*]] = mark_unresolved_non_copyable_value [strict] [no_consume_or_assign] [[COPY_INNER]]
    // CHECK: [[BORROW:%.*]] = begin_borrow [[MARK]]
    switch x.storedInner {
    case let y:
        // CHECK: apply {{.*}}([[BORROW]])
        use(y)
    }
    // CHECK: end_borrow [[BORROW]]
    // CHECK: end_borrow [[BORROW_OUTER]]

    // CHECK: [[BORROW_OUTER:%.*]] = begin_borrow {{.*}} : $Outer
    // CHECK: ([[BORROW_INNER:%.*]], [[TOKEN:%.*]]) = begin_apply {{.*}}([[BORROW_OUTER]]
    // CHECK: [[COPY_INNER:%.*]] = copy_value [[BORROW_INNER]]
    // CHECK: [[MARK:%.*]] = mark_unresolved_non_copyable_value [no_consume_or_assign] [[COPY_INNER]]
    // CHECK: [[BORROW:%.*]] = begin_borrow [[MARK]]
    // CHECK: [[BORROW_FIX:%.*]] = begin_borrow [fixed] [[BORROW]]
    // CHECK: [[COPY2:%.*]] = copy_value [[BORROW_FIX]]
    // CHECK: [[MARK2:%.*]] = mark_unresolved_non_copyable_value [strict] [no_consume_or_assign] [[COPY2]]
    // CHECK: [[BORROW2:%.*]] = begin_borrow [[MARK2]]
    switch x.readInner {
    case let y:
        // CHECK: apply {{.*}}([[BORROW2]])
        use(y)
    }
    // CHECK: end_apply [[TOKEN]]
    // CHECK: end_borrow [[BORROW_OUTER]]

    // `temporary()` is an rvalue, so we 
    // CHECK: [[FN:%.*]] = function_ref @{{.*}}9temporary
    // CHECK: [[TMP:%.*]] = apply [[FN]]()
    // CHECK: [[BORROW_OUTER:%.*]] = begin_borrow [fixed] [[TMP]]
    // CHECK: end_borrow [[BORROW_OUTER]]
    // CHECK: store [[TMP]] to [init] [[Y:%.*]] :
    // CHECK: [[MARK:%.*]] = mark_unresolved_non_copyable_value [no_consume_or_assign] [[Y]]
    switch temporary() {
    case let y:
        // CHECK: [[LOAD_BORROW:%.*]] = load_borrow [[MARK]]
        // CHECK: apply {{.*}}([[LOAD_BORROW]])
        use(y)
    }
}
