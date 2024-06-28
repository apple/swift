//===--- LayoutConstraint.cpp - Layout constraints types and APIs ---------===//
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
// This file implements APIs for layout constraints.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"

namespace swift {

/// This helper function is typically used by the parser to
/// map a type name to a corresponding layout constraint if possible.
LayoutConstraint getLayoutConstraint(Identifier ID, ASTContext &Ctx) {
  if (ID == Ctx.Id_TrivialLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::TrivialOfExactSize, 0, 0, Ctx);

  if (ID == Ctx.Id_TrivialAtMostLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::TrivialOfAtMostSize, 0, 0, Ctx);

  if (ID == Ctx.Id_RefCountedObjectLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::RefCountedObject, Ctx);

  if (ID == Ctx.Id_NativeRefCountedObjectLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::NativeRefCountedObject, Ctx);

  if (ID == Ctx.Id_ClassLayout)
    return LayoutConstraint::getLayoutConstraint(
      LayoutConstraintKind::Class, Ctx);

  if (ID == Ctx.Id_NativeClassLayout)
    return LayoutConstraint::getLayoutConstraint(
      LayoutConstraintKind::NativeClass, Ctx);

  if (ID == Ctx.Id_BridgeObjectLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::BridgeObject, Ctx);

  if (ID == Ctx.Id_TrivialStrideLayout)
    return LayoutConstraint::getLayoutConstraint(
        LayoutConstraintKind::TrivialStride, 0, 0, Ctx);

  return LayoutConstraint::getLayoutConstraint(
      LayoutConstraintKind::UnknownLayout, Ctx);
}

StringRef LayoutConstraintInfo::getName(bool internalName) const {
  return getName(getKind(), internalName);
}

StringRef LayoutConstraintInfo::getName(LayoutConstraintKind Kind, bool internalName) {
  switch (Kind) {
  case LayoutConstraintKind::UnknownLayout:
    return "_UnknownLayout";
  case LayoutConstraintKind::Class:
    return internalName ? "_Class" : "AnyObject";
  case LayoutConstraintKind::NativeClass:
    return "_NativeClass";
  case LayoutConstraintKind::RefCountedObject:
    return "_RefCountedObject";
  case LayoutConstraintKind::NativeRefCountedObject:
    return "_NativeRefCountedObject";
  case LayoutConstraintKind::Trivial:
    return "_Trivial";
  case LayoutConstraintKind::TrivialOfAtMostSize:
    return "_TrivialAtMost";
  case LayoutConstraintKind::TrivialOfExactSize:
    return "_Trivial";
  case LayoutConstraintKind::BridgeObject:
    return "_BridgeObject";
  case LayoutConstraintKind::TrivialStride:
    return "_TrivialStride";
  }

  llvm_unreachable("Unhandled LayoutConstraintKind in switch.");
}

/// Uniquing for the LayoutConstraintInfo.
void LayoutConstraintInfo::Profile(llvm::FoldingSetNodeID &ID,
                                   LayoutConstraintKind Kind,
                                   unsigned SizeInBits,
                                   unsigned Alignment) {
  ID.AddInteger((unsigned)Kind);
  ID.AddInteger(SizeInBits);
  ID.AddInteger(Alignment);
}

bool LayoutConstraintInfo::isKnownLayout(LayoutConstraintKind Kind) {
  return Kind != LayoutConstraintKind::UnknownLayout;
}

bool LayoutConstraintInfo::isFixedSizeTrivial(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::TrivialOfExactSize;
}

bool LayoutConstraintInfo::isKnownSizeTrivial(LayoutConstraintKind Kind) {
  return Kind > LayoutConstraintKind::UnknownLayout &&
         Kind < LayoutConstraintKind::Trivial;
}

bool LayoutConstraintInfo::isAddressOnlyTrivial(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::Trivial;
}

bool LayoutConstraintInfo::isTrivial(LayoutConstraintKind Kind) {
  return (Kind > LayoutConstraintKind::UnknownLayout &&
          Kind <= LayoutConstraintKind::Trivial) ||
         Kind == LayoutConstraintKind::TrivialStride;
}

bool LayoutConstraintInfo::isRefCountedObject(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::RefCountedObject;
}

bool LayoutConstraintInfo::isNativeRefCountedObject(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::NativeRefCountedObject;
}

bool LayoutConstraintInfo::isAnyRefCountedObject(LayoutConstraintKind Kind) {
  return isRefCountedObject(Kind) || isNativeRefCountedObject(Kind);
}

bool LayoutConstraintInfo::isClass(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::Class ||
         Kind == LayoutConstraintKind::NativeClass;
}

bool LayoutConstraintInfo::isNativeClass(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::NativeClass;
}

bool LayoutConstraintInfo::isRefCounted(LayoutConstraintKind Kind) {
  return isAnyRefCountedObject(Kind) || isClass(Kind) || isBridgeObject(Kind);
}

bool LayoutConstraintInfo::isNativeRefCounted(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::NativeRefCountedObject ||
         Kind == LayoutConstraintKind::NativeClass;
}

bool LayoutConstraintInfo::isBridgeObject(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::BridgeObject;
}

bool LayoutConstraintInfo::isTrivialStride(LayoutConstraintKind Kind) {
  return Kind == LayoutConstraintKind::TrivialStride;
}

SourceRange LayoutConstraintLoc::getSourceRange() const { return getLoc(); }

#define MERGE_LOOKUP(lhs, rhs)                                                 \
  mergeTable[int(lhs)][int(rhs)]

// Shortcut for spelling the whole enumerator.
#define E(X) LayoutConstraintKind::X
#define MERGE_CONFLICT LayoutConstraintKind::UnknownLayout

/// This is a 2-D table defining the merging rules for layout constraints.
/// It is indexed by means of LayoutConstraintKind.
/// mergeTable[i][j] tells the kind of a layout constraint is the result
/// of merging layout constraints of kinds 'i' and 'j'.
///
/// Some of the properties of the merge table, where C is any type of layout
/// constraint:
///   UnknownLayout x C -> C
///   C x UnknownLayout -> C
///   C x C -> C
///   mergeTable[i][j] == mergeTable[j][i], i.e. the table is symmetric.
///   mergeTable[i][j] == UnknownLayout if merging layout constraints of kinds i
///   and j would result in a conflict.
static LayoutConstraintKind mergeTable[unsigned(E(LastLayout)) +
                                       1][unsigned(E(LastLayout)) + 1] = {
    // Initialize the row for UnknownLayout.
    {E(/* UnknownLayout */ UnknownLayout),
     E(/* TrivialOfExactSize */ TrivialOfExactSize),
     E(/* TrivialOfAtMostSize */ TrivialOfAtMostSize), E(/* Trivial */ Trivial),
     E(/* Class */ Class), E(/* NativeClass */ NativeClass),
     E(/* RefCountedObject*/ RefCountedObject),
     E(/* NativeRefCountedObject */ NativeRefCountedObject), MERGE_CONFLICT,
     MERGE_CONFLICT},

    // Initialize the row for TrivialOfExactSize.
    {E(/* UnknownLayout */ TrivialOfExactSize),
     E(/* TrivialOfExactSize */ TrivialOfExactSize), MERGE_CONFLICT,
     E(/* Trivial */ TrivialOfExactSize), MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT},

    // Initialize the row for TrivialOfAtMostSize.
    {E(/* UnknownLayout */ TrivialOfAtMostSize), MERGE_CONFLICT,
     E(/* TrivialOfAtMostSize */ TrivialOfAtMostSize),
     E(/* Trivial */ TrivialOfAtMostSize), MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT},

    // Initialize the row for Trivial.
    {E(/* UnknownLayout */ Trivial),
     E(/* TrivialOfExactSize */ TrivialOfExactSize),
     E(/* TrivialOfAtMostSize */ TrivialOfAtMostSize), E(/* Trivial */ Trivial),
     MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT},

    // Initialize the row for Class.
    {E(/* UnknownLayout*/ Class), MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, E(/* Class */ Class), E(/* NativeClass */ NativeClass),
     E(/* RefCountedObject */ Class),
     E(/* NativeRefCountedObject */ NativeClass), MERGE_CONFLICT,
     MERGE_CONFLICT},

    // Initialize the row for NativeClass.
    {E(/* UnknownLayout */ NativeClass), MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, E(/* Class */ NativeClass),
     E(/* NativeClass */ NativeClass), E(/* RefCountedObject */ NativeClass),
     E(/* NativeRefCountedObject */ NativeClass), MERGE_CONFLICT,
     MERGE_CONFLICT},

    // Initialize the row for RefCountedObject.
    {E(/* UnknownLayout */ RefCountedObject), MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, E(/* Class */ Class), E(/* NativeClass */ NativeClass),
     E(/* RefCountedObject */ RefCountedObject),
     E(/* NativeRefCountedObject */ NativeRefCountedObject), MERGE_CONFLICT,
     MERGE_CONFLICT},

    // Initialize the row for NativeRefCountedObject.
    {E(/* UnknownLayout */ NativeRefCountedObject), MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT, E(/* Class */ NativeClass),
     E(/* NativeClass */ NativeClass),
     E(/* RefCountedObject */ NativeRefCountedObject),
     E(/* NativeRefCountedObject*/ NativeRefCountedObject), MERGE_CONFLICT,
     MERGE_CONFLICT},

    {E(BridgeObject), MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT,
     E(/*BridgeObject*/ BridgeObject), MERGE_CONFLICT},

    {E(TrivialStride), MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT, MERGE_CONFLICT,
     MERGE_CONFLICT, E(/*TrivialStride*/ TrivialStride)},
};

#undef E

// Fixed-size trivial constraint can be combined with AtMostSize trivial
// constraint into a fixed-size trivial constraint, if
// fixed_size_layout.size <= at_most_size_layout.size and
// their alignment requirements are not contradicting.
//
// Only merges if LHS would become the result of the merge.
static LayoutConstraint
mergeKnownSizeTrivialConstraints(LayoutConstraint LHS, LayoutConstraint RHS) {
  assert(LHS->isKnownSizeTrivial() && RHS->isKnownSizeTrivial());

  // LHS should be fixed-size.
  if (!LHS->isFixedSizeTrivial())
    return LayoutConstraint::getUnknownLayout();

  // RHS should be at-most-size.
  if (RHS->isFixedSizeTrivial())
    return LayoutConstraint::getUnknownLayout();

  // Check that sizes are compatible, i.e.
  // fixed_size_layout.size <= at_most_size_layout.size
  if (LHS->getMaxTrivialSizeInBits() > RHS->getMaxTrivialSizeInBits())
    return LayoutConstraint::getUnknownLayout();

  // Check alignments

  // Quick exit if at_most_size_layout does not care about the alignment.
  if (!RHS->getAlignmentInBits())
    return LHS;

  // Check if fixed_size_layout.alignment is a multiple of
  // at_most_size_layout.alignment.
  if (LHS->getAlignmentInBits() &&
      LHS->getAlignmentInBits() % RHS->getAlignmentInBits() == 0)
    return LHS;

  return LayoutConstraint::getUnknownLayout();
}

LayoutConstraint
LayoutConstraint::merge(LayoutConstraint Other) {
  auto Self = *this;

  // If both constraints are the same, they are always equal.
  if (Self == Other)
    return Self;

  // TrivialOfExactSize and TrivialOfAtMostSize are a special case,
  // because not only their kind, but their parameters need to be compared as
  // well.
  if (Self->isKnownSizeTrivial() && Other->isKnownSizeTrivial()) {
    // If we got here, it means that the Self == Other check above has failed.
    // And that could only happen if either the kinds are different or
    // size/alignment parameters are different.

    // Try to merge fixed-size constraint with an at-most-size constraint,
    // if possible.
    LayoutConstraint MergedKnownSizeTrivial;
    MergedKnownSizeTrivial = mergeKnownSizeTrivialConstraints(Self, Other);
    if (MergedKnownSizeTrivial->isKnownLayout())
      return MergedKnownSizeTrivial;

    MergedKnownSizeTrivial = mergeKnownSizeTrivialConstraints(Other, Self);
    if (MergedKnownSizeTrivial->isKnownLayout())
      return MergedKnownSizeTrivial;

    return LayoutConstraint::getUnknownLayout();
  }

  // Lookup in the mergeTable if this combination of layouts can be merged.
  auto mergeKind = MERGE_LOOKUP(Self->getKind(), Other->getKind());
  // The merge table should be symmetric.
  assert(mergeKind == MERGE_LOOKUP(Other->getKind(), Self->getKind()));

  // Merge is not possible, report a conflict.
  if (mergeKind == LayoutConstraintKind::UnknownLayout)
    return LayoutConstraint::getUnknownLayout();

  if (Self->getKind() == mergeKind)
    return Self;

  if (Other->getKind() == mergeKind)
    return Other;

  // The result of the merge is not equal to any of the input constraints, e.g.
  // Class x NativeRefCountedObject -> NativeClass.
  return LayoutConstraint::getLayoutConstraint(mergeKind);
}

LayoutConstraint
LayoutConstraint::getLayoutConstraint(LayoutConstraintKind Kind) {
  assert(!LayoutConstraintInfo::isKnownSizeTrivial(Kind));
  assert(!LayoutConstraintInfo::isTrivialStride(Kind));
  switch(Kind) {
  case LayoutConstraintKind::Trivial:
    return LayoutConstraint(&LayoutConstraintInfo::TrivialConstraintInfo);
  case LayoutConstraintKind::NativeClass:
    return LayoutConstraint(&LayoutConstraintInfo::NativeClassConstraintInfo);
  case LayoutConstraintKind::Class:
    return LayoutConstraint(&LayoutConstraintInfo::ClassConstraintInfo);
  case LayoutConstraintKind::NativeRefCountedObject:
    return LayoutConstraint(
        &LayoutConstraintInfo::NativeRefCountedObjectConstraintInfo);
  case LayoutConstraintKind::RefCountedObject:
    return LayoutConstraint(
        &LayoutConstraintInfo::RefCountedObjectConstraintInfo);
  case LayoutConstraintKind::UnknownLayout:
    return LayoutConstraint(&LayoutConstraintInfo::UnknownLayoutConstraintInfo);
  case LayoutConstraintKind::BridgeObject:
    return LayoutConstraint(&LayoutConstraintInfo::BridgeObjectConstraintInfo);
  case LayoutConstraintKind::TrivialOfAtMostSize:
  case LayoutConstraintKind::TrivialOfExactSize:
  case LayoutConstraintKind::TrivialStride:
    llvm_unreachable("Wrong layout constraint kind");
  }
  llvm_unreachable("unhandled kind");
}

LayoutConstraint LayoutConstraint::getUnknownLayout() {
  return LayoutConstraint(&LayoutConstraintInfo::UnknownLayoutConstraintInfo);
}

LayoutConstraintInfo LayoutConstraintInfo::UnknownLayoutConstraintInfo;

LayoutConstraintInfo LayoutConstraintInfo::RefCountedObjectConstraintInfo(
    LayoutConstraintKind::RefCountedObject);

LayoutConstraintInfo LayoutConstraintInfo::NativeRefCountedObjectConstraintInfo(
    LayoutConstraintKind::NativeRefCountedObject);

LayoutConstraintInfo LayoutConstraintInfo::ClassConstraintInfo(
    LayoutConstraintKind::Class);

LayoutConstraintInfo LayoutConstraintInfo::NativeClassConstraintInfo(
    LayoutConstraintKind::NativeClass);

LayoutConstraintInfo LayoutConstraintInfo::TrivialConstraintInfo(
    LayoutConstraintKind::Trivial);

LayoutConstraintInfo LayoutConstraintInfo::BridgeObjectConstraintInfo(
    LayoutConstraintKind::BridgeObject);

int LayoutConstraint::compare(LayoutConstraint rhs) const {
  if (Ptr->getKind() != rhs->getKind())
    return int(rhs->getKind()) - int(Ptr->getKind());

  if (Ptr->SizeInBits != rhs->SizeInBits)
    return int(rhs->SizeInBits) - int(Ptr->SizeInBits);

  if (Ptr->Alignment != rhs->Alignment)
    return int(rhs->Alignment) - int(Ptr->Alignment);

  assert(*this == rhs);
  return 0;
}

} // end namespace swift
