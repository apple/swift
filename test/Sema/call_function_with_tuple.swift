// RUN: %target-typecheck-verify-swift

func twoArgs(_ a: Int, _ b: Int) -> Void { //expected-note{{'twoArgs' declared here}}
}

func call() {
  // Call a function that takes two unnamed arguments with a tuple that has two
  // named arguments
  let namedTuple: (x: Int, y: Int) = (1, 1)
  twoArgs(namedTuple) // expected-error{{global function 'twoArgs' expects 2 separate arguments}} expected-error{{missing argument label ':' in call}}
}

func foo(_: (Int, Int)) -> String {}

func test() -> String {
  return foo((0, true))
  // expected-error@-1 {{cannot convert value of type '(Int, Bool)' to expected argument type '(Int, Int)'}}
}

func bar(_: (Int,Int), _: (Int,Bool)) {} // expected-note {{'bar' declared here}}
bar((0,false)) // expected-error {{missing argument for parameter #1 in call}}
