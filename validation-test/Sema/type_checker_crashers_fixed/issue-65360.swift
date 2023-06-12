// RUN: %target-typecheck-verify-swift

var a: Any?

let _: () -> Void = {
  for case (is Int)? in [a] {}
  if case (is Int, is Int) = a {} // expected-error {{cannot convert value of type 'Any?' to specified type '(_, _)'}}
}

let _: () -> Void = {
  for case (0)? in [a] {}
  // expected-error@-1 {{pattern cannot match values of type 'Any?'}}
  if case (0, 0) = a {}
}

let _: () -> Void = {
  for case (0)? in [a] {}
  // expected-error@-1 {{pattern cannot match values of type 'Any?'}}
  for case (0, 0) in [a] {}
}

let _: () -> Void = {
  if case (0, 0) = a {}
  // expected-error@-1 {{cannot convert value of type 'Any?' to specified type '(Int, Int)'}}
  for case (0)? in [a] {}
}

let _: () -> Void = {
  for case (0, 0) in [a] {}
  // expected-error@-1 {{cannot convert value of type 'Any?' to expected element type '(Int, Int)'}}
  for case (0)? in [a] {}
}
