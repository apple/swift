// End-to-end test of -experimental-hermetic-seal-at-link flag.

// RUN: %empty-directory(%t)

// (1) Build library swiftmodule
// RUN: %use_just_built_liblto %target-build-swift %s -DLIBRARY -module-name Library -experimental-hermetic-seal-at-link -lto=llvm-full %lto_flags \
// RUN:     -Xfrontend -disable-reflection-metadata -Xfrontend -disable-reflection-names -Xfrontend -disable-objc-interop \
// RUN:     -emit-library -static -o %t/libLibrary.a \
// RUN:     -emit-module -emit-module-path %t/Library.swiftmodule

// (2) Check that libLibrary.a does actually provide all its public interfaces
// RUN: %llvm-nm %t/libLibrary.a | %FileCheck %s --check-prefix CHECK-NM-LIB

// (3) Build client
// RUN: %use_just_built_liblto %target-build-swift %s -DCLIENT -parse-as-library -module-name Main -experimental-hermetic-seal-at-link -lto=llvm-full %lto_flags \
// RUN:     -Xfrontend -disable-reflection-metadata -Xfrontend -disable-reflection-names -Xfrontend -disable-objc-interop \
// RUN:     -I%t -L%t -lLibrary -o %t/main
// RUN: %target-codesign %t/main

// (4) Check that unused symbols are not present in final executable
// RUN: %llvm-nm %t/main | %FileCheck %s --check-prefix CHECK-NM-EXEC

// (5) Execute
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test

// FIXME(mracek): More work needed to get this to work on non-Apple platforms.
// REQUIRES: VENDOR=apple

// rdar://85476542 (https://ci.swift.org/job/oss-swift-incremental-ASAN-RA-macos/6089 failure)
// UNSUPPORTED: asan

#if LIBRARY

  // The only symbol that's actually used by client code
  public func used_func() {
    print("used_func")
  }

  public class MyUnusedClass {}
  public class MyUnusedSubClass: MyUnusedClass {}
  public protocol MyUnusedProtocol {}
  public struct MyUnusedStruct {}
  public struct MyUnusedStruct2: MyUnusedProtocol {}
  public enum MyUnusedEnum {}
  public func MyUnusedFunc() {}

  // (2) In libLibrary.a, all exported symbols are present...
  // CHECK-NM-LIB-DAG: MyUnusedClass
  // CHECK-NM-LIB-DAG: MyUnusedSubClass
  // CHECK-NM-LIB-DAG: MyUnusedProtocol
  // CHECK-NM-LIB-DAG: MyUnusedStruct
  // CHECK-NM-LIB-DAG: MyUnusedStruct2
  // CHECK-NM-LIB-DAG: MyUnusedEnum
  // CHECK-NM-LIB-DAG: MyUnusedFunc

  // (4) ... but after linking the main executable, they are removed.
  // CHECK-NM-EXEC-NOT: MyUnused

#endif  // LIBRARY

#if CLIENT

  import Library

  @_cdecl("main")
  func main() -> Int32 {
     used_func()
     print("Done")
     // CHECK: used_func
     // CHECK-NEXT: Done
     return 0
  }

#endif  // CLIENT
