// RUN: %target-typecheck-verify-swift -enable-experimental-opened-existential-types

protocol Q { }

protocol P {
  associatedtype A: Q
}

extension Int: P {
  typealias A = Double
}

extension Array: P where Element: P {
  typealias A = String
}

extension Double: Q { }
extension String: Q { }

func acceptGeneric<T: P>(_: T) -> T.A? { nil }
func acceptCollection<C: Collection>(_ c: C) -> C.Element { c.first! }

// --- Simple opening of existential values
func testSimpleExistentialOpening(p: any P, pq: any P & Q, c: any Collection) {
  let pa = acceptGeneric(p)
  let _: Int = pa // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}

  let pqa = acceptGeneric(pq)
  let _: Int = pqa  // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}

  let element = acceptCollection(c) 
  let _: Int = element // expected-error{{cannot convert value of type 'Any' to specified type 'Int'}}
}

// --- Requirements on nested types
protocol CollectionOfPs: Collection where Self.Element: P { }

func takeCollectionOfPs<C: Collection>(_: C) -> C.Element.A?    
  where C.Element: P
{
  nil
}

func testCollectionOfPs(cp: any CollectionOfPs) {
  let e = takeCollectionOfPs(cp)
  let _: Int = e // expected-error{{cannot convert value of type 'Q?' to specified type 'Int'}}
}

// --- Multiple opened existentials in the same expression
func takeTwoGenerics<T1: P, T2: P>(_ a: T1, _ b: T2) -> (T1, T2) { (a, b) }

extension P {
  func combineThePs<T: P & Q>(_ other: T) -> (A, T.A)? { nil }
}

func testMultipleOpened(a: any P, b: any P & Q) {
  let r1 = takeTwoGenerics(a, b)
  let _: Int = r1  // expected-error{{cannot convert value of type '(P, P & Q)' to specified type 'Int'}}

  let r2 = a.combineThePs(b)
  let _: Int = r2  // expected-error{{cannot convert value of type '(Q, Q)?' to specified type 'Int'}}  
}
