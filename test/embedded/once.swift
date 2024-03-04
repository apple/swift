// RUN: %target-run-simple-swift(-enable-experimental-feature Embedded -parse-as-library -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -enable-experimental-feature Embedded -parse-as-library -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop) | %FileCheck %s
// RUN: %target-run-simple-swift(-O -lto=llvm-full %lto_flags -enable-experimental-feature Embedded -parse-as-library -runtime-compatibility-version none -wmo -Xfrontend -disable-objc-interop) | %FileCheck %s

// REQUIRES: swift_in_compiler
// REQUIRES: executable_test
// REQUIRES: optimized_stdlib
// REQUIRES: OS=macosx || OS=linux-gnu
// UNSUPPORTED: linker_overridden

// For LTO, the linker dlopen()'s the libLTO library, which is a scenario that
// ASan cannot work in ("Interceptors are not working, AddressSanitizer is
// loaded too late").
// REQUIRES: no_asan

public struct MyStruct {
  static var singleton = MyStruct()

  init() {
    print("MyStruct.init")
  }

  func foo() {
    print("MyStruct.foo")
  }
}

@main
struct Main {
  static func main() {
    MyStruct.singleton.foo()
    // CHECK: MyStruct.init
    // CHECK: MyStruct.foo
  }
}
