// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

enum Either<T,U> {
  case first(T)
  case second(U)
}

@resultBuilder
struct TupleBuilder {
  static func buildBlock<T1>(_ t1: T1) -> (T1) {
    return (t1)
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

  static func buildIf<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }

  static func buildArray<T>(_ array: [T]) -> [T] { return array }
}

func tuplify<T>(_ cond: Bool, @TupleBuilder body: (Bool) throws -> T) rethrows {
  print(try body(cond))
}

// CHECK: (17, 3.14159, "Hello, DSL", (["nested", "do"], 6), Optional((2.71828, ["if", "stmt"])))
let name = "dsl"
tuplify(true) {
  17
  3.14159
  "Hello, \(name.map { $0.uppercased() }.joined())"
  do {
    ["nested", "do"]
    1 + 2 + 3
  }
  if $0 {
    2.71828
    ["if", "stmt"]
  }
}

// CHECK: ("Empty optional", nil)
tuplify(false) {
  "Empty optional"
  if $0 {
    2.71828
    ["if", "stmt"]
  }
}

// CHECK: ("chain0", main.Either<(Swift.String, Swift.Double), (Swift.Double, Swift.String)>.second(2.8, "capable"))
tuplify(false) {
  "chain0"
  if $0 {
    "marginal"
    2.9
  } else {
    2.8
    "capable"
  }
}

// CHECK: ("chain1", nil)
tuplify(false) {
  "chain1"
  if $0 {
    "marginal"
    2.9
  } else if $0 {
    2.8
    "capable"
  }
}

// CHECK: ("chain2", Optional(main.Either<(Swift.String, Swift.Double), (Swift.Double, Swift.String)>.first("marginal", 2.9)))
tuplify(true) {
  "chain2"
  if $0 {
    "marginal"
    2.9
  } else if $0 {
    2.8
    "capable"
  }
}

// CHECK: ("chain3", main.Either<main.Either<(Swift.String, Swift.Double), (Swift.Double, Swift.String)>, main.Either<(Swift.Double, Swift.Double), (Swift.String, Swift.String)>>.first(main.Either<(Swift.String, Swift.Double), (Swift.Double, Swift.String)>.first("marginal", 2.9)))
tuplify(true) {
  "chain3"
  if $0 {
    "marginal"
    2.9
  } else if $0 {
    2.8
    "capable"
  } else if $0 {
    2.8
    1.0
  } else {
    "wild"
    "broken"
  }
}

// CHECK: ("chain4", main.Either<main.Either<main.Either<(Swift.String, Swift.Int), (Swift.String, Swift.Int)>, main.Either<(Swift.String, Swift.Int), (Swift.String, Swift.Int)>>, main.Either<main.Either<(Swift.String, Swift.Int), (Swift.String, Swift.Int)>, (Swift.String, Swift.Int)>>.first
tuplify(true) {
  "chain4"
  if $0 {
    "0"
    0
  } else if $0 {
    "1"
    1
  } else if $0 {
    "2"
    2
  } else if $0 {
    "3"
    3
  } else if $0 {
    "4"
    4
  } else if $0 {
    "5"
    5
  } else {
    "6"
    6
  }
}

// rdar://50710698
// CHECK: ("chain5", 8, 9)
tuplify(true) {
  "chain5"
  #if false
    6
    $0
  #else
    8
    9
  #endif
}

// CHECK: ("getterBuilder", 0, 4, 12)
@TupleBuilder
var globalBuilder: (String, Int, Int, Int) {
  "getterBuilder"
  0
  4
  12
}
print(globalBuilder)

// CHECK: ("funcBuilder", 13, 45.0)
@TupleBuilder
func funcBuilder(d: Double) -> (String, Int, Double) {
  "funcBuilder"
  13
  d
}
print(funcBuilder(d: 45))

struct MemberBuilders {
  @TupleBuilder
  func methodBuilder(_ i: Int) -> (String, Int) {
    "methodBuilder"
    i
  }

  @TupleBuilder
  static func staticMethodBuilder(_ i: Int) -> (String, Int) {
    "staticMethodBuilder"
    i + 14
  }

  @TupleBuilder
  var propertyBuilder: (String, Int) {
    "propertyBuilder"
    12
  }
}

// CHECK: ("staticMethodBuilder", 27)
print(MemberBuilders.staticMethodBuilder(13))

let mbuilders = MemberBuilders()

// CHECK: ("methodBuilder", 13)
print(mbuilders.methodBuilder(13))

// CHECK: ("propertyBuilder", 12)
print(mbuilders.propertyBuilder)

// https://github.com/apple/swift/issues/53840
// Operator builders

infix operator ^^^
func ^^^ (lhs: Int, @TupleBuilder rhs: (Int) -> (String, Int)) -> (String, Int) {
  return rhs(lhs)
}

// CHECK: ("hello", 6)
print(5 ^^^ {
  "hello"
  $0 + 1
})

struct Tagged<Tag, Entity> {
  let tag: Tag
  let entity: Entity
}

protocol Taggable {
}

extension Taggable {
  func tag<Tag>(_ tag: Tag) -> Tagged<Tag, Self> {
    return Tagged(tag: tag, entity: self)
  }
}

extension Int: Taggable { }
extension String: Taggable { }
extension Double: Taggable { }

@resultBuilder
struct TaggedBuilder<Tag> {
  static func buildBlock() -> () { }

  static func buildBlock<T1>(_ t1: Tagged<Tag, T1>) -> Tagged<Tag, T1> {
    return t1
  }

  static func buildBlock<T1, T2>(_ t1: Tagged<Tag, T1>, _ t2: Tagged<Tag, T2>) -> (Tagged<Tag, T1>, Tagged<Tag, T2>) {
    return (t1, t2)
  }

  static func buildBlock<T1, T2, T3>(_ t1: Tagged<Tag, T1>, _ t2: Tagged<Tag, T2>, _ t3: Tagged<Tag, T3>)
      -> (Tagged<Tag, T1>, Tagged<Tag, T2>, Tagged<Tag, T3>) {
    return (t1, t2, t3)
  }

  static func buildBlock<T1, T2, T3, T4>(_ t1: Tagged<Tag, T1>, _ t2: Tagged<Tag, T2>, _ t3: Tagged<Tag, T3>, _ t4: Tagged<Tag, T4>)
      -> (Tagged<Tag, T1>, Tagged<Tag, T2>, Tagged<Tag, T3>, Tagged<Tag, T4>) {
    return (t1, t2, t3, t4)
  }

  static func buildBlock<T1, T2, T3, T4, T5>(
    _ t1: Tagged<Tag, T1>, _ t2: Tagged<Tag, T2>, _ t3: Tagged<Tag, T3>, _ t4: Tagged<Tag, T4>, _ t5: Tagged<Tag, T5>
  ) -> (Tagged<Tag, T1>, Tagged<Tag, T2>, Tagged<Tag, T3>, Tagged<Tag, T4>, Tagged<Tag, T5>) {
    return (t1, t2, t3, t4, t5)
  }

  static func buildIf<T>(_ value: Tagged<Tag, T>?) -> Tagged<Tag, T>? { return value }
}

enum Color {
  case red, green, blue
}

func acceptColorTagged<Result>(@TaggedBuilder<Color> body: () -> Result) {
  print(body())
}

struct TagAccepter<Tag> {
  static func acceptTagged<Result>(@TaggedBuilder<Tag> body: () -> Result) {
    print(body())
  }
}

func testAcceptColorTagged(b: Bool, i: Int, s: String, d: Double) {
  // FIXME: When we support buildExpression, drop the "Color" prefix
  // CHECK: Tagged<
  acceptColorTagged {
    i.tag(Color.red)
    s.tag(Color.green)
    d.tag(Color.blue)
  }

  // FIXME: When we support buildExpression, drop the "Color" prefix
  // CHECK: Tagged<
  TagAccepter<Color>.acceptTagged {
    i.tag(Color.red)
    s.tag(Color.green)
    d.tag(Color.blue)
  }

  // CHECK: Tagged<
  TagAccepter<Color>.acceptTagged { () -> Tagged<Color, Int> in 
    if b {
      return i.tag(Color.green)
    } else {
      return i.tag(Color.blue)
    }
  }
}

testAcceptColorTagged(b: true, i: 17, s: "Hello", d: 3.14159)

// Use buildExpression() when it's available.
enum Component {
  case string(StaticString)
  case floating(Double)
  case color(Color)
  indirect case array([Component])
  indirect case optional(Component?)
}

@resultBuilder
struct ComponentBuilder {
  static func buildExpression(_ string: StaticString) -> Component {
    return .string(string)
  }

  static func buildExpression(_ float: Double) -> Component {
    return .floating(float)
  }

  static func buildExpression(_ color: Color) -> Component {
    return .color(color)
  }

  static func buildBlock(_ components: Component...) -> Component {
    return .array(components)
  }

  static func buildIf(_ value: Component?) -> Component {
    return .optional(value)
  }
}

func acceptComponentBuilder(@ComponentBuilder _ body: () -> Component) {
  print(body())
}

func colorWithAutoClosure(_ color: @autoclosure () -> Color) -> Color {
  return color()
}

var trueValue = true
acceptComponentBuilder {
  "hello"
  if trueValue {
    3.14159
    colorWithAutoClosure(.red)
  }
  .red
}
// CHECK: array([main.Component.string("hello"), main.Component.optional(Optional(main.Component.array([main.Component.floating(3.14159), main.Component.color(main.Color.red)]))), main.Component.color(main.Color.red)])

// rdar://53325810

// Test that we don't have problems with expression pre-checking when
// type-checking an overloaded result-builder call.  In particular,
// we need to make sure that expressions in the closure are pre-checked
// before we build constraints for them.  Note that top-level expressions
// that need to be rewritten by expression prechecking (such as the operator
// sequences in the boolean conditions and statements below) won't be
// rewritten in the original closure body if we just precheck the
// expressions produced by the result-builder transformation.
struct ForEach1<Data : RandomAccessCollection, Content> {
  var data: Data
  var content: (Data.Element) -> Content

  func show() {
    print(content(data.first!))
    print(content(data.last!))
  }
}
extension ForEach1 where Data.Element: StringProtocol {
  // Checking this overload shouldn't trigger inappropriate caching that
  // affects checking the next overload.
  init(_ data: Data,
       @TupleBuilder content: @escaping (Data.Element) -> Content) {
    self.init(data: data, content: content)
  }
}
extension ForEach1 where Data == Range<Int> {
  // This is the overload we actually want.
  init(_ data: Data,
       @TupleBuilder content: @escaping (Int) -> Content) {
    self.init(data: data, content: content)
  }
}
let testForEach1 = ForEach1(-10 ..< 10) { i in
  "testForEach1"
  if i < 0 {
    "begin"
    i < -5
  } else {
    i > 5
    "end"
  }
}
testForEach1.show()

// CHECK: ("testForEach1", main.Either<(Swift.String, Swift.Bool), (Swift.Bool, Swift.String)>.first("begin", true))
// CHECK: ("testForEach1", main.Either<(Swift.String, Swift.Bool), (Swift.Bool, Swift.String)>.second(true, "end"))

func test_single_stmt_closure_support() {
  @resultBuilder
  struct MyBuilder {
    static func buildBlock(_ numbers: Int...) -> Int {
      return 42
    }
  }

  func test(@MyBuilder builder: () -> Int) -> Int {
    builder()
  }

  let _ = test { 0 } // ok
}

// Check a case involving nested closures that refer to parameters of their
// enclosing closures.
struct X<C: Collection, T> {
  init(_ c: C, @TupleBuilder body: (C.Element) -> T) { }
}

struct Y<T> {
  init(@TupleBuilder body: () -> T) { }
}

struct Z<T> {
  init(@TupleBuilder body: () -> T) { }
}

func testNestedClosuresWithDependencies(cond: Bool) {
  tuplify(cond) { _ in
    X([1, 2, 3]) { x in
      Y {
        Z {
          x
          1
        }
      }
    }
  }
}

// Check that we can handle multiple conditions in an 'if' statement.
func testIfConditions(cond: Bool, c1: Bool, i1: Int, i2: Int) {
  tuplify(cond) { x in
    "testIfConditions"
    if i1 == i2, c1, x {
      1
      "hello"
    }
    3.14159
  }
}
testIfConditions(cond: true, c1: true, i1: 1, i2: 1)
// CHECK: testIfConditions
// CHECK-SAME: hello

// Use a "let" declaration within a result builder.
tuplify(true) { c in
  "testLetDeclarations"
  let (a, b) = (c, c && true)
  if a == b {
    "hello"
    b
  }
  a
}
// CHECK: testLetDeclarations"
// CHECK-SAME: hello
// CHECK-SAME: true

// Use if let / if case with various forms of decomposition.
func getOptionalInt(_: Bool) -> Int? { return 25 }

enum E {
  case a
  case b(Int, String?)
}

func getE(_ i: Int) -> E {
  switch i {
  case 0:
    return .a
  case 1:
    return .b(17, "hello")
  case 2:
    return .b(42, nil)
  default:
    fatalError("Unhandled case")
  }
}

func test_labeled_splats() {
  enum E {
  case multi(a: String, b: String)
  case tuple((a: Int, b: Int))
  case single(result: Int)
  case single_multi(result: (a: Int, q: String))
  }

  func test_answer(_: String) -> Int { 42 }
  func test_question(_: Int) -> String { "ultimate question" }

  let e: E = E.single(result: 42)

  tuplify(true) { _ in
    switch e {
    case .single(let result):
      test_question(result)
    case let .single_multi(result):
      test_answer(result.q)
      test_question(result.a)
    case let .multi(value): // tuple splat preserves labels
      test_answer(value.a)
      test_answer(value.b)
    case let .tuple(a: a, b: b): // un-splat preserves labels too
      test_question(a)
      test_question(b)
    }

    // compound names still work with and without splat
    switch e {
    case .single(_): 42
    case .single_multi(result: (let a, let q)):
      test_answer(q)
      test_question(a)
    case let .multi(a: a, b: b):
      test_answer(a)
      test_answer(b)
    case let .tuple((a: a, b: b)):
      test_question(a)
      test_question(b)
    }

    // no labels, no problem regardless of splatting
    switch e {
    case .single(_): 42
    case let .single_multi(result: (a, q)):
      test_question(a)
      test_answer(q)
    case let .multi(a, b):
      test_answer(a)
      test_answer(b)
    case let .tuple((a, b)):
      test_question(a)
      test_question(b)
    }
  }
}

tuplify(true) { c in
  "testIfLetMatching"
  if let theValue = getOptionalInt(c) {
    theValue + 17
  }
  if case let .a = getE(0) {
    "matched without payload"
  }
  if case let .b(i, s?) = getE(1) {
    "matched with payload"
    s + "!"
    i + 17
  }
  if case let .b(i, s?) = getE(2) {
    fatalError("cannot match this")
  } else {
    "intentional mismatch"
  }
}
// CHECK: testIfLetMatching
// CHECK-SAME: Optional(42)
// CHECK-SAME: Optional("matched without payload")
// CHECK-SAME: "matched with payload", "hello!", 34
// CHECK-SAME: "intentional mismatch"

class Super { }

class Sub : Super {
  func subMethod() -> String {
    return "subMethod"
  }
}

func getSuper(wantSubclass: Bool) -> Super {
  return wantSubclass ? Sub() : Super()
}

tuplify(true) { c in
  "testIfLetAsMatching"
  if case let sub as Sub = getSuper(wantSubclass: true) {
    sub.subMethod()
  }
  if case let sub as Sub = getSuper(wantSubclass: false) {
    fatalError("cannot match this")
  } else {
    "Superclass instance"
  }
}
// CHECK: testIfLetAsMatching
// CHECK-SAME: "subMethod"
// CHECK-SAME: "Superclass instance"


// switch statements
func testSwitch(_ e: E) {
  tuplify(true) { c in
    "testSwitch"
    switch e {
    case .a:
      "a"
    case .b(let i, let s?):
      i * 2
      s + "!"
    case .b(let i, nil):
      "just \(i)"
    }
  }
}

func testExistingPatternsInCaseStatements() {
  tuplify(true) { c in
    switch false {
    case (c): 1 // Ok
    default:  0
    }
  }

  var arr: [Int] = []

  tuplify(true) { c in
    let n = arr.endIndex

    switch arr.startIndex {
    case (n): 1 // Ok
    default:  0
    }
  }
}

// CHECK: testSwitch
// CHECK-SAME: first(main.Either<Swift.String, (Swift.Int, Swift.String)>.first("a"))
testSwitch(getE(0))

// CHECK: testSwitch
// CHECK-SAME: first(main.Either<Swift.String, (Swift.Int, Swift.String)>.second(34, "hello!"))
testSwitch(getE(1))

// CHECK: testSwitch
// CHECK-SAME: second("just 42")
testSwitch(getE(2))

func testSwitchCombined(_ eIn: E) {
  var e = eIn
  tuplify(true) { c in
    "testSwitchCombined"
    switch e {
    case .a:
      "a"
    case .b(let i, _?), .b(let i, nil):
      "just \(i)"
    }
  }
}

// CHECK: testSwitchCombined
// CHECK-SAME: main.Either<Swift.String, Swift.String>.first("a")
testSwitchCombined(getE(0))

// CHECK: testSwitchCombined
// CHECK-SAME: second("just 17")
testSwitchCombined(getE(1))

// CHECK: testSwitchCombined
// CHECK-SAME: second("just 42")
testSwitchCombined(getE(2))


// Test buildOptional(_:) as an alternative to buildIf(_:).
@resultBuilder
struct TupleBuilderWithOpt {
  static func buildBlock<T1>(_ t1: T1) -> (T1) {
    return (t1)
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

  static func buildOptional<T>(_ value: T?) -> T? { return value }

  static func buildEither<T,U>(first value: T) -> Either<T,U> {
    return .first(value)
  }
  static func buildEither<T,U>(second value: U) -> Either<T,U> {
    return .second(value)
  }
}

func tuplifyWithOpt<T>(_ cond: Bool, @TupleBuilderWithOpt body: (Bool) -> T) {
  print(body(cond))
}
tuplifyWithOpt(true) { c in
  "1"
  3.14159
}

// Test for-each loops with buildArray.
// CHECK: testForEach
// CHECK-SAME: (1, "separator")
// CHECK-SAME: (2, "separator")
// CHECK-SAME: (3, "separator")
// CHECK-SAME: (4, "separator")
// CHECK-SAME: (5, "separator")
// CHECK-SAME: (6, "separator")
// CHECK-SAME: (7, "separator")
// CHECK-SAME: (8, "separator")
// CHECK-SAME: (9, "separator")
// CHECK-SAME: (10, "separator")
tuplify(true) { c in
  "testForEach"
  for i in 0 ..< (c ? 10 : 5) {
    i + 1
    "separator"
  }
}

// Test the use of result builders partly implemented through a protocol.
indirect enum FunctionBuilder<Expression> {
    case expression(Expression)
    case block([FunctionBuilder])
    case either(Either<FunctionBuilder, FunctionBuilder>)
    case optional(FunctionBuilder?)
}

protocol FunctionBuilderProtocol {
    associatedtype Expression
    typealias Component = FunctionBuilder<Expression>
    associatedtype Return

    static func buildExpression(_ expression: Expression) -> Component
    static func buildBlock(_ components: Component...) -> Component
    static func buildOptional(_ optional: Component?) -> Component
    static func buildArray(_ components: [Component]) -> Component
    static func buildLimitedAvailability(_ component: Component) -> Component

    static func buildFinalResult(_ components: Component) -> Return
}

extension FunctionBuilderProtocol {
    static func buildExpression(_ expression: Expression) -> Component { .expression(expression) }
    static func buildBlock(_ components: Component...) -> Component { .block(components) }
    static func buildOptional(_ optional: Component?) -> Component { .optional(optional) }
    static func buildArray(_ components: [Component]) -> Component { .block(components) }
    static func buildLimitedAvailability(_ component: Component) -> Component { component }
}

@resultBuilder
enum ArrayBuilder<E>: FunctionBuilderProtocol {
    typealias Expression = E
    typealias Component = FunctionBuilder<E>
    typealias Return = [E]

    static func buildFinalResult(_ components: Component) -> Return {
        switch components {
        case .expression(let e): return [e]
        case .block(let children): return children.flatMap(buildFinalResult)
        case .either(.first(let child)): return buildFinalResult(child)
        case .either(.second(let child)): return buildFinalResult(child)
        case .optional(let child?): return buildFinalResult(child)
        case .optional(nil): return []
        }
    }
}


func buildArray(@ArrayBuilder<String> build: () -> [String]) -> [String] {
    return build()
}


let a = buildArray {
    "1"
    "2"
    if Bool.random() {
        "maybe 3"
    }
}
// CHECK: ["1", "2"
print(a)

// Throwing in result builders.
enum MyError: Error {
  case boom
}

// CHECK: testThrow
do {
  print("testThrow")
  try tuplify(true) { c in
    "ready to throw"
    throw MyError.boom
  }
} catch MyError.boom {
  // CHECK: caught it!
  print("caught it!")
} catch {
  fatalError("Threw something else?")
}

// CHECK: testStoredProperties
struct MyTupleStruct<T, U> {
  @TupleBuilder let first: () -> T
  @TupleBuilder let second: U
}

print("testStoredProperties")
let ts1 = MyTupleStruct {
  1
  "hello"
  if true {
    "conditional"
  }
} second: {
  3.14159
  "blah"
}

// CHECK: MyTupleStruct<(Int, String, Optional<String>), (Double, String)>(first: (), second: (3.14159, "blah"))
print(ts1)

// Make sure that `weakV` is `Test?` and not `Test??`
func test_weak_optionality_stays_the_same() {
  class Test {
    func fn() -> Int { 42 }
  }

  tuplify(true) { c in
    weak var weakV: Test? = Test()

    0
    if let v = weakV {
      v.fn()
    }
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
  @resultBuilder
  struct Builder {
    static func buildBlock<T>(_ t: T) -> T { t }
    static func buildEither<T>(first: T) -> T { first }
    static func buildEither<T>(second: T) -> T { second }
  }

  enum TildeTest : String {
  case test = "test"
  }

  struct S {}

  struct MyView {
    var entry: WrapperEnum<TildeTest>

    @Builder var body: S {
      switch entry {
      case .test: S() // Ok although `.test` comes from `TildeTest` instead of `WrapperEnum`
      case .known(_): S() // Ok - `.known` comes directly from `WrapperEnum`
      }
    }
  }
}

struct Values<T> {
  var values: T

  init(values: T) {
    self.values = values
  }

  func map<R>(_ f: (T) -> R) -> Values<R> {
    .init(values: f(values))
  }
}

@resultBuilder
enum NestedTupleBuilder {
  static func buildPartialBlock<T>(first x: T) -> Values<T> {
    .init(values: x)
  }

  static func buildPartialBlock<T, U>(
    accumulated: Values<T>, next: U
  ) -> Values<(T, U)> {
    .init(values: (accumulated.values, next))
  }
}

extension Values {
  init(@NestedTupleBuilder nested values: () -> Self) {
    self = values()
  }
}

let nestedValues = Values(nested: {
  1
  "2"
  3.0
  "yes"
})
print(nestedValues)

@resultBuilder
enum NestedTupleBuilder_Not {
  @available(*, unavailable)
  static func buildPartialBlock<T>(first x: T) -> Values<T> {
    .init(values: x)
  }

  @available(*, unavailable)
  static func buildPartialBlock<T, U>(
    accumulated: Values<T>, next: U
  ) -> Values<(T, U)> {
    .init(values: (accumulated.values, next))
  }

#if os(macOS)
  @available(macOS 9999, *)
  static func buildPartialBlock(first x: Never) -> Values<Never> {
    fatalError()
  }

  @available(macOS 9999, *)
  static func buildPartialBlock(
    accumulated: Values<Never>, next: Never
  ) -> Values<Never> {
    fatalError()
  }
#endif

  // This one will be called because no `buildPartialBlock` is available.
  static func buildBlock(_ x: Any...) -> Values<[Any]> {
    .init(values: x)
  }
}

extension Values {
  init(@NestedTupleBuilder_Not nested_not values: () -> Self) {
    self = values()
  }
}

let nestedValues_not = Values(nested_not: {
  1
  "2"
  3.0
  "yes"
})
print(nestedValues_not)

// CHECK: Values<Array<Any>>(values: [1, "2", 3.0, "yes"])

@resultBuilder
enum FlatTupleBuilder {
  static func buildExpression<T>(_ x: T) -> Values<T> {
    .init(values: x)
  }

  static func buildPartialBlock<T>(first x: Values<T>) -> Values<T> {
    .init(values: x.values)
  }

  static func buildPartialBlock<T, N>(
    accumulated: Values<T>,
    next: Values<N>
  ) -> Values<(T, N)> {
    .init(values: (accumulated.values, next.values))
  }

  static func buildPartialBlock<T0, T1, N>(
    accumulated: Values<(T0, T1)>,
    next: Values<N>
  ) -> Values<(T0, T1, N)> {
    .init(values: (accumulated.values.0, accumulated.values.1, next.values))
  }

  static func buildPartialBlock<T0, T1, T2, N>(
    accumulated: Values<(T0, T1, T2)>,
    next: Values<N>
  ) -> Values<(T0, T1, T2, N)> {
    .init(values: (accumulated.values.0, accumulated.values.1, accumulated.values.2, next.values))
  }

  static func buildPartialBlock<T0, T1, T2, T3, N>(
    accumulated: Values<(T0, T1, T2, T3)>,
    next: Values<N>
  ) -> Values<(T0, T1, T2, T3, N)> {
    .init(values: (accumulated.values.0, accumulated.values.1, accumulated.values.2, accumulated.values.3, next.values))
  }

  static func buildBlock(_ x: Never...) -> Values<()> {
    assert(x.isEmpty, "I should never be called unless it's nullary")
    return .init(values: ())
  }

  static func buildEither<T>(first: T) -> T {
    first
  }

  static func buildEither<T>(second: T) -> T {
    second
  }

  static func buildOptional<T>(_ x: Values<T>?) -> Values<T?> {
    x?.map { $0 } ?? .init(values: nil)
  }

  static func buildLimitedAvailability<T>(_ x: Values<T>) -> Values<T> {
    x
  }
}

extension Values {
  init(@FlatTupleBuilder flat values: () -> Self) {
    self = values()
  }
}

let flatValues0 = Values(flat: {})
print(flatValues0)
// CHECK: Values<()>(values: ())

let flatValues1 = Values(flat: {
  1
  "2"
  3.0
})
print(flatValues1)
// CHECK: Values<(Int, String, Double)>(values: (1, "2", 3.0))

let flatValues2 = Values(flat: {
  1
  "2"
  let y = 3.0 + 4.0
  #if false
  "not gonna happen"
  #endif
  if true {
    "yes"
  } else {
    "no"
  }
  #warning("Beware of pairwise block building")
  #if true
  if false {
    "nah"
  }
  if #available(*) {
    5.0
  }
  #endif
})
print(flatValues2)

// CHECK: Values<(Int, String, String, Optional<String>, Optional<Double>)>(values: (1, "2", "yes", nil, Optional(5.0)))

struct Nil: CustomStringConvertible {
  var description: String {
    "nil"
  }
}
struct Cons<Head, Tail>: CustomStringConvertible {
  var head: Head
  var tail: Tail

  var description: String {
    "(cons \(String(reflecting: head)) \(tail))"
  }
}

@resultBuilder
enum ListBuilder {
  static func buildBlock() -> Nil {
    Nil()
  }

  static func buildPartialBlock<T>(first x: T) -> Cons<T, Nil> {
    .init(head: x, tail: Nil())
  }

  static func buildPartialBlock<New, T>(accumulated: T, next: New) -> Cons<New, T> {
    .init(head: next, tail: accumulated)
  }

  static func buildBlock<T>(_ x: T...) -> [T] {
    fatalError("I should never be called!")
  }
}

func list<T>(@ListBuilder f: () -> T) -> T {
  f()
}

let list0 = list {}
print(list0)
// CHECK: nil

let list1 = list { "1" }
print(list1)
// Check: (cons 1 nil)

let list2 = list {
  1
  2
}
print(list2)
// CHECK: (cons 2 (cons 1 nil))
let list3 = list {
  1
  list {
    2.0
    "3"
  }
  "4"
}
print(list3)
// CHECK: (cons "4" (cons (cons "3" (cons 2.0 nil)) (cons 1 nil)))

func test_callAsFunction_with_resultBuilder() {
  struct CallableTest {
    func callAsFunction<T>(@TupleBuilder _ body: (Bool) -> T) {
      print(body(true))
    }
  }

  CallableTest() {
    0
    "with parens"
    $0
  }

  CallableTest {
    1
    "without parens"
    $0
  }
}

test_callAsFunction_with_resultBuilder()
// CHECK: (0, "with parens", true)
// CHECK: (1, "without parens", true)

do {
  struct S {
    static func test<T>(@TupleBuilder _ body: (Bool) -> T) -> S {
      print(body(true))
      return .init()
    }
  }

  let _: S? = .test {
    42
    ""
    [$0]
  }
  // CHECK: (42, "", [true])
}

do {
  @resultBuilder
  struct MyBuilder {
    static func buildBlock<T1: ExpressibleByStringLiteral>(_ t1: T1) -> (T1) {
      return (t1)
    }

    static func buildBlock<T1, T2>(_ t1: T1, _ t2: T2) -> (T1, T2) {
      return (t1, t2)
    }

    static func buildOptional<T>(_ value: T?) -> T { return value! }

    static func buildEither<T>(first value: T) -> T {
      return value
    }

    static func buildEither<U>(second value: U) -> U {
      return value
    }
  }

  func test<T>(@MyBuilder _ builder: (Int) -> T) {
    print(builder(42))
  }

  test {
    if $0 < 0 {
      "\($0)"
    } else if $0 == 42 {
      "the answer"
    }
  }
  // CHECK: the answer
}

protocol TestIfSequences {
}

struct A: TestIfSequences {}
struct B: TestIfSequences {}
struct C: TestIfSequences {}
struct D: TestIfSequences {}

func testOptionalIfElseSequences() {
  func check<T>(_ v: TestIfSequences,
                @TupleBuilder body: (TestIfSequences) throws -> T) rethrows {
    print(try body(v))
  }

  check(A()) { v in
    if let a = v as? A {
      a
    } else if let b = v as? B {
      b
    } else if let c = v as? C {
      c
    }
  }

  check(B()) { v in
    if let a = v as? A {
      a
    } else if let b = v as? B {
      b
    } else if let c = v as? C {
      c
    }
  }

  check(C()) { v in
    if let a = v as? A {
      a
    } else if let b = v as? B {
      b
    } else if let c = v as? C {
      c
    }
  }

  check(D()) { v in
    if let a = v as? A {
      a
    } else if let b = v as? B {
      b
    } else if let c = v as? C {
      c
    } else {
      D()
    }
  }

  check(A()) { v in
    if let a = v as? A {
      a
    } else {
      if let b = v as? B {
        b
      }

      if let c = v as? C {
        c
      } else if let d = v as? D {
        d
      }
    }
  }

  check(B()) { v in
    if let a = v as? A {
      a
    } else {
      if let b = v as? B {
        b
      }

      if let c = v as? C {
        c
      } else if let d = v as? D {
        d
      }
    }
  }

  check(C()) { v in
    if let a = v as? A {
      a
    } else {
      if let b = v as? B {
        b
      }

      if let c = v as? C {
        c
      } else if let d = v as? D {
        d
      }
    }
  }

  check(D()) { v in
    if let a = v as? A {
      a
    } else {
      if let b = v as? B {
        b
      }

      if let c = v as? C {
        c
      } else if let d = v as? D {
        d
      }
    }
  }
}

testOptionalIfElseSequences()
// CHECK: Optional(main.Either<main.Either<main.A, main.B>, main.C>.first(main.Either<main.A, main.B>.first(main.A())))
// CHECK-NEXT: Optional(main.Either<main.Either<main.A, main.B>, main.C>.first(main.Either<main.A, main.B>.second(main.B())))
// CHECK-NEXT: Optional(main.Either<main.Either<main.A, main.B>, main.C>.second(main.C()))
// CHECK-NEXT: second(main.Either<main.C, main.D>.second(main.D()))
// CHECK-NEXT: first(main.A())
// CHECK-NEXT: second(Optional(main.B()), nil)
// CHECK-NEXT: second(nil, Optional(main.Either<main.C, main.D>.first(main.C())))
// CHECK-NEXT: second(nil, Optional(main.Either<main.C, main.D>.second(main.D())))

// rdar://106364495 - ambiguous use of `buildFinalResult`
func testBuildFinalResultDependentOnContextualType() {
  @resultBuilder
  struct MyBuilder {
    static func buildBlock(_ v: Int) -> Int { v }
    static func buildFinalResult(_ v: Int) -> Int { v }
    static func buildFinalResult(_ v: Int) -> String { "" }
  }

  func test(@MyBuilder _ fn: () -> Int?) { print(fn()) }

  test {
    42
  }
}

testBuildFinalResultDependentOnContextualType()
// CHECK: Optional(42)
