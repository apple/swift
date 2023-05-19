// RUN: %target-typecheck-verify-swift -warn-redundant-requirements

// REQUIRES: objc_interop

// rdar://problem/30610428
@objc protocol P14 { }

class X12<S: AnyObject> {
  func bar<V>(v: V) where S == P14 { // expected-warning {{redundant constraint 'S' : 'AnyObject'}}
  }
}

@objc protocol P15: P14 { }

class X13<S: P14> {
  func bar<V>(v: V) where S == P15 { // expected-warning {{redundant conformance constraint 'any P15' : 'P14'}}
  }
}

