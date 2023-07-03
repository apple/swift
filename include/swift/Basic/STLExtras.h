//===--- STLExtras.h - additions to the STL ---------------------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
/// \file Provides STL-style algorithms for convenience.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_STLEXTRAS_H
#define SWIFT_BASIC_STLEXTRAS_H

#include "llvm/ADT/None.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Casting.h"
#include <algorithm>
#include <cassert>
#include <functional>
#include <iterator>
#include <numeric>
#include <type_traits>
#include <unordered_set>

namespace swift {

//===----------------------------------------------------------------------===//
//                              Function Traits
//===----------------------------------------------------------------------===//

template <class T>
struct function_traits : function_traits<decltype(&T::operator())> {};

// function
template <class R, class... Args> struct function_traits<R(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
};

// function pointer
template <class R, class... Args> struct function_traits<R (*)(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
};

// std::function
template <class R, class... Args>
struct function_traits<std::function<R(Args...)>> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
};

// pointer-to-member-function (i.e., operator()'s)
template <class T, class R, class... Args>
struct function_traits<R (T::*)(Args...)> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
};

template <class T, class R, class... Args>
struct function_traits<R (T::*)(Args...) const> {
  using result_type = R;
  using argument_types = std::tuple<Args...>;
};

/// @{

/// The equivalent of std::for_each, but for two lists at once.
template <typename InputIt1, typename InputIt2, typename BinaryFunction>
inline void for_each(InputIt1 I1, InputIt1 E1, InputIt2 I2, BinaryFunction f) {
  while (I1 != E1) {
    f(*I1, *I2);
    ++I1; ++I2;
  }
}

template <typename Container1, typename Container2, typename BinaryFunction>
inline void for_each(const Container1 &c1, const Container2 &c2,
                     BinaryFunction f) {
  assert(c1.size() == c2.size());
  for_each(c1.begin(), c1.end(), c2.begin(), f);
}

/// The equivalent of std::for_each, but for three lists at once.
template <typename InputIt1, typename InputIt2, typename InputIt3,
          typename TernaryFunction>
inline void for_each3(InputIt1 I1, InputIt1 E1, InputIt2 I2, InputIt3 I3,
                      TernaryFunction f) {
  while (I1 != E1) {
    f(*I1, *I2, *I3);
    ++I1; ++I2; ++I3;
  }
}

template <typename Container1, typename Container2, typename Container3,
          typename TernaryFunction>
inline void for_each3(const Container1 &c1, const Container2 &c2,
                      const Container3 &c3,
                     TernaryFunction f) {
  assert(c1.size() == c2.size());
  assert(c2.size() == c3.size());
  for_each3(c1.begin(), c1.end(), c2.begin(), c3.begin(), f);
}

/// The equivalent of std::for_each, but visits the set union of two sorted
/// lists without allocating additional memory.
///
/// This has the following requirements:
///
/// 1. The ranges must be sorted.
/// 2. The elements must have the same type.
/// 3. There are no duplicate elements.
/// 4. All elements must be comparable with std::less.
template <typename InputIt1, typename InputIt2, typename BinaryFunction>
inline void set_union_for_each(InputIt1 I1, InputIt1 E1, InputIt2 I2,
                               InputIt2 E2, BinaryFunction f) {
  static_assert(
      std::is_same<
        typename std::iterator_traits<InputIt1>::value_type,
        typename std::iterator_traits<InputIt2>::value_type
      >::value,
      "Expected both iterator types to have the same underlying value type");

  using RefTy = typename std::iterator_traits<InputIt1>::reference;

  while (true) {
    // If we have reached the end of either list, visit the rest of the other
    // list, We do not need to worry about duplicates since each array we know
    // is unique.
    if (I1 == E1) {
      std::for_each(I2, E2, f);
      return;
    }

    if (I2 == E2) {
      std::for_each(I1, E1, f);
      return;
    }

    // If I1 < I2, then visit I1 and continue.
    if (std::less<RefTy>()(*I1, *I2)) {
      f(*I1);
      ++I1;
      continue;
    }

    // If I2 < I1, visit I2 and continue.
    if (std::less<RefTy>()(*I2, *I1)) {
      f(*I2);
      ++I2;
      continue;
    }

    // Otherwise, we know that I1 and I2 equal. We know that we can only have
    // one of each element in each list, so we can just visit I1 and continue.
    f(*I1);
    ++I1;
    ++I2;
  }
}

/// A container adapter for set_union_for_each.
///
/// To see the requirements upon the containers, please see the iterator based
/// set_union_for_each.
template <typename Container1, typename Container2, typename UnaryFunction>
inline void set_union_for_each(const Container1 &C1, const Container2 &C2,
                               UnaryFunction f) {
  // Make sure that our iterators have the same value type.
  static_assert(
      std::is_same<
        typename std::iterator_traits<
          typename Container1::iterator
        >::value_type,
        typename std::iterator_traits<
          typename Container2::iterator
        >::value_type
      >::value,
      "Expected both containers to have the same iterator value type");
  set_union_for_each(C1.begin(), C1.end(), C2.begin(), C2.end(), f);
}

/// If \p it is equal to \p end, then return \p defaultIter. Otherwise, return
/// std::next(\p it).
template <typename Iterator>
inline Iterator next_or_default(Iterator it, Iterator end,
                                Iterator defaultIter) {
  return (it == end) ? defaultIter : std::next(it);
}

/// If \p it is equal to \p begin, then return \p defaultIter. Otherwise, return
/// std::prev(\p it).
template <typename Iterator>
inline Iterator prev_or_default(Iterator it, Iterator begin,
                                Iterator defaultIter) {
  return (it == begin) ? defaultIter : std::prev(it);
}

/// Takes an iterator and an iterator pointing to the end of the iterator range.
/// If the iterator already points to the end of its range, simply return it,
/// otherwise return the next element.
template <typename Iterator>
inline Iterator next_or_end(Iterator it, Iterator end) {
  return next_or_default(it, end, end);
}

template <typename Iterator>
inline Iterator prev_or_begin(Iterator it, Iterator begin) {
  return prev_or_default(it, begin, begin);
}

/// @}

/// An iterator that walks a linked list of objects until it reaches
/// a null pointer.
template <class T, T* (&getNext)(T*)>
class LinkedListIterator {
  T *Pointer;
public:
  using iterator_category = std::forward_iterator_tag;
  using value_type = T *;
  using reference = T *;
  using pointer = void;

  /// Returns an iterator range starting from the given pointer and
  /// running until it reaches a null pointer.
  static llvm::iterator_range<LinkedListIterator> rangeBeginning(T *pointer) {
    return {pointer, nullptr};
  }

  constexpr LinkedListIterator(T *pointer) : Pointer(pointer) {}

  T *operator*() const {
    assert(Pointer && "dereferencing a null iterator");
    return Pointer;
  }

  LinkedListIterator &operator++() {
    Pointer = getNext(Pointer);
    return *this;
  }
  LinkedListIterator operator++(int) {
    auto copy = *this;
    Pointer = getNext(Pointer);
    return copy;
  }

  friend bool operator==(LinkedListIterator lhs, LinkedListIterator rhs) {
    return lhs.Pointer == rhs.Pointer;
  }
  friend bool operator!=(LinkedListIterator lhs, LinkedListIterator rhs) {
    return lhs.Pointer != rhs.Pointer;
  }
};

/// An iterator that transforms the result of an underlying bidirectional
/// iterator with a given operation.
///
/// Slightly different semantics from llvm::map_iterator, but we should
/// probably figure out how to merge them eventually.
///
/// \tparam Iterator the underlying iterator.
///
/// \tparam Operation A function object that transforms the underlying
/// sequence's values into the new sequence's values.
template<typename Iterator, typename Operation>
class TransformIterator {
  Iterator Current;

  /// FIXME: Could optimize away this storage with EBCO tricks.
  Operation Op;

  /// The underlying reference type, which will be passed to the
  /// operation.
  using OpTraits = function_traits<Operation>;

public:
  using iterator_category = std::bidirectional_iterator_tag;
  using value_type = typename OpTraits::result_type;
  using reference = value_type;
  using pointer = void; // FIXME: Should provide a pointer proxy.
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;

  /// Construct a new transforming iterator for the given iterator 
  /// and operation.
  TransformIterator(Iterator current, Operation op) 
    : Current(current), Op(op) { }

  reference operator*() const {
    return Op(*Current);
  }

  TransformIterator &operator++() {
    ++Current;
    return *this;
  }

  TransformIterator operator++(int) {
    TransformIterator old = *this;
    ++*this;
    return old;
  }

  TransformIterator &operator--() {
    --Current;
    return *this;
  }

  TransformIterator operator--(int) {
    TransformIterator old = *this;
    --*this;
    return old;
  }

  friend bool operator==(TransformIterator lhs, TransformIterator rhs) {
    return lhs.Current == rhs.Current;
  }
  friend bool operator!=(TransformIterator lhs, TransformIterator rhs) {
    return !(lhs == rhs);
  }
};

/// Create a new transform iterator.
template<typename Iterator, typename Operation>
inline TransformIterator<Iterator, Operation> 
makeTransformIterator(Iterator current, Operation op) {
  return TransformIterator<Iterator, Operation>(current, op);
}

/// A range transformed by a specific predicate.
template<typename Range, typename Operation>
class TransformRange {
  Range Rng;
  Operation Op;

public:
  using iterator = TransformIterator<decltype(Rng.begin()), Operation>;

  TransformRange(Range range, Operation op)
    : Rng(range), Op(op) { }

  iterator begin() const { return iterator(Rng.begin(), Op); }
  iterator end() const { return iterator(Rng.end(), Op); }
  bool empty() const { return begin() == end(); }

  // The dummy template parameter keeps 'size()' from being eagerly
  // instantiated.
  template <typename Dummy = Range>
  typename function_traits<decltype(&Dummy::size)>::result_type
  size() const {
    return Rng.size();
  }

  template <typename Index>
  typename function_traits<Operation>::result_type
  operator[](Index index) const {
    return Op(Rng[index]);
  }

  typename std::iterator_traits<iterator>::value_type front() const { 
    assert(!empty() && "Front of empty range");
    return *begin(); 
  }
};

/// Create a new transform range.
template<typename Range, typename Operation>
inline TransformRange<Range, Operation> 
makeTransformRange(Range range, Operation op) {
  return TransformRange<Range, Operation>(range, op);
}

/// An iterator that filters and transforms the results of an
/// underlying forward iterator based on a transformation from the underlying
/// value type to an optional result type.
///
/// \tparam Iterator the underlying iterator.
///
/// \tparam OptionalTransform A function object that maps a value of
/// the underlying iterator type to an optional containing a value of
/// the resulting sequence, or an empty optional if this item should
/// be skipped.
template<typename Iterator, typename OptionalTransform>
class OptionalTransformIterator {
  Iterator Current, End;

  /// FIXME: Could optimize away this storage with EBCO tricks.
  OptionalTransform Op;

  /// Skip any non-matching elements.
  void skipNonMatching() {
    while (Current != End && !Op(*Current))
      ++Current;
  }

  using UnderlyingReference =
      typename std::iterator_traits<Iterator>::reference;

  using ResultReference =
      typename std::invoke_result<OptionalTransform, UnderlyingReference>::type;

public:
  /// Used to indicate when the current iterator has already been
  /// "primed", meaning that it's at the end or points to a value that
  /// satisfies the transform.
  enum PrimedT { Primed };

  using iterator_category = std::forward_iterator_tag;
  using reference = typename ResultReference::value_type;
  using value_type = typename ResultReference::value_type;
  using pointer = void; // FIXME: should add a proxy here.
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;

  /// Construct a new optional transform iterator for the given
  /// iterator range and operation.
  OptionalTransformIterator(Iterator current, Iterator end, 
                            OptionalTransform op)
    : Current(current), End(end), Op(op)
  {
    // Prime the iterator.
    skipNonMatching();
  }

  /// Construct a new optional transform iterator for the given iterator range
  /// and operation, where the iterator range has already been
  /// "primed" by ensuring that it is empty or the current iterator
  /// points to something that matches the operation.
  OptionalTransformIterator(Iterator current, Iterator end, 
                            OptionalTransform op, PrimedT)
    : Current(current), End(end), Op(op) 
  { 
    // Assert that the iterators have already been primed.
    assert((Current == End || Op(*Current)) && "Not primed!");
  }

  reference operator*() const {
    return *Op(*Current);
  }

  reference operator*() { return *Op(*Current); }

  OptionalTransformIterator &operator++() {
    ++Current;
    skipNonMatching();
    return *this;
  }

  OptionalTransformIterator operator++(int) {
    OptionalTransformIterator old = *this;
    ++*this;
    return old;
  }

  friend bool operator==(OptionalTransformIterator lhs, 
                         OptionalTransformIterator rhs) {
    return lhs.Current == rhs.Current;
  }
  friend bool operator!=(OptionalTransformIterator lhs,
                         OptionalTransformIterator rhs) {
    return !(lhs == rhs);
  }
};

/// Create a new filter iterator.
template<typename Iterator, typename OptionalTransform>
inline OptionalTransformIterator<Iterator, OptionalTransform> 
makeOptionalTransformIterator(Iterator current, Iterator end, 
                              OptionalTransform op) {
  return OptionalTransformIterator<Iterator, OptionalTransform>(current, end,
                                                                op);
}

/// A range filtered and transformed by the optional transform.
template <typename Range, typename OptionalTransform,
          typename Iterator = decltype(std::declval<Range>().begin())>
class OptionalTransformRange {

  Iterator First, Last;
  OptionalTransform Op;

public:
  using iterator = OptionalTransformIterator<Iterator, OptionalTransform>;

  OptionalTransformRange(Range range, OptionalTransform op)
    : First(range.begin()), Last(range.end()), Op(op) 
  { 
    // Prime the sequence.
    while (First != Last && !Op(*First))
      ++First;
  }

  iterator begin() const { 
    return iterator(First, Last, Op, iterator::Primed); 
  }

  iterator end() const { 
    return iterator(Last, Last, Op, iterator::Primed); 
  }

  bool empty() const { return First == Last; }

  typename std::iterator_traits<iterator>::value_type front() const { 
    assert(!empty() && "Front of empty range");
    return *begin(); 
  }
};

/// Create a new filter range.
template<typename Range, typename OptionalTransform>
inline OptionalTransformRange<Range, OptionalTransform> 
makeOptionalTransformRange(Range range, OptionalTransform op) {
  return OptionalTransformRange<Range, OptionalTransform>(range, op);
}

/// Function object that attempts a downcast to a subclass, wrapping
/// the result in an optional to indicate success or failure.
template<typename Subclass>
struct DowncastAsOptional {
  template <typename Superclass>
  auto operator()(Superclass &value) const
      -> llvm::Optional<decltype(llvm::cast<Subclass>(value))> {
    if (auto result = llvm::dyn_cast<Subclass>(value))
      return result;

    return llvm::None;
  }

  template <typename Superclass>
  auto operator()(const Superclass &value) const
      -> llvm::Optional<decltype(llvm::cast<Subclass>(value))> {
    if (auto result = llvm::dyn_cast<Subclass>(value))
      return result;

    return llvm::None;
  }
};

template<typename Subclass, typename Iterator>
using DowncastFilterIterator
        = OptionalTransformIterator<Iterator, DowncastAsOptional<Subclass>>;

template<typename Subclass, typename Iterator>
inline DowncastFilterIterator<Subclass, Iterator>
makeDowncastFilterIterator(Iterator current, Iterator end) {
  DowncastAsOptional<Subclass> op;
  return DowncastFilterIterator<Subclass, Iterator>(current, end, op);
}

template<typename Subclass, typename Range>
class DowncastFilterRange 
  : public OptionalTransformRange<Range, DowncastAsOptional<Subclass>> {

  using Inherited = OptionalTransformRange<Range, DowncastAsOptional<Subclass>>;

public:
  DowncastFilterRange(Range range) 
    : Inherited(range, DowncastAsOptional<Subclass>()) { }
};
              
template<typename Subclass, typename Range>
DowncastFilterRange<Subclass, Range>
makeDowncastFilterRange(Range range) {
  return DowncastFilterRange<Subclass, Range>(range);
}

/// Sorts and then uniques a container with random access iterators and an erase
/// method that removes a range specified by random access iterators.
template <typename Container>
void sortUnique(
    Container &C,
    typename std::enable_if<
        std::is_same<typename std::iterator_traits<
                         typename Container::iterator>::iterator_category,
                     std::random_access_iterator_tag>::value,
        void>::type * = nullptr) {
  std::sort(C.begin(), C.end());
  C.erase(std::unique(C.begin(), C.end()), C.end());
}

/// Sorts and then uniques a container with random access iterators and an erase
/// method that removes a range specified by random access iterators.
template <typename Container, typename Comparator>
void sortUnique(
    Container &C,
    Comparator Cmp,
    typename std::enable_if<
        std::is_same<typename std::iterator_traits<
                         typename Container::iterator>::iterator_category,
                     std::random_access_iterator_tag>::value,
        void>::type * = nullptr) {
  std::sort(C.begin(), C.end(), Cmp);
  C.erase(std::unique(C.begin(), C.end()), C.end());
}

/// Returns true if [II, IE) is a sorted and uniqued array. Returns false
/// otherwise.
template <typename IterTy>
inline bool is_sorted_and_uniqued(IterTy II, IterTy IE) {
  using RefTy = typename std::iterator_traits<IterTy>::reference;

  // The empty list is always sorted and uniqued.
  if (II == IE)
    return true;

  // The list of one element is always sorted and uniqued.
  auto LastI = II;
  ++II;
  if (II == IE)
    return true;

  // Otherwise, until we reach the end of the list...
  while (II != IE) {
    // If LastI is greater than II then we know that our array is not sorted. If
    // LastI equals II, then we know that our array is not unique. If both of
    // those are conditions are false, then visit the next iterator element.
    if (std::greater_equal<RefTy>()(*LastI, *II)) {
      // Return false otherwise.
      return false;
    }

    LastI = II;
    ++II;
  }

  // Success!
  return true;
}

template <typename Container>
inline bool is_sorted_and_uniqued(const Container &C) {
  return is_sorted_and_uniqued(C.begin(), C.end());
}

template <typename Container, typename T, typename BinaryOperation>
inline T accumulate(const Container &C, T init, BinaryOperation op) {
  return std::accumulate(C.begin(), C.end(), init, op);
}

template <typename Container, typename T>
inline bool binary_search(const Container &C, const T &value) {
  return std::binary_search(C.begin(), C.end(), value);
}

template <typename Container, typename T, typename BinaryOperation>
inline bool binary_search(const Container &C, const T &value, BinaryOperation op) {
  return std::binary_search(C.begin(), C.end(), value, op);
}

/// Returns true if the range defined by \p mainBegin ..< \p mainEnd starts with
/// the same elements as the range defined by \p prefixBegin ..< \p prefixEnd.
///
/// This includes cases where the prefix range is empty, as well as when the two
/// ranges are the same length and contain the same elements.
template <typename MainInputIterator, typename PrefixInputIterator>
inline bool hasPrefix(MainInputIterator mainBegin,
                      const MainInputIterator mainEnd,
                      PrefixInputIterator prefixBegin,
                      const PrefixInputIterator prefixEnd) {
  while (prefixBegin != prefixEnd) {
    // If "main" is shorter than "prefix", it does not start with "prefix".
    if (mainBegin == mainEnd)
      return false;
    // If there's a mismatch, "main" does not start with "prefix".
    if (*mainBegin != *prefixBegin)
      return false;
    ++prefixBegin;
    ++mainBegin;
  }
  // If we checked every element of "prefix", "main" does start with "prefix".
  return true;
}

/// Provides default implementations of !=, <=, >, and >= based on == and <.
template <typename T>
class RelationalOperationsBase {
public:
  friend bool operator>(const T &left, const T &right) {
    return right < left;
  }
  friend bool operator>=(const T &left, const T &right) {
    return !(left < right);
  }
  friend bool operator<=(const T &left, const T &right) {
    return !(right < left);
  }
  friend bool operator!=(const T &left, const T &right) {
    return !(left == right);
  }
};
  
/// Cast a pointer to \c U  to a pointer to a supertype \c T.
/// Example:  Wobulator *w = up_cast<Wobulator>(coloredWobulator)
/// Useful with ?: where each arm is a different subtype.
/// If \c U is not a subtype of \c T, the compiler will complain.
template <typename T, typename U>
T *up_cast(U *ptr) { return ptr; }

/// Removes all runs of values that match \p pred from the range of \p begin
/// to \p end.
///
/// This is similar to std::unique, but std::unique leaves the first value in
/// place in a run of matching values, whereas this code removes all of them.
///
/// \returns The new end iterator for the container. You should erase elements
/// between this value and the existing end of the container.
template <typename Iterator, typename BinaryPredicate>
Iterator removeAdjacentIf(const Iterator first, const Iterator last,
                          BinaryPredicate pred) {
  using element_reference_t =
      typename std::iterator_traits<Iterator>::reference;

  auto nextOverlap = std::adjacent_find(first, last, pred);
  auto insertionPoint = nextOverlap;
  while (nextOverlap != last) {
    // We want to erase *all* the matching elements. There could be three or
    // more of them. Search for the end of the run.
    auto lastOverlapInRun =
        std::adjacent_find(std::next(nextOverlap), last,
                           [&pred](element_reference_t left,
                                   element_reference_t right) -> bool {
      return !pred(left, right);
    });
    // If we get the end iterator back, that means all remaining elements match.
    // If we don't, that means (lastOverlapInRun+1) is part of a different run.
    if (lastOverlapInRun != last)
      ++lastOverlapInRun;

    nextOverlap = std::adjacent_find(lastOverlapInRun, last, pred);
    insertionPoint = std::move(lastOverlapInRun, nextOverlap, insertionPoint);
  }

  return insertionPoint;
}

namespace detail {
template <bool...> struct bool_pack;
} // namespace detail

template <bool... b>
using all_true =
    std::is_same<detail::bool_pack<b..., true>, detail::bool_pack<true, b...>>;

/// traits class for checking whether Ts consists only of compound types.
template <class... Ts>
using are_all_compound = all_true<std::is_compound<Ts>::value...>;

/// Erase all elements in \p c that match the given predicate \p pred.
// FIXME: Remove this when C++20 is the new baseline.
template <class Key, class Hash, class KeyEqual, class Alloc, class Pred>
typename std::unordered_set<Key, Hash, KeyEqual, Alloc>::size_type
erase_if(std::unordered_set<Key, Hash, KeyEqual, Alloc> &c, Pred pred) {
  auto startingSize = c.size();
  for (auto i = c.begin(), last = c.end(); i != last;) {
    if (pred(*i)) {
      i = c.erase(i);
    } else {
      ++i;
    }
  }
  return startingSize - c.size();
}

/// Call \c vector.emplace_back with each of the other arguments
/// to this function, in order.  Constructing an intermediate
/// \c std::initializer_list can be inefficient; more problematically,
/// types such as \c std::vector copy out of the \c initializer_list
/// instead of move.
template <class VectorType, class ValueType, class... ValueTypes>
void emplace_back_all(VectorType &vector, ValueType &&value,
                      ValueTypes &&...values) {
  vector.emplace_back(std::forward<ValueType>(value));
  emplace_back_all(vector, std::forward<ValueTypes>(values)...);
}
template <class VectorType>
void emplace_back_all(VectorType &vector) {}

} // end namespace swift

#endif // SWIFT_BASIC_STLEXTRAS_H
