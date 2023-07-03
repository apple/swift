//===--- Cleanup.cpp - Implements the Cleanup mechanics -------------------===//
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

#include "Cleanup.h"
#include "ManagedValue.h"
#include "RValue.h"
#include "Scope.h"
#include "SILGenBuilder.h"
#include "SILGenFunction.h"

using namespace swift;
using namespace Lowering;

//===----------------------------------------------------------------------===//
//                                CleanupState
//===----------------------------------------------------------------------===//

llvm::raw_ostream &Lowering::operator<<(llvm::raw_ostream &os,
                                        CleanupState state) {
  switch (state) {
  case CleanupState::Dormant:
    return os << "Dormant";
  case CleanupState::Dead:
    return os << "Dead";
  case CleanupState::Active:
    return os << "Active";
  case CleanupState::PersistentlyActive:
    return os << "PersistentlyActive";
  }

  llvm_unreachable("Unhandled CleanupState in switch.");
}

//===----------------------------------------------------------------------===//
//                               CleanupManager
//===----------------------------------------------------------------------===//

/// Are there any active cleanups in the given range?
static bool hasAnyActiveCleanups(DiverseStackImpl<Cleanup>::iterator begin,
                                 DiverseStackImpl<Cleanup>::iterator end) {
  for (; begin != end; ++begin)
    if (begin->isActive())
      return true;
  return false;
}

void CleanupManager::popTopDeadCleanups() {
  auto end = (innermostScope ? innermostScope->depth : stack.stable_end());
  assert(end.isValid());
  stack.checkIterator(end);

  while (stack.stable_begin() != end && stack.begin()->isDead()) {
    assert(!stack.empty());
    stack.pop();
    stack.checkIterator(end);
  }
}

using CleanupBuffer = DiverseValueBuffer<Cleanup>;

void CleanupManager::popAndEmitCleanup(CleanupHandle handle,
                                       CleanupLocation loc,
                                       ForUnwind_t forUnwind) {
  auto iter = stack.find(handle);
  Cleanup &stackCleanup = *iter;

  // Copy the cleanup off the cleanup stack.
  CleanupBuffer buffer(stackCleanup);
  Cleanup &cleanup = buffer.getCopy();

  // Deactivate it.
  forwardCleanup(handle);

  // Emit the cleanup.
  cleanup.emit(SGF, loc, forUnwind);
}

void CleanupManager::emitCleanups(CleanupsDepth depth, CleanupLocation loc,
                                  ForUnwind_t forUnwind, bool popCleanups) {
  auto cur = stack.stable_begin();
#ifndef NDEBUG
  auto topOfStack = cur;
#endif
  while (cur != depth) {
    // Advance the iterator.
    auto cleanupHandle = cur;
    auto iter = stack.find(cleanupHandle);
    Cleanup &stackCleanup = *iter;
    cur = stack.stabilize(++iter);

    // Copy the cleanup off the stack if it needs to be emitted.
    // This is necessary both because we might need to pop the cleanup and
    // because the cleanup might push other cleanups that will invalidate
    // references onto the stack.
    llvm::Optional<CleanupBuffer> copiedCleanup;
    if (stackCleanup.isActive() && SGF.B.hasValidInsertionPoint()) {
      copiedCleanup.emplace(stackCleanup);
    }

    // Pop now if that was requested.
    if (popCleanups) {
#ifndef NDEBUG
      // This is an implicit deactivation.
      if (stackCleanup.isActive()) {
        SGF.FormalEvalContext.checkCleanupDeactivation(cleanupHandle);
      }
#endif

      stack.pop();

#ifndef NDEBUG
      topOfStack = stack.stable_begin();
#endif
    }

    // Emit the cleanup.
    if (copiedCleanup) {
      copiedCleanup->getCopy().emit(SGF, loc, forUnwind);
#ifndef NDEBUG
      if (hasAnyActiveCleanups(stack.stable_begin(), topOfStack)) {
        copiedCleanup->getCopy().dump(SGF);
        llvm_unreachable("cleanup left active cleanups on stack");
      }
#endif
    }

    stack.checkIterator(cur);
  }
}

/// Leave a scope, emitting all the cleanups that are currently active.
void CleanupManager::endScope(CleanupsDepth depth, CleanupLocation loc) {
  emitCleanups(depth, loc, NotForUnwind, /*popCleanups*/ true);
}

bool CleanupManager::hasAnyActiveCleanups(CleanupsDepth from,
                                          CleanupsDepth to) {
  return ::hasAnyActiveCleanups(stack.find(from), stack.find(to));
}

bool CleanupManager::hasAnyActiveCleanups(CleanupsDepth from) {
  return ::hasAnyActiveCleanups(stack.begin(), stack.find(from));
}

/// emitBranchAndCleanups - Emit a branch to the given jump destination,
/// threading out through any cleanups we might need to run.  This does not
/// pop the cleanup stack.
void CleanupManager::emitBranchAndCleanups(JumpDest dest, SILLocation branchLoc,
                                           ArrayRef<SILValue> args,
                                           ForUnwind_t forUnwind) {
  SILGenBuilder &builder = SGF.getBuilder();
  assert(builder.hasValidInsertionPoint() && "Emitting branch in invalid spot");
  emitCleanups(dest.getDepth(), dest.getCleanupLocation(),
               forUnwind, /*popCleanups=*/false);
  builder.createBranch(branchLoc, dest.getBlock(), args);
}

void CleanupManager::emitCleanupsForReturn(CleanupLocation loc,
                                           ForUnwind_t forUnwind) {
  SILGenBuilder &builder = SGF.getBuilder();
  assert(builder.hasValidInsertionPoint() && "Emitting return in invalid spot");
  (void)builder;
  emitCleanups(stack.stable_end(), loc, forUnwind, /*popCleanups=*/false);
}

/// Emit a new block that jumps to the specified location and runs necessary
/// cleanups based on its level. Emit a block even if there are no cleanups;
/// this is usually the destination of a conditional branch, so jumping
/// straight to `dest` creates a critical edge.
SILBasicBlock *CleanupManager::emitBlockForCleanups(JumpDest dest,
                                                    SILLocation branchLoc,
                                                    ArrayRef<SILValue> args,
                                                    ForUnwind_t forUnwind) {
  // Otherwise, create and emit a new block.
  auto *newBlock = SGF.createBasicBlock();
  SILGenSavedInsertionPoint IPRAII(SGF, newBlock);
  emitBranchAndCleanups(dest, branchLoc, args, forUnwind);
  return newBlock;
}


Cleanup &CleanupManager::initCleanup(Cleanup &cleanup,
                                     size_t allocSize,
                                     CleanupState state) {
  cleanup.allocatedSize = allocSize;
  cleanup.state = state;
  return cleanup;
}

#ifndef NDEBUG
static void checkCleanupDeactivation(SILGenFunction &SGF,
                                     CleanupsDepth handle,
                                     CleanupState state) {
  if (state != CleanupState::Dead) return;
  SGF.FormalEvalContext.checkCleanupDeactivation(handle);
}
#endif

void CleanupManager::setCleanupState(CleanupsDepth depth, CleanupState state) {
  auto iter = stack.find(depth);
  assert(iter != stack.end() && "can't change end of cleanups stack");
  setCleanupState(*iter, state);

#ifndef NDEBUG
  // This must be done after setting the state because setting the state can
  // itself finish a formal evaluation in some cases.
  checkCleanupDeactivation(SGF, depth, state);
#endif

  if (state == CleanupState::Dead && iter == stack.begin())
    popTopDeadCleanups();
}

std::tuple<Cleanup::Flags, llvm::Optional<SILValue>>
CleanupManager::getFlagsAndWritebackBuffer(CleanupHandle depth) {
  auto iter = stack.find(depth);
  assert(iter != stack.end() && "can't change end of cleanups stack");
  assert(iter->getState() != CleanupState::Dead &&
         "Trying to get writeback buffer of a dead cleanup?!");

  auto resultFlags = iter->getFlags();
  llvm::Optional<SILValue> result;
  bool foundValue = iter->getWritebackBuffer([&](SILValue v) { result = v; });
  (void)foundValue;
  assert(result.has_value() == foundValue);
  return std::make_tuple(resultFlags, result);
}

void CleanupManager::forwardCleanup(CleanupsDepth handle) {
  auto iter = stack.find(handle);
  assert(iter != stack.end() && "can't change end of cleanups stack");
  Cleanup &cleanup = *iter;
  assert(cleanup.isActive() && "forwarding inactive or dead cleanup?");

  CleanupState newState = (cleanup.getState() == CleanupState::Active
                             ? CleanupState::Dead
                             : CleanupState::Dormant);
  setCleanupState(cleanup, newState);

#ifndef NDEBUG
  // This must be done after setting the state because setting the state can
  // itself finish a formal evaluation in some cases.
  checkCleanupDeactivation(SGF, handle, newState);
#endif

  if (newState == CleanupState::Dead && iter == stack.begin())
    popTopDeadCleanups();
}

void CleanupManager::setCleanupState(Cleanup &cleanup, CleanupState state) {
  assert(SGF.B.hasValidInsertionPoint() &&
         "changing cleanup state at invalid IP");

  // Do the transition now to avoid doing it in N places below.
  CleanupState oldState = cleanup.getState();
  (void)oldState;
  cleanup.setState(SGF, state);

  assert(state != oldState && "trivial cleanup state change");
  assert(oldState != CleanupState::Dead && "changing state of dead cleanup");

  // Our current cleanup emission logic, where we don't try to re-use
  // cleanup emissions between various branches, doesn't require any
  // code to be emitted at transition points.
}

void CleanupManager::dump() const {
#ifndef NDEBUG
  auto begin = stack.stable_begin();
  auto end = stack.stable_end();
  while (begin != end) {
    auto iter = stack.find(begin);
    const Cleanup &stackCleanup = *iter;
    llvm::errs() << "CLEANUP DEPTH: " << begin.getDepth() << "\n";
    stackCleanup.dump(SGF);
    begin = stack.stabilize(++iter);
    stack.checkIterator(begin);
  }
#endif
}

void CleanupManager::dump(CleanupHandle handle) const {
  auto iter = stack.find(handle);
  const Cleanup &stackCleanup = *iter;
  llvm::errs() << "CLEANUP DEPTH: " << handle.getDepth() << "\n";
  stackCleanup.dump(SGF);
}

void CleanupManager::checkIterator(CleanupHandle handle) const {
#ifndef NDEBUG
  stack.checkIterator(handle);
#endif
}

//===----------------------------------------------------------------------===//
//                        CleanupStateRestorationScope
//===----------------------------------------------------------------------===//

void CleanupStateRestorationScope::pushCleanupState(CleanupHandle handle,
                                                    CleanupState newState) {
  // Don't put the cleanup in a state we can't restore it from.
  assert(newState != CleanupState::Dead && "cannot restore cleanup from death");

  auto iter = cleanups.stack.find(handle);
  assert(iter != cleanups.stack.end() && "can't change end of cleanups stack");
  Cleanup &cleanup = *iter;
  assert(cleanup.getState() != CleanupState::Dead &&
         "changing state of dead cleanup");

  CleanupState oldState = cleanup.getState();
  cleanup.setState(cleanups.SGF, newState);

  savedStates.push_back({handle, oldState});
}

void
CleanupStateRestorationScope::pushCurrentCleanupState(CleanupHandle handle) {
  auto iter = cleanups.stack.find(handle);
  assert(iter != cleanups.stack.end() && "can't change end of cleanups stack");
  Cleanup &cleanup = *iter;
  assert(cleanup.getState() != CleanupState::Dead &&
         "changing state of dead cleanup");

  CleanupState oldState = cleanup.getState();
  savedStates.push_back({handle, oldState});
}

void CleanupStateRestorationScope::popImpl() {
  // Restore cleanup states in the opposite order in which we saved them.
  for (auto i = savedStates.rbegin(), e = savedStates.rend(); i != e; ++i) {
    CleanupHandle handle = i->first;
    CleanupState stateToRestore = i->second;

    auto iter = cleanups.stack.find(handle);
    assert(iter != cleanups.stack.end() &&
           "can't change end of cleanups stack");
    Cleanup &cleanup = *iter;
    assert(cleanup.getState() != CleanupState::Dead &&
           "changing state of dead cleanup");
    cleanup.setState(cleanups.SGF, stateToRestore);
  }

  savedStates.clear();
}

void CleanupStateRestorationScope::pop() && { popImpl(); }

//===----------------------------------------------------------------------===//
//                               Cleanup Cloner
//===----------------------------------------------------------------------===//

CleanupCloner::CleanupCloner(SILGenFunction &SGF, const ManagedValue &mv)
    : SGF(SGF), writebackBuffer(llvm::None), hasCleanup(mv.hasCleanup()),
      isLValue(mv.isLValue()), isFormalAccess(false) {
  if (hasCleanup) {
    auto handle = mv.getCleanup();
    auto state = SGF.Cleanups.getFlagsAndWritebackBuffer(handle);
    using RawTy = std::underlying_type<Cleanup::Flags>::type;
    if (RawTy(std::get<0>(state)) & RawTy(Cleanup::Flags::FormalAccessCleanup))
      isFormalAccess = true;
    if (SILValue value = std::get<1>(state).value_or(SILValue()))
      writebackBuffer = value;
  }
}

CleanupCloner::CleanupCloner(SILGenBuilder &builder, const ManagedValue &mv)
    : CleanupCloner(builder.getSILGenFunction(), mv) {}

void CleanupCloner::getClonersForRValue(
    SILGenBuilder &builder, const RValue &rvalue,
    SmallVectorImpl<CleanupCloner> &resultingCloners) {
  return getClonersForRValue(builder.getSILGenFunction(), rvalue,
                             resultingCloners);
}

void CleanupCloner::getClonersForRValue(
    SILGenFunction &SGF, const RValue &rvalue,
    SmallVectorImpl<CleanupCloner> &resultingCloners) {
  transform(rvalue.values, std::back_inserter(resultingCloners),
            [&](ManagedValue mv) { return CleanupCloner(SGF, mv); });
}

ManagedValue CleanupCloner::clone(SILValue value) const {
  if (isLValue) {
    return ManagedValue::forLValue(value);
  }

  if (!hasCleanup) {
    return ManagedValue::forUnmanaged(value);
  }

  if (writebackBuffer.has_value()) {
    auto loc = RegularLocation::getAutoGeneratedLocation();
    auto cleanup =
        SGF.enterOwnedValueWritebackCleanup(loc, *writebackBuffer, value);
    return ManagedValue::forExclusivelyBorrowedOwnedObjectRValue(value,
                                                                 cleanup);
  }

  if (value->getType().isAddress()) {
    if (isFormalAccess) {
      auto loc = RegularLocation::getAutoGeneratedLocation();
      return SGF.emitFormalAccessManagedBufferWithCleanup(loc, value);
    }
    return SGF.emitManagedBufferWithCleanup(value);
  }

  if (isFormalAccess) {
    auto loc = RegularLocation::getAutoGeneratedLocation();
    return SGF.emitFormalAccessManagedRValueWithCleanup(loc, value);
  }
  return SGF.emitManagedRValueWithCleanup(value);
}

ManagedValue
CleanupCloner::cloneForTuplePackExpansionComponent(SILValue tupleAddr,
                                                   CanPackType inducedPackType,
                                                   unsigned componentIndex) const {
  if (isLValue) {
    return ManagedValue::forLValue(tupleAddr);
  }

  if (!hasCleanup) {
    return ManagedValue::forUnmanaged(tupleAddr);
  }

  assert(!writebackBuffer.has_value());
  auto expansionTy = tupleAddr->getType().getTupleElementType(componentIndex);
  if (expansionTy.getPackExpansionPatternType().isTrivial(SGF.F))
    return ManagedValue::forUnmanaged(tupleAddr);

  auto cleanup =
    SGF.enterPartialDestroyRemainingTupleCleanup(tupleAddr, inducedPackType,
                                                 componentIndex,
                                                 /*start at */ SILValue());
  return ManagedValue(tupleAddr, cleanup);
}

ManagedValue
CleanupCloner::cloneForPackPackExpansionComponent(SILValue packAddr,
                                                  CanPackType formalPackType,
                                                  unsigned componentIndex) const {
  if (isLValue) {
    return ManagedValue::forLValue(packAddr);
  }

  if (!hasCleanup) {
    return ManagedValue::forUnmanaged(packAddr);
  }

  assert(!writebackBuffer.has_value());
  auto expansionTy = packAddr->getType().getPackElementType(componentIndex);
  if (expansionTy.getPackExpansionPatternType().isTrivial(SGF.F))
    return ManagedValue::forUnmanaged(packAddr);

  auto cleanup =
    SGF.enterPartialDestroyRemainingPackCleanup(packAddr, formalPackType,
                                                componentIndex,
                                                /*start at */ SILValue());
  return ManagedValue(packAddr, cleanup);
}

ManagedValue
CleanupCloner::cloneForRemainingPackComponents(SILValue packAddr,
                                               CanPackType formalPackType,
                                               unsigned firstComponentIndex) const {
  if (isLValue) {
    return ManagedValue::forLValue(packAddr);
  }

  if (!hasCleanup) {
    return ManagedValue::forUnmanaged(packAddr);
  }

  assert(!writebackBuffer.has_value());
  bool isTrivial = true;
  auto packTy = packAddr->getType().castTo<SILPackType>();
  for (auto eltTy : packTy->getElementTypes().slice(firstComponentIndex)) {
    if (!SILType::getPrimitiveObjectType(eltTy).isTrivial(SGF.F)) {
      isTrivial = false;
      break;
    }
  }

  if (isTrivial)
    return ManagedValue::forUnmanaged(packAddr);

  auto cleanup =
    SGF.enterDestroyRemainingPackComponentsCleanup(packAddr, formalPackType,
                                                   firstComponentIndex);
  return ManagedValue(packAddr, cleanup);
}

ManagedValue
CleanupCloner::cloneForRemainingTupleComponents(SILValue tupleAddr,
                                                CanPackType inducedPackType,
                                                unsigned firstComponentIndex) const {
  if (isLValue) {
    return ManagedValue::forLValue(tupleAddr);
  }

  if (!hasCleanup) {
    return ManagedValue::forUnmanaged(tupleAddr);
  }

  assert(!writebackBuffer.has_value());
  bool isTrivial = true;
  auto tupleTy = tupleAddr->getType().castTo<TupleType>();
  for (auto eltTy : tupleTy.getElementTypes().slice(firstComponentIndex)) {
    if (!SILType::getPrimitiveObjectType(eltTy).isTrivial(SGF.F)) {
      isTrivial = false;
      break;
    }
  }

  if (isTrivial)
    return ManagedValue::forUnmanaged(tupleAddr);

  auto cleanup =
    SGF.enterDestroyRemainingTupleElementsCleanup(tupleAddr,
                                                  inducedPackType,
                                                  firstComponentIndex);
  return ManagedValue(tupleAddr, cleanup);
}
