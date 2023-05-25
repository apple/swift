// RUN: %target-run-simple-swift(-Xfrontend -enable-experimental-move-only)
// RUN: %target-run-simple-swift(-O -Xfrontend -sil-verify-all -Xfrontend -enable-experimental-move-only)
// REQUIRES: executable_test

// Verify that iterating through the fields of an object whose class has
// a move-only field does not trap from trying to reflect and copy those
// move-only fields.

@_moveOnly
public struct MO {
    var x: Int8 = 0
    var y: Int8 = 0
    var z: Int8 = 0

    deinit { print("destroyed MO") }
}

public class MOHaver {
    var s: String = "hello"
    var mo: MO = MO()
    var b: Int8 = 42
    var c: Any.Type = MOHaver.self
}

// CHECK-LABEL: doing nongeneric
print("doing nongeneric")
do {
    let k = MOHaver()

    let mirror = Mirror(reflecting: k)
    // CHECK-NEXT: s: hello
    // Whether this actually prints the value of `k.mo` or not is irrelevant
    // to the test; we care that attempting to reflect the field does not trap
    // copying a noncopyable field.
    // CHECK-NEXT: mo:
    // CHECK-NEXT: b: 42
    // CHECK-NEXT: c: {{.*}}.MOHaver
    for c in mirror.children {
        print("\(c.label!): \(c.value)")
    }
    // CHECK-NEXT: destroyed MO
}
// CHECK-NEXT: done with nongeneric
print("done with nongeneric")

