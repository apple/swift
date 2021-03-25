// RUN: %target-swift-frontend -emit-sil %s -verify

struct User {
  var id: Int
  var name: String
}

func setting<Root, Value>(_ kp: WritableKeyPath<Root, Value>, _ root: Root, _ value: Value) -> Root {
  var copy = root
  // Should not warn about lack of mutation
  copy[keyPath: kp] = value
  return copy
}

func referenceSetting<Root, Value>(_ kp: ReferenceWritableKeyPath<Root, Value>, _ root: Root, _ value: Value) -> Root {
  // Should warn about lack of mutation, since a RefKeyPath doesn't modify its
  // base.
  // expected-warning@+1 {{was never mutated}}
  var copy = root
  copy[keyPath: kp] = value
  
  // Should not warn about lack of use of `immCopy`
  let immCopy = root
  immCopy[keyPath: kp] = value
  return copy
}

func referenceUsage<Root, Value>(_ kp: ReferenceWritableKeyPath<Root, Value>, _ root: Root, _ value: Value) -> Root {
  // Should warn about lack of mutation, since a RefKeyPath doesn't modify its
  // base.
  // expected-warning@+1 {{was never mutated}}
  var copy = root
  copy[keyPath: kp] = value
  return copy
}
