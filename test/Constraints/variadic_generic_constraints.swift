// RUN: %target-typecheck-verify-swift

// Test instantiation of constraint solver constraints from generic requirements
// involving type pack parameters

// Conformance requirements

protocol P {}

func takesP<each T: P>(_: repeat each T) {}  // expected-note {{where 'each T' = 'DoesNotConformToP'}}

struct ConformsToP: P {}
struct DoesNotConformToP {}

takesP()  // ok
takesP(ConformsToP(), ConformsToP(), ConformsToP())  // ok

takesP(ConformsToP(), DoesNotConformToP(), ConformsToP()) // expected-error {{global function 'takesP' requires that 'DoesNotConformToP' conform to 'P'}}

// Superclass requirements

class C {}

class SubclassOfC: C {}
class NotSubclassOfC {}

func takesC<each T: C>(_: repeat each T) {}  // expected-note {{where 'each T' = 'NotSubclassOfC'}}

takesC()  // ok
takesC(SubclassOfC(), SubclassOfC(), SubclassOfC())  // ok

takesC(SubclassOfC(), NotSubclassOfC(), SubclassOfC())  // expected-error {{global function 'takesC' requires that 'NotSubclassOfC' inherit from 'C'}}

// Layout requirements

struct S {}

func takesAnyObject<each T: AnyObject>(_: repeat each T) {}

takesAnyObject()
takesAnyObject(C(), C(), C())

// FIXME: Bad diagnostic
takesAnyObject(C(), S(), C())  // expected-error {{type of expression is ambiguous without a type annotation}}

// Same-type requirements

// expected-note@+1 {{where '(each T).Element' = 'String', '(each U).Element' = 'Int'}}
func takesParallelSequences<each T, each U>(t: repeat each T, u: repeat each U)
    where repeat each T: Sequence,
          repeat each U: Sequence,
          repeat (each T).Element == (each U).Element {}
takesParallelSequences()  // ok
takesParallelSequences(t: Array<Int>(), u: Set<Int>())  // ok
takesParallelSequences(t: Array<String>(), Set<Int>(), u: Set<String>(), Array<Int>())  // ok
takesParallelSequences(t: Array<String>(), Set<Int>(), u: Array<Int>(), Set<String>())  // expected-error {{global function 'takesParallelSequences(t:u:)' requires the types 'String' and 'Int' be equivalent}}

// Same-shape requirements

func zip<each T, each U>(t: repeat each T, u: repeat each U) -> (repeat (each T, each U)) {}
// expected-note@-1 {{'zip(t:u:)' declared here}}

let _ = zip()  // ok
let _ = zip(t: 1, u: "hi")  // ok
let _ = zip(t: 1, 2, u: "hi", "hello")  // ok
let _ = zip(t: 1, 2, 3, u: "hi", "hello", "greetings")  // ok
let _ = zip(t: 1, u: "hi", "hello", "greetings")  // expected-error {{extra arguments at positions #3, #4 in call}}
// expected-error@-1 {{global function 'zip(t:u:)' requires the type packs 'Pack{Int}' and 'Pack{String, String, String}' have the same shape}}

func goodCallToZip<each T, each U>(t: repeat each T, u: repeat each U) where (repeat (each T, each U)): Any {
  _ = zip(t: repeat each t, u: repeat each u)
}

func badCallToZip<each T, each U>(t: repeat each T, u: repeat each U) {
  _ = zip(t: repeat each t, u: repeat each u)
  // expected-error@-1 {{global function 'zip(t:u:)' requires the type packs 'each T' and 'each U' have the same shape}}
}
