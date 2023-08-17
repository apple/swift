// RUN: %target-swift-frontend -emit-sil -sdk %S/../SILGen/Inputs %s -I %S/../SILGen/Inputs -enable-source-import -parse-stdlib -o /dev/null -verify

// REQUIRES: objc_interop

import Swift
import gizmo

@requires_stored_property_inits
class RequiresInitsDerived : Gizmo {
  var a = 1
  var b = 2
  var c = 3

  override init() {
    super.init()
  }

  init(i: Int) {
    if i > 0 {
      super.init()
    }
  } // expected-error{{'super.init' isn't called on all paths before returning from initializer}}

  init(d: Double) {
    f() // expected-error {{'self' used in method call 'f' before 'super.init' call}}
    super.init()
  }

  init(t: ()) {
    a = 5 // expected-error {{'self' used in property access 'a' before 'super.init' call}}
    b = 10 // expected-error {{'self' used in property access 'b' before 'super.init' call}}
    super.init()
    c = 15
  }

  func f() { }
}
