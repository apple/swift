// RUN: %target-swift-frontend -emit-sil -verify %s

// rdar://111060678

protocol P {
    func doStuff() throws
}

struct A: ~Copyable {
    let b = B()

    consuming func f(_ p: some P) throws -> B {
        // Passing the closure here undergoes a SIL-level function conversion
        // from the concrete type `() -> Void` to `<T> () throws -> T`.
        try b.takeClosure {
            try b.takeP(p)
        }
        return B()
    }
}

struct B {
    func takeClosure<T>(_ f: () throws -> T) throws -> T { try f() }
    func takeP(_: some P) throws {}
}
