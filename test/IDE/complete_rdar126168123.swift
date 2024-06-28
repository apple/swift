// RUN: %batch-code-completion

// rdar://126168123

protocol MyProto {}
protocol MyProto2 {}

struct MyStruct : MyProto {}

extension MyProto where Self == MyStruct {
  static var automatic: MyStruct { fatalError() }
}

func use<T: MyProto>(_ someT: T) {}
func use<T: MyProto2>(_ someT: T) {}

func test() {
  use(.#^COMPLETE^#)
}

// COMPLETE: Decl[StaticVar]/CurrNominal/TypeRelation[Convertible]: automatic[#MyStruct#]; name=automatic
