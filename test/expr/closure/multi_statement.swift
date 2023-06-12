// RUN: %target-typecheck-verify-swift -swift-version 5 -enable-experimental-static-assert

func isInt<T>(_ value: T) -> Bool {
  return value is Int
}

func maybeGetValue<T>(_ value: T) -> T? {
  return value
}

enum MyError: Error {
  case featureIsTooCool

  func doIt() { }
}

enum State {
  case suspended
  case partial(Int, Int)
  case finished
}

func random(_: Int) -> Bool { return false }

func mightThrow() throws -> Bool { throw MyError.featureIsTooCool }

func mapWithMoreStatements(ints: [Int], state: State) throws {
  let _ = try ints.map { i in
    guard var actualValue = maybeGetValue(i) else {
      return String(0)
    }

    let value = actualValue + 1
    do {
      if isInt(i) {
        print(value)
      } else if value == 17 {
        print("seventeen!")
      }
    }

    while actualValue < 100 {
      actualValue += 1
    }

  my_repeat:
    repeat {
      print("still here")

      if i % 7 == 0 {
        break my_repeat
      }
    } while random(i)

    defer {
      print("I am so done here")
    }

    for j in 0..<i where j % 2 == 0 {
      if j % 7 == 0 {
        continue
      }

      switch (state, j) {
      case (.suspended, 0):
        print("something")
        fallthrough
      case (.finished, 0):
        print("something else")

      case (.partial(let current, let end), let j):
        print("\(current) of \(end): \(j)")

      default:
        print("so, here we are")
      }
      print("even")
      throw MyError.featureIsTooCool
    }

    #assert(true)

    // expected-warning@+1{{danger zone}}
    #warning("danger zone")

#if false
    struct NothingHere { }
#else
    struct NestedStruct {
      var x: Int
    }
#endif

    do {
      print(try mightThrow())
    } catch let e as MyError {
      e.doIt()
    } catch {
      print(error)
    }
    return String(value)
  }
}

func acceptsWhateverClosure<T, R>(_ value: T, _ fn: (T) -> R) { }

func testReturnWithoutExpr(i: Int) {
  acceptsWhateverClosure(i) { i in
    print(i)
    return
  }
}

// `withContiguousStorageIfAvailable` is overloaded, so let's make sure that
// filtering works correctly.
func test_overloaded_call(arr: [Int], body: (UnsafeBufferPointer<Int>) -> Void) -> Void {
  arr.withContiguousStorageIfAvailable { buffer in
    let _ = type(of: buffer)
    body(buffer) // ok
  }
}

// Used to wrap closure in `FunctionConversionExpr` in this case,
// but now solver would just inject return expression into optional where necessary.
func test_result_optional_injection() {
  func fn<T>(_: () -> T?) -> [T] {
    []
  }

  _ = fn {
    if true {
      return // Ok
    }
  }
}

let _ = {
  for i: Int8 in 0 ..< 20 { // Ok (pattern can inform a type of the sequence)
    print(i)
  }
}

func test_workaround_for_optional_void_result() {
  func test<T>(_: (Int?) -> T?) {}

  test {
    guard let x = $0 else {
      return // Ok
    }

    print(x)
  }

  test {
    if $0! > 0 {
      return
    }

    let _ = $0
  }

  func test_concrete(_: (Int) -> Void?) {
  }

  test_concrete {
    guard let x = Optional($0) else {
      return // Ok
    }

    print(x)
  }

  test_concrete {
    if $0 > 0 {
      return // Ok
    }

    let _ = $0
  }
}

enum WrapperEnum<Wrapped> where Wrapped: RawRepresentable {
case known(Wrapped)

  static func ~= (lhs: Wrapped, rhs: WrapperEnum<Wrapped>) -> Bool where Wrapped: Equatable {
    switch rhs {
    case .known(let wrapped):
      return wrapped == lhs
    }
  }
}

func test_custom_tilde_equals_operator_matching() {
  enum TildeTest : String {
  case test = "test"
  case otherTest = ""
  }

  func test(_: (WrapperEnum<TildeTest>) -> Void) {}

  test { v in
    print(v)

    switch v {
    case .test: break // Ok although `.test` comes from `TildeTest` instead of `WrapperEnum`
    case .otherTest: break // Ok although `.otherTest` comes from `TildeTest` instead of `WrapperEnum`
    case .known(_): break // Ok - `.known` comes from `WrapperEnum`
    }
  }
}

// Local functions can capture variables before they are declared.
func test_local_function_capturing_vars() {
  struct A {
    var cond: Bool
  }

  func test<T>(fn: () -> T) -> T {
    fn()
  }

  func outer(a: A) {
    test {
      func local() {
        if !message.isEmpty { // Ok
          print(message)
        }

        message = "World" // Ok
      }

      var message = a.cond ? "hello" : ""
    }
  }
}

func test_test_invalid_redeclaration() {
  func test(_: () -> Void) {
  }

  test {
    let foo = 0 // expected-note {{'foo' previously declared here}}
    let foo = foo // expected-error {{invalid redeclaration of 'foo'}}
  }

  test {
    let (foo, foo) = (5, 6) // expected-error {{invalid redeclaration of 'foo'}} expected-note {{'foo' previously declared here}}
  }
}

func test_pattern_ambiguity_doesnot_crash_compiler() {
  enum E {
  case hello(result: Int) // expected-note 2 {{found this candidate}}
  case hello(status: Int) // expected-note 2 {{found this candidate}}
  }

  let _: (E) -> Void = {
    switch $0 {
    case .hello(_): break // expected-error {{ambiguous use of 'hello'}}
    }
  }

  let _: (E) -> Void = {
    switch $0 {
    case let E.hello(x): print(x) // expected-error {{ambiguous use of 'hello'}}
    default: break
    }
  }
}

func test_taps_type_checked_with_correct_decl_context() {
  struct Path {
    func contains<T>(_: T) -> Bool where T: StringProtocol { return false }
  }

  let paths: [Path] = []
  let strs: [String] = []

  _ = paths.filter { path in
    for str in strs where path.contains("\(str).hello") {
      return true
    }
    return false
  }
}

// rdar://90347159 - in pattern matching context `case` should be preferred over static declarations
func test_pattern_matches_only_cases() {
  enum ParsingError : Error {
    case ok(Int)
    case failed([Error], Int)

    static var ok: Int { 42 }
    static func failed(_: [Error], at: Any) -> Self { fatalError() }
  }

  let _: (ParsingError) -> Void = {
    switch $0 {
    case let ParsingError.failed(errors, _): print(errors) // Ok
    default: break
    }

    switch $0 {
    case let ParsingError.ok(result): print(result) // Ok
    default: break
    }
  }
}

// rdar://91225620 - type of expression is ambiguous without a type annotation in closure
func test_wrapped_var_without_initializer() {
  @propertyWrapper
  struct Wrapper {
    private let name: String

    var wrappedValue: Bool {
      didSet {}
    }

    init(name: String) {
      self.wrappedValue = false
      self.name = name
    }
  }

  func fn(_: () -> Void) {}

  fn {
    @Wrapper(name: "foo")
    var v;
  }
}

// rdar://92366212 - crash in ConstraintSystem::getType
func test_unknown_refs_in_tilde_operator() {
  enum E {
  }

  let _: (E) -> Void = {
    if case .test(unknown) = $0 {
      // expected-error@-1 2 {{cannot find 'unknown' in scope}}
      // expected-error@-2 {{type 'E' has no member 'test'}}
    }
  }
}

// rdar://92347054 - crash during conjunction processing
func test_no_crash_with_circular_ref_due_to_error() {
  struct S { // expected-note {{did you mean 'S'?}}
    var x: Int?
  }

  func test(v: Int?, arr: [S]) -> Int { // expected-note {{did you mean 'v'?}}
    // There is missing `f` here which made body of the
    // `if` a multiple statement closure instead that uses
    // `next` inside.
    i let x = v, let next = arr.first?.x { // expected-error {{cannot find 'i' in scope}}
      // expected-error@-1 {{consecutive statements on a line must be separated by ';'}}
      // expected-error@-2 {{'let' cannot appear nested inside another 'var' or 'let' pattern}}
      // expected-error@-3 {{cannot call value of non-function type 'Int?'}}
      print(next)
      return x
    }
    return 0
  }
}

func test_diagnosing_on_missing_member_in_case() {
  enum E {
    case one
  }

  func test(_: (E) -> Void) {}

  test {
    switch $0 {
    case .one: break
    case .unknown: break // expected-error {{type 'E' has no member 'unknown'}}
    }
  }
}

// rdar://92757114 - fallback diagnostic when member doesn't exist in a nested closure
func test_diagnose_missing_member_in_inner_closure() {
  struct B {
    static var member: any StringProtocol = ""
  }

  struct Cont<T, E: Error> {
    func resume(returning value: T) {}
  }

  func withCont<T>(function: String = #function,
                   _ body: (Cont<T, Never>) -> Void) -> T {
    fatalError()
  }

  func test(vals: [Int]?) -> [Int] {
    withCont { continuation in
      guard let vals = vals else {
        return continuation.resume(returning: [])
      }

      B.member.get(0, // expected-error {{value of type 'any StringProtocol' has no member 'get'}}
                   type: "type",
                   withinSecs: Int(60*60)) { arr in
        let result = arr.compactMap { $0 }
        return continuation.resume(returning: result)
      }
    }
  }
}

// Type finder shouldn't bring external closure result type
// into the scope of an inner closure e.g. while solving
// init of pattern binding `x`.
func test_type_finder_doesnt_walk_into_inner_closures() {
  func test<T>(fn: () -> T) -> T { fn() }

  _ = test { // Ok
    let x = test {
      42
    }

    let _ = test {
      test { "" }
    }

    // multi-statement
    let _ = test {
      _ = 42
      return test { "" }
    }

    return x
  }
}

// rdar://94049113 - compiler accepts non-optional `guard let` in a closure
func test_non_optional_guard_let_is_diagnosed() {
  func fn(_: (Int) -> Void) {}

  fn {
    if true {
      guard let v = $0 else { // expected-error {{initializer for conditional binding must have Optional type, not 'Int'}}
        return
      }

      print(v)
    }
  }

  fn {
    switch $0 {
    case (let val):
      fn {
        guard let x = val else {  // expected-error {{initializer for conditional binding must have Optional type, not 'Int'}}
          return
        }

        print($0 + x)
      }

    default: break
    }
  }
}

// rdar://93796211 (issue#59035) - crash during solution application to fallthrough statement
func test_fallthrough_stmt() {
  {
    var collector: [Void] = []
    for _: Void in [] {
      switch (() as Void?, ()) {
      case (let a?, let b):
        // expected-warning@-1 {{constant 'b' inferred to have type '()', which may be unexpected}}
        // expected-note@-2 {{add an explicit type annotation to silence this warning}}
        collector.append(a)
        fallthrough
      case (nil,    let b):
        // expected-warning@-1 {{constant 'b' inferred to have type '()', which may be unexpected}}
        // expected-note@-2 {{add an explicit type annotation to silence this warning}}
        collector.append(b)
      }
    }
  }()
}

// rdar://93061432 - No diagnostic for invalid `for-in` statement
func test_missing_conformance_diagnostics_in_for_sequence() {
  struct Event {}

  struct S {
    struct Iterator: IteratorProtocol {
      typealias Element = (event: Event, timestamp: Double)

      mutating func next() -> Element? { return nil }
    }
  }

  func fn(_: () -> Void) {}

  func test(_ iter: inout S.Iterator) {
    fn {
      for v in iter.next() { // expected-error {{for-in loop requires 'S.Iterator.Element?' (aka 'Optional<(event: Event, timestamp: Double)>') to conform to 'Sequence'; did you mean to unwrap optional?}}
        _ = v.event
      }
    }

    fn {
      while let v = iter.next() { // ok
        _ = v.event
      }
    }
  }
}

func test_conflicting_pattern_vars() {
  enum E {
  case a(Int, String)
  case b(String, Int)
  }

  func fn(_: (E) -> Void) {}
  func fn<T>(_: (E) -> T) {}

  func test(e: E) {
    fn {
      switch $0 {
      case .a(let x, let y),
           .b(let x, let y):
        // expected-error@-1 {{pattern variable bound to type 'String', expected type 'Int'}}
        // expected-error@-2 {{pattern variable bound to type 'Int', expected type 'String'}}
        _ = x
        _ = y
      }
    }

    fn {
      switch $0 {
      case .a(let x, let y),
           .b(let y, let x): // Ok
        _ = x
        _ = y
      }
    }
  }
}

// rdar://91452726 - crash in MissingMemberFailure::diagnoseInLiteralCollectionContext
struct Test {
  struct ID {
  }

  enum E : Hashable, Equatable {
  case id
  }

  var arr: [(ID, E)]

  func test() {
    _ = arr.map { v in
      switch v {
      case .id: return true // expected-error {{value of tuple type '(Test.ID, Test.E)' has no member 'id'}}
      }
    }
  }
}

https://github.com/apple/swift/issues/61017
do {
  @propertyWrapper
  struct Wrapper {
    var wrappedValue: Int

    init(wrappedValue: Int) {
      self.wrappedValue = wrappedValue
    }
  }

  class Test {
    let bar: () -> Void = {
      @Wrapper var wrapped = 1
      let wrapper: Wrapper = _wrapped // Ok
    }
  }
}

https://github.com/apple/swift/issues/61024
do {
  enum Baz: String {
  case someCase
  }

  @propertyWrapper
  struct Wrapper {
    var wrappedValue: Int
    let argument: String

    init(wrappedValue: Int, argument: String) { // expected-note 2 {{'init(wrappedValue:argument:)' declared here}}
      self.wrappedValue = wrappedValue
      self.argument = argument
    }
  }

  class Foo {
    let ok: () -> Void = {
      @Wrapper(argument: Baz.someCase.rawValue) var wrapped1 = 1 // Ok
      @Wrapper(wrappedValue: 42, argument: Baz.someCase.rawValue) var wrapped2 // Ok
      @Wrapper(wrappedValue: 42, argument: Baz.someCase.rawValue) var wrapped3: Int // Ok
    }

    let bad0: () -> Void = {
      @Wrapper var wrapped: Int
      // expected-error@-1 {{missing arguments for parameters 'wrappedValue', 'argument' in call}}
    }

    let bad1: () -> Void = {
      @Wrapper var wrapped = 0
      // expected-error@-1 {{missing argument for parameter 'argument' in property wrapper initializer; add 'wrappedValue' and 'argument' arguments in '@Wrapper(...)'}}
    }

    let bad2: () -> Void = {
      @Wrapper(wrappedValue: 42, argument: Baz.someCase.rawValue) var wrapped = 0
      // expected-error@-1 {{extra argument 'wrappedValue' in call}}
    }
  }
}

// Test to make sure that type-checker doesn't attempt the closure passed to
// `.filter` before the one from `.init`, otherwise it would mean that generic
// parameter of `.filter` wouldn't be inferred since it depends on result of
// `.init` closure.
func test_that_closures_are_attempted_in_order() {
  struct Test<T> {
    init(_: ([Int]) -> T) {}
    init(_: String) {}
    init(_: Int, _: String = "") {}

    func filter(_: (T) -> Bool) {}
  }

  Test {
    _ = 42
    return $0.map { Optional(Float($0)) }
  }
  .filter {
    if $0.isEmpty { // Ok
      return true
    }
    return false
  }
}

func test_use_of_concrete_params_in_for_condition() {
  struct S {
    var cond: Bool
  }

  func test(_: (S) -> Void) {}

  test { data in
    for i in 0...10 where !data.cond { // Ok
      print(i)
    }
  }
}

// https://github.com/apple/swift/issues/63455
func test_recursive_var_reference_in_multistatement_closure() {
  struct MyStruct {
    func someMethod() {}
  }

  func takeClosure(_ x: () -> Void) {}

  func test(optionalInt: Int?, themes: MyStruct?) {
    takeClosure {
      let int = optionalInt { // expected-error {{cannot call value of non-function type 'Int?'}}
        print(int)
      }
    }

    takeClosure {
      let theme = themes?.someMethod() { // expected-error {{extra trailing closure passed in call}}
        _ = theme
      }
    }

    takeClosure {
      let theme = themes?.filter({ $0 }) { // expected-error {{value of type 'MyStruct' has no member 'filter'}}
        _ = theme
      }
    }
  }
}
