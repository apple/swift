// RUN: %target-typecheck-verify-swift

protocol Derived<A, B> where C == any Derived<Never, B> {
  associatedtype A
  associatedtype B

  associatedtype C
}
