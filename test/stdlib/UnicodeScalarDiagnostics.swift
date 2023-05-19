// RUN: %target-typecheck-verify-swift

// FIXME: The clarity of these diagnostics could be improved.
// <rdar://problem/29912193>

func isString(_ s: inout String) {}

func test_UnicodeScalarDoesNotImplementArithmetic(_ us: UnicodeScalar, i: Int) {
  var a1 = "a" + "b" // OK
  isString(&a1)
  // We don't check for the overload choices list on the overload note match because they may change on different platforms. 
  let a2 = "a" - "b" // expected-error {{binary operator '-' cannot be applied to two 'String' operands}}
  let a3 = "a" * "b" // expected-error {{binary operator '*' cannot be applied to two 'String' operands}}
  let a4 = "a" / "b" // expected-error {{binary operator '/' cannot be applied to two 'String' operands}}

  let b1 = us + us // expected-error {{binary operator '+' cannot be applied to two 'UnicodeScalar' (aka 'Unicode.Scalar') operands}}
  let b2 = us - us // expected-error {{binary operator '-' cannot be applied to two 'UnicodeScalar' (aka 'Unicode.Scalar') operands}}
  let b3 = us * us // expected-error {{binary operator '*' cannot be applied to two 'UnicodeScalar' (aka 'Unicode.Scalar') operands}}
  // DurationProtocol is a near miss here
  let b4 = us / us // expected-error {{referencing operator function '/' on 'DurationProtocol' requires that 'UnicodeScalar' (aka 'Unicode.Scalar') conform to 'DurationProtocol'}}

  let c1 = us + i // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
  let c2 = us - i // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
  let c3 = us * i // expected-note {{overloads for '*' exist with these partially matching parameter lists: (Int, Int)}}
  // expected-error@-1 {{binary operator '*' cannot be applied to operands of type 'UnicodeScalar' (aka 'Unicode.Scalar') and 'Int'}}
  let c4 = us / i // expected-note {{overloads for '/' exist with these partially matching parameter lists: (Int, Int)}}
  // expected-error@-1 {{binary operator '/' cannot be applied to operands of type 'UnicodeScalar' (aka 'Unicode.Scalar') and 'Int'}}

  let d1 = i + us // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
  let d2 = i - us // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
  let d3 = i * us // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
  let d4 = i / us // expected-error {{cannot convert value of type 'UnicodeScalar' (aka 'Unicode.Scalar') to expected argument type 'Int'}}
}

