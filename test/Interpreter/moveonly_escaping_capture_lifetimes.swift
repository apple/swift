// RUN: %target-run-simple-swift | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

@_moveOnly
struct MO {
    var x: Int
    deinit { print("dying \(x)") }
}

var f: () -> () = {}

func foo(x: consuming MO) {
    var counter = 42
    f = {
        x = MO(x: counter)
        counter += 1
    }
}

func main() {
    let x = MO(x: 69)

    // CHECK: a
    print("a")
    foo(x: x)
    // CHECK-NEXT: b
    print("b")
    // CHECK-NEXT: dying 69
    // CHECK-NEXT: c
    f()
    print("c")
    // CHECK-NEXT: dying 42
    // CHECK-NEXT: d
    f()
    print("d")
    // CHECK-NEXT: dying 43
    // CHECK-NEXT: e
    f()
    print("e")
    // CHECK-NEXT: dying 44
    // CHECK-NEXT: f
    f = {}
    print("f")
}
main()
print("done")
