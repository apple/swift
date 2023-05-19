// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// EMTPY: Token
// EMPTY-NOT: Begin completions

//===--- Helper types that are used in this test

struct FooStruct {
  var instanceVar : Int

  func instanceFunc0() {}
}

// FOO_OBJECT_DOT: Begin completions, 3 items
// FOO_OBJECT_DOT-DAG: Keyword[self]/CurrNominal: self[#FooStruct#]; name=self
// FOO_OBJECT_DOT-DAG: Decl[InstanceVar]/CurrNominal:    instanceVar[#Int#]{{; name=.+$}}
// FOO_OBJECT_DOT-DAG: Decl[InstanceMethod]/CurrNominal{{(/TypeRelation\[Convertible\])?}}: instanceFunc0()[#Void#]{{; name=.+$}}

// WITH_GLOBAL_DECLS: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}

//===--- Check that we can resolve closure parameters.

func testResolveClosureParam1() {
  var x = { (fs: FooStruct) in fs.#^RESOLVE_CLOSURE_PARAM_1?check=FOO_OBJECT_DOT^# }
}

func testResolveClosureParam2() {
  { (fs: FooStruct) in fs.#^RESOLVE_CLOSURE_PARAM_2?check=FOO_OBJECT_DOT^# }
}

//===--- Check that we can resolve parent function parameters.

func testResolveParentParam1(_ fs: FooStruct) {
  { (a: Int) in fs.#^RESOLVE_PARENT_PARAM_1?check=FOO_OBJECT_DOT^# }
}

func testResolveParentParam2(_ fs: FooStruct) {
  { fs.#^RESOLVE_PARENT_PARAM_2?check=FOO_OBJECT_DOT^# }
}

class TestResolveParentParam3 {
  func testResolveParentParam3a(_ fs: FooStruct) {
    { (a: Int) in fs.#^RESOLVE_PARENT_PARAM_3?check=FOO_OBJECT_DOT^# }
  }
}

class TestResolveParentParam4 {
  func testResolveParentParam4a(_ fs: FooStruct) {
    { fs.#^RESOLVE_PARENT_PARAM_4?check=FOO_OBJECT_DOT^# }
  }
}

func testResolveParentParam5(_ fs: FooStruct) {
  func testResolveParentParam5a() {
    { fs.#^RESOLVE_PARENT_PARAM_5?check=FOO_OBJECT_DOT^# }
  }
}

func testResolveParentParam6() {
  func testResolveParentParam6a(_ fs: FooStruct) {
    { fs.#^RESOLVE_PARENT_PARAM_6?check=FOO_OBJECT_DOT^# }
  }
}

//===--- Test completion in various statements in closures.

func testReturnInClosure1() {
  var f = { () -> Int in
    return #^RETURN_1?check=WITH_GLOBAL_DECLS^#
  }
}

//===--- Test that we do delayed parsing of closures.

var topLevelClosure1 = { #^DELAYED_1?check=WITH_GLOBAL_DECLS^# }

var topLevelClosure2 = { func f() { #^DELAYED_2?check=WITH_GLOBAL_DECLS^# } }

var topLevelClosure3 = { class C { func f() { #^DELAYED_3?check=WITH_GLOBAL_DECLS^# } } }

class ClassWithClosureMember1 {
  var c1 = { #^DELAYED_4?check=WITH_GLOBAL_DECLS^# }
  lazy var c2 = { #^DELAYED_5?check=WITH_GLOBAL_DECLS^# }
  var c3 = ({ #^DELAYED_6?check=WITH_GLOBAL_DECLS^# })()
  lazy var c4 = ({ #^DELAYED_7?check=WITH_GLOBAL_DECLS^# })()
}

struct NestedStructWithClosureMember1 {
  struct Nested {
    var c1 = { #^DELAYED_8?check=WITH_GLOBAL_DECLS^# }
    lazy var c2 = { #^DELAYED_9?check=WITH_GLOBAL_DECLS^# }
  }
}

// WITH_GLOBAL_DECLS_AND_LOCAL1: Decl[LocalVar]/Local: x[#Int#]
// WITH_GLOBAL_DECLS_AND_LOCAL1: Decl[Struct]/CurrModule: FooStruct[#FooStruct#]{{; name=.+$}}

struct StructWithClosureMemberAndLocal {
  var c = {
    var x = 0
    #^DELAYED_10?check=WITH_GLOBAL_DECLS_AND_LOCAL1^#
  }
}

func acceptsTrailingClosureFooVoid(_ code: (FooStruct) -> Void) {}

acceptsTrailingClosureFooVoid {
  #^IN_TRAILING_CLOSURE_1?check=WITH_GLOBAL_DECLS^#
}

acceptsTrailingClosureFooVoid {
  $0.#^IN_TRAILING_CLOSURE_2?check=FOO_OBJECT_DOT^#
}

acceptsTrailingClosureFooVoid {
  item in #^IN_TRAILING_CLOSURE_3?check=WITH_GLOBAL_DECLS^#
}

acceptsTrailingClosureFooVoid {
  item in item.#^IN_TRAILING_CLOSURE_4?check=FOO_OBJECT_DOT^#
}

acceptsTrailingClosureFooVoid {
  item in
  item.instanceFunc0()
  item.#^IN_TRAILING_CLOSURE_5?check=FOO_OBJECT_DOT^#
}

func acceptsListAndTrailingClosureFooVoid(
    _ list: [FooStruct], code: (FooStruct) -> Void) {
}

acceptsListAndTrailingClosureFooVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  #^IN_TRAILING_CLOSURE_6?check=WITH_GLOBAL_DECLS^#
}

acceptsListAndTrailingClosureFooVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  $0.#^IN_TRAILING_CLOSURE_7?check=FOO_OBJECT_DOT^#
}

acceptsListAndTrailingClosureFooVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in #^IN_TRAILING_CLOSURE_8?check=WITH_GLOBAL_DECLS^#
}

acceptsListAndTrailingClosureFooVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in item.#^IN_TRAILING_CLOSURE_9?check=FOO_OBJECT_DOT^#
}

acceptsListAndTrailingClosureFooVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in
  item.instanceFunc0()
  item.#^IN_TRAILING_CLOSURE_10?check=FOO_OBJECT_DOT^#
}

func acceptsListAndTrailingClosureTVoid<T>(_ list: [T], code: (T) -> Void) {}

acceptsListAndTrailingClosureTVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  #^IN_TRAILING_CLOSURE_11?check=WITH_GLOBAL_DECLS^#
}

acceptsListAndTrailingClosureTVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  $0.#^IN_TRAILING_CLOSURE_12?check=FOO_OBJECT_DOT^#
}

acceptsListAndTrailingClosureTVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in #^IN_TRAILING_CLOSURE_13?check=WITH_GLOBAL_DECLS^#
}

acceptsListAndTrailingClosureTVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in item.#^IN_TRAILING_CLOSURE_14?check=FOO_OBJECT_DOT^#
}

acceptsListAndTrailingClosureTVoid(
    [ FooStruct(instanceVar: 0), FooStruct(instanceVar: 0) ]) {
  item in
  item.instanceFunc0()
  item.#^IN_TRAILING_CLOSURE_15?check=FOO_OBJECT_DOT^#
}

func getInt() -> Int? { return 0 }
func testAcceptsTrailingClosureInt1() {
  acceptsTrailingClosureFooVoid { #^IN_TRAILING_CLOSURE_16?check=EMPTY^# in
    if let myvar = getInt() {
    }
  }
}
func testAcceptsTrailingClosureInt2() {
  acceptsTrailingClosureFooVoid {
    #^IN_TRAILING_CLOSURE_17?check=WITH_GLOBAL_DECLS^#
    if let myvar = getInt() {
    }
  }
}
func testAcceptsTrailingClosureInt3() {
  acceptsTrailingClosureFooVoid {
    if let myvar = getInt() {
    }
    #^IN_TRAILING_CLOSURE_18?check=WITH_GLOBAL_DECLS^#
  }
}
func testAcceptsTrailingClosureInt4() {
  acceptsTrailingClosureFooVoid {
    if let myvar = getInt() {
      #^IN_TRAILING_CLOSURE_19?check=WITH_GLOBAL_DECLS^#
    }
  }
}

func testTypeInClosure1() {
  acceptsTrailingClosureFooVoid {
    struct S : #^STRUCT_INHERITANCE_IN_CLOSURE_0?check=WITH_GLOBAL_DECLS^#
  }
}
func testTypeInClosure2() {
  acceptsTrailingClosureFooVoid {
    class S : #^CLASS_INHERITANCE_IN_CLOSURE_0?check=WITH_GLOBAL_DECLS^#
  }
}
func testTypeInClosure3() {
  acceptsTrailingClosureFooVoid {
    func test(_ x: #^ARGUMENT_TYPE_IN_CLOSURE_0?check=WITH_GLOBAL_DECLS^#
  }
}
acceptsTrailingClosureFooVoid {
  struct S : #^STRUCT_INHERITANCE_IN_CLOSURE_1?check=WITH_GLOBAL_DECLS^#
}
acceptsTrailingClosureFooVoid {
  class S : #^CLASS_INHERITANCE_IN_CLOSURE_1?check=WITH_GLOBAL_DECLS^#
}
acceptsTrailingClosureFooVoid {
  func test(_ x: #^ARGUMENT_TYPE_IN_CLOSURE_1?check=WITH_GLOBAL_DECLS^#
}

struct LazyVar1 {
  lazy var x: Int = {
    struct S : #^STRUCT_INHERITANCE_IN_CLOSURE_2?check=WITH_GLOBAL_DECLS^#
  }()
}
struct LazyVar2 {
  lazy var x: Int = {
    class S : #^CLASS_INHERITANCE_IN_CLOSURE_2?check=WITH_GLOBAL_DECLS^#
  }()
}
struct LazyVar3 {
  lazy var x: Int = {
    func test(_ x: #^ARGUMENT_TYPE_IN_CLOSURE_2?check=WITH_GLOBAL_DECLS^#
  }()
}

func closureTaker(_ theFunc:(theValue: Int) -> ()) {}
func closureTaker2(_ theFunc: (Value1: Int, Value2: Int) -> ()) {}
func testClosureParam1() {
  closureTaker { (theValue) -> () in
    #^CLOSURE_PARAM_1^#
  }
}
// CLOSURE_PARAM_1-DAG: Decl[LocalVar]/Local:         theValue[#Int#]{{; name=.+$}}
func testClosureParam2() {
  closureTaker2 { (Value1, Value2) -> () in
    #^CLOSURE_PARAM_2^#
  }
}
// CLOSURE_PARAM_2-DAG: Decl[LocalVar]/Local:         Value1[#Int#]{{; name=.+$}}
// CLOSURE_PARAM_2-DAG: Decl[LocalVar]/Local:         Value2[#Int#]{{; name=.+$}}

enum SomeEnum {
  case north, south
}

struct BarStruct {
  var enumVal: SomeEnum = .north
}

var testIIFEVar: BarStruct = {
  var obj = BarStruct()
  obj.enumVal = .#^IN_IIFE_1^#
  return obj
}()
testIIFEVar = {
  var obj = BarStruct()
  obj.enumVal = .#^IN_IIFE_2?check=IN_IIFE_1^#
  return obj
}()

func testIIFE() {
  var testIIFEVar: FooStruct = {
    var obj = BarStruct()
    obj.enumVal = .#^IN_IIFE_3?check=IN_IIFE_1^#
    return obj
  }()
  testIIFEVar = {
    var obj = BarStruct()
    obj.enumVal = .#^IN_IIFE_4?check=IN_IIFE_1^#
    return obj
  }()
}
// IN_IIFE_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: north[#SomeEnum#]
// IN_IIFE_1-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: south[#SomeEnum#]

extension Error {
  var myErrorNumber: Int { return 0 }
}

class C {
  var foo: String = {
    do {
    } catch {
      error.#^ERROR_IN_CLOSURE_IN_INITIALIZER^#
// ERROR_IN_CLOSURE_IN_INITIALIZER-DAG: Keyword[self]/CurrNominal:          self[#any Error#]; name=self
// ERROR_IN_CLOSURE_IN_INITIALIZER-DAG: Decl[InstanceVar]/CurrNominal:      myErrorNumber[#Int#]; name=myErrorNumber
    }
    return ""
  }()
}

var foo = {
  let x = "Siesta:\(3)".#^DECL_IN_CLOSURE_IN_TOPLEVEL_INIT^#
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Keyword[self]/CurrNominal:          self[#String#]; name=self
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: count[#Int#]; name=count
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: unicodeScalars[#String.UnicodeScalarView#]; name=unicodeScalars
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: hasPrefix({#(prefix): String#})[#Bool#]; name=hasPrefix(:)
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: utf16[#String.UTF16View#]; name=utf16
  // DECL_IN_CLOSURE_IN_TOPLEVEL_INIT-DAG: Decl[InstanceMethod]/Super/IsSystem: dropFirst()[#Substring#]; name=dropFirst()
}

func testWithMemoryRebound(_ bar: UnsafePointer<UInt64>) {
    _ = bar.withMemoryRebound(to: Int64.self, capacity: 3) { ptr in
        return ptr #^SINGLE_EXPR_CLOSURE_CONTEXT^#
        // SINGLE_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: .deallocate()[#Void#]; name=deallocate()
        // SINGLE_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem:    .pointee[#Int64#]; name=pointee
    }
}

func testInsideTernaryClosureReturn(test: Bool) -> [String] {
    return "hello".map { thing in
        test ? String(thing #^SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT^#).uppercased() : String(thing).lowercased()
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .utf8[#Character.UTF8View#]; name=utf8
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .description[#String#]; name=description
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: .isWhitespace[#Bool#]; name=isWhitespace
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: .uppercased()[#String#]; name=uppercased()
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']... {#String.Element#}[#ClosedRange<String.Element>#]; name=...
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']< {#Character#}[#Bool#]; name=<
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']>= {#String.Element#}[#Bool#]; name=>=
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem: [' ']== {#Character#}[#Bool#]; name===
        // SINGLE_TERNARY_EXPR_CLOSURE_CONTEXT-DAG: Keyword[self]/CurrNominal:          .self[#String.Element#]; name=self
    }
}

func testSignature() {
    func accept<T>(_: () -> T) {}

    accept { #^PARAM_BARE_1?check=EMPTY^# in }
    accept { #^PARAM_BARE_2?check=EMPTY^#, arg2 in }
    accept { arg1, #^PARAM_BARE_3?check=EMPTY^# in }

    accept { (#^PARAM_PAREN_1?check=EMPTY^#) in }
    accept { (#^PARAM_PAREN_2?check=EMPTY^#, arg2) in }
    accept { (arg1, #^PARAM_PAREN_3?check=EMPTY^#) in }

    accept { (arg1: #^PARAMTYPE_1?check=WITH_GLOBAL_DECLS^#) in }
    accept { (arg1: Int, arg2: #^PARAMTYPE_2?check=WITH_GLOBAL_DECLS^#) in }

    accept { [#^CAPTURE_1?check=WITH_GLOBAL_DECLS^#] in }
    accept { [weak #^CAPTURE_2?check=WITH_GLOBAL_DECLS^#] in }
    accept { [#^CAPTURE_3?check=EMPTY^# = capture] in }
    accept { [weak #^CAPTURE_4?check=EMPTY^# = capture] in }

    accept { () -> #^RESULTTYPE_1?check=WITH_GLOBAL_DECLS^# in }
    accept { arg1, arg2 -> #^RESULTTYPE_2?check=WITH_GLOBAL_DECLS^# in }

    // NOTE: For effects specifiers completion (e.g. '() <HERE> -> Void') see test/IDE/complete_concurrency_specifier.swift
}

func testClosureInPatternBindingInit() {
  enum DragState {
    case dragging(translation: Int, predictedLocation: Int)
  }

  func pnChanged(_ action: () -> Void) {}

  func foo() {
    var gestureViewState: DragState = .dragging(translation: 0, predictedLocation: 0)
    let longPressDrag = pnChanged {
      _ = 1
      gestureViewState = .dragging(translation: 0, #^CLOSURE_IN_PATTERN_BINDING^#predictedLocation: 0)
    }
  }
  // CLOSURE_IN_PATTERN_BINDING: Begin completions, 1 items
  // CLOSURE_IN_PATTERN_BINDING-DAG: Pattern/Local/Flair[ArgLabels]:     {#predictedLocation: Int#}[#Int#];
  // CLOSURE_IN_PATTERN_BINDING: End completion

}

func testSwitchInClosure() {
  func executeClosure(closure: () -> Void) {}

  struct Boredom {
    static func doNothing() {}
  }

  enum MyEnum {
    case first
    case second
  }

  let item: MyEnum? = nil
  executeClosure(closure: {
    switch item {
    case .#^SWITCH_IN_CLOSURE^#first:
      break
    case .second:
      Boredom.doNothing()
    }
  })

// SWITCH_IN_CLOSURE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: first[#MyEnum#];
// SWITCH_IN_CLOSURE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: second[#MyEnum#];
}

func testSwitchWithAssociatedValueInClosure() {
  func executeClosure(closure: () -> Void) {}

  struct Boredom {
    static func doNothing() {}
  }

  enum MyEnum {
    case first(String)
  }

  let item: MyEnum? = nil
  executeClosure(closure: {
    switch item {
    case .#^SWITCH_WITH_ASSOC_IN_CLOSURE^#first(_):
      break
    }
  })

// SWITCH_WITH_ASSOC_IN_CLOSURE-DAG: Decl[EnumElement]/CurrNominal/Flair[ExprSpecific]/TypeRelation[Convertible]: first({#String#})[#MyEnum#];
}

func testCompleteInMatchOfAssociatedValueInSwitchCase() {
  func testSwitchWithAssociatedValueInClosure() {
  func executeClosure(closure: () -> Void) {}

  struct Boredom {
    static func doNothing() {}
  }

  enum MyEnum {
    case first(Bool, String)
  }

  let item: MyEnum? = nil
  let str = "hi"
  executeClosure(closure: {
    switch item {
    case .first(true, #^IN_ASSOC_OF_CASE_IN_CLOSURE^#str):
      break
    }
  })

// IN_ASSOC_OF_CASE_IN_CLOSURE-DAG: Decl[LocalVar]/Local:               str[#String#]; name=str
}

}

func testReferenceToVariableDefinedInClosure() {
  func takeClosure(_ x: () -> Void) {}

  takeClosure {
    let item = "\(1)"
    #^VARIABLE_DEFINED_IN_CLOSURE^#
  }
  // VARIABLE_DEFINED_IN_CLOSURE: Decl[LocalVar]/Local:               item[#String#]; name=item
}

