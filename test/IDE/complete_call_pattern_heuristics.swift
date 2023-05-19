// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t -code-complete-call-pattern-heuristics

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

func subscriptAccess(info: [String: Int]) {
  info[#^SUBSCRIPT_ACCESS^#]
// SUBSCRIPT_ACCESS: Pattern/Local/Flair[ArgLabels]:     {#keyPath: KeyPath<[String : Int], Value>#}[#KeyPath<[String : Int], Value>#]; name=keyPath:
}
