// RUN: %target-typecheck-verify-swift

struct borrowing {}
struct consuming {}

struct Foo {}

func foo(x: borrowing Foo) {}
func bar(x: consuming Foo) {}
func baz(x: (borrowing Foo, consuming Foo) -> ()) {}

func bad(x: borrowing borrowing Foo) {} // expected-error{{at most one}}
func worse(x: borrowing consuming Foo) {} // expected-error{{at most one}}
func worst(x: (borrowing consuming Foo) -> ()) {} // expected-error{{at most one}}

// `borrowing` and `consuming` are contextual keywords, so they should also
// continue working as type and/or parameter names

func zim(x: borrowing) {}
func zang(x: consuming) {}
func zung(x: borrowing consuming) {}
func zip(x: consuming borrowing) {}
func zap(x: (borrowing, consuming) -> ()) {}
func zoop(x: (borrowing consuming, consuming borrowing) -> ()) {}

func worster(x: borrowing borrowing borrowing) {} // expected-error{{at most one}}
func worstest(x: (borrowing borrowing borrowing) -> ()) {} // expected-error{{at most one}}

// Parameter specifier names are regular identifiers in other positions,
// including argument labels.

func argumentLabel(borrowing consuming: Int) {}
func argumentLabel(consuming borrowing: Int) {}
func argumentLabel(__shared __owned: Int) {}
func argumentLabel(__owned __shared: Int) {}

// We should parse them as argument labels in function types, even though that
// isn't currently supported.

func argumentLabel(borrowingInClosure: (borrowing consuming: Int) -> ()) {} // expected-error{{function types cannot have argument labels}}
func argumentLabel(consumingInClosure: (consuming borrowing: Int) -> ()) {} // expected-error{{function types cannot have argument labels}}
func argumentLabel(sharedInClosure: (__shared __owned: Int) -> ()) {} // expected-error{{function types cannot have argument labels}}
func argumentLabel(ownedInClosure: (__owned __shared: Int) -> ()) {} // expected-error{{function types cannot have argument labels}}

func argumentLabel(anonBorrowingInClosure: (_ borrowing: Int) -> ()) {}
func argumentLabel(anonConsumingInClosure: (_ consuming: Int) -> ()) {}
func argumentLabel(anonSharedInClosure: (_ __shared: Int) -> ()) {}
func argumentLabel(anonOwnedInClosure: (_ __owned: Int) -> ()) {}

struct MethodModifiers {
    mutating func mutating() {}
    borrowing func borrowing() {}
    consuming func consuming() {}
    nonmutating func nonmutating() {}
    __consuming func __consuming() {}

    mutating borrowing func tooManyA() {} // expected-error{{method must not be declared both 'mutating' and 'borrowing'}}
    nonmutating borrowing func tooManyB() {} // expected-error{{method must not be declared both 'nonmutating' and 'borrowing'}}
    borrowing consuming func tooManyC() {} // expected-error{{method must not be declared both 'borrowing' and 'consuming'}}
    borrowing mutating consuming func tooManyD() {} // expected-error 2 {{method must not be declared both }}
}

func chalk(_ a: consuming String,
           _ b: borrowing [Int],
           _ c: __shared [String],
           _ d: __owned Int?)
           {}

struct Stepping {
    consuming func perform() {}
    borrowing func doIt() {}
  mutating func change() {}
  var ex: Int {
    __consuming get { 0 }
  }
}

class Clapping {
    consuming func perform() {}
    borrowing func doIt() {}
  var ex: Int {
    __consuming get { 0 }
  }
}

protocol Popping {
    consuming func perform()
    borrowing func doIt()
  mutating func change()
  var ex: Int {
    __consuming get
  }
}

enum Exercising {
    consuming func perform() {}
    borrowing func doIt() {}
  mutating func change() {}
  var ex: Int {
    __consuming get { 0 }
  }
}

func consumingClosure1(_ f: consuming () -> ()) { } // expected-error {{'consuming' cannot be applied to nonescaping closure}}
func consumingClosure2(_ f: consuming @escaping () -> ()) { }

func borrowingClosure1(_ f: borrowing () -> ()) { }
func borrowingClosure2(_ f: borrowing @escaping () -> ()) { }

