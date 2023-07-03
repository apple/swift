//===--- TupleGenerators.h - Generators for tuple components ----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file defines several generator classes useful for destructuring
// tuples in various situations that arise in SIL generation.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SILGEN_TUPLEINPUTGENERATOR_H
#define SWIFT_SILGEN_TUPLEINPUTGENERATOR_H

#include "swift/SIL/SILValue.h"
#include "swift/SIL/AbstractionPatternGenerators.h"
#include "swift/AST/Types.h"
#include "swift/Basic/ExternalUnion.h"
#include "swift/Basic/Generators.h"

namespace swift {
namespace Lowering {

/// A generator for destructuring a tuple type that is recursively
/// expanded in some sequence.  In SIL, this sort of expansion
/// happens in both function parameters and function results.
/// The expectation is that the client will use this generator
/// to walk the sequence and then handle the element cases itself.
///
/// Unlike TupleElementGenerator, the iteration visits each
/// individual element of the substituted tuple type.  This makes
/// this generator more appropriate for use cases where the elements
/// must all be visited, such as when iterating the elements of two
/// tuples in paralle.
///
/// In order to properly handle pack expansions within tuples,
/// especially any empty pack expansions in the original tuple type,
/// this generator must claim the packs as it goes.  Since pack
/// values are typically claimed from a sequence that interleaves
/// packs with other values, it is important that clients claim
/// non-pack values from the sequence immediately when iteration
/// reaches them.
class ExpandedTupleInputGenerator {
  const ASTContext &ctx;
  SimpleGeneratorRef<ManagedValue> packInputs;
  TupleElementGenerator origElt;

  /// If origElt.isPackExpansion(), this is the pack currently
  /// being destructured.
  ManagedValue packValue;

  /// If origElt.isPackExpansion(), this is the formal type
  /// of the orig pack.
  CanPackType formalPackType;

  /// The current index within origElt.getSubstEltTypes().
  unsigned substEltIndex;

  /// Precondition: we aren't finished visiting orig elements,
  /// and we've already readied the current orig element.
  ///
  /// Ready the next subst element (the one at substEltIndex)
  /// from the current orig element.  If we've exhausted the supply
  /// of subst elements from this orig element, advance to the
  /// next orig element and repeat.
  ///
  /// Postcondition: we're either finished or properly configured
  /// with a subst element that hasn't been presented before.
  void readyNextSubstElement() {
    while (true) {
      assert(!origElt.isFinished());
      assert(substEltIndex <= origElt.getSubstTypes().size());

      // If we haven't reached the limit of the current element yet,
      // continue.
      if (substEltIndex != origElt.getSubstTypes().size())
        return;

      // Otherwise, advance, and ready the next orig element if we
      // didn't finish.
      origElt.advance();
      if (origElt.isFinished()) return;
      readyOrigElement();
    }
  }

  /// Ready the current orig element.
  void readyOrigElement() {
    substEltIndex = 0;
    if (origElt.isOrigPackExpansion()) {
      // The pack value exists in the lowered inputs and must be
      // claimed whether it contains formal elements or not.
      packValue = packInputs.claimNext();

      // We don't need to do any other set up if we're going to
      // immediately move past it, though.
      if (origElt.getSubstTypes().empty())
        return;

      // Compute a formal pack type for the pack.
      formalPackType = CanPackType::get(ctx, origElt.getSubstTypes());
    }
  }

  void updatePackValue(ManagedValue newPackValue) {
    assert(isOrigPackExpansion());
    packValue = newPackValue;
  }

public:
  ExpandedTupleInputGenerator(const ASTContext &ctx,
                              SimpleGeneratorRef<ManagedValue> inputs,
                              AbstractionPattern origTupleType,
                              CanType substType)
    : ctx(ctx), packInputs(inputs), origElt(origTupleType, substType) {

    if (!origElt.isFinished()) {
      readyOrigElement();
      readyNextSubstElement();
    }
  }

  /// Is this generator finished?  If so, the getters below may not be used.
  bool isFinished() const {
    return origElt.isFinished();
  }

  bool doesOrigTupleVanish() const {
    return origElt.doesOrigTupleVanish();
  }

  bool isOrigPackExpansion() const {
    return origElt.isOrigPackExpansion();
  }

  AbstractionPattern getOrigType() const {
    return origElt.isOrigPackExpansion()
             ? origElt.getOrigType().getPackExpansionPatternType()
             : origElt.getOrigType();
  }

  /// Return the index of the current element in the substituted
  /// tuple type.
  unsigned getSubstElementIndex() const {
    return origElt.getSubstIndex() + substEltIndex;
  }

  CanType getSubstType() const {
    return origElt.getSubstTypes()[substEltIndex];
  }

  bool isSubstPackExpansion() const {
    return isa<PackExpansionType>(getSubstType());
  }

  ManagedValue getPackValue() const {
    assert(isOrigPackExpansion());
    return packValue;
  }

  unsigned getPackComponentIndex() const {
    assert(isOrigPackExpansion());
    return substEltIndex;
  }

  CanPackType getFormalPackType() const {
    assert(isOrigPackExpansion());
    return formalPackType;
  }

  /// Given that we just processed an input, advance to the next,
  /// if there is on.
  ///
  /// Postcondition: either isFinished() or all of the invariants
  /// for the mutable state have been established.
  void advance() {
    assert(!isFinished());
    assert(substEltIndex < origElt.getSubstTypes().size());
    substEltIndex++;

    readyNextSubstElement();
  }

  void finish() {
    origElt.finish();
  }

  /// Project out the current pack component.  Do not call this multiple
  /// times for the same component.
  ///
  /// You should only call this when the pack is already initialized,
  /// which generally means when it corresponds to an input you've
  /// received.  In the reabstraction code, this means it should only
  /// be used for *outer* types.  If the pack is part of an output
  /// you're generating, you should call createPackComponentTemporary.
  ManagedValue projectPackComponent(SILGenFunction &SGF, SILLocation loc);

  /// Create a temporary for the current pack component and set it in the
  /// pack.  Do not call this multiple times for the same component.
  ///
  /// You should only call this when the pack not yet initialized,
  /// which generally means when it corresponds to an output you're
  /// generating.  In the reabstraction code, this means it should only
  /// be used for *inner* types.  If the pack is an input you've
  /// received, you should call projectPackComponent.
  SILValue createPackComponentTemporary(SILGenFunction &SGF, SILLocation loc);
};

/// A generator for visiting the addresses of the elements of
/// a tuple.  Unlike the other tuple generators, this does not
/// require the original abstraction pattern to be a tuple pattern:
/// like forEachExpandedTupleElement, it permits an opaque
/// abstraction pattern.
class TupleElementAddressGenerator {
  struct OpaquePatternStorage {
    AbstractionPattern origType;
    CanTupleType substType;
  };
  using Members = ExternalUnionMembers<TupleElementGenerator,
                                       OpaquePatternStorage>;
  static Members::Index getIndexForKind(bool isOpaque) {
    return isOpaque ? Members::indexOf<OpaquePatternStorage>()
                    : Members::indexOf<TupleElementGenerator>();
  }
  ExternalUnion<bool, Members, getIndexForKind> origElt;

  /// The address of the tuple value.
  ManagedValue tupleAddr;

  /// If the substituted tuple type contains pack expansions, this is
  /// the induced pack type for the element sequence.
  CanPackType inducedPackType;

  /// The current index within origElt.getSubstEltTypes().
  unsigned substEltIndex;

  /// Whether the orig type is opaque, and therefore whether origElt
  /// is storing an OpaquePatternStorage or a TupleElementGenerator.
  bool isOrigTypeOpaque;

  /// Precondition: we aren't finished visiting orig elements,
  /// and we've already readied the current orig element.
  ///
  /// Ready the next subst element (the one at substEltIndex)
  /// from the current orig element.  If we've exhausted the supply
  /// of subst elements from this orig element, advance to the
  /// next orig element and repeat.
  ///
  /// Postcondition: we're either finished or properly configured
  /// with a subst element that hasn't been presented before.
  void readyNextSubstElement() {
    auto &gen = origElt.get<TupleElementGenerator>(isOrigTypeOpaque);
    while (true) {
      assert(!gen.isFinished());
      assert(substEltIndex <= gen.getSubstTypes().size());

      // If we haven't reached the limit of the current element yet,
      // continue.
      if (substEltIndex != gen.getSubstTypes().size())
        return;

      // Otherwise, advance, and ready the next orig element if we
      // didn't finish.
      gen.advance();
      if (gen.isFinished()) return;
      substEltIndex = 0;
    }
  }

public:
  TupleElementAddressGenerator(const ASTContext &ctx,
                               ManagedValue tupleAddr,
                               AbstractionPattern origType,
                               CanTupleType substType)
      : tupleAddr(tupleAddr) {

    if (substType->containsPackExpansionType()) {
      inducedPackType = CanPackType::get(ctx, substType.getElementTypes());
    }

    isOrigTypeOpaque = !origType.isTuple();
    substEltIndex = 0;
    if (isOrigTypeOpaque) {
      assert(origType.isTypeParameterOrOpaqueArchetype());
      origElt.emplaceAggregate<OpaquePatternStorage>(isOrigTypeOpaque,
                                                     origType, substType);
    } else {
      auto &gen =
        origElt.emplace<TupleElementGenerator>(isOrigTypeOpaque,
                                               origType, substType);
      if (!gen.isFinished()) {
        readyNextSubstElement();
      }
    }
  }

  /// Is this generator finished?  If so, the getters below may not be used.
  bool isFinished() const {
    if (isOrigTypeOpaque) {
      auto &storage = origElt.get<OpaquePatternStorage>(isOrigTypeOpaque);
      return substEltIndex == storage.substType->getNumElements();
    } else {
      return origElt.get<TupleElementGenerator>(isOrigTypeOpaque).isFinished();
    }
  }

  AbstractionPattern getOrigType() const {
    if (isOrigTypeOpaque) {
      return origElt.get<OpaquePatternStorage>(isOrigTypeOpaque).origType;
    } else {
      auto &gen = origElt.get<TupleElementGenerator>(isOrigTypeOpaque);
      return gen.isOrigPackExpansion()
               ? gen.getOrigType().getPackExpansionPatternType()
               : gen.getOrigType();
    }
  }

  CanType getSubstType() const {
    if (isOrigTypeOpaque) {
      auto &opaque = origElt.get<OpaquePatternStorage>(isOrigTypeOpaque);
      return opaque.substType.getElementType(substEltIndex);
    } else {
      auto &gen = origElt.get<TupleElementGenerator>(isOrigTypeOpaque);
      return gen.getSubstTypes()[substEltIndex];
    }
  }

  bool isSubstPackExpansion() const {
    return isa<PackExpansionType>(getSubstType());
  }

  unsigned getSubstElementIndex() const {
    if (isOrigTypeOpaque) {
      return substEltIndex;
    } else {
      auto &gen = origElt.get<TupleElementGenerator>(isOrigTypeOpaque);
      return gen.getSubstIndex() + substEltIndex;
    }
  }

  bool tupleContainsPackExpansion() const {
    return (bool) inducedPackType;
  }

  CanPackType getInducedPackType() const {
    assert(tupleContainsPackExpansion());
    return inducedPackType;
  }

  /// Given that we just processed an input, advance to the next,
  /// if there is on.
  ///
  /// Postcondition: either isFinished() or all of the invariants
  /// for the mutable state have been established.
  void advance() {
    assert(!isFinished());
    substEltIndex++;
    if (!isOrigTypeOpaque) {
      readyNextSubstElement();
    }
  }

  void finish() {
    if (isOrigTypeOpaque) {
#ifndef NDEBUG
      auto &opaque = origElt.get<OpaquePatternStorage>(isOrigTypeOpaque);
      assert(substEltIndex == opaque.substType->getNumElements());
#endif
    } else {
      auto &gen = origElt.get<TupleElementGenerator>(isOrigTypeOpaque);
      gen.finish();
    }
  }

  /// Project out the current tuple element.  Call this exactly once
  /// per element.
  ManagedValue projectElementAddress(SILGenFunction &SGF, SILLocation loc);
};

} // end namespace Lowering
} // end namespace swift

#endif
