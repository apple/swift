// RUN: %empty-directory(%t)
// RUN: %clang -c -v %target-cc-options %target-threading-opt -g -O0 -isysroot %sdk %S/Inputs/isPrespecialized.cpp -o %t/isPrespecialized.o -I %clang-include-dir -I %swift_src_root/include/ -I %swift_src_root/stdlib/public/SwiftShims/ -I %llvm_src_root/include -I %llvm_obj_root/include -L %clang-include-dir/../lib/swift/macosx

// RUN: %target-build-swift -v %mcp_opt %s %t/isPrespecialized.o -import-objc-header %S/Inputs/isPrespecialized.h -Xfrontend -prespecialize-generic-metadata -target %module-target-future -lc++ -L %clang-include-dir/../lib/swift/macosx -sdk %sdk -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-run %t/main

// REQUIRES: executable_test
// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: swift_test_mode_optimize
// UNSUPPORTED: swift_test_mode_optimize_size

func ptr<T>(to ty: T.Type) -> UnsafeMutableRawPointer {
    UnsafeMutableRawPointer(mutating: unsafePointerToMetadata(of: ty))!
}

func unsafePointerToMetadata<T>(of ty: T.Type) -> UnsafePointer<T.Type> {
  unsafeBitCast(ty, to: UnsafePointer<T.Type>.self)
}

@inline(never)
func assertIsPrespecialized<T>(_ t: T.Type, is prespecialized: Bool) {
  assert(isCanonicalStaticallySpecializedGenericMetadata(ptr(to: t)) == prespecialized)
}

@inline(never)
func assertIsPrespecialized<T>(clazzArgument: T.Type, is prespecialized: Bool) {
  assertIsPrespecialized(Clazz<T>.self, is: prespecialized)
}

class Clazz<T> {
  let value: T

  init(value: T) {
    self.value = value
  }
}

func doit() {
  assertIsPrespecialized(Clazz<Int>.self, is: true)
  assertIsPrespecialized(clazzArgument: Int.self, is: true) // Clazz<Int> is prespecialized by the preceding call

  assertIsPrespecialized(clazzArgument: Double.self, is: false) // Clazz<Double> is not prespecialized
}
doit()
