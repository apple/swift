//===--- Scope.cpp --------------------------------------------------------===//
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

#include "Scope.h"
#include "swift/Basic/Range.h"

using namespace swift;
using namespace Lowering;

ManagedValue Scope::popPreservingValue(ManagedValue mv) {
  // If we have a value, make sure that it is an object. The reason why is
  // that we want to make sure that we are not forwarding a cleanup for a
  // stack location that will be destroyed by this scope.
  assert(mv && mv.getType().isObject() &&
         (mv.getType().isTrivial(cleanups.SGF.F) ||
          mv.getOwnershipKind() == OwnershipKind::None || mv.hasCleanup()));
  CleanupCloner cloner(cleanups.SGF, mv);
  SILValue value = mv.forward(cleanups.SGF);
  pop();
  return cloner.clone(value);
}

// Since we have an RValue, we know that RValue invariants imply that all
// subvalues that are addresses must be address only types. Such address only
// types if they are +1 rvalues should be independent of any values from outside
// the +1 rvalue emission (that is if someone else has a reference to the
// address only type, we should have produced a copy). This means that it is
// safe to move the value into new memory that is guaranteed to live through the
// scope being pushed. As an additional complication due to SIL enforcing stack
// ordering, we can not use a temporary stack location since any stack locations
// that are inside the scope will be cleaned up while such a scope jumping stack
// is still alive (violating stack ordering). Instead we use an alloc_box to
// store the new value. allocbox-to-stack will then reorder/expand the stack
// lifetimes to resolve the issues.
static void lifetimeExtendAddressOnlyRValueSubValues(
    SILGenFunction &SGF, SILLocation loc,
    llvm::SmallVectorImpl<SILValue> &values,
    llvm::SmallVectorImpl<SILValue> &lifetimeExtendingBoxes) {
  for (SILValue &v : values) {
    // If v is not an address, it isn't interesting, continue.
    if (!v->getType().isAddress()) {
      continue;
    }

    // Otherwise, create the box and move the address only value into the box.
    assert(v->getType().isAddressOnly(SGF.F) &&
           "RValue invariants imply that all RValue subtypes that are "
           "addresses must be address only.");
    auto boxTy = SILBoxType::get(v->getType().getASTType());
    SILValue box = SGF.B.createAllocBox(loc, boxTy);
    // TODO: Should these boxes that extend lifetimes for rvalue subobjects ever
    //       be lexical?
    if (SGF.getASTContext().SILOpts.supportsLexicalLifetimes(SGF.getModule())) {
      if (v->getType().getLifetime(SGF.F).isLexical()) {
        box = SGF.B.createBeginBorrow(loc, box,
                                      /*isLexical=*/true);
      }
    }
    SILValue addr = SGF.B.createProjectBox(loc, box, 0);
    SGF.B.createCopyAddr(loc, v, addr, IsTake, IsInitialization);

    // Then save the box so we create the box destroy in the caller and
    // overwrite v with the project box since that is where the value is now.
    lifetimeExtendingBoxes.emplace_back(box);
    v = addr;
  }
}

RValue Scope::popPreservingValue(RValue &&rv) {
  auto &SGF = cleanups.SGF;
  assert(rv.isPlusOneOrTrivial(SGF) &&
         "Can only push plus one rvalues through a scope");

  // Perform a quick check if we have an incontext value. If so, just pop and
  // return rv.
  if (rv.isInContext()) {
    pop();
    return std::move(rv);
  }

  // After this point, we should have /no/ special states.
  assert(!rv.isInSpecialState());

  // Ok, we have a normal RValue. Gather all of the data that we need to
  // recreate the RValue in the outer scope.
  CanType type = rv.type;
  unsigned numEltsRemaining = rv.elementsToBeAdded;
  SmallVector<CleanupCloner, 4> cloners;
  CleanupCloner::getClonersForRValue(SGF, rv, cloners);

  SmallVector<SILValue, 4> values;
  std::move(rv).forwardAll(SGF, values);

  // Lifetime any address only values that we may have.
  SmallVector<SILValue, 4> lifetimeExtendingBoxes;
  lifetimeExtendAddressOnlyRValueSubValues(SGF, loc, values,
                                           lifetimeExtendingBoxes);

  // Then pop the cleanups.
  pop();

  // Then create cleanups for any lifetime extending boxes that we may have to
  // ensure that the boxes are cleaned up /after/ the value stored in the
  // box. We assume that our values will be destroyed via a destroy_addr or the
  // like /before/ the end of our box's lifetime, implying that the value inside
  // the box should be uninitialized when the box is destroyed, so it is
  // important that we use a dealloc_box.
  for (auto v : lifetimeExtendingBoxes) {
    SGF.enterDeallocBoxCleanup(v);
  }

  // Reconstruct the managed values from the underlying sil values in the outer
  // scope. Since the RValue wants a std::vector value, we use that instead.
  std::vector<ManagedValue> managedValues;
  for (unsigned i : indices(values)) {
    managedValues.push_back(cloners[i].clone(values[i]));
  }

  // And then assemble the managed values into a rvalue.
  return RValue(SGF, std::move(managedValues), type, numEltsRemaining);
}

void Scope::popImpl() {
  verify();
  cleanups.innermostScope = savedInnermostScope;
  cleanups.endScope(depth, loc);
  if (cleanups.innermostScope)
    cleanups.stack.checkIterator(cleanups.innermostScope->depth);
  cleanups.popTopDeadCleanups();
}

void Scope::verify() {
  assert(cleanups.innermostScope == this && "popping scopes out of order");
  assert(depth.isValid());
  cleanups.stack.checkIterator(depth);
}
