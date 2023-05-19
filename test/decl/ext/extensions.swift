// RUN: %target-typecheck-verify-swift -warn-redundant-requirements

extension extension_for_invalid_type_1 { // expected-error {{cannot find type 'extension_for_invalid_type_1' in scope}}
  func f() { }
}
extension extension_for_invalid_type_2 { // expected-error {{cannot find type 'extension_for_invalid_type_2' in scope}}
  static func f() { }
}
extension extension_for_invalid_type_3 { // expected-error {{cannot find type 'extension_for_invalid_type_3' in scope}}
  init() {}
}
extension extension_for_invalid_type_4 { // expected-error {{cannot find type 'extension_for_invalid_type_4' in scope}}
  deinit {} // expected-error {{deinitializers may only be declared within a class, actor, or noncopyable type}}
}
extension extension_for_invalid_type_5 { // expected-error {{cannot find type 'extension_for_invalid_type_5' in scope}}
  typealias X = Int
}

//===--- Test that we only allow extensions at file scope.
struct Foo { }

extension NestingTest1 { // expected-error {{cannot find type 'NestingTest1' in scope}}
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
struct NestingTest2 {
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
class NestingTest3 {
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
enum NestingTest4 {
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
protocol NestingTest5 {
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
func nestingTest6() {
  extension Foo {} // expected-error {{declaration is only valid at file scope}}
}
extension Array {
  func foo() {
    extension Array { // expected-error {{declaration is only valid at file scope}}
    }
  }
}

//===--- Test that we only allow extensions only for nominal types.

struct S1 {
  struct NestedStruct {}
}
extension S1 {} // no-error
extension S1.Type {} // expected-error {{cannot extend a metatype 'S1.Type'}}
extension S1.NestedStruct {} // no-error

struct S1_2 {
  // expected-error @+2 {{type member must not be named 'Type', since it would conflict with the 'foo.Type' expression}}
  // expected-note @+1 {{if this name is unavoidable, use backticks to escape it}} {{8-12=`Type`}}
  enum Type {}
}
struct S1_3 {
  enum `Type` {} // no-error
}

extension S1_2.Type {} // expected-error {{cannot extend a metatype 'S1_2.Type'}}
extension S1_3.`Type` {} // no-error

typealias TA_S1 = S1
extension TA_S1 {} // no-error

typealias TA_S1_NestedStruct = S1.NestedStruct
extension TA_S1_NestedStruct {} // no-error

enum U1 {
  struct NestedStruct {}
}
extension U1 {} // no-error
extension U1.NestedStruct {} // no-error

class C1 {
  struct NestedStruct {}
}
extension C1 {} // no-error
extension C1.NestedStruct {} // no-error

protocol P1 {}

protocol P2 {}

extension () {} // expected-error {{non-nominal type '()' cannot be extended}} {{educational-notes=nominal-types}}

typealias TupleAlias = (x: Int, y: Int)
extension TupleAlias {} // expected-error{{non-nominal type 'TupleAlias' (aka '(x: Int, y: Int)') cannot be extended}} {{educational-notes=nominal-types}}

// Test property accessors in extended types
class C {}
extension C {
  var p1: Int {
    get {return 1}
    set(v) {}
  }
}
var c = C()
var x = c.p1
c.p1 = 1

// Reject extension of nominal type via inferred associated type
protocol P3 {
  associatedtype Assoc
  func foo() -> Assoc
}

struct X3 : P3 {
}

extension X3.Assoc {
// expected-error@-1 {{extension of type 'X3.Assoc' (aka 'Int') must be declared as an extension of 'Int'}}
// expected-note@-2 {{did you mean to extend 'Int' instead?}}
}

extension X3 {
  func foo() -> Int { return 0 }
}

// Make sure the test case from https://github.com/apple/swift/issues/46432
// doesn't cause problems when the later extension is incorrectly nested inside
// another declaration.
extension C1.NestedStruct {
  static let originalValue = 0
}
struct WrapperContext {
  extension C1.NestedStruct { // expected-error {{declaration is only valid at file scope}}
    static let propUsingMember = originalValue
  }
}

// Class-constrained extension where protocol does not impose class requirement
// https://github.com/apple/swift/issues/53699

protocol DoesNotImposeClassReq_1 {}

class JustAClass: DoesNotImposeClassReq_1 {
  var property: String = ""
}

extension DoesNotImposeClassReq_1 where Self: JustAClass { // expected-warning{{redundant conformance constraint 'JustAClass' : 'DoesNotImposeClassReq_1'}}
  var wrappingProperty1: String {
    get { return property }
    set { property = newValue } // Okay
  }
  
  var wrappingProperty2: String {
    get { return property }
    nonmutating set { property = newValue } // Okay
  }
  
  var wrappingProperty3: String {
    get { return property }
    mutating set { property = newValue } // Okay
  }
  
  mutating func foo() {
    property = "" // Okay
    wrappingProperty1 = "" // Okay
    wrappingProperty2 = "" // Okay
    wrappingProperty3 = "" // Okay
  }
  
  func bar() { // expected-note {{mark method 'mutating' to make 'self' mutable}}{{3-3=mutating }}
    property = "" // Okay
    wrappingProperty1 = "" // Okay
    wrappingProperty2 = "" // Okay
    wrappingProperty3 = "" // expected-error {{cannot assign to property: 'self' is immutable}}
  }
  
  nonmutating func baz() { // expected-note {{mark method 'mutating' to make 'self' mutable}}{{3-14=mutating}}
    property = "" // Okay
    wrappingProperty1 = "" // Okay
    wrappingProperty2 = "" // Okay
    wrappingProperty3 = "" // expected-error {{cannot assign to property: 'self' is immutable}}
  }
}

let instanceOfJustAClass1 = JustAClass() // expected-note 2{{change 'let' to 'var' to make it mutable}}
instanceOfJustAClass1.wrappingProperty1 = "" // Okay
instanceOfJustAClass1.wrappingProperty2 = "" // Okay
instanceOfJustAClass1.wrappingProperty3 = "" // expected-error {{cannot assign to property: 'instanceOfJustAClass1' is a 'let' constant}}
instanceOfJustAClass1.foo() // expected-error {{cannot use mutating member on immutable value: 'instanceOfJustAClass1' is a 'let' constant}}
instanceOfJustAClass1.bar() // Okay
instanceOfJustAClass1.baz() // Okay

var instanceOfJustAClass2 = JustAClass()
instanceOfJustAClass2.foo() // Okay

protocol DoesNotImposeClassReq_2 {
  var property: String { get set }
}

extension DoesNotImposeClassReq_2 where Self : AnyObject {
  var wrappingProperty1: String {
    get { property }
    set { property = newValue } // expected-error {{cannot assign to property: 'self' is immutable}}
    // expected-note@-1 {{mark accessor 'mutating' to make 'self' mutable}}{{5-5=mutating }}
  }
  
  var wrappingProperty2: String {
    get { property }
    nonmutating set { property = newValue } // expected-error {{cannot assign to property: 'self' is immutable}}
    // expected-note@-1 {{mark accessor 'mutating' to make 'self' mutable}}{{5-16=mutating}}
  }
  
  var wrappingProperty3: String {
    get { property }
    mutating set { property = newValue } // Okay
  }
  
  mutating func foo() {
    property = "" // Okay
    wrappingProperty1 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty2 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty3 = "" // Okay
  }
  
  func bar() { // expected-note 2{{mark method 'mutating' to make 'self' mutable}}{{3-3=mutating }}
    property = "" // expected-error {{cannot assign to property: 'self' is immutable}}
    wrappingProperty1 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty2 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty3 = "" // expected-error {{cannot assign to property: 'self' is immutable}}
  }
  
  nonmutating func baz() { // expected-note 2{{mark method 'mutating' to make 'self' mutable}}{{3-14=mutating}}
    property = "" // expected-error {{cannot assign to property: 'self' is immutable}}
    wrappingProperty1 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty2 = "" // Okay (the error is on the setter declaration above)
    wrappingProperty3 = "" // expected-error {{cannot assign to property: 'self' is immutable}}
  }
}

protocol DoesNotImposeClassReq_3 {
  var someProperty: Int { get set }
}

class JustAClass1: DoesNotImposeClassReq_3 {
  var someProperty = 0
}

extension DoesNotImposeClassReq_3 where Self: JustAClass1 { // expected-warning {{redundant conformance constraint 'JustAClass1' : 'DoesNotImposeClassReq_3'}}
  var anotherProperty1: Int {
    get { return someProperty }
    set { someProperty = newValue } // Okay
  }

  var anotherProperty2: Int {
    get { return someProperty }
    mutating set { someProperty = newValue } // Okay
  }
}

let justAClass1 = JustAClass1() // expected-note {{change 'let' to 'var' to make it mutable}}
justAClass1.anotherProperty1 = 1234 // Okay
justAClass1.anotherProperty2 = 4321 // expected-error {{cannot assign to property: 'justAClass1' is a 'let' constant}}

protocol ImposeClassReq1: AnyObject {
  var someProperty: Int { get set }
}

class JustAClass2: ImposeClassReq1 {
  var someProperty = 0
}

extension ImposeClassReq1 where Self: AnyObject { // expected-warning {{redundant constraint 'Self' : 'AnyObject'}}
  var wrappingProperty1: Int {
    get { return someProperty }
    set { someProperty = newValue }
  }

  var wrappingProperty2: Int {
    get { return someProperty }
    mutating set { someProperty = newValue } // expected-error {{'mutating' is not valid on setters in class-bound protocols}}
  }

  mutating func foo() { // expected-error {{mutating' is not valid on instance methods in class-bound protocols}}
    someProperty = 1
  }

  nonmutating func bar() { // expected-error {{'nonmutating' is not valid on instance methods in class-bound protocols}}
    someProperty = 2
  }

  func baz() { // Okay
    someProperty = 3
  }
}

extension ImposeClassReq1 {
  var wrappingProperty3: Int {
    get { return someProperty }
    set { someProperty = newValue }
  }
}

let justAClass2 = JustAClass2() // expected-note {{change 'let' to 'var' to make it mutable}}
justAClass2.wrappingProperty1 = 9876 // Okay
justAClass2.wrappingProperty3 = 0987 // Okay
justAClass2.foo() // expected-error {{cannot use mutating member on immutable value: 'justAClass2' is a 'let' constant}}
justAClass2.bar() // Okay as well (complains about explicit nonmutating on decl)
justAClass2.baz() // Okay

protocol ImposeClassReq2: AnyObject {
  var someProperty: Int { get set }
}

extension ImposeClassReq2 {
  var wrappingProperty1: Int {
    get { return someProperty }
    set { someProperty = newValue }
  }

  var wrappingProperty2: Int {
    get { return someProperty }
    mutating set { someProperty = newValue } // expected-error {{'mutating' is not valid on setters in class-bound protocols}}
  }

  mutating func foo() { // expected-error {{mutating' is not valid on instance methods in class-bound protocols}}
    someProperty = 1
  }

  nonmutating func bar() { // expected-error {{'nonmutating' is not valid on instance methods in class-bound protocols}}
    someProperty = 2
  }

  func baz() { // Okay
    someProperty = 3
  }
}

// Reject extension of nominal type via typealias with dependent underlying type

struct Nest<Egg> { typealias Contents = Egg }
struct Tree { 
  typealias LimbContent = Nest<Int> 
  typealias BoughPayload = Nest<Nest<Int>>
}

extension Tree.LimbContent.Contents {
  // expected-error@-1 {{extension of type 'Tree.LimbContent.Contents' (aka 'Int') must be declared as an extension of 'Int'}}
  // expected-note@-2 {{did you mean to extend 'Int' instead?}} {{11-36=Int}}
}

extension Tree.BoughPayload.Contents {
  // expected-error@-1 {{extension of type 'Tree.BoughPayload.Contents' (aka 'Nest<Int>') must be declared as an extension of 'Nest<Int>'}}
  // expected-note@-2 {{did you mean to extend 'Nest<Int>' instead?}} {{11-37=Nest<Int>}}
}

// https://github.com/apple/swift/issues/52866
// Check 'where' clause when referencing type defined inside extension.
struct S_52866<T> {
  var a : A // expected-error {{'S_52866<T>.A' (aka 'Int') requires the types 'T' and 'Never' be equivalent}}
}
extension S_52866 where T == Never { // expected-note {{requirement specified as 'T' == 'Never' [with T = T]}}
  typealias A = Int
}

#if true
protocol Rdar66943328 {
  associatedtype Assoc
}
extension Rdar66943328 where Assoc == Int // expected-error {{expected '{' in extension}}
#endif

// Reject extension of existential type

protocol P4 {}

extension any P4 {
// expected-error@-1 {{extension of existential type 'any P4' is not supported}}
// expected-note@-2 {{did you mean to extend 'P4' instead?}} {{11-17=P4}}
}

typealias A4 = P4

extension any A4 {
// expected-error@-1 {{extension of existential type 'any A4' (aka 'any P4') is not supported}}
// expected-note@-2 {{did you mean to extend 'P4' instead?}} {{11-17=P4}}
}

typealias B4 = any P4
extension B4 {
// expected-error@-1 {{extension of existential type 'B4' (aka 'any P4') is not supported}}
// expected-note@-2 {{did you mean to extend 'P4' instead?}} {{11-13=P4}}
}
