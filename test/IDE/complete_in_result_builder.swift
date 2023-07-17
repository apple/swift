// RUN %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

@resultBuilder
struct TupleBuilder<T> {
  static func buildBlock() -> () { }

  static func buildBlock(_ t1: T) -> T {
    return t1
  }

  static func buildBlock(_ t1: T, _ t2: T) -> (T, T) {
    return (t1, t2)
  }
  static func buildBlock(_ t1: T, _ t2: T, _ t3: T) -> (T, T, T) {
    return (t1, t2, t3)
  }
}

func buildStringTuple<Result>(@TupleBuilder<String> x: () -> Result) {}

enum StringFactory {
  static func makeString(x: String) -> String { return x }
}

enum Letters {
  case a
  case b
  case c
}

let MyConstantString = "MyConstant"
let MyConstantBool = true

func testGlobalLookup() {
  @TupleBuilder<String> var x1 {
    #^GLOBAL_LOOKUP^#
    // GLOBAL_LOOKUP: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]:         MyConstantString[#String#]; name=MyConstantString
  }

  @TupleBuilder<String> var x2 {
    if true {
      #^GLOBAL_LOOKUP_IN_IF_BODY^#
// GLOBAL_LOOKUP_IN_IF_BODY: Decl[GlobalVar]/CurrModule:         MyConstantString[#String#];
    }
  }

  @TupleBuilder<String> var x3 {
    if {
      #^GLOBAL_LOOKUP_IN_IF_BODY_WITHOUT_CONDITION?check=GLOBAL_LOOKUP_IN_IF_BODY^#
    }
  }

  @TupleBuilder<String> var x4 {
    guard else {
      #^GLOBAL_LOOKUP_IN_GUARD_BODY_WITHOUT_CONDITION?check=GLOBAL_LOOKUP_IN_IF_BODY^#
    }
  }

  @TupleBuilder<String> var x5 {
    "hello: \(#^GLOBAL_LOOKUP_IN_STRING_LITERAL^#)"
// GLOBAL_LOOKUP_IN_STRING_LITERAL: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: MyConstantString[#String#];
  }

  @TupleBuilder<String> var x5 {
    if #^GLOBAL_LOOKUP_IN_IF_CONDITION^# {
// GLOBAL_LOOKUP_IN_IF_CONDITION: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: MyConstantBool[#Bool#]; name=MyConstantBool
    }
  }
}

func testStaticMemberLookup() {
  @TupleBuilder<String> var x1 {
    StringFactory.#^COMPLETE_STATIC_MEMBER^#
    // COMPLETE_STATIC_MEMBER: Keyword[self]/CurrNominal:          self[#StringFactory.Type#]; name=self
    // COMPLETE_STATIC_MEMBER: Keyword/CurrNominal:                Type[#StringFactory.Type#]; name=Type
    // COMPLETE_STATIC_MEMBER: Decl[StaticMethod]/CurrNominal/TypeRelation[Convertible]:     makeString({#x: String#})[#String#];
  }

  @TupleBuilder<String> var x2 {
    if true {
      StringFactory.#^COMPLETE_STATIC_MEMBER_IN_IF_BODY^#
// COMPLETE_STATIC_MEMBER_IN_IF_BODY: Decl[StaticMethod]/CurrNominal:     makeString({#x: String#})[#String#];
    }
  }

  @TupleBuilder<String> var x3 {
    "hello: \(StringFactory.#^COMPLETE_STATIC_MEMBER_IN_STRING_LITERAL^#)"
// COMPLETE_STATIC_MEMBER_IN_STRING_LITERAL: Begin completions
// COMPLETE_STATIC_MEMBER_IN_STRING_LITERAL: Decl[StaticMethod]/CurrNominal/TypeRelation[Convertible]:     makeString({#x: String#})[#String#];
// COMPLETE_STATIC_MEMBER_IN_STRING_LITERAL: End completions
  }
}

struct FooStruct {
  var instanceVar : Int
  init(_: Int = 0) { }
  func boolGen() -> Bool { return false }
  func intGen() -> Int { return 1 }
}

func testPatternMatching() {
  @TupleBuilder<String> var x1 {
    let x = Letters.b
    if case .#^COMPLETE_PATTERN_MATCHING_IN_IF?check=COMPLETE_CASE^# = x {
// COMPLETE_CASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: a[#Letters#];
// COMPLETE_CASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: b[#Letters#];
// COMPLETE_CASE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: c[#Letters#];
    }
  }

  @TupleBuilder<String> var x2 {
    let x = Letters.a
    switch x {
    case .#^COMPLETE_CASE_IN_SWITCH?check=COMPLETE_CASE^#:
      break
    }
  }

  @TupleBuilder<String> var x3 {
    let x: FooStruct? = FooStruct()
    guard case .#^GUARD_CASE_PATTERN_1?check=OPTIONAL_FOOSTRUCT^# = x {}
    // OPTIONAL_FOOSTRUCT: Begin completions, 2 items
    // OPTIONAL_FOOSTRUCT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: none[#Optional<FooStruct>#]; name=none
    // OPTIONAL_FOOSTRUCT-DAG: Decl[EnumElement]/CurrNominal/IsSystem/TypeRelation[Convertible]: some({#FooStruct#})[#Optional<FooStruct>#]; name=some()
  }

  @TupleBuilder<String> var x4 {
    let x: FooStruct? = FooStruct()
    guard case .#^GUARD_CASE_PATTERN_2?check=OPTIONAL_FOOSTRUCT^#some() = x {}
  }
}

func testCompleteFunctionArgumentLabels() {
  @TupleBuilder<String> var x1 {
    StringFactory.makeString(#^FUNCTION_ARGUMENT_LABEL^#)
    // FUNCTION_ARGUMENT_LABEL: Begin completions, 1 item
    // FUNCTION_ARGUMENT_LABEL: Decl[StaticMethod]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#x: String#}[')'][#String#];
  }
}

func testCompleteFunctionArgument() {
  @TupleBuilder<String> var x1 {
    StringFactory.makeString(x: #^ARGUMENT_LOOKUP^#)
    // ARGUMENT_LOOKUP: Decl[GlobalVar]/CurrModule/TypeRelation[Convertible]: MyConstantString[#String#];
  }

  @TupleBuilder<String> var x2 {
    if true {
      StringFactory.makeString(x: #^ARGUMENT_LOOKUP_IN_IF_BODY?check=ARGUMENT_LOOKUP^#)
    }
  }
}

func testCompleteErrorTypeInCatch() {
  enum Error4 : Error {
    case E1
    case E2(Int32)
  }
  @TupleBuilder<String> var x1 {
    do {} catch Error4.#^CATCH2^#
  }
// CATCH2-DAG: Decl[EnumElement]/CurrNominal: E1[#Error4#]; name=E1
// CATCH2-DAG: Decl[EnumElement]/CurrNominal: E2({#Int32#})[#Error4#]; name=E2()
}

func testCompleteInStringLiteral() {
  struct Island {
    var turnipPrice: String
  }

  func takeTrailingClosure(_ x: () -> Void) -> Text { fatalError() }

  struct BStack<Content> {
    init(@ViewBuilder content: () -> Content) {}
  }

  protocol View {}

  struct Text: View {
    init(_ x: String) {}

    var body: Never { fatalError() }
  }

  @resultBuilder struct ViewBuilder {
    static func buildBlock() -> Text { fatalError() }
    static func buildBlock<C: View>(_ c: C) -> C { return c }
    static func buildBlock<C1: View, C2: View>(_ c: C1, _ d: C2) -> C1 { return c }
  }


  func foo(island: Island) {
    BStack {
      let b = "\(island.#^STRING_LITERAL_VAR^#turnipPrice)"
      takeTrailingClosure {}
    }
// STRING_LITERAL_VAR: Begin completions, 2 items
// STRING_LITERAL_VAR-DAG: Keyword[self]/CurrNominal:          self[#Island#]; name=self
// STRING_LITERAL_VAR-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]: turnipPrice[#String#]; name=turnipPrice
  }

  func bar(island: Island) {
    BStack {
      Text("\(island.#^STRING_LITERAL_AS_ARGUMENT?check=STRING_LITERAL_VAR^#turnipPrice)")
      takeTrailingClosure {}
    } 
  }
}

func testTypeRelationInResultBuilder() {
  protocol View2 {}

  @resultBuilder public struct ViewBuilder2 {
    static func buildBlock<Content>(_ content: Content) -> Content where Content : View2 { fatalError() }
    static func buildBlock<C0, C1>(_ c0: C0, _ c1: C1) -> C0 where C0 : View2, C1: View2 { fatalError() }
  }

  struct MyText: View2 {}

  struct MyView {
    @ViewBuilder2 var body: some View2 {
      #^SINGLE_ELEMENT^#
    }
    // SINGLE_ELEMENT-DAG: Decl[Struct]/Local/TypeRelation[Convertible]: MyText[#MyText#];

    @ViewBuilder2 var body2: some View2 {
      MyText()
      #^SECOND_ELEMENT?check=SINGLE_ELEMENT^#
    }
  }
}

func testAmbiguousInResultBuilder() {
  @resultBuilder
  struct MyViewBuilder {
    static func buildBlock(_ x: Int) -> Int { return x }
  }

  struct QStack {
    init(@MyViewBuilder content: () -> Int) {}
  }

  struct Foo {
    func qtroke(_ content: Int, style: Int) -> Int { return 1 }
    func qtroke(_ content: Int, lineWidth: Int = 1) -> Int { return 2 }
  }

  QStack {
    Foo().qtroke(0, #^AMBIGUOUS_IN_RESULT_BUILDER^#)
// AMBIGUOUS_IN_RESULT_BUILDER: Begin completions, 2 items
// AMBIGUOUS_IN_RESULT_BUILDER-DAG: Pattern/Local/Flair[ArgLabels]:     {#style: Int#}[#Int#];
// AMBIGUOUS_IN_RESULT_BUILDER-DAG: Pattern/Local/Flair[ArgLabels]:     {#lineWidth: Int#}[#Int#];
  }
}

func testCompleteGlobalInResultBuilderIf() {
  func buildView(@ViewBuilder2 content: () -> MyView) {}

  @resultBuilder public struct ViewBuilder2 {
    static func buildBlock() -> MyView { fatalError() }
    static func buildBlock(_ content: MyView) -> MyView { fatalError() }
    static func buildIf(_ content: MyView?) -> MyView? { fatalError() }
    static func buildEither(first: MyView) -> MyView { fatalError() }
    static func buildEither(second: MyView) -> MyView { fatalError() }
  }

  struct MyView {}

  func test() {
    buildView {
      if true {
        MyView()
      } else {
        #^GLOBAL_IN_RESULT_BUILDER_IF^#
      }
    }
  }

  // GLOBAL_IN_RESULT_BUILDER_IF-DAG: Decl[Struct]/Local/TypeRelation[Convertible]: MyView[#MyView#]; name=MyView
}

func testInStringLiteralInResultBuilder() {
  func buildResult<Content>(@MyResultBuilder content: () -> Content) {}

  @resultBuilder
  struct MyResultBuilder {
    static func buildBlock(_ components: String) -> String {
      components
    }
  }

  struct Foo {
    var bar: Int
  }

  func withClosure(_ x: () -> Bool) -> String { return "" }

  func test(foo: Foo) {
    buildResult {
      "\(withClosure { foo.#^IN_STRING_LITERAL_IN_RESULT_BUILDER^# })"
    }
  }
// IN_STRING_LITERAL_IN_RESULT_BUILDER: Begin completions, 2 items
// IN_STRING_LITERAL_IN_RESULT_BUILDER-DAG: Keyword[self]/CurrNominal:          self[#Foo#]; name=self
// IN_STRING_LITERAL_IN_RESULT_BUILDER-DAG: Decl[InstanceVar]/CurrNominal:      bar[#Int#]; name=bar
}

func testSwitchInResultBuilder() {
  @resultBuilder
  enum ReducerBuilder2<Action> {
    static func buildBlock(_ r: Reduce2<Action>) -> Reduce2<Action> { r }
    static func buildBlock(_ r0: Reduce2<Action>, _ r1: Reduce2<Action>) -> Reduce2<Action> { r0 }
    static func buildExpression(_ r: Reduce2<Action>) -> Reduce2<Action> { r }
  }

  enum Action {
    case alertDismissed
  }

  struct Reduce2<Action> {
    init() {}

    init(_ reduce: (Action) -> Int) {}
  }

  struct Login2 {
    @ReducerBuilder2<Action>
    var body: Reduce2<Action> {
      Reduce2()
      Reduce2 { action in
        switch action {
        case .#^SWITCH_IN_RESULT_BUILDER?xfail=rdar106720462^# alertDismissed:
          return 0
        }
      }
    }
  }
// SWITCH_IN_RESULT_BUILDER: Begin completions, 1 item
// SWITCH_IN_RESULT_BUILDER-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: alertDismissed[#Action#];
}

func testCompleteIfLetInResultBuilder() {
  func takeClosure(_ x: () -> Void) -> Int {
    return 0
  }

  @resultBuilder struct MyResultBuilder {
    static func buildBlock() -> Int { return 0 }
    static func buildBlock(_ content: Int) -> Int { content }
  }

  @MyResultBuilder func test(integer: Int?) -> Int {
    takeClosure {
      if let #^IF_LET_IN_RESULT_BUILDER^#integer = integer {
      }
    }
    // IF_LET_IN_RESULT_BUILDER: Begin completions, 1 items
    // IF_LET_IN_RESULT_BUILDER: Decl[LocalVar]/Local:               integer[#Int?#]; name=integer
    // IF_LET_IN_RESULT_BUILDER: End completions
  }
}

func testOverloadedCallArgs() {
  func overloaded(single: Int) -> Int {}
  func overloaded(_ first: Int, second: Int) -> Int {}

  @resultBuilder struct ViewBuilder {
    static func buildBlock(_ content: Int) -> Int { content }
  }

  struct Test {
    @ViewBuilder var body: Int {
      overloaded(#^OVERLOADED_CALL_ARG^#, second: 1)
      // OVERLOADED_CALL_ARG: Begin completions
      // OVERLOADED_CALL_ARG-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#single: Int#}[')'][#Int#];
      // OVERLOADED_CALL_ARG-DAG: Decl[FreeFunction]/Local/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#(first): Int#}, {#second: Int#}[')'][#Int#];
      // OVERLOADED_CALL_ARG-DAG: Literal[Integer]/None/TypeRelation[Convertible]: 0[#Int#];
      // OVERLOADED_CALL_ARG: End completions
    }
  }

}
