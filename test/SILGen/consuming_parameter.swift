// RUN: %target-swift-emit-silgen %s | %FileCheck %s

func bar(_: String) {}

// CHECK-LABEL: sil {{.*}} @${{.*}}3foo
func foo(y: consuming String, z: String) -> () -> String {
    // CHECK: bb0(%0 : @noImplicitCopy @_eagerMove @owned $String, %1 : @guaranteed $String):
    // CHECK:   [[BOX:%.*]] = alloc_box ${ var @moveOnly String }
    // CHECK:   [[Y:%.*]] = project_box [[BOX]]
    // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[Y]]
    // CHECK:   store %0 to [init] [[UNWRAP]]

    // CHECK:   [[YCAPTURE:%.*]] = copy_value [[BOX]]
    // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_box [[YCAPTURE]]
    // CHECK:   partial_apply {{.*}} {{%.*}}([[UNWRAP]])
    let r = { y }

    // CHECK:   [[ZCOPY:%.*]] = copy_value %1
    // CHECK:   [[YACCESS:%.*]] = begin_access [modify] [unknown] [[Y]]
    // CHECK:   [[MARK:%.*]] = mark_must_check [assignable_but_not_consumable] [[YACCESS]]
    // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[MARK]]
    // CHECK:   assign [[ZCOPY]] to [[UNWRAP]]
    y = z

    // CHECK:   [[YACCESS:%.*]] = begin_access [read] [unknown] [[Y]]
    // CHECK:   [[MARK:%.*]] = mark_must_check [no_consume_or_assign] [[YACCESS]]
    // CHECK:   [[YVAL:%.*]] = load [copy] [[MARK]]
    // CHECK:   [[BORROW:%.*]] = begin_borrow [[YVAL]]
    // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[BORROW]]
    // CHECK:   apply {{%.*}}([[UNWRAP]]
    bar(y)

    return r
}

struct Butt {
    var value: String

    func bar() {}

    // CHECK-LABEL: sil {{.*}} @${{.*}}4Butt{{.*}}6merged
    consuming func merged(with other: Butt) -> () -> Butt {
        // CHECK: bb0(%0 : @guaranteed $Butt, %1 : @noImplicitCopy @_eagerMove @owned $Butt):
        // CHECK:   [[BOX:%.*]] = alloc_box ${ var @moveOnly Butt }
        // CHECK:   [[SELF:%.*]] = project_box [[BOX]]
        // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[SELF]]
        // CHECK:   store %1 to [init] [[UNWRAP]]

        // CHECK:   [[SELFCAPTURE:%.*]] = copy_value [[BOX]]
        // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_box [[SELFCAPTURE]]
        // CHECK:   partial_apply {{.*}} {{%.*}}([[UNWRAP]])
        let r = { self }

        // CHECK:   [[OCOPY:%.*]] = copy_value %0
        // CHECK:   [[SELFACCESS:%.*]] = begin_access [modify] [unknown] [[SELF]]
        // CHECK:   [[MARK:%.*]] = mark_must_check [assignable_but_not_consumable] [[SELFACCESS]]
        // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable_addr [[MARK]]
        // CHECK:   assign [[OCOPY]] to [[UNWRAP]]
        self = other

        // CHECK:   [[SELFACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
        // CHECK:   [[MARK:%.*]] = mark_must_check [no_consume_or_assign] [[SELFACCESS]]
        // CHECK:   [[SELFVAL:%.*]] = load [copy] [[MARK]]
        // CHECK:   [[BORROW:%.*]] = begin_borrow [[SELFVAL]]
        // CHECK:   [[UNWRAP:%.*]] = moveonlywrapper_to_copyable [guaranteed] [[BORROW]]
        // CHECK:   apply {{%.*}}([[UNWRAP]]

        self.bar()

        return r
    }
}
