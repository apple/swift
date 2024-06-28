// RUN: %target-typecheck-verify-swift

protocol P1 {
  typealias A = P2
}

protocol P2 {
  associatedtype B
  typealias A1 = Int
  func f1()
}

extension P2 {
  typealias A2 = String
  func f2() {}
}

extension P1 where Self: A {
  typealias B1 = A1
  typealias B2 = A2

  func g() {
    f1()
    f2()
  }
}

// This is terrible and we should ban it some day
extension P1 where Self: A, B: Hashable {
  func h(_: Set<B>) {}
}

// This is also terrible and we should ban it
public protocol P3 {
  associatedtype A
}

public protocol P4: P3 where A == B {}

extension P4 {
  public typealias B = String
}