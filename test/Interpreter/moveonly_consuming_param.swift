// RUN: %target-run-simple-swift | %FileCheck %s
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all) | %FileCheck %s

// REQUIRES: executable_test

@_moveOnly
struct Butt {
    var value: Int
    deinit { print("disposing \(value)") }

    mutating func merge(with: borrowing Butt) {
        value += with.value
    }

    func report() {
        print("got \(value)")
    }
}

func foo(x: consuming Butt, y: borrowing Butt) -> Butt {
    x.merge(with: y)

    return x
}

func main() {
    let x = Butt(value: 17)

    // CHECK: disposing 38
    let y = foo(x: x, y: Butt(value: 38))

    // CHECK-NEXT: got 55
    y.report()
    // CHECK-NEXT: disposing 55
}
main()
