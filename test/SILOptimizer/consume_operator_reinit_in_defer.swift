// RUN: %target-swift-frontend -emit-sil -verify %s

func consume<T>(_: consuming T) {}

func testSingleBlock<T>(x: inout T, y: T) {
    defer { x = y }
    consume(consume x)
}

func cond() -> Bool { fatalError() }

// TODO: should be accepted
func testAlwaysReinitAfterConditional<T>(x: inout T, y: T) { // not-really expected-error{{used after consume}}
    defer {
        if cond() { }
        x = y // not-really expected-note{{}}
    }
    consume(consume x) // not-really expected-note{{}}
}

// TODO: should be accepted
func testAlwaysReinitBeforeConditional<T>(x: inout T, y: T) { // not-really expected-error{{used after consume}}
    defer {
        x = y // not-really expected-note{{}}
        if cond() { }
    }
    consume(consume x) // not-really expected-note{{}}
}

// TODO: should be accepted
func testAlwaysReinitInBothBranchesOfConditional<T>(x: inout T, y: T) { // not-really expected-error{{used after consume}}
    defer {
        if cond() {
            x = y // not-really expected-note{{}}
        } else {
            x = y
        }
    }
    consume(consume x) // not-really expected-note{{}}
}

// TODO: should raise an error about inout not being reinitialized on all paths
func testSometimesReinitInConditional<T>(x: inout T, y: T) { // not-really expected-error{{used after consume}}
    defer {
        if cond() {
            x = y // not-really expected-note{{}}
        } else {
            // ex/pected-note {{not initialized on this path}}
        }
    }
    consume(consume x) // not-really expected-note{{}}
}
