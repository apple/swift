#ifndef TEST_INTEROP_CXX_CLASS_INPUTS_MEMBERWISE_INITIALIZER_H
#define TEST_INTEROP_CXX_CLASS_INPUTS_MEMBERWISE_INITIALIZER_H

template <typename T>
struct TemplatedType {};


struct StructPrivateOnly {
private:
  int varPrivate;
};

struct StructPublicOnly {
  int varPublic;
};

struct StructEmptyPrivateSection {
  int varPublic;
private:
};

struct StructPublicAndPrivate {
  int varPublic;
private:
  int varPrivate;
};

struct StructWithUnimportedMemberFunction {
  int varPublic;
  int StructWithUnimportedMemberFunction::* unimportedMemberFunction();
};

class ClassPrivateOnly {
  int varPrivate;
};

class ClassPublicOnly {
public:
  int varPublic;
};

class ClassEmptyPublicSection {
  int varPrivate;
public:
};

class ClassPrivateAndPublic {
  int varPrivate;
public:
  int varPublic;
};

struct ClassWithUnimportedMemberFunction {
public:
  int varPublic;
  int ClassWithUnimportedMemberFunction::* unimportedMemberFunction();
};

struct ClassWithTemplatedFunction {
public:
  int varPublic;

  template <int I>
  void foo();
};

struct ClassWithTemplatedUsingDecl {
public:
  int varPublic;

  template <typename T>
  using MyUsing = TemplatedType<T>;
};

#endif
