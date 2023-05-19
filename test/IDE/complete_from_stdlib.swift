// RUN: %empty-directory(%t)
// RUN: %target-swift-ide-test -disable-implicit-concurrency-module-import -disable-implicit-string-processing-module-import -batch-code-completion -source-filename %s -filecheck %raw-FileCheck -completion-output-dir %t

// NO_STDLIB_PRIVATE-NOT: Decl{{[^:]*}}/IsSystem: _

#^PLAIN_TOP_LEVEL_1?check=PLAIN_TOP_LEVEL;check=NO_STDLIB_PRIVATE^#

// PLAIN_TOP_LEVEL-DAG: Decl[Struct]/OtherModule[Swift]/IsSystem: Array[#Array#]{{; name=.+$}}

func privateNominalMembers(_ a: String) {
  a.#^PRIVATE_NOMINAL_MEMBERS_1?check=PRIVATE_NOMINAL_MEMBERS_1;check=NO_STDLIB_PRIVATE^#
}


// FIXME: we should show the qualified String.Index type.
// rdar://problem/20788802
// PRIVATE_NOMINAL_MEMBERS_1-DAG: Decl[InstanceVar]/CurrNominal/IsSystem: startIndex[#String.Index#]{{; name=.+$}}

func protocolExtCollection1a<C : Collection>(_ a: C) {
  a.#^PRIVATE_NOMINAL_MEMBERS_2A?check=PRIVATE_NOMINAL_MEMBERS_2A;check=NEGATIVE_PRIVATE_NOMINAL_MEMBERS_2A;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_2A-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem: map({#(transform): (C.Element) throws -> T##(C.Element) throws -> T#})[' rethrows'][#[T]#];
// NEGATIVE_PRIVATE_NOMINAL_MEMBERS_2A-NOT: Decl{{.*}}: index({#before: any Comparable#})

func protocolExtCollection1b(_ a: Collection) {
  a.#^PRIVATE_NOMINAL_MEMBERS_2B?check=PRIVATE_NOMINAL_MEMBERS_2B;check=NEGATIVE_PRIVATE_NOMINAL_MEMBERS_2B;check=NO_STDLIB_PRIVATE^#
}

// FIXME(https://github.com/apple/swift/issues/65696): We should not be showing this because (1) it cannot be accessed on the existential (2) we don't have the syntax and features to represent the projected type sig anyway.
// PRIVATE_NOMINAL_MEMBERS_2B-DAG: map({#(transform): (any Collection.Element) throws -> T##(any Collection.Element) throws -> T#})[' rethrows'][#[T]#]{{; name=.+}}
// NEGATIVE_PRIVATE_NOMINAL_MEMBERS_2B-NOT: Decl{{.*}}: index({#before: any Comparable#})

func protocolExtCollection2<C : Collection where C.Index : BidirectionalIndex>(_ a: C) {
  a.#^PRIVATE_NOMINAL_MEMBERS_3?check=PRIVATE_NOMINAL_MEMBERS_3;check=NEGATIVE_PRIVATE_NOMINAL_MEMBERS_3;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_3-DAG: Decl[InstanceMethod]/Super/IsSystem:         map({#(transform): (C.Element) throws -> T##(C.Element) throws -> T#})[' rethrows'][#[T]#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_3-DAG: Decl[InstanceVar]/Super/IsSystem:            lazy[#LazySequence<Collection>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_3-DAG: firstIndex({#where: (C.Element) throws -> Bool##(C.Element) throws -> Bool#})[' rethrows'][#Comparable?#]{{; name=.+}}
// NEGATIVE_PRIVATE_NOMINAL_MEMBERS_3-NOT: Decl{{.*}}:         firstIndex({#({{.*}}): Self.Iterator.Element

func protocolExtArray<T : Equatable>(_ a: [T]) {
  a.#^PRIVATE_NOMINAL_MEMBERS_4?check=PRIVATE_NOMINAL_MEMBERS_4;check=NO_STDLIB_PRIVATE^#
}
// PRIVATE_NOMINAL_MEMBERS_4-DAG: Decl[InstanceMethod]/Super/IsSystem:         map({#(transform): (Equatable) throws -> T##(Equatable) throws -> T#})[' rethrows'][#[T]#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_4-DAG: Decl[InstanceVar]/Super/IsSystem:            last[#Equatable?#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_4-DAG: Decl[InstanceMethod]/Super/IsSystem:         firstIndex({#of: Equatable#})[#Int?#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_4-DAG: Decl[InstanceMethod]/Super/IsSystem:         firstIndex({#where: (Equatable) throws -> Bool##(Equatable) throws -> Bool#})[' rethrows'][#Int?#]{{; name=.+}}

func testArchetypeReplacement1<FOO : Equatable>(_ a: [FOO]) {
  a.#^PRIVATE_NOMINAL_MEMBERS_5?check=PRIVATE_NOMINAL_MEMBERS_5;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem:   append({#(newElement): Equatable#})[#Void#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem:   insert({#(newElement): Equatable#}, {#at: Int#})[#Void#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceVar]/Super/IsSystem:            isEmpty[#Bool#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceVar]/Super/IsSystem:            first[#Equatable?#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropFirst({#(k): Int#})[#ArraySlice<Equatable>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropLast({#(k): Int#})[#ArraySlice<Equatable>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/Super/IsSystem:         prefix({#(maxLength): Int#})[#ArraySlice<Equatable>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_5-DAG: Decl[InstanceMethod]/Super/IsSystem:         suffix({#(maxLength): Int#})[#ArraySlice<Equatable>#]{{; name=.+}}


func testArchetypeReplacement2<BAR : Equatable>(_ a: [BAR]) {
  a.#^PRIVATE_NOMINAL_MEMBERS_6?check=PRIVATE_NOMINAL_MEMBERS_6;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem:   append({#(newElement): Equatable#})[#Void#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem:   insert({#(newElement): Equatable#}, {#at: Int#})[#Void#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropFirst()[#ArraySlice<Equatable>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropLast()[#[Equatable]#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         enumerated()[#EnumeratedSequence<[Equatable]>#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         min({#by: (Equatable, Equatable) throws -> Bool##(Equatable, Equatable) throws -> Bool#})[' rethrows'][#Equatable?#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         max({#by: (Equatable, Equatable) throws -> Bool##(Equatable, Equatable) throws -> Bool#})[' rethrows'][#Equatable?#]{{; name=.+}}
// FIXME: The following should include 'partialResult' as local parameter name: "(nextPartialResult): (_ partialResult: Result, Equatable)"
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         reduce({#(initialResult): Result#}, {#(nextPartialResult): (Result, Equatable) throws -> Result##(_ partialResult: Result, Equatable) throws -> Result#})[' rethrows'][#Result#]{{; name=.+}}
// PRIVATE_NOMINAL_MEMBERS_6-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropFirst({#(k): Int#})[#ArraySlice<Equatable>#]{{; name=.+}}
// FIXME: restore Decl[InstanceMethod]/Super:         flatMap({#(transform): (Equatable) throws -> Sequence##(Equatable) throws -> Sequence#})[' rethrows'][#[IteratorProtocol.Element]#]{{; name=.+}}

func testArchetypeReplacement3 (_ a : [Int]) {
  a.#^PRIVATE_NOMINAL_MEMBERS_7?check=PRIVATE_NOMINAL_MEMBERS_7;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/CurrNominal/IsSystem:   append({#(newElement): Int#})[#Void#]
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/Super/IsSystem:         removeLast()[#Int#]
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceVar]/Super/IsSystem:            first[#Int?#]
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/Super/IsSystem:         map({#(transform): (Int) throws -> T##(Int) throws -> T#})[' rethrows'][#[T]#]
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/Super/IsSystem:         dropLast({#(k): Int#})[#ArraySlice<Int>#]
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/Super/IsSystem:         elementsEqual({#(other): Sequence#}, {#by: (Int, Sequence.Element) throws -> Bool##(Int, Sequence.Element) throws -> Bool#})[' rethrows'][#Bool#]; name=elementsEqual(:by:)
// PRIVATE_NOMINAL_MEMBERS_7-DAG: Decl[InstanceMethod]/Super/IsSystem:         elementsEqual({#(other): Sequence#})[#Bool#]; name=elementsEqual(:)


protocol P2 {
  associatedtype MyElement
}

extension P2 {
  func foo(_ x: MyElement) {}
}

typealias MyInt = Int

class MyClass1 : P2 {
  typealias MyElement = MyInt
}

class MyClass2 : P2 {
  typealias MyElement = Int
}

protocol P1{}

class MyClass3 {
  func foo<T: P1 & P2>(_ t : T) {}
}

func testArchetypeReplacement4(_ a : MyClass1) {
  a.#^PRIVATE_NOMINAL_MEMBERS_8?check=PRIVATE_NOMINAL_MEMBERS_8;check=NO_STDLIB_PRIVATE^#
}
// PRIVATE_NOMINAL_MEMBERS_8-DAG: Decl[InstanceMethod]/Super: foo({#(x): MyInt#})[#Void#]{{; name=.+}}

func testArchetypeReplacement5(_ a : MyClass2) {
  a.#^PRIVATE_NOMINAL_MEMBERS_9?check=PRIVATE_NOMINAL_MEMBERS_9;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_9-DAG: Decl[InstanceMethod]/Super: foo({#(x): Int#})[#Void#]{{; name=.+}}

func testArchetypeReplacement6() {
  var a = MyClass3()
  a.#^PRIVATE_NOMINAL_MEMBERS_10?check=PRIVATE_NOMINAL_MEMBERS_10;check=NO_STDLIB_PRIVATE^#
}

// PRIVATE_NOMINAL_MEMBERS_10-DAG: Decl[InstanceMethod]/CurrNominal:   foo({#(t): P1 & P2#})[#Void#]{{; name=.+}}

func testPostfixOperator1(_ x: Int) {
  x#^POSTFIX_INT_1?check=POSTFIX_RVALUE_INT^#
}
// POSTFIX_RVALUE_INT-NOT: ++
// POSTFIX_RVALUE_INT-NOT: --

func testPostfixOperator2(_ x: inout Int) {
  x#^POSTFIX_INT_2?check=POSTFIX_LVALUE_INT^#
}
// POSTFIX_LVALUE_INT-NOT: Decl[PostfixOperatorFunction]/OtherModule[Swift]/IsSystem: ++[#Int#]; name=
// POSTFIX_LVALUE_INT-NOT: Decl[PostfixOperatorFunction]/OtherModule[Swift]/IsSystem: --[#Int#]; name=

func testPostfixOperator3(_ x: MyInt??) {
  x#^POSTFIX_OPTIONAL_1?check=POSTFIX_OPTIONAL^#
}
// POSTFIX_OPTIONAL: BuiltinOperator/None: ![#MyInt?#]; name=!

func testInfixOperator1(_ x: Int) {
  x#^INFIX_INT_1?check=INFIX_INT;check=NEGATIVE_INFIX_INT^#
}
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  ... {#Int#}[#ClosedRange<Int>#]
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  &+ {#Int#}[#Int#]
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  + {#Int#}[#Int#]
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  &<< {#Int#}[#Int#]
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  < {#Int#}[#Bool#]
// INFIX_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#Int#}[#Bool#]
// NEGATIVE_INFIX_INT-NOT: &&
// NEGATIVE_INFIX_INT-NOT: +=
func testInfixOperator2(_ x: inout Int) {
  x#^INFIX_INT_2?check=INFIX_LVALUE_INT^#
}
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  ... {#Int#}[#ClosedRange<Int>#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  &+ {#Int#}[#Int#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  + {#Int#}[#Int#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  &<< {#Int#}[#Int#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  < {#Int#}[#Bool#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#Int#}[#Bool#]
// INFIX_LVALUE_INT-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  += {#Int#}[#Void#]
// INFIX_LVALUE_INT-NOT: &&

func testInfixOperator3(_ x: String) {
  x#^INFIX_STRING_1?check=INFIX_STRING^#
}
// INFIX_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  + {#String#}[#String#]
// INFIX_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  == {#String#}[#Bool#]
// INFIX_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  < {#String#}[#Bool#]
// INFIX_STRING-NOT: +=
// INFIX_STRING-NOT: <<

func testInfixOperator4(_ x: String) {
  x == ""#^INFIX_EXT_STRING_1?check=INFIX_EXT_STRING^#
}
// INFIX_EXT_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  + {#String#}[#String#]
// INFIX_EXT_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  || {#Bool#}[#Bool#]
// INFIX_EXT_STRING-DAG: Decl[InfixOperatorFunction]/OtherModule[Swift]/IsSystem:  && {#Bool#}[#Bool#]
// INFIX_EXT_STRING-NOT: ==

class TestSequence : Sequence {
#^CONFORM_SEQUENCE^#
// CONFORM_SEQUENCE-DAG: Decl[AssociatedType]/Super/IsSystem: typealias Element = {#(Type)#};
// CONFORM_SEQUENCE-DAG: Decl[AssociatedType]/Super/IsSystem: typealias Iterator = {#(Type)#};
// CONFORM_SEQUENCE-DAG: Decl[InstanceMethod]/Super/IsSystem: func makeIterator() -> some IteratorProtocol {|};
// CONFORM_SEQUENCE-DAG: Decl[InstanceVar]/Super/IsSystem:    var underestimatedCount: Int;
// CONFORM_SEQUENCE-DAG: Decl[InstanceMethod]/Super/IsSystem: func withContiguousStorageIfAvailable<R>(_ body: (UnsafeBufferPointer<Element>) throws -> R) rethrows -> R? {|};
}

public func rdar_70057258<T>(_ f: T) {}
extension Result {
  public init(_ value: Success?) {
    self = value.map(#^GENERIC_CLOSUREARG^#)
  }
}
// GENERIC_CLOSUREARG: Decl[FreeFunction]/CurrModule/TypeRelation[Convertible]: rdar_70057258(_:)[#<T> (T) -> ()#]; name=rdar_70057258(_:)
