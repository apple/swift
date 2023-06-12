// RUN: %empty-directory(%t)
// RUN: %target-clang %s -isysroot %sdk -L%swift-lib-dir/swift/%target-sdk-name -lswiftCore -lobjc -o %t/objc-getclass
// RUN: %target-codesign %t/objc-getclass
// RUN: %target-run %t/objc-getclass %S/Inputs/objc-getclass.txt

// REQUIRES: executable_test
// REQUIRES: objc_interop

// UNSUPPORTED: use_os_stdlib
// UNSUPPORTED: back_deployment_runtime

#include <objc/runtime.h>
#include <dlfcn.h>
#include <stdio.h>
#include <ctype.h>
#include <errno.h>
#include <string.h>

static BOOL dummyHook(const char * _Nonnull name,
                      Class _Nullable * _Nonnull outClass) {
  return NO;
}

int main(int argc, char **argv) {
  if (argc != 2) {
    fprintf(stderr, "usage: objc-getclass <input.txt>\n"
            "\n"
            "Test demangling class names to get classes.\n");
    return 0;
  }

  // Find the class-by-mangled-name hook
  objc_hook_getClass getObjCClassByMangledName = NULL;

  if (__builtin_available(macOS 10.14.4, iOS 12.2, tvOS 12.2, watchOS 5.2, *)) {
    objc_hook_getClass dummy = NULL;
    objc_setHook_getClass(dummyHook, &getObjCClassByMangledName);
    objc_setHook_getClass(getObjCClassByMangledName, &dummy);
  } else {
    fprintf(stderr, "objc-getclass: OS version is too old\n");
    return 0;
  }

  // Open the input file
  FILE *fp = fopen(argv[1], "rt");
  if (!fp) {
    fprintf(stderr, "objc-getclass: unable to open \"%s\" - %s\n",
            argv[1], strerror(errno));
  }

  // Input file is a list of manglings; we don't really care what classes they
  // resolve to here; this test is about whether or not they actually crash or
  // assert.
  char *line = NULL;
  size_t linecap = 0;
  ssize_t linelen = 0;

  while ((linelen = getline(&line, &linecap, fp)) > 0) {
    char *mangling = line;

    // Trim whitespace
    while (isspace(*mangling))
      ++mangling;

    char *end = line + linelen;
    while (end > line && isspace(end[-1]))
      --end;
    *end = '\0';

    // Skip comments and blank lines
    if (*mangling == '#' || !*mangling)
      continue;

    // Try to get a class
    Class outClass = nil;
    BOOL result = getObjCClassByMangledName(mangling, &outClass);

    if (result)
      printf("%s -> %s\n", mangling, class_getName(outClass));
    else
      printf("%s not found\n", mangling);
  }

  fclose(fp);

  return 0;
}
