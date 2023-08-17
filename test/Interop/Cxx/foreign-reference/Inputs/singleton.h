#ifndef TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_SINGLETON_H
#define TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_SINGLETON_H

#include <stdlib.h>
#include <new>

#include "visibility.h"

SWIFT_BEGIN_NULLABILITY_ANNOTATIONS

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) DeletedDtor {
  int value = 42;

  DeletedDtor() = default;
  DeletedDtor(const DeletedDtor &) = default;
  DeletedDtor(DeletedDtor &&) = default;
  ~DeletedDtor() = delete;

  int test() const { return value; }
  int testMutable() { return value; }

  static DeletedDtor *create() {
    return new (malloc(sizeof(DeletedDtor))) DeletedDtor();
  }
};

void mutateIt(DeletedDtor &x) { x.value = 32; }

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) PrivateDtor {
  int value = 42;

  PrivateDtor() = default;
  PrivateDtor(const PrivateDtor &) = default;
  PrivateDtor(PrivateDtor &&) = default;

  int test() const { return value; }
  int testMutable() { return value; }

  static PrivateDtor *create() {
    return new (malloc(sizeof(PrivateDtor))) PrivateDtor();
  }

private:
  ~PrivateDtor() {}
};

void mutateIt(PrivateDtor &x) { x.value = 32; }

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) DeletedSpecialMembers {
  int value = 42;

  DeletedSpecialMembers() = default;
  DeletedSpecialMembers(const DeletedSpecialMembers &) = delete;
  DeletedSpecialMembers(DeletedSpecialMembers &&) = delete;
  ~DeletedSpecialMembers() = delete;

  int test() const { return value; }
  int testMutable() { return value; }

  static DeletedSpecialMembers *create() {
    return new (malloc(sizeof(DeletedSpecialMembers))) DeletedSpecialMembers();
  }
};

void mutateIt(DeletedSpecialMembers &x) { x.value = 32; }

struct __attribute__((swift_attr("import_reference")))
__attribute__((swift_attr("retain:immortal")))
__attribute__((swift_attr("release:immortal"))) PrivateSpecialMembers {
  int value = 42;

  PrivateSpecialMembers() = default;

  int test() const { return value; }
  int testMutable() { return value; }

  static PrivateSpecialMembers *create() {
    return new (malloc(sizeof(PrivateSpecialMembers))) PrivateSpecialMembers();
  }

private:
  PrivateSpecialMembers(const PrivateSpecialMembers &) = default;
  PrivateSpecialMembers(PrivateSpecialMembers &&) = default;
  ~PrivateSpecialMembers() = default;
};

void mutateIt(PrivateSpecialMembers &x) { x.value = 32; }

SWIFT_END_NULLABILITY_ANNOTATIONS

#endif // TEST_INTEROP_CXX_FOREIGN_REFERENCE_INPUTS_SINGLETON_H
