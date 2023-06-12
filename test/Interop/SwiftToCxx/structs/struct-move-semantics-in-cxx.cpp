// RUN: %empty-directory(%t)

// RUN: %target-swift-frontend %S/large-structs-pass-return-indirect-in-cxx.swift -typecheck -module-name Structs -clang-header-expose-decls=all-public -emit-clang-header-path %t/structs.h

// Link should fail by default when a move is
// performed in C++.
// RUN: %target-interop-build-clangxx -c %s -I %t -o %t/swift-structs-execution.o
// RUN: not %target-interop-build-swift %S/large-structs-pass-return-indirect-in-cxx.swift -o %t/swift-structs-execution -Xlinker %t/swift-structs-execution.o -module-name Structs -Xfrontend -entry-point-function-name -Xfrontend swiftMain 2>&1 | %FileCheck --check-prefix=LINK %s

// LINK: fatalError_Cxx_move_of_Swift_value_type_not_supported_yet

// Compile should fail by default when move assignment is attempted in C++:

// RUN: not %target-interop-build-clangxx -c %s -I %t -o %t/swift-structs-execution.o -DMOVE_ASSIGN 2>&1 | %FileCheck --check-prefix=MOVEASSIGN %s

// Fallback to abort at runtime:

// RUN: %target-interop-build-clangxx -c %s -I %t -o %t/swift-structs-execution.o -DLINKS
// RUN: %target-interop-build-swift %S/large-structs-pass-return-indirect-in-cxx.swift -o %t/swift-structs-execution -Xlinker %t/swift-structs-execution.o -module-name Structs -Xfrontend -entry-point-function-name -Xfrontend swiftMain 2>&1

// RUN: %target-codesign %t/swift-structs-execution
// RUN: %target-run %t/swift-structs-execution 2>%t/output || true
// RUN: cat %t/output  | %FileCheck %s

// REQUIRES: executable_test

#include <assert.h>
#include "structs.h"
#include <memory>

#ifdef LINKS
extern "C" void _fatalError_Cxx_move_of_Swift_value_type_not_supported_yet() {
    
}
#endif

int main() {
  using namespace Structs;

  auto x = returnNewStructSeveralI64(42);
#ifdef MOVE_ASSIGN
  auto y = returnNewStructSeveralI64(24);
  x = std::move(y);
// MOVEASSIGN: deleted operator '='
#else
  StructSeveralI64 x2 = std::move(x);
#endif
  return 0;
}

// CHECK: C++ does not support moving a Swift value yet
