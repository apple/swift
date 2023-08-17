// RUN: %target-typecheck-verify-swift

// rdar://112264204: Make sure we can type-check this.
func foo(_ fn: (Int) -> Void) {}

func bar(_ x: Int) {
  foo { [x] y in
    switch y {
    case x:
      ()
    default:
      ()
    }
  }
}
