// REQUIRES: swift_swift_parser

// RUN: %empty-directory(%t)
// RUN: mkdir -p %t/plugins

//##-- Prepare the macro plugin.
// RUN: %host-build-swift -swift-version 5 -emit-library -o %t/plugins/%target-library-name(MacroDefinition) -module-name=MacroDefinition %S/../Macros/Inputs/syntax_macro_definitions.swift -g -no-toolchain-stdlib-rpath

// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t -plugin-path %t/plugins -parse-as-library 

@freestanding(declaration, names: named(A), named(B), named(foo), named(addOne))
macro defineDeclsWithKnownNames() = #externalMacro(module: "MacroDefinition", type: "DefineDeclsWithKnownNamesMacro")

@attached(peer, names: suffixed(_peer))
macro PeerWithSuffix() = #externalMacro(module: "MacroDefinition", type: "PeerValueWithSuffixNameMacro")

@attached(peer, names: arbitrary)
macro PeerWithSuffixAsArbitrary() = #externalMacro(module: "MacroDefinition", type: "PeerValueWithSuffixNameMacro")

@freestanding(declaration, names: arbitrary)
macro VarValueDecl() = #externalMacro(module: "MacroDefinition", type: "VarValueMacro")


#defineDeclsWithKnownNames

@PeerWithSuffix
func globalFunc() {}

func test() {
   #^GLOBAL^#
// GLOBAL-DAG: Decl[Struct]/CurrModule:            A[#A#]; name=A
// GLOBAL-DAG: Decl[Struct]/CurrModule:            B[#B#]; name=B
// GLOBAL-DAG: Decl[GlobalVar]/CurrModule:         foo[#Int#]; name=foo
// GLOBAL-DAG: Decl[GlobalVar]/CurrModule:         addOne[#(Int) -> Int#]; name=addOne
// GLOBAL-DAG: Decl[FreeFunction]/CurrModule:      globalFunc()[#Void#]; name=globalFunc()
// GLOBAL-DAG: Decl[GlobalVar]/CurrModule:         globalFunc_peer[#Int#]; name=globalFunc_peer
}

struct MyStruct {
  @PeerWithSuffix
  func instanceMethod() {}

  @PeerWithSuffix
  static func staticMethod() {}

  @PeerWithSuffixAsArbitrary
  func forArbitrary() {}

  #defineDeclsWithKnownNames

  #VarValueDecl
}

func testMemberInstance(value: MyStruct) {
  value.#^MEMBER_INSTANCE^#
// MEMBER_INSTANCE-DAG: Keyword[self]/CurrNominal:          self[#MyStruct#]; name=self
// MEMBER_INSTANCE-DAG: Decl[InstanceMethod]/CurrNominal:   instanceMethod()[#Void#]; name=instanceMethod()
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      instanceMethod_peer[#Int#]; name=instanceMethod_peer
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      staticMethod_peer[#Int#]; name=staticMethod_peer
// MEMBER_INSTANCE-DAG: Decl[InstanceMethod]/CurrNominal:   forArbitrary()[#Void#]; name=forArbitrary()
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      forArbitrary_peer[#Int#]; name=forArbitrary_peer
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      foo[#Int#]; name=foo
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      addOne[#(Int) -> Int#]; name=addOne
// MEMBER_INSTANCE-DAG: Decl[InstanceVar]/CurrNominal:      value[#Int#]; name=value
// NOTE: 'staticMethod_peer' is a instance var because the macro emits the decl without 'static'
}

func testMemberStatic() {
  MyStruct.#^MEMBER_STATIC^#
// MEMBER_STATIC-NOT: _peer
// MEMBER_STATIC-DAG: Keyword[self]/CurrNominal:          self[#MyStruct.Type#]; name=self
// MEMBER_STATIC-DAG: Keyword/CurrNominal:                Type[#MyStruct.Type#]; name=Type
// MEMBER_STATIC-DAG: Decl[Struct]/CurrNominal:           A[#MyStruct.A#]; name=A
// MEMBER_STATIC-DAG: Decl[Struct]/CurrNominal:           B[#MyStruct.B#]; name=B
// MEMBER_STATIC-DAG: Decl[InstanceMethod]/CurrNominal:   instanceMethod({#(self): MyStruct#})[#() -> Void#]; name=instanceMethod(:)
// MEMBER_STATIC-DAG: Decl[StaticMethod]/CurrNominal:     staticMethod()[#Void#]; name=staticMethod()
// MEMBER_STATIC-NOT: _peer
}

func testLocal() {
  #defineDeclsWithKnownNames

  @PeerWithSuffix func localFunc() {}

  do {
    #^LOCAL?skip=FIXME^#
// FIXME: macros in replace function bodies are not handled correclty.
// FIXME: decls instroduced by #defineDeclsWithKnownNames are missing.
// LOCAL-DAG: Decl[FreeFunction]/Local:           localFunc()[#Void#]; name=localFunc()
// LOCAL-DAG: Decl[LocalVar]/Local:               localFunc_peer[#Int#]; name=localFunc_peer
// LOCAL-DAG: Decl[Struct]/Local:            A[#A#]; name=A
// LOCAL-DAG: Decl[Struct]/Local:            B[#B#]; name=B
// LOCAL-DAG: Decl[LocalVar]/Local:         foo[#Int#]; name=foo
// LOCAL-DAG: Decl[LocalVar]/Local:         addOne[#(Int) -> Int#]; name=addOne
  }
}
