// RUN: %target-typecheck-verify-swift -disable-availability-checking

enum Either<T,U> {
  case first(T)
  case second(U)
}

// expected-note @+2 {{add 'buildArray(_:)' to the result builder 'TupleBuilder' to add support for 'for'..'in' loops}}
// expected-note @+1 2 {{struct 'TupleBuilder' declared here}}
@resultBuilder struct TupleBuilder {
  static func buildBlock() -> () { }
  
  static func buildBlock<T1>(_ t1: T1) -> T1 {
    return t1
  }
  
  static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
    return (t1, t2)
  }
  
  static func buildBlock<T1, T2, T3>(_ t1: T1, _ t2: T2, _ t3: T3)
      -> (T1, T2, T3) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4)
      -> (T1, T2, T3, T4) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4, _ t5: T5
  ) -> (T1, T2, T3, T4, T5) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildDo<T>(_ value: T) -> T { return value }
  static func buildIf<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }
}

@resultBuilder
struct TupleBuilderWithoutIf { // expected-note 3{{struct 'TupleBuilderWithoutIf' declared here}}
  // expected-note@-1{{add 'buildOptional(_:)' to the result builder 'TupleBuilderWithoutIf' to add support for 'if' statements without an 'else'}}
  // expected-note@-2{{add 'buildEither(first:)' and 'buildEither(second:)' to the result builder 'TupleBuilderWithoutIf' to add support for 'if'-'else' and 'switch'}}
  // expected-note@-3{{add 'buildArray(_:)' to the result builder 'TupleBuilderWithoutIf' to add support for 'for'..'in' loops}}
  static func buildBlock() -> () { }
  
  static func buildBlock<T1>(_ t1: T1) -> T1 {
    return t1
  }
  
  static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
    return (t1, t2)
  }
  
  static func buildBlock<T1, T2, T3>(_ t1: T1, _ t2: T2, _ t3: T3)
      -> (T1, T2, T3) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4)
      -> (T1, T2, T3, T4) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4, _ t5: T5
  ) -> (T1, T2, T3, T4, T5) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildDo<T>(_ value: T) -> T { return value }
}

func tuplify<T>(_ cond: Bool, @TupleBuilder body: (Bool) -> T) { // expected-note {{'tuplify(_:body:)' declared here}}
  print(body(cond))
}

func tuplifyWithoutIf<T>(_ cond: Bool, @TupleBuilderWithoutIf body: (Bool) -> T) {
  print(body(cond))
}

func testDiags() {
  // For loop
  tuplify(true) { _ in
    17
    for c in name { // expected-error {{closure containing control flow statement cannot be used with result builder 'TupleBuilder'}}
    // expected-error@-1 {{cannot find 'name' in scope}}
    }
  }

  // Declarations
  tuplify(true) { _ in
    17
    let x = 17
    let y: Int // Ok
    x + 25
  }

  tuplify(true) { _ in
    17
    let y: Int, z: String // Ok
    y + 25
  }

  tuplify(true) { _ in
    0
    let x: Int = 0, y: String = "" // Multiple initialized pattern bindings are okay
    x + 1
    y
  }

  // Statements unsupported by the particular builder.
  tuplifyWithoutIf(true) {
    if $0 {    // expected-error{{closure containing control flow statement cannot be used with result builder 'TupleBuilderWithoutIf'}}
      "hello"
    }
  }

  tuplifyWithoutIf(true) {
    if $0 {    // expected-error{{closure containing control flow statement cannot be used with result builder 'TupleBuilderWithoutIf'}}
      "hello"
    } else {
    }
  }

  tuplifyWithoutIf(true) { a in
    for x in 0..<100 {    // expected-error{{closure containing control flow statement cannot be used with result builder 'TupleBuilderWithoutIf'}}
      x
    }
  }
}

struct A { }
struct B { }

func overloadedTuplify<T>(_ cond: Bool, @TupleBuilder body: (Bool) -> T) -> A { // expected-note {{found this candidate}}
  return A()
}

func overloadedTuplify<T>(_ cond: Bool, @TupleBuilderWithoutIf body: (Bool) -> T) -> B { // expected-note {{found this candidate}}
  return B()
}

func testOverloading(name: String) {
  let a1 = overloadedTuplify(true) { b in
    if b {
      "Hello, \(name)" 
    }
  }

  _ = overloadedTuplify(true) { cond in
    if cond {
      print(\"hello") // expected-error {{invalid component of Swift key path}}
    }
  }

  let _: A = a1

  _ = overloadedTuplify(true) { b in // expected-error {{ambiguous use of 'overloadedTuplify(_:body:)'}}
    b ? "Hello, \(name)" : "Goodbye"
    42
    overloadedTuplify(false) {
      $0 ? "Hello, \(name)" : "Goodbye"
      42
      if $0 {
        "Hello, \(name)" 
      }
    }
  }
}

protocol P {
  associatedtype T
}

struct AnyP : P {
  typealias T = Any
  init<T>(_: T) where T : P {}
}

struct TupleP<U> : P {
  typealias T = U
  init(_: U) {}
}

@resultBuilder
struct Builder {
  static func buildBlock<S0, S1>(_ stmt1: S0, _ stmt2: S1)
           -> TupleP<(S0, S1)> where S0: P, S1: P {
    return TupleP((stmt1, stmt2))
  }
}

struct G<C> : P where C : P {
  typealias T = C
  init(@Builder _: () -> C) {}
}

struct Text : P {
  typealias T = String
  init(_: T) {}
}

struct Label<L> : P where L : P { // expected-note 2 {{'L' declared as parameter to type 'Label'}}
  typealias T = L
  init(@Builder _: () -> L) {} // expected-note {{'init(_:)' declared here}}
}

func test_51167632() -> some P {
  AnyP(G {
    Text("hello")
    Label  // expected-error {{generic parameter 'L' could not be inferred}}
    // expected-note@-1 {{explicitly specify the generic arguments to fix this issue}} {{10-10=<<#L: P#>>}}
  })
}

func test_56221372() -> some P {
  AnyP(G {
    Text("hello")
    Label() // expected-error {{generic parameter 'L' could not be inferred}}
    // expected-error@-1 {{missing argument for parameter #1 in call}}
    // expected-note@-2 {{explicitly specify the generic arguments to fix this issue}} {{10-10=<<#L: P#>>}}
  })
}

// https://github.com/apple/swift/issues/53841
struct S_53841 {
  typealias ReturnsTuple<T> = () -> (T, T)
  subscript<T, U>(@TupleBuilder x: ReturnsTuple<T>) -> (ReturnsTuple<U>) -> Void { //expected-note {{in call to 'subscript(_:)'}}
    return { _ in }
  }

  func foo() {
    // This is okay, we apply the result builder for the subscript arg.
    self[{
      5
      5
    }]({
      (5, 5)
    })

    // But we shouldn't perform the transform for the argument to the call
    // made on the function returned from the subscript.
    self[{ // expected-error {{generic parameter 'U' could not be inferred}}
      5
      5
    }]({
      5
      5
    })
  }
}

func acceptInt(_: Int, _: () -> Void) { }

// https://github.com/apple/swift/issues/53751
// Crash due to improper recontextualization.
func erroneous_53751(x: Int) {
  tuplify(true) { b in
    17
    x + 25
    Optional(tuplify(false) { b in
      if b {
        acceptInt(0) { }
      }
    }).domap(0) // expected-error{{value of type '()?' has no member 'domap'}}
  }
}

func extraArg() {
  tuplify(true) { _ in
    1
    2
    3
    4
    5
    6 // expected-error {{extra argument in call}}
  }
}

// rdar://problem/53209000 - use of #warning and #error
tuplify(true) { x in
  1
  #error("boom")    // expected-error{{boom}}
  "hello"
  #warning("oops")  // expected-warning{{oops}}
  3.14159
}

struct MyTuplifiedStruct {
  var condition: Bool

  @TupleBuilder var computed: some Any { // expected-note{{remove the attribute to explicitly disable the result builder}}{{3-17=}}
    if condition {
      // expected-note@+1 {{remove 'return' statements to apply the result builder}}{{7-14=}}{{+2:12-19=}}
      return 17 // expected-warning{{application of result builder 'TupleBuilder' disabled by explicit 'return' statement}}
    } else {
           return 42
    }
  }
}

// Check that we're performing syntactic use diagnostics.
func acceptMetatype<T>(_: T.Type) -> Bool { true }

func syntacticUses<T>(_: T) {
  tuplify(true) { x in
    if x && acceptMetatype(T) { // expected-error{{expected member name or constructor call after type name}}
      // expected-note@-1{{use '.self' to reference the type object}}
      acceptMetatype(T) // expected-error{{expected member name or constructor call after type name}}
      // expected-note@-1{{use '.self' to reference the type object}}
    }
  }
}

// Check custom diagnostics within "if" conditions.
struct HasProperty {
  var property: Bool = false
}

func checkConditions(cond: Bool) {
  var x = HasProperty()

  tuplify(cond) { value in
    if x.property = value { // expected-error{{use of '=' in a boolean context, did you mean '=='?}}
      "matched it"
    }
  }
}

// Check that a closure with a single "return" works with result builders.
func checkSingleReturn(cond: Bool) {
  tuplify(cond) { value in
    return (value, 17)
  }

  tuplify(cond) { value in
    (value, 17)
  }

  tuplify(cond) {
    ($0, 17)
  }
}

// rdar://problem/59116520
func checkImplicitSelfInClosure() {
  @resultBuilder
  struct Builder {
    static func buildBlock(_ children: String...) -> Element { Element() }
  }

  struct Element {
    static func nonEscapingClosure(@Builder closure: (() -> Element)) {}
    static func escapingClosure(@Builder closure: @escaping (() -> Element)) {}
  }

  class C {
    let identifier: String = ""

    func testImplicitSelf() {
      Element.nonEscapingClosure {
        identifier // okay
      }

      Element.escapingClosure { // expected-note {{capture 'self' explicitly to enable implicit 'self' in this closure}}
        identifier // expected-error {{reference to property 'identifier' in closure requires explicit use of 'self' to make capture semantics explicit}}
        // expected-note@-1 {{reference 'self.' explicitly}}
      }
    }
  }
}

// rdar://problem/59239224 - crash because some nodes don't have type
// information during solution application.
struct X<T> {
  init(_: T) { }
}

@TupleBuilder func foo(cond: Bool) -> some Any {
  if cond {
    tuplify(cond) { x in
      X(x)
    }
  }
}

// switch statements don't allow fallthrough
enum E {
  case a
  case b(Int, String?)
}

func testSwitch(e: E) {
  tuplify(true) { c in
    "testSwitch"
    switch e {
    case .a:
      "a"
    case .b(let i, let s?):
      i * 2
      s + "!"
      fallthrough // expected-error{{closure containing control flow statement cannot be used with result builder 'TupleBuilder'}}
    case .b(let i, nil):
      "just \(i)"
    }
  }
}

// Ensure that we don't back-propagate constraints to the subject
// expression. This is a potential avenue for future exploration, but
// is currently not supported by switch statements outside of function
// builders. It's better to be consistent for now.
enum E2 {
  case b(Int, String?) // expected-note{{'b' declared here}}
}

func getSomeEnumOverloaded(_: Double) -> E { return .a }
func getSomeEnumOverloaded(_: Int) -> E2 { return .b(0, nil) }

func testOverloadedSwitch() {
  tuplify(true) { c in
    switch getSomeEnumOverloaded(17) {
    case .a: // expected-error{{type 'E2' has no member 'a'; did you mean 'b'?}}
      "a"
    default:
      "default"
    }
  }
}

// Check exhaustivity.
func testNonExhaustiveSwitch(e: E) {
    tuplify(true) { c in
    "testSwitch"
    switch e { // expected-error{{switch must be exhaustive}}
      // expected-note @-1{{add missing case: '.b(_, .none)'}}
    case .a:
      "a"
    case .b(let i, let s?):
      i * 2
      s + "!"
    }
  }
}

// rdar://problem/59856491
struct TestConstraintGenerationErrors {
  @TupleBuilder var buildTupleFnBody: String {
    let a = nil // There is no diagnostic here because next line fails to pre-check, so body is invalid
    String(nothing) // expected-error {{cannot find 'nothing' in scope}}
  }

  @TupleBuilder var nilWithoutContext: String {
    let a = nil // expected-error {{'nil' requires a contextual type}}
    ""
  }

  func buildTupleClosure() {
    tuplify(true) { _ in
      let a = nothing // expected-error {{cannot find 'nothing' in scope}}
      String(nothing) // expected-error {{cannot find 'nothing' in scope}}
    }
  }
}

// Check @unknown
func testUnknownInSwitchSwitch(e: E) {
  tuplify(true) { c in
    "testSwitch"
    switch e {
    case .b(let i, let s?):
      i * 2
      s + "!"
    default:
      "nothing"
    @unknown case .a: // expected-error{{'@unknown' is only supported for catch-all cases ("case _")}}
      // expected-error@-1{{'case' blocks cannot appear after the 'default' block of a 'switch'}}
      "a"
    }
  }

  tuplify(true) { c in
    "testSwitch"
    switch e {
      @unknown case _: // expected-error{{'@unknown' can only be applied to the last case in a switch}}
      "a"
    default:
      "default"
    }
  }
}

// Check for mutability mismatches when there are multiple case items
// referring to same-named variables.
enum E3 {
  case a(Int, String)
  case b(String, Int)
  case c(String, Int)
}

func testCaseMutabilityMismatches(e: E3) {
   tuplify(true) { c in
    "testSwitch"
    switch e {
    case .a(let x, var y),
         .b(let y, // expected-error{{'let' pattern binding must match previous 'var' pattern binding}}
            var x), // expected-error{{'var' pattern binding must match previous 'let' pattern binding}}
         .c(let y, // expected-error{{'let' pattern binding must match previous 'var' pattern binding}}
            var x): // expected-error{{'var' pattern binding must match previous 'let' pattern binding}}
      x
      y += "a"
    default:
      "default"
    }
  }
}

// Check for type equivalence among different case variables with the same name.
func testCaseVarTypes(e: E3) {
    tuplify(true) { c in
    "testSwitch"
    switch e {
    case .a(let x, let y),
         .c(let x, let y):
         // expected-error@-1 {{pattern variable bound to type 'String', expected type 'Int'}}
         // expected-error@-2 {{pattern variable bound to type 'Int', expected type 'String'}}
      x
      y + "a"
    }
  }
}

// Test for buildFinalResult.
@resultBuilder
struct WrapperBuilder {
  static func buildBlock() -> () { }
  
  static func buildBlock<T1>(_ t1: T1) -> T1 {
    return t1
  }
  
  static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
    return (t1, t2)
  }
  
  static func buildBlock<T1, T2, T3>(_ t1: T1, _ t2: T2, _ t3: T3)
      -> (T1, T2, T3) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4)
      -> (T1, T2, T3, T4) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: T1, _ t2: T2, _ t3: T3, _ t4: T4, _ t5: T5
  ) -> (T1, T2, T3, T4, T5) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildDo<T>(_ value: T) -> T { return value }
  static func buildIf<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }
  static func buildFinalResult<T>(_ value: T) -> Wrapper<T> {
    return Wrapper(value: value)
  }
}

struct Wrapper<T> {
  var value: T
}

func wrapperify<T>(_ cond: Bool, @WrapperBuilder body: (Bool) -> T) -> T{
  return body(cond)
}

func testWrapperBuilder() {
  let x = wrapperify(true) { c in
    3.14159
    "hello"
  }

  let _: Int = x // expected-error{{cannot convert value of type 'Wrapper<(Double, String)>' to specified type 'Int'}}
}

// rdar://problem/61347993 - empty result builder doesn't compile
func rdar61347993() {
  struct Result {}

  @resultBuilder
  struct Builder {
    static func buildBlock() -> Result {
      Result()
    }
  }

  func test_builder<T>(@Builder _: () -> T) {}
  test_builder {} // Ok

  func test_closure(_: () -> Result) {}
  test_closure {} // expected-error {{cannot convert value of type '()' to closure result type 'Result'}}
}

// One-way constraints through parameters.
func wrapperifyInfer<T, U>(_ cond: Bool, @WrapperBuilder body: (U) -> T) -> T {
  fatalError("boom")
}

let intValue = 17
_ = wrapperifyInfer(true) { x in // Ok
  intValue + x
}

struct DoesNotConform {}

struct MyView {
  @TupleBuilder var value: some P { // expected-error {{return type of property 'value' requires that 'DoesNotConform' conform to 'P'}}
    // expected-note@-1 {{opaque return type declared here}}
    DoesNotConform()
  }

  @TupleBuilder func test() -> some P { // expected-error {{return type of instance method 'test()' requires that 'DoesNotConform' conform to 'P'}}
    // expected-note@-1 {{opaque return type declared here}}
    DoesNotConform()
  }

  @TupleBuilder var emptySwitch: some P {
    switch Optional.some(1) { // expected-error {{'switch' statement body must have at least one 'case' or 'default' block; do you want to add a default case?}}
    }
  }

  @TupleBuilder var invalidSwitchOne: some P {
    switch Optional.some(1) {
    case . // expected-error {{expected ':' after 'case'}}
    } // expected-error {{expected identifier after '.' expression}}
  }

  @TupleBuilder var invalidSwitchMultiple: some P {
    switch Optional.some(1) {
    case .none: // expected-error {{'case' label in a 'switch' must have at least one executable statement}}
    case . // expected-error {{expected ':' after 'case'}}
    } // expected-error {{expected identifier after '.' expression}}
  }

  @TupleBuilder var invalidCaseWithoutDot: some P {
    switch Optional.some(1) {
    case none: 42 // expected-error {{cannot find 'none' in scope}}
    case .some(let x):
      0
    }
  }

  @TupleBuilder var invalidConversion: Int { // expected-error {{cannot convert return expression of type 'String' to return type 'Int'}}
    ""
  }
}

// Make sure throwing result builder closures are implied.
enum MyError: Error {
  case boom
}

do {
    tuplify(true) { c in // expected-error{{invalid conversion from throwing function of type '(Bool) throws -> String' to non-throwing function type '(Bool) -> String'}}
    "testThrow"
    throw MyError.boom
  }
}

struct TuplifiedStructWithInvalidClosure {
  var condition: Bool?

  @TupleBuilder var unknownParameter: some Any {
    if let cond = condition {
      let _ = { (arg: UnknownType) in // expected-error {{cannot find type 'UnknownType' in scope}}
      }
      42
    } else {
      0
    }
  }

  @TupleBuilder var unknownResult: some Any {
    if let _ = condition {
      let _ = { () -> UnknownType in // expected-error {{cannot find type 'UnknownType' in scope}}
      }
      42
    } else {
      0
    }
  }

  @TupleBuilder var multipleLevelsDeep: some Any {
    if let _ = condition {
      switch MyError.boom {
      case .boom:
        let _ = { () -> UnknownType in // expected-error {{cannot find type 'UnknownType' in scope}}
        }
      }

      42
    } else {
      0
    }
  }

  @TupleBuilder var errorsDiagnosedByParser: some Any {
    if let _ = condition {
      tuplify { _ in
        self. // expected-error {{expected member name following '.'}}
      }
      42
    }
  }

  @TupleBuilder var nestedErrorsDiagnosedByParser: some Any {
    tuplify(true) { _ in
      tuplify { _ in // expected-error {{missing argument for parameter #1 in call}}
        self. // expected-error {{expected member name following '.'}}
      }
      42
    }
  }
}

// rdar://65667992 - invalid case in enum causes fallback diagnostic
func test_rdar65667992() {
  @resultBuilder
  struct Builder {
    static func buildBlock<T>(_ t: T) -> T { t }
    static func buildEither<T>(first: T) -> T { first }
    static func buildEither<T>(second: T) -> T { second }
  }

  struct S {}

  enum E {
    case set(v: Int, choices: [Int])
    case notSet(choices: [Int])
  }

  struct MyView {
    var entry: E

    @Builder var body: S {
      switch entry {
      case .set(_, _): S()
      case .unset(_): S() // expected-error {{type 'E' has no member 'unset'}}
      // expected-error@-1 {{'_' can only appear in a pattern or on the left side of an assignment}}
      default: S()
      }
    }
  }
}


func test_weak_with_nonoptional_type() {
  class X {
    func test() -> Int { 0 }
  }

  tuplify(true) { c in
    weak var x: X = X() // expected-error {{'weak' variable should have optional type 'X?'}}

    if let x = x {
      x.test()
    }

    42
  }
}

// rdar://80941497 - compiler fails to produce diagnostic when referencing missing member in optional context
func test_missing_member_in_optional_context() {
  struct Test {
  }

  var test: Test? = nil

  tuplify(true) { c in
    if let prop = test?.prop { // expected-error {{value of type 'Test' has no member 'prop'}}
      0
    }
  }

  tuplify(true) { c in
    if let method = test?.method() { // expected-error {{value of type 'Test' has no member 'method'}}
      1
    }
  }
}

func test_redeclarations() {
  tuplify(true) { c in
    let foo = 0 // expected-note {{'foo' previously declared here}}
    let foo = foo // expected-error {{invalid redeclaration of 'foo'}}
  }

  tuplify(true) { c in
    let (foo, foo) = (5, 6) // expected-error {{invalid redeclaration of 'foo'}} expected-note {{'foo' previously declared here}}
  }
}

func test_rdar89742267() {
  @resultBuilder
  struct Builder {
    static func buildBlock<T>(_ t: T) -> T { t }
    static func buildEither<T>(first: T) -> T { first }
    static func buildEither<T>(second: T) -> T { second }
  }

  struct S {}

  enum Hey {
    case listen
  }

  struct MyView {
    var entry: Hey

    @Builder var body: S {
      switch entry {
      case .listen: S()
      case nil: S() // expected-warning {{type 'Hey' is not optional, value can never be nil; this is an error in Swift 6}}
      default: S()
      }
    }
  }
}

// https://github.com/apple/swift/issues/59390
func test_invalid_result_is_diagnosed() {
  @resultBuilder
  struct MyBuilder {
    static func buildBlock<T1>(_ t1: T1) -> T1 {
      return t1
    }
  }

  struct S<T> {}

  @MyBuilder
  func test() -> S<String> { // expected-error {{conflicting arguments to generic parameter 'T1' ('S<Int>' vs. 'S<String>')}}
    S<Int>()
  }
}

func test_associated_values_dont_block_solver_when_unresolved() {
  @resultBuilder
  struct Builder {
    static func buildBlock<T>(_ t: T) -> T { t }
    static func buildEither<T>(first: T) -> T { first }
    static func buildEither<T>(second: T) -> T { second }
  }

  struct Value {
    enum Kind {
      case a(String)
      case b
    }

    var kind: Kind
  }

  struct Container {
    var prop: Value? = nil
  }

  struct TestWithAny {
    var container: Container

    @Builder var body: String {
      let v = container.prop.kind // expected-error {{value of optional type 'Value?' must be unwrapped to refer to member 'kind' of wrapped base type 'Value'}}
      // expected-note@-1 {{chain the optional using '?' to access member 'kind' only for non-'nil' base values}}
      // expected-note@-2 {{force-unwrap using '!' to abort execution if the optional value contains 'nil'}}

      switch v.kind {
      case .a(_): "a"
      case .b: "b"
      }
    }

    @Builder var switchError: String {
      let v = container.prop!.kind

      switch v.kind { // expected-error {{value of type 'Value.Kind' has no member 'kind'}}
      case .a(_): "a"
      case .b: "b"
      }
    }
  }
}

func test_dependent_member_with_unresolved_base_type() {
  struct Wrapper<A, T: P> : P {
  }

  @resultBuilder
  struct Builder<A> {
    static func buildBlock(_ value: some P) -> some P {
      value
    }
  }

  func test<A, U: P>(data: KeyPath<A, [(Int, U.T)]>, // expected-note {{in call to function 'test(data:_:)'}}
                     @Builder<U.T> _: () -> U) -> Wrapper<A, U> { fatalError() }

  struct Value : P {
    typealias T = (Int, String)
  }

  struct Test : P {
    struct T {
      var values: [(Int, Value.T)]
    }

    var v: some P {
      test(data: \T.values) { // expected-error {{generic parameter 'U' could not be inferred}}
        Value()
      }
    }
  }
}

// rdar://89880662 - incorrect error about unsupported control flow statement
func test_impact_of_control_flow_fix() {
  @resultBuilder
  struct BuilderA {
    static func buildOptional<T>(_ value: T?) -> T? { return value }
    static func buildBlock<T>(_ v1: T) -> T { v1 }
    static func buildBlock<T, U>(_ v1: T, _ v2: U) -> (T, U) { (v1, v2) }
  }

  @resultBuilder
  struct BuilderB {
    static func buildBlock<T>(_ v1: T) -> T { v1 }
    static func buildBlock<T, U>(_ v1: T, _ v2: U) -> (T, U) { (v1, v2) }
  }

  func fn<T>(@BuilderA _: () -> T) {}
  func fn(@BuilderB _: () -> (Int?, Void)) {}

  func test(_: Int) {}

  fn {
    if true {
      0
    }

    test("") // expected-error {{cannot convert value of type 'String' to expected argument type 'Int'}}
  }
}

protocol Q {}

func test_requirement_failure_in_buildBlock() {
  struct A : P { typealias T = Int }
  struct B : Q { typealias T = String }

  struct Result<T> : P { typealias T = Int }

  @resultBuilder
  struct BuilderA {
    static func buildBlock<T0: P, T1: P>(_: T0, _: T1) -> Result<(T0, T1)> { fatalError() }
    // expected-note@-1 {{candidate requires that 'B' conform to 'P' (requirement specified as 'T1' : 'P')}}
  }

  @resultBuilder
  struct BuilderB {
    static func buildBlock<U0: Q, U1: Q>(_ v: U0, _: U1) -> some Q { v }
    // expected-note@-1 {{candidate requires that 'A' conform to 'Q' (requirement specified as 'U0' : 'Q')}}
  }

  struct Test {
    func fn<T: P>(@BuilderA _: () -> T) {}
    func fn<T: Q>(@BuilderB _: () -> T) {}

    func test() {
      fn { // expected-error {{no exact matches in reference to static method 'buildBlock'}}
        A()
        B()
      }
    }
  }
}

func test_partially_resolved_closure_params() {
  struct S<T> {
    var a: String = ""
  }

  func test<T>(@TupleBuilder _: (S<T>) -> T) { // expected-note {{in call to function 'test'}}
  }

  test { // expected-error {{generic parameter 'T' could not be inferred}}
    $0.a
    42
  }
}
