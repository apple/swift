#ifndef TEST_INTEROP_CXX_CLASS_INPUTS_TYPE_CLASSIFICATION_H
#define TEST_INTEROP_CXX_CLASS_INPUTS_TYPE_CLASSIFICATION_H

struct EmptyStruct {};

// Tests for individual special members

struct StructWithDefaultConstructor {
  StructWithDefaultConstructor() {}
};

struct StructWithAdditionalConstructor {
  StructWithAdditionalConstructor() {}
  StructWithAdditionalConstructor(int parameter) {}
};

struct __attribute__((swift_attr("import_unsafe"))) StructWithCopyConstructor {
  StructWithCopyConstructor(const StructWithCopyConstructor &) {}
};

struct StructWithInheritedCopyConstructor : StructWithCopyConstructor {};

struct StructWithSubobjectCopyConstructor {
  StructWithCopyConstructor subobject;
};

struct StructWithDefaultedCopyConstructor {
  StructWithDefaultedCopyConstructor(
      const StructWithDefaultedCopyConstructor &) = default;
};

struct StructWithInheritedDefaultedCopyConstructor
    : StructWithDefaultedCopyConstructor {};

struct StructWithSubobjectDefaultedCopyConstructor {
  StructWithDefaultedCopyConstructor subobject;
};

struct StructWithPrivateDefaultedCopyConstructor {
private:
  StructWithPrivateDefaultedCopyConstructor(
      const StructWithPrivateDefaultedCopyConstructor &) = default;
};

struct StructWithInheritedPrivateDefaultedCopyConstructor
    : StructWithPrivateDefaultedCopyConstructor {};

struct StructWithSubobjectPrivateDefaultedCopyConstructor {
  StructWithPrivateDefaultedCopyConstructor subobject;
};

struct StructWithMoveConstructor {
  StructWithMoveConstructor(StructWithMoveConstructor &&) {}
};

struct StructWithInheritedMoveConstructor : StructWithMoveConstructor {};

struct StructWithSubobjectMoveConstructor {
  StructWithMoveConstructor subobject;
};

struct __attribute__((swift_attr("import_unsafe"))) StructWithCopyAssignment {
  StructWithCopyAssignment &operator=(const StructWithCopyAssignment &);
};

struct StructWithInheritedCopyAssignment : StructWithCopyAssignment {};

struct StructWithSubobjectCopyAssignment {
  StructWithCopyAssignment subobject;
};

struct StructWithMoveAssignment {
  StructWithMoveAssignment &operator=(StructWithMoveAssignment &&);
};

struct StructWithInheritedMoveAssignment : StructWithMoveAssignment {};

struct StructWithSubobjectMoveAssignment {
  StructWithMoveAssignment subobject;
};

struct __attribute__((swift_attr("import_unsafe"))) StructWithDestructor {
  ~StructWithDestructor() {}
};

struct StructWithInheritedDestructor : StructWithDestructor {};

struct StructWithSubobjectDestructor {
  StructWithDestructor subobject;
};

struct StructWithDefaultedDestructor {
  ~StructWithDefaultedDestructor() = default;
};

struct StructWithInheritedDefaultedDestructor : StructWithDefaultedDestructor {
};

struct StructWithSubobjectDefaultedDestructor {
  StructWithDefaultedDestructor subobject;
};

struct StructWithPrivateDefaultedDestructor {
private:
  ~StructWithPrivateDefaultedDestructor() = default;
};

struct StructWithInheritedPrivateDefaultedDestructor
    : StructWithPrivateDefaultedDestructor {};

struct StructWithSubobjectPrivateDefaultedDestructor {
  StructWithPrivateDefaultedDestructor subobject;
};

struct StructWithDeletedDestructor {
  ~StructWithDeletedDestructor() = delete;
};

struct StructWithInheritedDeletedDestructor
    : StructWithDeletedDestructor {};

struct StructWithSubobjectDeletedDestructor {
  StructWithDeletedDestructor subobject;
};

// Tests for common sets of special member functions.

struct StructTriviallyCopyableMovable {
  StructTriviallyCopyableMovable(const StructTriviallyCopyableMovable &) =
      default;
  StructTriviallyCopyableMovable(StructTriviallyCopyableMovable &&) = default;
  StructTriviallyCopyableMovable &
  operator=(const StructTriviallyCopyableMovable &) = default;
  StructTriviallyCopyableMovable &
  operator=(StructTriviallyCopyableMovable &&) = default;
  ~StructTriviallyCopyableMovable() = default;
};

struct StructNonCopyableTriviallyMovable {
  StructNonCopyableTriviallyMovable(const StructNonCopyableTriviallyMovable &) =
      delete;
  StructNonCopyableTriviallyMovable(StructNonCopyableTriviallyMovable &&) =
      default;
  StructNonCopyableTriviallyMovable &
  operator=(const StructNonCopyableTriviallyMovable &) = delete;
  StructNonCopyableTriviallyMovable &
  operator=(StructNonCopyableTriviallyMovable &&) = default;
  ~StructNonCopyableTriviallyMovable() = default;
};

struct StructNonCopyableNonMovable {
  StructNonCopyableNonMovable(const StructNonCopyableNonMovable &) = delete;
  StructNonCopyableNonMovable(StructNonCopyableNonMovable &&) = default;
  StructNonCopyableNonMovable &
  operator=(const StructNonCopyableNonMovable &) = delete;
  StructNonCopyableNonMovable &
  operator=(StructNonCopyableNonMovable &&) = default;
  ~StructNonCopyableNonMovable() = default;
};

struct StructDeletedDestructor {
  StructDeletedDestructor(const StructDeletedDestructor &) = default;
  StructDeletedDestructor(StructDeletedDestructor &&) = default;
  StructDeletedDestructor &operator=(const StructDeletedDestructor &) = default;
  StructDeletedDestructor &operator=(StructDeletedDestructor &&) = default;
  ~StructDeletedDestructor() = delete;
};

struct __attribute__((swift_attr("import_owned")))
StructWithCopyConstructorAndValue {
  int value;
  StructWithCopyConstructorAndValue() : value(0) {}
  StructWithCopyConstructorAndValue(int value) : value(value) {}
  StructWithCopyConstructorAndValue(
      const StructWithCopyConstructorAndValue &other)
      : value(other.value) {}
};

struct StructWithSubobjectCopyConstructorAndValue {
  StructWithCopyConstructorAndValue member;
};

struct __attribute__((swift_attr("import_owned")))
StructWithCopyConstructorAndSubobjectCopyConstructorAndValue {
  StructWithCopyConstructorAndValue member;
  StructWithCopyConstructorAndSubobjectCopyConstructorAndValue(
      StructWithCopyConstructorAndValue member)
      : member(member) {}
  StructWithCopyConstructorAndSubobjectCopyConstructorAndValue(
      const StructWithCopyConstructorAndSubobjectCopyConstructorAndValue &other)
      : member(other.member) {}
};

template<class> struct DependentParent { struct Child { }; };

struct HasUnsupportedUsingShadow : DependentParent<int> {
  using typename DependentParent<int>::Child;
};

struct __attribute__((swift_attr("import_iterator"))) Iterator {
  int idx;
};

struct HasMethodThatReturnsIterator {
  Iterator getIterator() const;
};

struct IteratorBox {
  Iterator it;
};

struct HasMethodThatReturnsIteratorBox {
  IteratorBox getIteratorBox() const;
};

#endif // TEST_INTEROP_CXX_CLASS_INPUTS_TYPE_CLASSIFICATION_H
