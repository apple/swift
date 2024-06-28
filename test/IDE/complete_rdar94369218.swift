// RUN: %batch-code-completion -code-complete-inits-in-postfix-expr

protocol MyProto {
  init(value: String)
}

extension MyProto where Self == MyStruct {
  init(arg: String) { self = Self(value: arg) }
}

struct MyStruct: MyProto {
  init(value: String) {}
}

func test1() {
  #^GLOBALEXPR^#
// GLOBALEXPR-NOT: name=MyProto(
// GLOBALEXPR-DAG: Decl[Struct]/CurrModule:            MyStruct[#MyStruct#]; name=MyStruct
// GLOBALEXPR-DAG: Decl[Constructor]/CurrModule:       MyStruct({#value: String#})[#MyStruct#]; name=MyStruct(value:)
// GLOBALEXPR-DAG: Decl[Constructor]/CurrModule:       MyStruct({#arg: String#})[#MyStruct#]; name=MyStruct(arg:)
// GLOBALEXPR-DAG: Decl[Protocol]/CurrModule/Flair[RareType]: MyProto[#MyProto#]; name=MyProto
// GLOBALEXPR-NOT: name=MyProto(
}

func test2() {
  _ = MyProto(#^PROTOCOL_AFTER_PAREN^#
// PROTOCOL_AFTER_PAREN-NOT: name=arg: 
// PROTOCOL_AFTER_PAREN-NOT: name=value: 
}

func test3<MyGeneric: MyProto>() {
  class Inner {
    func test() {
      #^GENERICEXPR^#
// GENERICEXPR: Decl[GenericTypeParam]/Local:       MyGeneric[#MyGeneric#]; name=MyGeneric
// GENERICEXPR: Decl[Constructor]/Local:            MyGeneric({#value: String#})[#MyProto#]; name=MyGeneric(value:)
// GENERICEXPR: Decl[Constructor]/Local:            MyGeneric({#arg: String#})[#MyStruct#]; name=MyGeneric(arg:)
    }
  }
}
