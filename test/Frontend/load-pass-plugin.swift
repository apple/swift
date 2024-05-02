// REQUIRES: OS=macosx

// RUN: %target-swift-frontend -load-pass-plugin=nonexistent.dylib %s -emit-ir -o /dev/null 2>&1 | %FileCheck -check-prefix=CHECK-UNABLE-LOAD %s
// CHECK-UNABLE-LOAD: error: unable to load plugin 'nonexistent.dylib': 'Could not load library{{.*}}'

// RUN: %empty-directory(%t)
// RUN: %target-clangxx %S/Inputs/TestPlugin.cpp -std=c++17 -stdlib=libc++ \
// RUN:     -isysroot %sdk -I %llvm_src_root/include -I %llvm_obj_root/include -L %llvm_obj_root/lib -lLLVMSupport \
// RUN:     -Wl,-undefined -Wl,suppress -Wl,-flat_namespace \
// RUN:     -dynamiclib -o %t/libTestPlugin.dylib

// RUN: %target-swift-frontend -load-pass-plugin=%t/libTestPlugin.dylib %s -emit-ir -o /dev/null 2>&1 | %swift-demangle | %FileCheck %s
// CHECK: TestPlugin: main
// CHECK: TestPlugin: null.empty() -> ()

func empty() {}
