// RUN: %target-typecheck-verify-swift

enum E: Error { case e }

// rdar://106598067 – Make sure we don't crash.
// FIXME: We ought to have a tailored diagnostic to change to 'as' instead of 'as?'
let fn = {
  do {} catch let x as? E {}
  // expected-error@-1 {{pattern variable binding cannot appear in an expression}}
  // expected-error@-2 {{expression pattern of type 'E?' cannot match values of type 'any Error'}}
  // expected-warning@-3 {{'catch' block is unreachable because no errors are thrown in 'do' block}}
}
