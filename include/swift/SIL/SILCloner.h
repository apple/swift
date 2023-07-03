//===--- SILCloner.h - Defines the SILCloner class --------------*- C++ -*-===//
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
// This file defines the SILCloner class, used for cloning SIL instructions.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_SIL_SILCLONER_H
#define SWIFT_SIL_SILCLONER_H

#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/SIL/BasicBlockUtils.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/Dominance.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILDebugScope.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILVisitor.h"

namespace swift {

/// SILCloner - Abstract SIL visitor which knows how to clone instructions and
/// whose behavior can be customized by subclasses via the CRTP. This is meant
/// to be subclassed to implement inlining, function specialization, and other
/// operations requiring cloning (while possibly modifying, at the same time)
/// instruction sequences.
///
/// By default, this visitor will not do anything useful when called on a
/// basic block, or function; subclasses that want to handle those should
/// implement the appropriate visit functions and/or provide other entry points.
template<typename ImplClass>
class SILCloner : protected SILInstructionVisitor<ImplClass> {
  friend class SILVisitorBase<ImplClass>;
  friend class SILInstructionVisitor<ImplClass>;


protected:
  /// MARK: Context shared with CRTP extensions.

  SILBuilder Builder;
  DominanceInfo *DomTree = nullptr;
  TypeSubstitutionMap LocalArchetypeSubs;

  // The old-to-new value map.
  llvm::DenseMap<SILValue, SILValue> ValueMap;

  /// The old-to-new block map. Some entries may be premapped with original
  /// blocks.
  llvm::DenseMap<SILBasicBlock*, SILBasicBlock*> BBMap;

private:
  /// MARK: Private state hidden from CRTP extensions.

  // The original blocks in DFS preorder. All blocks in this list are mapped.
  // After cloning, this represents the entire cloned CFG.
  //
  // This could always be rediscovered by the client, but caching it is a
  // convenient way to iterate over the cloned region.
  SmallVector<SILBasicBlock *, 8> preorderBlocks;

  // Keep track of the last cloned block in function order. For single block
  // regions, this will be the start block.
  SILBasicBlock *lastClonedBB = nullptr;

public:
  using SILInstructionVisitor<ImplClass>::asImpl;

  explicit SILCloner(SILFunction &F, DominanceInfo *DT = nullptr)
      : Builder(F), DomTree(DT) {}

  explicit SILCloner(SILGlobalVariable *GlobVar) : Builder(GlobVar) {}

  void clearClonerState() {
    ValueMap.clear();
    BBMap.clear();
    preorderBlocks.clear();
  }

  /// Clients of SILCloner who want to know about any newly created
  /// instructions can install a SmallVector into the builder to collect them.
  void setTrackingList(SmallVectorImpl<SILInstruction*> *II) {
    getBuilder().setTrackingList(II);
  }
  
  SmallVectorImpl<SILInstruction*> *getTrackingList() {
    return getBuilder().getTrackingList();
  }

  SILBuilder &getBuilder() { return Builder; }

  // After cloning, returns a non-null pointer to the last cloned block in
  // function order. For single block regions, this will be the start block.
  SILBasicBlock *getLastClonedBB() { return lastClonedBB; }

  /// Visit all blocks reachable from the given `StartBB` and all instructions
  /// in those blocks.
  ///
  /// This is used to clone a region within a function and mutates the original
  /// function. `StartBB` cannot be the function entry block.
  ///
  /// The entire CFG is discovered in DFS preorder while cloning non-terminator
  /// instructions. `visitTerminator` is called in the same order, but only
  /// after mapping all blocks.
  void cloneReachableBlocks(SILBasicBlock *startBB,
                            ArrayRef<SILBasicBlock *> exitBlocks,
                            SILBasicBlock *insertAfterBB = nullptr,
                            bool havePrepopulatedFunctionArgs = false);

  /// Clone all blocks in this function and all instructions in those
  /// blocks.
  ///
  /// This is used to clone an entire function and should not mutate the
  /// original function except if \p replaceOriginalFunctionInPlace is true.
  ///
  /// entryArgs must have a SILValue from the cloned function corresponding to
  /// each argument in the original function `F`.
  ///
  /// Cloned instructions are inserted starting at the end of clonedEntryBB.
  void cloneFunctionBody(SILFunction *F, SILBasicBlock *clonedEntryBB,
                         ArrayRef<SILValue> entryArgs,
                         bool replaceOriginalFunctionInPlace = false);

  /// The same as clone function body, except the caller can provide a callback
  /// that allows for an entry arg to be assigned to a custom old argument. This
  /// is useful if one re-arranges parameters when converting from inout to out.
  void
  cloneFunctionBody(SILFunction *F, SILBasicBlock *clonedEntryBB,
                    ArrayRef<SILValue> entryArgs,
                    llvm::function_ref<SILValue(SILValue)> entryArgToOldArgMap);

  /// MARK: Callback utilities used from CRTP extensions during cloning.
  /// These should only be called from within an instruction cloning visitor.

  /// Visitor callback that registers a cloned instruction. All the original
  /// instruction's results are mapped onto the cloned instruction's results for
  /// use within the cloned region.
  ///
  /// CRTP extensions can
  /// override the implementation via `postProcess`.
  void recordClonedInstruction(SILInstruction *Orig, SILInstruction *Cloned) {
    asImpl().postProcess(Orig, Cloned);
    assert((!Orig->getDebugScope() || Cloned->getDebugScope() ||
            Builder.isInsertingIntoGlobal())
           && "cloned instruction dropped debug scope");
  }

  /// Visitor callback that maps an original value to an existing value when the
  /// original instruction will not be cloned. This is used when the instruction
  /// visitor can fold away the cloned instruction, and it skips the usual
  /// `postProcess()` callback. recordClonedInstruction() and
  /// recordFoldedValue() are the only two ways for a visitor to map an original
  /// value to another value for use within the cloned region.
  void recordFoldedValue(SILValue origValue, SILValue mappedValue) {
    asImpl().mapValue(origValue, mappedValue);
  }

  /// Register a re-mapping for local archetypes such as opened existentials.
  void registerLocalArchetypeRemapping(ArchetypeType *From,
                                       ArchetypeType *To) {
    auto result = LocalArchetypeSubs.insert(
        std::make_pair(CanArchetypeType(From), CanType(To)));
    assert(result.second);
    (void)result;
  }

  /// MARK: Public access to the cloned state, during and after cloning.

  /// After cloning, provides a list of all cloned blocks in DFS preorder.
  ArrayRef<SILBasicBlock *> originalPreorderBlocks() const {
    return preorderBlocks;
  }

  SILLocation getOpLocation(SILLocation Loc) {
    return asImpl().remapLocation(Loc);
  }

  const SILDebugScope *getOpScope(const SILDebugScope *DS) {
    return asImpl().remapScope(DS);
  }

  SubstitutionMap getOpSubstitutionMap(SubstitutionMap Subs) {
    // If we have local archetypes to substitute, check whether that's
    // relevant to this particular substitution.
    if (!LocalArchetypeSubs.empty()) {
      if (Subs.hasLocalArchetypes()) {
        // If we found a type containing a local archetype, substitute
        // open existentials throughout the substitution map.
        Subs = Subs.subst(QueryTypeSubstitutionMapOrIdentity{LocalArchetypeSubs},
                          MakeAbstractConformanceForGenericType());
      }
    }

    return asImpl().remapSubstitutionMap(Subs)
                   .getCanonical(/*canonicalizeSignature*/false);
  }

  SILType getTypeInClonedContext(SILType Ty) {
    auto objectTy = Ty.getASTType();
    // Do not substitute local archetypes, if we do not have any.
    if (!objectTy->hasLocalArchetype())
      return Ty;
    // Do not substitute local archetypes, if it is not required.
    // This is often the case when cloning basic blocks inside the same
    // function.
    if (LocalArchetypeSubs.empty())
      return Ty;

    // Substitute local archetypes, if we have any.
    return Ty.subst(
      Builder.getModule(),
      QueryTypeSubstitutionMapOrIdentity{LocalArchetypeSubs},
      MakeAbstractConformanceForGenericType(),
      CanGenericSignature());
  }
  SILType getOpType(SILType Ty) {
    Ty = getTypeInClonedContext(Ty);
    return asImpl().remapType(Ty);
  }

  CanType getASTTypeInClonedContext(Type ty) {
    // Do not substitute local archetypes, if we do not have any.
    if (!ty->hasLocalArchetype())
      return ty->getCanonicalType();
    // Do not substitute local archetypes, if it is not required.
    // This is often the case when cloning basic blocks inside the same
    // function.
    if (LocalArchetypeSubs.empty())
      return ty->getCanonicalType();

    return ty.subst(
      QueryTypeSubstitutionMapOrIdentity{LocalArchetypeSubs},
      MakeAbstractConformanceForGenericType()
    )->getCanonicalType();
  }

  CanType getOpASTType(CanType ty) {
    ty = getASTTypeInClonedContext(ty);
    return asImpl().remapASTType(ty);
  }

  /// Remap a structural index into a pack so that it will point to the
  /// corresponding structural index in the remapped pack type.
  unsigned getOpStructuralPackIndex(CanPackType origPackType,
                                    unsigned origIndex) {
    assert(origIndex < origPackType->getNumElements());
    unsigned newIndex = 0;
    for (unsigned i = 0; i != origIndex; ++i) {
      auto origComponentType = origPackType.getElementType(i);
      if (auto origExpansionType =
            dyn_cast<PackExpansionType>(origComponentType)) {
        auto newShapeClass = getOpASTType(origExpansionType.getCountType());
        if (auto newShapePack = dyn_cast<PackType>(newShapeClass))
          newIndex += newShapePack->getNumElements();
        else
          newIndex++;
      } else {
        newIndex++;
      }
    }
    return newIndex;
  }

  /// Does type substitution make the given tuple type no longer a tuple?
  bool doesOpTupleDisappear(CanTupleType type) {
    // Fast-path the empty tuple.
    if (type->getNumElements() == 0) return false;

    // Do a first pass over the tuple elements to check out the
    // non-expansions.  If there's more than one of them we definitely
    // stay a tuple and don't need to substitute any of the expansions.
    unsigned numScalarElements = type->getNumScalarElements();
    if (numScalarElements > 1)
      return false;

    // Okay, we need to substitute the count types for the expansions.
    for (auto index : indices(type->getElements())) {
      // Ignore non-expansions because we've already counted them.
      auto expansion = dyn_cast<PackExpansionType>(type.getElementType(index));
      if (!expansion) {
        // If we have a non-expansion with a label, we stay a tuple.
        if (type->getElement(index).hasName())
          return false;
        continue;
      }

      // Substitute the shape class of the expansion.
      auto newShapeClass = getOpASTType(expansion.getCountType());
      auto newShapePack = dyn_cast<PackType>(newShapeClass);

      // If the element has a name, then the tuple sticks around unless
      // the expansion disappears completely.
      if (type->getElement(index).hasName()) {
        if (newShapePack && newShapePack->getNumElements() == 0)
          continue;
        return false;
      }

      // Otherwise, walk the substituted shape components.
      for (auto newShapeElement : newShapePack.getElementTypes()) {
        // If there's an expansion in the shape, we'll have an expansion
        // in the tuple elements, which forces the tuple structure to remain.
        if (isa<PackExpansionType>(newShapeElement)) return false;

        // Otherwise, add another scalar element.
        if (++numScalarElements > 1) return false;
      }
    }

    // All of the packs expanded to scalars.  We should've short-circuited
    // if we ever saw a second or labeled scalar, so all we need to test
    // is whether we have exactly one scalar.
    assert(numScalarElements <= 1);
    return numScalarElements == 1;
  }

  void remapRootOpenedType(CanOpenedArchetypeType archetypeTy) {
    assert(archetypeTy->isRoot());

    auto sig = Builder.getFunction().getGenericSignature();
    auto origExistentialTy = archetypeTy->getExistentialType()
        ->getCanonicalType();
    auto substExistentialTy = getOpASTType(origExistentialTy);
    auto replacementTy = OpenedArchetypeType::get(substExistentialTy, sig);
    registerLocalArchetypeRemapping(archetypeTy, replacementTy);
  }

  // SILCloner will take care of debug scope on the instruction
  // and this helper will remap the auxiliary debug scope too, if there is any.
  void remapDebugVarInfo(DebugVarCarryingInst DbgVarInst) {
    if (!DbgVarInst)
      return;
    auto VarInfo = DbgVarInst.getVarInfo();
    if (VarInfo && VarInfo->Scope)
      DbgVarInst.setDebugVarScope(getOpScope(VarInfo->Scope));
  }

  ProtocolConformanceRef getOpConformance(Type ty,
                                          ProtocolConformanceRef conformance) {
    // If we have local archetypes to substitute, do so now.
    if (ty->hasLocalArchetype() && !LocalArchetypeSubs.empty()) {
      conformance =
        conformance.subst(ty,
                          QueryTypeSubstitutionMapOrIdentity{
                                                        LocalArchetypeSubs},
                          MakeAbstractConformanceForGenericType());
    }

    return asImpl().remapConformance(getASTTypeInClonedContext(ty),
                                     conformance);
  }

  ArrayRef<ProtocolConformanceRef>
  getOpConformances(Type ty,
                    ArrayRef<ProtocolConformanceRef> conformances) {
    SmallVector<ProtocolConformanceRef, 4> newConformances;
    for (auto conformance : conformances)
      newConformances.push_back(getOpConformance(ty, conformance));
    return ty->getASTContext().AllocateCopy(newConformances);
  }

  bool isValueCloned(SILValue OrigValue) const {
    return ValueMap.count(OrigValue);
  }

  /// Return the possibly new value representing the given value within the
  /// cloned region.
  ///
  /// Assumes that `isValueCloned` is true.
  SILValue getOpValue(SILValue Value) {
    return asImpl().getMappedValue(Value);
  }
  template <size_t N, typename ArrayRefType>
  SmallVector<SILValue, N> getOpValueArray(ArrayRefType Values) {
    SmallVector<SILValue, N> Ret(Values.size());
    for (unsigned i = 0, e = Values.size(); i != e; ++i)
      Ret[i] = asImpl().getMappedValue(Values[i]);
    return Ret;
  }

  SILFunction *getOpFunction(SILFunction *Func) {
    return asImpl().remapFunction(Func);
  }

  bool isBlockCloned(SILBasicBlock *OrigBB) const {
    auto bbIter = BBMap.find(OrigBB);
    if (bbIter == BBMap.end())
      return false;

    // Exit blocks are mapped to themselves during region cloning.
    return bbIter->second != OrigBB;
  }

  /// Return the new block within the cloned region analagous to the given
  /// original block.
  ///
  /// Assumes that `isBlockCloned` is true.
  SILBasicBlock *getOpBasicBlock(SILBasicBlock *BB) {
    return asImpl().remapBasicBlock(BB);
  }

protected:
  /// MARK: CRTP visitors and other CRTP overrides.

#define INST(CLASS, PARENT) void visit##CLASS(CLASS *I);
#include "swift/SIL/SILNodes.def"

  // Visit the instructions in a single basic block, not including the block
  // terminator.
  void visitInstructionsInBlock(SILBasicBlock *BB);

  // Visit a block's terminator. This is called with each block in DFS preorder
  // after visiting and mapping all basic blocks and after visiting all
  // non-terminator instructions in the block.
  void visitTerminator(SILBasicBlock *BB) {
    asImpl().visit(BB->getTerminator());
  }

  // CFG cloning requires cloneFunction() or cloneReachableBlocks().
  void visitSILBasicBlock(SILFunction *F) = delete;

  // Function cloning requires cloneFunction().
  void visitSILFunction(SILFunction *F) = delete;

  // MARK: SILCloner subclasses use the CRTP to customize the following callback
  // implementations. Remap functions are called before cloning to modify
  // constructor arguments. The postProcess function is called afterwards on
  // the result.

  SILLocation remapLocation(SILLocation Loc) { return Loc; }
  const SILDebugScope *remapScope(const SILDebugScope *DS) { return DS; }
  SILType remapType(SILType Ty) {
      return Ty;
  }

  CanType remapASTType(CanType Ty) {
    return Ty;
  }

  ProtocolConformanceRef remapConformance(Type Ty, ProtocolConformanceRef C) {
    return C;
  }
  /// Get the value that takes the place of the given `Value` within the cloned
  /// region. The given value must already have been mapped by this cloner.
  SILValue getMappedValue(SILValue Value);
  void mapValue(SILValue origValue, SILValue mappedValue);

  SILFunction *remapFunction(SILFunction *Func) { return Func; }
  SILBasicBlock *remapBasicBlock(SILBasicBlock *BB);
  void postProcess(SILInstruction *Orig, SILInstruction *Cloned);

  SubstitutionMap remapSubstitutionMap(SubstitutionMap Subs) { return Subs; }

  /// This is called by either of the top-level visitors, cloneReachableBlocks
  /// or cloneSILFunction, after all other visitors are have been called.

  /// `preFixUp` is called first.
  void preFixUp(SILFunction *F) {}
  /// After postFixUp, the SIL must be valid and semantically equivalent to the
  /// SIL before cloning.
  ///
  /// Common fix-ups are handled first in `commonFixUp` and may not be
  /// overridden.
  void postFixUp(SILFunction *F) {}

private:
  /// MARK: SILCloner implementation details hidden from CRTP extensions.

  void clonePhiArgs(SILBasicBlock *oldBB);

  void visitBlocksDepthFirst(SILBasicBlock *StartBB);

  /// Also perform fundamental cleanup first, then call the CRTP extension,
  /// `postFixUp`.
  void commonFixUp(SILFunction *F);
};

/// A SILBuilder that automatically invokes postprocess on each
/// inserted instruction.
template<class SomeSILCloner, unsigned N = 4>
class SILBuilderWithPostProcess : public SILBuilder {
  SomeSILCloner &SC;
  SILInstruction *Orig;
  SmallVector<SILInstruction*, N> InsertedInstrs;

public:
  SILBuilderWithPostProcess(SomeSILCloner *sc, SILInstruction *Orig)
    : SILBuilder(sc->getBuilder().getInsertionBB(), &InsertedInstrs),
      SC(*sc), Orig(Orig)
    {
      setInsertionPoint(SC.getBuilder().getInsertionBB(),
                        SC.getBuilder().getInsertionPoint());
    }

  ~SILBuilderWithPostProcess() {
    for (auto *I : InsertedInstrs) {
      SC.recordClonedInstruction(Orig, I);
    }
  }
};


/// SILClonerWithScopes - a SILCloner that automatically clones
/// SILDebugScopes. In contrast to inline scopes, this generates a
/// deep copy of the scope tree.
template<typename ImplClass>
class SILClonerWithScopes : public SILCloner<ImplClass> {
  friend class SILCloner<ImplClass>;
public:
  SILClonerWithScopes(SILFunction &To,
                      DominanceInfo *DT = nullptr,
                      bool Disable = false)
      : SILCloner<ImplClass>(To, DT) {

    // We only want to do this when we generate cloned functions, not
    // when we inline.

    // FIXME: This is due to having TypeSubstCloner inherit from
    //        SILClonerWithScopes, and having TypeSubstCloner be used
    //        both by passes that clone whole functions and ones that
    //        inline functions.
    if (Disable)
      return;

    scopeCloner.reset(new ScopeCloner(To));
  }

private:
  std::unique_ptr<ScopeCloner> scopeCloner;
protected:
  /// Clone the SILDebugScope for the cloned function.
  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    SILCloner<ImplClass>::postProcess(Orig, Cloned);
  }

  const SILDebugScope *remapScope(const SILDebugScope *DS) {
    return scopeCloner ? scopeCloner->getOrCreateClonedScope(DS) : DS;
  }
};

/// Clone a function without transforming it.
class SILFunctionCloner : public SILClonerWithScopes<SILFunctionCloner> {
  using SuperTy = SILClonerWithScopes<SILFunctionCloner>;
  friend class SILCloner<SILFunctionCloner>;

public:
  SILFunctionCloner(SILFunction *newF) : SILClonerWithScopes(*newF) {}

  /// Clone all blocks in this function and all instructions in those
  /// blocks.
  ///
  /// This is used to clone an entire function without mutating the original
  /// function.
  ///
  /// The new function is expected to be completely empty. Clone the entry
  /// blocks arguments here. The cloned arguments become the inputs to the
  /// general SILCloner, which expects the new entry block to be ready to emit
  /// instructions into.
  void cloneFunction(SILFunction *origF) {
    SILFunction *newF = &Builder.getFunction();

    auto *newEntryBB = newF->createBasicBlock();
    newEntryBB->cloneArgumentList(origF->getEntryBlock());

    // Copy the new entry block arguments into a separate vector purely to
    // resolve the type mismatch between SILArgument* and SILValue.
    SmallVector<SILValue, 8> entryArgs;
    entryArgs.reserve(newF->getArguments().size());
    llvm::transform(newF->getArguments(), std::back_inserter(entryArgs),
                    [](SILArgument *arg) -> SILValue { return arg; });

    SuperTy::cloneFunctionBody(origF, newEntryBB, entryArgs);
  }
};

template<typename ImplClass>
SILValue
SILCloner<ImplClass>::getMappedValue(SILValue Value) {
  auto VI = ValueMap.find(Value);
  if (VI != ValueMap.end())
    return VI->second;

  // If we have undef, just remap the type.
  if (auto *U = dyn_cast<SILUndef>(Value)) {
    auto type = getOpType(U->getType());
    ValueBase *undef =
      (type == U->getType() ? U : SILUndef::get(type, Builder.getFunction()));
    return SILValue(undef);
  }

  llvm_unreachable("Unmapped value while cloning?");
}

template <typename ImplClass>
void SILCloner<ImplClass>::mapValue(SILValue origValue, SILValue mappedValue) {
  auto iterAndInserted = ValueMap.insert({origValue, mappedValue});
  (void)iterAndInserted;
  assert(iterAndInserted.second && "Original value already mapped.");
}

template<typename ImplClass>
SILBasicBlock*
SILCloner<ImplClass>::remapBasicBlock(SILBasicBlock *BB) {
  SILBasicBlock *MappedBB = BBMap[BB];
  assert(MappedBB && "Unmapped basic block while cloning?");
  return MappedBB;
}

template<typename ImplClass>
void
SILCloner<ImplClass>::postProcess(SILInstruction *orig,
                                  SILInstruction *cloned) {
  assert((!orig->getDebugScope() || cloned->getDebugScope() ||
          Builder.isInsertingIntoGlobal()) &&
         "cloned function dropped debug scope");

  // It sometimes happens that an instruction with no results gets mapped
  // to an instruction with results, e.g. when specializing a cast.
  // Just ignore this.
  auto origResults = orig->getResults();
  if (origResults.empty()) return;

  // Otherwise, map the results over one-by-one.
  auto clonedResults = cloned->getResults();
  assert(origResults.size() == clonedResults.size());
  for (auto i : indices(origResults))
    asImpl().mapValue(origResults[i], clonedResults[i]);
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitInstructionsInBlock(SILBasicBlock* BB) {
  // Iterate over and visit all instructions other than the terminator to clone.
  for (auto I = BB->begin(), E = --BB->end(); I != E; ++I) {
    asImpl().visit(&*I);
  }
}

template <typename ImplClass>
void SILCloner<ImplClass>::cloneReachableBlocks(
  SILBasicBlock *startBB, ArrayRef<SILBasicBlock *> exitBlocks,
  SILBasicBlock *insertAfterBB,
  bool havePrepopulatedFunctionArgs) {

  SILFunction *F = startBB->getParent();
  assert(F == &Builder.getFunction()
         && "cannot clone region across functions.");
  assert(BBMap.empty() && "This API does not allow clients to map blocks.");
  assert((havePrepopulatedFunctionArgs || ValueMap.empty()) &&
         "Stale ValueMap.");

  auto *clonedStartBB = insertAfterBB ? F->createBasicBlockAfter(insertAfterBB)
    : F->createBasicBlock();

  BBMap.insert(std::make_pair(startBB, clonedStartBB));
  getBuilder().setInsertionPoint(clonedStartBB);
  clonePhiArgs(startBB);

  // Premap exit blocks to terminate so that visitBlocksDepthFirst terminates
  // after discovering the cloned region. Mapping an exit block to itself
  // provides the correct destination block during visitTerminator.
  for (auto *exitBB : exitBlocks)
    BBMap[exitBB] = exitBB;

  // Discover and map the region to be cloned.
  visitBlocksDepthFirst(startBB);

  commonFixUp(F);
}

template <typename ImplClass>
void SILCloner<ImplClass>::cloneFunctionBody(SILFunction *F,
                                             SILBasicBlock *clonedEntryBB,
                                             ArrayRef<SILValue> entryArgs,
                                             bool replaceOriginalFunctionInPlace) {

  assert((replaceOriginalFunctionInPlace || F != clonedEntryBB->getParent()) &&
         "Must clone into a new function.");
  assert(BBMap.empty() && "This API does not allow clients to map blocks.");
  assert(ValueMap.empty() && "Stale ValueMap.");

  assert(entryArgs.size() == F->getArguments().size());
  for (unsigned i = 0, e = entryArgs.size(); i != e; ++i)
    ValueMap[F->getArgument(i)] = entryArgs[i];

  BBMap.insert(std::make_pair(&*F->begin(), clonedEntryBB));

  Builder.setInsertionPoint(clonedEntryBB);

  // This will layout all newly cloned blocks immediate after clonedEntryBB.
  visitBlocksDepthFirst(&*F->begin());

  commonFixUp(F);
}

template <typename ImplClass>
void SILCloner<ImplClass>::cloneFunctionBody(
    SILFunction *F, SILBasicBlock *clonedEntryBB, ArrayRef<SILValue> entryArgs,
    llvm::function_ref<SILValue(SILValue)> entryArgIndexToOldArgIndex) {
  assert(F != clonedEntryBB->getParent() && "Must clone into a new function.");
  assert(BBMap.empty() && "This API does not allow clients to map blocks.");
  assert(ValueMap.empty() && "Stale ValueMap.");

  assert(entryArgs.size() == F->getArguments().size());
  for (unsigned i = 0, e = entryArgs.size(); i != e; ++i) {
    ValueMap[entryArgIndexToOldArgIndex(entryArgs[i])] = entryArgs[i];
  }

  BBMap.insert(std::make_pair(&*F->begin(), clonedEntryBB));

  Builder.setInsertionPoint(clonedEntryBB);

  // This will layout all newly cloned blocks immediate after clonedEntryBB.
  visitBlocksDepthFirst(&*F->begin());

  commonFixUp(F);
}

template<typename ImplClass>
void SILCloner<ImplClass>::clonePhiArgs(SILBasicBlock *oldBB) {
  auto *mappedBB = BBMap[oldBB];

  // Create new arguments for each of the original block's arguments.
  for (auto *Arg : oldBB->getSILPhiArguments()) {
    SILValue mappedArg = mappedBB->createPhiArgument(
        getOpType(Arg->getType()), Arg->getOwnershipKind(), Arg->getDecl(),
        Arg->isReborrow(), Arg->hasPointerEscape());

    asImpl().mapValue(Arg, mappedArg);
  }
}

// This private helper visits BBs in depth-first preorder (only processing
// blocks on the first visit), mapping newly visited BBs to new BBs and cloning
// all instructions into the caller.
template <typename ImplClass>
void SILCloner<ImplClass>::visitBlocksDepthFirst(SILBasicBlock *startBB) {
  // The caller clones startBB because it may be a function header, which
  // requires special handling.
  assert(BBMap.count(startBB) && "The caller must map the first BB.");

  assert(preorderBlocks.empty());

  // First clone the CFG region.
  //
  // FIXME: Add reverse iteration to SILSuccessor, then convert this to an RPOT
  // traversal. We would prefer to keep CFG regions in RPO order, and this would
  // not create as large a worklist for functions with many large switches.
  SmallVector<SILBasicBlock *, 8> dfsWorklist(1, startBB);
  // Keep a reference to the last cloned BB so blocks can be laid out in the
  // order they are created, which differs from the order they are
  // cloned. Blocks are created in BFS order but cloned in DFS preorder (when no
  // critical edges are present).
  lastClonedBB = BBMap[startBB];
  while (!dfsWorklist.empty()) {
    auto *BB = dfsWorklist.pop_back_val();
    preorderBlocks.push_back(BB);

    // Phis are cloned during the first preorder walk so that successor phis
    // exist before predecessor terminators are generated.
    if (BB != startBB)
      clonePhiArgs(BB);

    // Non-terminating instructions are cloned in the first preorder walk.
    getBuilder().setInsertionPoint(BBMap[BB]);
    asImpl().visitInstructionsInBlock(BB);

    unsigned dfsSuccStartIdx = dfsWorklist.size();

    // splitEdge may rewrite BB's successors during this loop.
    for (unsigned succIdx = 0, numSucc = BB->getSuccessors().size();
         succIdx != numSucc; ++succIdx) {

      // Only visit a successor that has not already been visited and was not
      // premapped by the client.
      if (BBMap.count(BB->getSuccessors()[succIdx])) {
        // After cloning BB, this successor may be a new CFG merge. If it is
        // valid to branch directly from the BB to its clone do nothing; if not,
        // split the edge from BB->succ and clone the new block.
        //
        // A CFG merge may require new block arguments, so check for both a
        // critical edge and the ability to add branch arguments (BranchInst).
        if (BB->getSingleSuccessorBlock()
            && isa<BranchInst>(BB->getTerminator())) {
          continue;
        }
        // This predecessor has multiple successors, so cloning it without
        // cloning its successors would create a critical edge.
        splitEdge(BB->getTerminator(), succIdx, DomTree);
        assert(!BBMap.count(BB->getSuccessors()[succIdx]));
      }
      // Map the successor to a new BB. Layout the cloned blocks in the order
      // they are visited and cloned.
      lastClonedBB =
          getBuilder().getFunction().createBasicBlockAfter(lastClonedBB);

      // After splitting, BB has stable successors.
      auto &succ = BB->getSuccessors()[succIdx];
      BBMap.insert(std::make_pair(succ.getBB(), lastClonedBB));

      dfsWorklist.push_back(succ);
    }
    // Reverse the worklist to pop the successors in forward order. This
    // precisely yields DFS preorder when no critical edges are present.
    std::reverse(dfsWorklist.begin() + dfsSuccStartIdx, dfsWorklist.end());
  }
  // Visit terminators only after the CFG is valid so all branch targets exist.
  //
  // Visiting in pre-order provides a nice property for the individual
  // instruction visitors. It allows those visitors to make use of dominance
  // relationships, particularly the fact that operand values will be mapped.
  for (auto *origBB : preorderBlocks) {
    // Set the insertion point to the new mapped BB
    getBuilder().setInsertionPoint(BBMap[origBB]);
    asImpl().visitTerminator(origBB);
  }
}

/// Clean-up after cloning.
template <typename ImplClass>
void SILCloner<ImplClass>::commonFixUp(SILFunction *F) {
  // Call any cleanup specific to the CRTP extensions.
  asImpl().preFixUp(F);

  // If our source function is in ossa form, but the function into which we are
  // cloning is not in ossa, after we clone, eliminate default arguments.
  if (!getBuilder().hasOwnership() && F->hasOwnership()) {
    for (auto &Block : getBuilder().getFunction()) {
      auto *Term = Block.getTerminator();
      if (auto *CCBI = dyn_cast<CheckedCastBranchInst>(Term)) {
        // Check if we have a default argument.
        auto *FailureBlock = CCBI->getFailureBB();
        assert(FailureBlock->getNumArguments() <= 1 &&
               "We should either have no args or a single default arg");
        if (0 == FailureBlock->getNumArguments())
          continue;
        FailureBlock->getArgument(0)->replaceAllUsesWith(CCBI->getOperand());
        FailureBlock->eraseArgument(0);
        continue;
      }

      if (auto *SEI = dyn_cast<SwitchEnumInst>(Term)) {
        if (auto DefaultBlock = SEI->getDefaultBBOrNull()) {
          assert(DefaultBlock.get()->getNumArguments() <= 1 &&
                 "We should either have no args or a single default arg");
          if (0 == DefaultBlock.get()->getNumArguments())
            continue;
          DefaultBlock.get()->getArgument(0)->replaceAllUsesWith(
              SEI->getOperand());
          DefaultBlock.get()->eraseArgument(0);
          continue;
        }
      }
    }
  }

  // Call any cleanup specific to the CRTP extensions.
  asImpl().postFixUp(F);
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocStackInst(AllocStackInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  // Drop the debug info from mandatory-inlined instructions. It's the law!
  SILLocation Loc = getOpLocation(Inst->getLoc());
  llvm::Optional<SILDebugVariable> VarInfo = Inst->getVarInfo();
  if (Loc.getKind() == SILLocation::MandatoryInlinedKind) {
    Loc = MandatoryInlinedLocation::getAutoGeneratedLocation();
    VarInfo = llvm::None;
  }
  auto *NewInst = getBuilder().createAllocStack(
      Loc, getOpType(Inst->getElementType()), VarInfo,
      Inst->hasDynamicLifetime(), Inst->isLexical(),
      Inst->getUsesMoveableValueDebugInfo()
#ifndef NDEBUG
    , true
#endif
  );
  remapDebugVarInfo(DebugVarCarryingInst(NewInst));
  recordClonedInstruction(Inst, NewInst);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitAllocPackMetadataInst(
    AllocPackMetadataInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createAllocPackMetadata(
                                    getOpLocation(Inst->getLoc())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocPackInst(AllocPackInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  SILLocation Loc = getOpLocation(Inst->getLoc());
  auto *NewInst = getBuilder().createAllocPack(
      Loc, getOpType(Inst->getType().getObjectType()));
  recordClonedInstruction(Inst, NewInst);
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocRefInst(AllocRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto CountArgs = getOpValueArray<8>(OperandValueArrayRef(Inst->
                                                    getTailAllocatedCounts()));
  SmallVector<SILType, 4> ElemTypes;
  for (SILType OrigElemType : Inst->getTailAllocatedTypes()) {
    ElemTypes.push_back(getOpType(OrigElemType));
  }
  auto *NewInst = getBuilder().createAllocRef(getOpLocation(Inst->getLoc()),
                                      getOpType(Inst->getType()),
                                      Inst->isObjC(), Inst->canAllocOnStack(), Inst->isBare(),
                                      ElemTypes, CountArgs);
  recordClonedInstruction(Inst, NewInst);
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocRefDynamicInst(AllocRefDynamicInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto CountArgs = getOpValueArray<8>(OperandValueArrayRef(Inst->
                                                  getTailAllocatedCounts()));
  SmallVector<SILType, 4> ElemTypes;
  for (SILType OrigElemType : Inst->getTailAllocatedTypes()) {
    ElemTypes.push_back(getOpType(OrigElemType));
  }
  auto *NewInst = getBuilder().createAllocRefDynamic(
                                      getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getMetatypeOperand()),
                                      getOpType(Inst->getType()),
                                      Inst->isObjC(),
                                      Inst->canAllocOnStack(),
                                      ElemTypes, CountArgs);
  recordClonedInstruction(Inst, NewInst);
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocBoxInst(AllocBoxInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  // Drop the debug info from mandatory-inlined instructions.
  SILLocation Loc = getOpLocation(Inst->getLoc());
  llvm::Optional<SILDebugVariable> VarInfo = Inst->getVarInfo();
  if (Loc.getKind() == SILLocation::MandatoryInlinedKind) {
    Loc = MandatoryInlinedLocation::getAutoGeneratedLocation();
    VarInfo = llvm::None;
  }

  recordClonedInstruction(
      Inst,
      getBuilder().createAllocBox(
          Loc, this->getOpType(Inst->getType()).template castTo<SILBoxType>(),
          VarInfo, /*hasDynamicLifetime*/ false,
          /*reflection*/ false,
          /*usesMoveableValueDebugInfo*/ false, /*skipVarDeclAssert*/ true
          ));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocExistentialBoxInst(
                                                AllocExistentialBoxInst *Inst) {
  auto origExistentialType = Inst->getExistentialType();
  auto origFormalType = Inst->getFormalConcreteType();

  auto conformances = getOpConformances(origFormalType,
                                        Inst->getConformances());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAllocExistentialBox(
                getOpLocation(Inst->getLoc()), getOpType(origExistentialType),
                getOpASTType(origFormalType), conformances));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitBuiltinInst(BuiltinInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArguments());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBuiltin(
                getOpLocation(Inst->getLoc()), Inst->getName(),
                getOpType(Inst->getType()),
                getOpSubstitutionMap(Inst->getSubstitutions()), Args));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitApplyInst(ApplyInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArguments());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createApply(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getCallee()),
                getOpSubstitutionMap(Inst->getSubstitutionMap()), Args,
                Inst->getApplyOptions(),
                GenericSpecializationInformation::create(Inst, getBuilder())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitTryApplyInst(TryApplyInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArguments());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createTryApply(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getCallee()),
                getOpSubstitutionMap(Inst->getSubstitutionMap()), Args,
                getOpBasicBlock(Inst->getNormalBB()),
                getOpBasicBlock(Inst->getErrorBB()),
                Inst->getApplyOptions(),
                GenericSpecializationInformation::create(Inst, getBuilder())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitPartialApplyInst(PartialApplyInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArguments());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createPartialApply(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getCallee()),
                getOpSubstitutionMap(Inst->getSubstitutionMap()), Args,
                Inst->getType().getAs<SILFunctionType>()->getCalleeConvention(),
                Inst->isOnStack(),
                GenericSpecializationInformation::create(Inst, getBuilder())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitBeginApplyInst(BeginApplyInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArguments());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBeginApply(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getCallee()),
                getOpSubstitutionMap(Inst->getSubstitutionMap()), Args,
                Inst->getApplyOptions(),
                GenericSpecializationInformation::create(Inst, getBuilder())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAbortApplyInst(AbortApplyInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAbortApply(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitEndApplyInst(EndApplyInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createEndApply(getOpLocation(Inst->getLoc()),
                                        getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitFunctionRefInst(FunctionRefInst *Inst) {
  SILFunction *OpFunction =
      getOpFunction(Inst->getReferencedFunction());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createFunctionRef(
                                    getOpLocation(Inst->getLoc()), OpFunction));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDynamicFunctionRefInst(
    DynamicFunctionRefInst *Inst) {
  SILFunction *OpFunction =
      getOpFunction(Inst->getInitiallyReferencedFunction());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createDynamicFunctionRef(
                                    getOpLocation(Inst->getLoc()), OpFunction));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitPreviousDynamicFunctionRefInst(
    PreviousDynamicFunctionRefInst *Inst) {
  SILFunction *OpFunction =
      getOpFunction(Inst->getInitiallyReferencedFunction());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createPreviousDynamicFunctionRef(
                                    getOpLocation(Inst->getLoc()), OpFunction));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAllocGlobalInst(AllocGlobalInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAllocGlobal(getOpLocation(Inst->getLoc()),
                                           Inst->getReferencedGlobal()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitGlobalAddrInst(GlobalAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createGlobalAddr(getOpLocation(Inst->getLoc()),
                                          Inst->getReferencedGlobal()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitGlobalValueInst(GlobalValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createGlobalValue(getOpLocation(Inst->getLoc()),
                                           Inst->getReferencedGlobal(),
                                           Inst->isBare()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitBaseAddrForOffsetInst(BaseAddrForOffsetInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBaseAddrForOffset(getOpLocation(Inst->getLoc()),
                                                  getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitIntegerLiteralInst(IntegerLiteralInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createIntegerLiteral(getOpLocation(Inst->getLoc()),
                                              getOpType(Inst->getType()),
                                              Inst->getValue()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitFloatLiteralInst(FloatLiteralInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createFloatLiteral(getOpLocation(Inst->getLoc()),
                                            getOpType(Inst->getType()),
                                            Inst->getValue()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStringLiteralInst(StringLiteralInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createStringLiteral(
                                    getOpLocation(Inst->getLoc()),
                                    Inst->getValue(), Inst->getEncoding()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitLoadInst(LoadInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    switch (Inst->getOwnershipQualifier()) {
    case LoadOwnershipQualifier::Copy: {
      auto *li = getBuilder().createLoad(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getOperand()),
                                         LoadOwnershipQualifier::Unqualified);
      // This will emit a retain_value/strong_retain as appropriate.
      getBuilder().emitCopyValueOperation(getOpLocation(Inst->getLoc()), li);
      return recordClonedInstruction(Inst, li);
    }
    case LoadOwnershipQualifier::Take:
    case LoadOwnershipQualifier::Trivial:
    case LoadOwnershipQualifier::Unqualified:
      break;
    }
    return recordClonedInstruction(
        Inst, getBuilder().createLoad(getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getOperand()),
                                      LoadOwnershipQualifier::Unqualified));
  }

  return recordClonedInstruction(
      Inst, getBuilder().createLoad(getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getOwnershipQualifier()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitLoadBorrowInst(LoadBorrowInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  // If we are not inlining into an ownership function, just use a load.
  if (!getBuilder().hasOwnership()) {
    return recordClonedInstruction(
        Inst, getBuilder().createLoad(getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getOperand()),
                                      LoadOwnershipQualifier::Unqualified));
  }

  recordClonedInstruction(
      Inst, getBuilder().createLoadBorrow(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitBeginBorrowInst(BeginBorrowInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordFoldedValue(Inst, getOpValue(Inst->getOperand()));
  }

  recordClonedInstruction(
      Inst, getBuilder().createBeginBorrow(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           Inst->isLexical()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitStoreInst(StoreInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    switch (Inst->getOwnershipQualifier()) {
    case StoreOwnershipQualifier::Assign: {
      auto *li = getBuilder().createLoad(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getDest()),
                                         LoadOwnershipQualifier::Unqualified);
      auto *si = getBuilder().createStore(
          getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
          getOpValue(Inst->getDest()), StoreOwnershipQualifier::Unqualified);
      getBuilder().emitDestroyValueOperation(getOpLocation(Inst->getLoc()), li);
      return recordClonedInstruction(Inst, si);
    }
    case StoreOwnershipQualifier::Init:
    case StoreOwnershipQualifier::Trivial:
    case StoreOwnershipQualifier::Unqualified:
      break;
    }

    return recordClonedInstruction(
        Inst, getBuilder().createStore(getOpLocation(Inst->getLoc()),
                                       getOpValue(Inst->getSrc()),
                                       getOpValue(Inst->getDest()),
                                       StoreOwnershipQualifier::Unqualified));
  }

  recordClonedInstruction(
      Inst, getBuilder().createStore(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
                getOpValue(Inst->getDest()), Inst->getOwnershipQualifier()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitStoreBorrowInst(StoreBorrowInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    getBuilder().createStore(
        getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
        getOpValue(Inst->getDest()), StoreOwnershipQualifier::Unqualified);
    mapValue(Inst, getOpValue(Inst->getDest()));
    return;
  }

  recordClonedInstruction(
      Inst, getBuilder().createStoreBorrow(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getSrc()),
                                           getOpValue(Inst->getDest())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitEndBorrowInst(EndBorrowInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  // Do not clone any end_borrow.
  if (!getBuilder().hasOwnership())
    return;

  recordClonedInstruction(
      Inst, getBuilder().createEndBorrow(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitBeginAccessInst(BeginAccessInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBeginAccess(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getAccessKind(), Inst->getEnforcement(),
                Inst->hasNoNestedConflict(), Inst->isFromBuiltin()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitEndAccessInst(EndAccessInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createEndAccess(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getOperand()),
                                         Inst->isAborting()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitBeginUnpairedAccessInst(
                                           BeginUnpairedAccessInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBeginUnpairedAccess(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getSource()),
                getOpValue(Inst->getBuffer()), Inst->getAccessKind(),
                Inst->getEnforcement(), Inst->hasNoNestedConflict(),
                Inst->isFromBuiltin()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitEndUnpairedAccessInst(
                                             EndUnpairedAccessInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createEndUnpairedAccess(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getEnforcement(), Inst->isAborting(),
                                    Inst->isFromBuiltin()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitAssignInst(AssignInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAssign(getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getSrc()),
                                      getOpValue(Inst->getDest()),
                                      Inst->getOwnershipQualifier()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitAssignByWrapperInst(AssignByWrapperInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAssignByWrapper(
                getOpLocation(Inst->getLoc()),
                getOpValue(Inst->getSrc()), getOpValue(Inst->getDest()),
                getOpValue(Inst->getInitializer()),
                getOpValue(Inst->getSetter()), Inst->getMode()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitAssignOrInitInst(AssignOrInitInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAssignOrInit(
                getOpLocation(Inst->getLoc()),
                getOpValue(Inst->getSelf()),
                getOpValue(Inst->getSrc()),
                getOpValue(Inst->getInitializer()),
                getOpValue(Inst->getSetter()), Inst->getMode()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitMarkUninitializedInst(MarkUninitializedInst *Inst) {
  SILValue OpValue = getOpValue(Inst->getOperand());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createMarkUninitialized(
                                    getOpLocation(Inst->getLoc()), OpValue,
                                    Inst->getMarkUninitializedKind()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitMarkFunctionEscapeInst(MarkFunctionEscapeInst *Inst){
  auto OpElements = getOpValueArray<8>(Inst->getElements());
  auto OpLoc = getOpLocation(Inst->getLoc());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createMarkFunctionEscape(OpLoc, OpElements));
}
template<typename ImplClass>
void
SILCloner<ImplClass>::visitDebugValueInst(DebugValueInst *Inst) {
  // We cannot inline/clone debug intrinsics without a scope. If they
  // describe function arguments there is no way to determine which
  // function they belong to.
  if (!Inst->getDebugScope())
    return;

  // Since we want the debug info to survive, we do not remap the location here.
  SILDebugVariable VarInfo = *Inst->getVarInfo();
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto *NewInst = getBuilder().createDebugValue(
      Inst->getLoc(), getOpValue(Inst->getOperand()), VarInfo,
      Inst->poisonRefs(), Inst->getUsesMoveableValueDebugInfo(),
      Inst->hasTrace());
  remapDebugVarInfo(DebugVarCarryingInst(NewInst));
  recordClonedInstruction(Inst, NewInst);
}
template<typename ImplClass>
void
SILCloner<ImplClass>::visitDebugStepInst(DebugStepInst *Inst) {
  recordClonedInstruction(Inst, getBuilder().createDebugStep(Inst->getLoc()));
}

#define NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...)                    \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visitLoad##Name##Inst(Load##Name##Inst *Inst) {   \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(                                                   \
        Inst, getBuilder().createLoad##Name(getOpLocation(Inst->getLoc()),     \
                                            getOpValue(Inst->getOperand()),    \
                                            Inst->isTake()));                  \
  }                                                                            \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visitStore##Name##Inst(Store##Name##Inst *Inst) { \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(                                                   \
        Inst, getBuilder().createStore##Name(getOpLocation(Inst->getLoc()),    \
                                             getOpValue(Inst->getSrc()),       \
                                             getOpValue(Inst->getDest()),      \
                                             Inst->isInitializationOfDest())); \
  }
#define LOADABLE_REF_STORAGE_HELPER(Name, name)                                \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visitRefTo##Name##Inst(RefTo##Name##Inst *Inst) { \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(                                                   \
        Inst, getBuilder().createRefTo##Name(getOpLocation(Inst->getLoc()),    \
                                             getOpValue(Inst->getOperand()),   \
                                             getOpType(Inst->getType())));     \
  }                                                                            \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visit##Name##ToRefInst(Name##ToRefInst *Inst) {   \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(                                                   \
        Inst, getBuilder().create##Name##ToRef(getOpLocation(Inst->getLoc()),  \
                                               getOpValue(Inst->getOperand()), \
                                               getOpType(Inst->getType())));   \
  }                                                                            \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visitStrongCopy##Name##ValueInst(                 \
      StrongCopy##Name##ValueInst *Inst) {                                     \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(Inst, getBuilder().createStrongCopy##Name##Value(  \
                                      getOpLocation(Inst->getLoc()),           \
                                      getOpValue(Inst->getOperand())));        \
  }
#define ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...)                   \
  LOADABLE_REF_STORAGE_HELPER(Name, name)                                      \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visitStrongRetain##Name##Inst(                    \
      StrongRetain##Name##Inst *Inst) {                                        \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(Inst, getBuilder().createStrongRetain##Name(       \
                                      getOpLocation(Inst->getLoc()),           \
                                      getOpValue(Inst->getOperand()),          \
                                      Inst->getAtomicity()));                  \
  }                                                                            \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visit##Name##RetainInst(Name##RetainInst *Inst) { \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(Inst, getBuilder().create##Name##Retain(           \
                                      getOpLocation(Inst->getLoc()),           \
                                      getOpValue(Inst->getOperand()),          \
                                      Inst->getAtomicity()));                  \
  }                                                                            \
  template <typename ImplClass>                                                \
  void SILCloner<ImplClass>::visit##Name##ReleaseInst(                         \
      Name##ReleaseInst *Inst) {                                               \
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));      \
    recordClonedInstruction(Inst, getBuilder().create##Name##Release(          \
                                      getOpLocation(Inst->getLoc()),           \
                                      getOpValue(Inst->getOperand()),          \
                                      Inst->getAtomicity()));                  \
  }
#define SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  NEVER_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...") \
  ALWAYS_LOADABLE_CHECKED_REF_STORAGE(Name, name, "...")
#define UNCHECKED_REF_STORAGE(Name, name, ...) \
  LOADABLE_REF_STORAGE_HELPER(Name, name)
#include "swift/AST/ReferenceStorage.def"
#undef LOADABLE_REF_STORAGE_HELPER

template<typename ImplClass>
void
SILCloner<ImplClass>::visitCopyAddrInst(CopyAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createCopyAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
                getOpValue(Inst->getDest()), Inst->isTakeOfSrc(),
                Inst->isInitializationOfDest()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitExplicitCopyAddrInst(
    ExplicitCopyAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    recordClonedInstruction(
        Inst, getBuilder().createCopyAddr(
                  getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
                  getOpValue(Inst->getDest()), Inst->isTakeOfSrc(),
                  Inst->isInitializationOfDest()));
  } else {
    // preserve the explicit_*
    recordClonedInstruction(
        Inst, getBuilder().createExplicitCopyAddr(
                  getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
                  getOpValue(Inst->getDest()), Inst->isTakeOfSrc(),
                  Inst->isInitializationOfDest()));
  }
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMarkUnresolvedMoveAddrInst(
    MarkUnresolvedMoveAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto *MVI = getBuilder().createMarkUnresolvedMoveAddr(
      getOpLocation(Inst->getLoc()), getOpValue(Inst->getSrc()),
      getOpValue(Inst->getDest()));
  recordClonedInstruction(Inst, MVI);
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitBindMemoryInst(BindMemoryInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBindMemory(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getBase()),
                getOpValue(Inst->getIndex()), getOpType(Inst->getBoundType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitRebindMemoryInst(RebindMemoryInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRebindMemory(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getBase()),
                                            getOpValue(Inst->getInToken())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitConvertFunctionInst(ConvertFunctionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createConvertFunction(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()), Inst->withoutActuallyEscaping(),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitConvertEscapeToNoEscapeInst(
    ConvertEscapeToNoEscapeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createConvertEscapeToNoEscape(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()), Inst->isLifetimeGuaranteed()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitUpcastInst(UpcastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUpcast(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitAddressToPointerInst(AddressToPointerInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAddressToPointer(getOpLocation(Inst->getLoc()),
                                                getOpValue(Inst->getOperand()),
                                                getOpType(Inst->getType()),
                                                Inst->needsStackProtection()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitPointerToAddressInst(PointerToAddressInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createPointerToAddress(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()), Inst->isStrict(),
                Inst->isInvariant(), Inst->alignment()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitUncheckedRefCastInst(UncheckedRefCastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedRefCast(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitUncheckedRefCastAddrInst(UncheckedRefCastAddrInst *Inst) {
  SILLocation OpLoc = getOpLocation(Inst->getLoc());
  SILValue SrcValue = getOpValue(Inst->getSrc());
  SILValue DestValue = getOpValue(Inst->getDest());
  CanType SrcType = getOpASTType(Inst->getSourceFormalType());
  CanType TargetType = getOpASTType(Inst->getTargetFormalType());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedRefCastAddr(OpLoc, SrcValue, SrcType,
                                                    DestValue, TargetType));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitUncheckedAddrCastInst(UncheckedAddrCastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedAddrCast(getOpLocation(Inst->getLoc()),
                                                 getOpValue(Inst->getOperand()),
                                                 getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitUncheckedTrivialBitCastInst(UncheckedTrivialBitCastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createUncheckedTrivialBitCast(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitUncheckedBitwiseCastInst(UncheckedBitwiseCastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createUncheckedBitwiseCast(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitUncheckedValueCastInst(
    UncheckedValueCastInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    recordClonedInstruction(Inst, getBuilder().createUncheckedBitwiseCast(
                                      getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getOperand()),
                                      getOpType(Inst->getType())));
    return;
  }
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedValueCast(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitRefToBridgeObjectInst(RefToBridgeObjectInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRefToBridgeObject(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand(0)),
                getOpValue(Inst->getBitsOperand()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitBridgeObjectToRefInst(BridgeObjectToRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBridgeObjectToRef(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitBridgeObjectToWordInst(BridgeObjectToWordInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createBridgeObjectToWord(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitRefToRawPointerInst(RefToRawPointerInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRefToRawPointer(getOpLocation(Inst->getLoc()),
                                               getOpValue(Inst->getOperand()),
                                               getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitValueToBridgeObjectInst(
    ValueToBridgeObjectInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createValueToBridgeObject(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitRawPointerToRefInst(RawPointerToRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRawPointerToRef(getOpLocation(Inst->getLoc()),
                                               getOpValue(Inst->getOperand()),
                                               getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitThinToThickFunctionInst(ThinToThickFunctionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createThinToThickFunction(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitThickToObjCMetatypeInst(ThickToObjCMetatypeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createThickToObjCMetatype(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitObjCToThickMetatypeInst(ObjCToThickMetatypeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createObjCToThickMetatype(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitUnconditionalCheckedCastInst(
                                          UnconditionalCheckedCastInst *Inst) {
  SILLocation OpLoc = getOpLocation(Inst->getLoc());
  SILValue OpValue = getOpValue(Inst->getOperand());
  SILType OpLoweredType = getOpType(Inst->getTargetLoweredType());
  CanType OpFormalType = getOpASTType(Inst->getTargetFormalType());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createUnconditionalCheckedCast(
                              OpLoc, OpValue, OpLoweredType, OpFormalType,
                              getBuilder().hasOwnership()
                                  ? Inst->getForwardingOwnershipKind()
                                  : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitUnconditionalCheckedCastAddrInst(
                                      UnconditionalCheckedCastAddrInst *Inst) {
  SILLocation OpLoc = getOpLocation(Inst->getLoc());
  SILValue SrcValue = getOpValue(Inst->getSrc());
  SILValue DestValue = getOpValue(Inst->getDest());
  CanType SrcType = getOpASTType(Inst->getSourceFormalType());
  CanType TargetType = getOpASTType(Inst->getTargetFormalType());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createUnconditionalCheckedCastAddr(
                              OpLoc, SrcValue, SrcType, DestValue, TargetType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitRetainValueInst(RetainValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRetainValue(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitRetainValueAddrInst(RetainValueAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRetainValueAddr(getOpLocation(Inst->getLoc()),
                                               getOpValue(Inst->getOperand()),
                                               Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitUnmanagedRetainValueInst(
    UnmanagedRetainValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordClonedInstruction(
        Inst, getBuilder().createRetainValue(getOpLocation(Inst->getLoc()),
                                             getOpValue(Inst->getOperand()),
                                             Inst->getAtomicity()));
  }

  recordClonedInstruction(Inst, getBuilder().createUnmanagedRetainValue(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitCopyValueInst(CopyValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    // Noescape closures become trivial after OSSA.
    if (auto fnTy = Inst->getType().getAs<SILFunctionType>()) {
      if (fnTy->isTrivialNoEscape()) {
        return recordFoldedValue(Inst, getOpValue(Inst->getOperand()));
      }
    }
  
    SILValue newValue = getBuilder().emitCopyValueOperation(
        getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()));
    return recordFoldedValue(Inst, newValue);
  }

  recordClonedInstruction(
      Inst, getBuilder().createCopyValue(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitExplicitCopyValueInst(
    ExplicitCopyValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    SILValue newValue = getBuilder().emitCopyValueOperation(
        getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()));
    return recordFoldedValue(Inst, newValue);
  }

  recordClonedInstruction(
      Inst, getBuilder().createExplicitCopyValue(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMoveValueInst(MoveValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordFoldedValue(Inst, getOpValue(Inst->getOperand()));
  }
  auto *MVI = getBuilder().createMoveValue(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           Inst->isLexical());
  MVI->setAllowsDiagnostics(Inst->getAllowDiagnostics());
  recordClonedInstruction(Inst, MVI);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDropDeinitInst(DropDeinitInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordFoldedValue(Inst, getOpValue(Inst->getOperand()));
  }
  auto *MVI = getBuilder().createDropDeinit(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getOperand()));
  recordClonedInstruction(Inst, MVI);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMarkMustCheckInst(MarkMustCheckInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto *MVI = getBuilder().createMarkMustCheckInst(
      getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
      Inst->getCheckKind());
  recordClonedInstruction(Inst, MVI);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMarkUnresolvedReferenceBindingInst(
    MarkUnresolvedReferenceBindingInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto *MVI = getBuilder().createMarkUnresolvedReferenceBindingInst(
      getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
      Inst->getKind());
  recordClonedInstruction(Inst, MVI);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMoveOnlyWrapperToCopyableValueInst(
    MoveOnlyWrapperToCopyableValueInst *inst) {
  getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
  MoveOnlyWrapperToCopyableValueInst *cvt;
  if (inst->getOwnershipKind() == OwnershipKind::Owned) {
    cvt = getBuilder().createOwnedMoveOnlyWrapperToCopyableValue(
        getOpLocation(inst->getLoc()), getOpValue(inst->getOperand()));
  } else {
    assert(inst->getOwnershipKind() == OwnershipKind::Guaranteed);
    cvt = getBuilder().createGuaranteedMoveOnlyWrapperToCopyableValue(
        getOpLocation(inst->getLoc()), getOpValue(inst->getOperand()));
  }
  recordClonedInstruction(inst, cvt);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMoveOnlyWrapperToCopyableBoxInst(
    MoveOnlyWrapperToCopyableBoxInst *inst) {
  getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
  recordClonedInstruction(
      inst, getBuilder().createMoveOnlyWrapperToCopyableBox(
                getOpLocation(inst->getLoc()), getOpValue(inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMoveOnlyWrapperToCopyableAddrInst(
    MoveOnlyWrapperToCopyableAddrInst *inst) {
  getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
  recordClonedInstruction(
      inst, getBuilder().createMoveOnlyWrapperToCopyableAddr(
                getOpLocation(inst->getLoc()), getOpValue(inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitCopyableToMoveOnlyWrapperAddrInst(
    CopyableToMoveOnlyWrapperAddrInst *inst) {
  getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
  recordClonedInstruction(
      inst, getBuilder().createCopyableToMoveOnlyWrapperAddr(
                getOpLocation(inst->getLoc()), getOpValue(inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitCopyableToMoveOnlyWrapperValueInst(
    CopyableToMoveOnlyWrapperValueInst *inst) {
  getBuilder().setCurrentDebugScope(getOpScope(inst->getDebugScope()));
  CopyableToMoveOnlyWrapperValueInst *cvt;
  if (inst->getOwnershipKind() == OwnershipKind::Owned) {
    cvt = getBuilder().createOwnedCopyableToMoveOnlyWrapperValue(
        getOpLocation(inst->getLoc()), getOpValue(inst->getOperand()));
  } else {
    assert(inst->getOwnershipKind() == OwnershipKind::Guaranteed);
    cvt = getBuilder().createGuaranteedCopyableToMoveOnlyWrapperValue(
        getOpLocation(inst->getLoc()), getOpValue(inst->getOperand()));
  }
  recordClonedInstruction(inst, cvt);
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitReleaseValueInst(ReleaseValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createReleaseValue(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getOperand()),
                                            Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitReleaseValueAddrInst(
    ReleaseValueAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createReleaseValueAddr(getOpLocation(Inst->getLoc()),
                                                getOpValue(Inst->getOperand()),
                                                Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitUnmanagedReleaseValueInst(
    UnmanagedReleaseValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordClonedInstruction(
        Inst, getBuilder().createReleaseValue(getOpLocation(Inst->getLoc()),
                                              getOpValue(Inst->getOperand()),
                                              Inst->getAtomicity()));
  }
  recordClonedInstruction(Inst, getBuilder().createUnmanagedReleaseValue(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDestroyValueInst(DestroyValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    // Noescape closures become trivial after OSSA.
    if (auto fnTy = Inst->getOperand()->getType().getAs<SILFunctionType>()) {
      if (fnTy->isTrivialNoEscape()) {
        // Destroying the partial_apply [stack] becomes the stack deallocation
        // of the context.
        if (auto origPA = Inst->getNonescapingClosureAllocation()) {
          recordClonedInstruction(Inst,
            getBuilder().createDeallocStack(getOpLocation(Inst->getLoc()),
                                            getOpValue(origPA)));
        }
        
        return;
      }
    }
  
    return recordClonedInstruction(
        Inst, getBuilder().createReleaseValue(
                  getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                  RefCountingInst::Atomicity::Atomic));
  }

  recordClonedInstruction(
      Inst, getBuilder().createDestroyValue(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getOperand()),
                                            Inst->poisonRefs()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitAutoreleaseValueInst(
    AutoreleaseValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createAutoreleaseValue(getOpLocation(Inst->getLoc()),
                                                getOpValue(Inst->getOperand()),
                                                Inst->getAtomicity()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitUnmanagedAutoreleaseValueInst(
    UnmanagedAutoreleaseValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    return recordClonedInstruction(Inst, getBuilder().createAutoreleaseValue(
                                             getOpLocation(Inst->getLoc()),
                                             getOpValue(Inst->getOperand()),
                                             Inst->getAtomicity()));
  }

  recordClonedInstruction(Inst, getBuilder().createUnmanagedAutoreleaseValue(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getAtomicity()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitSetDeallocatingInst(SetDeallocatingInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSetDeallocating(getOpLocation(Inst->getLoc()),
                                               getOpValue(Inst->getOperand()),
                                               Inst->getAtomicity()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitObjectInst(ObjectInst *Inst) {
  auto Elements = getOpValueArray<8>(Inst->getAllElements());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst,
      getBuilder().createObject(getOpLocation(Inst->getLoc()), Inst->getType(),
                                Elements, Inst->getBaseElements().size(),
                                getBuilder().hasOwnership()
                                    ? Inst->getForwardingOwnershipKind()
                                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStructInst(StructInst *Inst) {
  auto Elements = getOpValueArray<8>(Inst->getElements());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst,
      getBuilder().createStruct(getOpLocation(Inst->getLoc()),
                                getOpType(Inst->getType()), Elements,
                                getBuilder().hasOwnership()
                                    ? Inst->getForwardingOwnershipKind()
                                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitTupleInst(TupleInst *Inst) {
  auto Elements = getOpValueArray<8>(Inst->getElements());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst,
      getBuilder().createTuple(getOpLocation(Inst->getLoc()),
                               getOpType(Inst->getType()), Elements,
                               getBuilder().hasOwnership()
                                   ? Inst->getForwardingOwnershipKind()
                                   : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitEnumInst(EnumInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst,
      getBuilder().createEnum(
          getOpLocation(Inst->getLoc()),
          Inst->hasOperand() ? getOpValue(Inst->getOperand()) : SILValue(),
          Inst->getElement(), getOpType(Inst->getType()),
          getBuilder().hasOwnership()
              ? Inst->getForwardingOwnershipKind()
              : ValueOwnershipKind(OwnershipKind::None)));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitInitEnumDataAddrInst(InitEnumDataAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createInitEnumDataAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getElement(), getOpType(Inst->getType())));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitUncheckedEnumDataInst(UncheckedEnumDataInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedEnumData(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getElement(), getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitUncheckedTakeEnumDataAddrInst(UncheckedTakeEnumDataAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUncheckedTakeEnumDataAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getElement(), getOpType(Inst->getType())));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitInjectEnumAddrInst(InjectEnumAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createInjectEnumAddr(getOpLocation(Inst->getLoc()),
                                              getOpValue(Inst->getOperand()),
                                              Inst->getElement()));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitMetatypeInst(MetatypeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createMetatype(getOpLocation(Inst->getLoc()),
                                        getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitValueMetatypeInst(ValueMetatypeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createValueMetatype(getOpLocation(Inst->getLoc()),
                                             getOpType(Inst->getType()),
                                             getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitExistentialMetatypeInst(ExistentialMetatypeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createExistentialMetatype(
                                    getOpLocation(Inst->getLoc()),
                                    getOpType(Inst->getType()),
                                    getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitTupleExtractInst(TupleExtractInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createTupleExtract(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getFieldIndex(), getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitTupleElementAddrInst(TupleElementAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createTupleElementAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getFieldIndex(), getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStructExtractInst(StructExtractInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createStructExtract(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getField(), getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStructElementAddrInst(StructElementAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createStructElementAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getField(), getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitRefElementAddrInst(RefElementAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRefElementAddr(
            getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
            Inst->getField(), getOpType(Inst->getType()), Inst->isImmutable()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitRefTailAddrInst(RefTailAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createRefTailAddr(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           getOpType(Inst->getType()),
                                           Inst->isImmutable()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDestructureStructInst(
    DestructureStructInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  if (!getBuilder().hasOwnership()) {
    getBuilder().emitDestructureValueOperation(
        getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
        [&](unsigned index, SILValue value) {
          recordFoldedValue(Inst->getResults()[index], value);
        });
    return;
  }

  recordClonedInstruction(
      Inst, getBuilder().createDestructureStruct(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDestructureTupleInst(
    DestructureTupleInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  if (!getBuilder().hasOwnership()) {
    getBuilder().emitDestructureValueOperation(
        getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
        [&](unsigned index, SILValue value) {
          recordFoldedValue(Inst->getResults()[index], value);
        });
    return;
  }

  recordClonedInstruction(
      Inst, getBuilder().createDestructureTuple(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitClassMethodInst(ClassMethodInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createClassMethod(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           Inst->getMember(), Inst->getType()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitSuperMethodInst(SuperMethodInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSuperMethod(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           Inst->getMember(), Inst->getType()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitObjCMethodInst(ObjCMethodInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createObjCMethod(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getMember(), getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitObjCSuperMethodInst(ObjCSuperMethodInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createObjCSuperMethod(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    Inst->getMember(), Inst->getType()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitWitnessMethodInst(WitnessMethodInst *Inst) {
  auto lookupType = Inst->getLookupType();
  auto conformance = getOpConformance(lookupType, Inst->getConformance());
  auto newLookupType = getOpASTType(lookupType);

  if (conformance.isConcrete()) {
    CanType Ty = conformance.getConcrete()->getType()->getCanonicalType();

    if (Ty != newLookupType) {
      assert(
          (Ty->isExactSuperclassOf(newLookupType) ||
           getBuilder().getModule().Types.getLoweredRValueType(
               getBuilder().getTypeExpansionContext(), Ty) == newLookupType) &&
          "Should only create upcasts for sub class.");

      // We use the super class as the new look up type.
      newLookupType = Ty;
    }
  }

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createWitnessMethod(
                              getOpLocation(Inst->getLoc()), newLookupType,
                              conformance, Inst->getMember(), Inst->getType()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitOpenExistentialAddrInst(OpenExistentialAddrInst *Inst) {
  // Create a new archetype for this opened existential type.
  remapRootOpenedType(Inst->getType().castTo<OpenedArchetypeType>());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createOpenExistentialAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()), Inst->getAccessKind()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitOpenExistentialValueInst(
    OpenExistentialValueInst *Inst) {
  // Create a new archetype for this opened existential type.
  remapRootOpenedType(Inst->getType().castTo<OpenedArchetypeType>());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createOpenExistentialValue(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitOpenExistentialMetatypeInst(OpenExistentialMetatypeInst *Inst) {
  // Create a new archetype for this opened existential type.
  auto openedType = Inst->getType().getASTType();
  auto exType = Inst->getOperand()->getType().getASTType();
  while (auto exMetatype = dyn_cast<ExistentialMetatypeType>(exType)) {
    exType = exMetatype->getExistentialInstanceType()->getCanonicalType();
    openedType = cast<MetatypeType>(openedType).getInstanceType();
  }
  remapRootOpenedType(cast<OpenedArchetypeType>(openedType));

  if (!Inst->getOperand()->getType().canUseExistentialRepresentation(
          ExistentialRepresentation::Class)) {
    getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
    recordClonedInstruction(Inst, getBuilder().createOpenExistentialMetatype(
                                      getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getOperand()),
                                      getOpType(Inst->getType())));
    return;
  }

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createOpenExistentialMetatype(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitOpenExistentialRefInst(OpenExistentialRefInst *Inst) {
  // Create a new archetype for this opened existential type.
  remapRootOpenedType(Inst->getType().castTo<OpenedArchetypeType>());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createOpenExistentialRef(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitOpenExistentialBoxInst(OpenExistentialBoxInst *Inst) {
  // Create a new archetype for this opened existential type.
  remapRootOpenedType(Inst->getType().castTo<OpenedArchetypeType>());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createOpenExistentialBox(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitOpenExistentialBoxValueInst(OpenExistentialBoxValueInst *Inst) {
  // Create a new archetype for this opened existential type.
  remapRootOpenedType(Inst->getType().castTo<OpenedArchetypeType>());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createOpenExistentialBoxValue(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitInitExistentialAddrInst(InitExistentialAddrInst *Inst) {
  CanType origFormalType = Inst->getFormalConcreteType();

  auto conformances = getOpConformances(origFormalType,
                                        Inst->getConformances());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createInitExistentialAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpASTType(origFormalType),
                getOpType(Inst->getLoweredConcreteType()), conformances));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitInitExistentialValueInst(
    InitExistentialValueInst *Inst) {
  CanType origFormalType = Inst->getFormalConcreteType();

  auto conformances = getOpConformances(origFormalType,
                                        Inst->getConformances());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createInitExistentialValue(
                getOpLocation(Inst->getLoc()), getOpType(Inst->getType()),
                getOpASTType(origFormalType), getOpValue(Inst->getOperand()),
                conformances));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitInitExistentialMetatypeInst(InitExistentialMetatypeInst *Inst) {
  auto origFormalType = Inst->getFormalErasedObjectType();
  auto conformances = getOpConformances(origFormalType,
                                        Inst->getConformances());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createInitExistentialMetatype(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType()), conformances));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitInitExistentialRefInst(InitExistentialRefInst *Inst) {
  CanType origFormalType = Inst->getFormalConcreteType();
  auto conformances = getOpConformances(origFormalType,
                                        Inst->getConformances());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createInitExistentialRef(
                getOpLocation(Inst->getLoc()), getOpType(Inst->getType()),
                getOpASTType(origFormalType), getOpValue(Inst->getOperand()),
                conformances));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeinitExistentialAddrInst(DeinitExistentialAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeinitExistentialAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDeinitExistentialValueInst(
    DeinitExistentialValueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeinitExistentialValue(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitPackLengthInst(PackLengthInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  auto loc = getOpLocation(Inst->getLoc());
  auto newPackType = cast<PackType>(getOpASTType(Inst->getPackType()));

  recordClonedInstruction(
      Inst, getBuilder().createPackLength(loc, newPackType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDynamicPackIndexInst(
    DynamicPackIndexInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  auto newIndexValue = getOpValue(Inst->getOperand());
  auto loc = getOpLocation(Inst->getLoc());
  auto newPackType = cast<PackType>(getOpASTType(Inst->getIndexedPackType()));

  recordClonedInstruction(
      Inst, getBuilder().createDynamicPackIndex(loc, newIndexValue,
                                                newPackType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitPackPackIndexInst(PackPackIndexInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  auto newIndexValue = getOpValue(Inst->getOperand());
  auto loc = getOpLocation(Inst->getLoc());
  auto newPackType = cast<PackType>(getOpASTType(Inst->getIndexedPackType()));

  auto newComponentStartIndex =
    getOpStructuralPackIndex(Inst->getIndexedPackType(),
                             Inst->getComponentStartIndex());

  recordClonedInstruction(
      Inst, getBuilder().createPackPackIndex(loc, newComponentStartIndex,
                                             newIndexValue, newPackType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitScalarPackIndexInst(
    ScalarPackIndexInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  auto loc = getOpLocation(Inst->getLoc());
  auto newPackType = cast<PackType>(getOpASTType(Inst->getIndexedPackType()));

  auto newComponentIndex =
    getOpStructuralPackIndex(Inst->getIndexedPackType(),
                             Inst->getComponentIndex());

  recordClonedInstruction(
      Inst, getBuilder().createScalarPackIndex(loc, newComponentIndex,
                                               newPackType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitOpenPackElementInst(
    OpenPackElementInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  auto newIndexValue = getOpValue(Inst->getIndexOperand());
  auto loc = getOpLocation(Inst->getLoc());

  // We need to make a new opened-element environment.  This is *not*
  // a refinement of the contextual environment of the new insertion
  // site; we just substitute the contextual substitutions in the
  // opened environment and build a new one.
  auto origEnv = Inst->getOpenedGenericEnvironment();

  // Substitute the contextual substitutions.
  auto newContextSubs =
    getOpSubstitutionMap(origEnv->getPackElementContextSubstitutions());

  // The opened shape class is a parameter of the original signature,
  // which is unchanged.
  auto openedShapeClass = origEnv->getOpenedElementShapeClass();

  // Build the new environment.
  auto newEnv =
    GenericEnvironment::forOpenedElement(origEnv->getGenericSignature(),
                                         UUID::fromTime(),
                                         openedShapeClass,
                                         newContextSubs);

  // Associate the old opened archetypes with the new ones.
  SmallVector<ArchetypeType*, 4> oldOpenedArchetypes;
  origEnv->forEachPackElementArchetype([&](ElementArchetypeType *oldType) {
    oldOpenedArchetypes.push_back(oldType);
  });
  {
    size_t nextOldIndex = 0;
    newEnv->forEachPackElementArchetype([&](ElementArchetypeType *newType) {
      ArchetypeType *oldType = oldOpenedArchetypes[nextOldIndex++];
      registerLocalArchetypeRemapping(oldType, newType);
    });
    assert(nextOldIndex == oldOpenedArchetypes.size() &&
           "different opened archetype count");
  }

  recordClonedInstruction(
      Inst, getBuilder().createOpenPackElement(loc, newIndexValue, newEnv));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitPackElementGetInst(PackElementGetInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto loc = getOpLocation(Inst->getLoc());
  auto newIndex = getOpValue(Inst->getIndex());
  auto newPack = getOpValue(Inst->getPack());
  auto newElementType = getOpType(Inst->getElementType());
  recordClonedInstruction(
      Inst, getBuilder().createPackElementGet(loc, newIndex, newPack,
                                              newElementType));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitPackElementSetInst(PackElementSetInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto loc = getOpLocation(Inst->getLoc());
  auto newElementValue = getOpValue(Inst->getValue());
  auto newIndex = getOpValue(Inst->getIndex());
  auto newPack = getOpValue(Inst->getPack());
  recordClonedInstruction(
      Inst, getBuilder().createPackElementSet(loc, newElementValue,
                                              newIndex, newPack));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitTuplePackElementAddrInst(
                                             TuplePackElementAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto loc = getOpLocation(Inst->getLoc());
  auto newIndex = getOpValue(Inst->getIndex());
  auto newTuple = getOpValue(Inst->getTuple());
  auto newElementType = getOpType(Inst->getElementType());

  // If the tuple-ness of the operand disappears due to substitution,
  // replace this instruction with an unchecked_addr_cast.
  // FIXME: use type_refine_addr instead
  if (doesOpTupleDisappear(Inst->getTupleType())) {
    recordClonedInstruction(
        Inst, getBuilder().createUncheckedAddrCast(loc, newTuple,
                                                   newElementType));
    return;
  }

  recordClonedInstruction(
      Inst, getBuilder().createTuplePackElementAddr(loc, newIndex, newTuple,
                                                    newElementType));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitCopyBlockInst(CopyBlockInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, Builder.createCopyBlock(getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitCopyBlockWithoutEscapingInst(
    CopyBlockWithoutEscapingInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, Builder.createCopyBlockWithoutEscaping(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getBlock()),
                                    getOpValue(Inst->getClosure())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStrongRetainInst(StrongRetainInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createStrongRetain(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getOperand()),
                                            Inst->getAtomicity()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitClassifyBridgeObjectInst(
                                            ClassifyBridgeObjectInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createClassifyBridgeObject(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitFixLifetimeInst(FixLifetimeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createFixLifetime(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitEndLifetimeInst(EndLifetimeInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  // These are only needed in OSSA.
  if (!getBuilder().hasOwnership())
    return;

  recordClonedInstruction(
      Inst, getBuilder().createEndLifetime(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitUncheckedOwnershipConversionInst(
    UncheckedOwnershipConversionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));

  if (!getBuilder().hasOwnership()) {
    return recordFoldedValue(Inst, getOpValue(Inst->getOperand()));
  }

  ValueOwnershipKind Kind = SILValue(Inst)->getOwnershipKind();
  if (getOpValue(Inst->getOperand())->getOwnershipKind() ==
      OwnershipKind::None) {
    Kind = OwnershipKind::None;
  }
  recordClonedInstruction(Inst, getBuilder().createUncheckedOwnershipConversion(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()), Kind));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitMarkDependenceInst(MarkDependenceInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createMarkDependence(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getValue()),
                getOpValue(Inst->getBase()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitStrongReleaseInst(StrongReleaseInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createStrongRelease(getOpLocation(Inst->getLoc()),
                                             getOpValue(Inst->getOperand()),
                                             Inst->getAtomicity()));
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitIsUniqueInst(IsUniqueInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createIsUnique(getOpLocation(Inst->getLoc()),
                                        getOpValue(Inst->getOperand())));
}
template<typename ImplClass>
void SILCloner<ImplClass>::visitBeginCOWMutationInst(BeginCOWMutationInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createBeginCOWMutation(getOpLocation(Inst->getLoc()),
                            getOpValue(Inst->getOperand()), Inst->isNative()));
}
template<typename ImplClass>
void SILCloner<ImplClass>::visitEndCOWMutationInst(EndCOWMutationInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createEndCOWMutation(getOpLocation(Inst->getLoc()),
                        getOpValue(Inst->getOperand()), Inst->doKeepUnique()));
}
template <typename ImplClass>
void SILCloner<ImplClass>::visitIsEscapingClosureInst(
    IsEscapingClosureInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createIsEscapingClosure(getOpLocation(Inst->getLoc()),
                                                 getOpValue(Inst->getOperand()),
                                                 Inst->getVerificationType()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocStackInst(DeallocStackInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocStack(getOpLocation(Inst->getLoc()),
                                            getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocPackInst(DeallocPackInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocPack(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDeallocPackMetadataInst(
    DeallocPackMetadataInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocPackMetadata(
                getOpLocation(Inst->getLoc()), Inst->getOperand()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocRefInst(DeallocRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocRef(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocStackRefInst(DeallocStackRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocStackRef(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocPartialRefInst(DeallocPartialRefInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createDeallocPartialRef(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getInstance()),
                                    getOpValue(Inst->getMetatype())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocBoxInst(DeallocBoxInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDeallocBox(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDeallocExistentialBoxInst(
                                              DeallocExistentialBoxInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createDeallocExistentialBox(
                                    getOpLocation(Inst->getLoc()),
                                    getOpASTType(Inst->getConcreteType()),
                                    getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitDestroyAddrInst(DestroyAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDestroyAddr(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitProjectBoxInst(ProjectBoxInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createProjectBox(getOpLocation(Inst->getLoc()),
                                          getOpValue(Inst->getOperand()),
                                          Inst->getFieldIndex()));
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitProjectExistentialBoxInst(
                                            ProjectExistentialBoxInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createProjectExistentialBox(
                                    getOpLocation(Inst->getLoc()),
                                    getOpType(Inst->getType()),
                                    getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitCondFailInst(CondFailInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createCondFail(getOpLocation(Inst->getLoc()),
                                        getOpValue(Inst->getOperand()),
                                        Inst->getMessage()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitIncrementProfilerCounterInst(
    IncrementProfilerCounterInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createIncrementProfilerCounter(
                              getOpLocation(Inst->getLoc()),
                              Inst->getCounterIndex(), Inst->getPGOFuncName(),
                              Inst->getNumCounters(), Inst->getPGOFuncHash()));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitTestSpecificationInst(
    TestSpecificationInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createTestSpecificationInst(
                                    getOpLocation(Inst->getLoc()),
                                    Inst->getArgumentsSpecification()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitIndexAddrInst(IndexAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createIndexAddr(getOpLocation(Inst->getLoc()),
                                         getOpValue(Inst->getBase()),
                                         getOpValue(Inst->getIndex()),
                                         Inst->needsStackProtection()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitTailAddrInst(TailAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createTailAddr(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getBase()),
                getOpValue(Inst->getIndex()), getOpType(Inst->getType())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitIndexRawPointerInst(IndexRawPointerInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createIndexRawPointer(getOpLocation(Inst->getLoc()),
                                               getOpValue(Inst->getBase()),
                                               getOpValue(Inst->getIndex())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitUnreachableInst(UnreachableInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUnreachable(getOpLocation(Inst->getLoc())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitReturnInst(ReturnInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createReturn(getOpLocation(Inst->getLoc()),
                                      getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitThrowInst(ThrowInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createThrow(getOpLocation(Inst->getLoc()),
                                     getOpValue(Inst->getOperand())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitUnwindInst(UnwindInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createUnwind(getOpLocation(Inst->getLoc())));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitYieldInst(YieldInst *Inst) {
  auto Values = getOpValueArray<8>(Inst->getYieldedValues());
  auto ResumeBB = getOpBasicBlock(Inst->getResumeBB());
  auto UnwindBB = getOpBasicBlock(Inst->getUnwindBB());

  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createYield(getOpLocation(Inst->getLoc()), Values,
                                     ResumeBB, UnwindBB));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitBranchInst(BranchInst *Inst) {
  auto Args = getOpValueArray<8>(Inst->getArgs());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createBranch(
                                    getOpLocation(Inst->getLoc()),
                                    getOpBasicBlock(Inst->getDestBB()), Args));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitCondBranchInst(CondBranchInst *Inst) {
  auto TrueArgs = getOpValueArray<8>(Inst->getTrueArgs());
  auto FalseArgs = getOpValueArray<8>(Inst->getFalseArgs());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createCondBranch(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getCondition()),
                getOpBasicBlock(Inst->getTrueBB()), TrueArgs,
                getOpBasicBlock(Inst->getFalseBB()), FalseArgs,
                Inst->getTrueBBCount(), Inst->getFalseBBCount()));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitCheckedCastBranchInst(CheckedCastBranchInst *Inst) {
  SILBasicBlock *OpSuccBB = getOpBasicBlock(Inst->getSuccessBB());
  SILBasicBlock *OpFailBB = getOpBasicBlock(Inst->getFailureBB());
  auto TrueCount = Inst->getTrueBBCount();
  auto FalseCount = Inst->getFalseBBCount();
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createCheckedCastBranch(
                getOpLocation(Inst->getLoc()), Inst->isExact(),
                getOpValue(Inst->getOperand()),
                getOpType(Inst->getTargetLoweredType()),
                getOpASTType(Inst->getTargetFormalType()), OpSuccBB, OpFailBB,
                Inst->getForwardingOwnershipKind(), TrueCount, FalseCount));
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitCheckedCastAddrBranchInst(
                                             CheckedCastAddrBranchInst *Inst) {
  SILBasicBlock *OpSuccBB = getOpBasicBlock(Inst->getSuccessBB());
  SILBasicBlock *OpFailBB = getOpBasicBlock(Inst->getFailureBB());
  SILValue SrcValue = getOpValue(Inst->getSrc());
  SILValue DestValue = getOpValue(Inst->getDest());
  CanType SrcType = getOpASTType(Inst->getSourceFormalType());
  CanType TargetType = getOpASTType(Inst->getTargetFormalType());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto TrueCount = Inst->getTrueBBCount();
  auto FalseCount = Inst->getFalseBBCount();
  recordClonedInstruction(Inst, getBuilder().createCheckedCastAddrBranch(
                                    getOpLocation(Inst->getLoc()),
                                    Inst->getConsumptionKind(), SrcValue,
                                    SrcType, DestValue, TargetType, OpSuccBB,
                                    OpFailBB, TrueCount, FalseCount));
}
  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitSwitchValueInst(SwitchValueInst *Inst) {
  SILBasicBlock *DefaultBB = nullptr;
  if (Inst->hasDefault())
    DefaultBB = getOpBasicBlock(Inst->getDefaultBB());
  SmallVector<std::pair<SILValue, SILBasicBlock*>, 8> CaseBBs;
  for (int i = 0, e = Inst->getNumCases(); i != e; ++i)
    CaseBBs.push_back(std::make_pair(getOpValue(Inst->getCase(i).first),
                                     getOpBasicBlock(Inst->getCase(i).second)));
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSwitchValue(getOpLocation(Inst->getLoc()),
                                           getOpValue(Inst->getOperand()),
                                           DefaultBB, CaseBBs));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitSwitchEnumInst(SwitchEnumInst *Inst) {
  SILBasicBlock *DefaultBB = nullptr;
  if (Inst->hasDefault())
    DefaultBB = getOpBasicBlock(Inst->getDefaultBB());
  SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 8> CaseBBs;
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i)
    CaseBBs.push_back(std::make_pair(Inst->getCase(i).first,
                                     getOpBasicBlock(Inst->getCase(i).second)));
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSwitchEnum(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                DefaultBB, CaseBBs, llvm::None, ProfileCounter(),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::
visitSwitchEnumAddrInst(SwitchEnumAddrInst *Inst) {
  SILBasicBlock *DefaultBB = nullptr;
  if (Inst->hasDefault())
    DefaultBB = getOpBasicBlock(Inst->getDefaultBB());
  SmallVector<std::pair<EnumElementDecl*, SILBasicBlock*>, 8> CaseBBs;
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i)
    CaseBBs.push_back(std::make_pair(Inst->getCase(i).first,
                                     getOpBasicBlock(Inst->getCase(i).second)));
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSwitchEnumAddr(getOpLocation(Inst->getLoc()),
                                              getOpValue(Inst->getOperand()),
                                              DefaultBB, CaseBBs));
}
  

  
template<typename ImplClass>
void
SILCloner<ImplClass>::visitSelectEnumInst(SelectEnumInst *Inst) {
  SILValue DefaultResult;
  if (Inst->hasDefault())
    DefaultResult = getOpValue(Inst->getDefaultResult());
  SmallVector<std::pair<EnumElementDecl*, SILValue>, 8> CaseResults;
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i)
    CaseResults.push_back(std::make_pair(Inst->getCase(i).first,
                                         getOpValue(Inst->getCase(i).second)));
  
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createSelectEnum(
                getOpLocation(Inst->getLoc()),
                getOpValue(Inst->getEnumOperand()), getOpType(Inst->getType()),
                DefaultResult, CaseResults, llvm::None, ProfileCounter(),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void
SILCloner<ImplClass>::visitSelectEnumAddrInst(SelectEnumAddrInst *Inst) {
  SILValue DefaultResult;
  if (Inst->hasDefault())
    DefaultResult = getOpValue(Inst->getDefaultResult());
  SmallVector<std::pair<EnumElementDecl*, SILValue>, 8> CaseResults;
  for (unsigned i = 0, e = Inst->getNumCases(); i != e; ++i)
    CaseResults.push_back(std::make_pair(Inst->getCase(i).first,
                                         getOpValue(Inst->getCase(i).second)));
  
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createSelectEnumAddr(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getEnumOperand()),
                                    getOpType(Inst->getType()), DefaultResult,
                                    CaseResults));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDynamicMethodBranchInst(
    DynamicMethodBranchInst *Inst) {
  SILBasicBlock *OpHasMethodBB = getOpBasicBlock(Inst->getHasMethodBB());
  SILBasicBlock *OpHasNoMethodBB = getOpBasicBlock(Inst->getNoMethodBB());
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createDynamicMethodBranch(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                Inst->getMember(), OpHasMethodBB, OpHasNoMethodBB));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitProjectBlockStorageInst(
    ProjectBlockStorageInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createProjectBlockStorage(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitInitBlockStorageHeaderInst(
    InitBlockStorageHeaderInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst,
      getBuilder().createInitBlockStorageHeader(
          getOpLocation(Inst->getLoc()), getOpValue(Inst->getBlockStorage()),
          getOpValue(Inst->getInvokeFunction()), getOpType(Inst->getType()),
          getOpSubstitutionMap(Inst->getSubstitutions())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitObjCMetatypeToObjectInst(
    ObjCMetatypeToObjectInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst, getBuilder().createObjCMetatypeToObject(
                                    getOpLocation(Inst->getLoc()),
                                    getOpValue(Inst->getOperand()),
                                    getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitObjCExistentialMetatypeToObjectInst(
    ObjCExistentialMetatypeToObjectInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createObjCExistentialMetatypeToObject(
                getOpLocation(Inst->getLoc()), getOpValue(Inst->getOperand()),
                getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitObjCProtocolInst(ObjCProtocolInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createObjCProtocol(getOpLocation(Inst->getLoc()),
                                            Inst->getProtocol(),
                                            getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitKeyPathInst(KeyPathInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  SmallVector<SILValue, 4> opValues;
  for (auto &op : Inst->getAllOperands())
    opValues.push_back(getOpValue(op.get()));

  recordClonedInstruction(Inst,
                          getBuilder().createKeyPath(
                              getOpLocation(Inst->getLoc()), Inst->getPattern(),
                              getOpSubstitutionMap(Inst->getSubstitutions()),
                              opValues, getOpType(Inst->getType())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDifferentiableFunctionInst(
    DifferentiableFunctionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  llvm::Optional<std::pair<SILValue, SILValue>> derivativeFns = llvm::None;
  if (Inst->hasDerivativeFunctions())
    derivativeFns = std::make_pair(getOpValue(Inst->getJVPFunction()),
                                   getOpValue(Inst->getVJPFunction()));
  recordClonedInstruction(
      Inst, getBuilder().createDifferentiableFunction(
                getOpLocation(Inst->getLoc()), Inst->getParameterIndices(),
                Inst->getResultIndices(),
                getOpValue(Inst->getOriginalFunction()), derivativeFns,
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template<typename ImplClass>
void SILCloner<ImplClass>::visitLinearFunctionInst(LinearFunctionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  auto transpose = Inst->getOptionalTransposeFunction();
  if (transpose)
    transpose = getOpValue(*transpose);
  recordClonedInstruction(
      Inst,
      getBuilder().createLinearFunction(
          getOpLocation(Inst->getLoc()), Inst->getParameterIndices(),
          getOpValue(Inst->getOriginalFunction()),
          getBuilder().hasOwnership() ? Inst->getForwardingOwnershipKind()
                                      : ValueOwnershipKind(OwnershipKind::None),
          transpose));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDifferentiableFunctionExtractInst(
    DifferentiableFunctionExtractInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  llvm::Optional<SILType> explicitExtracteeType = llvm::None;
  if (Inst->hasExplicitExtracteeType())
    explicitExtracteeType = Inst->getType();
  recordClonedInstruction(
      Inst,
      getBuilder().createDifferentiableFunctionExtract(
          getOpLocation(Inst->getLoc()), Inst->getExtractee(),
          getOpValue(Inst->getOperand()),
          getBuilder().hasOwnership() ? Inst->getForwardingOwnershipKind()
                                      : ValueOwnershipKind(OwnershipKind::None),
          explicitExtracteeType));
}

template<typename ImplClass>
void SILCloner<ImplClass>::
visitLinearFunctionExtractInst(LinearFunctionExtractInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createLinearFunctionExtract(
                getOpLocation(Inst->getLoc()), Inst->getExtractee(),
                getOpValue(Inst->getOperand()),
                getBuilder().hasOwnership()
                    ? Inst->getForwardingOwnershipKind()
                    : ValueOwnershipKind(OwnershipKind::None)));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitDifferentiabilityWitnessFunctionInst(
    DifferentiabilityWitnessFunctionInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createDifferentiabilityWitnessFunction(
                              getOpLocation(Inst->getLoc()),
                              Inst->getWitnessKind(), Inst->getWitness()));
}

template <typename ImplClass>
void SILCloner<ImplClass>
::visitGetAsyncContinuationInst(GetAsyncContinuationInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createGetAsyncContinuation(
                            getOpLocation(Inst->getLoc()),
                            getOpASTType(Inst->getFormalResumeType()),
                            Inst->throws()));
}

template <typename ImplClass>
void SILCloner<ImplClass>
::visitGetAsyncContinuationAddrInst(GetAsyncContinuationAddrInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createGetAsyncContinuationAddr(
                            getOpLocation(Inst->getLoc()),
                            getOpValue(Inst->getOperand()),
                            getOpASTType(Inst->getFormalResumeType()),
                            Inst->throws()));
}

template <typename ImplClass>
void SILCloner<ImplClass>
::visitAwaitAsyncContinuationInst(AwaitAsyncContinuationInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createAwaitAsyncContinuation(
                            getOpLocation(Inst->getLoc()),
                            getOpValue(Inst->getOperand()),
                            getOpBasicBlock(Inst->getResumeBB()),
                            Inst->getErrorBB()
                              ? getOpBasicBlock(Inst->getErrorBB())
                              : nullptr));
}

template <typename ImplClass>
void SILCloner<ImplClass>
::visitHopToExecutorInst(HopToExecutorInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createHopToExecutor(
                            getOpLocation(Inst->getLoc()),
                            getOpValue(Inst->getTargetExecutor()),
                            Inst->isMandatory()));
}

template <typename ImplClass>
void SILCloner<ImplClass>
::visitExtractExecutorInst(ExtractExecutorInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(Inst,
                          getBuilder().createExtractExecutor(
                            getOpLocation(Inst->getLoc()),
                            getOpValue(Inst->getExpectedExecutor())));
}

template <typename ImplClass>
void SILCloner<ImplClass>::visitHasSymbolInst(HasSymbolInst *Inst) {
  getBuilder().setCurrentDebugScope(getOpScope(Inst->getDebugScope()));
  recordClonedInstruction(
      Inst, getBuilder().createHasSymbol(getOpLocation(Inst->getLoc()),
                                         Inst->getDecl()));
}

} // end namespace swift

#endif
