// Statically link a program with concurrency module
// REQUIRES: static_stdlib
// REQUIRES: concurrency
// REQUIRES: libdispatch_static

// RUN: %empty-directory(%t)
// RUN: echo 'public func asyncFunc() async { print("Hello") }' > %t/asyncModule.swift

// RUN: %target-swiftc_driver -emit-library -emit-module -module-name asyncModule -module-link-name asyncModule %t/asyncModule.swift -static -static-stdlib -o %t/libasyncModule.a
// RUN: %target-swiftc_driver -parse-as-library -static -static-stdlib -module-name main %s %import-static-libdispatch -I%t -L%t -o %t/main

// RUN: %t/main | %FileCheck %s
// CHECK: Hello

// RUN: if [ %target-os == "linux-gnu" ]; \
// RUN: then \
// RUN:   ldd %t/main | %FileCheck %s --check-prefix=LDD; \
// RUN: fi

// LDD-NOT: libswiftCore.so
// LDD-NOT: libswift_Concurrency.so

import asyncModule

@main
struct Main {
  static func main() async {
    await asyncFunc()
  }
}
