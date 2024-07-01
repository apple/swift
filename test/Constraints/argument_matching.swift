// RUN: %target-typecheck-verify-swift

// Single extraneous keyword argument (tuple-to-scalar)
func f1(_ a: Int) { }
f1(a: 5) // expected-error{{extraneous argument label 'a:' in call}}{{4-7=}}

struct X1 {
  init(_ a: Int) { }
  func f1(_ a: Int) {}
}
X1(a: 5).f1(b: 5) 
// expected-error@-1 {{extraneous argument label 'a:' in call}} {{4-7=}}
// expected-error@-2 {{extraneous argument label 'b:' in call}} {{13-16=}}

// <rdar://problem/16801056>
enum Policy {
  case Head(Int)
}

func extra2(x: Int, y: Int) { }

func testExtra2(_ policy : Policy) {
  switch (policy)
  {
    case .Head(let count):
      extra2(x: 0, y: count)
  }
}

// Single missing keyword argument (scalar-to-tuple)
func f2(a: Int) { }
f2(5) // expected-error{{missing argument label 'a:' in call}}{{4-4=a: }}

struct X2 {
  init(a: Int) { }
  func f2(b: Int) { }
}
X2(5).f2(5)
// expected-error@-1 {{missing argument label 'a:' in call}} {{4-4=a: }}
// expected-error@-2 {{missing argument label 'b:' in call}} {{10-10=b: }}


// -------------------------------------------
// Missing keywords
// -------------------------------------------

func allkeywords1(x: Int, y: Int) { }

// Missing keywords.
allkeywords1(1, 2) // expected-error{{missing argument labels}} {{14-14=x: }} {{17-17=y: }}
allkeywords1(x: 1, 2) // expected-error{{missing argument label 'y:' in call}} {{20-20=y: }}
allkeywords1(1, y: 2) // expected-error{{missing argument label 'x:' in call}} {{14-14=x: }}

// If keyword is reserved, make sure to quote it. rdar://problem/21392294
func reservedLabel(_ x: Int, `repeat`: Bool) {}
reservedLabel(1, true) // expected-error{{missing argument label 'repeat:' in call}}{{18-18=repeat: }}

// Insert missing keyword before initial backtick. rdar://problem/21392294 part 2
func reservedExpr(_ x: Int, y: Int) {}
let `do` = 2
reservedExpr(1, `do`) // expected-error{{missing argument label 'y:' in call}}{{17-17=y: }}
reservedExpr(1, y: `do`)

class GenericCtor<U> {
  init<T>(t : T) {}  // expected-note {{'init(t:)' declared here}}
}
GenericCtor<Int>()  // expected-error{{missing argument for parameter 't' in call}}

func secondArgumentNotLabeled(a: Int, _ b: Int) { }
secondArgumentNotLabeled(10, 20)
// expected-error@-1 {{missing argument label 'a:' in call}}

func f_31849281(x: Int, y: Int, z: Int) {}
f_31849281(42, y: 10, x: 20) // expected-error {{incorrect argument labels in call (have '_:y:x:', expected 'x:y:z:')}} {{12-12=x: }} {{23-24=z}}

// -------------------------------------------
// Extraneous keywords
// -------------------------------------------
func nokeywords1(_ x: Int, _ y: Int) { }

nokeywords1(x: 1, y: 1) // expected-error{{extraneous argument labels 'x:y:' in call}}{{13-16=}}{{19-22=}}

// -------------------------------------------
// Some missing, some extraneous keywords
// -------------------------------------------
func somekeywords1(_ x: Int, y: Int, z: Int) { }

somekeywords1(x: 1, y: 2, z: 3) // expected-error{{extraneous argument label 'x:' in call}}{{15-18=}}
somekeywords1(1, 2, 3) // expected-error{{missing argument labels 'y:z:' in call}}{{18-18=y: }}{{21-21=z: }}
somekeywords1(x: 1, 2, z: 3) // expected-error{{incorrect argument labels in call (have 'x:_:z:', expected '_:y:z:')}}{{15-18=}}{{21-21=y: }}

// https://github.com/apple/swift/issues/44849
// Poor diagnostic when argument label is omitted
do {
  func f(x: Int, _ y: Int) {}
  func f(a: Int, x: Int, _ y: Int) {}

  f(3, 5)             // expected-error {{missing argument label 'x:' in call}}
  f(3, y: 5)          // expected-error {{incorrect argument labels in call (have '_:y:', expected 'x:_:')}}
  f(3, x: 5)          // expected-error {{argument 'x' must precede unnamed argument #1}} {{5-5=x: 5, }} {{6-12=}}
  f(y: 3, x: 5)       // expected-error {{incorrect argument labels in call (have 'y:x:', expected 'x:_:')}} {{5-6=x}} {{11-14=}}
  f(y: 3, 5)          // expected-error {{incorrect argument label in call (have 'y:_:', expected 'x:_:')}}
  f(x: 3, x: 5)       // expected-error {{extraneous argument label 'x:' in call}}
  f(a: 1, 3, y: 5)    // expected-error {{incorrect argument labels in call (have 'a:_:y:', expected 'a:x:_:')}}
  f(1, x: 3, y: 5)    // expected-error {{incorrect argument labels in call (have '_:x:y:', expected 'a:x:_:')}}
  f(a: 1, y: 3, x: 5) // expected-error {{incorrect argument labels in call (have 'a:y:x:', expected 'a:x:_:')}}
  f(a: 1, 3, x: 5)    // expected-error {{argument 'x' must precede unnamed argument #2}} {{11-11=x: 5, }} {{12-18=}}
}

// -------------------------------------------
// Out-of-order keywords
// -------------------------------------------
allkeywords1(y: 1, x: 2) // expected-error{{argument 'x' must precede argument 'y'}} {{14-14=x: 2, }} {{18-24=}}

// rdar://problem/31849281 - Let's play "bump the argument"

struct rdar31849281 { var foo, a, b, c: Int }
_ = rdar31849281(a: 101, b: 102, c: 103, foo: 104) // expected-error {{argument 'foo' must precede argument 'a'}} {{18-18=foo: 104, }} {{40-50=}}

_ = rdar31849281(a: 101, c: 103, b: 102, foo: 104) // expected-error {{argument 'foo' must precede argument 'a'}} {{18-18=foo: 104, }} {{40-50=}}
_ = rdar31849281(foo: 104, a: 101, c: 103, b: 102) // expected-error {{argument 'b' must precede argument 'c'}} {{36-36=b: 102, }} {{42-50=}}

_ = rdar31849281(b: 102, c: 103, a: 101, foo: 104) // expected-error {{incorrect argument labels in call (have 'b:c:a:foo:', expected 'foo:a:b:c:')}} {{18-19=foo}} {{26-27=a}} {{34-35=b}} {{42-45=c}}
_ = rdar31849281(foo: 104, b: 102, c: 103, a: 101) // expected-error {{argument 'a' must precede argument 'b'}} {{28-28=a: 101, }} {{42-50=}}

func fun_31849281(a: (Bool) -> Bool, b: (Int) -> (String), c: [Int?]) {}
fun_31849281(c: [nil, 42], a: { !$0 }, b: { (num: Int) -> String in return "\(num)" })
// expected-error @-1 {{incorrect argument labels in call (have 'c:a:b:', expected 'a:b:c:')}} {{14-15=a}} {{28-29=b}} {{40-41=c}}
fun_31849281(a: { !$0 }, c: [nil, 42], b: { (num: Int) -> String in return String(describing: num) })
// expected-error @-1 {{argument 'b' must precede argument 'c'}} {{26-26=b: { (num: Int) -> String in return String(describing: num) }, }} {{38-101=}}
fun_31849281(a: { !$0 }, c: [nil, 42], b: { "\($0)" })
// expected-error @-1 {{argument 'b' must precede argument 'c'}} {{26-26=b: { "\\($0)" }, }} {{38-54=}}

struct ReorderAndAllLabels {
  func f(aa: Int, bb: Int, cc: Int, dd: Int) {}

  func test() {
    f(bb: 1, ccx: 2, ddx: 3, aa: 0) // expected-error {{argument 'aa' must precede argument 'bb'}} {{28-35=}} {{7-7=aa: 0, }} {{none}}

    f(bbx: 1, ccx: 2, ddx: 3, aa: 0) // expected-error {{incorrect argument labels in call (have 'bbx:ccx:ddx:aa:', expected 'aa:bb:cc:dd:')}} {{7-10=aa}} {{15-18=bb}} {{23-26=cc}} {{31-33=dd}} {{none}}
  }
}

// -------------------------------------------
// Default arguments
// -------------------------------------------

func defargs1(x: Int = 1, y: Int = 2, z: Int = 3) {}

// Using defaults (in-order)
defargs1() 
defargs1(x: 1)
defargs1(x: 1, y: 2)

// Using defaults (in-order, some missing)
defargs1(y: 2)
defargs1(y: 2, z: 3)
defargs1(z: 3)
defargs1(x: 1, z: 3)

// Using defaults (out-of-order, error by SE-0060)
defargs1(z: 3, y: 2, x: 1) // expected-error{{argument 'x' must precede argument 'z'}} {{10-10=x: 1, }} {{20-26=}}
defargs1(x: 1, z: 3, y: 2) // expected-error{{argument 'y' must precede argument 'z'}} {{16-16=y: 2, }} {{20-26=}}
defargs1(y: 2, x: 1) // expected-error{{argument 'x' must precede argument 'y'}} {{10-10=x: 1, }} {{14-20=}}

// Default arguments "boxed in".
func defargs2(first: Int, x: Int = 1, y: Int = 2, z: Int = 3, last: Int) { }

// Using defaults in the middle (in-order, some missing)
defargs2(first: 1, x: 1, z: 3, last: 4)
defargs2(first: 1, x: 1, last: 4)
defargs2(first: 1, y: 2, z: 3, last: 4)
defargs2(first: 1, last: 4)

// Using defaults in the middle (out-of-order, error by SE-0060)
defargs2(first: 1, z: 3, x: 1, last: 4) // expected-error{{argument 'x' must precede argument 'z'}} {{20-20=x: 1, }} {{24-30=}}
defargs2(first: 1, z: 3, y: 2, last: 4) // expected-error{{argument 'y' must precede argument 'z'}} {{20-20=y: 2, }} {{24-30=}}

// Using defaults that have moved past a non-defaulted parameter
defargs2(x: 1, first: 1, last: 4) // expected-error{{argument 'first' must precede argument 'x'}} {{10-10=first: 1, }} {{14-24=}}
defargs2(first: 1, last: 4, x: 1) // expected-error{{argument 'x' must precede argument 'last'}} {{20-20=x: 1, }} {{27-33=}}

func rdar43525641(_ a: Int, _ b: Int = 0, c: Int = 0, _ d: Int) {}
rdar43525641(1, c: 2, 3) // Ok

func testLabelErrorDefault() {
  func f(aa: Int, bb: Int, cc: Int = 0) {}

  f(aax: 0, bbx: 1, cc: 2)
  // expected-error@-1 {{incorrect argument labels in call (have 'aax:bbx:cc:', expected 'aa:bb:cc:')}}

  f(aax: 0, bbx: 1)
  // expected-error@-1 {{incorrect argument labels in call (have 'aax:bbx:', expected 'aa:bb:')}}
}

// -------------------------------------------
// Variadics
// -------------------------------------------
func variadics1(x: Int, y: Int, _ z: Int...) { }

// Using variadics (in-order, complete)
variadics1(x: 1, y: 2)
variadics1(x: 1, y: 2, 1)
variadics1(x: 1, y: 2, 1, 2)
variadics1(x: 1, y: 2, 1, 2, 3)

// Using various (out-of-order)
variadics1(1, 2, 3, 4, 5, x: 6, y: 7) // expected-error {{incorrect argument labels in call (have '_:_:_:_:_:x:y:', expected 'x:y:_:')}} {{12-12=x: }} {{15-15=y: }} {{27-30=}} {{33-36=}}

func variadics2(x: Int, y: Int = 2, z: Int...) { } // expected-note {{'variadics2(x:y:z:)' declared here}}

// Using variadics (in-order, complete)
variadics2(x: 1, y: 2, z: 1)
variadics2(x: 1, y: 2, z: 1, 2)
variadics2(x: 1, y: 2, z: 1, 2, 3)

// Using variadics (in-order, some missing)
variadics2(x: 1, z: 1, 2, 3)
variadics2(x: 1)

// Using variadics (out-of-order)
variadics2(z: 1, 2, 3, y: 2) // expected-error{{missing argument for parameter 'x' in call}}
variadics2(z: 1, 2, 3, x: 1) // expected-error{{argument 'x' must precede argument 'z'}} {{12-12=x: 1, }} {{22-28=}}

func variadics3(_ x: Int..., y: Int = 2, z: Int = 3) { }

// Using variadics (in-order, complete)
variadics3(1, 2, 3, y: 0, z: 1)
variadics3(1, y: 0, z: 1)
variadics3(y: 0, z: 1)

// Using variadics (in-order, some missing)
variadics3(1, 2, 3, y: 0)
variadics3(1, z: 1)
variadics3(z: 1)

variadics3(1, 2, 3, z: 1)
variadics3(1, z: 1)
variadics3(z: 1)

variadics3(1, 2, 3)
variadics3(1)
variadics3()

// Using variadics (out-of-order)
variadics3(y: 0, 1, 2, 3) // expected-error{{unnamed argument #2 must precede argument 'y'}} {{12-12=1, 2, 3, }} {{16-25=}}
variadics3(z: 1, 1) // expected-error{{unnamed argument #2 must precede argument 'z'}} {{12-12=1, }} {{16-19=}}

func variadics4(x: Int..., y: Int = 2, z: Int = 3) { }

// Using variadics (in-order, complete)
variadics4(x: 1, 2, 3, y: 0, z: 1)
variadics4(x: 1, y: 0, z: 1)
variadics4(y: 0, z: 1)

// Using variadics (in-order, some missing)
variadics4(x: 1, 2, 3, y: 0)
variadics4(x: 1, z: 1)
variadics4(z: 1)

variadics4(x: 1, 2, 3, z: 1)
variadics4(x: 1, z: 1)
variadics4(z: 1)

variadics4(x: 1, 2, 3)
variadics4(x: 1)
variadics4()

// Using variadics (in-order, some missing)
variadics4(y: 0, x: 1, 2, 3) // expected-error{{argument 'x' must precede argument 'y'}} {{12-12=x: 1, 2, 3, }} {{16-28=}}
variadics4(z: 1, x: 1) // expected-error{{argument 'x' must precede argument 'z'}} {{12-12=x: 1, }} {{16-22=}}

func variadics5(_ x: Int, y: Int, _ z: Int...) { } // expected-note {{declared here}}

// Using variadics (in-order, complete)
variadics5(1, y: 2)
variadics5(1, y: 2, 1)
variadics5(1, y: 2, 1, 2)
variadics5(1, y: 2, 1, 2, 3)

// Using various (out-of-order)
variadics5(1, 2, 3, 4, 5, 6, y: 7) // expected-error{{argument 'y' must precede unnamed argument #2}} {{15-15=y: 7, }} {{28-34=}}
variadics5(y: 1, 2, 3, 4, 5, 6, 7) // expected-error{{missing argument for parameter #1 in call}}

func variadics6(x: Int..., y: Int = 2, z: Int) { } // expected-note 4 {{'variadics6(x:y:z:)' declared here}}

// Using variadics (in-order, complete)
variadics6(x: 1, 2, 3, y: 0, z: 1)
variadics6(x: 1, y: 0, z: 1)
variadics6(y: 0, z: 1)

// Using variadics (in-order, some missing)
variadics6(x: 1, 2, 3, y: 0) // expected-error{{missing argument for parameter 'z' in call}}
variadics6(x: 1, z: 1)
variadics6(z: 1)

variadics6(x: 1, 2, 3, z: 1)
variadics6(x: 1, z: 1)
variadics6(z: 1)

variadics6(x: 1, 2, 3) // expected-error{{missing argument for parameter 'z' in call}}
variadics6(x: 1) // expected-error{{missing argument for parameter 'z' in call}}
variadics6() // expected-error{{missing argument for parameter 'z' in call}}

func variadics7(_ x: Int..., y: Int...) { }

// Using multiple variadics (in order, complete)
variadics7(1, y: 2)
variadics7(1, 2, 3, y: 4, 5, 6)
variadics7(1, 2, y: 2)
variadics7(1, y: 2, 1)

// multiple variadics, in order, some missing
variadics7(y: 1)
variadics7(1)
variadics7(y: 4, 5, 6)
variadics7(1, 2, 3)

func variadics8(x: Int..., y: Int...) { }

// multiple variadics, out of order
variadics8(y: 1, x: 2) // expected-error {{argument 'x' must precede argument 'y'}} {{12-12=x: 2, }} {{16-22=}}
variadics8(y: 1, 2, 3, x: 4) // expected-error {{argument 'x' must precede argument 'y'}} {{12-12=x: 4, }} {{22-28=}}
variadics8(y: 1, x: 2, 3, 4) // expected-error {{argument 'x' must precede argument 'y'}} {{12-12=x: 2, 3, 4, }} {{16-28=}}
variadics8(y: 1, 2, 3, x: 4, 5, 6) // expected-error {{argument 'x' must precede argument 'y'}} {{12-12=x: 4, 5, 6, }} {{22-34=}}

func variadics9(_ a: Int..., b: Int, _ c: Int...) { } // expected-note {{'variadics9(_:b:_:)' declared here}}

// multiple split variadics, in order, complete
variadics9(1, b: 2, 3)
variadics9(1, 2, 3, b: 2, 3)
variadics9(1, b: 2, 3, 2, 1)
variadics9(1, 2, 3, b: 2, 3, 2, 1)

// multiple split variadics, in order, some missing
variadics9(b: 2, 3)
variadics9(1, b: 2)
variadics9(1, 2, b: 2)
variadics9(b: 2, 3, 2, 1)

// multiple split variadics, required missing
variadics9(1) // expected-error {{missing argument for parameter 'b' in call}}

func variadics10(_ a: Int..., b: Int = 2, _ c: Int...) { }

// multiple unlabeled variadics split by defaulted param, in order, complete
variadics10(1, b: 2, 3)
variadics10(1, 2, 3, b: 2, 3)
variadics10(1, b: 2, 3, 2, 1)
variadics10(1, 2, 3, b: 2, 3, 2, 1)

// multiple unlabeled variadics split by defaulted param, in order, some missing
variadics10(1, 2, 3)
variadics10(1, 2, 3, b: 3)
variadics10(b: 3)

func variadics11(_ a: Int..., b: Bool = false, _ c: String...) { }

variadics11(1, 2, 3, b: true, "hello", "world")
variadics11(b: true, "hello", "world")
variadics11(1, 2, 3, b: true)
variadics11(b: true)
variadics11()
variadics11(1, 2, 3, "hello", "world") // expected-error 2 {{cannot convert value of type 'String' to expected argument type 'Int'}}

func variadics12(a: Int..., b: Int, c: Int...) { }

variadics12(a: 1, 2, 3, b: 4, c: 5, 6, 7)
variadics12(b: 4, c: 5, 6, 7)
variadics12(a: 1, 2, 3, b: 4)

variadics12(c: 5, 6, 7, b: 4, a: 1, 2, 3) // expected-error {{incorrect argument labels in call (have 'c:_:_:b:a:_:_:', expected 'a:b:c:')}} {{13-14=a}} {{19-19=b: }} {{22-22=c: }} {{25-28=}} {{31-34=}}


// Edge cases involving multiple trailing closures and forward matching.
func variadics13(a: Int..., b: (()->Void)...) {}

variadics13()
variadics13(a: 1, 2, 3) {} _: {} _: {}
variadics13() {} _: {} _: {}
variadics13(a: 1, 2, 3)
variadics13(a: 1, 2, 3) {}

func variadics14(a: (()->Void)..., b: (()->Void)...) {} // expected-note {{'variadics14(a:b:)' declared here}}

variadics14(a: {}, {}, b: {}, {})
variadics14(a: {}, {}) {} _: {}
variadics14 {} _: {} b: {} _: {}
variadics14 {} b: {}
variadics14 {} // expected-warning {{backward matching of the unlabeled trailing closure is deprecated; label the argument with 'b' to suppress this warning}}

func outOfOrder(_ a : Int, b: Int) {
  outOfOrder(b: 42, 52)  // expected-error {{unnamed argument #2 must precede argument 'b'}} {{14-14=52, }} {{19-23=}}
}

struct Variadics7 {
  func f(alpha: Int..., bravo: Int) {} // expected-note {{'f(alpha:bravo:)' declared here}}
  // expected-note@-1 {{'f(alpha:bravo:)' declared here}}

  func test() {
    // no error
    f(bravo: 0)
    f(alpha: 0, bravo: 3)
    f(alpha: 0, 1, bravo: 3)
    f(alpha: 0, 1, 2, bravo: 3)

    // OoO
    f(bravo: 0, alpha: 1) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2, 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}

    // typo A
    f(alphax: 0, bravo: 3) // expected-error {{incorrect argument label in call (have 'alphax:bravo:', expected 'alpha:bravo:')}}
    f(alphax: 0, 1, bravo: 3) // expected-error {{extra argument in call}}
    f(alphax: 0, 1, 2, bravo: 3) // expected-error {{extra arguments at positions #2, #3 in call}}

    // typo B
    f(bravox: 0) // expected-error {{incorrect argument label in call (have 'bravox:', expected 'bravo:')}}
    f(alpha: 0, bravox: 3) // expected-error {{incorrect argument label in call (have 'alpha:bravox:', expected 'alpha:bravo:')}}
    f(alpha: 0, 1, bravox: 3) // expected-error {{incorrect argument label in call (have 'alpha:_:bravox:', expected 'alpha:_:bravo:')}}
    f(alpha: 0, 1, 2, bravox: 3) // expected-error {{incorrect argument label in call (have 'alpha:_:_:bravox:', expected 'alpha:_:_:bravo:')}}

    // OoO + typo A B
    f(bravox: 0, alphax: 1) // expected-error {{incorrect argument labels in call (have 'bravox:alphax:', expected 'alpha:bravo:')}}
    f(bravox: 0, alphax: 1, 2) // expected-error {{extra argument in call}}
    f(bravox: 0, alphax: 1, 2, 3) // expected-error {{extra arguments at positions #3, #4 in call}}
  }
}

struct Variadics8 {
  func f(alpha: Int..., bravo: Int, charlie: Int) {} // expected-note {{'f(alpha:bravo:charlie:)' declared here}}

  func test() {
    // no error
    f(bravo: 3, charlie: 4)
    f(alpha: 0, bravo: 3, charlie: 4)
    f(alpha: 0, 1, bravo: 3, charlie: 4)
    f(alpha: 0, 1, 2, bravo: 3, charlie: 4)

    // OoO ACB
    f(charlie: 3, bravo: 4) // expected-error {{argument 'bravo' must precede argument 'charlie'}}
    f(alpha: 0, charlie: 3, bravo: 4) // expected-error {{argument 'bravo' must precede argument 'charlie'}}
    f(alpha: 0, 1, charlie: 3, bravo: 4) // expected-error {{argument 'bravo' must precede argument 'charlie'}}
    f(alpha: 0, 1, 2, charlie: 3, bravo: 4) // expected-error {{argument 'bravo' must precede argument 'charlie'}}
    // OoO CAB
    f(charlie: 0, alpha: 1, bravo: 4) // expected-error {{incorrect argument labels in call (have 'charlie:alpha:bravo:', expected 'alpha:bravo:charlie:')}}
    f(charlie: 0, alpha: 1, 2, bravo: 4) // expected-error {{incorrect argument labels in call (have 'charlie:alpha:_:bravo:', expected 'alpha:bravo:charlie:')}}
    f(charlie: 0, alpha: 1, 2, 3, bravo: 4) // expected-error {{incorrect argument labels in call (have 'charlie:alpha:_:_:bravo:', expected 'alpha:bravo:charlie:')}}
    // OoO BAC
    f(bravo: 0, alpha: 1, charlie: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2, charlie: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2, 3, charlie: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}

    // typo A
    f(alphax: 0, bravo: 3, charlie: 4) // expected-error {{incorrect argument label in call (have 'alphax:bravo:charlie:', expected 'alpha:bravo:charlie:')}}
    f(alphax: 0, 1, bravo: 3, charlie: 4) // expected-error {{extra argument in call}}
    f(alphax: 0, 1, 2, bravo: 3, charlie: 4) // expected-error {{extra arguments at positions #2, #3 in call}}
    // typo B
    f(bravox: 3, charlie: 4) // expected-error {{incorrect argument label in call (have 'bravox:charlie:', expected 'bravo:charlie:')}}
    f(alpha: 0, bravox: 3, charlie: 4) // expected-error {{incorrect argument label in call (have 'alpha:bravox:charlie:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, bravox: 3, charlie: 4) // expected-error {{incorrect argument label in call (have 'alpha:_:bravox:charlie:', expected 'alpha:_:bravo:charlie:')}}
    f(alpha: 0, 1, 2, bravox: 3, charlie: 4) // expected-error {{incorrect argument label in call (have 'alpha:_:_:bravox:charlie:', expected 'alpha:_:_:bravo:charlie:')}}
    // typo C
    f(bravo: 3, charliex: 4) // expected-error {{incorrect argument label in call (have 'bravo:charliex:', expected 'bravo:charlie:')}}
    f(alpha: 0, bravo: 3, charliex: 4) // expected-error {{incorrect argument label in call (have 'alpha:bravo:charliex:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, bravo: 3, charliex: 4) // expected-error {{incorrect argument label in call (have 'alpha:_:bravo:charliex:', expected 'alpha:_:bravo:charlie:')}}
    f(alpha: 0, 1, 2, bravo: 3, charliex: 4) // expected-error {{incorrect argument label in call (have 'alpha:_:_:bravo:charliex:', expected 'alpha:_:_:bravo:charlie:')}}

    // OoO ACB + typo B
    f(alpha: 0, charlie: 3, bravox: 4) // expected-error {{incorrect argument labels in call (have 'alpha:charlie:bravox:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, charlie: 3, bravox: 4) // expected-error {{incorrect argument labels in call (have 'alpha:_:charlie:bravox:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, 2, charlie: 3, bravox: 4) // expected-error {{incorrect argument labels in call (have 'alpha:_:_:charlie:bravox:', expected 'alpha:bravo:charlie:')}}
    // OoO ACB + typo C
    f(charliex: 3, bravo: 4) // expected-error {{incorrect argument labels in call (have 'charliex:bravo:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, charliex: 3, bravo: 4) // expected-error {{incorrect argument labels in call (have 'alpha:charliex:bravo:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, charliex: 3, bravo: 4) // expected-error {{incorrect argument labels in call (have 'alpha:_:charliex:bravo:', expected 'alpha:bravo:charlie:')}}
    f(alpha: 0, 1, 2, charliex: 3, bravo: 4) // expected-error {{incorrect argument labels in call (have 'alpha:_:_:charliex:bravo:', expected 'alpha:bravo:charlie:')}}

    // OoO BAC + typo B
    f(bravox: 0, alpha: 1, charlie: 4) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:', expected 'alpha:bravo:charlie:')}}
    f(bravox: 0, alpha: 1, 2, charlie: 4) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:_:charlie:', expected 'alpha:bravo:charlie:')}}
    f(bravox: 0, alpha: 1, 2, 3, charlie: 4) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:_:_:charlie:', expected 'alpha:bravo:charlie:')}}
    // OoO BAC + typo C
    f(bravo: 0, alpha: 1, charliex: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2, charliex: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f(bravo: 0, alpha: 1, 2, 3, charliex: 4) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
  }
}

func var_31849281(_ a: Int, _ b: Int..., c: Int) {}
var_31849281(1, c: 10, 3, 4, 5, 6, 7, 8, 9) // expected-error {{unnamed argument #3 must precede argument 'c'}} {{17-17=3, 4, 5, 6, 7, 8, 9, }} {{22-43=}}

func testLabelErrorVariadic() {
  func f(aa: Int, bb: Int, cc: Int...) {}

  f(aax: 0, bbx: 1, cc: 2, 3, 4)
  // expected-error@-1 {{incorrect argument labels in call (have 'aax:bbx:cc:_:_:', expected 'aa:bb:cc:_:_:')}}

  f(aax: 0, bbx: 1)
  // expected-error@-1 {{incorrect argument labels in call (have 'aax:bbx:', expected 'aa:bb:')}}
}

// -------------------------------------------
// Positions around defaults and variadics 
// -------------------------------------------

struct PositionsAroundDefaultsAndVariadics {
  // unlabeled defaulted around labeled parameter
  func f1(_ a: Bool = false, _ b: Int = 0, c: String = "", _ d: [Int] = []) {}

  func test_f1() {
    f1(true, 2, c: "3", [4])

    f1(true, c: "3", 2, [4]) // expected-error {{unnamed argument #4 must precede argument 'c'}}

    f1(true, c: "3", [4], 2) // expected-error {{unnamed argument #4 must precede argument 'c'}}

    f1(true, c: "3", 2) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int]'}}

    f1(true, c: "3", [4])

    f1(c: "3", 2, [4]) // expected-error {{unnamed argument #3 must precede argument 'c'}}

    f1(c: "3", [4], 2) // expected-error {{unnamed argument #3 must precede argument 'c'}}
    
    f1(c: "3", 2) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int]'}}

    f1(c: "3", [4])

    f1(b: "2", [3]) // expected-error {{incorrect argument labels in call (have 'b:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{cannot convert value of type '[Int]' to expected argument type 'Bool'}}
    
    f1(b: "2", 1) // expected-error {{incorrect argument labels in call (have 'b:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{integer literal value '1' cannot be used as a boolean; did you mean 'true'?}}

    f1(b: "2", [3], 1) // expected-error {{incorrect argument labels in call (have 'b:_:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{cannot convert value of type '[Int]' to expected argument type 'Bool'}}

    f1(b: "2", 1, [3]) // expected-error {{incorrect argument labels in call (have 'b:_:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{integer literal value '1' cannot be used as a boolean; did you mean 'true'?}}
    // expected-error@-2 {{cannot convert value of type '[Int]' to expected argument type 'Int'}}
  }

  // unlabeled variadics before labeled parameter
  func f2(_ a: Bool = false, _ b: Int..., c: String = "", _ d: [Int] = []) {}

  func test_f2() {
    f2(true, 21, 22, 23, c: "3", [4])

    f2(true, "21", 22, 23, c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f2(true, 21, "22", 23, c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f2(true, 21, 22, "23", c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f2(true, 21, 22, c: "3", [4])
    f2(true, 21, c: "3", [4])
    f2(true, c: "3", [4])

    f2(true, c: "3", 21) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int]'}}

    f2(true, c: "3", 21, [4]) // expected-error {{unnamed argument #4 must precede argument 'c'}}
    // expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-2 {{remove brackets to pass array elements directly}}

    f2(true, c: "3", [4], 21) // expected-error {{unnamed argument #4 must precede argument 'c'}}

    f2(true, [4]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-1 {{remove brackets to pass array elements directly}}

    f2(true, 21, [4]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-1 {{remove brackets to pass array elements directly}}
    
    f2(true, 21, 22, [4]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-1 {{remove brackets to pass array elements directly}}

    f2(21, 22, 23, c: "3", [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f2(21, 22, c: "3", [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f2(21, c: "3", [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f2(c: "3", [4])
    f2(c: "3")
    f2()

    f2(c: "3", 21) // expected-error {{cannot convert value of type 'Int' to expected argument type '[Int]'}}
    
    f2(c: "3", 21, [4]) // expected-error {{incorrect argument labels in call (have 'c:_:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{cannot convert value of type 'Int' to expected argument type '[Int]'}}
    // expected-error@-2 {{cannot convert value of type '[Int]' to expected argument type 'Bool'}}

    f2(c: "3", [4], 21) // expected-error {{incorrect argument labels in call (have 'c:_:_:', expected '_:_:c:_:')}}
    // expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f2([4]) // expected-error {{cannot convert value of type '[Int]' to expected argument type 'Bool'}}

    f2(21, [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}
    // expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-2 {{remove brackets to pass array elements directly}}

    f2(21, 22, [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}
    // expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-2 {{remove brackets to pass array elements directly}}
  }

  // labeled variadics before labeled parameter
  func f3(_ a: Bool = false, b: Int..., c: String = "", _ d: [Int] = []) {}

  func test_f3() {
    f3(true, b: 21, 22, 23, c: "3", [4])

    f3(true, b: "21", 22, 23, c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
    
    f3(true, b: 21, "22", 23, c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
    
    f3(true, b: 21, 22, "23", c: "3", [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f3(true, b: 21, 22, c: "3", [4])
    f3(true, b: 21, c: "3", [4])
    f3(true, c: "3", [4])

    f3(true, c: "3", b: 21) // expected-error {{argument 'b' must precede argument 'c'}}

    f3(true, c: "3", b: 21, [4]) // expected-error {{argument 'b' must precede argument 'c'}}
    // expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-2 {{remove brackets to pass array elements directly}}

    f3(true, c: "3", [4], b: 21) // expected-error {{argument 'b' must precede argument 'c'}}

    f3(true, b: 21, [4]) // expected-error {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-1 {{remove brackets to pass array elements directly}}

    f3(b: 21, 22, 23, c: "3", [4])
    f3(b: 21, 22, c: "3", [4])
    f3(b: 21, c: "3", [4])
    f3(c: "3", [4])

    f3([4]) // expected-error {{cannot convert value of type '[Int]' to expected argument type 'Bool'}}
    
    f3()

    f3(c: "3", b: 21) // expected-error {{argument 'b' must precede argument 'c'}}

    f3(c: "3", b: 21, [4]) // expected-error {{argument 'b' must precede argument 'c'}}
    // expected-error@-1 {{cannot pass array of type '[Int]' as variadic arguments of type 'Int'}}
    // expected-note@-2 {{remove brackets to pass array elements directly}}

    f3(c: "3", [4], b: 21) // expected-error {{argument 'b' must precede argument 'c'}}
  }

  // unlabeled variadics after labeled parameter
  func f4(_ a: Bool = false, b: String = "", _ c: Int..., d: [Int] = []) {}

  func test_f4() {
    f4(true, b: "2", 31, 32, 33, d: [4])
    
    f4(true, b: "2", "31", 32, 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f4(true, b: "2", 31, "32", 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
    
    f4(true, b: "2", 31, 32, "33", d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
    
    f4(true, b: "2", 31, 32, d: [4])
    f4(true, b: "2", 31, d: [4])
    f4(true, b: "2", d: [4])

    f4(true, 31, b: "2", d: [4]) // expected-error {{argument 'b' must precede unnamed argument #2}}

    f4(true, b: "2", d: [4], 31) // expected-error {{unnamed argument #4 must precede argument 'd'}}

    f4(true, b: "2", 31)
    f4(true, b: "2")

    f4(true)
    f4(true, 31)
    f4(true, 31, d: [4])
    f4(true, 31, 32)
    f4(true, 31, 32, d: [4])

    f4(b: "2", 31, 32, 33, d: [4])

    f4(b: "2", "31", 32, 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f4(b: "2", 31, "32", 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f4(b: "2", 31, 32, "33", d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f4(b: "2", 31, 32, d: [4])
    f4(b: "2", 31, d: [4])
    f4(b: "2", d: [4])

    f4(31, b: "2", d: [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f4(b: "2", d: [4], 31) // expected-error {{unnamed argument #3 must precede argument 'b'}}

    f4(b: "2", 31)
    f4(b: "2", 31, 32)
    f4(b: "2")

    f4()
    
    f4(31) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f4(31, d: [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f4(31, 32) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}

    f4(31, 32, d: [4]) // expected-error {{cannot convert value of type 'Int' to expected argument type 'Bool'}}
  }

  // labeled variadics after labeled parameter
  func f5(_ a: Bool = false, b: String = "", c: Int..., d: [Int] = []) {}

  func test_f5() {
    f5(true, b: "2", c: 31, 32, 33, d: [4])
    
    f5(true, b: "2", c: "31", 32, 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f5(true, b: "2", c: 31, "32", 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f5(true, b: "2", c: 31, 32, "33", d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
    
    f5(true, b: "2", c: 31, 32, d: [4])
    f5(true, b: "2", c: 31, d: [4])
    f5(true, b: "2", d: [4])

    f5(true, c: 31, b: "2", d: [4]) // expected-error {{argument 'b' must precede argument 'c'}}

    f5(true, b: "2", d: [4], 31) // expected-error {{incorrect argument labels in call (have '_:b:d:_:', expected '_:b:c:d:')}}

    f5(true, b: "2", c: 31)
    f5(true, b: "2")

    f5(true)
    f5(true, c: 31)
    f5(true, c: 31, d: [4])
    f5(true, c: 31, 32)
    f5(true, c: 31, 32, d: [4])

    f5(b: "2", c: 31, 32, 33, d: [4])
    
    f5(b: "2", c: "31", 32, 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f5(b: "2", c: 31, "32", 33, d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f5(b: "2", c: 31, 32, "33", d: [4]) // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}

    f5(b: "2", c: 31, 32, d: [4])
    f5(b: "2", c: 31, d: [4])
    f5(b: "2", d: [4])

    f5(c: 31, b: "2", d: [4]) // expected-error {{argument 'b' must precede argument 'c'}}

    f5(b: "2", d: [4], c: 31) // expected-error {{argument 'c' must precede argument 'd'}}

    f5(b: "2", c: 31)
    f5(b: "2", c: 31, 32)
    f5(b: "2")

    f5()
    f5(c: 31)
    f5(c: 31, d: [4])
    f5(c: 31, 32)
    f5(c: 31, 32, d: [4])
  }
}

// -------------------------------------------
// Matching position of unlabeled parameters
// -------------------------------------------

func testUnlabeledParameterBindingPosition() {
  do {
    func f(_ aa: Int) {}

    f(1) // ok

    f(xx: 1)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}

    f(0, 1)
    // expected-error@-1:10 {{extra argument in call}}

    f(xx: 1, 2)
    // expected-error@-1:14 {{extra argument in call}}

    f(1, xx: 2)
    // expected-error@-1:14 {{extra argument 'xx' in call}}

    f(xx: 1, yy: 2)
    // expected-error@-1:18 {{extra argument 'yy' in call}}
  }

  do {
    func f(aa: Int) { }

    f(1)
    // expected-error@-1:7 {{missing argument label 'aa:' in call}}

    f(aa: 1) // ok

    f(xx: 1)
    // expected-error@-1 {{incorrect argument label in call (have 'xx:', expected 'aa:')}}
  }

  do {
    func f(_ aa: Int, _ bb: Int) { }
    // expected-note@-1 3 {{'f' declared here}}

    f(1)
    // expected-error@-1:8 {{missing argument for parameter #2 in call}}

    f(xx: 1)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:12 {{missing argument for parameter #2 in call}}

    f(1, 2) // ok

    f(1, xx: 2)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}

    f(xx: 1, 2)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}

    f(xx: 1, yy: 2)
    // expected-error@-1 {{extraneous argument labels 'xx:yy:' in call}}

    f(xx: 1, 2, 3)
    // expected-error@-1:17 {{extra argument in call}}

    f(1, xx: 2, 3)
    // expected-error@-1:17 {{extra argument in call}}

    f(1, 2, xx: 3)
    // expected-error@-1:17 {{extra argument 'xx' in call}}

    f(xx: 1, yy: 2, 3)
    // expected-error@-1:21 {{extra argument in call}}

    f(xx: 1, yy: 2, 3, 4)
    // expected-error@-1:6 {{extra arguments at positions #3, #4 in call}}
  }

  do {
    func f(_ aa: Int = 0, _ bb: Int) { }
    // expected-note@-1 {{'f' declared here}}

    f(1)
    // expected-error@-1:8 {{missing argument for parameter #2 in call}}

    f(1, 2) // ok
  }

  do {
    func f(_ aa: Int, bb: Int) { }
    // expected-note@-1 3 {{'f(_:bb:)' declared here}}

    f(1)
    // expected-error@-1:8 {{missing argument for parameter 'bb' in call}}

    f(1, 2)
    // expected-error@-1 {{missing argument label 'bb:' in call}}

    f(1, bb: 2) // ok

    f(xx: 1, 2)
    // expected-error@-1 {{incorrect argument labels in call (have 'xx:_:', expected '_:bb:')}}

    f(xx: 1, bb: 2)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}

    f(bb: 1, 2, 3)
    // expected-error@-1:17 {{extra argument in call}}

    f(1, bb: 2, 3)
    // expected-error@-1:17 {{extra argument in call}}

    f(1, 2, bb: 3)
    // expected-error@-1:10 {{extra argument in call}}

    f(xx: 1, 2, 3)
    // expected-error@-1:17 {{extra argument in call}}

    f(1, xx: 2, 3)
    // expected-error@-1:6 {{extra arguments at positions #2, #3 in call}}
    // expected-error@-2:8 {{missing argument for parameter 'bb' in call}}

    f(1, 2, xx: 3)
    // expected-error@-1:17 {{extra argument 'xx' in call}}
  }

  do {
    // expected-note@+1 *{{'f(aa:_:)' declared here}}
    func f(aa: Int, _ bb: Int) { }

    f(1)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}

    f(0, 1)
    // expected-error@-1:6 {{missing argument label 'aa:' in call}}

    f(0, xx: 1)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:14 {{extra argument 'xx' in call}}

    f(xx: 0, 1)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:14 {{extra argument in call}}

    f(0, 1, 9)
    // expected-error@-1:13 {{extra argument in call}}

    f(0, 1, xx: 9)
    // expected-error@-1:17 {{extra argument 'xx' in call}}

    f(xx: 91, 1, 92)
    // expected-error@-1:6 {{extra arguments at positions #2, #3 in call}}
    // expected-error@-2:7 {{missing argument for parameter 'aa' in call}}

    f(1, xx: 2, 3)
    // expected-error@-1:6 {{extra arguments at positions #2, #3 in call}}
    // expected-error@-2:7 {{missing argument for parameter 'aa' in call}}

    f(1, 2, xx: 3)
    // expected-error@-1:17 {{extra argument 'xx' in call}}
  }

  do {
    func f(_ aa: Int, _ bb: Int = 82, _ cc: Int) { }
    // expected-note@-1 {{'f' declared here}}

    f(1, 2)
    // expected-error@-1:11 {{missing argument for parameter #3 in call}}

    f(1, 2, 3) // ok
  }

  do {
    func f(_ aa: Int, _ bb: Int, cc: Int) { }

    f(1, 2, cc: 3) // ok

    f(1, 2, xx: 3)
    // expected-error@-1 {{incorrect argument label in call (have '_:_:xx:', expected '_:_:cc:')}}

    f(1, cc: 2, 3)
    // expected-error@-1 {{unnamed argument #3 must precede argument 'cc'}}

    f(1, xx: 2, 3)
    // expected-error@-1 {{incorrect argument labels in call (have '_:xx:_:', expected '_:_:cc:')}}

    f(cc: 1, 2, 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'cc:_:_:', expected '_:_:cc:')}}

    f(xx: 1, 2, 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'xx:_:_:', expected '_:_:cc:')}}

    f(xx: 1, yy: 2, 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'xx:yy:_:', expected '_:_:cc:')}}

    f(xx: 1, 2, yy: 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'xx:_:yy:', expected '_:_:cc:')}}

    f(1, xx: 2, yy: 3)
    // expected-error@-1 {{incorrect argument labels in call (have '_:xx:yy:', expected '_:_:cc:')}}
  }

  do {
    func f(_ aa: Int, bb: Int, _ cc: Int) { }
    // expected-note@-1 4 {{'f(_:bb:_:)' declared here}}

    f(1)
    // expected-error@-1:7 {{missing arguments for parameters 'bb', #3 in call}}

    f(1, 2)
    // expected-error@-1:8 {{missing argument for parameter 'bb' in call}}

    f(1, 2, 3)
    // expected-error@-1 {{missing argument label 'bb:' in call}}

    f(1, 2, bb: 3)
    // expected-error@-1 {{argument 'bb' must precede unnamed argument #2}}

    f(1, bb: 2, 3) // ok

    f(bb: 1, 0, 2)
    // expected-error@-1 {{unnamed argument #3 must precede argument 'bb'}}

    f(xx: 1, 2, 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'xx:_:_:', expected '_:bb:_:')}}

    f(1, xx: 2, 3)
    // expected-error@-1:17 {{extra argument in call}}
    // expected-error@-2:8 {{missing argument for parameter 'bb' in call}}

    f(1, 2, xx: 3)
    // expected-error@-1:8 {{missing argument for parameter 'bb' in call}}
    // expected-error@-2:17 {{extra argument 'xx' in call}}
  }

  do {
    func f(_ aa: Int = 80, bb: Int, _ cc: Int) {}

    f(bb: 1, 2) // ok
  }

  do {
    // expected-note@+1 *{{'f(_:bb:_:)' declared here}}
    func f(_ aa: Int, bb: Int, _ cc: Int...) { }

    f(bb: 1, 2, 3, 4)
    // expected-error@-1:7 {{missing argument for parameter #1 in call}}
  }

  do {
    func f(_ aa: Int, bb: Int = 81, _ cc: Int...) {}

    f(0, 2, 3) // ok
  }

  do {
    // expected-note@+1 *{{'f(aa:_:_:)' declared here}}
    func f(aa: Int, _ bb: Int, _ cc: Int) {}

    f(1)
    // expected-error@-1:7 {{missing arguments for parameters 'aa', #3 in call}}

    f(0, 1)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}

    f(1, 2, 3)
    // expected-error@-1:6 {{missing argument label 'aa:' in call}}

    f(1, aa: 2, 3)
    // expected-error@-1:10 {{argument 'aa' must precede unnamed argument #1}}

    f(1, xx: 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:17 {{extra argument in call}}

    f(aa: 1, 2, 3) // ok

    f(xx: 1, 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:17 {{extra argument in call}}

    f(xx: 1, 2, yy: 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:21 {{extra argument 'yy' in call}}

    f(xx: 1, yy: 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:21 {{extra argument in call}}

    f(1, xx: 2, yy: 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:21 {{extra argument 'yy' in call}}

    f(1, 2, 3, 4)
    // expected-error@-1:16 {{extra argument in call}}

    f(1, aa: 2, 3, 4)
    // expected-error@-1:20 {{extra argument in call}}

    f(1, aa: 2, 3, xx: 4)
    // expected-error@-1:24 {{extra argument 'xx' in call}}

    f(1, aa: 2, xx: 3, 4)
    // expected-error@-1:24 {{extra argument in call}}
  }

  do {
    // expected-note@+1 *{{'f(aa:_:_:)' declared here}}
    func f(aa: Int, _ bb: Int = 81, _ cc: Int) {}

    f(0, 1)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
  }

  do {
    // expected-note@+1 *{{'f(aa:bb:_:)' declared here}}
    func f(aa: Int, bb: Int, _ cc: Int) {}

    f(0, 2)
    // expected-error@-1:6 {{missing argument labels 'aa:bb::' in call}}
    // expected-error@-2:8 {{missing argument for parameter 'bb' in call}}

    f(0, bb: 1, 2)
    // expected-error@-1:6 {{missing argument label 'aa:' in call}}

    f(xx: 1, 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:17 {{extra argument in call}}

    f(1, xx: 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:17 {{extra argument in call}}

    f(1, 2, xx: 3)
    // expected-error@-1:8 {{missing argument for parameter 'bb' in call}}
    // expected-error@-2:17 {{extra argument 'xx' in call}}
  }

  do {
    func f(aa: Int, bb: Int, cc: Int) { }

    f(1, aa: 2, bb: 3)
    // expected-error@-1 {{incorrect argument labels in call (have '_:aa:bb:', expected 'aa:bb:cc:')}}

    f(1, bb: 2, aa: 3)
    // expected-error@-1 {{incorrect argument labels in call (have '_:bb:aa:', expected 'aa:bb:cc:')}}

    f(aa: 1, 2, bb: 3)
    // expected-error@-1 {{incorrect argument labels in call (have 'aa:_:bb:', expected 'aa:bb:cc:')}}

    f(aa: 1, bb: 2, 3)
    // expected-error@-1 {{missing argument label 'cc:' in call}}
  }

  do {
    func f(_ aa: Int, _ bb: Int = 81, cc: Int, _ dd: Int) {}

    f(0, cc: 2, 3) // ok
  }

  do {
    func f(_ aa: Int, _ bb: Int = 81, cc: Int = 82, _ dd: Int) {}

    f(0, cc: 2, 3) // ok

    f(cc: 1, 2, 3, 4)
    // expected-error@-1 {{unnamed argument #3 must precede argument 'cc'}}
  }

  do {
    func f(_ aa: Int, _ bb: Int, cc: Int, dd: Int) { }
    // expected-note@-1 6 {{'f(_:_:cc:dd:)' declared here}}

    f(1, xx: 2)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:6 {{missing arguments for parameters 'cc', 'dd' in call}}

    f(xx: 1, 2)
    // expected-error@-1:6 {{missing arguments for parameters 'cc', 'dd' in call}}
    // expected-error@-2 {{extraneous argument label 'xx:' in call}}

    f(1, xx: 2, cc: 3)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:22 {{missing argument for parameter 'dd' in call}}

    f(1, xx: 2, dd: 3)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:15 {{missing argument for parameter 'cc' in call}}

    f(xx: 1, 2, cc: 3)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:22 {{missing argument for parameter 'dd' in call}}

    f(xx: 1, 2, dd: 3)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:15 {{missing argument for parameter 'cc' in call}}

    f(1, xx: 2, cc: 3, dd: 4)
    // expected-error@-1:6 {{extraneous argument label 'xx:' in call}}

    f(xx: 1, 2, cc: 3, dd: 4)
    // expected-error@-1:6 {{extraneous argument label 'xx:' in call}}
  }

  do {
    func f(_ aa: Int, bb: Int = 82, _ cc: Int, _ dd: Int) { }

    f(1, bb: 2, 3, 4) // ok

    f(1, 2, bb: 3, 4)
    // expected-error@-1 {{argument 'bb' must precede unnamed argument #2}}
  }

  do {
    func f(aa: Int, _ bb: Int, cc: Int, _ dd: Int) { }
    // expected-note@-1 3 {{'f(aa:_:cc:_:)' declared here}}

    f(1)
    // expected-error@-1:7 {{missing arguments for parameters 'aa', 'cc', #4 in call}}

    f(1, 2)
    // expected-error@-1:6 {{missing arguments for parameters 'aa', 'cc' in call}}

    f(1, 2, 3)
    // expected-error@-1 {{missing argument labels 'aa:cc::' in call}}
    // expected-error@-2:11 {{missing argument for parameter 'cc' in call}}

    f(1, 2, 3, 4)
    // expected-error@-1:6 {{missing argument labels 'aa:cc:' in call}}

    f(1, 2, 3, 4, 5)
    // expected-error@-1:19 {{extra argument in call}}
  }

  do {
    func f(aa: Int, bb: Int, _ cc: Int, _ dd: Int) { }
    // expected-note@-1 6 {{'f(aa:bb:_:_:)' declared here}}

    f(1, xx: 2)
    // expected-error@-1:6 {{missing arguments for parameters 'aa', 'bb' in call}}
    // expected-error@-2 {{extraneous argument label 'xx:' in call}}

    f(xx: 1, 2)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:6 {{missing arguments for parameters 'aa', 'bb' in call}}

    f(bb: 1, 2, xx: 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:6 {{extraneous argument label 'xx:' in call}}

    f(bb: 1, xx: 2, 3)
    // expected-error@-1:7 {{missing argument for parameter 'aa' in call}}
    // expected-error@-2:6 {{extraneous argument label 'xx:' in call}}

    f(aa: 1, 2, xx: 3)
    // expected-error@-1:12 {{missing argument for parameter 'bb' in call}}
    // expected-error@-2:6 {{extraneous argument label 'xx:' in call}}

    f(aa: 1, xx: 2, 3)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
    // expected-error@-2:12 {{missing argument for parameter 'bb' in call}}

    f(aa: 1, bb: 2, 3, xx: 4)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}

    f(aa: 1, bb: 2, xx: 3, 4)
    // expected-error@-1 {{extraneous argument label 'xx:' in call}}
  }

  do {
    func f(_ aa: Int, bb: Int, _ cc: Int, dd: Int, _ ee: Int) { }

    f(1, bb: 2, 3, 4, dd: 5)
    // expected-error@-1:23 {{argument 'dd' must precede unnamed argument #4}}

    f(1, dd: 2, 3, 4, bb: 5)
    // expected-error@-1 {{incorrect argument labels in call (have '_:dd:_:_:bb:', expected '_:bb:_:dd:_:')}}

    f(1, bb: 2, 3, dd: 4, 5) // ok

    f(1, dd: 2, 3, bb: 4, 5)
    // expected-error@-1 {{incorrect argument labels in call (have '_:dd:_:bb:_:', expected '_:bb:_:dd:_:')}}

    f(1, 2, bb: 3, 4, dd: 5, 6)
    // expected-error@-1:30 {{extra argument in call}}

    f(1, bb: 2, 3, 4, dd: 5, 6)
    // expected-error@-1:30 {{extra argument in call}}

    f(1, dd: 2, 3, 4, bb: 5, 6)
    // expected-error@-1:30 {{extra argument in call}}
  }
}

// -------------------------------------------
// Missing arguments
// -------------------------------------------
// FIXME: Diagnostics could be improved with all missing names, or
// simply # of arguments required.
func missingargs1(x: Int, y: Int, z: Int) {} // expected-note {{'missingargs1(x:y:z:)' declared here}}

missingargs1(x: 1, y: 2) // expected-error{{missing argument for parameter 'z' in call}}

func missingargs2(x: Int, y: Int, _ z: Int) {} // expected-note {{'missingargs2(x:y:_:)' declared here}}
missingargs2(x: 1, y: 2) // expected-error{{missing argument for parameter #3 in call}}

// -------------------------------------------
// Extra arguments
// -------------------------------------------
func extraargs1(x: Int) {} // expected-note {{'extraargs1(x:)' declared here}}

extraargs1(x: 1, y: 2) // expected-error{{extra argument 'y' in call}}
extraargs1(x: 1, 2, 3) // expected-error{{extra arguments at positions #2, #3 in call}}

// -------------------------------------------
// Argument name mismatch
// -------------------------------------------

func mismatch1(thisFoo: Int = 0, bar: Int = 0, wibble: Int = 0) { } // expected-note {{'mismatch1(thisFoo:bar:wibble:)' declared here}}

mismatch1(foo: 5) // expected-error {{extra argument 'foo' in call}}
mismatch1(baz: 1, wobble: 2) // expected-error{{incorrect argument labels in call (have 'baz:wobble:', expected 'bar:wibble:')}} {{11-14=bar}} {{19-25=wibble}}
mismatch1(food: 1, zap: 2) // expected-error{{extra arguments at positions #1, #2 in call}}

// <rdar://problem/27891805> QoI: FailureDiagnosis doesn't look through 'try'
struct rdar27891805 {
  init(contentsOf: String, encoding: String) throws {}
  init(contentsOf: String, usedEncoding: inout String) throws {}
  init<T>(_ t: T) {}
}

try rdar27891805(contentsOfURL: nil, usedEncoding: nil)
// expected-error@-1 {{incorrect argument label in call (have 'contentsOfURL:usedEncoding:', expected 'contentsOf:usedEncoding:')}}
// expected-error@-2 {{'nil' is not compatible with expected argument type 'String'}}
// expected-error@-3 {{'nil' is not compatible with expected argument type 'String'}}

// -------------------------------------------
// Out of order and default
// -------------------------------------------

struct OutOfOrderAndDefault {
  func f11(alpha: Int, bravo: Int) {}
  func f12(alpha: Int = -1, bravo: Int) {}
  func f13(alpha: Int, bravo: Int = -1) {}
  func f14(alpha: Int = -1, bravo: Int = -1) {}

  func test1() {
    // typo
    f11(bravo: 0, alphax: 1) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:', expected 'alpha:bravo:')}}
    f11(bravox: 0, alpha: 1) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:', expected 'alpha:bravo:')}}
    f12(bravo: 0, alphax: 1) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:', expected 'alpha:bravo:')}}
    f12(bravox: 0, alpha: 1) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:', expected 'alpha:bravo:')}}
    f13(bravo: 0, alphax: 1) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:', expected 'alpha:bravo:')}}
    f13(bravox: 0, alpha: 1) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:', expected 'alpha:bravo:')}}
    f14(bravo: 0, alphax: 1) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:', expected 'alpha:bravo:')}}
    f14(bravox: 0, alpha: 1) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:', expected 'alpha:bravo:')}}
  }

  func f21(alpha: Int, bravo: Int, charlie: Int) {}
  func f22(alpha: Int = -1, bravo: Int, charlie: Int) {}
  func f23(alpha: Int = -1, bravo: Int = -1, charlie: Int) {}

  func test2() {
    // BAC
    f21(bravo: 0, alphax: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:', expected 'alpha:bravo:charlie:')}}
    f21(bravox: 0, alpha: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:', expected 'alpha:bravo:charlie:')}}
    f21(bravo: 0, alpha: 1, charliex: 2) // expected-error {{'alpha' must precede argument 'bravo'}}
    f22(bravo: 0, alphax: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:', expected 'alpha:bravo:charlie:')}}
    f22(bravox: 0, alpha: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:', expected 'alpha:bravo:charlie:')}}
    f22(bravo: 0, alpha: 1, charliex: 2) // expected-error {{'alpha' must precede argument 'bravo'}}
    f23(bravo: 0, alphax: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:', expected 'alpha:bravo:charlie:')}}
    f23(bravox: 0, alpha: 1, charlie: 2) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:', expected 'alpha:bravo:charlie:')}}
    f23(bravo: 0, alpha: 1, charliex: 2) // expected-error {{'alpha' must precede argument 'bravo'}}

    // BCA
    f21(bravo: 0, charlie: 1, alphax: 2) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:', expected 'alpha:bravo:charlie:')}}
    f21(bravox: 0, charlie: 1, alpha: 2) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:', expected 'alpha:bravo:charlie:')}}
    f21(bravo: 0, charliex: 1, alpha: 2) // expected-error {{'alpha' must precede argument 'bravo'}}
    f22(bravo: 0, charlie: 1, alphax: 2) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:', expected 'alpha:bravo:charlie:')}}
    f22(bravox: 0, charlie: 1, alpha: 2) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:', expected 'alpha:bravo:charlie:')}}
    f22(bravo: 0, charliex: 1, alpha: 2) // expected-error {{'alpha' must precede argument 'bravo'}}
    f23(bravo: 0, charlie: 1, alphax: 2) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:', expected 'alpha:bravo:charlie:')}}
    f23(bravox: 0, charlie: 1, alpha: 2) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:', expected 'alpha:bravo:charlie:')}}
    f23(bravo: 0, charliex: 1, alpha: 2) // expected-error {{'alpha' must precede argument 'bravo'}}

    // CAB
    f21(charlie: 0, alphax: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charlie:alphax:bravo:', expected 'alpha:bravo:charlie:')}}
    f21(charlie: 0, alpha: 1, bravox: 2) // expected-error {{incorrect argument labels in call (have 'charlie:alpha:bravox:', expected 'alpha:bravo:charlie:')}}
    f21(charliex: 0, alpha: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charliex:alpha:bravo:', expected 'alpha:bravo:charlie:')}}
    f22(charlie: 0, alphax: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charlie:alphax:bravo:', expected 'alpha:bravo:charlie:')}}
    f22(charlie: 0, alpha: 1, bravox: 2) // expected-error {{incorrect argument labels in call (have 'charlie:alpha:bravox:', expected 'alpha:bravo:charlie:')}}
    f22(charliex: 0, alpha: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charliex:alpha:bravo:', expected 'alpha:bravo:charlie:')}}
    f23(charlie: 0, alphax: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charlie:alphax:bravo:', expected 'alpha:bravo:charlie:')}}
    f23(charlie: 0, alpha: 1, bravox: 2) // expected-error {{argument 'alpha' must precede argument 'charlie'}}
    f23(charliex: 0, alpha: 1, bravo: 2) // expected-error {{incorrect argument labels in call (have 'charliex:alpha:bravo:', expected 'alpha:bravo:charlie:')}}
  }

  func f31(alpha: Int, bravo: Int, charlie: Int, delta: Int) {}
  func f32(alpha: Int = -1, bravo: Int = -1, charlie: Int, delta: Int) {}
  func f33(alpha: Int = -1, bravo: Int = -1, charlie: Int, delta: Int = -1) {}

  func test3() {
    // BACD
    f31(bravo: 0, alphax: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f31(bravox: 0, alpha: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f31(bravo: 0, alpha: 2, charliex: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f31(bravo: 0, alpha: 2, charlie: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f32(bravo: 0, alphax: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f32(bravox: 0, alpha: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f32(bravo: 0, alpha: 2, charliex: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f32(bravo: 0, alpha: 2, charlie: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f33(bravo: 0, alphax: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:alphax:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f33(bravox: 0, alpha: 2, charlie: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:alpha:charlie:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f33(bravo: 0, alpha: 2, charliex: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f33(bravo: 0, alpha: 2, charlie: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}

    // BCAD
    f31(bravo: 0, charlie: 1, alphax: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f31(bravox: 0, charlie: 1, alpha: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f31(bravo: 0, charliex: 1, alpha: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f31(bravo: 0, charlie: 1, alpha: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f32(bravo: 0, charlie: 1, alphax: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f32(bravox: 0, charlie: 1, alpha: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f32(bravo: 0, charliex: 1, alpha: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f32(bravo: 0, charlie: 1, alpha: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f33(bravo: 0, charlie: 1, alphax: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravo:charlie:alphax:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f33(bravox: 0, charlie: 1, alpha: 2, delta: 3) // expected-error {{incorrect argument labels in call (have 'bravox:charlie:alpha:delta:', expected 'alpha:bravo:charlie:delta:')}}
    f33(bravo: 0, charliex: 1, alpha: 2, delta: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
    f33(bravo: 0, charlie: 1, alpha: 2, deltax: 3) // expected-error {{argument 'alpha' must precede argument 'bravo'}}
  }
}

// -------------------------------------------
// Subscript keyword arguments
// -------------------------------------------

struct Sub1 {
  subscript (i: Int) -> Int {
    get { return i }
  }
}

var sub1 = Sub1()
var i: Int = 0
i = sub1[i]
i = sub1[i: i] // expected-error{{extraneous argument label 'i:' in subscript}} {{10-13=}}

struct Sub2 {
  subscript (d d: Double) -> Double {
    get { return d }
  }
}

var sub2 = Sub2()
var d: Double = 0.0
d = sub2[d] // expected-error{{missing argument label 'd:' in subscript}} {{10-10=d: }}
d = sub2[d: d]
d = sub2[f: d] // expected-error{{incorrect argument label in subscript (have 'f:', expected 'd:')}} {{10-11=d}}

struct Sub3 {
  subscript (a: Int..., b b: Int...) -> Int { 42 }
}

let sub3 = Sub3()
_ = sub3[1, 2, 3, b: 4, 5, 6]
_ = sub3[b: 4, 5, 6]
_ = sub3[1, 2, 3]
_ = sub3[1, c: 4] // expected-error {{incorrect argument label in subscript (have '_:c:', expected '_:b:')}}

struct Sub4 {
  subscript (a: Int..., b b: Int = 0, c: Int...) -> Int { 42 }
}

let sub4 = Sub4()
_ = sub4[1, 2, 3, b: 2, 1, 2, 3]
_ = sub4[1, 2, 3, b: 2]
_ = sub4[1, 2, 3]
_ = sub4[]

// -------------------------------------------
// Closures
// -------------------------------------------
func intToInt(_ i: Int) -> Int { return i }

func testClosures() {
  let c0 = { (x: Int, y: Int) in x + y }
  _ = c0(1, 2)

  let c1 = { x, y in intToInt(x + y) }
  _ = c1(1, 2)

  let c2 = { intToInt($0 + $1) }
  _ = c2(1, 2)
}

func acceptAutoclosure(f: @autoclosure () -> Int) { }
func produceInt() -> Int { }
acceptAutoclosure(f: produceInt) // expected-error{{add () to forward @autoclosure parameter}} {{32-32=()}}

// -------------------------------------------
// Trailing closures
// -------------------------------------------
func trailingclosure1(x: Int, f: () -> Int) {}

trailingclosure1(x: 1) { return 5 }
trailingclosure1(1) { return 5 } // expected-error{{missing argument label 'x:' in call}}{{18-18=x: }}

trailingclosure1(x: 1, { return 5 }) // expected-error{{missing argument label 'f:' in call}} {{24-24=f: }}

func trailingclosure2(x: Int, f: (() -> Int)?...) {}
trailingclosure2(x: 5) { return 5 }

func trailingclosure3(x: Int, f: (() -> Int)!) {
  var f = f
  f = nil
  _ = f
}

trailingclosure3(x: 5) { return 5 }

func trailingclosure4(f: () -> Int) {}
trailingclosure4 { 5 }

func trailingClosure5<T>(_ file: String = #file, line: UInt = #line, expression: () -> T?) { }
func trailingClosure6<T>(value: Int, expression: () -> T?) { }

trailingClosure5(file: "hello", line: 17) { // expected-error{{extraneous argument label 'file:' in call}}{{18-24=}}
  return Optional.Some(5)
  // expected-error@-1 {{enum type 'Optional<Wrapped>' has no case 'Some'; did you mean 'some'?}} {{19-23=some}}
  // expected-error@-2 {{generic parameter 'Wrapped' could not be inferred}}
  // expected-note@-3 {{explicitly specify the generic arguments to fix this issue}}
}
trailingClosure6(5) { // expected-error{{missing argument label 'value:' in call}}{{18-18=value: }}
  return Optional.Some(5)
  // expected-error@-1 {{enum type 'Optional<Wrapped>' has no case 'Some'; did you mean 'some'?}} {{19-23=some}}
  // expected-error@-2 {{generic parameter 'Wrapped' could not be inferred}}
  // expected-note@-3 {{explicitly specify the generic arguments to fix this issue}}
}

class MismatchOverloaded1 {
  func method1(_ x: Int!, arg: ((Int) -> Int)!) { }
  func method1(_ x: Int!, secondArg: ((Int) -> Int)!) { }

  @available(*, unavailable)
  func method2(_ x: Int!, arg: ((Int) -> Int)!) { }

  func method2(_ x: Int!, secondArg: ((Int) -> Int)!) { }
}

var mismatchOverloaded1 = MismatchOverloaded1()
mismatchOverloaded1.method1(5, arg: nil)
mismatchOverloaded1.method1(5, secondArg: nil)

// Prefer available to unavailable declaration, if it comes up.
mismatchOverloaded1.method2(5) { $0 }

struct RelabelAndTrailingClosure {
  func f1(aa: Int, bb: Int, cc: () -> Void = {}) {}
  func f2(aa: Int, bb: Int, _ cc: () -> Void = {}) {}

  func test() {
    f1(aax: 1, bbx: 2) {} // expected-error {{incorrect argument labels in call (have 'aax:bbx:_:', expected 'aa:bb:_:')}} {{8-11=aa}} {{16-19=bb}} {{none}}
    f2(aax: 1, bbx: 2) {} // expected-error {{incorrect argument labels in call (have 'aax:bbx:_:', expected 'aa:bb:_:')}} {{8-11=aa}} {{16-19=bb}} {{none}}

    f1(aax: 1, bbx: 2) // expected-error {{incorrect argument labels in call (have 'aax:bbx:', expected 'aa:bb:')}} {{8-11=aa}} {{16-19=bb}} {{none}}
    f2(aax: 1, bbx: 2) // expected-error {{incorrect argument labels in call (have 'aax:bbx:', expected 'aa:bb:')}} {{8-11=aa}} {{16-19=bb}} {{none}}
  }
}

// -------------------------------------------
// Values of function type
// -------------------------------------------
func testValuesOfFunctionType(_ f1: (_: Int, _ arg: Int) -> () ) {
  f1(3, arg: 5) // expected-error{{extraneous argument label 'arg:' in call}}{{9-14=}}
  f1(x: 3, 5) // expected-error{{extraneous argument label 'x:' in call}} {{6-9=}} 
  f1(3, 5)
}


// -------------------------------------------
// Literals
// -------------------------------------------
func string_literals1(x: String) { }
string_literals1(x: "hello")

func int_literals1(x: Int) { }
int_literals1(x: 1)

func float_literals1(x: Double) { }
float_literals1(x: 5)

// -------------------------------------------
// Tuples as arguments
// -------------------------------------------
func produceTuple1() -> (Int, Bool) { return (1, true) }

func acceptTuple1<T>(_ x: (T, Bool)) { }

acceptTuple1(produceTuple1())
acceptTuple1((1, false))
acceptTuple1(1, false) // expected-error {{global function 'acceptTuple1' expects a single parameter of type '(T, Bool)' [with T = Int]}} {{14-14=(}} {{22-22=)}}

func acceptTuple2<T>(_ input : T) -> T { return input }
var tuple1 = (1, "hello")
_ = acceptTuple2(tuple1)
_ = acceptTuple2((1, "hello", 3.14159))


func generic_and_missing_label(x: Int) {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(x:)')}}
func generic_and_missing_label<T>(x: T) {} // expected-note {{incorrect labels for candidate (have: '(_:)', expected: '(x:)')}}

generic_and_missing_label(42)
// expected-error@-1 {{no exact matches in call to global function 'generic_and_missing_label'}}

// -------------------------------------------
// Curried functions
// -------------------------------------------
func f7(_ a: Int) -> (_ b: Int) -> Int {
  return { b in a+b }
}

_ = f7(1)(1)
f7(1.0)(2)       // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
f7(1)(1.0)       // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
f7(1)(b: 1.0)    // expected-error{{extraneous argument label 'b:' in call}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
let f10 = f7(2)
_ = f10(1)
f10(10)          // expected-warning {{result of call to function returning 'Int' is unused}}
f10(1.0)         // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
f10(b: 1.0)         // expected-error {{extraneous argument label 'b:' in call}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}

class CurriedClass {
  func method1() {}
  func method2(_ a: Int) -> (_ b : Int) -> () { return { b in () } }
  func method3(_ a: Int, b : Int) {}  // expected-note 3 {{'method3(_:b:)' declared here}}
}

let c = CurriedClass()
_ = c.method1
c.method1(1)         // expected-error {{argument passed to call that takes no arguments}}
_ = c.method2(1)
_ = c.method2(1.0)   // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
c.method2(1)(2)
c.method2(1)(c: 2)   // expected-error {{extraneous argument label 'c:' in call}}
c.method2(1)(c: 2.0) // expected-error {{extraneous argument label 'c:' in call}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
c.method2(1)(2.0) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
c.method2(1.0)(2) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
c.method2(1.0)(2.0) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
CurriedClass.method1(c)()
_ = CurriedClass.method1(c)
CurriedClass.method1(c)(1)         // expected-error {{argument passed to call that takes no arguments}}
CurriedClass.method1(2.0)(1)       // expected-error {{cannot convert value of type 'Double' to expected argument type 'CurriedClass'}}
// expected-error@-1:27 {{argument passed to call that takes no arguments}}
CurriedClass.method2(c)(32)(b: 1) // expected-error{{extraneous argument label 'b:' in call}}
_ = CurriedClass.method2(c)
_ = CurriedClass.method2(c)(32)
_ = CurriedClass.method2(1,2)      // expected-error {{extra argument in call}}
// expected-error@-1 {{instance member 'method2' of type 'CurriedClass' cannot be used in static context; did you mean to use a value of this type instead?}}
CurriedClass.method2(c)(1.0)(b: 1) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
// expected-error@-1 {{extraneous argument label 'b:' in call}}
CurriedClass.method2(c)(1)(1.0) // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
CurriedClass.method2(c)(2)(c: 1.0) // expected-error {{extraneous argument label 'c:'}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
CurriedClass.method3(c)(32, b: 1)
_ = CurriedClass.method3(c)
_ = CurriedClass.method3(c)(1, 2)        // expected-error {{missing argument label 'b:' in call}} {{32-32=b: }}
_ = CurriedClass.method3(c)(1, b: 2)(32) // expected-error {{cannot call value of non-function type '()'}}
_ = CurriedClass.method3(1, 2)           // expected-error {{instance member 'method3' of type 'CurriedClass' cannot be used in static context; did you mean to use a value of this type instead?}}
// expected-error@-1 {{missing argument label 'b:' in call}}
CurriedClass.method3(c)(1.0, b: 1)       // expected-error {{cannot convert value of type 'Double' to expected argument type 'Int'}}
CurriedClass.method3(c)(1)               // expected-error {{missing argument for parameter 'b' in call}}
CurriedClass.method3(c)(c: 1.0)          // expected-error {{incorrect argument label in call (have 'c:', expected 'b:')}}
// expected-error@-1 {{cannot convert value of type 'Double' to expected argument type 'Int'}}
// expected-error@-2 {{missing argument for parameter #1 in call}}

extension CurriedClass {
  func f() {
    method3(1, b: 2)
    method3()            // expected-error {{missing arguments for parameters #1, 'b' in call}} {{13-13=<#Int#>, b: <#Int#>}}
    method3(42)          // expected-error {{missing argument for parameter 'b' in call}}
    method3(self)
    // expected-error@-1:13 {{cannot convert value of type 'CurriedClass' to expected argument type 'Int'}}
    // expected-error@-2:17 {{missing argument for parameter 'b' in call}} {{17-17=, b: <#Int#>}}
  }
}

extension CurriedClass {
  func m1(_ a : Int, b : Int) {}

  func m2(_ a : Int) {}
}

// <rdar://problem/23718816> QoI: "Extra argument" error when accidentally currying a method
CurriedClass.m1(2, b: 42)   // expected-error {{instance member 'm1' of type 'CurriedClass' cannot be used in static context; did you mean to use a value of this type instead?}}

// <rdar://problem/22108559> QoI: Confusing error message when calling an instance method as a class method
CurriedClass.m2(12)  // expected-error {{instance member 'm2' of type 'CurriedClass' cannot be used in static context; did you mean to use a value of this type instead?}}
// -------------------------------------------
// Multiple label errors
// -------------------------------------------
func testLabelErrorsBasic() {
  func f(_ aa: Int, _ bb: Int, cc: Int, dd: Int, ee: Int, ff: Int) {}

  // 1 wrong
  f(0, 1, ccx: 2, dd: 3, ee: 4, ff: 5)
  // expected-error@-1 {{incorrect argument label in call (have '_:_:ccx:dd:ee:ff:', expected '_:_:cc:dd:ee:ff:')}} {{11-14=cc}} {{none}}
  // 1 missing
  f(0, 1, 2, dd: 3, ee: 4, ff: 5)
  // expected-error@-1 {{missing argument label 'cc:' in call}} {{11-11=cc: }} {{none}}
  // 1 extra
  f(aa: 0, 1, cc: 2, dd: 3, ee: 4, ff: 5)
  // expected-error@-1 {{extraneous argument label 'aa:' in call}} {{5-9=}} {{none}}
  // 1 ooo
  f(0, 1, dd: 3, cc: 2, ee: 4, ff: 5)
  // expected-error@-1 {{argument 'cc' must precede argument 'dd'}} {{16-23=}} {{11-11=cc: 2, }} {{none}}
  // 2 wrong
  f(0, 1, ccx: 2, ddx: 3, ee: 4, ff: 5)
  // expected-error@-1 {{incorrect argument labels in call (have '_:_:ccx:ddx:ee:ff:', expected '_:_:cc:dd:ee:ff:')}} {{11-14=cc}} {{19-22=dd}} {{none}}
  // 2 missing
  f(0, 1, 2, 3, ee: 4, ff: 5)
  // expected-error@-1 {{missing argument labels 'cc:dd:' in call}} {{11-11=cc: }} {{14-14=dd: }} {{none}}
  // 2 extra
  f(aa: 0, bb: 1, cc: 2, dd: 3, ee: 4, ff: 5)
  // expected-error@-1 {{extraneous argument labels 'aa:bb:' in call}} {{5-9=}} {{12-16=}} {{none}}
  // 2 ooo
  f(0, 1, dd: 3, cc: 2, ff: 5, ee: 4)
  // expected-error@-1 {{argument 'cc' must precede argument 'dd'}} {{16-23=}} {{11-11=cc: 2, }} {{none}}
  // 1 wrong + 1 missing
  f(0, 1, ccx: 2, 3, ee: 4, ff: 5)
  // expected-error@-1 {{incorrect argument labels in call (have '_:_:ccx:_:ee:ff:', expected '_:_:cc:dd:ee:ff:')}} {{11-14=cc}} {{19-19=dd: }} {{none}}
  // 1 wrong + 1 extra
  f(aa: 0, 1, ccx: 2, dd: 3, ee: 4, ff: 5)
  // expected-error@-1 {{incorrect argument labels in call (have 'aa:_:ccx:dd:ee:ff:', expected '_:_:cc:dd:ee:ff:')}} {{5-9=}} {{15-18=cc}} {{none}}
  // 1 wrong + 1 ooo
  f(0, 1, ccx: 2, dd: 3, ff: 5, ee: 4)
  // expected-error@-1 {{incorrect argument labels in call (have '_:_:ccx:dd:ff:ee:', expected '_:_:cc:dd:ee:ff:')}} {{11-14=cc}} {{26-28=ee}} {{33-35=ff}} {{none}}
}

struct DiagnoseAllLabels {
  func f(aa: Int, bb: Int, cc: Int..., dd: Int, ee: Int = 0, ff: Int = 0) {}

  func test() {
    f(aax: 0, bbx: 1, cc: 21, 22, 23, dd: 3, ff: 5) // expected-error {{incorrect argument labels in call (have 'aax:bbx:cc:_:_:dd:ff:', expected 'aa:bb:cc:_:_:dd:ff:')}} {{7-10=aa}} {{15-18=bb}} {{none}}
    f(aax: 0, bbx: 1, dd: 3, ff: 5) // expected-error {{incorrect argument labels in call (have 'aax:bbx:dd:ff:', expected 'aa:bb:dd:ff:')}} {{7-10=aa}} {{15-18=bb}} {{none}}
  }
}

/// https://github.com/apple/swift/issues/55581
/// Type inference regression in Swift 5.3 - can't infer a type of
/// `@autoclosure` result
do {
  struct Foo {
    var bar: [Int] = []
  }

  let baz: Int? = nil

  func foo<T: Equatable>(
    _ a: @autoclosure () throws -> T,
    _ b: @autoclosure () throws -> T
  ) {}

  foo(Foo().bar, [baz])
}

// https://github.com/apple/swift/issues/55681
do {
  func twoargs(_ x: String, _ y: String) {}

  let x = 1
  twoargs(x, x)
  // expected-error@-1 2 {{cannot convert value of type 'Int' to expected argument type 'String'}}
}

infix operator ---

func --- (_ lhs: String, _ rhs: String) -> Bool { true }

let x = 1
x --- x // expected-error 2 {{cannot convert value of type 'Int' to expected argument type 'String'}}

// rdar://problem/70764991 - out-of-order diagnostic crashes the compiler
func rdar70764991() {
  struct S {
    static var foo: S { get { S() } }
  }

  func bar(_: Any, _: String) {
  }

  func test(_ str: String) {
    bar(str, S.foo) // expected-error {{unnamed argument #1 must precede unnamed argument #2}}  {{9-12=}} {{14-14=str}}
  }
}

func testExtraTrailingClosure() {
  func foo() {}
  foo() {} // expected-error@:9 {{extra trailing closure passed in call}}
  foo {} // expected-error@:7 {{extra trailing closure passed in call}}
  foo {} x: {} // expected-error@:7 {{argument passed to call that takes no arguments}}

  func bar(_ x: Int) {} // expected-note 2{{'bar' declared here}}
  bar(5) {} // expected-error@:10 {{extra trailing closure passed in call}}
  bar(0) {} x: {} // expected-error@:6 {{extra trailing closures at positions #2, #3 in call}}
  bar(5, "") {} // expected-error@:6 {{extra arguments at positions #2, #3 in call}}

  func baz(_ fn: () -> Void) {} // expected-note {{'baz' declared here}}
  baz {} x: {} // expected-error@:13 {{extra trailing closure passed in call}}
  baz({}) {} // expected-error@:11 {{extra trailing closure passed in call}}
  baz({}) {} y: {} // expected-error@:6 {{extra trailing closures at positions #2, #3 in call}}

  func qux(x: () -> Void, y: () -> Void, z: () -> Void) {} // expected-note {{'qux(x:y:z:)' declared here}}
  qux() {} m: {} y: {} n: {} z: {} o: {} // expected-error@:6 {{extra trailing closures at positions #2, #4, #6 in call}}
}

// rdar://93922410 - argument-to-parameter doesn't handle applies of ParamDecls
func rdar93922410(_ completion: (Int?) -> Void) { // expected-note {{'completion' declared here}}
  _ = {
    return completion() // expected-error {{missing argument for parameter #1 in call}}
  }
}

// https://github.com/apple/swift/issues/43509
do {
  func myAssertionFailure(
    _ message: @autoclosure () -> String = String(),
    file: StaticString = #file, line: UInt = #line
  ) {}

  let x: Int?
  myAssertionFailure(x != nil, "error")
  // expected-error@-1 {{cannot convert value of type 'Bool' to expected argument type 'String'}}
  // expected-error@-2 {{missing argument label 'file:' in call}}
}

// https://github.com/apple/swift/issues/60436
func test_extraneous_argument_with_inout() {
  func test(_: Int) {}

  var x: Int = 0
  test(42, &x) // expected-error {{extra argument in call}}
}
