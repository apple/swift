// (1) Onone, no evolution

// RUN: %empty-directory(%t-onone)

// RUN: %target-swift-frontend %S/swift-class-in-cxx.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t-onone/class.h -Onone

// RUN: %target-interop-build-clangxx -c %s -I %t-onone -o %t-onone/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-in-cxx.swift -o %t-onone/swift-class-execution -Xlinker %t-onone/swift-class-execution.o -module-name Class -Xfrontend -entry-point-function-name -Xfrontend swiftMain -Onone

// RUN: %target-codesign %t-onone/swift-class-execution
// RUN: %target-run %t-onone/swift-class-execution | %FileCheck %s --check-prefixes=CHECK,CHECK-ONONE

// (2) O, no evolution

// RUN: %empty-directory(%t-ofast)

// RUN: %target-swift-frontend %S/swift-class-in-cxx.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t-ofast/class.h -O

// RUN: %target-interop-build-clangxx -c %s -I %t-ofast -o %t-ofast/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-in-cxx.swift -o %t-ofast/swift-class-execution -Xlinker %t-ofast/swift-class-execution.o -module-name Class -Xfrontend -entry-point-function-name -Xfrontend swiftMain -O

// RUN: %target-codesign %t-ofast/swift-class-execution
// RUN: %target-run %t-ofast/swift-class-execution | %FileCheck %s --check-prefixes=CHECK,CHECK-OPT

// (3) Onone, evolution

// RUN: %empty-directory(%t-evo-onone)

// RUN: %target-swift-frontend %S/swift-class-in-cxx.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -enable-library-evolution -emit-clang-header-path %t-evo-onone/class.h -Onone

// RUN: %target-interop-build-clangxx -c %s -I %t-evo-onone -o %t-evo-onone/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-in-cxx.swift -o %t-evo-onone/swift-class-execution-evo -Xlinker %t-evo-onone/swift-class-execution.o -module-name Class -enable-library-evolution -Xfrontend -entry-point-function-name -Xfrontend swiftMain -Onone

// RUN: %target-codesign %t-evo-onone/swift-class-execution-evo
// RUN: %target-run %t-evo-onone/swift-class-execution-evo | %FileCheck %s --check-prefixes=CHECK,CHECK-ONONE

// (4) O, evolution

// RUN: %empty-directory(%t-evo-ofast)

// RUN: %target-swift-frontend %S/swift-class-in-cxx.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -enable-library-evolution -emit-clang-header-path %t-evo-ofast/class.h -O

// RUN: %target-interop-build-clangxx -c %s -I %t-evo-ofast -o %t-evo-ofast/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-in-cxx.swift -o %t-evo-ofast/swift-class-execution-evo -Xlinker %t-evo-ofast/swift-class-execution.o -module-name Class -enable-library-evolution -Xfrontend -entry-point-function-name -Xfrontend swiftMain -O

// RUN: %target-codesign %t-evo-ofast/swift-class-execution-evo
// RUN: %target-run %t-evo-ofast/swift-class-execution-evo | %FileCheck %s --check-prefixes=CHECK,CHECK-OPT

// REQUIRES: executable_test

#include <assert.h>
#include "class.h"
#include <cstdio>
#include <utility>

extern "C" size_t swift_retainCount(void * _Nonnull obj);

size_t getRetainCount(const Class::ClassWithIntField & swiftClass) {
  void *p = swift::_impl::_impl_RefCountedClass::getOpaquePointer(swiftClass);
  return swift_retainCount(p);
}

int main() {
  using namespace Class;

  // Ensure that the class is released.
  {
    auto x = returnClassWithIntField();
    assert(getRetainCount(x) == 1);
  }
// CHECK:      init ClassWithIntField
// CHECK-NEXT: destroy ClassWithIntField
    
  {
    auto x = returnClassWithIntField();
    {
      takeClassWithIntField(x);
      assert(getRetainCount(x) == 1);
      auto x2 = passThroughClassWithIntField(x);
      assert(getRetainCount(x) == 2);
      assert(getRetainCount(x2) == 2);
      takeClassWithIntField(x2);
      assert(getRetainCount(x) == 2);
    }
    assert(getRetainCount(x) == 1);
    takeClassWithIntField(x);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: ClassWithIntField: 0;
// CHECK-NEXT: ClassWithIntField: 42;
// CHECK-NEXT: ClassWithIntField: 42;
// CHECK-NEXT: destroy ClassWithIntField

  {
    auto x = returnClassWithIntField();
    assert(getRetainCount(x) == 1);
    takeClassWithIntFieldInout(x);
    assert(getRetainCount(x) == 1);
    takeClassWithIntField(x);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-ONONE-NEXT: init ClassWithIntField
// CHECK-ONONE-NEXT: destroy ClassWithIntField
// CHECK-OPT-NEXT: destroy ClassWithIntField
// CHECK-OPT-NEXT: init ClassWithIntField
// CHECK-NEXT: ClassWithIntField: -11;
// CHECK-NEXT: destroy ClassWithIntField

  {
    auto x = returnClassWithIntField();
    {
      auto x2 = x;
      assert(getRetainCount(x) == 2);
    }
    assert(getRetainCount(x) == 1);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: destroy ClassWithIntField

  {
    auto x = returnClassWithIntField();
    {
      auto x2 = returnClassWithIntField();
      assert(getRetainCount(x2) == 1);
      assert(getRetainCount(x) == 1);
      x = x2;
      assert(getRetainCount(x) == 2);
    }
    takeClassWithIntField(x);
    assert(getRetainCount(x) == 1);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: destroy ClassWithIntField
// CHECK-NEXT: ClassWithIntField: 0;
// CHECK-NEXT: destroy ClassWithIntField

  {
    auto x = returnClassWithIntField();
    assert(getRetainCount(x) == 1);
    ClassWithIntField x2(std::move(x));
    // Moving a Swift class in C++ is not consuming.
    assert(getRetainCount(x) == 2);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: destroy ClassWithIntField

  {
      auto x = returnClassWithIntField();
      auto x2 = returnClassWithIntField();
      assert(getRetainCount(x) == 1);
      assert(getRetainCount(x2) == 1);
      x2 = std::move(x);
      // Moving a Swift class in C++ is not consuming.
      assert(getRetainCount(x) == 2);
      assert(getRetainCount(x2) == 2);
      takeClassWithIntField(x2);
  }
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: init ClassWithIntField
// CHECK-NEXT: destroy ClassWithIntField
// CHECK-NEXT: ClassWithIntField: 0;
// CHECK-NEXT: destroy ClassWithIntField
  return 0;
}
