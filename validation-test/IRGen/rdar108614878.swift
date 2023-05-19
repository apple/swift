// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

final class C {}

struct S {
    @inline(never)
    func g1<T>(_ componentType: T.Type) -> T? {
        return nil
    }
    @inline(never)
    func g2<T>(_ type: T.Type) -> T? {
        g1(T.self) as? T
    }
}

func run() -> C? {
    var e = S()
    let r = e.g2(C.self)
    return r
}

// CHECK: nil
print(run())
