//===--- OptionSet.h - Sets of boolean options ------------------*- C++ -*-===//
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
//  This file defines the OptionSet class template.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_OPTIONSET_H
#define SWIFT_BASIC_OPTIONSET_H

#include "llvm/ADT/None.h"

#include <type_traits>
#include <cstdint>
#include <initializer_list>

namespace swift {

/// The class template \c OptionSet captures a set of options stored as the
/// bits in an unsigned integral value.
///
/// Each option corresponds to a particular flag value in the provided
/// enumeration type (\c Flags). The option set provides ways to add options,
/// remove options, intersect sets, etc., providing a thin type-safe layer
/// over the underlying unsigned value.
///
/// \tparam Flags An enumeration type that provides the individual flags
/// for options. Each enumerator should have a power-of-two value, indicating
/// which bit it is associated with.
///
/// \tparam StorageType The unsigned integral type to use to store the flags
/// enabled within this option set. This defaults to the unsigned form of the
/// underlying type of the enumeration.
template<typename Flags,
         typename StorageType = typename std::make_unsigned<
                                  typename std::underlying_type<Flags>::type
                                 >::type>
class OptionSet {
  StorageType Storage;

public:
  /// Create an empty option set.
  constexpr OptionSet() : Storage() {}

  /// Create an empty option set.
  constexpr OptionSet(llvm::NoneType) : Storage() {}

  /// Create an option set with only the given option set.
  constexpr OptionSet(Flags flag) : Storage(static_cast<StorageType>(flag)) {}

  /// Create an option set containing the given options.
  constexpr OptionSet(std::initializer_list<Flags> flags)
      : Storage(combineFlags(flags)) {}

  /// Create an option set from raw storage.
  explicit constexpr OptionSet(StorageType storage) : Storage(storage) {}

  /// Check whether an option set is non-empty.
  explicit constexpr operator bool() const { return Storage != 0; }

  /// Explicitly convert an option set to its underlying storage.
  explicit constexpr operator StorageType() const { return Storage; }

  /// Explicitly convert an option set to intptr_t, for use in
  /// llvm::PointerIntPair.
  ///
  /// This member is not present if the underlying type is bigger than
  /// a pointer.
  template <typename T = std::intptr_t>
  explicit constexpr
  operator typename std::enable_if<sizeof(StorageType) <= sizeof(T),
                                   std::intptr_t>::type() const {
    return static_cast<intptr_t>(Storage);
  }

  /// Retrieve the "raw" representation of this option set.
  StorageType toRaw() const { return Storage; }

  /// Determine whether this option set contains all of the options in the
  /// given set.
  constexpr bool contains(OptionSet set) const {
    return !static_cast<bool>(set - *this);
  }

  /// Check if this option set contains the exact same options as the given set.
  constexpr bool containsOnly(OptionSet set) const {
    return Storage == set.Storage;
  }

  // '==' and '!=' are deliberately not defined because they provide a pitfall
  // where someone might use '==' but really want 'contains'. If you actually
  // want '==' behavior, use 'containsOnly'.

  /// Produce the union of two option sets.
  friend constexpr OptionSet operator|(OptionSet lhs, OptionSet rhs) {
    return OptionSet(lhs.Storage | rhs.Storage);
  }

  /// Produce the union of two option sets.
  friend constexpr OptionSet &operator|=(OptionSet &lhs, OptionSet rhs) {
    lhs.Storage |= rhs.Storage;
    return lhs;
  }

  /// Produce the intersection of two option sets.
  friend constexpr OptionSet operator&(OptionSet lhs, OptionSet rhs) {
    return OptionSet(lhs.Storage & rhs.Storage);
  }

  /// Produce the intersection of two option sets.
  friend constexpr OptionSet &operator&=(OptionSet &lhs, OptionSet rhs) {
    lhs.Storage &= rhs.Storage;
    return lhs;
  }

  /// Produce the difference of two option sets.
  friend constexpr OptionSet operator-(OptionSet lhs, OptionSet rhs) {
    return OptionSet(lhs.Storage & ~rhs.Storage);
  }

  /// Produce the difference of two option sets.
  friend constexpr OptionSet &operator-=(OptionSet &lhs, OptionSet rhs) {
    lhs.Storage &= ~rhs.Storage;
    return lhs;
  }

private:
  template <typename T>
  static auto _checkResultTypeOperatorOr(T t) -> decltype(t | t) { return T(); }

  static void _checkResultTypeOperatorOr(...) {}

  static constexpr StorageType
  combineFlags(const std::initializer_list<Flags> &flags) {
    OptionSet result;
    for (Flags flag : flags)
      result |= flag;
    return result.Storage;
  }

  static_assert(!std::is_same<decltype(_checkResultTypeOperatorOr(Flags())),
                              Flags>::value,
                "operator| should produce an OptionSet");
};

} // end namespace swift

#endif // SWIFT_BASIC_OPTIONSET_H
