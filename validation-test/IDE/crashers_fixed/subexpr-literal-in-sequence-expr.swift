// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename=%s -filecheck %raw-FileCheck -completion-output-dir %t

func test1() {
  1 + [0]#^A^#
// A: Decl[InstanceVar]/CurrNominal/IsSystem/TypeRelation[Convertible]:      .startIndex[#Int#]; name=startIndex
}
func test2() {
  "" + [""]#^B^#
// B: Decl[InstanceVar]/CurrNominal/IsSystem:      .startIndex[#Int#]; name=startIndex
}

