#ifndef TEST_INTEROP_CXX_CLASS_INPUTS_DESTRUCTORS_H
#define TEST_INTEROP_CXX_CLASS_INPUTS_DESTRUCTORS_H

struct DummyStruct {};

struct __attribute__((swift_attr("import_unsafe")))
HasUserProvidedDestructorAndDummy {
  DummyStruct dummy;
  HasUserProvidedDestructorAndDummy(DummyStruct dummy) : dummy(dummy) {}
#if __is_target_os(windows)
  // On windows, force this type to be address-only.
  HasUserProvidedDestructorAndDummy(const HasUserProvidedDestructorAndDummy &) {}
#endif
  ~HasUserProvidedDestructorAndDummy() {}
};

struct __attribute__((swift_attr("import_unsafe"))) HasUserProvidedDestructor {
  int *value;
#if __is_target_os(windows)
  // On windows, force this type to be address-only.
  HasUserProvidedDestructor() {}
  HasUserProvidedDestructor(const HasUserProvidedDestructor &other) {}
#endif
  ~HasUserProvidedDestructor() { *value = 42; }
};

struct HasNonTrivialImplicitDestructor {
  HasUserProvidedDestructor member;
};

#endif // TEST_INTEROP_CXX_CLASS_INPUTS_DESTRUCTORS_H
