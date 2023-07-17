// RUN: %empty-directory(%t)
// RUN: %swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

protocol View {}

@resultBuilder
struct Builder {
  static func buildBlock<C0: View>(_ c0: C0) -> C0 {}
  static func buildBlock<C0: View, C1: View>(_ c0: C0, _ c1: C1) -> C1 {}
  static func buildBlock<C0: View, C1: View, C2: View>(_ c0: C0, _ c1: C1, _ c2: C2) -> C1 {}
}

struct ForEach<Data, Content>: View where Data: RandomAccessCollection {
  init(_ dat: Data, @Builder content: (Data.Element) -> Content) {}
}

struct Text: View {
  init(_ text: String) {}
}

struct Value {
  var name: String
}

func test(values: [Value]) {
  _ = ForEach(values) { value in
    Text("foobar")
    Text("value \(value.#^STRINGLITERAL?check=CHECK^#)")
  }
  _ = ForEach(values) { value in
    Text("foobar")
    Text(value.#^NORMAL?check=CHECK^#)
  }
}
// CHECK: Begin completions, 2 items
// CHECK-DAG: Keyword[self]/CurrNominal:          self[#Value#];
// CHECK-DAG: Decl[InstanceVar]/CurrNominal/TypeRelation[Convertible]: name[#String#];
