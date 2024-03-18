// RUN: %target-typecheck-verify-swift -swift-version 4

// https://github.com/apple/swift/issues/44270
// Dollar was accidentally allowed as an identifier in Swift 3.
// SE-0144: Reject this behavior in the future.

func dollarVar() {
  var $ : Int = 42 // expected-error {{'$' is not an identifier; use backticks to escape it}} {{7-8=`$`}}
  $ += 1 // expected-error {{'$' is not an identifier; use backticks to escape it}} {{3-4=`$`}}
  print($) // expected-error {{'$' is not an identifier; use backticks to escape it}} {{9-10=`$`}}
}
func dollarLet() {
  let $ = 42 // expected-error {{'$' is not an identifier; use backticks to escape it}} {{7-8=`$`}}
  print($) // expected-error {{'$' is not an identifier; use backticks to escape it}} {{9-10=`$`}}
}
func dollarClass() {
  class $ {} // expected-error {{'$' is not an identifier; use backticks to escape it}} {{9-10=`$`}}
}
func dollarEnum() {
  enum $ {} // expected-error {{'$' is not an identifier; use backticks to escape it}} {{8-9=`$`}}
}
func dollarStruct() {
  struct $ {} // expected-error {{'$' is not an identifier; use backticks to escape it}} {{10-11=`$`}}
}

func dollarFunc() {
  func $($ dollarParam: Int) {}
  // expected-error@-1 {{'$' is not an identifier; use backticks to escape it}} {{8-9=`$`}}
  // expected-error@-2 {{'$' is not an identifier; use backticks to escape it}} {{10-11=`$`}}
  $($: 24)
  // expected-error@-1 {{'$' is not an identifier; use backticks to escape it}} {{3-4=`$`}}
  // expected-error@-2 {{'$' is not an identifier; use backticks to escape it}} {{5-6=`$`}}
}

func escapedDollarVar() {
  var `$` : Int = 42 // no error
  `$` += 1
  print(`$`)
}
func escapedDollarLet() {
  let `$` = 42 // no error
  print(`$`)
}
func escapedDollarClass() {
  class `$` {} // no error
}
func escapedDollarEnum() {
  enum `$` {} // no error
}
func escapedDollarStruct() {
  struct `$` {} // no error
}

func escapedDollarFunc() {
  func `$`(`$`: Int) {} // no error
  `$`(`$`: 25) // no error
}

func escapedDollarAnd() {
  // FIXME: Bad diagnostics.
  `$0` = 1 // expected-error {{expected expression}}
  `$$` = 2
  `$abc` = 3
}

// Test that we disallow user-defined $-prefixed identifiers. However, the error
// should not be emitted on $-prefixed identifiers that are not considered
// declarations.

func $declareWithDollar() { // expected-error{{cannot declare entity named '$declareWithDollar'}}
  var $foo: Int { // expected-error{{cannot declare entity named '$foo'}}
    get { 0 }
    set($value) {} // expected-error{{cannot declare entity named '$value'}}
  }
  func $bar() { } // expected-error{{cannot declare entity named '$bar'}}
  func wibble(
    $a: Int, // expected-error{{cannot declare entity named '$a'}}
    $b c: Int) { } // expected-error{{cannot declare entity named '$b'}}
  let _: (Int) -> Int = {
    [$capture = 0] // expected-error{{cannot declare entity named '$capture'}}
    $a in // expected-error{{inferred projection type 'Int' is not a property wrapper}}
    $capture
  }
  let ($a: _, _) = (0, 0) // expected-error{{cannot declare entity named '$a'}}
  $label: if true { // expected-error{{cannot declare entity named '$label'}}
    break $label
  }
  switch 0 {
  @$dollar case _: // expected-error {{unknown attribute '$dollar'}}
    break
  }
  if #available($Dummy 9999, *) {} // expected-warning {{unrecognized platform name '$Dummy'}}
  @_swift_native_objc_runtime_base($Dollar)
  class $Class {} // expected-error{{cannot declare entity named '$Class'; the '$' prefix is reserved}}
  enum $Enum {} // expected-error{{cannot declare entity named '$Enum'; the '$' prefix is reserved}}
  struct $Struct { // expected-error{{cannot declare entity named '$Struct'; the '$' prefix is reserved}}
    @_projectedValueProperty($dummy)
    let property: Never
  }
}
protocol $Protocol {} // expected-error {{cannot declare entity named '$Protocol'; the '$' prefix is reserved}}
precedencegroup $Precedence { // expected-error {{cannot declare entity named '$Precedence'; the '$' prefix is reserved}}
  higherThan: $Precedence // expected-error {{cycle in 'higherThan' relation}}
}
infix operator **: $Precedence
#$UnknownDirective() // expected-error {{no macro named '$UnknownDirective'}}


// https://github.com/apple/swift/issues/55672

@propertyWrapper
struct Wrapper {
  var wrappedValue: Int
  var projectedValue: String { String(wrappedValue) }
}

struct S {
  @Wrapper var café = 42
}

let _ = S().$café // Okay

protocol P {
  var foo: Int { get set }
  var $foo: String { get }
}

struct S2: P {
  @Wrapper var foo = 1337
}

// https://github.com/apple/swift/issues/55538
infix operator $ // expected-error{{'$' is considered an identifier and must not appear within an operator name}}
infix operator `$` // expected-error{{'$' is considered an identifier and must not appear within an operator name}}
