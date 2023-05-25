//===--- PackExpansionMatcher.h - Matching pack expansions ------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// Utilities for structural matching of sequences of types containing pack
// expansions.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_PACK_EXPANSION_MATCHER_H
#define SWIFT_AST_PACK_EXPANSION_MATCHER_H

#include "swift/AST/Type.h"
#include "swift/AST/Types.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/SmallVector.h"

namespace swift {

class ASTContext;

/// The result of a match. An important invariant is that either both types
/// are PackExpansionTypes, or both types are scalars. In particular, any
/// PackTypes are always wrapped in a PackExpansionType.
struct MatchedPair {
  Type lhs;
  Type rhs;

  unsigned lhsIdx;
  unsigned rhsIdx;

  MatchedPair(Type lhs, Type rhs, unsigned lhsIdx, unsigned rhsIdx)
    : lhs(lhs), rhs(rhs), lhsIdx(lhsIdx), rhsIdx(rhsIdx) {}
};

/// Performs a structural match of two lists of types.
///
/// The invariant is that each list must only contain at most one pack
/// expansion type. After collecting a common prefix and suffix, the
/// pack expansion on either side asborbs the remaining elements on the
/// other side.
template <typename Element>
class TypeListPackMatcher {
  ASTContext &ctx;

  ArrayRef<Element> lhsElements;
  ArrayRef<Element> rhsElements;

  std::function<bool(Type)> IsPackExpansionType;
protected:
  TypeListPackMatcher(ASTContext &ctx, ArrayRef<Element> lhs,
                      ArrayRef<Element> rhs,
                      std::function<bool(Type)> isPackExpansionType)
      : ctx(ctx), lhsElements(lhs), rhsElements(rhs),
        IsPackExpansionType(isPackExpansionType) {}

public:
  SmallVector<MatchedPair, 4> pairs;

  [[nodiscard]] bool match() {
    ArrayRef<Element> lhsElts(lhsElements);
    ArrayRef<Element> rhsElts(rhsElements);

    unsigned minLength = std::min(lhsElts.size(), rhsElts.size());

    // Consume the longest possible prefix where neither type in
    // the pair is a pack expansion type.
    unsigned prefixLength = 0;
    for (unsigned i = 0; i < minLength; ++i) {
      unsigned lhsIdx = i;
      unsigned rhsIdx = i;

      auto lhsElt = lhsElts[lhsIdx];
      auto rhsElt = rhsElts[rhsIdx];

      if (getElementLabel(lhsElt) != getElementLabel(rhsElt))
        break;

      // FIXME: Check flags

      auto lhsType = getElementType(lhsElt);
      auto rhsType = getElementType(rhsElt);

      if (IsPackExpansionType(lhsType) ||
          IsPackExpansionType(rhsType)) {
        break;
      }

      // FIXME: Check flags

      pairs.emplace_back(lhsType, rhsType, lhsIdx, rhsIdx);
      ++prefixLength;
    }

    // Consume the longest possible suffix where neither type in
    // the pair is a pack expansion type.
    unsigned suffixLength = 0;
    for (unsigned i = 0; i < minLength - prefixLength; ++i) {
      unsigned lhsIdx = lhsElts.size() - i - 1;
      unsigned rhsIdx = rhsElts.size() - i - 1;

      auto lhsElt = lhsElts[lhsIdx];
      auto rhsElt = rhsElts[rhsIdx];

      // FIXME: Check flags

      if (getElementLabel(lhsElt) != getElementLabel(rhsElt))
        break;

      auto lhsType = getElementType(lhsElt);
      auto rhsType = getElementType(rhsElt);

      if (IsPackExpansionType(lhsType) ||
          IsPackExpansionType(rhsType)) {
        break;
      }

      pairs.emplace_back(lhsType, rhsType, lhsIdx, rhsIdx);
      ++suffixLength;
    }

    assert(prefixLength + suffixLength <= lhsElts.size());
    assert(prefixLength + suffixLength <= rhsElts.size());

    // Drop the consumed prefix and suffix from each list of types.
    lhsElts = lhsElts.drop_front(prefixLength).drop_back(suffixLength);
    rhsElts = rhsElts.drop_front(prefixLength).drop_back(suffixLength);

    // If nothing remains, we're done.
    if (lhsElts.empty() && rhsElts.empty())
      return false;

    // If the left hand side is a single pack expansion type, bind it
    // to what remains of the right hand side.
    if (lhsElts.size() == 1) {
      auto lhsType = getElementType(lhsElts[0]);
      if (IsPackExpansionType(lhsType)) {
        unsigned lhsIdx = prefixLength;
        unsigned rhsIdx = prefixLength;

        SmallVector<Type, 2> rhsTypes;
        for (auto rhsElt : rhsElts) {
          if (!getElementLabel(rhsElt).empty())
            return true;

          // FIXME: Check rhs flags
          rhsTypes.push_back(getElementType(rhsElt));
        }
        auto rhs = createPackBinding(rhsTypes);

        // FIXME: Check lhs flags
        pairs.emplace_back(lhsType, rhs, lhsIdx, rhsIdx);
        return false;
      }
    }

    // If the right hand side is a single pack expansion type, bind it
    // to what remains of the left hand side.
    if (rhsElts.size() == 1) {
      auto rhsType = getElementType(rhsElts[0]);
      if (IsPackExpansionType(rhsType)) {
        unsigned lhsIdx = prefixLength;
        unsigned rhsIdx = prefixLength;

        SmallVector<Type, 2> lhsTypes;
        for (auto lhsElt : lhsElts) {
          if (!getElementLabel(lhsElt).empty())
            return true;

          // FIXME: Check lhs flags
          lhsTypes.push_back(getElementType(lhsElt));
        }
        auto lhs = createPackBinding(lhsTypes);

        // FIXME: Check rhs flags
        pairs.emplace_back(lhs, rhsType, lhsIdx, rhsIdx);
        return false;
      }
    }

    // If both sides have the same number of elements and all of
    // them are pack expansions there is not going to be any
    // expansion "absorption" and it's okay to match per-index.
    //
    // Like in all previous cases the callers are responsible
    // to check whether the element types actually line up,
    // this is a purely structural match.
    if (lhsElts.size() == rhsElts.size()) {
      for (unsigned i = 0, n = lhsElts.size(); i != n; ++i) {
        auto lhsType = getElementType(lhsElts[i]);
        auto rhsType = getElementType(rhsElts[i]);

        if (IsPackExpansionType(lhsType) && IsPackExpansionType(rhsType)) {
          pairs.emplace_back(lhsType, rhsType, i, i);
        } else {
          pairs.clear();
          return true;
        }
      }

      return false;
    }

    // Otherwise, all remaining possibilities are invalid:
    // - Neither side has any pack expansions, and they have different lengths.
    // - One side has a pack expansion but the other side is too short, eg
    //   {Int, T..., Float} vs {Int}.
    // - The prefix and suffix are mismatched, so we're left with something
    //   like {T..., Int} vs {Float, U...}.
    return true;
  }

private:
  Identifier getElementLabel(const Element &) const;
  Type getElementType(const Element &) const;
  ParameterTypeFlags getElementFlags(const Element &) const;

  Type createPackBinding(ArrayRef<Type> types) const {
    // If there is only one element and it's a PackExpansionType,
    // return it directly.
    if (types.size() == 1 && IsPackExpansionType(types.front()))
      return types.front();

    // Otherwise, wrap the elements in PackExpansionType(PackType(...)).
    auto *packType = PackType::get(ctx, types);
    return PackExpansionType::get(packType, packType);
  }
};

/// Performs a structural match of two lists of tuple elements.
///
/// The invariant is that each list must only contain at most one pack
/// expansion type. After collecting a common prefix and suffix, the
/// pack expansion on either side asborbs the remaining elements on the
/// other side.
class TuplePackMatcher : public TypeListPackMatcher<TupleTypeElt> {
public:
  TuplePackMatcher(
      TupleType *lhsTuple, TupleType *rhsTuple,
      std::function<bool(Type)> isPackExpansionType =
          [](Type T) { return T->is<PackExpansionType>(); })
      : TypeListPackMatcher(lhsTuple->getASTContext(), lhsTuple->getElements(),
                            rhsTuple->getElements(), isPackExpansionType) {}
};

/// Performs a structural match of two lists of (unlabeled) function
/// parameters.
///
/// The invariant is that each list must only contain at most one pack
/// expansion type. After collecting a common prefix and suffix, the
/// pack expansion on either side asborbs the remaining elements on the
/// other side.
class ParamPackMatcher : public TypeListPackMatcher<AnyFunctionType::Param> {
public:
  ParamPackMatcher(
      ArrayRef<AnyFunctionType::Param> lhsParams,
      ArrayRef<AnyFunctionType::Param> rhsParams, ASTContext &ctx,
      std::function<bool(Type)> isPackExpansionType =
          [](Type T) { return T->is<PackExpansionType>(); })
      : TypeListPackMatcher(ctx, lhsParams, rhsParams, isPackExpansionType) {}
};

/// Performs a structural match of two lists of types.
///
/// The invariant is that each list must only contain at most one pack
/// expansion type. After collecting a common prefix and suffix, the
/// pack expansion on either side asborbs the remaining elements on the
/// other side.
class PackMatcher : public TypeListPackMatcher<Type> {
public:
  PackMatcher(
      ArrayRef<Type> lhsTypes, ArrayRef<Type> rhsTypes, ASTContext &ctx,
      std::function<bool(Type)> isPackExpansionType =
          [](Type T) { return T->is<PackExpansionType>(); })
      : TypeListPackMatcher(ctx, lhsTypes, rhsTypes, isPackExpansionType) {}
};

} // end namespace swift

#endif // SWIFT_AST_PACK_EXPANSION_MATCHER_H
