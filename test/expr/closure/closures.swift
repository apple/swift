// RUN: %target-typecheck-verify-swift -disable-availability-checking

var func6 : (_ fn : (Int,Int) -> Int) -> ()
var func6a : ((Int, Int) -> Int) -> ()
var func6b : (Int, (Int, Int) -> Int) -> ()
func func6c(_ f: (Int, Int) -> Int, _ n: Int = 0) {}


// Expressions can be auto-closurified, so that they can be evaluated separately
// from their definition.
var closure1 : () -> Int = {4}  // Function producing 4 whenever it is called.
var closure2 : (Int,Int) -> Int = { 4 } // expected-error{{contextual type for closure argument list expects 2 arguments, which cannot be implicitly ignored}} {{36-36= _,_ in}}
var closure3a : () -> () -> (Int,Int) = {{ (4, 2) }} // multi-level closing.
var closure3b : (Int,Int) -> (Int) -> (Int,Int) = {{ (4, 2) }} // expected-error{{contextual type for closure argument list expects 2 arguments, which cannot be implicitly ignored}}  {{52-52=_,_ in }}
// expected-error@-1 {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{53-53= _ in}}
var closure4 : (Int,Int) -> Int = { $0 + $1 }
var closure5 : (Double) -> Int = {
       $0 + 1.0
       // expected-error@-1 {{cannot convert value of type 'Double' to closure result type 'Int'}}
}

var closure6 = $0  // expected-error {{anonymous closure argument not contained in a closure}}

var closure7 : Int = { 4 }  // expected-error {{function produces expected type 'Int'; did you mean to call it with '()'?}} {{27-27=()}} // expected-note {{Remove '=' to make 'closure7' a computed property}}{{20-22=}}

var capturedVariable = 1
var closure8 = { [capturedVariable] in
  capturedVariable += 1 // expected-error {{left side of mutating operator isn't mutable: 'capturedVariable' is an immutable capture}}
}

func funcdecl1(_ a: Int, _ y: Int) {}
func funcdecl3() -> Int {}
func funcdecl4(_ a: ((Int) -> Int), _ b: Int) {}

func funcdecl5(_ a: Int, _ y: Int) {
  // Pass in a closure containing the call to funcdecl3.
  funcdecl4({ funcdecl3() }, 12)  // expected-error {{contextual type for closure argument list expects 1 argument, which cannot be implicitly ignored}} {{14-14= _ in}}
  
  
  func6({$0 + $1})       // Closure with two named anonymous arguments
  func6({($0) + $1})    // Closure with sequence expr inferred type
  func6({($0) + $0})    // // expected-error {{contextual closure type '(Int, Int) -> Int' expects 2 arguments, but 1 was used in closure body}}


  var testfunc : ((), Int) -> Int  // expected-note {{'testfunc' declared here}}
  testfunc({$0+1})  // expected-error {{missing argument for parameter #2 in call}}
  // expected-error@-1 {{cannot convert value of type '(Int) -> Int' to expected argument type '()'}}

  funcdecl5(1, 2) // recursion.

  // Element access from a tuple.
  var a : (Int, f : Int, Int)
  var b = a.1+a.f

  // Tuple expressions with named elements.
  var i : (y : Int, x : Int) = (x : 42, y : 11) // expected-warning {{expression shuffles the elements of this tuple; this behavior is deprecated}}
  funcdecl1(123, 444)
  
  // Calls.
  4()  // expected-error {{cannot call value of non-function type 'Int'}}{{4-6=}}
  
  
  // rdar://12017658 - Infer some argument types from func6.
  func6({ a, b -> Int in a+b})
  // Return type inference.
  func6({ a,b in a+b })
  
  // Infer incompatible type.
  func6({a,b -> Float in 4.0 })    // expected-error {{declared closure result 'Float' is incompatible with contextual type 'Int'}} {{17-22=Int}}  // Pattern doesn't need to name arguments.
  func6({ _,_ in 4 })
  
  func6({a,b in 4.0 })  // expected-error {{cannot convert value of type 'Double' to closure result type 'Int'}}
  
  // TODO: This diagnostic can be improved: rdar://22128205
  func6({(a : Float, b) in 4 }) // expected-error {{cannot convert value of type '(Float, Int) -> Int' to expected argument type '(Int, Int) -> Int'}}

  
  
  var fn = {}
  var fn2 = { 4 }
  
  
  var c : Int = { a,b -> Int in a+b} // expected-error{{cannot convert value of type '(Int, Int) -> Int' to specified type 'Int'}}
  
  
}

func unlabeledClosureArgument() {

  func add(_ x: Int, y: Int) -> Int { return x + y }
  func6a({$0 + $1}) // single closure argument
  func6a(add)
  func6b(1, {$0 + $1}) // second arg is closure
  func6b(1, add)
  func6c({$0 + $1}) // second arg is default int
  func6c(add)
}

// rdar://11935352 - closure with no body.
func closure_no_body(_ p: () -> ()) {
  return closure_no_body({})
}


// rdar://12019415
func t() {
  let u8 : UInt8 = 1
  let x : Bool = true

  if 0xA0..<0xBF ~= Int(u8) && x {
  }
}

// <rdar://problem/11927184>
func f0(_ a: Any) -> Int { return 1 }
assert(f0(1) == 1)

// TODO(diagnostics): Bad diagnostic - should be `circular reference`
var selfRef = { selfRef() }
// expected-error@-1 {{unable to infer closure type without a type annotation}}

// TODO: should be an error `circular reference` but it's diagnosed via overlapped requests
var nestedSelfRef = {
  var recursive = { nestedSelfRef() }
  // expected-warning@-1 {{variable 'recursive' was never mutated; consider changing to 'let' constant}}
  recursive()
}

var shadowed = { (shadowed: Int) -> Int in
  let x = shadowed
  return x
} // no-warning
var shadowedShort = { (shadowedShort: Int) -> Int in shadowedShort+1 } // no-warning


func anonymousClosureArgsInClosureWithArgs() {
  func f(_: String) {}
  var a1 = { () in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
  var a2 = { () -> Int in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
  var a3 = { (z: Int) in $0 } // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'z'?}} {{26-28=z}}
  var a4 = { (z: [Int], w: [Int]) in
    f($0.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'z'?}} {{7-9=z}} expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
    f($1.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'w'?}} {{7-9=w}}
  }
  var a5 = { (_: [Int], w: [Int]) in
    f($0.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
    f($1.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'w'?}} {{7-9=w}}
  }
}

func doStuff(_ fn : @escaping () -> Int) {}
func doVoidStuff(_ fn : @escaping () -> ()) {}
func doVoidStuffNonEscaping(_ fn: () -> ()) {}

// <rdar://problem/16193162> Require specifying self for locations in code where strong reference cycles are likely
class ExplicitSelfRequiredTest {
  var x = 42
  func method() -> Int {
    // explicit closure requires an explicit "self." base or an explicit capture.
    doVoidStuff({ self.x += 1 })
    doVoidStuff({ [self] in x += 1 })
    doVoidStuff({ [self = self] in x += 1 })
    doVoidStuff({ [unowned self] in x += 1 })
    doVoidStuff({ [unowned(unsafe) self] in x += 1 })
    doVoidStuff({ [unowned self = self] in x += 1 })

    doStuff({ [self] in x+1 })
    doStuff({ [self = self] in x+1 })
    doStuff({ self.x+1 })
    doStuff({ [unowned self] in x+1 })
    doStuff({ [unowned(unsafe) self] in x+1 })
    doStuff({ [unowned self = self] in x+1 })
    doStuff({ x+1 })    // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}} expected-note{{reference 'self.' explicitly}} {{15-15=self.}}
    doVoidStuff({ doStuff({ x+1 })}) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{28-28= [self] in}} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ x += 1 })    // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{19-19=self.}}
    doVoidStuff({ _ = "\(x)"}) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{26-26=self.}}
    doVoidStuff({ [y = self] in x += 1 }) // expected-warning {{capture 'y' was never used}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{20-20=self, }} expected-note{{reference 'self.' explicitly}} {{33-33=self.}}
    doStuff({ [y = self] in x+1 }) // expected-warning {{capture 'y' was never used}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ [self = ExplicitSelfRequiredTest()] in x += 1 }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{capture 'self' was never used}}
    doStuff({ [self = ExplicitSelfRequiredTest()] in x+1 }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{capture 'self' was never used}}

    // Methods follow the same rules as properties, uses of 'self' without capturing must be marked with "self."
    doStuff { method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}} expected-note{{reference 'self.' explicitly}} {{15-15=self.}}
    doVoidStuff { _ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{23-23=self.}}
    doVoidStuff { _ = "\(method())" } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{26-26=self.}}
    doVoidStuff { () -> () in _ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self]}} expected-note{{reference 'self.' explicitly}} {{35-35=self.}}
    doVoidStuff { [y = self] in _ = method() } // expected-warning {{capture 'y' was never used}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{20-20=self, }} expected-note{{reference 'self.' explicitly}} {{37-37=self.}}
    doStuff({ [y = self] in method() }) // expected-warning {{capture 'y' was never used}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ [self = ExplicitSelfRequiredTest()] in _ = method() }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{capture 'self' was never used}}
    doStuff({ [self = ExplicitSelfRequiredTest()] in method() }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{capture 'self' was never used}}
    doVoidStuff { _ = self.method() }
    doVoidStuff { [self] in _ = method() }
    doVoidStuff { [self = self] in _ = method() }
    doVoidStuff({ [unowned self] in _ = method() })
    doVoidStuff({ [unowned(unsafe) self] in _ = method() })
    doVoidStuff({ [unowned self = self] in _ = method() })

    doStuff { self.method() }
    doStuff { [self] in method() }
    doStuff({ [self = self] in method() })
    doStuff({ [unowned self] in method() })
    doStuff({ [unowned(unsafe) self] in method() })
    doStuff({ [unowned self = self] in method() })
    
    // When there's no space between the opening brace and the first expression, insert it
    doStuff {method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in }} expected-note{{reference 'self.' explicitly}} {{14-14=self.}}
    doVoidStuff {_ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in }} expected-note{{reference 'self.' explicitly}} {{22-22=self.}}
    doVoidStuff {() -> () in _ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self]}} expected-note{{reference 'self.' explicitly}} {{34-34=self.}}
    // With an empty capture list, insertion should be suggested without a comma
    doStuff { [] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self}} expected-note{{reference 'self.' explicitly}} {{21-21=self.}}
    doStuff { [  ] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self}} expected-note{{reference 'self.' explicitly}} {{23-23=self.}}
    doStuff { [ /* This space intentionally left blank. */ ] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self}} expected-note{{reference 'self.' explicitly}} {{65-65=self.}}
    // expected-note@+1 {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self}}
    doStuff { [ // Nothing in this capture list!
        ]
        in
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{9-9=self.}}
    }
    // An inserted capture list should be on the same line as the opening brace, immediately following it.
    // expected-note@+1 {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}}
    doStuff {
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{7-7=self.}}
    }
    // expected-note@+2 {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}}
    // Note: Trailing whitespace on the following line is intentional and should not be removed!
    doStuff {
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{7-7=self.}}
    }
    // expected-note@+1 {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}}
    doStuff {   // We have stuff to do.
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{7-7=self.}}
    }
    // expected-note@+1 {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}}
    doStuff {// We have stuff to do.
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{7-7=self.}}
    }

    // String interpolation should offer the diagnosis and fix-its at the expected locations
    doVoidStuff { _ = "\(method())" } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{26-26=self.}} expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}}
    doVoidStuff { _ = "\(x+1)" } // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{reference 'self.' explicitly}} {{26-26=self.}} expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}}
    
    // If we already have a capture list, self should be added to the list
    let y = 1
    doStuff { [y] in method() } // expected-warning {{capture 'y' was never used}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{22-22=self.}}
    doStuff { [ // expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }}
        y // expected-warning {{capture 'y' was never used}}
        ] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}  expected-note{{reference 'self.' explicitly}} {{14-14=self.}}

    // <rdar://problem/18877391> "self." shouldn't be required in the initializer expression in a capture list
    // This should not produce an error, "x" isn't being captured by the closure.
    doStuff({ [myX = x] in myX })

    // This should produce an error, since x is used within the inner closure.
    doStuff({ [myX = {x}] in 4 })    // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{23-23= [self] in }} expected-note{{reference 'self.' explicitly}} {{23-23=self.}}
    // expected-warning @-1 {{capture 'myX' was never used}}

    return 42
  }
  
  // The error emitted by these cases cause `VarDeclUsageChecker` to not run analysis on this method,
  // because its `sawError` flag is set to true. To preserve the "capture 'y' was never used" warnings
  // above, we put these cases in their own method.
  func weakSelfError() {
    doVoidStuff({ [weak self] in x += 1 }) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
    doVoidStuffNonEscaping({ [weak self] in x += 1 }) // expected-warning {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
    doStuff({ [weak self] in x+1 }) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
    doVoidStuff({ [weak self] in _ = method() }) // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
    doVoidStuffNonEscaping({ [weak self] in _ = method() }) // expected-warning {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
    doStuff({ [weak self] in method() }) // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-warning {{variable 'self' was written to, but never read}}
  }
}

// If the implicit self is of value type, no diagnostic should be produced.
struct ImplicitSelfAllowedInStruct {
    var x = 42
    mutating func method() -> Int {
        doStuff({ x+1 })
        doVoidStuff({ x += 1 })
        doStuff({ method() })
        doVoidStuff({ _ = method() })
    }
    
    func method2() -> Int {
        doStuff({ x+1 })
        doVoidStuff({ _ = x+1 })
        doStuff({ method2() })
        doVoidStuff({ _ = method2() })
    }
}

enum ImplicitSelfAllowedInEnum {
    case foo
    var x: Int { 42 }
    mutating func method() -> Int {
        doStuff({ x+1 })
        doVoidStuff({ _ = x+1 })
        doStuff({ method() })
        doVoidStuff({ _ = method() })
    }
    
    func method2() -> Int {
        doStuff({ x+1 })
        doVoidStuff({ _ = x+1 })
        doStuff({ method2() })
        doVoidStuff({ _ = method2() })
    }
}


class SomeClass {
  var field : SomeClass?
  var `class` : SomeClass?
  var `in`: Int = 0
  func foo() -> Int {}
}

func testCaptureBehavior(_ ptr : SomeClass) {
  // Test normal captures.
  weak var wv : SomeClass? = ptr
  unowned let uv : SomeClass = ptr
  unowned(unsafe) let uv1 : SomeClass = ptr
  unowned(safe) let uv2 : SomeClass = ptr
  doStuff { wv!.foo() }
  doStuff { uv.foo() }
  doStuff { uv1.foo() }
  doStuff { uv2.foo() }

  
  // Capture list tests
  let v1 : SomeClass? = ptr
  let v2 : SomeClass = ptr

  doStuff { [weak v1] in v1!.foo() }
  doStuff { [weak v1,                 // expected-note {{previous}}
             weak v1] in v1!.foo() }  // expected-error {{invalid redeclaration of 'v1'}}
  doStuff { [unowned v2] in v2.foo() }
  doStuff { [unowned(unsafe) v2] in v2.foo() }
  doStuff { [unowned(safe) v2] in v2.foo() }
  doStuff { [weak v1, weak v2] in v1!.foo() + v2!.foo() }

  let i = 42
  doStuff { [weak i] in i! }   // expected-error {{'weak' may only be applied to class and class-bound protocol types, not 'Int'}}
}

extension SomeClass {
  func bar() {
    doStuff { [unowned self] in self.foo() }
    doStuff { [unowned xyz = self.field!] in xyz.foo() }
    doStuff { [weak xyz = self.field] in xyz!.foo() }

    // rdar://16889886 - Assert when trying to weak capture a property of self in a lazy closure
    doStuff { [weak self.field] in field!.foo() }
    // expected-error@-1{{fields may only be captured by assigning to a specific name}}{{21-21=field = }}
    // expected-error@-2{{reference to property 'field' in closure requires explicit use of 'self' to make capture semantics explicit}}
    // expected-note@-3{{reference 'self.' explicitly}} {{36-36=self.}}
    // expected-note@-4{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }}

    doStuff { [self.field] in field!.foo() }
    // expected-error@-1{{fields may only be captured by assigning to a specific name}}{{16-16=field = }}
    // expected-error@-2{{reference to property 'field' in closure requires explicit use of 'self' to make capture semantics explicit}}
    // expected-note@-3{{reference 'self.' explicitly}} {{31-31=self.}}
    // expected-note@-4{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }}

    doStuff { [self.field!.foo()] in 32 }
    //expected-error@-1{{fields may only be captured by assigning to a specific name}}

    doStuff { [self.class] in self.class!.foo() }
    //expected-error@-1{{fields may only be captured by assigning to a specific name}}{{16-16=`class` = }}

    doStuff { [self.`in`] in `in` }
    //expected-note@-1{{capture 'self' explicitly to enable implicit 'self' in this closure}}
    //expected-error@-2{{fields may only be captured by assigning to a specific name}}{{16-16=`in` = }}
    //expected-error@-3{{reference to property 'in' in closure requires explicit use of 'self' to make capture semantics explicit}}
    //expected-note@-4{{reference 'self.' explicitly}}

    // expected-warning @+1 {{variable 'self' was written to, but never read}}
    doStuff { [weak self&field] in 42 }  // expected-error {{expected ']' at end of capture list}}

  }

  func strong_in_capture_list() {
    // <rdar://problem/18819742> QOI: "[strong self]" in capture list generates unhelpful error message
    _ = {[strong self] () -> () in return }  // expected-error {{expected 'weak', 'unowned', or no specifier in capture list}}
  }
}


// <rdar://problem/16955318> Observed variable in a closure triggers an assertion
var closureWithObservedProperty: () -> () = {
  var a: Int = 42 { // expected-warning {{variable 'a' was never used; consider replacing with '_' or removing it}}
  willSet {
    _ = "Will set a to \(newValue)"
  }
  didSet {
    _ = "Did set a with old value of \(oldValue)"
  }
  }
}

;

{}() // expected-error{{top-level statement cannot begin with a closure expression}}



// rdar://19179412 - Crash on valid code.
func rdar19179412() -> (Int) -> Int {
  return { x in
    class A {
      let d : Int = 0
    }
    return 0
  }
}

// Test coercion of single-expression closure return types to void.
func takesVoidFunc(_ f: () -> ()) {}
var i: Int = 1

// expected-warning @+1 {{expression of type 'Int' is unused}}
takesVoidFunc({i})
// expected-warning @+1 {{expression of type 'Int' is unused}}
var f1: () -> () = {i}
var x = {return $0}(1)

func returnsInt() -> Int { return 0 }
takesVoidFunc(returnsInt) // expected-error {{cannot convert value of type '() -> Int' to expected argument type '() -> ()'}}
takesVoidFunc({() -> Int in 0}) // expected-error {{declared closure result 'Int' is incompatible with contextual type '()'}} {{22-25=()}}
  
// These used to crash the compiler, but were fixed to support the implementation of rdar://problem/17228969
Void(0) // expected-error{{argument passed to call that takes no arguments}}
_ = {0}

// <rdar://problem/22086634> "multi-statement closures require an explicit return type" should be an error not a note
let samples = {
          if (i > 10) { return true }
          else { return false }
        }()

// <rdar://problem/19756953> Swift error: cannot capture '$0' before it is declared
func f(_ fp : (Bool, Bool) -> Bool) {}
f { $0 && !$1 }


// <rdar://problem/18123596> unexpected error on self. capture inside class method
func TakesIntReturnsVoid(_ fp : ((Int) -> ())) {}

struct TestStructWithStaticMethod {
  static func myClassMethod(_ count: Int) {
    // Shouldn't require "self."
    TakesIntReturnsVoid { _ in myClassMethod(0) }
  }
}

class TestClassWithStaticMethod {
  class func myClassMethod(_ count: Int) {
    // Shouldn't require "self."
    TakesIntReturnsVoid { _ in myClassMethod(0) }
  }
}

// Test that we can infer () as the result type of these closures.
func genericOne<T>(_ a: () -> T) {}
func genericTwo<T>(_ a: () -> T, _ b: () -> T) {}
genericOne {}
genericTwo({}, {})


// <rdar://problem/22344208> QoI: Warning for unused capture list variable should be customized
class r22344208 {
  func f() {
    let q = 42
    let _: () -> Int = {
      [unowned self,  // expected-warning {{capture 'self' was never used}}
       q] in       // expected-warning {{capture 'q' was never used}}
      1 }
  }
}

var f = { (s: Undeclared) -> Int in 0 } // expected-error {{cannot find type 'Undeclared' in scope}}

// <rdar://problem/21375863> Swift compiler crashes when using closure, declared to return illegal type.
func r21375863() {
  var width = 0
  var height = 0
  var bufs: [[UInt8]] = (0..<4).map { _ -> [asdf] in  // expected-error {{cannot find type 'asdf' in scope}}
    [UInt8](repeating: 0, count: width*height)
  }
}

// <rdar://problem/25993258>
//   Don't crash if we infer a closure argument to have a tuple type containing inouts.
func r25993258_helper(_ fn: (inout Int, Int) -> ()) {}
func r25993258a() {
  r25993258_helper { x in () } // expected-error {{contextual closure type '(inout Int, Int) -> ()' expects 2 arguments, but 1 was used in closure body}}
}
func r25993258b() {
  r25993258_helper { _ in () } // expected-error {{contextual closure type '(inout Int, Int) -> ()' expects 2 arguments, but 1 was used in closure body}}
}

// We have to map the captured var type into the right generic environment.
class GenericClass<T> {}

func lvalueCapture<T>(c: GenericClass<T>) {
  var cc = c
  weak var wc = c

  func innerGeneric<U>(_: U) {
    _ = cc
    _ = wc

    cc = wc!
  }
}

// Don't expose @lvalue-ness in diagnostics.
let closure = {
  var helper = true // expected-warning {{variable 'helper' was never mutated; consider changing to 'let' constant}}
  return helper
}

// https://github.com/apple/swift/issues/52253
do {
  func f(_: @escaping @convention(block) () -> Void) {}

  func id<T>(_: T) -> T {}

  let qux: () -> Void

  f(qux)
  f(id(qux)) // expected-error {{conflicting arguments to generic parameter 'T' ('() -> Void' vs. '@convention(block) () -> Void')}}

  func forceUnwrap<T>(_: T?) -> T {}

  let qux1: (() -> Void)?

  f(qux1!)
  f(forceUnwrap(qux1))
}

// rdar://problem/65155671 - crash referencing parameter of outer closure
func rdar65155671(x: Int) {
    { a in
      _ = { [a] in a }
    }(x)
}

// https://github.com/apple/swift/issues/45774
do {
  func f<T, U>(_: (@escaping (@escaping (T) -> U) -> ((T) -> U))) -> ((T) -> U) {}

  class C {
    init() {
      // expected-warning@+1{{capture 'self' was never used}}
      let _ = f { fn in { [unowned self, fn] x in x != 1000 ? fn(x + 1) : "success" } }(0)
    }
  }
}

// https://github.com/apple/swift/issues/56501
// Apply the explicit 'self' rule even if it refers to a capture, if
// we're inside a nested closure.
class C_56501 {
  func operation() {}

  func test1() {
    doVoidStuff { [self] in
      operation()
    }
  }

  func test2() {
    doVoidStuff { [self] in
      doVoidStuff {
        // expected-warning@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
        // expected-note@-2 {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        // expected-note@+1 {{reference 'self.' explicitly}}
        operation()
      }
    }
  }

  func test3() {
    doVoidStuff { [self] in
      doVoidStuff { [self] in
        operation()
      }
    }
  }

  func test4() {
    doVoidStuff { [self] in
      doVoidStuff {
        self.operation()
      }
    }
  }

  func test5() {
    doVoidStuff { [self] in
      doVoidStuffNonEscaping {
        operation()
      }
    }
  }

  func test6() {
    doVoidStuff { [self] in
      doVoidStuff { [self] in
        doVoidStuff {
          // expected-warning@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
          // expected-note@-2 {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          // expected-note@+1 {{reference 'self.' explicitly}}
          operation()
        }
      }
    }
  }
  
  func test7() {
    doVoidStuff { [self] in
      func innerFunction() {
        operation()
      }
    }
  }
  
  func test8() {
    doVoidStuffNonEscaping { [self] in
      func innerFunction() {
        operation()
      }
    }
  }
}

// https://github.com/apple/swift/issues/57029
do {
  func call<T>(_ : Int, _ f: () -> (T, Int)) -> (T, Int) {}

  func f() -> (Int, Int) {
    call(1) { // expected-error {{cannot convert return expression of type '((), Int)' to return type '(Int, Int)'}}
       ((), 0)
    }
  }

  func f_Optional() -> (Int, Int)? {
    call(1) { // expected-error {{cannot convert return expression of type '((), Int)' to return type '(Int, Int)'}}
       ((), 0)
    }
  }
}

// https://github.com/apple/swift/issues/55680

func callit<T>(_ f: () -> T) -> T {
  f()
}

func callitArgs<T>(_ : Int, _ f: () -> T) -> T {
  f()
}

func callitArgsFn<T>(_ : Int, _ f: () -> () -> T) -> T {
  f()()
}

func callitGenericArg<T>(_ a: T, _ f: () -> T) -> T {
  f()
}

func callitTuple<T>(_ : Int, _ f: () -> (T, Int)) -> T {
  f().0
}

func callitVariadic<T>(_ fs: () -> T...) -> T {
  fs.first!()
}

func test_55680_Tuple() -> Int {
  // expected-error@+2{{conflicting arguments to generic parameter 'T' ('()' vs. 'Int')}}
  // expected-note@+1:3{{generic parameter 'T' inferred as 'Int' from context}}
  callitTuple(1) { // expected-note@:18{{generic parameter 'T' inferred as '()' from closure return expression}}
    (print("hello"), 0)
  }
}

func test_55680() -> Int {
  // expected-error@+2{{conflicting arguments to generic parameter 'T' ('()' vs. 'Int')}}
  // expected-note@+1:3{{generic parameter 'T' inferred as 'Int' from context}}
  callit { // expected-note@:10{{generic parameter 'T' inferred as '()' from closure return expression}}
    print("hello")
  }
}

func test_55680_Args() -> Int {
  // expected-error@+2{{conflicting arguments to generic parameter 'T' ('()' vs. 'Int')}}
  // expected-note@+1:3{{generic parameter 'T' inferred as 'Int' from context}}
  callitArgs(1) { // expected-note@:17{{generic parameter 'T' inferred as '()' from closure return expression}}
    print("hello")
  }
}

func test_55680_ArgsFn() -> Int {
  // expected-error@+2{{conflicting arguments to generic parameter 'T' ('()' vs. 'Int')}}
  // expected-note@+1:3{{generic parameter 'T' inferred as 'Int' from context}}
  callitArgsFn(1) { // expected-note@:19{{generic parameter 'T' inferred as '()' from closure return expression}}
    { print("hello") }
  }
}

func test_55680_MultiExpr() -> Int {
  callit {
    print("hello") 
    return print("hello") // expected-error {{cannot convert value of type '()' to closure result type 'Int'}}
  }
}

func test_55680_GenericArg() -> Int {
  // Generic argument is inferred as Int from first argument literal, so no conflict in this case.
  callitGenericArg(1) {
    print("hello") // expected-error {{cannot convert value of type '()' to closure result type 'Int'}}
  }
}

func test_55680_Variadic() -> Int {
  // expected-error@+2{{conflicting arguments to generic parameter 'T' ('()' vs. 'Int')}}
  // expected-note@+1:3{{generic parameter 'T' inferred as 'Int' from context}}
  callitVariadic({ // expected-note@:18{{generic parameter 'T' inferred as '()' from closure return expression}}
    print("hello")
  })
}

func test_55680_Variadic_Twos() -> Int {
  // expected-error@+1{{cannot convert return expression of type '()' to return type 'Int'}}
  callitVariadic({
    print("hello")
  }, {
    print("hello")
  })
}

// rdar://82545600: this should just be a warning until Swift 6
public class TestImplicitCaptureOfExplicitCaptureOfSelfInEscapingClosure {
    var property = false

    private init() {
        doVoidStuff { [unowned self] in
            doVoidStuff {}
            doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
                doVoidStuff {}
                property = false // expected-warning {{reference to property 'property' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
            }
        }
    }
}

public class TestImplicitSelfForWeakSelfCapture {
  static let staticOptional: TestImplicitSelfForWeakSelfCapture? = .init()
  func method() { }
  
  private init() {
    doVoidStuff { [weak self] in
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      guard let self = self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      if let self = self {
        method()
      }

      if let self {
        method()
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self = self else { return } // expected-warning {{value 'self' was defined but never used; consider replacing with boolean test}}
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional else { return } // expected-warning {{value 'self' was defined but never used; consider replacing with boolean test}}
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      method() // expected-warning {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
      guard let self = self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      if let self = self {
        method()
      }
    }
    
    doVoidStuff { [weak self] in
      let `self`: TestImplicitSelfForWeakSelfCapture? = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional
      guard let self = self else { return }
      method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      let `self`: TestImplicitSelfForWeakSelfCapture? = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional
      guard let self = self else { return }
      method() // expected-warning {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self = self else { return } // expected-warning {{value 'self' was defined but never used; consider replacing with boolean test}}
      doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
      }
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self = self ?? TestImplicitSelfForWeakSelfCapture.staticOptional else { return } // expected-warning {{value 'self' was defined but never used; consider replacing with boolean test}}
      method() // expected-warning {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    }
    
    doVoidStuff { [weak self] in
      func innerFunction1() {
          method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self?.method()
      }
      
      guard let self else { return }
      
      func innerFunction2() {
          method()
          self.method()
      }
      
      subscript(index: Int) -> Int { // expected-error {{subscript' functions may only be declared within a type}}
        method()
        return index
      }
    }
    
    doVoidStuffNonEscaping { [weak self] in
      func innerFunction1() {
          method()
          self?.method()
      }
      
      guard let self else { return }
      
      func innerFunction2() {
          method()
          self.method()
      }
      
      subscript(index: Int) -> Int { // expected-error {{subscript' functions may only be declared within a type}}
        method()
        return index
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self else { return }
      
      func innerFunction1() {
        doVoidStuff { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}}
        }
        
        // This example should probably compile without an error -- seems like a bug in the impl of SE-0269
        doVoidStuff { [self] in // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}}
          method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self.method()
        }
        
        doVoidStuff { [weak self] in
          method() // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
          self?.method()
        }
        
        doVoidStuff { [weak self] in
          guard let self else { return }
          method()
          
          func innerMethod3() {
            method()
            self.method()
          }
        }
      }
    }
  }
}

class NoImplicitSelfInInnerClass {
  func method() { }
  
  private init() { // expected-note {{'self' declared here}} expected-note {{'self' declared here}} expected-note {{'self' declared here}} expected-note {{'self' declared here}} expected-note {{'self' declared here}} expected-note {{'self' declared here}} expected-note {{'self' declared here}}
    doVoidStuff {
      class InnerType { // expected-note {{type declared here}} expected-note {{type declared here}} expected-note {{type declared here}}
        init() {
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
        }
        
        func functionInsideInnerType() {
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
        }
        
        subscript(index: Int) -> Int {
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
          return index
        }
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self else { return }
      method()
      
      class InnerType { // expected-note {{type declared here}} expected-note {{type declared here}} expected-note {{type declared here}}
        func methodOnInnerType() { }
        
        init() {
          methodOnInnerType()
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
        }
        
        func functionInsideInnerType() {
          methodOnInnerType()
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
        }
        
        subscript(index: Int) -> Int {
          methodOnInnerType()
          method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
          self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
          return index
        }
      }
    }
    
    doVoidStuff { [weak self] in
      guard let self else { return }
      
      func innerMethod() {
        method()
        
        class InnerType { // expected-note {{type declared here}}
          func methodOnInnerType() { }
          
          init() {
            methodOnInnerType()
            method() // expected-error {{class declaration cannot close over value 'self' defined in outer scope}}
            self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
            
            doVoidStuff { [weak self] in
              guard let self else { return }
              self.method() // expected-error {{value of type 'InnerType' has no member 'method'}}
              methodOnInnerType()
            }
            
            doVoidStuff { [weak self] in
              guard let self else { return }
              method() // expected-error {{value of type 'InnerType' has no member 'method'}}
              methodOnInnerType()
            }
          }
        }
      }
    }
    
  }
  
  func foo(condition: Bool) {
    doVoidStuff { [weak self] in
      guard condition, let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, condition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard condition, let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, condition else { return }
      method()
    }
  }

  func foo(optionalCondition: Bool?) {
    doVoidStuff { [weak self] in
      guard let optionalCondition, optionalCondition, let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, let optionalCondition, optionalCondition else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let optionalCondition, let self, optionalCondition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let optionalCondition, optionalCondition, let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, let optionalCondition, optionalCondition else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let optionalCondition, let self, optionalCondition else { return }
      method()
    }
  }
  
  func foo() {
    doVoidStuff { [weak self] in
      guard #available(SwiftStdlib 5.8, *), let self else { return }
      method()
    }
    
    doVoidStuff { [weak self] in
      guard let self, #available(SwiftStdlib 5.8, *) else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard #available(SwiftStdlib 5.8, *), let self else { return }
      method()
    }
    
    doVoidStuffNonEscaping { [weak self] in
      guard let self, #available(SwiftStdlib 5.8, *) else { return }
      method()
    }
  }
}

public class TestRebindingSelfIsDisallowed {
  let count: Void = ()
  
  private init() {
    doVoidStuff {
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuffNonEscaping {
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuff { [weak self] in
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
    
    doVoidStuffNonEscaping { [weak self] in
      let `self` = "self shouldn't become a string"
      let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
    }
  }
  
  func method() {
    let `self` = "self shouldn't become a string"
    let _: Int = count // expected-error{{cannot convert value of type 'Void' to specified type 'Int'}}
  }
}

// https://github.com/apple/swift/issues/59716
["foo"].map { s in
    if s == "1" { return } // expected-error{{cannot convert value of type '()' to closure result type 'Bool'}}
    return s.isEmpty
}.filter { $0 }

["foo"].map { s in
    if s == "1" { return } // expected-error{{cannot convert value of type '()' to closure result type 'Bool'}}
    if s == "2" { return }
    if s == "3" { return }
    return s.isEmpty
}.filter { $0 }

["foo"].map { s in
    if s == "1" { return () } // expected-error{{cannot convert value of type '()' to closure result type 'Bool'}}
    return s.isEmpty
}.filter { $0 }

func producer<T>(_ f: (String) -> T) -> T {}
func f59716() -> some BinaryInteger { // expected-note{{required by opaque return type of global function 'f59716()'}}
  // expected-note@+1{{only concrete types such as structs, enums and classes can conform to protocols}}
  return producer { s in // expected-error{{type '()' cannot conform to 'BinaryInteger'}}
    if s == "1" { return }
    return s.count // expected-error{{cannot convert value of type 'Int' to closure result type '()'}}
  }
}

func f59716_1() -> some BinaryInteger {
  return producer { s in 
    if s == "1" { return 1 }
    return s.count 
  }
}

// https://github.com/apple/swift/issues/60781
func f60781<T>(_ x: T) -> T { x }
func f60781<T>(_ x: T, _ y: T) -> T { x }

func test60781() -> Int {
  f60781({ 1 }) // expected-error{{conflicting arguments to generic parameter 'T' ('Int' vs. '() -> Int')}}
}

func test60781_MultiArg() -> Int {
  f60781({ 1 }, { 1 }) // expected-error{{conflicting arguments to generic parameter 'T' ('Int' vs. '() -> Int')}}
}

@resultBuilder
struct VoidBuilder {
  static func buildBlock() -> Void { }
  static func buildPartialBlock<T>(first: T) -> Void { }
  static func buildPartialBlock<T>(accumulated: Void, next: T) -> Void { }
}

final class EscapingWrapper {
  static func wrapper(_ closure: @escaping () -> Void) {
    closure()
  }
}

final class TestGithubIssue64757 {
  var instanceProperty: String = "instance property"
  
  @VoidBuilder
  var void: Void {
    EscapingWrapper.wrapper { [weak self] in
      print(instanceProperty) // expected-error {{reference to property 'instanceProperty' in closure requires explicit use of 'self' to make capture semantics explicit}}
      
      if let self {
        print(instanceProperty)
      }
      
      guard let self else { return }
      print(instanceProperty)
    }
  }
}
