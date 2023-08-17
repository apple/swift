// RUN: %empty-directory(%t)
// RUN: %target-clang %s -std=c++11 -isysroot %sdk -o %t/main
// RUN: %target-codesign %t/main
// RUN: %target-swift-frontend -c %S/Inputs/entry-point-section/main.swift -O -o %t/howdy.o -module-name Howdy
// RUN: %target-ld %t/howdy.o -syslibroot %sdk -lSystem -dylib -o %t/libHowdy.dylib
// RUN: %target-codesign %t/libHowdy.dylib
// RUN: %target-run %t/main %t/libHowdy.dylib | %FileCheck %s

// REQUIRES: VENDOR=apple
// REQUIRES: executable_test
// UNSUPPORTED: remote_run
// UNSUPPORTED: asan

#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <mach-o/getsect.h>
#include <ptrauth.h>
#include <stdio.h>
#include <string.h>

#if __POINTER_WIDTH__ == 64
using mach_header_platform = mach_header_64;
#else
using mach_header_platform = mach_header;
#endif

int main(int argc, char *argv[]) {
  if (argc != 2) {
    printf("no argument!\n");
    return 1;
  }
  auto *dylibPath = argv[1];
  auto *handle = dlopen(dylibPath, RTLD_LAZY);
  if (!handle) {
    printf("no library!\n");
    return 1;
  }

  using MainFunction = void(int, char *[]);
  MainFunction *mainFunction = nullptr;
  for (int index = 0; index < _dyld_image_count(); ++index) {
    auto *imageName = _dyld_get_image_name(index);
    if (strcmp(dylibPath, imageName)) {
      printf("skipping %s\n", imageName);
      continue;
    }
    auto *header = reinterpret_cast<const mach_header_platform *>(
        _dyld_get_image_header(index));
    size_t size;
    auto *data = getsectiondata(header, "__TEXT", "__swift5_entry", &size);
    int32_t offset = *reinterpret_cast<int32_t *>(data);
    if (size >= 8) {
      auto flags = *(reinterpret_cast<int32_t *>(data) + 1);
      enum EntryPointFlags : unsigned {
        HasAtMainTypeFlag = 1 << 0,
      };
      printf("flags: %d\n", flags);
      bool isAtMainEntryPoint = flags & HasAtMainTypeFlag;
      if (!isAtMainEntryPoint) {
        printf("no @main entry point!\n"); // CHECK: no @main entry point!
        continue;
      }
    }
    mainFunction = reinterpret_cast<MainFunction *>(
      ptrauth_sign_unauthenticated(
          reinterpret_cast<void *>(
              reinterpret_cast<long>(data) + offset
          ),
          ptrauth_key_function_pointer,
          ptrauth_function_pointer_type_discriminator(MainFunction)
      )
    );


    break;
  }
  if (!mainFunction) {
    printf("no function!\n"); // CHECK: no function!
    return 0;
  }
  mainFunction(argc, argv);
  return 1;
}
