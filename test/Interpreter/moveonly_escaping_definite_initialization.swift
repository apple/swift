// RUN: %target-run-simple-swift | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

@_moveOnly
struct MO {
    var value: Int

    func use() {}

    deinit { print("destroying \(value)") }
}

var closure: () -> () = {}
var counter = 42

func foo(goUp: Bool) {
    let mo: MO

    if goUp {
        counter += 1
        mo = MO(value: counter)
    } else {
        counter -= 1
        mo = MO(value: counter)
    }

    mo.use()

    closure = { print("captured \(mo.value)") }
}

// CHECK:      starting
print("starting")
// CHECK-NEXT: captured 43
foo(goUp: true)
closure()
// CHECK-NEXT: destroying 43
// CHECK-NEXT: captured 44
foo(goUp: true)
closure()
// CHECK-NEXT: destroying 44
// CHECK-NEXT: captured 43
foo(goUp: false)
closure()
// CHECK-NEXT: destroying 43
closure = {}

// CHECK-NEXT: starting bar
print("starting bar")
func bar() {
    var y: MO

    y = MO(value: 17)
    // CHECK-NEXT: destroying 17
    y = MO(value: 38)

    closure = { y = MO(value: 679) }

    y.use()
}
bar()
// CHECK-NEXT: destroying 38
closure()
// CHECK-NEXT: done with bar
print("done with bar")
// CHECK-NEXT: destroying 679
closure = {}

// CHECK-NEXT: done
print("done")

