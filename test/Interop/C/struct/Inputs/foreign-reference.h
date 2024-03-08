#ifndef TEST_INTEROP_C_INPUTS_FOREIGN_REFERENCE_H
#define TEST_INTEROP_C_INPUTS_FOREIGN_REFERENCE_H

#include <stdlib.h>

#if __has_feature(nullability)
// Provide macros to temporarily suppress warning about the use of
// _Nullable and _Nonnull.
# define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS                                   \
  _Pragma("clang diagnostic push")                                             \
  _Pragma("clang diagnostic ignored \"-Wnullability-extension\"")              \
  _Pragma("clang assume_nonnull begin")
# define SWIFT_END_NULLABILITY_ANNOTATIONS                                     \
  _Pragma("clang diagnostic pop")                                              \
  _Pragma("clang assume_nonnull end")

#else
// #define _Nullable and _Nonnull to nothing if we're not being built
// with a compiler that supports them.
# define _Nullable
# define _Nonnull
# define _Null_unspecified
# define SWIFT_BEGIN_NULLABILITY_ANNOTATIONS
# define SWIFT_END_NULLABILITY_ANNOTATIONS
#endif

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

struct
    __attribute__((swift_attr("import_reference")))
    __attribute__((swift_attr("retain:LCRetain")))
    __attribute__((swift_attr("release:LCRelease")))
LocalCount {
  int value;
};

static inline struct LocalCount *createLocalCount() {
  struct LocalCount *ptr = malloc(sizeof(struct LocalCount));
  ptr->value = 1;
  return ptr;
}

static inline void LCRetain(struct LocalCount *x) { x->value++; }
static inline void LCRelease(struct LocalCount *x) { x->value--; }

static int globalCount = 0;

struct
    __attribute__((swift_attr("import_reference")))
    __attribute__((swift_attr("retain:GCRetain")))
    __attribute__((swift_attr("release:GCRelease")))
GlobalCount {};

static inline struct GlobalCount *createGlobalCount() {
  globalCount++;
  return malloc(sizeof(struct GlobalCount));
}

static inline void GCRetain(struct GlobalCount *x) { globalCount++; }
static inline void GCRelease(struct GlobalCount *x) { globalCount--; }

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // TEST_INTEROP_C_INPUTS_FOREIGN_REFERENCE_H
