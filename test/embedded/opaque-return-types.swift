// RUN: %target-swift-emit-ir %s -enable-experimental-feature Embedded | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: OS=macosx || OS=linux-gnu || OS=windows-msvc || OS=windows-msvc

protocol Proto { }

struct MyStruct: Proto { }

func foo() -> some Proto {
  MyStruct()
}

// CHECK: define {{.*}}@main(
