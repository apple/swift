#ifndef TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CUSTOM_DESTRUCTORS_H
#define TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CUSTOM_DESTRUCTORS_H

struct __attribute__((swift_attr("import_unsafe"))) HasUserProvidedDestructor {
  int *value;
  HasUserProvidedDestructor() {}
  HasUserProvidedDestructor(int *value) : value(value) {}
#if __is_target_os(windows) && !defined(WIN_TRIVIAL)
  // On windows, force this type to be address-only.
  HasUserProvidedDestructor(const HasUserProvidedDestructor &other) : value(other.value) {}
#endif
  ~HasUserProvidedDestructor() { *value = 42; }
};

struct __attribute__((swift_attr("import_unsafe")))
HasEmptyDestructorAndMemberWithUserDefinedConstructor {
  HasUserProvidedDestructor member;
  ~HasEmptyDestructorAndMemberWithUserDefinedConstructor() { /* empty */
  }
};

struct HasNonTrivialImplicitDestructor {
  HasUserProvidedDestructor member;
};

struct HasNonTrivialDefaultedDestructor {
  HasUserProvidedDestructor member;
  ~HasNonTrivialDefaultedDestructor() = default;
};

struct HasDefaultedDestructor {
  ~HasDefaultedDestructor() = default;
};

// For the following objects with virtual bases / destructors, make sure that
// any executable user of these objects disable rtti and exceptions. Otherwise,
// the linker will error because of undefined vtables.
// FIXME: Once we can link with libc++ we can enable RTTI.

struct __attribute__((swift_attr("import_unsafe"))) HasVirtualBaseAndDestructor
    : virtual HasDefaultedDestructor {
  int *value;
  HasVirtualBaseAndDestructor(int *value) : value(value) {}
  ~HasVirtualBaseAndDestructor() { *value = 42; }
};

struct __attribute__((swift_attr("import_unsafe"))) HasVirtualDestructor {
  // An object with a virtual destructor requires a delete operator in case
  // we try to delete the base object. Until we can link against libc++, use
  // this dummy implementation.
  static void operator delete(void *p) { __builtin_unreachable(); }
  virtual ~HasVirtualDestructor(){};
};

struct __attribute__((swift_attr("import_unsafe")))
HasVirtualDefaultedDestructor {
  static void operator delete(void *p) { __builtin_unreachable(); }
  virtual ~HasVirtualDefaultedDestructor() = default;
};

struct __attribute__((swift_attr("import_unsafe"))) HasBaseWithVirtualDestructor
    : HasVirtualDestructor {
  int *value;
  HasBaseWithVirtualDestructor(int *value) : value(value) {}
  ~HasBaseWithVirtualDestructor() { *value = 42; }
};

struct __attribute__((swift_attr("import_unsafe")))
HasVirtualBaseWithVirtualDestructor : virtual HasVirtualDestructor {
  int *value;
  HasVirtualBaseWithVirtualDestructor(int *value) : value(value) {}
  ~HasVirtualBaseWithVirtualDestructor() { *value = 42; }
};

struct DummyStruct {};

struct __attribute__((swift_attr("import_unsafe")))
HasUserProvidedDestructorAndDummy {
  DummyStruct dummy;
  HasUserProvidedDestructorAndDummy(DummyStruct dummy) : dummy(dummy) {}
#if __is_target_os(windows) && !defined(WIN_TRIVIAL)
  // On windows, force this type to be address-only.
  HasUserProvidedDestructorAndDummy(const HasUserProvidedDestructorAndDummy &other) : dummy(other.dummy) {}
#endif
  ~HasUserProvidedDestructorAndDummy() {}
};

// Make sure that we don't crash on struct templates with destructors.
template <typename T>
struct __attribute__((swift_attr("import_unsafe"))) S {
  ~S() {}
};

#endif // TEST_INTEROP_CXX_VALUE_WITNESS_TABLE_INPUTS_CUSTOM_DESTRUCTORS_H
