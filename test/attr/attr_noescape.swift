// RUN: %target-typecheck-verify-swift

@noescape var fn : () -> Int = { 4 }  // expected-error {{attribute can only be applied to types, not declarations}}

func conflictingAttrs(_ fn: @noescape @escaping () -> Int) {} // expected-error {{@escaping conflicts with @noescape}}

func doesEscape(_ fn : @escaping () -> Int) {}

func takesGenericClosure<T>(_ a : Int, _ fn : @noescape () -> T) {}

func x_or_y(_ b : Bool, _ x : ()->(), _ y : ()->()) {
  (b ? x : y)()
}

func x_or_y2(_ b : Bool, _ x : ()->(), _ y : ()->()) {
  let c = b ? x : y
  c()
}

func x_or_y3(_ b : Bool, _ x : ()->(), _ y : ()->()) {
  let c : @noescape () -> () = b ? x : y
  c()
}

func no_noescape_retval() -> @noescape () -> () {}
// expected-error@-1{{@noescape attribute may only be used in local function contexts}}

var globalAny: Any = 0

func assignToGlobal<T>(_ t: T) {
  globalAny = t
}

func takesArray(_ fns: [() -> Int]) {
  doesEscape(fns[0]) // Okay - array-of-function parameters are escaping
}

func takesVariadic(_ fns: () -> Int...) {
  doesEscape(fns[0]) // Okay - variadic-of-function parameters are escaping
}

func takesNoEscapeClosure(_ fn : () -> Int) {
  // expected-note@-1{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-2{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-3{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-4{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-5{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-6{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  // expected-note@-7{{parameter 'fn' is implicitly non-escaping}} {{34-34=@escaping }}
  takesNoEscapeClosure { 4 }  // ok

  _ = fn()  // ok

  var x = fn // okay

  // This is ok, because the closure itself is noescape.
  takesNoEscapeClosure { fn() }

  // This is not ok, because it escapes the 'fn' closure.
  doesEscape { fn() }   // expected-error {{closure use of non-escaping parameter 'fn' may allow it to escape}}

  // This is not ok, because it escapes the 'fn' closure.
  func nested_function() {
    _ = fn()   // expected-error {{declaration closing over non-escaping parameter 'fn' may allow it to escape}}
  }

  takesNoEscapeClosure(fn)  // ok

  doesEscape(fn)                   // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  takesGenericClosure(4, fn)       // ok
  takesGenericClosure(4) { fn() }  // ok.

  _ = [fn] // expected-error {{converting non-escaping value to 'Element' may allow it to escape}}
  _ = [doesEscape(fn)] // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = [1 : fn] // expected-error {{converting non-escaping value to 'Value' may allow it to escape}}
  _ = [1 : doesEscape(fn)] // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = "\(doesEscape(fn))" // expected-error {{passing non-escaping parameter 'fn' to function expecting an @escaping closure}}
  _ = "\(takesArray([fn]))" // expected-error {{using non-escaping parameter 'fn' in a context expecting an @escaping closure}}

  assignToGlobal(fn) // expected-error {{converting non-escaping value to 'T' may allow it to escape}}
}

class SomeClass {
  final var x = 42

  func test() {
    // This should require "self."
    doesEscape { x }  // expected-error {{reference to property 'x' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}

    // Since 'takesNoEscapeClosure' doesn't escape its closure, it doesn't
    // require "self." qualification of member references.
    takesNoEscapeClosure { x }
  }

  @discardableResult
  func foo() -> Int {
    foo()

    func plain() { foo() }
    let plain2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}

    func multi() -> Int { foo(); return 0 }
    let mulit2: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{31-31=self.}}

    doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}
    takesNoEscapeClosure { foo() } // okay

    doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{18-18=self.}}
    takesNoEscapeClosure { foo(); return 0 } // okay

    func outer() {
      func inner() { foo() }
      let inner2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      func multi() -> Int { foo(); return 0 }
      let _: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{28-28=self.}}
      doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }
      doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }
    }

    let outer2: () -> Void = {
      func inner() { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      let inner2 = { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      func multi() -> Int { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{29-29=self.}}
      let mulit2: () -> Int = { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      doesEscape { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      doesEscape { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
    }

    doesEscape {
      func inner() { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      let inner2 = { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      func multi() -> Int { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{29-29=self.}}
      let mulit2: () -> Int = { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      doesEscape { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      doesEscape { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}
      return 0
    }
    takesNoEscapeClosure {
      func inner() { foo() }
      let inner2 = { foo() }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
      func multi() -> Int { foo(); return 0 }
      let mulit2: () -> Int = { foo(); return 0 }  // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{33-33=self.}}
      doesEscape { foo() } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo() } // okay
      doesEscape { foo(); return 0 } // expected-error {{call to method 'foo' in closure requires explicit 'self.' to make capture semantics explicit}} {{20-20=self.}}
      takesNoEscapeClosure { foo(); return 0 } // okay
      return 0
    }

    struct Outer {
      @discardableResult
      func bar() -> Int {
        bar()

        func plain() { bar() }
        let plain2 = { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}

        func multi() -> Int { bar(); return 0 }
        let _: () -> Int = { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{30-30=self.}}

        doesEscape { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
        takesNoEscapeClosure { bar() } // okay

        doesEscape { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{22-22=self.}}
        takesNoEscapeClosure { bar(); return 0 } // okay

        return 0
      }
    }

    func structOuter() {
      struct Inner {
        @discardableResult
        func bar() -> Int {
          bar() // no-warning

          func plain() { bar() }
          let plain2 = { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{26-26=self.}}

          func multi() -> Int { bar(); return 0 }
          let _: () -> Int = { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{32-32=self.}}

          doesEscape { bar() } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}
          takesNoEscapeClosure { bar() } // okay

          doesEscape { bar(); return 0 } // expected-error {{call to method 'bar' in closure requires explicit 'self.' to make capture semantics explicit}} {{24-24=self.}}
          takesNoEscapeClosure { bar(); return 0 } // okay

          return 0
        }
      }
    }

    return 0
  }
}


// Implicit conversions (in this case to @convention(block)) are ok.
@_silgen_name("whatever") 
func takeNoEscapeAsObjCBlock(_: @noescape @convention(block) () -> Void)
func takeNoEscapeTest2(_ fn : @noescape () -> ()) {
  takeNoEscapeAsObjCBlock(fn)
}

// Autoclosure implies noescape, but produce nice diagnostics so people know
// why noescape problems happen.
func testAutoclosure(_ a : @autoclosure () -> Int) { // expected-note{{parameter 'a' is implicitly non-escaping}}
  doesEscape { a() }  // expected-error {{closure use of non-escaping parameter 'a' may allow it to escape}}
}


// <rdar://problem/19470858> QoI: @autoclosure implies @noescape, so you shouldn't be allowed to specify both
func redundant(_ fn : @noescape  // expected-error @+1 {{@noescape is implied by @autoclosure and should not be redundantly specified}}
               @autoclosure () -> Int) {
}


protocol P1 {
  associatedtype Element
}
protocol P2 : P1 {
  associatedtype Element
}

func overloadedEach<O: P1, T>(_ source: O, _ transform: @escaping (O.Element) -> (), _: T) {}

func overloadedEach<P: P2, T>(_ source: P, _ transform: @escaping (P.Element) -> (), _: T) {}

struct S : P2 {
  typealias Element = Int
  func each(_ transform: @noescape (Int) -> ()) {
    overloadedEach(self,  // expected-error {{cannot invoke 'overloadedEach' with an argument list of type '(S, (Int) -> (), Int)'}}
                   transform, 1)
    // expected-note @-2 {{overloads for 'overloadedEach' exist with these partially matching parameter lists: (O, @escaping (O.Element) -> (), T), (P, @escaping (P.Element) -> (), T)}}
  }
}



// rdar://19763676 - False positive in @noescape analysis triggered by parameter label
func r19763676Callee(_ f: @noescape (_ param: Int) -> Int) {}

func r19763676Caller(_ g: @noescape (Int) -> Int) {
  r19763676Callee({ _ in g(1) })
}


// <rdar://problem/19763732> False positive in @noescape analysis triggered by default arguments
func calleeWithDefaultParameters(_ f: @noescape () -> (), x : Int = 1) {}
func callerOfDefaultParams(_ g: @noescape () -> ()) {
  calleeWithDefaultParameters(g)
}



// <rdar://problem/19773562> Closures executed immediately { like this }() are not automatically @noescape
class NoEscapeImmediatelyApplied {
  func f() {
    // Shouldn't require "self.", the closure is obviously @noescape.
    _ = { return ivar }()
  }
  
  final var ivar  = 42
}



// Reduced example from XCTest overlay, involves a TupleShuffleExpr
public func XCTAssertTrue(_ expression: @autoclosure () -> Bool, _ message: String = "", file: StaticString = #file, line: UInt = #line) -> Void {
}
public func XCTAssert(_ expression: @autoclosure () -> Bool, _ message: String = "", file: StaticString = #file, line: UInt = #line)  -> Void {
  XCTAssertTrue(expression, message, file: file, line: line);
}



/// SR-770 - Currying and `noescape`/`rethrows` don't work together anymore
func curriedFlatMap<A, B>(_ x: [A]) -> (@noescape (A) -> [B]) -> [B] {
  return { f in
    x.flatMap(f)
  }
}

func curriedFlatMap2<A, B>(_ x: [A]) -> (@noescape (A) -> [B]) -> [B] {
  return { (f : @noescape (A) -> [B]) in
    x.flatMap(f)
  }
}

func bad(_ a : @escaping (Int)-> Int) -> Int { return 42 }
func escapeNoEscapeResult(_ x: [Int]) -> (@noescape (Int) -> Int) -> Int {
  return { f in // expected-note{{parameter 'f' is implicitly non-escaping}}
    bad(f)  // expected-error {{passing non-escaping parameter 'f' to function expecting an @escaping closure}}
  }
}


// SR-824 - @noescape for Type Aliased Closures
//

typealias CompletionHandlerNE = @noescape (_ success: Bool) -> ()
// expected-error@-1{{@noescape attribute may only be used in local function contexts}}

// Explicit @escaping is not allowed here
typealias CompletionHandlerE = @escaping (_ success: Bool) -> () // expected-error{{@escaping attribute may only be used in function parameter position}} {{32-42=}}

// No @escaping -- it's implicit from context
typealias CompletionHandler = (_ success: Bool) -> ()

var escape : CompletionHandler
var escapeOther : CompletionHandler
func doThing1(_ completion: (_ success: Bool) -> ()) {
  // expected-note@-1{{parameter 'completion' is implicitly non-escaping}}
  escape = completion
  // expected-error@-1{{assigning non-escaping parameter 'completion' to an @escaping closure}}
}
func doThing3(_ completion: CompletionHandler) {
  // expected-note@-1{{parameter 'completion' is implicitly non-escaping}}
  escape = completion
  // expected-error@-1{{assigning non-escaping parameter 'completion' to an @escaping closure}}
}
func doThing4(_ completion: @escaping CompletionHandler) {
  escapeOther = completion
}

// <rdar://problem/19997680> @noescape doesn't work on parameters of function type
func apply<T, U>(_ f: @noescape (T) -> U, g: @noescape (@noescape (T) -> U) -> U) -> U { 
  return g(f)
  // expected-error@-1{{passing a non-escaping function parameter 'f' to a call to a non-escaping function parameter}}
}

// <rdar://problem/19997577> @noescape cannot be applied to locals, leading to duplication of code
enum r19997577Type {
  case Unit
  case Function(() -> r19997577Type, () -> r19997577Type)
  case Sum(() -> r19997577Type, () -> r19997577Type)

  func reduce<Result>(_ initial: Result, _ combine: @noescape (Result, r19997577Type) -> Result) -> Result {
    let binary: @noescape (r19997577Type, r19997577Type) -> Result = { combine(combine(combine(initial, self), $0), $1) }
    switch self {
    case .Unit:
      return combine(initial, self)
    case let .Function(t1, t2):
      return binary(t1(), t2())
    case let .Sum(t1, t2):
      return binary(t1(), t2())
    }
  }
}

// type attribute and decl attribute
func noescapeD(@noescape f: @escaping () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
func noescapeT(f: @noescape () -> Bool) {}
func noescapeG<T>(@noescape f: () -> T) {} // expected-error{{attribute can only be applied to types, not declarations}}

func autoclosureD(@autoclosure f: () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
func autoclosureT(f: @autoclosure () -> Bool) {}  // ok
func autoclosureG<T>(@autoclosure f: () -> T) {} // expected-error{{attribute can only be applied to types, not declarations}}

func noescapeD_noescapeT(@noescape f: @noescape () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}

func autoclosureD_noescapeT(@autoclosure f: @noescape () -> Bool) {} // expected-error {{attribute can only be applied to types, not declarations}}
