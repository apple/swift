// RUN: %target-typecheck-verify-swift

@_moveOnly
enum Foo {
    deinit {} // expected-error {{deinitializers are not yet supported on noncopyable enums}}
}

@_moveOnly
enum Foo2 {
}

@_moveOnly
struct Bar {
    var x: Int
}

@_moveOnly
enum Foo3 {
    case foo(Int)
    case bar(String)
    case bas
}

func test_switch(x: consuming Foo3) {
    switch x { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{12-12=consume }}
    default:
        break
    }

    switch consume x {
    default:
        break
    }

    switch (x) { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{13-13=consume }}
    default:
        break
    }

    switch (consume x) {
    default:
        break
    }

    let _: () -> () = {
        switch x { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{16-16=consume }}
        default:
            break
        }
    }

    let _: () -> () = {
        switch consume x {
        default:
            break
        }
    }

    let _: () -> () = {
        switch (x) { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{17-17=consume }}
        default:
            break
        }
    }

    let _: () -> () = {
        switch (consume x) {
        default:
            break
        }
    }
}

func test_if_case(x: consuming Foo3) {
    if case .bar(let y) = x { _ = y } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{27-27=consume }}

    guard case .bar(let y) = x else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{30-30=consume }}
    _ = y

    if case .bar(let z) = consume x { _ = z }

    guard case .bar(let z) = consume x else { return }
    _ = z

    if case .bar(let a) = (x) { _ = a } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{28-28=consume }}

    guard case .bar(let a) = (x) else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{31-31=consume }}
    _ = a

    if case .bar(let b) = (consume x) { _ = b }

    guard case .bar(let b) = (consume x) else { return }
    _ = b

    let _: () -> () = {
        if case .bar(let y) = x { _ = y } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{31-31=consume }}
    }

    let _: () -> () = {
        guard case .bar(let y) = x else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{34-34=consume }}
        _ = y
    }

    let _: () -> () = {
        if case .bar(let z) = consume x { _ = z }
    }

    let _: () -> () = {
        guard case .bar(let z) = consume x else { return }
        _ = z
    }

    let _: () -> () = {
        if case .bar(let a) = (x) { _ = a } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{32-32=consume }}
    }

    let _: () -> () = {
        guard case .bar(let a) = (x) else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{35-35=consume }}
        _ = a
    }

    let _: () -> () = {
        if case .bar(let b) = (consume x) { _ = b }
    }

    let _: () -> () = {
        guard case .bar(let b) = (consume x) else { return }
        _ = b
    }
}

func test_switch_b(x: __owned Foo3) {
    switch x { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{12-12=consume }}
    default:
        break
    }

    switch consume x {
    default:
        break
    }

    switch (x) { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{13-13=consume }}
    default:
        break
    }

    switch (consume x) {
    default:
        break
    }

    let _: () -> () = {
        switch x { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{16-16=consume }}
        default:
            break
        }
    }

    let _: () -> () = {
        switch consume x {
        default:
            break
        }
    }

    let _: () -> () = {
        switch (x) { // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{17-17=consume }}
        default:
            break
        }
    }

    let _: () -> () = {
        switch (consume x) {
        default:
            break
        }
    }
}

func test_if_case_b(x: __owned Foo3) {
    if case .bar(let y) = x { _ = y } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{27-27=consume }}

    guard case .bar(let y) = x else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{30-30=consume }}
    _ = y

    if case .bar(let z) = consume x { _ = z }

    guard case .bar(let z) = consume x else { return }
    _ = z

    if case .bar(let a) = (x) { _ = a } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{28-28=consume }}

    guard case .bar(let a) = (x) else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{31-31=consume }}
    _ = a

    if case .bar(let b) = (consume x) { _ = b }

    guard case .bar(let b) = (consume x) else { return }
    _ = b

    let _: () -> () = {
        if case .bar(let y) = x { _ = y } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{31-31=consume }}
    }

    let _: () -> () = {
        guard case .bar(let y) = x else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{34-34=consume }}
        _ = y
    }

    let _: () -> () = {
        if case .bar(let z) = consume x { _ = z }
    }

    let _: () -> () = {
        guard case .bar(let z) = consume x else { return }
        _ = z
    }

    let _: () -> () = {
        if case .bar(let a) = (x) { _ = a } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{32-32=consume }}
    }

    let _: () -> () = {
        guard case .bar(let a) = (x) else { return } // expected-warning{{noncopyable binding being pattern-matched must have the 'consume' operator applied}} {{35-35=consume }}
        _ = a
    }

    let _: () -> () = {
        if case .bar(let b) = (consume x) { _ = b }
    }

    let _: () -> () = {
        guard case .bar(let b) = (consume x) else { return }
        _ = b
    }
}
