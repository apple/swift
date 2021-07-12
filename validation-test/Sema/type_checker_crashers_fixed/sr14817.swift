// RUN: %target-typecheck-verify-swift

@resultBuilder
struct MyResultBuilder {
  static func buildBlock(_ elements: Int...) -> Int { fatalError() }
}

struct VariableDecl2 {
  init( // expected-note {{found this candidate}}
    paramClosure: () -> Int? = { nil },
    paramInt: Int,
    @MyResultBuilder paramResultBuilder: () -> Int? = { nil }
  ) { fatalError() }

  init( // expected-note {{found this candidate}}
    paramInt: Int,
    paramClosure: () -> Int? = { nil },
    @MyResultBuilder paramResultBuilder: () -> Int? = { nil }
  ) {
    fatalError()
  }
}

let buildable = VariableDecl2(paramInt: 1) { // expected-error {{ambiguous use of 'init'}}
}
