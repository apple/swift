// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

struct Context {
    unowned(unsafe) var x: AnyObject? = nil
}

@inline(never)
func test(x: Context) -> Context? {
  return x
}

// CHECK: works
if (test(x: Context()) == nil) {
    print("bug")
} else {
    print("works")
}

@inline(never)
func test2() -> Context {
  var g: [Context]  = [Context()]
  print(g.endIndex)
  return g.removeLast()
}

// CHECK: Context(x: nil)
print(test2())
