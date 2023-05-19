// Statically link a "hello world" program
// REQUIRES: OS=linux-gnu
// REQUIRES: static_stdlib
print("hello world!")
// RUN: %empty-directory(%t)
// RUN: %target-swiftc_driver %import-static-libdispatch -static-stdlib -o %t/static-stdlib %s
// RUN: %t/static-stdlib | %FileCheck %s
// RUN: ldd %t/static-stdlib | %FileCheck %s --check-prefix=LDD
// CHECK: hello world!
// LDD-NOT: libswiftCore.so 
