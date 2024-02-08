//===-- LayoutConstraint.h - Layout constraints types and APIs --*- C++ -*-===//
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
// This file defines types and APIs for layout constraints.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_LAYOUT_CONSTRAINT_H
#define SWIFT_LAYOUT_CONSTRAINT_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/LayoutConstraintKind.h"
#include "swift/AST/PrintOptions.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/Basic/Debug.h"
#include "swift/Basic/SourceLoc.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/FoldingSet.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

class ASTPrinter;

/// This is a class representing the layout constraint.
class LayoutConstraintInfo
    : public llvm::FoldingSetNode,
      public ASTAllocated<std::aligned_storage<8, 8>::type> {
  friend class LayoutConstraint;
  // Alignment of the layout in bytes.
  const unsigned Alignment : 16;
  // Size of the layout in bits.
  const unsigned SizeInBits : 24;
  // Kind of the layout.
  const LayoutConstraintKind Kind;

  LayoutConstraintInfo()
      : Alignment(0), SizeInBits(0), Kind(LayoutConstraintKind::UnknownLayout) {
  }

  LayoutConstraintInfo(const LayoutConstraintInfo &Layout)
      : Alignment(Layout.Alignment), SizeInBits(Layout.SizeInBits),
        Kind(Layout.Kind) {
  }

  LayoutConstraintInfo(LayoutConstraintKind Kind)
      : Alignment(0), SizeInBits(0), Kind(Kind) {
    assert(!isKnownSizeTrivial() && "Size in bits should be specified");
  }

  LayoutConstraintInfo(LayoutConstraintKind Kind, unsigned SizeInBits,
                       unsigned Alignment)
      : Alignment(Alignment), SizeInBits(SizeInBits), Kind(Kind) {
    assert(
        isTrivial() &&
        "Size in bits should be specified only for trivial layout constraints");
  }
  public:
  LayoutConstraintKind getKind() const { return Kind; }

  bool isKnownLayout() const {
    return isKnownLayout(Kind);
  }

  bool isFixedSizeTrivial() const {
    return isFixedSizeTrivial(Kind);
  }

  bool isKnownSizeTrivial() const {
    return isKnownSizeTrivial(Kind);
  }

  bool isAddressOnlyTrivial() const {
    return isAddressOnlyTrivial(Kind);
  }

  bool isTrivial() const {
    return isTrivial(Kind);
  }

  bool isRefCountedObject() const {
    return isRefCountedObject(Kind);
  }

  bool isNativeRefCountedObject() const {
    return isNativeRefCountedObject(Kind);
  }

  bool isClass() const {
    return isClass(Kind);
  }

  bool isNativeClass() const {
    return isNativeClass(Kind);
  }

  bool isRefCounted() const {
    return isRefCounted(Kind);
  }

  bool isNativeRefCounted() const {
    return isNativeRefCounted(Kind);
  }

  bool isBridgeObject() const { return isBridgeObject(Kind); }

  bool isTrivialStride() const { return isTrivialStride(Kind); }

  unsigned getTrivialSizeInBytes() const {
    assert(isKnownSizeTrivial());
    return (SizeInBits + 7) / 8;
  }

  unsigned getMaxTrivialSizeInBytes() const {
    assert(isKnownSizeTrivial());
    return (SizeInBits + 7) / 8;
  }

  unsigned getTrivialSizeInBits() const {
    assert(isKnownSizeTrivial());
    return SizeInBits;
  }

  unsigned getMaxTrivialSizeInBits() const {
    assert(isKnownSizeTrivial());
    return SizeInBits;
  }

  unsigned getAlignmentInBits() const {
    return Alignment;
  }

  unsigned getAlignmentInBytes() const {
    assert(isKnownSizeTrivial() || isTrivialStride());
    if (Alignment)
      return Alignment;

    // There is no explicitly defined alignment. Try to come up with a
    // reasonable one.

    // If the size is a power of 2, use it also for the default alignment.
    auto SizeInBytes = getTrivialSizeInBytes();
    if (llvm::isPowerOf2_32(SizeInBytes))
      return SizeInBytes * 8;

    // Otherwise assume the alignment of 8 bytes.
    return 8*8;
  }

  unsigned getTrivialStride() const {
    assert(isTrivialStride());
    return (SizeInBits + 7) / 8;
  }

  unsigned getTrivialStrideInBits() const {
    assert(isTrivialStride());
    return SizeInBits;
  }

  operator bool() const {
    return isKnownLayout();
  }

  bool operator==(const LayoutConstraintInfo &rhs) const {
    return getKind() == rhs.getKind() && SizeInBits == rhs.SizeInBits &&
           Alignment == rhs.Alignment;
  }

  bool operator!=(LayoutConstraintInfo rhs) const { return !(*this == rhs); }

  void print(raw_ostream &OS, const PrintOptions &PO = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &PO) const;

  /// Return the layout constraint as a string, for use in diagnostics only.
  std::string getString(const PrintOptions &PO = PrintOptions()) const;

  /// Return the name of this layout constraint.
  StringRef getName(bool internalName = false) const;

  /// Return the name of a layout constraint with a given kind.
  static StringRef getName(LayoutConstraintKind Kind, bool internalName = false);

  static bool isKnownLayout(LayoutConstraintKind Kind);

  static bool isFixedSizeTrivial(LayoutConstraintKind Kind);

  static bool isKnownSizeTrivial(LayoutConstraintKind Kind);

  static bool isAddressOnlyTrivial(LayoutConstraintKind Kind);

  static bool isTrivial(LayoutConstraintKind Kind);

  static bool isRefCountedObject(LayoutConstraintKind Kind);

  static bool isNativeRefCountedObject(LayoutConstraintKind Kind);

  static bool isAnyRefCountedObject(LayoutConstraintKind Kind);

  static bool isClass(LayoutConstraintKind Kind);

  static bool isNativeClass(LayoutConstraintKind Kind);

  static bool isRefCounted(LayoutConstraintKind Kind);

  static bool isNativeRefCounted(LayoutConstraintKind Kind);

  static bool isBridgeObject(LayoutConstraintKind Kind);

  static bool isTrivialStride(LayoutConstraintKind Kind);

  /// Uniquing for the LayoutConstraintInfo.
  void Profile(llvm::FoldingSetNodeID &ID) {
    Profile(ID, Kind, SizeInBits, Alignment);
  }

  static void Profile(llvm::FoldingSetNodeID &ID,
                      LayoutConstraintKind Kind,
                      unsigned SizeInBits,
                      unsigned Alignment);

  // Representation of the non-parameterized layouts.
  static LayoutConstraintInfo UnknownLayoutConstraintInfo;
  static LayoutConstraintInfo RefCountedObjectConstraintInfo;
  static LayoutConstraintInfo NativeRefCountedObjectConstraintInfo;
  static LayoutConstraintInfo ClassConstraintInfo;
  static LayoutConstraintInfo NativeClassConstraintInfo;
  static LayoutConstraintInfo TrivialConstraintInfo;
  static LayoutConstraintInfo BridgeObjectConstraintInfo;
};

/// A wrapper class containing a reference to the actual LayoutConstraintInfo
/// object.
class LayoutConstraint {
  LayoutConstraintInfo *Ptr;
  public:
  /*implicit*/ LayoutConstraint(LayoutConstraintInfo *P = 0) : Ptr(P) {}

  static LayoutConstraint getLayoutConstraint(const LayoutConstraint &Layout,
                                              ASTContext &C);

  static LayoutConstraint getLayoutConstraint(LayoutConstraintKind Kind,
                                              ASTContext &C);

  static LayoutConstraint getLayoutConstraint(LayoutConstraintKind Kind);

  static LayoutConstraint getLayoutConstraint(LayoutConstraintKind Kind,
                                              unsigned SizeInBits,
                                              unsigned Alignment,
                                              ASTContext &C);

  static LayoutConstraint getUnknownLayout();

  LayoutConstraintInfo *getPointer() const { return Ptr; }

  bool isNull() const { return Ptr == 0; }

  LayoutConstraintInfo *operator->() const { return Ptr; }

  /// Merge these two constraints and return a more specific one
  /// or fail if they're incompatible and return an unknown constraint.
  LayoutConstraint merge(LayoutConstraint Other);

  explicit operator bool() const { return Ptr != 0; }

  SWIFT_DEBUG_DUMP;
  void dump(raw_ostream &os, unsigned indent = 0) const;

  void print(raw_ostream &OS, const PrintOptions &PO = PrintOptions()) const;
  void print(ASTPrinter &Printer, const PrintOptions &PO) const;

  /// Return the layout constraint as a string, for use in diagnostics only.
  std::string getString(const PrintOptions &PO = PrintOptions()) const;

  friend llvm::hash_code hash_value(const LayoutConstraint &layout) {
    return hash_value(layout.getPointer());
  }

  bool operator==(LayoutConstraint rhs) const {
    if (isNull() && rhs.isNull())
      return true;
    return *getPointer() == *rhs.getPointer();
  }

  bool operator!=(LayoutConstraint rhs) const {
    return !(*this == rhs);
  }

  /// Defines a somewhat arbitrary linear order on layout constraints.
  /// -1 if this < rhs, 0 if this == rhs, 1 if this > rhs.
  int compare(LayoutConstraint rhs) const;
};

// Permit direct uses of isa/cast/dyn_cast on LayoutConstraint.
template <class X> inline bool isa(LayoutConstraint LC) {
  return isa<X>(LC.getPointer());
}
template <class X> inline X cast_or_null(LayoutConstraint LC) {
  return cast_or_null<X>(LC.getPointer());
}
template <class X> inline X dyn_cast(LayoutConstraint LC) {
  return dyn_cast<X>(LC.getPointer());
}
template <class X> inline X dyn_cast_or_null(LayoutConstraint LC) {
  return dyn_cast_or_null<X>(LC.getPointer());
}

/// LayoutConstraintLoc - Provides source location information for a
/// parsed layout constraint.
struct LayoutConstraintLoc {
private:
  LayoutConstraint Layout;
  SourceLoc Loc;

public:
  LayoutConstraintLoc(LayoutConstraint Layout, SourceLoc Loc)
      : Layout(Layout), Loc(Loc) {}

  bool isError() const;

  /// Get the representative location of this type, for diagnostic
  /// purposes.
  SourceLoc getLoc() const { return Loc; }

  SourceRange getSourceRange() const;

  bool hasLocation() const { return Loc.isValid(); }
  LayoutConstraint getLayoutConstraint() const { return Layout; }

  bool isNull() const { return Layout.isNull(); }
};

/// Checks if ID is a name of a layout constraint and returns this
/// constraint. If ID does not match any known layout constraint names,
/// returns UnknownLayout.
LayoutConstraint getLayoutConstraint(Identifier ID, ASTContext &Ctx);

} // end namespace swift

LLVM_DECLARE_TYPE_ALIGNMENT(swift::LayoutConstraintInfo, swift::TypeAlignInBits)

namespace llvm {
static inline raw_ostream &
operator<<(raw_ostream &OS, swift::LayoutConstraint LC) {
  LC->print(OS);
  return OS;
}

// A LayoutConstraint casts like a LayoutConstraintInfo*.
template <> struct simplify_type<const ::swift::LayoutConstraint> {
  typedef ::swift::LayoutConstraintInfo *SimpleType;
  static SimpleType getSimplifiedValue(const ::swift::LayoutConstraint &Val) {
    return Val.getPointer();
  }
};

template <>
struct simplify_type<::swift::LayoutConstraint>
    : public simplify_type<const ::swift::LayoutConstraint> {};

// LayoutConstraint hashes just like pointers.
template <> struct DenseMapInfo<swift::LayoutConstraint> {
  static swift::LayoutConstraint getEmptyKey() {
    return llvm::DenseMapInfo<swift::LayoutConstraintInfo *>::getEmptyKey();
  }
  static swift::LayoutConstraint getTombstoneKey() {
    return llvm::DenseMapInfo<swift::LayoutConstraintInfo *>::getTombstoneKey();
  }
  static unsigned getHashValue(swift::LayoutConstraint Val) {
    return DenseMapInfo<swift::LayoutConstraintInfo *>::getHashValue(
        Val.getPointer());
  }
  static bool isEqual(swift::LayoutConstraint LHS,
                      swift::LayoutConstraint RHS) {
    return LHS.getPointer() == RHS.getPointer();
  }
};

// A LayoutConstraint is "pointer like".
template <> struct PointerLikeTypeTraits<swift::LayoutConstraint> {
public:
  static inline void *getAsVoidPointer(swift::LayoutConstraint I) {
    return (void *)I.getPointer();
  }
  static inline swift::LayoutConstraint getFromVoidPointer(void *P) {
    return (swift::LayoutConstraintInfo *)P;
  }
  enum { NumLowBitsAvailable = swift::TypeAlignInBits };
};
} // end namespace llvm

#endif
