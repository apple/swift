// RUN: %target-typecheck-verify-swift

struct NC: ~Copyable {}

func testBorrowing(_ v: borrowing NC?) {}
func testConsuming(_ v: consuming NC?) {}
func testInout(_ v: inout NC?) {}

class MethodSir {
  func borrow(_ v: borrowing NC?) {}
  func consume(_ v: consuming NC?) {}
}

func testExplicitCasts() {
  let nc = NC()
  _ = nc as NC?
}

func testCalls() {
  let method = MethodSir()
  let foo = NC()
  testBorrowing(foo) // expected-error {{implicit conversion to 'NC?' is consuming}}
                     // expected-note@-1 {{add 'consume' to make consumption explicit}} {{17-17=consume }}
  testBorrowing(consume foo)
  testBorrowing(foo as NC?)

  method.borrow(foo) // expected-error {{implicit conversion to 'NC?' is consuming}}
                     // expected-note@-1 {{add 'consume' to make consumption explicit}} {{17-17=consume }}
  method.borrow(consume foo)

  testConsuming(foo)
  testConsuming(consume foo)

  var optNC: NC? = NC() // ConstraintLocator::ContextualType
  testInout(&optNC)
}

func testReturn() -> NC? {
    let foo = NC()
    return foo // ConstraintLocator::ContextualType
}

func higherOrder(_ f: () -> NC?) -> NC? {
  if let nc = f() {
    nc // ConstraintLocator::ContextualType
  } else {
    nil
  }
}
func callHigherOrder() {
  let nc = NC()
  let _ = higherOrder { nc } // ConstraintLocator::ClosureBody

  let _ = higherOrder { return nc } // ConstraintLocator::ClosureBody
}


func delay(_ f: @autoclosure () -> NC?) -> NC? { f() }

func testDelay() {
  let nc = NC()
  let _ = delay(nc) // expected-error {{implicit conversion to 'NC?' is consuming}}
                    // expected-note@-1 {{add 'consume' to make consumption explicit}} {{17-17=consume }}
}

struct PropCity {
  var harmless1: NC? { NC() }
  var harmless2: NC? {
    get { return NC() }
  }

  subscript(_ i: Int) -> NC? { return NC() }

  func chk(_ t: borrowing NC!) {}
  func chkWithDefaultArg(_ oath: borrowing NC? = NC()) {}
  func test(_ nc: consuming NC) {
    chk(nc) // expected-error {{implicit conversion to 'NC?' is consuming}}
            // expected-note@-1 {{add 'consume' to make consumption explicit}} {{9-9=consume }}

    chk(consume nc)

    chkWithDefaultArg()
    chkWithDefaultArg(nc) // expected-error {{implicit conversion to 'NC?' is consuming}}
                          // expected-note@-1 {{add 'consume' to make consumption explicit}} {{23-23=consume }}
  }
}

protocol Veggie: ~Copyable {}
struct Carrot: ~Copyable, Veggie {}

func restockBorrow(_ x: borrowing any Veggie & ~Copyable) {}
func restockConsume(_ x: consuming any Veggie & ~Copyable) {}

func checkExistential() {
  let carrot = Carrot()
  restockBorrow(carrot) // expected-error {{implicit conversion to 'any Veggie & ~Copyable' is consuming}}
                        // expected-note@-1 {{add 'consume' to make consumption explicit}} {{17-17=consume }}
  restockBorrow(consume carrot)

  restockConsume(carrot)
}

func genericErasure<T: Veggie & ~Copyable>(_ veg: consuming T) {
  restockBorrow(veg) // expected-error {{implicit conversion to 'any Veggie & ~Copyable' is consuming}}
                     // expected-note@-1 {{add 'consume' to make consumption explicit}} {{17-17=consume }}
  restockBorrow(consume veg)
  restockBorrow(veg as any Veggie & ~Copyable)
  restockConsume(veg)

  let _ = veg as any Veggie & ~Copyable
}

extension Veggie where Self: ~Copyable {
  func inspect(_ b: borrowing any Veggie & ~Copyable) {}
}
extension Carrot {
  consuming func check() {
    inspect(self) // expected-error {{implicit conversion to 'any Veggie & ~Copyable' is consuming}}
                  // expected-note@-1 {{add 'consume' to make consumption explicit}} {{13-13=consume }}
    inspect(consume self)
    inspect(self as any Veggie & ~Copyable)

    let _: any Veggie & ~Copyable = self
  }
}
