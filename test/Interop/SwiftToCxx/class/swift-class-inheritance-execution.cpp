// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/swift-class-inheritance-in-cxx.swift -typecheck -module-name Class -clang-header-expose-decls=all-public -emit-clang-header-path %t/class.h

// RUN: %target-interop-build-clangxx -c %s -I %t -o %t/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-inheritance-in-cxx.swift -o %t/swift-class-execution -Xlinker %t/swift-class-execution.o -module-name Class -Xfrontend -entry-point-function-name -Xfrontend swiftMain

// RUN: %target-codesign %t/swift-class-execution
// RUN: %target-run %t/swift-class-execution | %FileCheck %s

// RUN: not %target-interop-build-clangxx -c %s -I %t -o %t/swift-class-execution.o -DERROR1
// RUN: not %target-interop-build-clangxx -c %s -I %t -o %t/swift-class-execution.o -DERROR2

// RUN: %empty-directory(%t-evo)

// RUN: %target-swift-frontend %S/swift-class-inheritance-in-cxx.swift -typecheck -module-name Class -enable-library-evolution -clang-header-expose-decls=all-public -emit-clang-header-path %t-evo/class.h

// RUN: %target-interop-build-clangxx -c %s -I %t-evo -o %t-evo/swift-class-execution.o
// RUN: %target-interop-build-swift %S/swift-class-inheritance-in-cxx.swift -o %t-evo/swift-class-execution-evo -Xlinker %t-evo/swift-class-execution.o -module-name Class -enable-library-evolution -Xfrontend -entry-point-function-name -Xfrontend swiftMain

// RUN: %target-codesign %t-evo/swift-class-execution-evo
// RUN: %target-run %t-evo/swift-class-execution-evo | %FileCheck %s

// REQUIRES: executable_test

#include <assert.h>
#include "class.h"

using namespace Class;

int passByValue(BaseClass b) { return 0; }
int passByValue(DerivedClass d) { return 1; }

int passByRef(const BaseClass &b) { return 0; }
int passByRef(const DerivedClass &d){ return 1; }

int passByRef2(const BaseClass &b) { return 0; }
int passByRef2(const DerivedClass &d){ return 1; }
int passByRef2(const DerivedDerivedClass &d){ return 2; }

extern "C" size_t swift_retainCount(void * _Nonnull obj);

size_t getRetainCount(const BaseClass &swiftClass) {
  return swift_retainCount(swift::_impl::_impl_RefCountedClass::getOpaquePointer(swiftClass));
}

#ifdef ERROR1
void checkNotCompile1(BaseClass b) {
  useDerivedClass(b);
}
#endif

#ifdef ERROR2
void checkNotCompile2(SiblingDerivedClass b) {
  DerivedClass c = b;
}
#endif

int main() {
  {
    auto x = returnDerivedClass();
    useBaseClass(x);
    useDerivedClass(x);
    assert(passByValue(x) == 1);
    assert(passByRef(x) == 1);
    {
      assert(getRetainCount(x) == 1);
      BaseClass baseCopy = x;
      useBaseClass(baseCopy);
      assert(getRetainCount(x) == 2);
      baseCopy = x;
      assert(getRetainCount(x) == 2);
    }
  }
// CHECK: init BaseClass
// CHECK-NEXT: init DerivedClass
// CHECK-NEXT: useBaseClass, type=Class.DerivedClass
// CHECK-NEXT: useDerivedClass, type=Class.DerivedClass
// CHECK-NEXT: useBaseClass, type=Class.DerivedClass
// CHECK-NEXT: destroy DerivedClass
// CHECK-NEXT: destroy BaseClass

  {
    auto x = returnDerivedDerivedClass();
    useBaseClass(x);
    useDerivedClass(x);
    assert(passByValue(x) == 1);
    assert(passByRef(x) == 1);
    assert(passByRef2(x) == 2);
  }
// CHECK-NEXT: init BaseClass
// CHECK-NEXT: init DerivedClass
// CHECK-NEXT: init DerivedDerivedClass
// CHECK-NEXT: useBaseClass, type=Class.DerivedDerivedClass
// CHECK-NEXT: useDerivedClass, type=Class.DerivedDerivedClass
// CHECK-NEXT: destroy DerivedDerivedClass
// CHECK-NEXT: destroy DerivedClass
// CHECK-NEXT: destroy BaseClass

  {
    BaseClass x = returnDerivedClass();
    assert(getRetainCount(x) == 1);
    useBaseClass(x);
  }
// CHECK-NEXT: init BaseClass
// CHECK-NEXT: init DerivedClass
// CHECK-NEXT: useBaseClass, type=Class.DerivedClass
// CHECK-NEXT: destroy DerivedClass
// CHECK-NEXT: destroy BaseClass
  return 0;
}
