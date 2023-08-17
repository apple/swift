#ifndef TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_ITERATOR_H
#define TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_ITERATOR_H

#include <cstddef>
#include <iterator>

// MARK: Valid iterator types

struct ConstIterator {
private:
  int value;

public:
  using iterator_category = std::input_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  ConstIterator(int value) : value(value) {}
  ConstIterator(const ConstIterator &other) = default;

  const int &operator*() const { return value; }

  ConstIterator &operator++() {
    value++;
    return *this;
  }
  ConstIterator operator++(int) {
    auto tmp = ConstIterator(value);
    value++;
    return tmp;
  }

  bool operator==(const ConstIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const ConstIterator &other) const {
    return value != other.value;
  }
};

struct ConstRACIterator {
private:
  int value;

public:
  using iterator_category = std::random_access_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  ConstRACIterator(int value) : value(value) {}
  ConstRACIterator(const ConstRACIterator &other) = default;

  const int &operator*() const { return value; }

  ConstRACIterator &operator++() {
    value++;
    return *this;
  }
  ConstRACIterator operator++(int) {
    auto tmp = ConstRACIterator(value);
    value++;
    return tmp;
  }

  void operator+=(difference_type v) { value += v; }
  void operator-=(difference_type v) { value -= v; }
  ConstRACIterator operator+(difference_type v) const {
    return ConstRACIterator(value + v);
  }
  ConstRACIterator operator-(difference_type v) const {
    return ConstRACIterator(value - v);
  }
  friend ConstRACIterator operator+(difference_type v,
                                    const ConstRACIterator &it) {
    return it + v;
  }
  int operator-(const ConstRACIterator &other) const {
    return value - other.value;
  }

  bool operator<(const ConstRACIterator &other) const {
    return value < other.value;
  }

  bool operator==(const ConstRACIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const ConstRACIterator &other) const {
    return value != other.value;
  }
};

// Same as ConstRACIterator, but operator+= returns a reference to this.
struct ConstRACIteratorRefPlusEq {
private:
  int value;

public:
  using iterator_category = std::random_access_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  ConstRACIteratorRefPlusEq(int value) : value(value) {}
  ConstRACIteratorRefPlusEq(const ConstRACIteratorRefPlusEq &other) = default;

  const int &operator*() const { return value; }

  ConstRACIteratorRefPlusEq &operator++() {
    value++;
    return *this;
  }
  ConstRACIteratorRefPlusEq operator++(int) {
    auto tmp = ConstRACIteratorRefPlusEq(value);
    value++;
    return tmp;
  }

  ConstRACIteratorRefPlusEq &operator+=(difference_type v) {
    value += v;
    return *this;
  }
  ConstRACIteratorRefPlusEq &operator-=(difference_type v) {
    value -= v;
    return *this;
  }
  ConstRACIteratorRefPlusEq operator+(difference_type v) const {
    return ConstRACIteratorRefPlusEq(value + v);
  }
  ConstRACIteratorRefPlusEq operator-(difference_type v) const {
    return ConstRACIteratorRefPlusEq(value - v);
  }
  friend ConstRACIteratorRefPlusEq
  operator+(difference_type v, const ConstRACIteratorRefPlusEq &it) {
    return it + v;
  }
  int operator-(const ConstRACIteratorRefPlusEq &other) const {
    return value - other.value;
  }

  bool operator<(const ConstRACIteratorRefPlusEq &other) const {
    return value < other.value;
  }

  bool operator==(const ConstRACIteratorRefPlusEq &other) const {
    return value == other.value;
  }
  bool operator!=(const ConstRACIteratorRefPlusEq &other) const {
    return value != other.value;
  }
};

/// Same as ConstIterator, but defines `operator==` as a non-member.
struct ConstIteratorOutOfLineEq {
  int value;

  using iterator_category = std::input_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = int &;
  using difference_type = int;

  ConstIteratorOutOfLineEq(int value) : value(value) {}
  ConstIteratorOutOfLineEq(const ConstIteratorOutOfLineEq &other) = default;

  const int &operator*() const { return value; }

  ConstIteratorOutOfLineEq &operator++() {
    value++;
    return *this;
  }
  ConstIteratorOutOfLineEq operator++(int) {
    auto tmp = ConstIteratorOutOfLineEq(value);
    value++;
    return tmp;
  }
};

bool operator==(const ConstIteratorOutOfLineEq &lhs,
                const ConstIteratorOutOfLineEq &rhs) {
  return lhs.value == rhs.value;
}

bool operator!=(const ConstIteratorOutOfLineEq &lhs,
                const ConstIteratorOutOfLineEq &rhs) {
  return lhs.value != rhs.value;
}

/// Only satisfies the minimal requirements for an iterator.
struct MinimalIterator {
  int value;
  using iterator_category = std::input_iterator_tag;
  const int &operator*() const { return value; }
  MinimalIterator &operator++() {
    value++;
    return *this;
  }
  bool operator==(const MinimalIterator &other) const {
    return value == other.value;
  }
};

struct ForwardIterator {
  int value;
  using iterator_category = std::forward_iterator_tag;
  using reference = const int &;

  const int &operator*() const { return value; }
  ForwardIterator &operator++() {
    value++;
    return *this;
  }
  ForwardIterator operator++(int) {
    ForwardIterator tmp = {value};
    value++;
    return tmp;
  }
  bool operator==(const ForwardIterator &other) const {
    return value == other.value;
  }
};

struct HasCustomIteratorTag {
  struct CustomTag : public std::input_iterator_tag {};

  int value;
  using iterator_category = CustomTag;
  const int &operator*() const { return value; }
  HasCustomIteratorTag &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasCustomIteratorTag &other) const {
    return value == other.value;
  }
};

struct HasCustomRACIteratorTag {
  struct CustomTag : public std::random_access_iterator_tag {};

  int value;
  using iterator_category = CustomTag;
  const int &operator*() const { return value; }
  HasCustomRACIteratorTag &operator++() {
    value++;
    return *this;
  }
  void operator+=(int x) { value += x; }
  int operator-(const HasCustomRACIteratorTag &x) const {
    return value - x.value;
  }
  bool operator==(const HasCustomRACIteratorTag &other) const {
    return value == other.value;
  }
};

struct HasCustomIteratorTagInline {
  struct iterator_category : public std::input_iterator_tag {};

  int value;
  const int &operator*() const { return value; }
  HasCustomIteratorTagInline &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasCustomIteratorTagInline &other) const {
    return value == other.value;
  }
};

struct HasTypedefIteratorTag {
  int value;
  typedef std::input_iterator_tag iterator_category;
  const int &operator*() const { return value; }
  HasTypedefIteratorTag &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasTypedefIteratorTag &other) const {
    return value == other.value;
  }
};

// MARK: Types that are not actually iterators

struct HasNoIteratorCategory {
  int value;
  const int &operator*() const { return value; }
  HasNoIteratorCategory &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasNoIteratorCategory &other) const {
    return value == other.value;
  }
};

struct HasInvalidIteratorCategory {
  struct NotAnIteratorTag {};

  int value;
  using iterator_category = NotAnIteratorTag;
  const int &operator*() const { return value; }
  HasInvalidIteratorCategory &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasInvalidIteratorCategory &other) const {
    return value == other.value;
  }
};

struct HasNoEqualEqual {
  int value;
  using iterator_category = std::input_iterator_tag;
  const int &operator*() const { return value; }
  HasNoEqualEqual &operator++() {
    value++;
    return *this;
  }
};

struct HasInvalidEqualEqual {
  int value;
  using iterator_category = std::input_iterator_tag;
  const int &operator*() const { return value; }
  HasInvalidEqualEqual &operator++() {
    value++;
    return *this;
  }
  bool operator==(const int &other) const { // wrong type
    return value == other;
  }
};

struct HasNoIncrementOperator {
  int value;
  using iterator_category = std::input_iterator_tag;
  const int &operator*() const { return value; }
  bool operator==(const HasNoIncrementOperator &other) const {
    return value == other.value;
  }
};

struct HasNoPreIncrementOperator {
  int value;
  using iterator_category = std::input_iterator_tag;
  const int &operator*() const { return value; }
  HasNoPreIncrementOperator operator++(int) { // post-increment
    HasNoPreIncrementOperator tmp = {value};
    value++;
    return tmp;
  }
  bool operator==(const HasNoPreIncrementOperator &other) const {
    return value == other.value;
  }
};

struct HasNoDereferenceOperator {
  int value;
  using iterator_category = std::input_iterator_tag;
  HasNoDereferenceOperator &operator++() {
    value++;
    return *this;
  }
  bool operator==(const HasNoDereferenceOperator &other) const {
    return value == other.value;
  }
};

// MARK: Types that claim to be random access iterators, but actually aren't.

struct HasNoMinusOperator {
  int value;

  using iterator_category = std::random_access_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  HasNoMinusOperator(int value) : value(value) {}
  HasNoMinusOperator(const HasNoMinusOperator &other) = default;

  const int &operator*() const { return value; }

  HasNoMinusOperator &operator++() {
    value++;
    return *this;
  }
  void operator+=(difference_type v) { value += v; }
  void operator-=(difference_type v) { value -= v; }
  HasNoMinusOperator operator+(difference_type v) const {
    return HasNoMinusOperator(value + v);
  }
  HasNoMinusOperator operator-(difference_type v) const {
    return HasNoMinusOperator(value - v);
  }
  friend HasNoMinusOperator operator+(difference_type v,
                                      const HasNoMinusOperator &it) {
    return it + v;
  }

  bool operator<(const HasNoMinusOperator &other) const {
    return value < other.value;
  }

  bool operator==(const HasNoMinusOperator &other) const {
    return value == other.value;
  }
  bool operator!=(const HasNoMinusOperator &other) const {
    return value != other.value;
  }
};

struct HasNoPlusEqualOperator {
  int value;

  using iterator_category = std::random_access_iterator_tag;
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  HasNoPlusEqualOperator(int value) : value(value) {}
  HasNoPlusEqualOperator(const HasNoPlusEqualOperator &other) = default;

  const int &operator*() const { return value; }

  HasNoPlusEqualOperator &operator++() {
    value++;
    return *this;
  }
  void operator-=(difference_type v) { value -= v; }
  HasNoPlusEqualOperator operator+(difference_type v) const {
    return HasNoPlusEqualOperator(value + v);
  }
  HasNoPlusEqualOperator operator-(difference_type v) const {
    return HasNoPlusEqualOperator(value - v);
  }
  friend HasNoPlusEqualOperator operator+(difference_type v,
                                          const HasNoPlusEqualOperator &it) {
    return it + v;
  }
  int operator-(const HasNoPlusEqualOperator &other) const {
    return value - other.value;
  }

  bool operator<(const HasNoPlusEqualOperator &other) const {
    return value < other.value;
  }

  bool operator==(const HasNoPlusEqualOperator &other) const {
    return value == other.value;
  }
  bool operator!=(const HasNoPlusEqualOperator &other) const {
    return value != other.value;
  }
};

// MARK: Templated iterators

template <typename T>
struct TemplatedIterator {
  T value;

  using iterator_category = std::input_iterator_tag;
  using value_type = T;
  using pointer = T *;
  using reference = const T &;
  using difference_type = int;

  TemplatedIterator(int value) : value(value) {}
  TemplatedIterator(const TemplatedIterator &other) = default;

  const int &operator*() const { return value; }

  TemplatedIterator &operator++() {
    value++;
    return *this;
  }
  TemplatedIterator operator++(int) {
    auto tmp = TemplatedIterator(value);
    value++;
    return tmp;
  }
  bool operator==(const TemplatedIterator &other) const {
    return value == other.value;
  }
};

using TemplatedIteratorInt = TemplatedIterator<int>;

template <typename T>
struct TemplatedIteratorOutOfLineEq {
  T value;

  using iterator_category = std::input_iterator_tag;
  using value_type = T;
  using pointer = T *;
  using reference = const T &;
  using difference_type = int;

  TemplatedIteratorOutOfLineEq(int value) : value(value) {}
  TemplatedIteratorOutOfLineEq(const TemplatedIteratorOutOfLineEq &other) =
      default;

  const int &operator*() const { return value; }

  TemplatedIteratorOutOfLineEq &operator++() {
    value++;
    return *this;
  }
  TemplatedIteratorOutOfLineEq operator++(int) {
    auto tmp = TemplatedIteratorOutOfLineEq(value);
    value++;
    return tmp;
  }
};

template <typename T>
bool operator==(const TemplatedIteratorOutOfLineEq<T> &lhs,
                const TemplatedIteratorOutOfLineEq<T> &rhs) {
  return lhs.value == rhs.value;
}

using TemplatedIteratorOutOfLineEqInt = TemplatedIteratorOutOfLineEq<int>;

template <typename T>
struct TemplatedRACIteratorOutOfLineEq {
  T value;
  
  using iterator_category = std::random_access_iterator_tag;
  using value_type = T;
  using pointer = T *;
  using reference = const T &;
  using difference_type = int;

  TemplatedRACIteratorOutOfLineEq(int value) : value(value) {}
  TemplatedRACIteratorOutOfLineEq(const TemplatedRACIteratorOutOfLineEq &other) =
      default;

  const int &operator*() const { return value; }

  TemplatedRACIteratorOutOfLineEq &operator++() {
    value++;
    return *this;
  }
  TemplatedRACIteratorOutOfLineEq &operator--() {
    value--;
    return *this;
  }
  TemplatedRACIteratorOutOfLineEq operator++(int) {
    auto tmp = TemplatedRACIteratorOutOfLineEq(value);
    value++;
    return tmp;
  }

  TemplatedRACIteratorOutOfLineEq &operator+=(difference_type v) {
    value += v;
    return *this;
  }
  TemplatedRACIteratorOutOfLineEq &operator-=(difference_type v) {
    value -= v;
    return *this;
  }

  TemplatedRACIteratorOutOfLineEq operator+(difference_type v) const {
    return TemplatedRACIteratorOutOfLineEq(value + v);
  }
  TemplatedRACIteratorOutOfLineEq operator-(difference_type v) const {
    return TemplatedRACIteratorOutOfLineEq(value - v);
  }
};

template <typename T>
bool operator==(const TemplatedRACIteratorOutOfLineEq<T> &lhs,
                const TemplatedRACIteratorOutOfLineEq<T> &rhs) {
  return lhs.value == rhs.value;
}

template <typename T>
typename TemplatedRACIteratorOutOfLineEq<T>::difference_type
operator-(const TemplatedRACIteratorOutOfLineEq<T> &lhs,
          const TemplatedRACIteratorOutOfLineEq<T> &rhs) {
  return lhs.value - rhs.value;
}

using TemplatedRACIteratorOutOfLineEqInt = TemplatedRACIteratorOutOfLineEq<int>;

// MARK: Iterator types that use inheritance

struct BaseIntIterator {
  using value_type = int;
  using reference = const int &;

  int value;

  BaseIntIterator(int value) : value(value) {}

  reference operator*() const { return value; }

  bool operator==(const BaseIntIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const BaseIntIterator &other) const {
    return value != other.value;
  }
};

struct InheritedConstIterator : BaseIntIterator {
  using iterator_category = std::input_iterator_tag;
  using pointer = int *;
  using difference_type = int;

  InheritedConstIterator(int value) : BaseIntIterator(value) {}
  InheritedConstIterator(const InheritedConstIterator &other) = default;

  InheritedConstIterator &operator++() {
    value++;
    return *this;
  }
  InheritedConstIterator operator++(int) {
    auto tmp = InheritedConstIterator(value);
    value++;
    return tmp;
  }
};

// MARK: Templated iterator types that use inheritance

template <typename T>
struct BaseTemplatedIterator {
  using value_type = T;
  using reference = const T &;

  T value;

  BaseTemplatedIterator(T value) : value(value) {}

  reference operator*() const { return value; }

  bool operator==(const BaseTemplatedIterator<T> &other) const {
    return value == other.value;
  }
  bool operator!=(const BaseTemplatedIterator<T> &other) const {
    return value != other.value;
  }
};

template <typename T>
struct InheritedTemplatedConstIterator : BaseTemplatedIterator<T> {
  using iterator_category = std::input_iterator_tag;
  using pointer = int *;
  using difference_type = int;

  InheritedTemplatedConstIterator(T value) : BaseTemplatedIterator<T>(value) {}
  InheritedTemplatedConstIterator(const InheritedTemplatedConstIterator<T> &other) = default;

  InheritedTemplatedConstIterator<T> &operator++() {
    BaseTemplatedIterator<T>::value++;
    return *this;
  }
  InheritedTemplatedConstIterator<T> operator++(int) {
    auto tmp = InheritedTemplatedConstIterator<T>(BaseTemplatedIterator<T>::value);
    BaseTemplatedIterator<T>::value++;
    return tmp;
  }
};

typedef InheritedTemplatedConstIterator<int> InheritedTemplatedConstIteratorInt;

template <typename T>
struct BaseTemplatedRACIterator {
  using value_type = T;
  using reference = const T &;
  using difference_type = int;

  T value;

  BaseTemplatedRACIterator(T value) : value(value) {}

  reference operator*() const { return value; }

  bool operator==(const BaseTemplatedRACIterator<T> &other) const {
    return value == other.value;
  }
  bool operator!=(const BaseTemplatedRACIterator<T> &other) const {
    return value != other.value;
  }
};

template <typename T>
struct InheritedTemplatedConstRACIterator : BaseTemplatedRACIterator<T> {
  using _super = BaseTemplatedRACIterator<T>;
  using iterator_category = std::random_access_iterator_tag;
  using pointer = int *;

  InheritedTemplatedConstRACIterator(T value)
      : BaseTemplatedRACIterator<T>(value) {}
  InheritedTemplatedConstRACIterator(
      const InheritedTemplatedConstRACIterator<T> &other) = default;

  InheritedTemplatedConstRACIterator<T> &operator++() {
    _super::value++;
    return *this;
  }
  InheritedTemplatedConstRACIterator<T> operator++(int) {
    auto tmp = InheritedTemplatedConstRACIterator<T>(_super::value);
    _super::value++;
    return tmp;
  }

  InheritedTemplatedConstRACIterator<T>
  operator+(typename _super::difference_type v) const {
    return {_super::value + v};
  }
  InheritedTemplatedConstRACIterator<T>
  operator-(typename _super::difference_type v) const {
    return {_super::value - v};
  }
  friend InheritedTemplatedConstRACIterator<T>
  operator+(typename _super::difference_type v,
            const InheritedTemplatedConstRACIterator<T> &it) {
    return it + v;
  }
  int operator-(const InheritedTemplatedConstRACIterator<T> &other) const {
    return _super::value - other.value;
  }

  void operator+=(typename _super::difference_type v) { _super::value += v; }
  void operator-=(typename _super::difference_type v) { _super::value -= v; }

  bool operator<(const InheritedTemplatedConstRACIterator<T> &other) const {
    return _super::value < other.value;
  }
};

typedef InheritedTemplatedConstRACIterator<int> InheritedTemplatedConstRACIteratorInt;

template <typename T>
struct BaseTemplatedRACIteratorOutOfLineOps {
  using value_type = T;
  using reference = const T &;
  using difference_type = int;
  
  T value;

  BaseTemplatedRACIteratorOutOfLineOps(T value) : value(value) {}

  reference operator*() const { return value; }
};

template <typename T>
bool operator==(const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
                const BaseTemplatedRACIteratorOutOfLineOps<T> &rhs) {
  return lhs.value == rhs.value;
}

template <typename T>
bool operator!=(const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
                const BaseTemplatedRACIteratorOutOfLineOps<T> &rhs) {
  return lhs.value != rhs.value;
}

template <typename T>
bool operator<(const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
               const BaseTemplatedRACIteratorOutOfLineOps<T> &rhs) {
  return lhs.value < rhs.value;
}

template <typename T>
typename BaseTemplatedRACIteratorOutOfLineOps<T>::difference_type
operator-(const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
          const BaseTemplatedRACIteratorOutOfLineOps<T> &rhs) {
  return lhs.value - rhs.value;
}

template <typename T>
BaseTemplatedRACIteratorOutOfLineOps<T> operator+(
    const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
    typename BaseTemplatedRACIteratorOutOfLineOps<T>::difference_type rhs) {
  return {lhs.value + rhs};
}

template <typename T>
BaseTemplatedRACIteratorOutOfLineOps<T> operator-(
    const BaseTemplatedRACIteratorOutOfLineOps<T> &lhs,
    typename BaseTemplatedRACIteratorOutOfLineOps<T>::difference_type rhs) {
  return {lhs.value - rhs};
}

template <typename T>
BaseTemplatedRACIteratorOutOfLineOps<T>
operator+(typename BaseTemplatedRACIteratorOutOfLineOps<T>::difference_type lhs,
          const BaseTemplatedRACIteratorOutOfLineOps<T> &rhs) {
  return {rhs.value + lhs};
}

template <typename T>
struct InheritedTemplatedConstRACIteratorOutOfLineOps
    : BaseTemplatedRACIteratorOutOfLineOps<T> {
  using _super = BaseTemplatedRACIteratorOutOfLineOps<T>;
  using _self = InheritedTemplatedConstRACIteratorOutOfLineOps<T>;
  using iterator_category = std::random_access_iterator_tag;
  using pointer = int *;

  InheritedTemplatedConstRACIteratorOutOfLineOps(T value) : _super(value) {}
  InheritedTemplatedConstRACIteratorOutOfLineOps(const _self &other) = default;

  _self &operator++() {
    _super::value++;
    return *this;
  }
  _self operator++(int) {
    auto tmp = _self(_super::value);
    _super::value++;
    return tmp;
  }

  void operator+=(typename _super::difference_type v) { _super::value += v; }
  void operator-=(typename _super::difference_type v) { _super::value -= v; }
};

typedef InheritedTemplatedConstRACIteratorOutOfLineOps<int>
    InheritedTemplatedConstRACIteratorOutOfLineOpsInt;

struct InputOutputIterator {
private:
  int value;

public:
  struct iterator_category : std::input_iterator_tag,
                             std::output_iterator_tag {};
  using value_type = int;
  using pointer = int *;
  using reference = int &;
  using const_reference = const int &;
  using difference_type = int;

  InputOutputIterator(int value) : value(value) {}
  InputOutputIterator(const InputOutputIterator &other) = default;

  const_reference operator*() const { return value; }
  reference operator*() { return value; }

  InputOutputIterator &operator++() {
    value++;
    return *this;
  }
  InputOutputIterator operator++(int) {
    auto tmp = InputOutputIterator(value);
    value++;
    return tmp;
  }

  bool operator==(const InputOutputIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const InputOutputIterator &other) const {
    return value != other.value;
  }
};

struct InputOutputConstIterator {
private:
  int *value;

public:
  struct iterator_category : std::input_iterator_tag,
                             std::output_iterator_tag {};
  using value_type = int;
  using pointer = int *;
  using reference = int &;
  using difference_type = int;

  InputOutputConstIterator(int *value) : value(value) {}
  InputOutputConstIterator(const InputOutputConstIterator &other) = default;

  reference operator*() const { return *value; }

  InputOutputConstIterator &operator++() {
    value++;
    return *this;
  }
  InputOutputConstIterator operator++(int) {
    auto tmp = InputOutputConstIterator(value);
    value++;
    return tmp;
  }

  bool operator==(const InputOutputConstIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const InputOutputConstIterator &other) const {
    return value != other.value;
  }
};

struct MutableRACIterator {
private:
  int value;

public:
  struct iterator_category : std::random_access_iterator_tag,
                             std::output_iterator_tag {};
  using value_type = int;
  using pointer = int *;
  using reference = const int &;
  using difference_type = int;

  MutableRACIterator(int value) : value(value) {}
  MutableRACIterator(const MutableRACIterator &other) = default;

  const int &operator*() const { return value; }
  int &operator*() { return value; }

  MutableRACIterator &operator++() {
    value++;
    return *this;
  }
  MutableRACIterator operator++(int) {
    auto tmp = MutableRACIterator(value);
    value++;
    return tmp;
  }

  void operator+=(difference_type v) { value += v; }
  void operator-=(difference_type v) { value -= v; }
  MutableRACIterator operator+(difference_type v) const {
    return MutableRACIterator(value + v);
  }
  MutableRACIterator operator-(difference_type v) const {
    return MutableRACIterator(value - v);
  }
  friend MutableRACIterator operator+(difference_type v,
                                      const MutableRACIterator &it) {
    return it + v;
  }
  int operator-(const MutableRACIterator &other) const {
    return value - other.value;
  }

  bool operator<(const MutableRACIterator &other) const {
    return value < other.value;
  }

  bool operator==(const MutableRACIterator &other) const {
    return value == other.value;
  }
  bool operator!=(const MutableRACIterator &other) const {
    return value != other.value;
  }
};

/// clang::StmtIteratorBase
class ProtectedIteratorBase {
protected:
  int value;
  ProtectedIteratorBase() : value(0) {}
};

/// clang::StmtIteratorImpl
template <typename DERIVED>
class ProtectedIteratorImpl : public ProtectedIteratorBase {
protected:
  ProtectedIteratorImpl(const ProtectedIteratorBase& RHS) : ProtectedIteratorBase(RHS) {}

public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = int;
  using difference_type = std::ptrdiff_t;
  using pointer = int *;
  using reference = int &;

  ProtectedIteratorImpl() = default;

  DERIVED& operator++() {
    value++;
    return static_cast<DERIVED&>(*this);
  }

  DERIVED operator++(int) {
    DERIVED tmp = static_cast<DERIVED&>(*this);
    operator++();
    return tmp;
  }

  friend bool operator==(const DERIVED &LHS, const DERIVED &RHS) {
    return LHS.value == RHS.value;
  }

  reference operator*() const {
    return value;
  }
};

/// StmtIterator
struct HasInheritedProtectedCopyConstructor : public ProtectedIteratorImpl<HasInheritedProtectedCopyConstructor> {
  HasInheritedProtectedCopyConstructor() = default;

private:
  HasInheritedProtectedCopyConstructor(const ProtectedIteratorBase &other)
      : ProtectedIteratorImpl<HasInheritedProtectedCopyConstructor>(other) {}
};

#endif // TEST_INTEROP_CXX_STDLIB_INPUTS_CUSTOM_ITERATOR_H
