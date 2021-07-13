// RUN: %target-typecheck-verify-swift

infix operator **

func **(a: Double, b: Double) -> Double { return 0 }

func testDeclaredPowerOperator() { 
  let x: Double = 3.0
  let y: Double = 3.0
  _ = 2.0**2.0 // no error
  _ = x**y // no error
  _ = x^y // no error
}

func testDeclaredPowerOperatorWithIncompatibleType() { 
  let x: Int8 = 3
  let y: Int8 = 3
  _ = x**y // expected-error{{cannot convert value of type 'Int8' to expected argument type 'Double'}} expected-error{{cannot convert value of type 'Int8' to expected argument type 'Double'}}
}