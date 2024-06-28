// RUN: %empty-directory(%t)
// RUN: split-file %s %t

// RUN: %target-swift-frontend -typecheck %t/use-cxx-types.swift -typecheck -module-name UseCxx -emit-clang-header-path %t/UseCxx.h -I %t -enable-experimental-cxx-interop -clang-header-expose-decls=all-public -disable-availability-checking

// RUN: %target-interop-build-clangxx -std=c++20 -c %t/use-swift-cxx-types.cpp -I %t -o %t/swift-cxx-execution.o
// RUN: %target-interop-build-swift %t/use-cxx-types.swift -o %t/swift-cxx-execution -Xlinker %t/swift-cxx-execution.o -module-name UseCxx -Xfrontend -entry-point-function-name -Xfrontend swiftMain -I %t -O -Xfrontend -disable-availability-checking

// RUN: %target-codesign %t/swift-cxx-execution
// RUN: %target-run %t/swift-cxx-execution | %FileCheck %s

// REQUIRES: executable_test

//--- header.h

#include <stdio.h>

struct Trivial {
    int x, y;

    inline Trivial(int x, int y) : x(x), y(y) {}
};

template<class T>
struct NonTrivialTemplate {
    T x;

    inline NonTrivialTemplate(T x) : x(x) {
        puts("create NonTrivialTemplate");
    }
    inline NonTrivialTemplate(const NonTrivialTemplate<T> &other) : x(other.x) {
        puts("copy NonTrivialTemplate");
    }
    inline NonTrivialTemplate(NonTrivialTemplate<T> &&other) : x(static_cast<T &&>(other.x)) {
        puts("move NonTrivialTemplate");
    }
    inline ~NonTrivialTemplate() {
        puts("~NonTrivialTemplate");
    }
    inline void testPrint() const {
        puts("testPrint");
    }
};

using NonTrivialTemplateTrivial = NonTrivialTemplate<Trivial>;

class ImmortalFRT {
public:
    int x;
} __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal")));

class SharedFRT {
public:
  SharedFRT() {}
  SharedFRT(int x) : x(x) {}
  int x;
} __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:retainShared")))
__attribute__((swift_attr("release:releaseShared")));

inline void retainShared(SharedFRT *r) { puts("retainShared"); }
inline void releaseShared(SharedFRT *r) { puts("releaseShared"); }

//--- module.modulemap
module CxxTest {
    header "header.h"
    requires cplusplus
}

//--- use-cxx-types.swift
import CxxTest

public func consumeNonTrivial(_ x: consuming NonTrivialTemplateTrivial) -> CInt {
    print("x and y: \(x.x.x), \(x.x.y)")
    return x.x.x
}

public struct TakesNonTrivial {
    public init(_ x: NonTrivialTemplateTrivial) {
        self.prop = x
    }

    public var prop: NonTrivialTemplateTrivial
}

public func consumeImmortalFRT(_ x: consuming ImmortalFRT) {
  print("immortal frt x \(x.x)")
}

public
func consumeSharedFRT(_ x : consuming SharedFRT) {
  print("consume shared frt x \(x.x)")
}

public
func takeSharedFRT(_ x : SharedFRT) { print("take shared frt x \(x.x)") }

public
func returnSharedFRT(_ x : SharedFRT) -> SharedFRT {
  print("return shared frt x \(x.x)")
  return x
}

//--- use-swift-cxx-types.cpp

#include "header.h"
#include "UseCxx.h"
#include <assert.h>

int main() {
  {
    auto x = NonTrivialTemplate(Trivial(1, 2));
    UseCxx::consumeNonTrivial(x);
    puts("DoneCall");
  }
// CHECK: create NonTrivialTemplate
// CHECK-NEXT: copy NonTrivialTemplate
// CHECK-NEXT: x and y: 1, 2
// CHECK-NEXT: ~NonTrivialTemplate
// CHECK-NEXT: DoneCall
// CHECK-NEXT: ~NonTrivialTemplate
  {
    auto x = NonTrivialTemplate(Trivial(-4, 0));
    puts("call");
    auto swiftVal = UseCxx::TakesNonTrivial::init(x);
    puts("DoneCall");
    swiftVal.setProp(x);
  }
// CHECK-NEXT: create NonTrivialTemplate
// CHECK-NEXT: call
// CHECK-NEXT: copy NonTrivialTemplate
// CHECK-NEXT: copy NonTrivialTemplate
// CHECK-NEXT: ~NonTrivialTemplate
// CHECK-NEXT: DoneCall
// CHECK-NEXT: copy NonTrivialTemplate
// CHECK-NEXT: ~NonTrivialTemplate
// CHECK-NEXT: copy NonTrivialTemplate
// CHECK-NEXT: ~NonTrivialTemplate
// CHECK-NEXT: ~NonTrivialTemplate
// CHECK-NEXT: ~NonTrivialTemplate
  {
    ImmortalFRT frt;
    frt.x = 2;
    UseCxx::consumeImmortalFRT(&frt);
  }
  // CHECK-NEXT: immortal frt x 2
  {
    SharedFRT sfrt;
    sfrt.x = 2;
    UseCxx::takeSharedFRT(&sfrt);
    // CHECK-NEXT: retainShared
    // CHECK-NEXT: releaseShared
    // CHECK-NEXT: take shared frt x 2
    UseCxx::consumeSharedFRT(&sfrt);
    // CHECK-NEXT: retainShared
    // CHECK-NEXT: releaseShared
    // CHECK-NEXT: consume shared frt x 2
    SharedFRT *sfrtptr = UseCxx::returnSharedFRT(&sfrt);
    // CHECK-NEXT: retainShared
    // CHECK-NEXT: return shared frt x 2
  }
  puts("EndOfTest");
// CHECK-NEXT: EndOfTest
  return 0;
}
