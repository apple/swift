// RUN: %target-typecheck-verify-swift

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


var selfRef = { selfRef() }
// expected-note@-1 2{{through reference here}}
// expected-error@-2 {{circular reference}}

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
    f($1.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'w'?}} {{7-9=w}} expected-error {{cannot convert value of type 'Int' to expected argument type 'String'}}
  }
  var a5 = { (_: [Int], w: [Int]) in
    f($0.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments}}
    f($1.count) // expected-error {{anonymous closure arguments cannot be used inside a closure that has explicit arguments; did you mean 'w'?}} {{7-9=w}}
    // expected-error@-1 {{cannot convert value of type 'Int' to expected argument type 'String'}}
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
    doVoidStuff({ [y = self] in x += 1 }) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{20-20=self, }} expected-note{{reference 'self.' explicitly}} {{33-33=self.}}
    doStuff({ [y = self] in x+1 }) // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ [weak self] in x += 1 }) // expected-note {{weak capture of 'self' here does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doStuff({ [weak self] in x+1 }) // expected-note {{weak capture of 'self' here does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doVoidStuff({ [self = self.x] in x += 1 }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doStuff({ [self = self.x] in x+1 }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}}

    // Methods follow the same rules as properties, uses of 'self' without capturing must be marked with "self."
    doStuff { method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{14-14= [self] in}} expected-note{{reference 'self.' explicitly}} {{15-15=self.}}
    doVoidStuff { _ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{23-23=self.}}
    doVoidStuff { _ = "\(method())" } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self] in}} expected-note{{reference 'self.' explicitly}} {{26-26=self.}}
    doVoidStuff { () -> () in _ = method() }  // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{18-18= [self]}} expected-note{{reference 'self.' explicitly}} {{35-35=self.}}
    doVoidStuff { [y = self] in _ = method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{20-20=self, }} expected-note{{reference 'self.' explicitly}} {{37-37=self.}}
    doStuff({ [y = self] in method() }) // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{29-29=self.}}
    doVoidStuff({ [weak self] in _ = method() }) // expected-note {{weak capture of 'self' here does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doStuff({ [weak self] in method() }) // expected-note {{weak capture of 'self' here does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doVoidStuff({ [self = self.x] in _ = method() }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
    doStuff({ [self = self.x] in method() }) // expected-note {{variable other than 'self' captured here under the name 'self' does not enable implicit 'self'}} expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}
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
    // With an empty capture list, insertion should should be suggested without a comma
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
    doStuff { [y] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }} expected-note{{reference 'self.' explicitly}} {{22-22=self.}}
    doStuff { [ // expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }}
        y
        ] in method() } // expected-error {{call to method 'method' in closure requires explicit use of 'self' to make capture semantics explicit}}  expected-note{{reference 'self.' explicitly}} {{14-14=self.}}

    // <rdar://problem/18877391> "self." shouldn't be required in the initializer expression in a capture list
    // This should not produce an error, "x" isn't being captured by the closure.
    doStuff({ [myX = x] in myX })

    // This should produce an error, since x is used within the inner closure.
    doStuff({ [myX = {x}] in 4 })    // expected-error {{reference to property 'x' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note{{capture 'self' explicitly to enable implicit 'self' in this closure}} {{23-23= [self] in }} expected-note{{reference 'self.' explicitly}} {{23-23=self.}}

    return 42
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
}

extension SomeClass {
  func bar() {
    doStuff { [unowned self] in self.foo() }
    doStuff { [unowned xyz = self.field!] in xyz.foo() }
    doStuff { [weak xyz = self.field] in xyz!.foo() }

    // rdar://16889886 - Assert when trying to weak capture a property of self in a lazy closure
    // FIXME: We should probably offer a fix-it to the field capture error and suppress the 'implicit self' error. https://bugs.swift.org/browse/SR-11634
    doStuff { [weak self.field] in field!.foo() }   // expected-error {{fields may only be captured by assigning to a specific name}} expected-error {{reference to property 'field' in closure requires explicit use of 'self' to make capture semantics explicit}} expected-note {{reference 'self.' explicitly}} {{36-36=self.}} expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}} {{16-16=self, }}
    doStuff { [weak self&field] in 42 }  // expected-error {{expected ']' at end of capture list}}

  }

  func strong_in_capture_list() {
    // <rdar://problem/18819742> QOI: "[strong self]" in capture list generates unhelpful error message
    _ = {[strong self] () -> () in return }  // expected-error {{expected 'weak', 'unowned', or no specifier in capture list}}
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
let samples = {   // expected-error {{unable to infer complex closure return type; add explicit type to disambiguate}} {{16-16= () -> <#Result#> in }}
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
let closure = { // expected-error {{unable to infer complex closure return type; add explicit type to disambiguate}} {{16-16= () -> <#Result#> in }}
  var helper = true
  return helper
}

// SR-9839
func SR9839(_ x: @escaping @convention(block) () -> Void) {}

func id<T>(_ x: T) -> T {
  return x
}

var qux: () -> Void = {}

SR9839(qux)
SR9839(id(qux)) // expected-error {{conflicting arguments to generic parameter 'T' ('() -> Void' vs. '@convention(block) () -> Void')}}

func forceUnwrap<T>(_ x: T?) -> T {
  return x!
}

var qux1: (() -> Void)? = {}

SR9839(qux1!)
SR9839(forceUnwrap(qux1))

// rdar://problem/65155671 - crash referencing parameter of outer closure
func rdar65155671(x: Int) {
    { a in
      _ = { [a] in a }
    }(x)
}

// Apply the explicit 'self' rule even if it referrs to a capture, if
// we're inside a nested closure
class SR14120 {
  func operation() {}

  func test1() {
    doVoidStuff { [self] in
      operation()
    }
  }

  func test2() {
    doVoidStuff { [self] in
      doVoidStuff {
        // expected-error@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
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
          // expected-error@+3 {{call to method 'operation' in closure requires explicit use of 'self'}}
          // expected-note@-2 {{capture 'self' explicitly to enable implicit 'self' in this closure}}
          // expected-note@+1 {{reference 'self.' explicitly}}
          operation()
        }
      }
    }
  }
}
