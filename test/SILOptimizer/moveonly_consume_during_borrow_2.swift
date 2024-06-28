// RUN: %target-swift-frontend -emit-sil -verify %s

struct Box<Wrapped: ~Copyable>: ~Copyable { }

struct Tree<Element> {
  struct Node: ~Copyable{
    typealias Link = Box<Self>?
    var left: Link
  }
}

extension Tree.Node {
    consuming func balance() -> Self { // expected-error 2 {{'self' used after consume}}
        switch left {
        case nil:
          return self // expected-note{{}}
        case .some(let box): // expected-warning{{}}
          return self // expected-note{{}}
        }
    } // expected-note 2 {{}}
}

