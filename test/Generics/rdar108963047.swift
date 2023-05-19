// RUN: %target-typecheck-verify-swift

protocol HasElt {
  associatedtype Element
}
struct IntElement : HasElt {
  typealias Element = Int
}
struct FloatElement : HasElt {
  typealias Element = Float
}

struct G<T: HasElt> where T.Element == Float {
  func foo() where T == IntElement {} // expected-error {{generic signature requires types 'IntElement.Element' (aka 'Int') and 'Float' to be the same}}
}
