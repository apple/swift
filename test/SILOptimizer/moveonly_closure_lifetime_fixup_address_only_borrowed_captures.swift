// RUN: %target-swift-frontend -emit-sil %s | %FileCheck %s

// When ClosureLifetimeFixup promotes a closure to the stack, we should
// eliminate the temporary copy of any borrowed move-only captures, so that
// they get checked as borrows and not consumes.

struct E<T>: ~Copyable {
    var t: T
}

var escaper: () -> () = {}

func nonescaping(_ x: () -> ()) { }
func escaping(_ x: @escaping () -> ()) {  escaper = x }

func borrow<T>(_: borrowing E<T>) {}

// CHECK-LABEL: sil {{.*}}16testNeverEscaped
func testNeverEscaped<T>(_ e: borrowing E<T>) {
    // CHECK: partial_apply {{.*}}[on_stack] {{.*}}(%0) :
    nonescaping {
        borrow(e)
    }
}

func nonescaping(_ x: () -> (), and y: () -> ()) {}

// CHECK-LABEL: sil {{.*}}21testNeverEscapedTwice
func testNeverEscapedTwice<T>(_ e: borrowing E<T>) {
    // CHECK: partial_apply {{.*}}[on_stack] {{.*}}(%0) :
    // CHECK: partial_apply {{.*}}[on_stack] {{.*}}(%0) :
    nonescaping({ borrow(e) }, and: { borrow(e) })
}

// CHECK-LABEL: sil {{.*}}16testEscapedLater
func testEscapedLater<T>(_ e: consuming E<T>) {
    // CHECK: [[BOX:%.*]] = alloc_box
    // CHECK: [[CONTENTS:%.*]] = project_box [[BOX]]
    // CHECK: partial_apply {{.*}}[on_stack] {{.*}}([[CONTENTS]])
    nonescaping {
        borrow(e)
    }

    escaping {
        borrow(e)
    }
}

// CHECK-LABEL: sil {{.*}}17testEscapedLater2
func testEscapedLater2<T>(_ e: consuming E<T>) {
    // CHECK: [[BOX:%.*]] = alloc_box
    // CHECK: [[CONTENTS:%.*]] = project_box [[BOX]]
    // CHECK: partial_apply {{.*}}[on_stack] {{.*}}([[CONTENTS]])
    let f = e

    nonescaping {
        borrow(f)
    }

    escaping {
        borrow(f)
    }
}

