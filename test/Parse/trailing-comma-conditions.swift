// RUN: %target-typecheck-verify-swift -disable-experimental-parser-round-trip -enable-experimental-feature TrailingComma

func f(_ block: (Bool) -> Bool) -> Bool { block(true) }

func testConditionListTrailingComma() {
    if true, { }

    if true, { }; { }()

    if true, { print("if-body") } else { print("else-body") }

    if true, { print("if-body") } else if true, { print("else-if-body") } else { print("else-body") }

    if true, { if true { { } } } // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}}

    { if true, { print(0) } } // expected-error {{closure expression is unused}} expected-note {{did you mean to use a 'do' statement?}}

    ( if true, { print(0) } ) // expected-error {{'if' may only be used as expression in return, throw, or as the source of an assignment}} expected-error {{'if' must have an unconditional 'else' to be used as expression}}

    if true, {true}(), { }

    if true, { true }, { print(0) } // expected-error {{function produces expected type 'Bool'; did you mean to call it with '()'?}}

    if true, { print(0) }
    { }()

    if true, { true } // expected-error {{function produces expected type 'Bool'; did you mean to call it with '()'?}}
    ,{ print(0) }

    if true, { (x: () -> Void) in true } != nil { print(0) } // expected-warning {{comparing non-optional value of type '(() -> Void) -> Bool' to 'nil' always returns true}}

    if true, { (x: () -> Void) in true }
    != nil // expected-warning {{comparing non-optional value of type '(() -> Void) -> Bool' to 'nil' always returns true}}
    {
    print(0)
    }

    if { } // expected-error {{missing condition in 'if' statement}} 

    if , { } // expected-error {{expected expression, var, or let in 'if' condition}}

    guard else { return } // expected-error {{missing condition in 'guard' statement}} 

    guard , else { return } // expected-error {{expected expression, var, let or case in 'guard' condition}} 

    guard true, else { return }

    guard true, , else { return } // expected-error {{expected expression in conditional}}

    guard true, { return } // expected-error {{expected 'else' after 'guard' condition}}
}