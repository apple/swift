// Tests that under -enable-llvm-wme, LLVM GlobalDCE is able to remove unused
// witness methods, and that used witness methods are not removed (by running
// the program).

// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -enable-llvm-wme %s -Onone -emit-ir -o %t/main.ll -Xfrontend -enable-relative-protocol-witness-tables
// RUN: %target-clang %t/main.ll -isysroot %sdk -L%swift-lib-dir/swift/%target-sdk-name -flto -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main | %FileCheck %s

// RUN: %llvm-nm --defined-only %t/main | %FileCheck %s --check-prefix=NM

// REQUIRES: executable_test

// FIXME(mracek): More work needed to get this to work on non-Apple platforms.
// REQUIRES: VENDOR=apple

// For LTO, the linker dlopen()'s the libLTO library, which is a scenario that
// ASan cannot work in ("Interceptors are not working, AddressSanitizer is
// loaded too late").
// REQUIRES: no_asan

protocol TheProtocol {
  func func1_live()
  func func2_dead()
}

struct MyStruct : TheProtocol {
  func func1_live() { print("MyStruct.func1_live") }
  func func2_dead() { print("MyStruct.func2_dead") }
}

let x: TheProtocol = MyStruct()
x.func1_live()
// CHECK: MyStruct.func1_live

// NM:     $s4main8MyStructV10func1_liveyyF
// NM-NOT: $s4main8MyStructV10func2_deadyyF
