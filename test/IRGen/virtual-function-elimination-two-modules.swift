// Tests that under -enable-llvm-vfe + -internalize-at-link, cross-module
// virtual calls are done via thunks and LLVM GlobalDCE is able to remove unused
// virtual methods from a library based on a list of used symbols by a client.

// RUN: %empty-directory(%t)

// (1) Build library swiftmodule
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe \
// RUN:     %s -DLIBRARY -module-name Library \
// RUN:     -emit-module -o %t/Library.swiftmodule \
// RUN:     -emit-tbd -emit-tbd-path %t/libLibrary.tbd -Xfrontend -tbd-install_name=%t/libLibrary.dylib

// (2) Build client
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe \
// RUN:     %s -DCLIENT -module-name Main -I%t -L%t -lLibrary -o %t/main

// (3) Extract a list of used symbols by client from library
// RUN: %llvm-nm --undefined-only -m %t/main | grep 'from libLibrary' | awk '{print $3}' > %t/used-symbols

// (4) Now produce the .dylib with just the symbols needed by the client
// RUN: %target-build-swift -parse-as-library -Xfrontend -enable-llvm-vfe -Xfrontend -internalize-at-link \
// RUN:     %s -DLIBRARY -lto=llvm-full %lto_flags -module-name Library \
// RUN:     -emit-library -o %t/libLibrary.dylib -runtime-compatibility-version none \
// RUN:     -Xlinker -exported_symbols_list -Xlinker %t/used-symbols -Xlinker -dead_strip

// (5) Check list of symbols in library
// RUN: %llvm-nm --defined-only %t/libLibrary.dylib | %FileCheck %s --check-prefix=NM

// (6) Execution test
// RUN: %target-run %t/main | %FileCheck %s

// REQUIRES: executable_test

// FIXME(mracek): More work needed to get this to work on non-Apple platforms.
// REQUIRES: VENDOR=apple

// For LTO, the linker dlopen()'s the libLTO library, which is a scenario that
// ASan cannot work in ("Interceptors are not working, AddressSanitizer is
// loaded too late").
// REQUIRES: no_asan

// Remote test execution does not support dynamically loaded libraries.
// UNSUPPORTED: remote_run

#if LIBRARY

public class MyClass {
  public init() {}
  public func foo() { print("MyClass.foo") }
  public func bar() { print("MyClass.bar") }
}

public class MyDerivedClass: MyClass {
  override public func foo() { print("MyDerivedClass.foo") }
  override public func bar() { print("MyDerivedClass.bar") }
}

// NM-NOT: $s7Library14MyDerivedClassC3baryyF
// NM:     $s7Library14MyDerivedClassC3fooyyF
// NM-NOT: $s7Library7MyClassC3baryyF
// NM-NOT: $s7Library7MyClassC3fooyyF   // This fails, even though MyClass.foo clearly can't be called!

// The only reason the bar implementations can be stripped is because they aren't called anywhere, through
// any class.

#endif

#if CLIENT

import Library

@_cdecl("main")
func main() -> Int32 {
   let o = MyDerivedClass()
   o.foo()
   print("Done")
   // CHECK: MyDerivedClass.foo
   // CHECK-NEXT: Done
   return 0
}

#endif
