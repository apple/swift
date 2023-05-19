// RUN: %target-swift-emit-silgen -enable-experimental-feature NoImplicitCopy %s | %FileCheck %s

func bar(_: String) {}

// CHECK-LABEL: sil {{.*}} @${{.*}}3foo
func foo(y: consuming String, z: String) -> () -> String {
    // CHECK: bb0(%0 : @_eagerMove @owned $String, %1 : @guaranteed $String):
    // CHECK:   [[BOX:%.*]] = alloc_box ${ var String }
    // CHECK:   [[Y:%.*]] = project_box [[BOX]]
    // CHECK:   store %0 to [init] [[Y]]

    // CHECK:   [[YCAPTURE:%.*]] = copy_value [[BOX]]
    // CHECK:   partial_apply {{.*}} {{%.*}}([[YCAPTURE]])
    let r = { y }

    // CHECK:   [[ZCOPY:%.*]] = copy_value %1
    // CHECK:   [[YACCESS:%.*]] = begin_access [modify] [unknown] [[Y]]
    // CHECK:   assign [[ZCOPY]] to [[YACCESS]]
    y = z

    // CHECK:   [[YACCESS:%.*]] = begin_access [read] [unknown] [[Y]]
    // CHECK:   [[YVAL:%.*]] = load [copy] [[YACCESS]]
    // CHECK:   apply {{%.*}}([[YVAL]]
    bar(y)

    return r
}

struct Butt {
    var value: String

    func bar() {}

    // CHECK-LABEL: sil {{.*}} @${{.*}}4Butt{{.*}}6merged
    consuming func merged(with other: Butt) -> () -> Butt {
        // CHECK: bb0(%0 : @guaranteed $Butt, %1 : @_eagerMove @owned $Butt):
        // CHECK:   [[BOX:%.*]] = alloc_box ${ var Butt }
        // CHECK:   [[SELF:%.*]] = project_box [[BOX]]
        // CHECK:   store %1 to [init] [[SELF]]

        // CHECK:   [[SELFCAPTURE:%.*]] = copy_value [[BOX]]
        // CHECK:   partial_apply {{.*}} {{%.*}}([[SELFCAPTURE]])
        let r = { self }

        // CHECK:   [[OCOPY:%.*]] = copy_value %0
        // CHECK:   [[SELFACCESS:%.*]] = begin_access [modify] [unknown] [[SELF]]
        // CHECK:   assign [[OCOPY]] to [[SELFACCESS]]
        self = other

        // CHECK:   [[SELFACCESS:%.*]] = begin_access [read] [unknown] [[SELF]]
        // CHECK:   [[SELFVAL:%.*]] = load [copy] [[SELFACCESS]]
        // CHECK:   apply {{%.*}}([[SELFVAL]]

        self.bar()

        return r
    }
}
