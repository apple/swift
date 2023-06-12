// RUN: %target-typecheck-verify-swift

// This test validates that move only enums cannot be marked indirect or have
// indirect cases.

@_moveOnly
struct S {
    var i = 5
}

@_moveOnly enum E { }

@_moveOnly
enum E1 {
    case first
    case second(S)
}

@_moveOnly
indirect enum E2 { // expected-error {{noncopyable enum 'E2' cannot be marked indirect or have indirect cases yet}}
    case first
    case second(S)
}

@_moveOnly
enum E3 {
    case first
    indirect case second(S) // expected-error {{noncopyable enum 'E3' cannot be marked indirect or have indirect cases yet}}
}
