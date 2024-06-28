// RUN: %target-typecheck-verify-swift

// TODO: Implement tuple equality in the library.
// BLOCKED: <rdar://problem/13822406>
func ~= (x: (Int,Int), y: (Int,Int)) -> Bool {
  return true
}

func parseError1(x: Int) {
  switch func {} // expected-error {{expected expression in 'switch' statement}} expected-error {{expected identifier in function declaration}}
}

func parseError2(x: Int) {
  switch x // expected-error {{expected '{' after 'switch' subject expression}}
}

func parseError3(x: Int) {
  switch x {
    case // expected-error {{expected pattern}} expected-error {{expected ':' after 'case'}}
  }
}

func parseError4(x: Int) {
  switch x {
  case var z where // expected-error {{expected expression for 'where' guard of 'case'}} expected-error {{expected ':' after 'case'}}
  }
}

func parseError5(x: Int) {
  switch x {
  case let z // expected-error {{expected ':' after 'case'}} expected-warning {{immutable value 'z' was never used}} {{8-13=_}}
  }
}

func parseError6(x: Int) {
  switch x {
  default // expected-error {{expected ':' after 'default'}}
  }
}

var x: Int

switch x {} // expected-error {{'switch' statement body must have at least one 'case' or 'default' block}}

switch x {
case 0:
  x = 0
// Multiple patterns per case
case 1, 2, 3:
  x = 0
// 'where' guard
case _ where x % 2 == 0:
  x = 1
  x = 2
  x = 3
case _ where x % 2 == 0,
     _ where x % 3 == 0:
  x = 1
case 10,
     _ where x % 3 == 0:
  x = 1
case _ where x % 2 == 0,
     20:
  x = 1
case var y where y % 2 == 0:
  x = y + 1
case _ where 0: // expected-error {{integer literal value '0' cannot be used as a boolean; did you mean 'false'?}}
  x = 0
default:
  x = 1
}

// Multiple cases per case block
switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0: // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
case 1, case 2: // expected-error {{extraneous 'case' keyword in pattern}} {{9-14=}}
  x = 0
}

switch x {
case 0: // expected-error{{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
default:
  x = 0
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0:
  x = 0
case 1: // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
}

switch x {
case 0:
  x = 0
default: // expected-error {{'default' label in a 'switch' must have at least one executable statement}} {{9-9= break}}
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0:
  ; // expected-error {{';' statements are not allowed}} {{3-5=}}
case 1:
  x = 0
}



switch x {
  x = 1 // expected-error{{all statements inside a switch must be covered by a 'case' or 'default'}}
default:
  x = 0
case 0: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
case 1:
  x = 0
}

switch x {
default:
  x = 0
default: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
}

switch x { // expected-error{{'switch' statement body must have at least one 'case' or 'default' block}}
  x = 1 // expected-error{{all statements inside a switch must be covered by a 'case' or 'default'}}
}

switch x { // expected-error{{'switch' statement body must have at least one 'case' or 'default' block}}
  x = 1 // expected-error{{all statements inside a switch must be covered by a 'case' or 'default'}}
  x = 2
}

switch x {
default: // expected-error{{'default' label in a 'switch' must have at least one executable statement}} {{9-9= break}}
case 0: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
}

switch x {
default: // expected-error{{'default' label in a 'switch' must have at least one executable statement}} {{9-9= break}}
default: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
default where x == 0: // expected-error{{'default' cannot be used with a 'where' guard expression}}
  x = 0
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0: // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0: // expected-error{{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
case 1:
  x = 0
}

switch x { // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
case 0:
  x = 0
case 1: // expected-error{{'case' label in a 'switch' must have at least one executable statement}} {{8-8= break}}
}


case 0: // expected-error{{'case' label can only appear inside a 'switch' statement}}
var y = 0
default: // expected-error{{'default' label can only appear inside a 'switch' statement}}
var z = 1

fallthrough // expected-error{{'fallthrough' is only allowed inside a switch}}

switch x {
case 0:
  fallthrough
case 1:
  fallthrough
default:
  fallthrough // expected-error{{'fallthrough' without a following 'case' or 'default' block}}
}

// Fallthrough can transfer control anywhere within a case and can appear
// multiple times in the same case.
switch x {
case 0:
  if true { fallthrough }
  if false { fallthrough }
  x += 1
default:
  x += 1
}

// Cases cannot contain 'var' bindings if there are multiple matching patterns
// attached to a block. They may however contain other non-binding patterns.

var t = (1, 2)

switch t {
case (var a, 2), (1, _): // expected-error {{'a' must be bound in every pattern}} expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  ()

case (_, 2), (var a, _): // expected-error {{'a' must be bound in every pattern}}
  ()

case (var a, 2), (1, var b): // expected-error {{'a' must be bound in every pattern}} expected-error {{'b' must be bound in every pattern}} expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  ()

case (var a, 2): // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{17-17= break}} expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
case (1, _):
  ()

case (_, 2): // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{13-13= break}}
case (1, var a): // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  ()

case (var a, 2): // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{17-17= break}} expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
case (1, var b): // expected-warning {{variable 'b' was never used; consider replacing with '_' or removing it}}
  ()

case (1, let b): // let bindings expected-warning {{immutable value 'b' was never used; consider replacing with '_' or removing it}}
  ()

case (_, 2), (let a, _): // expected-error {{'a' must be bound in every pattern}} expected-warning {{case is already handled by previous patterns; consider removing it}}
  ()

// OK
case (_, 2), (1, _):
  ()
  
case (_, var a), (_, var a): // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  // expected-warning@-1 {{case is already handled by previous patterns; consider removing it}}
  // expected-warning@-2 {{case is already handled by previous patterns; consider removing it}}
  ()
  
case (var a, var b), (var b, var a): // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}} expected-warning {{variable 'b' was never used; consider replacing with '_' or removing it}}
  // expected-warning@-1 {{case is already handled by previous patterns; consider removing it}}
  // expected-warning@-2 {{case is already handled by previous patterns; consider removing it}}
  ()

case (_, 2): // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{13-13= break}}
case (1, _):
  ()
}

func patternVarUsedInAnotherPattern(x: Int) {
  switch x {
  case let a, // expected-error {{'a' must be bound in every pattern}}
       value: // expected-error {{cannot find 'value' in scope}}
    break
  }
}

// Fallthroughs can only transfer control into a case label with bindings if the previous case binds a superset of those vars.
switch t {
case (1, 2):
  fallthrough // expected-error {{'fallthrough' from a case which doesn't bind variable 'a'}} expected-error {{'fallthrough' from a case which doesn't bind variable 'b'}}
case (var a, var b): // expected-warning {{variable 'a' was never mutated; consider changing to 'let' constant}} expected-warning {{variable 'b' was never mutated; consider changing to 'let' constant}}
  t = (b, a)
}

switch t { // specifically notice on next line that we shouldn't complain that a is unused - just never mutated
case (var a, let b): // expected-warning {{variable 'a' was never mutated; consider changing to 'let' constant}}
  t = (b, b)
  fallthrough // ok - notice that subset of bound variables falling through is fine
case (2, let a):
  t = (a, a)
}

func patternVarDiffType(x: Int, y: Double) {
  switch (x, y) {
  case (1, let a): // expected-error {{pattern variable bound to type 'Double', fallthrough case bound to type 'Int'}}
    fallthrough
  case (let a, _):
    break
  }
}

func patternVarDiffMutability(x: Int, y: Double) {
  switch x {
  case let a where a < 5, var a where a > 10: // expected-error {{'var' pattern binding must match previous 'let' pattern binding}}{{27-30=let}}
    break
  default:
    break
  }
  switch (x, y) {
  // Would be nice to have a fixit in the following line if we detect that all bindings in the same pattern have the same problem.
  case let (a, b) where a < 5, var (a, b) where a > 10: // expected-error 2{{'var' pattern binding must match previous 'let' pattern binding}}{{none}}
    break
  case (let a, var b) where a < 5, (let a, let b) where a > 10: // expected-error {{'let' pattern binding must match previous 'var' pattern binding}}{{44-47=var}}
    break
  case (let a, let b) where a < 5, (var a, let b) where a > 10, (let a, var b) where a == 8:
    // expected-error@-1 {{'var' pattern binding must match previous 'let' pattern binding}}{{37-40=let}}
    // expected-error@-2 {{'var' pattern binding must match previous 'let' pattern binding}}{{73-76=let}}
    break
  default:
    break
  }
}

func test_label(x : Int) {
Gronk: // expected-error {{switch must be exhaustive}} expected-note{{add a default clause}}
  switch x {
  case 42: return
  }
}

func enumElementSyntaxOnTuple() {
  switch (1, 1) {
  case .Bar: // expected-error {{value of tuple type '(Int, Int)' has no member 'Bar'}}
    break
  default:
    break
  }
}

// https://github.com/apple/swift/issues/42798
enum Whatever { case Thing }
func f0(values: [Whatever]) { // expected-note {{'values' declared here}}
    switch value { // expected-error {{cannot find 'value' in scope; did you mean 'values'?}}
    case .Thing: // Ok. Don't emit diagnostics about enum case not found in type <<error type>>.
        break
    }
}

// https://github.com/apple/swift/issues/43334
// https://github.com/apple/swift/issues/43335
enum Whichever {
  case Thing
  static let title = "title"
  static let alias: Whichever = .Thing
}
func f1(x: String, y: Whichever) {
  switch x {
    case Whichever.title: // Ok. Don't emit diagnostics for static member of enum.
        break
    case Whichever.buzz: // expected-error {{type 'Whichever' has no member 'buzz'}}
        break
    // expected-note @+1 {{overloads for '~=' exist with these partially matching parameter lists: (Substring, String)}}
    case Whichever.alias: // expected-error {{expression pattern of type 'Whichever' cannot match values of type 'String'}}
    // expected-error@-1 {{'case' label in a 'switch' must have at least one executable statement}}
    default:
      break
  }
  switch y {
    case Whichever.Thing: // Ok.
        break
    case Whichever.alias: // Ok. Don't emit diagnostics for static member of enum.
        break
    case Whichever.title: // expected-error {{expression pattern of type 'String' cannot match values of type 'Whichever'}}
        break
  }
  switch y {
    case .alias:
      break
    default:
      break
  }
}


switch Whatever.Thing {
case .Thing: // expected-error{{'case' label in a 'switch' must have at least one executable statement}} {{13-13= break}}
@unknown case _:
  x = 0
}

switch Whatever.Thing {
case .Thing: // expected-error{{'case' label in a 'switch' must have at least one executable statement}} {{13-13= break}}
@unknown default:
  x = 0
}

switch Whatever.Thing {
case .Thing:
  x = 0
@unknown case _: // expected-error {{'case' label in a 'switch' must have at least one executable statement}} {{17-17= break}}
}

switch Whatever.Thing {
case .Thing:
  x = 0
@unknown default: // expected-error {{'default' label in a 'switch' must have at least one executable statement}} {{18-18= break}}
}


switch Whatever.Thing {
@unknown default:
  x = 0
default: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
case .Thing:
  x = 0
}

switch Whatever.Thing {
default:
  x = 0
@unknown case _: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}} expected-error {{'@unknown' can only be applied to the last case in a switch}}
  x = 0
case .Thing:
  x = 0
}

switch Whatever.Thing {
default:
  x = 0
@unknown default: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  x = 0
case .Thing:
  x = 0
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown default where x == 0: // expected-error{{'default' cannot be used with a 'where' guard expression}}
  x = 0
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case _:
  fallthrough // expected-error{{'fallthrough' without a following 'case' or 'default' block}}
}

switch Whatever.Thing {
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  fallthrough
case .Thing:
  break
}

switch Whatever.Thing {
@unknown default:
  fallthrough
case .Thing: // expected-error{{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  break
}

switch Whatever.Thing {
@unknown case _, _: // expected-error {{'@unknown' cannot be applied to multiple patterns}}
  break
}

switch Whatever.Thing {
@unknown case _, _, _: // expected-error {{'@unknown' cannot be applied to multiple patterns}}
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case let value: // expected-error {{'@unknown' is only supported for catch-all cases ("case _")}}
  _ = value
}

switch (Whatever.Thing, Whatever.Thing) { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '(_, _)'}}
@unknown case (_, _): // expected-error {{'@unknown' is only supported for catch-all cases ("case _")}}
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case is Whatever: // expected-error {{'@unknown' is only supported for catch-all cases ("case _")}}
  // expected-warning@-1 {{'is' test is always true}}
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case .Thing: // expected-error {{'@unknown' is only supported for catch-all cases ("case _")}}
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case (_): // okay
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown case _ where x == 0: // expected-error {{'where' cannot be used with '@unknown'}}
  break
}

switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note{{add missing case: '.Thing'}}
@unknown default where x == 0: // expected-error {{'default' cannot be used with a 'where' guard expression}}
  break
}

switch Whatever.Thing {
case .Thing:
  x = 0
#if true
@unknown case _:
  x = 0
#endif
}

switch x {
case 0:
  break
@garbage case _: // expected-error {{unknown attribute 'garbage'}}
  break
}

switch x {
case 0:
  break
@garbage @moreGarbage default: // expected-error {{unknown attribute 'garbage'}} expected-error {{unknown attribute 'moreGarbage'}}
  break
}

@unknown let _ = 1 // expected-error {{unknown attribute 'unknown'}}

switch x {
case _:
  @unknown let _ = 1 // expected-error {{unknown attribute 'unknown'}}
}

switch Whatever.Thing {
case .Thing:
  break
@unknown(garbage) case _: // expected-error {{unexpected '(' in attribute 'unknown'}}
  break
}
switch Whatever.Thing {
case .Thing:
  break
@unknown // expected-note {{attribute already specified here}}
@unknown // expected-error {{duplicate attribute}}
case _:
  break
}
switch Whatever.Thing { // expected-warning {{switch must be exhaustive}} expected-note {{add missing case: '.Thing'}}
@unknown @garbage(foobar) // expected-error {{unknown attribute 'garbage'}}
case _:
  break
}

switch x { // expected-error {{switch must be exhaustive}}
case 1:
  break
@unknown case _: // expected-note {{remove '@unknown' to handle remaining values}} {{1-10=}}
  break
}

switch x { // expected-error {{switch must be exhaustive}}
@unknown case _: // expected-note {{remove '@unknown' to handle remaining values}} {{1-10=}}
  break
}

switch x { // expected-error {{switch must be exhaustive}}
@unknown default: // expected-note {{remove '@unknown' to handle remaining values}} {{1-10=}}
  break
}

switch Whatever.Thing {
case .Thing:
  break
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown case _:
  break
}

switch Whatever.Thing {
case .Thing:
  break
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown default:
  break
}

switch Whatever.Thing {
case .Thing:
  break
@unknown default:
  break
@unknown default: // expected-error {{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  break
}

switch Whatever.Thing {
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown case _:
  break
}

switch Whatever.Thing {
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown default:
  break
}

switch Whatever.Thing {
@unknown default:
  break
@unknown default: // expected-error {{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  break
}


switch x {
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown case _:
  break
}

switch x {
@unknown case _: // expected-error {{'@unknown' can only be applied to the last case in a switch}}
  break
@unknown default:
  break
}

switch x {
@unknown default:
  break
@unknown default: // expected-error {{additional 'case' blocks cannot appear after the 'default' block of a 'switch'}}
  break
}

func testReturnBeforeUnknownDefault() {
  switch x { // expected-error {{switch must be exhaustive}}
  case 1:
    return
  @unknown default: // expected-note {{remove '@unknown' to handle remaining values}}
    break
  }
}

func testReturnBeforeIncompleteUnknownDefault() {
  switch x { // expected-error {{switch must be exhaustive}}
  case 1:
    return
  @unknown default // expected-error {{expected ':' after 'default'}}
  // expected-note@-1 {{remove '@unknown' to handle remaining values}}
  }
}

func testReturnBeforeIncompleteUnknownDefault2() {
  switch x { // expected-error {{switch must be exhaustive}} expected-note {{add a default clause}}
  case 1:
    return
  @unknown // expected-error {{unknown attribute 'unknown'}}
  } // expected-error {{expected declaration}}
}

func testIncompleteArrayLiteral() {
  switch x { // expected-error {{switch must be exhaustive}}
  case 1:
    _ = [1 // expected-error {{expected ']' in container literal expression}} expected-note {{to match this opening '['}}
  @unknown default: // expected-note {{remove '@unknown' to handle remaining values}}
    ()
  }
}
