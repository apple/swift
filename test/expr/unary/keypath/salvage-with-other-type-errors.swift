// RUN: %target-typecheck-verify-swift

// Ensure that key path exprs can tolerate being re-type-checked when necessary
// to diagnose other errors in adjacent exprs.

struct P<T: K> { }

struct S {
    init<B>(_ a: P<B>) { // expected-note {{in call to initializer}}
        fatalError()
    }
}

protocol K { }

func + <Object>(lhs: KeyPath<A, Object>, rhs: String) -> P<Object> {
    fatalError()
}

// expected-error@+1{{type 'String' does not conform to protocol 'K'}}
func + (lhs: KeyPath<A, String>, rhs: String) -> P<String> {
    fatalError()
}

struct A {
    let id: String
}

extension A: K {
  static let j = S(\A.id + "id") // expected-error {{generic parameter 'B' could not be inferred}}
  // expected-error@-1 {{binary operator '+' cannot be applied to operands of type 'KeyPath<A, String>' and 'String'}}
  // expected-note@-2 {{overloads for '+' exist with these partially matching parameter lists: (String, String)}}
}

// https://github.com/apple/swift/issues/47610

struct B {
    let v: String
    func f1<T, E>(block: (T) -> E) -> B {
        return self
    }

    func f2<T, E: Equatable>(keyPath: KeyPath<T, E>) {
    }
}
func f3() {
    B(v: "").f1(block: { _ in }).f2(keyPath: \B.v) // expected-error{{cannot infer type of closure parameter '_' without a type annotation}}
}

// https://github.com/apple/swift/issues/47949

protocol Bindable: class { }

extension Bindable {
  func test<Value>(to targetKeyPath: ReferenceWritableKeyPath<Self, Value>, change: Value?) {
    // There is also a note attached to declaration - requirement from conditional conformance of 'Optional<Value>' to 'Equatable'
    if self[keyPath:targetKeyPath] != change {
      // expected-error@-1 {{operator function '!=' requires that 'Value' conform to 'Equatable'}}
      self[keyPath: targetKeyPath] = change!
    }
  }
}
