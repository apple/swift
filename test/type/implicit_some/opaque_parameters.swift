// RUN: %target-typecheck-verify-swift -disable-availability-checking -warn-redundant-requirements  -enable-experimental-feature ImplicitSome

// REQUIRES: asserts

protocol P { }

protocol Q {
  associatedtype A: P & Equatable

  func f() -> A
  func takesA(_: A)
}

extension Int: P { }
extension String: P { }
extension Array: P { }
extension Set: Q {
  typealias A = String

  func f() -> String { }
  func takesA(_: String) { }
}

func takesImplicitQ(_ q: Q) -> Bool {
  // expected-error@+1 {{cannot convert value of type 'Int' to expected argument type '(Q).A'}}
  q.takesA(1)

  return q.f() == q.f()
}

func testParam(_ a: Collection) -> Bool  {
  a.isEmpty
}

func testMultiple(_ a: Collection, _ b: Collection) -> Bool  {
  a.count == b.count
}

// expected-note@+1{{where 'P' = 'Set<String>'}}
func takeMultiple<T>(_: T, _: Q, _: P) { }

func testTakeMultiple(
  arrayOfInts: [Int], setOfStrings: Set<String>, i: Int, d: Double
) {
  takeMultiple(d, setOfStrings, i)
  takeMultiple(d,setOfStrings, arrayOfInts)
  takeMultiple(d, setOfStrings, setOfStrings) // expected-error{{global function 'takeMultiple' requires that 'Set<String>' conform to 'P'}}
}

// inout
func implicitInOut(_: inout P) { }

func testInOut() {
  var i = 17
  implicitInOut(&i)
}

// Prohibit use of opaque parameters in consuming positions.
typealias FnType<T> = (T) -> Void

func consumingA(fn: (P) -> Void) { } // expected-error{{'some' cannot appear in parameter position in parameter type '(P) -> Void'}}
func consumingB(fn: FnType<P>) { } // expected-error{{'some' cannot appear in parameter position in parameter type '(P) -> Void'}}


func takePrimaryCollections(
  _ strings: Collection<String>,
  _ ints : Collection<Int>
) {
  for s in strings {
    let _: String = s
  }

  for i in ints {
    let _: Int = i
  }
}
func takeMatchedPrimaryCollections<T: Equatable>(
  _ first: Collection<T>, _ second: Collection<T>
) -> Bool {
  first.elementsEqual(second)
}

func testPrimaries(
  arrayOfInts: [Int], setOfStrings: Set<String>, setOfInts: Set<Int>
) {
  takePrimaryCollections(setOfStrings, setOfInts)
  takePrimaryCollections(setOfStrings, arrayOfInts)
  _ = takeMatchedPrimaryCollections(arrayOfInts, setOfInts)
  _ = takeMatchedPrimaryCollections(arrayOfInts, setOfStrings) // expected-error{{type of expression is ambiguous without a type annotation}}
}
