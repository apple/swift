// RUN: %target-swift-ide-test -code-completion -source-filename %s -code-completion-token=GLOBALFUNC_SAMELINE | %FileCheck %s -check-prefix=GLOBALFUNC_SAMELINE

func func1(
  fn1: () -> Int,
  fn2: () -> Void = {},
  fn3: (Int) -> Void = {_ in},
  fn4: (Int, String) -> Void = {_,_ in},
  fn5: (Int, String) -> Int = {_,_ in 1},
  fn6: (_ a: Int, _ b: String) -> Int = {_,_ in 1},
  fn7: (inout Int) -> Void = {_ in},
  fn8: (Int...) -> Void = { (_:Int...) in})
{}

func test() {
  func1()
    { 1 } #^GLOBALFUNC_SAMELINE^#

// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn2:  () -> Void {|}#}[#() -> Void#];
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn3:  (Int) -> Void {<#Int#> in|}#}[#(Int) -> Void#];
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn4:  (Int, String) -> Void {<#Int#>, <#String#> in|}#}[#(Int, String) -> Void#];
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn5:  (Int, String) -> Int {<#Int#>, <#String#> in|}#}[#(Int, String) -> Int#];
// FIXME: recover names
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn6:  (Int, String) -> Int {a, b in|}#}[#(Int, String) -> Int#];
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn7:  (inout Int) -> Void {<#inout Int#> in|}#}[#(inout Int) -> Void#];
// GLOBALFUNC_SAMELINE-DAG: Pattern/Local/Flair[ArgLabels]: {#fn8:  (Int...) -> Void {<#Int...#> in|}#}[#(Int...) -> Void#];
}

func testStringAndMulipleTrailingClosures() {
  func stringAndClosure(_ key: String, _ body: () -> Void) {}

  func takeClosure(_ x: () -> Void) {}

  takeClosure {
    stringAndClosure("\(1)") { }#^STRING_AND_MULTIPLE_TRAILING_CLOSURES^#
  }
  // STRING_AND_MULTIPLE_TRAILING_CLOSURES: Begin completions
  // STRING_AND_MULTIPLE_TRAILING_CLOSURES-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#()#}[#Bool#];
  // STRING_AND_MULTIPLE_TRAILING_CLOSURES-DAG: Keyword[self]/CurrNominal:          .self[#Void#]; name=self
  // STRING_AND_MULTIPLE_TRAILING_CLOSURES: End completions
}
