// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

func buildView<T>(@MyViewBuilder _ x: () -> T) {}

@resultBuilder struct MyViewBuilder {
  static func buildBlock(_ content: MyText) -> MyText { content }
}

struct MyText : Equatable {
  init(verbatim content: String) {}
  init<S>(_ content: S) where S : StringProtocol {}
}

func test(text: String) {
  buildView {
    MyText(#^COMPLETE^#text)
  }
}

// COMPLETE: Begin completions
// COMPLETE-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#verbatim: String#}[')'][#MyText#];
// COMPLETE-DAG: Decl[Constructor]/CurrNominal/Flair[ArgLabels]/TypeRelation[Convertible]: ['(']{#(content): StringProtocol#}[')'][#MyText#];
// COMPLETE-DAG: Decl[LocalVar]/Local/TypeRelation[Convertible]: text[#String#];
// COMPLETE: End completions
