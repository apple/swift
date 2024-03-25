// RUN: %target-run-simple-swift | %FileCheck %s
// REQUIRES: executable_test

let c = UInt8(ascii: "a")

// CHECK: OK
if c == "a" {
  print("OK")
}

// CHECK: OK
if c != "b" {
  print("OK")
}

// CHECK: OK
switch c {
case "a":
  print("OK")
case "b":
  fallthrough
default:
  print("FAIL")
}

let d: UInt8? = UInt8(ascii: "a")

// CHECK: OK
if d == "a" {
  print("OK")
}

// CHECK: OK
if d != "b" {
  print("OK")
}

// CHECK: OK
switch d {
case "a":
  print("OK")
case "b":
  fallthrough
default:
  print("FAIL")
}

// CHECK: OK
if UInt8(unicode: "a") == "a" {
  print("OK")
}

// CHECK: 128512
print(UInt64(unicode: "😀"))
