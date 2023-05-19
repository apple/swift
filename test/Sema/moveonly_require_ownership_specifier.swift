// RUN: %target-typecheck-verify-swift

// Coverage for check that requires some ownership specifier to be written
// when a move-only / noncopyable type appears as a parameter of a function.

@_moveOnly
struct MO {
    var x = 0
}

struct Box<T> { var val: T }

@_moveOnly
public struct NoncopyableWrapper<T> {
  var x: T
}

class Inspector {
  func inspect(_ hasIt: inout MO, _ mo: MO, _ hasItAgain: __owned MO) {}
  // expected-error@-1 {{noncopyable parameter must specify its ownership}}
  // expected-note@-2 {{add 'borrowing' for an immutable reference}}{{41-41=borrowing }}
  // expected-note@-3 {{add 'inout' for a mutable reference}}{{41-41=inout }}
  // expected-note@-4 {{add 'consuming' to take the value from the caller}}{{41-41=consuming }}
}

// expected-error@+4 {{noncopyable parameter must specify its ownership}}
// expected-note@+3 {{add 'borrowing' for an immutable reference}}{{20-20=borrowing }}
// expected-note@+2 {{add 'inout' for a mutable reference}}{{20-20=inout }}
// expected-note@+1 {{add 'consuming' to take the value from the caller}}{{20-20=consuming }}
func applier(_ f: (MO) -> (),
             _ v: MO) {}
// expected-error@-1 {{noncopyable parameter must specify its ownership}}
// expected-note@-2 {{add 'borrowing' for an immutable reference}}{{19-19=borrowing }}
// expected-note@-3 {{add 'inout' for a mutable reference}}{{19-19=inout }}
// expected-note@-4 {{add 'consuming' to take the value from the caller}}{{19-19=consuming }}

func caller() {
  let f = { (_ mo1: MO, _ mo2: MO) in () }
  // expected-error@-1 2{{noncopyable parameter must specify its ownership}}
  // expected-note@-2 {{add 'borrowing' for an immutable reference}}{{21-21=borrowing }}
  // expected-note@-3 {{add 'borrowing' for an immutable reference}}{{32-32=borrowing }}
  // expected-note@-4 {{add 'inout' for a mutable reference}}{{21-21=inout }}
  // expected-note@-5 {{add 'inout' for a mutable reference}}{{32-32=inout }}
  // expected-note@-6 {{add 'consuming' to take the value from the caller}}{{21-21=consuming }}
  // expected-note@-7 {{add 'consuming' to take the value from the caller}}{{32-32=consuming }}

  let g: (MO, MO) -> () = f
  // expected-error@-1 2{{noncopyable parameter must specify its ownership}}
  // expected-note@-2 {{add 'borrowing' for an immutable reference}}{{11-11=borrowing }}
  // expected-note@-3 {{add 'borrowing' for an immutable reference}}{{15-15=borrowing }}
  // expected-note@-4 {{add 'inout' for a mutable reference}}{{11-11=inout }}
  // expected-note@-5 {{add 'inout' for a mutable reference}}{{15-15=inout }}
  // expected-note@-6 {{add 'consuming' to take the value from the caller}}{{11-11=consuming }}
  // expected-note@-7 {{add 'consuming' to take the value from the caller}}{{15-15=consuming }}

  let partialG = { g($0, MO()) }

  let _: Box<(MO) -> ()> = Box(val: partialG)
  // expected-error@-1 {{noncopyable parameter must specify its ownership}}
  // expected-note@-2 {{add 'borrowing' for an immutable reference}}{{15-15=borrowing }}
  // expected-note@-3 {{add 'inout' for a mutable reference}}{{15-15=inout }}
  // expected-note@-4 {{add 'consuming' to take the value from the caller}}{{15-15=consuming }}

  let _: Box<(inout MO) -> ()>? = nil
  let _: Box<(borrowing MO) -> ()>? = nil

  let _: Box<(MO) -> ()>? = nil
  // expected-error@-1 {{noncopyable parameter must specify its ownership}}
  // expected-note@-2 {{add 'borrowing' for an immutable reference}}{{15-15=borrowing }}
  // expected-note@-3 {{add 'inout' for a mutable reference}}{{15-15=inout }}
  // expected-note@-4 {{add 'consuming' to take the value from the caller}}{{15-15=consuming }}

  applier(partialG, MO())
}

func takeGeneric<T>(_ x: NoncopyableWrapper<T>) {}
// expected-error@-1 {{noncopyable parameter must specify its ownership}}
// expected-note@-2 {{add 'borrowing' for an immutable reference}}{{26-26=borrowing }}
// expected-note@-3 {{add 'inout' for a mutable reference}}{{26-26=inout }}
// expected-note@-4 {{add 'consuming' to take the value from the caller}}{{26-26=consuming }}

func takeInstantiated(_ x: NoncopyableWrapper<Int>) {}
// expected-error@-1 {{noncopyable parameter must specify its ownership}}
// expected-note@-2 {{add 'borrowing' for an immutable reference}}{{28-28=borrowing }}
// expected-note@-3 {{add 'inout' for a mutable reference}}{{28-28=inout }}
// expected-note@-4 {{add 'consuming' to take the value from the caller}}{{28-28=consuming }}

struct O: ~Copyable {}

public struct M: ~Copyable {
  subscript(_ i: O) -> Int { // expected-error {{subscripts cannot have noncopyable parameters}}
    get { fatalError() }
    set { }
  }
}
