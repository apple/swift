//===--- SILBasicBlock.cpp - Basic blocks for high-level SIL code ---------===//
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
// This file defines the high-level BasicBlocks used for Swift SIL code.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/STLExtras.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILUndef.h"
#include "swift/Strings.h"

using namespace swift;

//===----------------------------------------------------------------------===//
// SILBasicBlock Implementation
//===----------------------------------------------------------------------===//

SwiftMetatype SILBasicBlock::registeredMetatype;    

SILBasicBlock::SILBasicBlock() :
  SwiftObjectHeader(registeredMetatype), Parent(nullptr) {}

SILBasicBlock::SILBasicBlock(SILFunction *parent) :
  SwiftObjectHeader(registeredMetatype), Parent(parent) {}

SILBasicBlock::~SILBasicBlock() {
  if (!getParent()) {
    assert(ArgumentList.empty() &&
           "a static initializer block must not have arguments");
    assert(InstList.empty() &&
           "a static initializer block must be cleared before the destructor");
    return;
  }
    
  dropAllReferences();
  eraseAllInstructions(getModule());
}

int SILBasicBlock::getDebugID() const {
  if (!getParent())
    return -1;
  int idx = 0;
  for (const SILBasicBlock &B : *getParent()) {
    if (&B == this)
      return idx;
    ++idx;
  }
  llvm_unreachable("block not in function's block list");
}

void SILBasicBlock::setDebugName(llvm::StringRef name) {
  getModule().setBasicBlockName(this, name);
}

Optional<llvm::StringRef> SILBasicBlock::getDebugName() const {
  return getModule().getBasicBlockName(this);
}

SILModule &SILBasicBlock::getModule() const {
  return getParent()->getModule();
}

void SILBasicBlock::insert(iterator InsertPt, SILInstruction *I) {
  InstList.insert(InsertPt, I);
}

void SILBasicBlock::push_back(SILInstruction *I) {
  InstList.push_back(I);
}

void SILBasicBlock::push_front(SILInstruction *I) {
  InstList.push_front(I);
}

void SILBasicBlock::eraseAllInstructions(SILModule &module) {
  while (!empty()) {
    erase(&*begin(), module);
  }
}

/// Returns the iterator following the erased instruction.
void SILBasicBlock::erase(SILInstruction *I) {
  erase(I, getModule());
}

void SILBasicBlock::erase(SILInstruction *I, SILModule &module) {
  assert(!I->isDeleted() && "double delete of instruction");
  module.willDeleteInstruction(I);
  InstList.remove(I);
  I->asSILNode()->markAsDeleted();
  module.scheduleForDeletion(I);
}

void SILBasicBlock::moveInstruction(SILInstruction *inst,
                                    SILInstruction *beforeInst) {
  if (inst == beforeInst)
    return;
  inst->getParent()->InstList.remove(inst);
  beforeInst->getParent()->insert(beforeInst->getIterator(), inst);
}

void SILBasicBlock::moveInstructionToFront(SILInstruction *inst) {
  inst->getParent()->InstList.remove(inst);
  push_front(inst);
}

/// This method unlinks 'self' from the containing SILFunction and deletes it.
void SILBasicBlock::eraseFromParent() {
  getParent()->eraseBlock(this);
}

void SILBasicBlock::cloneArgumentList(SILBasicBlock *Other) {
  assert(Other->isEntry() == isEntry() &&
         "Expected to both blocks to be entries or not");
  if (isEntry()) {
    assert(args_empty() && "Expected to have no arguments");
    for (auto *FuncArg : Other->getSILFunctionArguments()) {
      auto *NewArg =
          createFunctionArgument(FuncArg->getType(), FuncArg->getDecl());
      NewArg->copyFlags(FuncArg);
    }
    return;
  }

  for (auto *PHIArg : Other->getSILPhiArguments()) {
    createPhiArgument(PHIArg->getType(), PHIArg->getOwnershipKind(),
                      PHIArg->getDecl());
  }
}

void SILBasicBlock::moveArgumentList(SILBasicBlock *from) {
  ArgumentList = std::move(from->ArgumentList);
  for (SILArgument *arg : getArguments()) {
    arg->parentBlock = this;
  }
}

SILFunctionArgument *
SILBasicBlock::createFunctionArgument(SILType Ty, const ValueDecl *D,
                                      bool disableEntryBlockVerification) {
  assert((disableEntryBlockVerification || isEntry()) &&
         "Function Arguments can only be in the entry block");
  const SILFunction *Parent = getParent();
  auto OwnershipKind = ValueOwnershipKind(
      *Parent, Ty,
      Parent->getConventions().getSILArgumentConvention(getNumArguments()));
  return new (getModule()) SILFunctionArgument(this, Ty, OwnershipKind, D);
}

SILFunctionArgument *SILBasicBlock::insertFunctionArgument(unsigned AtArgPos,
                                                           SILType Ty,
                                                           ValueOwnershipKind OwnershipKind,
                                                           const ValueDecl *D) {
  assert(isEntry() && "Function Arguments can only be in the entry block");
  auto *arg = new (getModule()) SILFunctionArgument(Ty, OwnershipKind, D);
  arg->parentBlock = this;
  insertArgument(ArgumentList.begin() + AtArgPos, arg);
  return arg;
}

SILFunctionArgument *SILBasicBlock::replaceFunctionArgument(
    unsigned i, SILType Ty, ValueOwnershipKind Kind, const ValueDecl *D) {
  assert(isEntry() && "Function Arguments can only be in the entry block");

  SILFunction *F = getParent();
  SILModule &M = F->getModule();
  if (Ty.isTrivial(*F))
    Kind = OwnershipKind::None;

  assert(ArgumentList[i]->use_empty() && "Expected no uses of the old arg!");

  SILFunctionArgument *NewArg = new (M) SILFunctionArgument(Ty, Kind, D);
  NewArg->setParent(this);
  ArgumentList[i]->parentBlock = nullptr;

  // TODO: When we switch to malloc/free allocation we'll be leaking memory
  // here.
  *(ArgumentList.begin() + i) = NewArg;

  return NewArg;
}

/// Replace the ith BB argument with a new one with type Ty (and optional
/// ValueDecl D).
SILPhiArgument *SILBasicBlock::replacePhiArgument(unsigned i, SILType Ty,
                                                  ValueOwnershipKind Kind,
                                                  const ValueDecl *D,
                                                  bool isReborrow,
                                                  bool isEscaping) {
  assert(!isEntry() && "PHI Arguments can not be in the entry block");
  SILFunction *F = getParent();
  SILModule &M = F->getModule();
  if (Ty.isTrivial(*F))
    Kind = OwnershipKind::None;

  assert(ArgumentList[i]->use_empty() && "Expected no uses of the old BB arg!");

  SILPhiArgument *NewArg =
      new (M) SILPhiArgument(Ty, Kind, D, isReborrow, isEscaping);
  NewArg->setParent(this);
  ArgumentList[i]->parentBlock = nullptr;

  // TODO: When we switch to malloc/free allocation we'll be leaking memory
  // here.
  *(ArgumentList.begin() + i) = NewArg;

  return NewArg;
}

SILPhiArgument *SILBasicBlock::replacePhiArgumentAndReplaceAllUses(
    unsigned i, SILType ty, ValueOwnershipKind kind, const ValueDecl *d,
    bool isReborrow, bool isEscaping) {
  // Put in an undef placeholder before we do the replacement since
  // replacePhiArgument() expects the replaced argument to not have
  // any uses.
  SmallVector<Operand *, 16> operands;
  SILValue undef = SILUndef::get(ty, *getParent());
  SILArgument *arg = getArgument(i);
  while (!arg->use_empty()) {
    Operand *use = *arg->use_begin();
    use->set(undef);
    operands.push_back(use);
  }

  // Perform the replacement.
  auto *newArg = replacePhiArgument(i, ty, kind, d, isReborrow, isEscaping);

  // Wire back up the uses.
  while (!operands.empty()) {
    operands.pop_back_val()->set(newArg);
  }

  return newArg;
}

SILPhiArgument *SILBasicBlock::createPhiArgument(SILType Ty,
                                                 ValueOwnershipKind Kind,
                                                 const ValueDecl *D,
                                                 bool isReborrow,
                                                 bool isEscaping) {
  assert(!isEntry() && "PHI Arguments can not be in the entry block");
  if (Ty.isTrivial(*getParent()))
    Kind = OwnershipKind::None;
  return new (getModule())
      SILPhiArgument(this, Ty, Kind, D, isReborrow, isEscaping);
}

SILPhiArgument *SILBasicBlock::insertPhiArgument(unsigned AtArgPos, SILType Ty,
                                                 ValueOwnershipKind Kind,
                                                 const ValueDecl *D,
                                                 bool isReborrow,
                                                 bool isEscaping) {
  assert(!isEntry() && "PHI Arguments can not be in the entry block");
  if (Ty.isTrivial(*getParent()))
    Kind = OwnershipKind::None;
  auto *arg =
      new (getModule()) SILPhiArgument(Ty, Kind, D, isReborrow, isEscaping);
  arg->parentBlock = this;
  insertArgument(ArgumentList.begin() + AtArgPos, arg);
  return arg;
}

void SILBasicBlock::dropAllArguments() {
  for (SILArgument *arg : ArgumentList) {
    arg->parentBlock = nullptr;
  }
  ArgumentList.clear();
}

void SILBasicBlock::eraseArgument(int Index) {
  assert(getArgument(Index)->use_empty() &&
         "Erasing block argument that has uses!");
  ArgumentList[Index]->parentBlock = nullptr;
  ArgumentList.erase(ArgumentList.begin() + Index);
}

/// Splits a basic block into two at the specified instruction.
///
/// Note that all the instructions BEFORE the specified iterator
/// stay as part of the original basic block. The old basic block is left
/// without a terminator.
SILBasicBlock *SILBasicBlock::split(iterator I) {
  SILBasicBlock *New = Parent->createBasicBlockAfter(this);
  // Move all of the specified instructions from the original basic block into
  // the new basic block.
  New->InstList.splice(New->end(), InstList, I, end());
  return New;
}

void SILBasicBlock::moveTo(SILBasicBlock::iterator To, SILInstruction *I) {
  assert(I->getParent() != this && "Must move from different basic block");
  InstList.splice(To, I->getParent()->InstList, I);
  ScopeCloner ScopeCloner(*Parent);
  I->setDebugScope(ScopeCloner.getOrCreateClonedScope(I->getDebugScope()));
}

void
llvm::ilist_traits<swift::SILBasicBlock>::
transferNodesFromList(llvm::ilist_traits<SILBasicBlock> &SrcTraits,
                      block_iterator First, block_iterator Last) {
  assert(&Parent->getModule() == &SrcTraits.Parent->getModule() &&
         "Module mismatch!");

  // If we are asked to splice into the same function, don't update parent
  // pointers.
  if (Parent == SrcTraits.Parent)
    return;

  ScopeCloner ScopeCloner(*Parent);

  // If splicing blocks not in the same function, update the parent pointers.
  for (; First != Last; ++First) {
    First->Parent = Parent;
    First->index = -1;
    First->lastInitializedBitfieldID = 0;
    for (auto &II : *First) {
      for (SILValue result : II.getResults()) {
        result->resetBitfields();
      }
      II.asSILNode()->resetBitfields();
    
      II.setDebugScope(ScopeCloner.getOrCreateClonedScope(II.getDebugScope()));
      // Special handling for SILDebugVariable.
      if (auto DVI = DebugVarCarryingInst(&II))
        if (auto VarInfo = DVI.getVarInfo())
          if (VarInfo->Scope)
            DVI.setDebugVarScope(
                ScopeCloner.getOrCreateClonedScope(VarInfo->Scope));
    }
    for (SILArgument *arg : First->getArguments()) {
      arg->resetBitfields();
    }
  }
}

/// ScopeCloner expects NewFn to be a clone of the original
/// function, with all debug scopes and locations still pointing to
/// the original function.
ScopeCloner::ScopeCloner(SILFunction &NewFn) : NewFn(NewFn) {
  // Some clients of SILCloner copy over the original function's
  // debug scope. Create a new one here.
  // FIXME: Audit all call sites and make them create the function
  // debug scope.
  auto *SILFn = NewFn.getDebugScope()->Parent.get<SILFunction *>();
  if (SILFn != &NewFn) {
    SILFn->setInlined();
    NewFn.setDebugScope(getOrCreateClonedScope(NewFn.getDebugScope()));
  }
}

const SILDebugScope *
ScopeCloner::getOrCreateClonedScope(const SILDebugScope *OrigScope) {
  if (!OrigScope)
    return nullptr;

  auto it = ClonedScopeCache.find(OrigScope);
  if (it != ClonedScopeCache.end())
    return it->second;

  auto ClonedScope = new (NewFn.getModule()) SILDebugScope(*OrigScope);
  if (OrigScope->InlinedCallSite) {
    // For inlined functions, we need to rewrite the inlined call site.
    ClonedScope->InlinedCallSite =
        getOrCreateClonedScope(OrigScope->InlinedCallSite);
  } else {
    if (auto *ParentScope = OrigScope->Parent.dyn_cast<const SILDebugScope *>())
      ClonedScope->Parent = getOrCreateClonedScope(ParentScope);
    else
      ClonedScope->Parent = &NewFn;
  }
  // Create an inline scope for the cloned instruction.
  assert(ClonedScopeCache.find(OrigScope) == ClonedScopeCache.end());
  ClonedScopeCache.insert({OrigScope, ClonedScope});
  return ClonedScope;
}

/// Declared out of line so we can have a declaration of SILArgument.
#define ARGUMENT(NAME, PARENT)                                                 \
  NAME##ArrayRef SILBasicBlock::get##NAME##s() const {                         \
    return NAME##ArrayRef(getArguments(),                                      \
                          [](SILArgument *arg) { return cast<NAME>(arg); });   \
  }
#include "swift/SIL/SILNodes.def"

/// Returns true if this block ends in an unreachable or an apply of a
/// no-return apply or builtin.
bool SILBasicBlock::isNoReturn() const {
  if (isa<UnreachableInst>(getTerminator()))
    return true;

  auto Iter = prev_or_begin(getTerminator()->getIterator(), begin());
  FullApplySite FAS = FullApplySite::isa(const_cast<SILInstruction *>(&*Iter));
  if (FAS && FAS.isCalleeNoReturn()) {
    return true;
  }

  if (auto *BI = dyn_cast<BuiltinInst>(&*Iter)) {
    return BI->getModule().isNoReturnBuiltinOrIntrinsic(BI->getName());
  }

  return false;
}

bool SILBasicBlock::isTrampoline() const {
  auto *Branch = dyn_cast<BranchInst>(getTerminator());
  if (!Branch)
    return false;
  return begin() == Branch->getIterator();
}

bool SILBasicBlock::isLegalToHoistInto() const {
  return true;
}

bool SILBasicBlock::hasPhi() const {
  if (getArguments().size() == 0)
    return false;
  // It is sufficient to check whether the first argument is a phi.  A block
  // can't have both phis and terminator results.
  auto *argument = getArguments()[0];
  return argument->isPhi();
}

const SILDebugScope *SILBasicBlock::getScopeOfFirstNonMetaInstruction() {
  for (auto &Inst : *this)
    if (Inst.isMetaInstruction())
      return Inst.getDebugScope();
  return begin()->getDebugScope();
}
