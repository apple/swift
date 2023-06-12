// RUN: %empty-directory(%t)

// RUN: mkdir -p %t/swift-root/libexec/swift %t/swift-root/bin
// RUN: touch %t/swift-root/libexec/swift/Foo
// RUN: touch %t/swift-root/libexec/swift/Foo.exe
// RUN: touch %t/swift-root/bin/Foo.exe

// RUN: %target-clang %s -std=c++11 -I %swift_src_root/include -I %swift_obj_root/include -I %swift_src_root/stdlib/public/SwiftShims -I %clang-include-dir -isysroot %sdk -L%swift-lib-dir/swift/%target-sdk-name/%target-arch -L%llvm_obj_root/lib/swift/%target-sdk-name/%target-arch -lswiftCore -o %t/paths-test
// RUN: %target-codesign %t/paths-test
// RUN: %target-run %t/paths-test | %FileCheck %s
// RUN: env %env-SWIFT_ROOT=%t/swift-root %target-run %t/paths-test | %FileCheck %s --check-prefix CHECK-FR
// REQUIRES: executable_test

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

// This can't be done in unittests, because that statically links the runtime
// so we get the wrong paths.  We explicitly want to test that we get the
// path we expect (that is, the path to the runtime, and paths relative to
// that).

#include "swift/Runtime/Paths.h"

#include <sys/types.h>
#include <sys/stat.h>

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#if defined(_WIN32) && !defined(__CYGWIN)
#define stat _stat
#define S_IFDIR _S_IFDIR
#endif

static bool
exists(const char *path) {
  struct stat st;

  return stat(path, &st) == 0;
}

static bool
isfile(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 && !(st.st_mode & S_IFDIR);
}

static bool
isdir(const char *path) {
  struct stat st;

  return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static bool
containsLibSwift(const char *path) {
  const char *posix = "/lib/swift/";
  const char *windows = "\\lib\\swift\\";

  return strstr(path, posix) || strstr(path, windows);
}

int main(void) {
  const char *runtimePath = swift_getRuntimeLibraryPath();

  // Runtime path must point to libswiftCore and must be a file.

  // CHECK: runtime path: {{.*[\\/](lib)?}}swiftCore.{{so|dylib|dll}}
  // CHECK-NEXT: runtime is a file: yes

  // CHECK-FR: runtime path: {{.*[\\/](lib)?}}swiftCore.{{so|dylib|dll}}
  // CHECK-FR-NEXT: runtime is a file: yes
  printf("runtime path: %s\n", runtimePath ? runtimePath: "<NULL>");
  printf("runtime is a file: %s\n", isfile(runtimePath) ? "yes" : "no");

  const char *rootPath = swift_getRootPath();

  // Root path must end in a separator and must be a directory

  // CHECK: root path: {{.*[\\/]$}}
  // CHECK-NEXT: root is a directory: yes

  // CHECK-FR: root path: {{.*[\\/]$}}
  // CHECK-FR-NEXT: root is a directory: yes
  printf("root path: %s\n", rootPath ? rootPath : "<NULL>");
  printf("root is a directory: %s\n", isdir(rootPath) ? "yes" : "no");

  // CHECK: root path contains /lib/swift/: no
  // CHECK-FR: root path contains /lib/swift/: no
  printf("root path contains /lib/swift/: %s\n",
         containsLibSwift(rootPath) ? "yes" : "no");

  const char *auxPath = swift_copyAuxiliaryExecutablePath("Foo");

  // CHECK: aux path: <NULL>
  // CHECK-FR: aux path: {{.*[\\/]libexec[\\/]swift[\\/]Foo(\.exe)?}}

  printf("aux path: %s\n", auxPath ? auxPath : "<NULL>");

  free((void *)auxPath);

  return 0;
}
