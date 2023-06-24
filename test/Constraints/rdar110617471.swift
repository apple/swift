// RUN: %target-typecheck-verify-swift

// rdar://110617471: Make sure we can type-check this.
class C {
  var prop = 0
}

func foo(_ fn: () -> Void) {}

class D {
  let c = C()

  func bar() {
    foo { [c] in
      foo {
        switch 0 {
        case c.prop:
          break
        default:
          break
        }
      }
    }
  }
}
