// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t -code-complete-call-pattern-heuristics -disable-objc-attr-requires-foundation-module

struct FooStruct {
  init() {}
  init(a: Int) {}
  init(a: Int, b: Float) {}
  mutating func instanceFunc2(_ a: Int, b: inout Double) {}
}

func testInsideFunctionCall_1(_ x: inout FooStruct) {
  x.instanceFunc(#^BEFORE_COMMA^#,
// BEFORE_COMMA-NOT: Pattern/{{.*}}:{{.*}}({{.*}}{#Int#}
// BOFORE_COMMA-NOT: Decl[InstanceMethod]/{{.*}}:{{.*}}({{.*}}{#Int#}
}
func testInsideFunctionCall_2(_ x: inout FooStruct) {
  x.instanceFunc(#^BEFORE_PLACEHOLDER^#<#placeholder#>
// BEFORE_PLACEHOLDER-NOT: Pattern/{{.*}}:{{.*}}({{.*}}{#Int#}
// BOFORE_PLACEHOLDER-NOT: Decl[InstanceMethod]/{{.*}}:{{.*}}({{.*}}{#Int#}
}
func testConstructor() {
  FooStruct(#^CONSTRUCTOR^#,
// CONSTRUCTOR-NOT: Pattern/{{.*}}
// CONSTRUCTOR-NOT: Decl[Constructor]
// CONSTRUCTOR: Pattern/Local/Flair[ArgLabels]: {#a: Int#}[#Int#]
// CONSTRUCTOR-NOT: Pattern/{{.*}}
// CONSTRUCTOR-NOT: Decl[Constructor]
}

func firstArg(arg1 arg1: Int, arg2: Int) {}
func testArg2Name3() {
  firstArg(#^LABELED_FIRSTARG^#,
// LABELED_FIRSTARG-NOT: ['(']{#arg1: Int#}, {#arg2: Int#}[')'][#Void#];
// LABELED_FIRSTARG-DAG: Pattern/Local/Flair[ArgLabels]: {#arg1: Int#}[#Int#];
// LABELED_FIRSTARG-NOT: ['(']{#arg1: Int#}, {#arg2: Int#}[')'][#Void#];
}

func optionalClosure(optClosure: ((Int) -> Void)?, someArg: Int) {
  optClosure?(#^OPTIONAL_CLOSURE^#someArg)
  // OPTIONAL_CLOSURE-DAG: Decl[LocalVar]/Local/TypeRelation[Convertible]: someArg[#Int#]; name=someArg
}

func optionalProtocolMethod() {
  @objc protocol Foo {
    @objc optional func foo(arg: Int)
  }

  func test(foo: Foo) {
    foo.foo?(#^OPTIONAL_PROTOCOL_METHOD^#)
    // OPTIONAL_PROTOCOL_METHOD-DAG: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#arg: Int#}[')'][#Void#];
  }
}

func subscriptAccess(info: [String: Int]) {
  info[#^SUBSCRIPT_ACCESS^#]
// SUBSCRIPT_ACCESS: Pattern/Local/Flair[ArgLabels]:     {#keyPath: KeyPath<[String : Int], Value>#}[#KeyPath<[String : Int], Value>#]; name=keyPath:
}

struct StaticMethods {
  static func before() {
      self.after(num)#^AFTER_STATIC_FUNC^#
  }
  static func after(_ num: Int) -> (() -> Int) {}
// AFTER_STATIC_FUNC: Begin completions, 2 items
// AFTER_STATIC_FUNC-DAG: Keyword[self]/CurrNominal:          .self[#(() -> Int)#];
// AFTER_STATIC_FUNC-DAG: Pattern/CurrModule/Flair[ArgLabels]: ()[#Int#];
// AFTER_STATIC_FUNC: End completions
}

struct AmbiguousInResultBuilder {
  @resultBuilder
  struct MyViewBuilder {
    static func buildBlock(_ elt: Text) -> Int {
      53
    }
  }

  struct Text {
    init(verbatim content: String) {}
    init<S>(_ content: S) where S : StringProtocol {}
  }

  func foo(@MyViewBuilder content: () -> Int) {}

  func test(myStr: String) {
    foo {
      Text(#^AMBIGUOUS_IN_RESULT_BUILDER?xfail=TODO^#)
// AMBIGUOUS_IN_RESULT_BUILDER: Begin completions
// AMBIGUOUS_IN_RESULT_BUILDER-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#verbatim: String#}[')'][#Text#]; name=verbatim:
// AMBIGUOUS_IN_RESULT_BUILDER-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]: ['(']{#(content): _#}[')'][#Text#]; name=:
// AMBIGUOUS_IN_RESULT_BUILDER-DAG: Decl[LocalVar]/Local/TypeRelation[Convertible]: myStr[#String#]; name=myStr
// AMBIGUOUS_IN_RESULT_BUILDER: End completions
    }
  }
}
