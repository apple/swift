// RUN: %empty-directory(%t)
// RUN: %target-build-swift -Xfrontend -conditional-runtime-records %s -emit-ir -o %t/main.ll
// RUN: %target-clang %t/main.ll -isysroot %sdk -L%swift-lib-dir/swift/%target-sdk-name -flto -o %t/main
// RUN: %target-swift-reflection-dump %t/main | %FileCheck %s

// FIXME(mracek): More work needed to get this to work on non-Apple platforms.
// REQUIRES: VENDOR=apple

// For LTO, the linker dlopen()'s the libLTO library, which is a scenario that
// ASan cannot work in ("Interceptors are not working, AddressSanitizer is
// loaded too late").
// REQUIRES: no_asan

public protocol TheProtocol {
}

public class Class: TheProtocol {
}

public struct Struct {
}

public enum Enum {
}

// CHECK: FIELDS:
// CHECK: =======
// CHECK: main.TheProtocol
// CHECK: main.Class
// CHECK: main.Struct
// CHECK: main.Enum
