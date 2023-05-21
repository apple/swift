// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

func test(myParam: Int) {
  copy #^COPY^#
  // COPY: Decl[LocalVar]/Local:               myParam[#Int#]; name=myParam
}
