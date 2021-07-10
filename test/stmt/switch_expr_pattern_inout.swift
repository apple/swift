// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

func ~= (pattern: inout Int, value: Int) -> Bool {
  pattern = value
  return true
}

var x = 0

switch 49 {
case x:
  print("yup")
default:
  print("nope")
}

// CHECK: nope
// CHECK: 0
print(x)
