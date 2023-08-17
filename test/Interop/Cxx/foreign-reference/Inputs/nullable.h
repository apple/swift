#ifndef TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_NULLABLE_H
#define TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_NULLABLE_H

#include <stdlib.h>
#include <new>

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) Empty {
  int test() const { return 42; }

  static Empty *create() { return new (malloc(sizeof(Empty))) Empty(); }
};

void mutateIt(Empty &) {}

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) IntPair {
  int a = 1;
  int b = 2;

  int test() const { return b - a; }

  static IntPair *create() { return new (malloc(sizeof(IntPair))) IntPair(); }
};

void mutateIt(IntPair *x) {
  x->a = 2;
  x->b = 4;
}

#endif // TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_NULLABLE_H
