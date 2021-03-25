// RUN: %target-typecheck-verify-swift

class A1 {
  func foo1() -> Int {}
  func foo2() {
    var foo1 = foo1()
  }
}

class A2 {
  var foo1 = 2
  func foo2() {
    var foo1 = foo1
  }
}

class A3 {
  func foo2() {
    var foo1 = foo1()
  }
  func foo1() -> Int {}
}

class A4 {
  func foo2() {
    var foo1 = foo1 // expected-error {{use of local variable 'foo1' before its declaration}}
    // expected-note@-1 {{'foo1' declared here}}
  }
}

func localContext() {
  class A5 {
    func foo1() -> Int {}
    func foo2() {
      var foo1 = foo1()
    }

    class A6 {
      func foo1() -> Int {}
      func foo2() {
        var foo1 = foo1()
      }
    }

    extension E { // expected-error {{declaration is only valid at file scope}}
      // expected-error@-1{{cannot find type 'E' in scope}}
      class A7 {
        func foo1() {}
        func foo2() {
          var foo1 = foo1()
        }
      }
    }
  }
}
