// RUN: %target-typecheck-verify-swift

enum E: Error { case e }

// rdar://106598067 – Make sure we don't crash.
// FIXME: Bad diagnostic (the issue is that it should be written 'as', not 'as?')
let fn = {
  // expected-error@-1 {{unable to infer closure type without a type annotation}}
  do {} catch let x as? E {}
  // expected-warning@-1 {{'catch' block is unreachable because no errors are thrown in 'do' block}}
}
