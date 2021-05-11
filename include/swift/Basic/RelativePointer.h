//===--- RelativePointer.h - Relative Pointer Support -----------*- C++ -*-===//
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
///
/// \file
///
/// Some data structures emitted by the Swift compiler use relative indirect
/// addresses in order to minimize startup cost for a process. By referring to
/// the offset of the global offset table entry for a symbol, instead of
/// directly referring to the symbol, compiler-emitted data structures avoid
/// requiring unnecessary relocation at dynamic linking time. This header
/// contains types to help dereference these relative addresses.
///
/// Theory of references to objects
/// -------------------------------
///
/// A reference can be absolute or relative:
///
///   - An absolute reference is a pointer to the object.
///
///   - A relative reference is a (signed) offset from the address of the
///     reference to the address of its direct referent.
///
/// A relative reference can be direct, indirect, or symbolic.
///
/// In a direct reference, the direct referent is simply the target object.
/// Generally, a statically-emitted relative reference can only be direct
/// if it can be resolved to a constant offset by the linker, because loaders
/// do not support forming relative references.  This means that either the
/// reference and object must lie within the same linkage unit or the
/// difference must be computed at runtime by code.
///
/// In a symbolic reference, the direct referent is a string holding the symbol
/// name of the object.  A relative reference can only be symbolic if the
/// object actually has a symbol at runtime, which may require exporting
/// many internal symbols that would otherwise be strippable.
///
/// In an indirect reference, the direct referent is a variable holding an
/// absolute reference to the object.  An indirect relative reference may
/// refer to an arbitrary symbol, be it anonymous within the linkage unit
/// or completely external to it, but it requires the introduction of an
/// intermediate absolute reference that requires load-time initialization.
/// However, this initialization can be shared among all indirect references
/// within the linkage unit, and the linker will generally place all such
/// references adjacent to one another to improve load-time locality.
///
/// A reference can be made a dynamic union of more than one of these options.
/// This allows the compiler/linker to use a direct reference when possible
/// and a less-efficient option where required.  However, it also requires
/// the cases to be dynamically distinguished.  This can be done by setting
/// a low bit of the offset, as long as the difference between the direct
/// referent's address and the reference is a multiple of 2.  This works well
/// for "indirectable" references because most objects are known to be
/// well-aligned, and the cases that aren't (chiefly functions and strings)
/// rarely need the flexibility of this kind of reference.  It does not
/// work quite as well for "possibly symbolic" references because C strings
/// are not naturally aligned, and making them aligned generally requires
/// moving them out of the linker's ordinary string section; however, it's
/// still workable.
///
/// Finally, a relative reference can be near or far.  A near reference
/// is potentially smaller, but it requires the direct referent to lie
/// within a certain distance of the reference, even if dynamically
/// initialized.
///
/// In Swift, we always prefer to use a near direct relative reference
/// when it is possible to do so: that is, when the relationship is always
/// between two global objects emitted in the same linkage unit, and there
/// is no compatibility constraint requiring the use of an absolute reference.
///
/// When more flexibility is required, there are several options:
///
///   1. Use an absolute reference.  Size penalty on 64-bit.  Requires
///      load-time work.
///
///   2. Use a far direct relative reference.  Size penalty on 64-bit.
///      Requires load-time work when object is outside linkage unit.
///      Generally not directly supported by loaders.
///
///   3. Use an always-indirect relative reference.  Size penalty of one
///      pointer (shared).  Requires load-time work even when object is
///      within linkage unit.
///
///   4. Use a near indirectable relative reference.  Size penalty of one
///      pointer (shared) when reference exceeds range.  Runtime / code-size
///      penalty on access.  Requires load-time work (shared) only when
///      object is outside linkage unit.
///
///   5. Use a far indirectable relative reference.  Size penalty on 64-bit.
///      Size penalty of one pointer (shared) when reference exceeds range
///      and is initialized statically.  Runtime / code-size penalty on access.
///      Requires load-time work (shared) only when object is outside linkage
///      unit.
///
///   6. Use a near or far symbolic relative reference.  No load-time work.
///      Severe runtime penalty on access.  Requires custom logic to statically
///      optimize.  Requires emission of symbol for target even if private
///      to linkage unit.
///
///   7. Use a near or far direct-or-symbolic relative reference.  No
///      load-time work.  Severe runtime penalty on access if object is
///      outside of linkage unit.  Requires custom logic to statically optimize.
///
/// In general, it's our preference in Swift to use option #4 when there
/// is no possibility of initializing the reference dynamically and option #5
/// when there is.  This is because it is infeasible to actually share the
/// memory for the intermediate absolute reference when it must be allocated
/// dynamically.
///
/// Symbolic references are an interesting idea that we have not yet made
/// use of.  They may be acceptable in reflective metadata cases where it
/// is desirable to heavily bias towards never using the metadata.  However,
/// they're only profitable if there wasn't any other indirect reference
/// to the target, and it is likely that their optimal use requires a more
/// intelligent toolchain from top to bottom.
///
/// Note that the cost of load-time work also includes a binary-size penalty
/// to store the loader metadata necessary to perform that work.  Therefore
/// it is better to avoid it even when there are dynamic optimizations in
/// place to skip the work itself.
///
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_RELATIVEPOINTER_H
#define SWIFT_BASIC_RELATIVEPOINTER_H

#include <cstdint>

namespace swift {

namespace detail {

/// Apply a relative offset to a base pointer. The offset is applied to the base
/// pointer using sign-extended, wrapping arithmetic.
template<typename BasePtrTy, typename Offset>
static inline uintptr_t applyRelativeOffset(BasePtrTy *basePtr, Offset offset) {
  static_assert(std::is_integral<Offset>::value &&
                std::is_signed<Offset>::value,
                "offset type should be signed integer");

#ifdef __wasm__
  // WebAssembly: hack: disable relative pointers
  return (uintptr_t)(intptr_t)offset;
#endif

  auto base = reinterpret_cast<uintptr_t>(basePtr);
  // We want to do wrapping arithmetic, but with a sign-extended
  // offset. To do this in C, we need to do signed promotion to get
  // the sign extension, but we need to perform arithmetic on unsigned values,
  // since signed overflow is undefined behavior.
  auto extendOffset = (uintptr_t)(intptr_t)offset;
  return base + extendOffset;
}

/// Measure the relative offset between two pointers. This measures
/// (referent - base) using wrapping arithmetic. The result is truncated if
/// Offset is smaller than a pointer, with an assertion that the
/// pre-truncation result is a sign extension of the truncated result.
template<typename Offset, typename A, typename B>
static inline Offset measureRelativeOffset(A *referent, B *base) {
  static_assert(std::is_integral<Offset>::value &&
                std::is_signed<Offset>::value,
                "offset type should be signed integer");
#ifdef __wasm__
  // WebAssembly: hack: disable relative pointers
  auto offset = (Offset)(uintptr_t)referent;
  assert((intptr_t)offset == (intptr_t)(uintptr_t)referent
         && "pointer too large to fit in offset type");
  return offset;
#endif

  auto distance = (uintptr_t)referent - (uintptr_t)base;
  // Truncate as unsigned, then wrap around to signed.
  auto truncatedDistance =
    (Offset)(typename std::make_unsigned<Offset>::type)distance;
  // Assert that the truncation didn't discard any non-sign-extended bits.
  assert((intptr_t)truncatedDistance == (intptr_t)distance
         && "pointers are too far apart to fit in offset type");
  return truncatedDistance;
}

} // namespace detail

/// A relative reference to an object stored in memory. The reference may be
/// direct or indirect, and uses the low bit of the (assumed at least
/// 2-byte-aligned) pointer to differentiate.
template<typename ValueTy, bool Nullable = false, typename Offset = int32_t>
class RelativeIndirectPointer {
private:
  static_assert(std::is_integral<Offset>::value &&
                std::is_signed<Offset>::value,
                "offset type should be signed integer");

  /// The relative offset of the pointer's memory from the `this` pointer.
  /// This is an indirect reference.
  Offset RelativeOffset;

  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeIndirectPointer() = delete;
  RelativeIndirectPointer(RelativeIndirectPointer &&) = delete;
  RelativeIndirectPointer(const RelativeIndirectPointer &) = delete;
  RelativeIndirectPointer &operator=(RelativeIndirectPointer &&)
    = delete;
  RelativeIndirectPointer &operator=(const RelativeIndirectPointer &)
    = delete;

public:
  const ValueTy *get() const & {
    // Check for null.
    if (Nullable && RelativeOffset == 0)
      return nullptr;

    uintptr_t address = detail::applyRelativeOffset(this, RelativeOffset);
    return *reinterpret_cast<const ValueTy * const *>(address);
  }

  /// A zero relative offset encodes a null reference.
  bool isNull() const & {
    return RelativeOffset == 0;
  }

  operator const ValueTy* () const & {
    return get();
  }

  const ValueTy *operator->() const & {
    return get();
  }
};

/// A relative reference to an object stored in memory. The reference may be
/// direct or indirect, and uses the low bit of the (assumed at least
/// 2-byte-aligned) pointer to differentiate.
template<typename ValueTy, bool Nullable = false, typename Offset = int32_t, typename IndirectType = const ValueTy *>
class RelativeIndirectablePointer {
private:
  static_assert(std::is_integral<Offset>::value &&
                std::is_signed<Offset>::value,
                "offset type should be signed integer");
  
  /// The relative offset of the pointer's memory from the `this` pointer.
  /// If the low bit is clear, this is a direct reference; otherwise, it is
  /// an indirect reference.
  Offset RelativeOffsetPlusIndirect;

  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeIndirectablePointer() = delete;
  RelativeIndirectablePointer(RelativeIndirectablePointer &&) = delete;
  RelativeIndirectablePointer(const RelativeIndirectablePointer &) = delete;
  RelativeIndirectablePointer &operator=(RelativeIndirectablePointer &&)
    = delete;
  RelativeIndirectablePointer &operator=(const RelativeIndirectablePointer &)
    = delete;

public:
  /// Allow construction and reassignment from an absolute pointer.
  /// These always produce a direct relative offset.
  RelativeIndirectablePointer(ValueTy *absolute)
  : RelativeOffsetPlusIndirect(
      Nullable && absolute == nullptr
        ? 0
        : detail::measureRelativeOffset<Offset>(absolute, this)) {
    if (!Nullable)
      assert(absolute != nullptr &&
             "constructing non-nullable relative pointer from null");
  }
  
  RelativeIndirectablePointer &operator=(ValueTy *absolute) & {
    if (!Nullable)
      assert(absolute != nullptr &&
             "constructing non-nullable relative pointer from null");
      
    RelativeOffsetPlusIndirect = Nullable && absolute == nullptr
      ? 0
      : detail::measureRelativeOffset<Offset>(absolute, this);
    return *this;
  }

  const ValueTy *get() const & {
    static_assert(alignof(ValueTy) >= 2 && alignof(Offset) >= 2,
                  "alignment of value and offset must be at least 2 to "
                  "make room for indirectable flag");
  
    // Check for null.
    if (Nullable && RelativeOffsetPlusIndirect == 0)
      return nullptr;
    
    Offset offsetPlusIndirect = RelativeOffsetPlusIndirect;
    uintptr_t address = detail::applyRelativeOffset(this,
                                                    offsetPlusIndirect & ~1);

    // If the low bit is set, then this is an indirect address. Otherwise,
    // it's direct.
    if (offsetPlusIndirect & 1) {
      return *reinterpret_cast<IndirectType const *>(address);
    } else {
      return reinterpret_cast<const ValueTy *>(address);
    }
  }

  /// A zero relative offset encodes a null reference.
  bool isNull() const & {
    return RelativeOffsetPlusIndirect == 0;
  }
  
  operator const ValueTy* () const & {
    return get();
  }

  const ValueTy *operator->() const & {
    return get();
  }
};

/// A relative reference to an aligned object stored in memory. The reference
/// may be direct or indirect, and uses the low bit of the (assumed at least
/// 2-byte-aligned) pointer to differentiate. The remaining low bits store
/// an additional tiny integer value.
template<typename ValueTy, typename IntTy, bool Nullable = false,
         typename Offset = int32_t,
         typename IndirectType = const ValueTy *>
class RelativeIndirectablePointerIntPair {
private:
  static_assert(std::is_integral<Offset>::value &&
                std::is_signed<Offset>::value,
                "offset type should be signed integer");

  /// The relative offset of the pointer's memory from the `this` pointer.
  /// If the low bit is clear, this is a direct reference; otherwise, it is
  /// an indirect reference.
  Offset RelativeOffsetPlusIndirectAndInt;

  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeIndirectablePointerIntPair() = delete;
  RelativeIndirectablePointerIntPair(
                           RelativeIndirectablePointerIntPair &&) = delete;
  RelativeIndirectablePointerIntPair(
                      const RelativeIndirectablePointerIntPair &) = delete;
  RelativeIndirectablePointerIntPair& operator=(
                           RelativeIndirectablePointerIntPair &&) = delete;
  RelativeIndirectablePointerIntPair &operator=(
                      const RelativeIndirectablePointerIntPair &) = delete;

  // Retrieve the mask for the stored integer value.
  static Offset getIntMask() {
    return (alignof(Offset) - 1) & ~(Offset)0x01;
  }

public:
  const ValueTy *getPointer() const & {
    static_assert(alignof(ValueTy) >= 2 && alignof(Offset) >= 2,
                  "alignment of value and offset must be at least 2 to "
                  "make room for indirectable flag");

    Offset offset = (RelativeOffsetPlusIndirectAndInt & ~getIntMask());

    // Check for null.
    if (Nullable && offset == 0)
      return nullptr;

    Offset offsetPlusIndirect = offset;
    uintptr_t address = detail::applyRelativeOffset(this,
                                                    offsetPlusIndirect & ~1);

    // If the low bit is set, then this is an indirect address. Otherwise,
    // it's direct.
    if (offsetPlusIndirect & 1) {
      return *reinterpret_cast<const IndirectType *>(address);
    } else {
      return reinterpret_cast<const ValueTy *>(address);
    }
  }

  /// A zero relative offset encodes a null reference.
  bool isNull() const & {
    Offset offset = (RelativeOffsetPlusIndirectAndInt & ~getIntMask());
    return offset == 0;
  }

  IntTy getInt() const & {
    return IntTy((RelativeOffsetPlusIndirectAndInt & getIntMask()) >> 1);
  }
};

/// A relative reference to a function, intended to reference private metadata
/// functions for the current executable or dynamic library image from
/// position-independent constant data.
template<typename T, bool Nullable, typename Offset>
class RelativeDirectPointerImpl {
private:
  /// The relative offset of the function's entry point from *this.
  Offset RelativeOffset;

  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeDirectPointerImpl() = delete;
  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeDirectPointerImpl(RelativeDirectPointerImpl &&) = delete;
  RelativeDirectPointerImpl(const RelativeDirectPointerImpl &) = delete;
  RelativeDirectPointerImpl &operator=(RelativeDirectPointerImpl &&)
    = delete;
  RelativeDirectPointerImpl &operator=(const RelativeDirectPointerImpl &)
    = delete;


public:
  using ValueTy = T;
  using PointerTy = T*;

  // Allow construction and reassignment from an absolute pointer.
  RelativeDirectPointerImpl(PointerTy absolute)
    : RelativeOffset(Nullable && absolute == nullptr
                       ? 0
                       : detail::measureRelativeOffset<Offset>(absolute, this))
  {
    if (!Nullable)
      assert(absolute != nullptr &&
             "constructing non-nullable relative pointer from null");
  }
  explicit constexpr RelativeDirectPointerImpl(std::nullptr_t)
  : RelativeOffset (0) {
    static_assert(Nullable, "can't construct non-nullable pointer from null");
  }
  
  RelativeDirectPointerImpl &operator=(PointerTy absolute) & {
    if (!Nullable)
      assert(absolute != nullptr &&
             "constructing non-nullable relative pointer from null");
    RelativeOffset = Nullable && absolute == nullptr
      ? 0
      : detail::measureRelativeOffset<Offset>(absolute, this);
    return *this;
  }

  PointerTy get() const & {
    // Check for null.
    if (Nullable && RelativeOffset == 0)
      return nullptr;
    
    // The value is addressed relative to `this`.
    uintptr_t absolute = detail::applyRelativeOffset(this, RelativeOffset);
    return reinterpret_cast<PointerTy>(absolute);
  }

  /// A zero relative offset encodes a null reference.
  bool isNull() const & {
    return RelativeOffset == 0;
  }
};

template <typename T, bool Nullable = true, typename Offset = int32_t,
          typename = void>
class RelativeDirectPointer;

/// A direct relative reference to an object that is not a function pointer.
template <typename T, bool Nullable, typename Offset>
class RelativeDirectPointer<T, Nullable, Offset,
    typename std::enable_if<!std::is_function<T>::value>::type>
    : private RelativeDirectPointerImpl<T, Nullable, Offset>
{
  using super = RelativeDirectPointerImpl<T, Nullable, Offset>;
public:
  using super::get;
  using super::super;
  
  RelativeDirectPointer &operator=(T *absolute) & {
    super::operator=(absolute);
    return *this;
  }

  operator typename super::PointerTy() const & {
    return this->get();
  }

  const typename super::ValueTy *operator->() const & {
    return this->get();
  }

  using super::isNull;
};

/// A specialization of RelativeDirectPointer for function pointers,
/// allowing for calls.
template<typename T, bool Nullable, typename Offset>
class RelativeDirectPointer<T, Nullable, Offset,
    typename std::enable_if<std::is_function<T>::value>::type>
    : private RelativeDirectPointerImpl<T, Nullable, Offset>
{
  using super = RelativeDirectPointerImpl<T, Nullable, Offset>;
public:
  using super::super;

  RelativeDirectPointer &operator=(T absolute) & {
    super::operator=(absolute);
    return *this;
  }

  typename super::PointerTy get() const & {
    auto ptr = this->super::get();
#if SWIFT_PTRAUTH
    if (Nullable && !ptr)
      return ptr;
    return ptrauth_sign_unauthenticated(ptr, ptrauth_key_function_pointer, 0);
#else
    return ptr;
#endif
  }

  operator typename super::PointerTy() const & {
    return this->get();
  }

  template <typename...ArgTy>
  typename std::result_of<T* (ArgTy...)>::type operator()(ArgTy...arg) const {
#if SWIFT_PTRAUTH
    return ptrauth_sign_unauthenticated(this->super::get(),
                                        ptrauth_key_function_pointer,
                                        0)(std::forward<ArgTy>(arg)...);
#else
    return this->super::get()(std::forward<ArgTy>(arg)...);
#endif
  }

  using super::isNull;
};

/// A direct relative reference to an aligned object, with an additional
/// tiny integer value crammed into its low bits.
template<typename PointeeTy, typename IntTy, bool Nullable = false,
         typename Offset = int32_t>
class RelativeDirectPointerIntPairImpl {
  Offset RelativeOffsetPlusInt;

  /// RelativePointers should appear in statically-generated metadata. They
  /// shouldn't be constructed or copied.
  RelativeDirectPointerIntPairImpl() = delete;
  RelativeDirectPointerIntPairImpl(RelativeDirectPointerIntPairImpl &&) = delete;
  RelativeDirectPointerIntPairImpl(const RelativeDirectPointerIntPairImpl &) = delete;
  RelativeDirectPointerIntPairImpl &operator=(RelativeDirectPointerIntPairImpl &&)
    = delete;
  RelativeDirectPointerIntPairImpl &operator=(const RelativeDirectPointerIntPairImpl&)
    = delete;

  static Offset getMask() {
    return alignof(Offset) - 1;
  }

public:
  using ValueTy = PointeeTy;
  using PointerTy = PointeeTy*;

  PointerTy getPointer() const & {
    Offset offset = (RelativeOffsetPlusInt & ~getMask());

    // Check for null.
    if (Nullable && offset == 0)
      return nullptr;

    // The value is addressed relative to `this`.
    uintptr_t absolute = detail::applyRelativeOffset(this, offset);
    return reinterpret_cast<PointerTy>(absolute);
  }

  IntTy getInt() const & {
    return IntTy(RelativeOffsetPlusInt & getMask());
  }
  
  Offset getOpaqueValue() const & {
    return RelativeOffsetPlusInt;
  }
};

/// A direct relative reference to an aligned object, with an additional
/// tiny integer value crammed into its low bits.
template<typename PointeeTy, typename IntTy, bool Nullable = false,
         typename Offset = int32_t, typename = void>
class RelativeDirectPointerIntPair;

template<typename PointeeTy, typename IntTy, bool Nullable, typename Offset>
class RelativeDirectPointerIntPair<PointeeTy, IntTy, Nullable, Offset,
    typename std::enable_if<!std::is_function<PointeeTy>::value>::type>
    : private RelativeDirectPointerIntPairImpl<PointeeTy, IntTy, Nullable, Offset>
{
  using super = RelativeDirectPointerIntPairImpl<PointeeTy, IntTy, Nullable, Offset>;
public:
  using super::getPointer;
  using super::getInt;
  using super::getOpaqueValue;
};

// Type aliases for "far" relative pointers, which need to be able to reach
// across the full address space instead of only across a single small-code-
// model image.

template<typename T, bool Nullable = false>
using FarRelativeIndirectablePointer =
  RelativeIndirectablePointer<T, Nullable, intptr_t>;

template<typename T, bool Nullable = false>
using FarRelativeDirectPointer = RelativeDirectPointer<T, Nullable, intptr_t>;

} // end namespace swift

#endif // SWIFT_BASIC_RELATIVEPOINTER_H
