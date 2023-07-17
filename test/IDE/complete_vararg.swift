// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=TOP_LEVEL_1 | %FileCheck %s -check-prefix=TOP_LEVEL_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=OBJ_DOT_1 | %FileCheck %s -check-prefix=OBJ_DOT_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=FREE_FUNC_1 | %FileCheck %s -check-prefix=FREE_FUNC_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=FREE_FUNC_2 | %FileCheck %s -check-prefix=FREE_FUNC_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INIT_1 | %FileCheck %s -check-prefix=INIT_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METHOD_1 | %FileCheck %s -check-prefix=METHOD_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=METHOD_2 | %FileCheck %s -check-prefix=METHOD_2
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=SUBSCRIPT_1 | %FileCheck %s -check-prefix=SUBSCRIPT_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GENERIC_FREE_FUNC_1 | %FileCheck %s -check-prefix=GENERIC_FREE_FUNC_1
// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=INTERESTING_TYPE_1 | %FileCheck %s -check-prefix=INTERESTING_TYPE_1

func freeFunc1(x x: Int...) { }
func freeFunc2(x x: Int, y y: Int...) { }

class C {
  init(x: Int...) { }
  func method1(x: Int...) { }
  func method2(x: Int, y: Int...) { }
  func method4(`do` : Int...) {}
  func method5(_ `class` : Int...) {}
  func method6(`class` `protocol`: Int...) {}
  func method7(`inout` value: Int...) {}
  subscript(i: Int...) -> Int { return 0 }
}

func genericFreeFunc1<T>(t t: T...) { }
func interestingType1(x x: (Int, (Int, String))...) { }

func testTopLevel() {
  #^TOP_LEVEL_1^#
}
// TOP_LEVEL_1-DAG: Decl[FreeFunction]/CurrModule:      freeFunc1({#x: Int...#})[#Void#]{{; name=.+$}}
// TOP_LEVEL_1-DAG: Decl[FreeFunction]/CurrModule:      freeFunc2({#x: Int#}, {#y: Int...#})[#Void#]{{; name=.+$}}
// TOP_LEVEL_1-DAG: Decl[FreeFunction]/CurrModule:      genericFreeFunc1({#t: T...#})[#Void#]{{; name=.+$}}
// TOP_LEVEL_1-DAG: Decl[FreeFunction]/CurrModule:      interestingType1({#x: (Int, (Int, String))...#})[#Void#]{{; name=.+$}}

var obj = C()
func testObjDot1() {
  obj.#^OBJ_DOT_1^#
}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method1({#x: Int...#})[#Void#]{{; name=.+$}}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method2({#x: Int#}, {#y: Int...#})[#Void#]{{; name=.+$}}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method4({#do: Int...#})[#Void#]{{; name=.+$}}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method5({#(class): Int...#})[#Void#]{{; name=.+$}}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method6({#class: Int...#})[#Void#]{{; name=.+$}}
// OBJ_DOT_1-DAG: Decl[InstanceMethod]/CurrNominal:   method7({#`inout`: Int...#})[#Void#]{{; name=.+$}}

func testFreeFunc1() {
  freeFunc1(#^FREE_FUNC_1^#
}
func testFreeFunc2() {
  freeFunc2(#^FREE_FUNC_2^#
}
// FREE_FUNC_1: Begin completions, 1 items
// FREE_FUNC_1: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#x: Int...#}[')'][#Void#]{{; name=.+$}}
// FREE_FUNC_2: Begin completions, 1 items
// FREE_FUNC_2: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#x: Int#}, {#y: Int...#}[')'][#Void#]{{; name=.+$}}

func testInit() {
  let c =C(#^INIT_1^#
}
// INIT_1: Begin completions, 1 items
// INIT_1: Decl[Constructor]/CurrNominal/Flair[ArgLabels]:      ['(']{#x: Int...#}[')'][#C#]{{; name=.+$}}

func testMethod1() {
  obj.method1(#^METHOD_1^#
}
func testMethod2() {
  obj.method2(#^METHOD_2^#
}
// METHOD_1: Begin completions, 1 items
// METHOD_1: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#x: Int...#}[')'][#Void#]{{; name=.+$}}
// METHOD_2: Begin completions, 1 items
// METHOD_2: Decl[InstanceMethod]/CurrNominal/Flair[ArgLabels]: ['(']{#x: Int#}, {#y: Int...#}[')'][#Void#]{{; name=.+$}}

func testSubscript() {
  obj#^SUBSCRIPT_1^#
}
// SUBSCRIPT_1: Decl[Subscript]/CurrNominal:        [{#(i): Int...#}][#Int#]{{; name=.+$}}

func testGenericFreeFunc() {
  genericFreeFunc1(#^GENERIC_FREE_FUNC_1^#
}
// GENERIC_FREE_FUNC_1: Begin completions, 1 items
// GENERIC_FREE_FUNC_1: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#t: T...#}[')'][#Void#]{{; name=.+$}}


func testInterestingType() {
  interestingType1(#^INTERESTING_TYPE_1^#
}
// INTERESTING_TYPE_1: Begin completions, 1 items
// INTERESTING_TYPE_1: Decl[FreeFunction]/CurrModule/Flair[ArgLabels]: ['(']{#x: (Int, (Int, String))...#}[')'][#Void#]{{; name=.+$}}
