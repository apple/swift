#ifndef TEST_INTEROP_CXX_CLASS_INPUTS_MUTABLE_MEMBERS_H
#define TEST_INTEROP_CXX_CLASS_INPUTS_MUTABLE_MEMBERS_H

#ifdef USE_MUTATING
// Note: in actuality, this will be included
// as <swift/bridging>, but in this test we include
// it directly.
#include "bridging"
#else
#define SWIFT_MUTATING
#endif

struct HasPublicMutableMember {
  mutable int a = 0;

  int foo() const SWIFT_MUTATING {
    a++;
    return a;
  }
};

struct HasPrivateMutableMember {
private:
  mutable int a = 0;

public:
  void bar() const SWIFT_MUTATING { a++; }
};

#endif // TEST_INTEROP_CXX_CLASS_INPUTS_MUTABLE_MEMBERS_H
