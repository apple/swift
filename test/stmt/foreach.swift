// RUN: %target-typecheck-verify-swift

// Bad containers and ranges
struct BadContainer1 {
}

func bad_containers_1(bc: BadContainer1) {
  for e in bc { } // expected-error{{for-in loop requires 'BadContainer1' to conform to 'Sequence'}}
}

struct BadContainer2 : Sequence { // expected-error{{type 'BadContainer2' does not conform to protocol 'Sequence'}}
  var generate : Int
}

func bad_containers_2(bc: BadContainer2) {
  for e in bc { }
}

struct BadContainer3 : Sequence { // expected-error{{type 'BadContainer3' does not conform to protocol 'Sequence'}}
  func makeIterator() { } // expected-note{{candidate can not infer 'Iterator' = '()' because '()' is not a nominal type and so can't conform to 'IteratorProtocol'}}
}

func bad_containers_3(bc: BadContainer3) {
  for e in bc { }
}

struct BadIterator1 {}

struct BadContainer4 : Sequence { // expected-error{{type 'BadContainer4' does not conform to protocol 'Sequence'}}
  typealias Iterator = BadIterator1 // expected-note{{possibly intended match 'BadContainer4.Iterator' (aka 'BadIterator1') does not conform to 'IteratorProtocol'}}
  func makeIterator() -> BadIterator1 { }
}

func bad_containers_4(bc: BadContainer4) {
  for e in bc { }
}

// Pattern type-checking

struct GoodRange<Int> : Sequence, IteratorProtocol {
  typealias Element = Int
  func next() -> Int? {}

  typealias Iterator = GoodRange<Int>
  func makeIterator() -> GoodRange<Int> { return self }
}

struct GoodTupleIterator: Sequence, IteratorProtocol {
  typealias Element = (Int, Float)
  func next() -> (Int, Float)? {}

  typealias Iterator = GoodTupleIterator
  func makeIterator() -> GoodTupleIterator {}
}

protocol ElementProtocol {}

func patterns(gir: GoodRange<Int>, gtr: GoodTupleIterator) {
  var sum : Int
  var sumf : Float
  for i : Int in gir { sum = sum + i }
  for i in gir { sum = sum + i }
  for f : Float in gir { sum = sum + f } // expected-error{{cannot convert sequence element type 'GoodRange<Int>.Element' (aka 'Int') to expected type 'Float'}}
  for f : ElementProtocol in gir { } // expected-error {{sequence element type 'GoodRange<Int>.Element' (aka 'Int') does not conform to expected protocol 'ElementProtocol'}}

  for (i, f) : (Int, Float) in gtr { sum = sum + i }

  for (i, f) in gtr {
    sum = sum + i
    sumf = sumf + f
    sum = sum + f  // expected-error {{cannot convert value of type 'Float' to expected argument type 'Int'}} {{17-17=Int(}} {{18-18=)}}
  }

  for (i, _) : (Int, Float) in gtr { sum = sum + i }

  for (i, _) : (Int, Int) in gtr { sum = sum + i } // expected-error{{cannot convert sequence element type 'GoodTupleIterator.Element' (aka '(Int, Float)') to expected type '(Int, Int)'}}

  for (i, f) in gtr {}
}

func slices(i_s: [Int], ias: [[Int]]) {
  var sum = 0
  for i in i_s { sum = sum + i }

  for ia in ias {
    for i in ia {
      sum = sum + i
    }
  }
}

func discard_binding() {
  for _ in [0] {}
}

struct X<T> { 
  var value: T
}

struct Gen<T> : IteratorProtocol {
  func next() -> T? { return nil }
}

struct Seq<T> : Sequence {
  func makeIterator() -> Gen<T> { return Gen() }
}

func getIntSeq() -> Seq<Int> { return Seq() }

func getOvlSeq() -> Seq<Int> { return Seq() } // expected-note{{found this candidate}}
func getOvlSeq() -> Seq<Double> { return Seq() } // expected-note{{found this candidate}}
func getOvlSeq() -> Seq<X<Int>> { return Seq() } // expected-note{{found this candidate}}

func getGenericSeq<T>() -> Seq<T> { return Seq() }

func getXIntSeq() -> Seq<X<Int>> { return Seq() }

func getXIntSeqIUO() -> Seq<X<Int>>! { return nil }

func testForEachInference() {
  for i in getIntSeq() { }

  // Overloaded sequence resolved contextually
  for i: Int in getOvlSeq() { }
  for d: Double in getOvlSeq() { }

  // Overloaded sequence not resolved contextually
  for v in getOvlSeq() { } // expected-error{{ambiguous use of 'getOvlSeq()'}}

  // Generic sequence resolved contextually
  for i: Int in getGenericSeq() { }
  for d: Double in getGenericSeq() { }

  // Inference of generic arguments in the element type from the
  // sequence.
  for x: X in getXIntSeq() {
    let z = x.value + 1
  }

  for x: X in getOvlSeq() {
    let z = x.value + 1
  }

  // Inference with implicitly unwrapped optional
  for x: X in getXIntSeqIUO() {
    let z = x.value + 1
  }

  // Range overloading.
  for i: Int8 in 0..<10 { }
  for i: UInt in 0...10 { }
}

func testMatchingPatterns() {
  // <rdar://problem/21428712> for case parse failure
  let myArray : [Int?] = []
  for case .some(let x) in myArray {
    _ = x
  }

  // <rdar://problem/21392677> for/case/in patterns aren't parsed properly
  class A {}
  class B : A {}
  class C : A {}
  let array : [A] = [A(), B(), C()]
  for case (let x as B) in array {
    _ = x
  }
}

// <rdar://problem/21662365> QoI: diagnostic for for-each over an optional sequence isn't great
func testOptionalSequence() {
  let array : [Int]?
  for x in array {  // expected-error {{for-in loop requires '[Int]?' to conform to 'Sequence'; did you mean to unwrap optional?}}
  }
}

// Crash with (invalid) for each over an existential
func testExistentialSequence(s: Sequence) { // Ok
  for x in s { // expected-error {{protocol 'Sequence' as a type cannot conform to the protocol itself}} expected-note {{only concrete types such as structs, enums and classes can conform to protocols}}
    _ = x
  }
}

// Conditional conformance to Sequence and IteratorProtocol.
protocol P { }

struct RepeatedSequence<T> {
  var value: T
  var count: Int
}

struct RepeatedIterator<T> {
  var value: T
  var count: Int
}

extension RepeatedIterator: IteratorProtocol where T: P {
  typealias Element = T

  mutating func next() -> T? {
    if count == 0 { return nil }
    count = count - 1
    return value
  }
}

extension RepeatedSequence: Sequence where T: P {
  typealias Element = T
  typealias Iterator = RepeatedIterator<T>
  typealias SubSequence = AnySequence<T>

  func makeIterator() -> RepeatedIterator<T> {
    return Iterator(value: value, count: count)
  }
}

extension Int : P { }

func testRepeated(ri: RepeatedSequence<Int>) {
  for x in ri { _ = x }
}

// SR-12398: Poor pattern matching diagnostic: "for-in loop requires '[Int]' to conform to 'Sequence'"
func sr_12398(arr1: [Int], arr2: [(a: Int, b: String)]) {
  for (x, y) in arr1 {}
  // expected-error@-1 {{tuple pattern cannot match values of non-tuple type 'Int'}}

  for (x, y, _) in arr2 {}
  // expected-error@-1 {{pattern cannot match values of type '(a: Int, b: String)'}}
}

// rdar://62339835
func testForEachWhereWithClosure(_ x: [Int]) {
  func foo<T>(_ fn: () -> T) -> Bool { true }

  for i in x where foo({ i }) {}
  for i in x where foo({ i.byteSwapped == 5 }) {}
  for i in x where x.contains(where: { $0.byteSwapped == i }) {}
}
