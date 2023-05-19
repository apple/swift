// RUN: %target-swift-ide-test -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

class A {
  typealias T = Int
  enum E {
    case a
    case b
  }
}

class B : A {
  typealias T = String
  struct E {}
}

func foo() {
  _ = B.#^MEMBER_INHERITED^#
}

// MEMBER_INHERITED-LABEL: Begin completions, 5 items
// MEMBER_INHERITED-NEXT: Keyword[self]/CurrNominal:          self[#B.Type#]; name=self
// MEMBER_INHERITED-NEXT: Keyword/CurrNominal:                Type[#B.Type#]; name=Type
// MEMBER_INHERITED-NEXT: Decl[TypeAlias]/CurrNominal:        T[#String#]; name=T
// MEMBER_INHERITED-NEXT: Decl[Struct]/CurrNominal:           E[#B.E#]; name=E
// MEMBER_INHERITED-NEXT: Decl[Constructor]/CurrNominal:      init()[#B#]; name=init()

do {
  let _: Any.#^MEMBER_ANY_DOT^#

  // MEMBER_ANY_DOT-LABEL: Begin completions, 2 items
  // MEMBER_ANY_DOT-NEXT: Keyword/None: Protocol[#(any Any).Type#]; name=Protocol
  // MEMBER_ANY_DOT-NEXT: Keyword/None: Type[#any Any.Type#]; name=Type

  let _: Any#^MEMBER_ANY_NO_DOT^#

  // MEMBER_ANY_NO_DOT-LABEL: Begin completions, 2 items
  // MEMBER_ANY_NO_DOT-NEXT: Keyword/None: .Protocol[#(any Any).Type#]; name=Protocol
  // MEMBER_ANY_NO_DOT-NEXT: Keyword/None: .Type[#any Any.Type#]; name=Type
}

do {
  struct S {
    struct Nested {}
  }

  let _: S.#^MEMBER_DOT^#
  let _: (S).#^MEMBER_PAREN_DOT?check=MEMBER_DOT^#

  // MEMBER_DOT-LABEL: Begin completions, 2 items
  // MEMBER_DOT-NEXT: Decl[Struct]/CurrNominal: Nested[#S.Nested#]; name=Nested
  // MEMBER_DOT-NEXT: Keyword/None: Type[#{{\(?}}S{{\)?}}.Type#]; name=Type

  let _: (S)#^MEMBER_PAREN_NO_DOT^#

  // MEMBER_PAREN_NO_DOT-LABEL: Begin completions, 4 items
  // MEMBER_PAREN_NO_DOT-NEXT: Keyword/None: async; name=async
  // MEMBER_PAREN_NO_DOT-NEXT: Keyword[throws]/None: throws; name=throws
  // MEMBER_PAREN_NO_DOT-NEXT: Decl[Struct]/CurrNominal: .Nested[#S.Nested#]; name=Nested
  // MEMBER_PAREN_NO_DOT-NEXT: Keyword/None: .Type[#(S).Type#]; name=Type
}

do {
  let _: Int.Type.#^MEMBER_META_DOT^#

  // MEMBER_META_DOT-LABEL: Begin completions, 1 items
  // MEMBER_META_DOT-NEXT: Keyword/None: Type[#Int.Type.Type#]; name=Type

  let _: Int.Type#^MEMBER_META_NO_DOT^#

  // MEMBER_META_NO_DOT-LABEL: Begin completions, 1 items
  // MEMBER_META_NO_DOT-NEXT: Keyword/None: .Type[#Int.Type.Type#]; name=Type

  let _: Sequence.Protocol.#^MEMBER_PROTOCOL_META_DOT^#

  // MEMBER_PROTOCOL_META_DOT-LABEL: Begin completions, 1 items
  // MEMBER_PROTOCOL_META_DOT-NEXT: Keyword/None: Type[#(any Sequence).Type.Type#]; name=Type

  let _: Sequence.Protocol#^MEMBER_PROTOCOL_META_NO_DOT^#

  // MEMBER_PROTOCOL_META_NO_DOT-LABEL: Begin completions, 1 items
  // MEMBER_PROTOCOL_META_NO_DOT-NEXT: Keyword/None: .Type[#(any Sequence).Type.Type#]; name=Type
}

extension Optional {
  typealias Wrappt = Wrapped
}
do {
  let _: Optional<Int>.#^OPTIONAL_MEMBER_DOT?check=OPTIONAL^#
  let _: Int?.#^SUGARED_OPTIONAL_MEMBER_DOT?check=OPTIONAL^#
  let _: Int!.#^SUGARED_IUOPTIONAL_MEMBER_DOT?check=OPTIONAL^#

  // OPTIONAL-LABEL: Begin completions, 2 items
  // OPTIONAL-LABEL: Decl[TypeAlias]/CurrNominal: Wrappt[#Wrapped#]; name=Wrappt
  // OPTIONAL-LABEL: Keyword/None: Type[#{{Optional<Int>|Int\?}}.Type#]; name=Type

  let _: Array<Int>.#^ARRAY_MEMBER_DOT?check=ARRAY^#
  let _: [Int].#^SUGARED_ARRAY_MEMBER_DOT?check=ARRAY^#

  // ARRAY-LABEL: Begin completions, 8 items
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Index[#Int#]; name=Index
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Indices[#Range<Int>#]; name=Indices
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Iterator[#IndexingIterator<Array<Element>>#]; name=Iterator
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Element[#Element#]; name=Element
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: SubSequence[#ArraySlice<Element>#]; name=SubSequence
  // ARRAY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: ArrayLiteralElement[#Element#]; name=ArrayLiteralElement
  // ARRAY-NEXT: Decl[TypeAlias]/Super/NotRecommended/IsSystem: IndexDistance[#Int#]; name=IndexDistance; diagnostics=warning:'IndexDistance' is deprecated: all index distances are now of type Int
  // ARRAY-NEXT: Keyword/None: Type[#{{Array<Int>|\[Int\]}}.Type#]; name=Type

  let _: Dictionary<Int, Int>.#^DICTIONARY_MEMBER_DOT?check=DICTIONARY^#
  let _: [Int : Int].#^SUGARED_DICTIONARY_MEMBER_DOT?check=DICTIONARY^#

  // DICTIONARY-LABEL: Begin completions, 11 items
  // DICTIONARY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Element[#(key: Key, value: Value)#]; name=Element
  // DICTIONARY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: SubSequence[#Slice<Dictionary<Key, Value>>#]; name=SubSequence
  // DICTIONARY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Indices[#DefaultIndices<Dictionary<Key, Value>>#]; name=Indices
  // DICTIONARY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Key[#Key#]; name=Key
  // DICTIONARY-NEXT: Decl[TypeAlias]/CurrNominal/IsSystem: Value[#Value#]; name=Value
  // DICTIONARY-NEXT: Decl[Struct]/CurrNominal/IsSystem: Keys[#Dictionary.Keys#]; name=Keys
  // DICTIONARY-NEXT: Decl[Struct]/CurrNominal/IsSystem: Values[#Dictionary.Values#]; name=Values
  // DICTIONARY-NEXT: Decl[Struct]/CurrNominal/IsSystem: Index[#Dictionary.Index#]; name=Index
  // DICTIONARY-NEXT: Decl[Struct]/CurrNominal/IsSystem: Iterator[#Dictionary.Iterator#]; name=Iterator
  // DICTIONARY-NEXT: Decl[TypeAlias]/Super/NotRecommended/IsSystem: IndexDistance[#Int#]; name=IndexDistance; diagnostics=warning
  // DICTIONARY-NEXT: Keyword/None: Type[#{{Dictionary<Int, Int>|\[Int : Int\]}}.Type#]; name=Type
}

do {
  let _: (Int, Int).#^MEMBER_TUPLE_DOT^#
  let _: ().#^MEMBER_VOID_DOT?check=MEMBER_TUPLE_DOT^#
  let _: ((Int) -> Void).#^MEMBER_FUNCTION_DOT?check=MEMBER_TUPLE_DOT^#

  // MEMBER_TUPLE_DOT-LABEL: Begin completions, 1 items
  // MEMBER_TUPLE_DOT-NEXT: Keyword/None: Type[#({{.*}}).Type#]; name=Type

  let _: (Int, Int)#^MEMBER_TUPLE_NO_DOT^#
  let _: ()#^MEMBER_VOID_NO_DOT?check=MEMBER_TUPLE_NO_DOT^#
  let _: ((Int) -> Void)#^MEMBER_FUNCTION_NO_DOT?check=MEMBER_TUPLE_NO_DOT^#

  // MEMBER_TUPLE_NO_DOT-LABEL: Begin completions, 3 items
  // MEMBER_TUPLE_NO_DOT-NEXT: Keyword/None: async; name=async
  // MEMBER_TUPLE_NO_DOT-NEXT: Keyword[throws]/None: throws; name=throws
  // MEMBER_TUPLE_NO_DOT-NEXT: Keyword/None: .Type[#({{.*}}).Type#]; name=Type

  let _: (any Sequence).#^MEMBER_EXISTENTIAL_DOT^#

  // FIXME: Do not show associated types?
  // MEMBER_EXISTENTIAL_DOT-LABEL: Begin completions, 4 items
  // MEMBER_EXISTENTIAL_DOT-NEXT: Decl[AssociatedType]/CurrNominal/IsSystem: Element; name=Element
  // MEMBER_EXISTENTIAL_DOT-NEXT: Decl[AssociatedType]/CurrNominal/IsSystem: Iterator; name=Iterator
  // MEMBER_EXISTENTIAL_DOT-NEXT: Keyword/None: Protocol[#(any Sequence).Type#]; name=Protocol
  // MEMBER_EXISTENTIAL_DOT-NEXT: Keyword/None: Type[#any Sequence.Type#]; name=Type

  let _: (any Sequence)#^MEMBER_EXISTENTIAL_NO_DOT^#

  // FIXME: Do not show associated types?
  // MEMBER_EXISTENTIAL_NO_DOT-LABEL: Begin completions, 6 items
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Keyword/None: async; name=async
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Keyword[throws]/None: throws; name=throws
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Decl[AssociatedType]/CurrNominal/IsSystem: .Element; name=Element
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Decl[AssociatedType]/CurrNominal/IsSystem: .Iterator; name=Iterator
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Keyword/None: .Protocol[#(any Sequence).Type#]; name=Protocol
  // MEMBER_EXISTENTIAL_NO_DOT-NEXT: Keyword/None: .Type[#any Sequence.Type#]; name=Type
}
