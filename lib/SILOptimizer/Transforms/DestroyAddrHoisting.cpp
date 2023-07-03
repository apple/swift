//===--- DestroyAddrHoisting.cpp - SSA-based destroy_addr hoisting --------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// This is a light-weight utility for hoisting destroy instructions for unique
/// storage--typically alloc_stack or owned incoming arguments. Shrinking an
/// object's memory lifetime can allow removal of copy_addr and other
/// optimization.
///
/// This algorithm is:
/// - Incremental
/// - SSA-based
/// - Canonical
/// - Free from alias analysis
///
/// Incremental: Handle a single in-memory value at a time. The value's address
/// typically originates from an alloc_stack or owned function argument
/// (@in). It does not depend on any analysis result, which would need to be
/// preserved by a pass.
///
/// SSA-based: Starting with uniquely identified (exclusive) storage,
/// discovers all known uses based on recognizable SIL patterns. Bails-out on
/// unknown uses. Derivation of a raw pointer is considered a "known use".
///
/// Canonical: Assumes that aggregate values, which are allocated in a single
/// operation, are also destroyed in a single operation. This canonical form is
/// not fully enforced, so violations result in a bail-out.
///
/// Free from alias analysis: this only handles exclusively identified
/// addresses to owned values, which cannot be derived from object references.
///
/// ----------------------------------------------------------------------------
///
/// DestroyAddr hoisting stops at either a direct use, or a deinitialization
/// barrier. Direct uses are checked by guaranteeing that all storage uses are
/// known.
///
/// Deinitialization barriers:
///
/// Case #1. Weak reference loads: Any load of a weak or unowned referenceto an
/// object that may be deallocated when this variable is destroyed. Any use of
/// the weak reference is considered a barrier, even if the referenced object is
/// not accessed. This only applies to loads within the current lexical
/// scope. Programmers must properly check escaping weak references for null.
///
/// Case #2. Derived pointers: Any memory access based on a raw pointer to
/// memory that may be deallocated when this variable is destroyed. This only
/// applies to pointer access within this variable's lexical scope. Programmers
/// must manage escaping pointers explicitly via Builtin.fixLifetime.
///
/// Case #3. Synchronization points: If the object potentially has a custom
/// deinitializer with side effects, then any external function call, which may
/// contain a memory barrier or system call, prevents hoisting. If the external
/// function call is annotated as "read-only", then it is safe. Since Swift does
/// not directly support atomics, no SIL instructions are currently considered
/// synchronization points.
///
/// ----------------------------------------------------------------------------
///
/// TODO: replace the destroy hoisting in CopyForwarding::forwardCopiesOf and
/// ensure related tests still pass. This requires hoisting over certain
/// calls. We can do this as long as the call takes a copy of the storage value
/// as an argument. The copy will be guarded by the callee's lexical scope, so
/// the deinits cannot be invoked by the hoisted destroy (in fact it should be
/// possible to eliminate the destroy).
///
/// TODO: As a utility, hoistDestroys should be repeatable. Subsequent runs
/// without changing input should have no effect, including putting new
/// instructions on a worklist. MergeDestroys currently breaks this because the
/// destroys are inserted first before they are merged. This will trigger the
/// createdNewInst callback and cause hadCallbackInvocation() to return true
/// even when the merged result is identical to the input. Fix this by keeping
/// track of the newly created destroys, defer calling createdNewInst, and defer
/// deleting dead instructions. When merging, check if the merged destroy is
/// inserted at the old destroy to reuse it and bypass triggering callbacks.
///
/// TODO: enforce an invariant that destroy_addrs jointly post-dominate any
/// exclusive owned address, that would simplify the algorithm.
///
/// ===--------------------------------------------------------------------===//

#define DEBUG_TYPE "destroy-addr-hoisting"

#include "swift/AST/Type.h"
#include "swift/Basic/GraphNodeWorklist.h"
#include "swift/Basic/SmallPtrSetVector.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILBasicBlock.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/Reachability.h"
#include "swift/SILOptimizer/Analysis/VisitBarrierAccessScopes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/InstructionDeleter.h"

using namespace swift;

namespace {

/// Step #1: Find all known uses of the unique storage object.
struct KnownStorageUses : UniqueStorageUseVisitor {
  bool preserveDebugInfo;

  SmallPtrSet<SILInstruction *, 16> storageUsers;
  SmallSetVector<SILInstruction *, 4> originalDestroys;
  SmallPtrSet<SILInstruction *, 4> debugInsts;

  KnownStorageUses(AccessStorage storage, SILFunction *function)
      : UniqueStorageUseVisitor(storage, function),
        preserveDebugInfo(function->preserveDebugInfo()) {}

  bool empty() const {
    return storageUsers.empty() && originalDestroys.empty() &&
           debugInsts.empty();
  }

  SILFunction *getFunction() const { return function; }

  AccessStorage getStorage() const { return storage; }

  // Return true if all leaf users of the root address are recognized.
  //
  // Populate addressUsers, originalDestroys, and debugInsts.
  bool findUses() {
    assert(empty() && "already initialized");

    return UniqueStorageUseVisitor::findUses(*this);
  }

protected:
  KnownStorageUses(KnownStorageUses const &) = delete;
  KnownStorageUses &operator=(KnownStorageUses const &) = delete;

  bool recordUser(SILInstruction *user) {
    storageUsers.insert(user);
    return true;
  }

  bool visitBeginAccess(Operand *use) override {
    auto *bai = cast<BeginAccessInst>(use->getUser());
    for (auto *eai : bai->getEndAccesses()) {
      storageUsers.insert(eai);
    }
    return true;
  }

  bool visitLoad(Operand *use) override { return recordUser(use->getUser()); }

  bool visitStore(Operand *use) override { return recordUser(use->getUser()); }

  bool visitDestroy(Operand *use) override {
    originalDestroys.insert(use->getUser());
    return true;
  }

  bool visitDealloc(Operand *use) override { return true; }

  bool visitDebugUse(Operand *use) override {
    if (preserveDebugInfo) {
      storageUsers.insert(use->getUser());
    } else {
      debugInsts.insert(use->getUser());
    }
    return true;
  }

  bool visitUnknownUse(Operand *use) override {
    auto *user = use->getUser();
    if (isa<BuiltinRawPointerType>(use->get()->getType().getASTType())) {
      // Destroy hoisting considers address_to_pointer to be a leaf use because
      // any potential pointer access is already considered to be a
      // deinitialization barrier.  Consequently, any instruction that uses a
      // value produced by address_to_pointer isn't regarded as a storage use.
      return true;
    }
    LLVM_DEBUG(llvm::dbgs() << "Unknown user " << *user);
    return false;
  }
};

class DestroyReachability;

/// Step #2: Perform backward dataflow from KnownStorageUses.originalDestroys to
/// KnownStorageUses.storageUsers to find deinitialization barriers.
class DeinitBarriers final {
  BasicCalleeAnalysis *calleeAnalysis;

public:
  // Instructions beyond which a destroy_addr cannot be hoisted, reachable from
  // a destroy_addr.  Deinit barriers or storage uses.
  SmallSetVector<SILInstruction *, 4> barrierInstructions;

  // Phis beyond which a destroy_addr cannot be hoisted, reachable from a
  // destroy_addr.
  SmallSetVector<SILBasicBlock *, 4> barrierPhis;

  // Blocks beyond the end of which a destroy_addr cannot be hoisted.
  SmallSetVector<SILBasicBlock *, 4> barrierBlocks;

  // Debug instructions that are no longer within this lifetime after shrinking.
  SmallSetVector<SILInstruction *, 4> deadUsers;

  // The access scopes which are hoisting barriers.
  //
  // They are hoisting barriers if they include any barriers.  We need to be
  // sure not to hoist a destroy_addr into an access scope and by doing so cause
  // a deinit which had previously executed outside an access scope to start
  // executing within it--that could violate exclusivity.
  SmallPtrSet<BeginAccessInst *, 8> barrierAccessScopes;

  explicit DeinitBarriers(bool ignoreDeinitBarriers,
                          const KnownStorageUses &knownUses,
                          SILFunction *function,
                          BasicCalleeAnalysis *calleeAnalysis)
      : calleeAnalysis(calleeAnalysis),
        ignoreDeinitBarriers(ignoreDeinitBarriers), knownUses(knownUses) {
    auto rootValue = knownUses.getStorage().getRoot();
    assert(rootValue && "HoistDestroys requires a single storage root");
    // null for function args
    storageDefInst = rootValue->getDefiningInstruction();
  }

  void compute() { DestroyReachability(*this).solve(); }

  bool isBarrier(SILInstruction *instruction) const {
    return classificationIsBarrier(classifyInstruction(instruction));
  };

  friend class DestroyReachability;

private:
  DeinitBarriers(DeinitBarriers const &) = delete;
  DeinitBarriers &operator=(DeinitBarriers const &) = delete;

  bool ignoreDeinitBarriers;
  const KnownStorageUses &knownUses;
  SILInstruction *storageDefInst = nullptr;

  enum class Classification { DeadUser, Barrier, Other };

  Classification classifyInstruction(SILInstruction *inst) const;

  static bool classificationIsBarrier(Classification classification);

  /// Operates backward reachability and access scope visitor.  Implements the
  /// interfaces involved.
  ///
  /// Implements IterativeBackwardReachability::findBarriers::Visitor
  /// Implements VisitBarrierAccessScopes::Visitor
  /// Implements IterativeBackwardReachability::Effects
  /// Implements VisitBarrierAccessScopes::Effects
  class DestroyReachability final {
    using Dataflow = IterativeBackwardReachability<DestroyReachability>;
    using Effect = Dataflow::Effect;
    using ScopeVisitor =
        VisitBarrierAccessScopes<DestroyReachability, DestroyReachability>;

    DeinitBarriers &result;
    Dataflow::Result reachability;
    Dataflow dataflow;
    llvm::Optional<SmallVector<SILBasicBlock *, 16>> cachedRoots;
    bool recordDeadUsers = false;

  public:
    DestroyReachability(DeinitBarriers &result)
        : result(result), reachability(result.knownUses.getFunction()),
          dataflow(Dataflow::untilInitialBlock(
              result.knownUses.getFunction(),
              result.storageDefInst ? result.storageDefInst->getParent()
                                    : nullptr,
              *this, reachability)) {}

    void solve();

  private:
    friend Dataflow;
    friend ScopeVisitor;

    /// IterativeBackwardReachability::Effects
    /// VisitBarrierAccessScopes::Effects

    auto gens() { return result.knownUses.originalDestroys; }

    Effect effectForInstruction(SILInstruction *instruction);

    Effect effectForPhi(SILBasicBlock *block);

    /// VisitBarrierAccessScopes::Effects

    bool isLocalGen(SILInstruction *instruction) {
      return reachability.localGens.contains(instruction);
    }

    auto localGens() { return reachability.localGens; }

    /// IterativeBackwardReachability::findBarriers::Visitor:

    void visitBarrierInstruction(SILInstruction *instruction) {
      result.barrierInstructions.insert(instruction);
    }

    void visitBarrierPhi(SILBasicBlock *block) {
      result.barrierPhis.insert(block);
    }

    void visitBarrierBlock(SILBasicBlock *block) {
      result.barrierBlocks.insert(block);
    }

    /// VisitBarrierAccessScopes::Visitor

    ArrayRef<SILBasicBlock *> roots();

    bool isInRegion(SILBasicBlock *block) {
      return reachability.discoveredBlocks.contains(block);
    }

    void visitBarrierAccessScope(BeginAccessInst *bai) {
      result.barrierAccessScopes.insert(bai);
      for (auto *eai : bai->getEndAccesses()) {
        dataflow.addKill(eai);
      }
    }
  };
};

DeinitBarriers::Classification
DeinitBarriers::classifyInstruction(SILInstruction *inst) const {
  if (knownUses.debugInsts.contains(inst)) {
    return Classification::DeadUser;
  }
  if (inst == storageDefInst) {
    return Classification::Barrier;
  }
  if (knownUses.storageUsers.contains(inst)) {
    return Classification::Barrier;
  }
  if (!ignoreDeinitBarriers && isDeinitBarrier(inst, calleeAnalysis)) {
    return Classification::Barrier;
  }
  if (auto *eai = dyn_cast<EndAccessInst>(inst)) {
    if (barrierAccessScopes.contains(eai->getBeginAccess())) {
      return Classification::Barrier;
    }
  }
  return Classification::Other;
}

bool DeinitBarriers::classificationIsBarrier(Classification classification) {
  switch (classification) {
  case Classification::DeadUser:
  case Classification::Other:
    return false;
  case Classification::Barrier:
    return true;
  }
  llvm_unreachable("exhaustive switch is not exhaustive?!");
}

DeinitBarriers::DestroyReachability::Effect
DeinitBarriers::DestroyReachability::effectForInstruction(
    SILInstruction *instruction) {
  if (result.knownUses.originalDestroys.contains(instruction))
    return Effect::Gen();
  auto classification = result.classifyInstruction(instruction);
  if (recordDeadUsers && classification == Classification::DeadUser)
    result.deadUsers.insert(instruction);
  return result.classificationIsBarrier(classification) ? Effect::Kill()
                                                        : Effect::NoEffect();
}

DeinitBarriers::DestroyReachability::Effect
DeinitBarriers::DestroyReachability::effectForPhi(SILBasicBlock *block) {
  bool isBarrier =
      llvm::any_of(block->getPredecessorBlocks(), [&](auto *predecessor) {
        return result.isBarrier(predecessor->getTerminator());
      });
  return isBarrier ? Effect::Kill() : Effect::NoEffect();
}

void DeinitBarriers::DestroyReachability::solve() {
  dataflow.initialize();
  ScopeVisitor visitor(result.knownUses.getFunction(), *this, *this);
  visitor.visit();
  dataflow.solve();
  recordDeadUsers = true;
  dataflow.findBarriers(*this);
}

/// Algorithm for hoisting the destroys of a single uniquely identified storage
/// object.
class HoistDestroys {
  SILValue storageRoot;
  SILFunction *function;
  SILModule &module;
  TypeExpansionContext typeExpansionContext;
  bool ignoreDeinitBarriers;
  SmallPtrSetImpl<SILInstruction *> &remainingDestroyAddrs;
  InstructionDeleter &deleter;
  BasicCalleeAnalysis *calleeAnalysis;

  // Book-keeping for the rewriting stage.
  SmallPtrSet<SILInstruction *, 4> reusedDestroys;

  BasicBlockSetVector destroyMergeBlocks;

public:
  HoistDestroys(SILValue storageRoot, bool ignoreDeinitBarriers,
                SmallPtrSetImpl<SILInstruction *> &remainingDestroyAddrs,
                InstructionDeleter &deleter,
                BasicCalleeAnalysis *calleeAnalysis)
      : storageRoot(storageRoot), function(storageRoot->getFunction()),
        module(function->getModule()), typeExpansionContext(*function),
        ignoreDeinitBarriers(ignoreDeinitBarriers),
        remainingDestroyAddrs(remainingDestroyAddrs), deleter(deleter),
        calleeAnalysis(calleeAnalysis), destroyMergeBlocks(getFunction()) {}

  bool perform();

protected:
  SILFunction *getFunction() const { return storageRoot->getFunction(); }

  bool foldBarrier(SILInstruction *barrier, const AccessStorage &storage,
                   const DeinitBarriers &deinitBarriers);

  bool foldBarrier(SILInstruction *barrier, const AccessStorage &storage,
                   const KnownStorageUses &knownUses,
                   const DeinitBarriers &deinitBarriers);

  bool checkFoldingBarrier(SILInstruction *instruction,
                           SmallVectorImpl<LoadInst *> &loads,
                           SmallVectorImpl<CopyAddrInst *> &copies,
                           SmallPtrSetImpl<AccessPath::PathNode> &leaves,
                           SmallPtrSetImpl<AccessPath::PathNode> &trivialLeaves,
                           const AccessStorage &storage,
                           const DeinitBarriers &deinitBarriers);

  void insertDestroy(SILInstruction *barrier, SILInstruction *insertBefore,
                     const KnownStorageUses &knownUses);

  void createDestroy(SILInstruction *insertBefore, const SILDebugScope *scope);

  void createSuccessorDestroys(SILBasicBlock *barrierBlock);

  bool rewriteDestroys(const AccessStorage &storage,
                       const KnownStorageUses &knownUses,
                       const DeinitBarriers &deinitBarriers);

  void mergeDestroys(SILBasicBlock *mergeBlock);
};

} // namespace

bool HoistDestroys::perform() {
  auto storage = AccessStorage::computeInScope(storageRoot);
  if (!storage.isUniquelyIdentified() &&
      storage.getKind() != AccessStorage::Kind::Nested)
    return false;

  KnownStorageUses knownUses(storage, getFunction());
  if (!knownUses.findUses())
    return false;

  DeinitBarriers deinitBarriers(ignoreDeinitBarriers, knownUses, getFunction(),
                                calleeAnalysis);
  deinitBarriers.compute();

  // No SIL changes happen before rewriting.
  return rewriteDestroys(storage, knownUses, deinitBarriers);
}

bool HoistDestroys::rewriteDestroys(const AccessStorage &storage,
                                    const KnownStorageUses &knownUses,
                                    const DeinitBarriers &deinitBarriers) {
  // Place a new destroy after each barrier instruction.
  for (SILInstruction *barrier : deinitBarriers.barrierInstructions) {
    auto *barrierBlock = barrier->getParent();
    if (barrier != barrierBlock->getTerminator()) {
      if (!foldBarrier(barrier, storage, knownUses, deinitBarriers))
        insertDestroy(barrier, barrier->getNextInstruction(), knownUses);
      continue;
    }
    for (auto *successor : barrierBlock->getSuccessorBlocks()) {
      insertDestroy(barrier, &successor->front(), knownUses);
    }
  }
  // Place a new destroy at each CFG edge in which the successor's beginning is
  // reached but the predecessors end is not reached.
  for (auto *block : deinitBarriers.barrierPhis) {
    // The destroy does not reach above the block's phi.
    insertDestroy(nullptr, &block->front(), knownUses);
  }
  for (auto *block : deinitBarriers.barrierBlocks) {
    // The destroy does not reach the end of any predecessors.
    insertDestroy(nullptr, &block->front(), knownUses);
  }
  // Delete dead users before merging destroys.
  for (auto *deadInst : deinitBarriers.deadUsers) {
    deleter.forceDelete(deadInst);
  }
  for (auto *destroyInst : knownUses.originalDestroys) {
    if (reusedDestroys.contains(destroyInst))
      continue;

    remainingDestroyAddrs.erase(destroyInst);
    deleter.forceDelete(destroyInst);
  }
  deleter.cleanupDeadInstructions();

  for (auto *mergeBlock : destroyMergeBlocks) {
    mergeDestroys(mergeBlock);
  }
  return deleter.hadCallbackInvocation();
}

/// Try to fold the destroy_addr with the specified barrier, or a backwards
/// sequence of instructions that it begins.
///
/// Do the following kinds of folds:
///
/// - loads:
///   given: load [copy] %addr
///          destroy_addr %addr
///   yield: load [take]
/// - copy_addrs:
///   given: copy_addr %addr to ...
///          destroy_addr %addr
///   yield: copy_addr [take] %addr
///
/// Additionally, generalize this to subobjects.  If there is a sequence of
/// copy_addrs and loads that covers all the subobjects of %addr.  Given
/// projections %subobject_1 and %subobject_2 out of %addr which fully cover all
/// the non-trivial fields of the recursive type-tree of %addr, fold
///
///     load [copy] %subobject_1
///     copy_addr %subobject_2 to ...
///     destroy_addr %addr
///
/// into
///
///     load [take] %subobject_1
///     copy_addr [take] %subobject_2 to ...
///
/// so long as all the loads and copy_addrs occur within the same block.
bool HoistDestroys::foldBarrier(SILInstruction *barrier,
                                const AccessStorage &storage,
                                const DeinitBarriers &deinitBarriers) {

  // The load [copy]s which will be folded into load [take]s if folding is
  // possible.
  llvm::SmallVector<LoadInst *, 4> loads;
  // The copy_addrs which will be folded into copy_addr [take]s if folding is
  // possible.
  llvm::SmallVector<CopyAddrInst *, 4> copies;

  // The non-trivial storage leaves of the root storage all of which must be
  // destroyed exactly once in the sequence of instructions prior to the
  // destroy_addr in order for folding to occur.
  llvm::SmallPtrSet<AccessPath::PathNode, 16> leaves;

  // The trivial storage leaves of the root storage.  They needn't be destroyed
  // in the sequence prior to the destroy_addr, but their uses may obstruct
  // folding.  For example,  given an %object and %triv a trivial subobject
  //
  //     load [copy] %object
  //     load [trivial] %triv
  //     destroy_addr %object
  //
  // it isn't legal to fold the destroy_addr into the load of %object like
  //
  //     load [take] %object
  //     load [trivial] %triv
  //
  // because the memory location %triv is no longer valid.  In general, it would
  // be fine to support folding over accesses of trivial subobjects so long as
  // they occur prior to the access to some nontrivial subobject that contains
  // it.
  SmallPtrSet<AccessPath::PathNode, 16> trivialLeaves;

  visitProductLeafAccessPathNodes(storageRoot, typeExpansionContext, module,
                                  [&](AccessPath::PathNode node, SILType ty) {
                                    if (ty.isTrivial(*function))
                                      return;
                                    leaves.insert(node);
                                  });

  for (auto *instruction = barrier; instruction != nullptr;
       instruction = instruction->getPreviousInstruction()) {
    if (checkFoldingBarrier(instruction, loads, copies, leaves, trivialLeaves,
                            storage, deinitBarriers))
      return false;

    // If we have load [copy]s or copy_addrs of projections out of the root
    // storage that cover all non-trivial product leaves, then we can fold!
    //
    // Stop looking for instructions to fold.
    if (leaves.empty())
      break;
  }

  if (!leaves.empty())
    return false;

  for (auto *load : loads) {
    assert(load->getOwnershipQualifier() == LoadOwnershipQualifier::Copy);
    load->setOwnershipQualifier(LoadOwnershipQualifier::Take);
  }
  for (auto *copy : copies) {
    assert(!copy->isTakeOfSrc());
    copy->setIsTakeOfSrc(IsTake);
  }

  return true;
}

/// Whether the specified instruction is a barrier to folding.
///
/// TODO: This is a bit more conservative that it needs to be in a couple of
/// ways:
///
/// (1) even if we've already seen a leaf, we could still fold, in certain
/// cases, we should be able to fold anyway.  For example, given projections
/// %p1 and %p2 of some root storage %a, in the following scenario:
///
///    %p1 = <PROJECT> %a
///    %p2 = <PROJECT> %a
///    %v1 = load [copy] %p1
///    %v2_1 = load [copy] %p2
///    %v2_1 = load [copy] %p2
///    destroy_addr %a
///
/// we could fold destroy_addr %a into the first load [copy] %p2 and the
/// load [copy] %p1:
///
///     %v1 = load [take] %p1
///     %v2_1 = load [copy] %p2
///     %v2_2 = load [take] %p1
///
/// And indeed we can do that for loads from a subprojection %p2_sub of
/// %p2; the following
///
///     %v1 = load [copy] %p1
///     %v2_sub = load [copy] %p2_sub
///     %v2 = load [copy] %p2
///
/// could be folded to
///
///     %v1 = load [take] %p1
///     %v2_sub = load [copy] %p2_sub
///     %v2 = load [take] %p2
///
/// (2) We should be able to continue folding over a load [trivial] so long as
/// the instructions that we're folding with don't destroy an aggregate that
/// contains the projection which is the target of the load [trivial].  For
/// example, given
///
///     %addr = alloc_stack %(X, I)
///     %x_addr = tuple_element_addr %addr : $*(X, I), 0
///     %i_addr = tuple_element_addr %addr : $*(X, I), 1
///     %x = load [copy] %x_addr : $*X
///     %i = load [trivial] %i_addr : $*I
///     destroy_addr %addr
///
/// we should be able to fold the destroy_addr of the tuple with the load [copy]
/// and ignore the load [trivial].
///
/// Doing this is complicated by the fact that we can't ignore the load
/// [trivial] if the load [copy] is of the whole tuple.  If we have instead
///
///     %addr = alloc_stack %(X, I)
///     %x_addr = tuple_element_addr %addr : $*(X, I), 0
///     %i_addr = tuple_element_addr %addr : $*(X, I), 1
///     %x = load [copy] %addr : $*(X, I)
///     %i = load [trivial] %i_addr : $*I
///     destroy_addr %addr
///
/// then we cannot fold.  If we did, we would end up with invalid SIL:
///
///     %x = load [take] %addr
///     %i = load [trivial] %i_addr
bool HoistDestroys::checkFoldingBarrier(
    SILInstruction *instruction, SmallVectorImpl<LoadInst *> &loads,
    SmallVectorImpl<CopyAddrInst *> &copies,
    SmallPtrSetImpl<AccessPath::PathNode> &leaves,
    SmallPtrSetImpl<AccessPath::PathNode> &trivialLeaves,
    const AccessStorage &storage, const DeinitBarriers &deinitBarriers) {
  // The address of a projection out of the root storage which would be
  // folded if folding is possible.
  //
  // If no such address is found, we need to check whether the instruction
  // is a barrier.
  SILValue address;
  if (auto *load = dyn_cast<LoadInst>(instruction)) {
    auto loadee = load->getOperand();
    auto relativeAccessStorage = RelativeAccessStorageWithBase::compute(loadee);
    if (relativeAccessStorage.getStorage().hasIdenticalStorage(storage)) {
      // If the access path from the loaded address to its root storage involves
      // a (layout non-equivalent) typecast--a load [take] of the casted address
      // would not be equivalent to a load [copy] followed by a destroy_addr of
      // the corresponding uncast projection--the truncated portion might have
      // refcounted components.
      if (relativeAccessStorage.cast == AccessStorageCast::Type)
        return true;
      if (load->getOwnershipQualifier() == LoadOwnershipQualifier::Copy) {
        address = loadee;
        loads.push_back(load);
      } else {
        assert(loadee->getType().isTrivial(*load->getFunction()));
        return true;
      }
    }
  } else if (auto *copy = dyn_cast<CopyAddrInst>(instruction)) {
    auto source = copy->getSrc();
    auto relativeAccessStorage = RelativeAccessStorageWithBase::compute(source);
    if (relativeAccessStorage.getStorage().hasIdenticalStorage(storage)) {
      // If the access path from the copy_addr'd address to its root storage
      // involves a (layout non-equivalent) typecast--a copy_addr [take] of the
      // casted address would not be equivalent to a copy_addr followed by a
      // destroy_addr of the corresponding uncast projection--the truncated
      // portion might have refcounted components.
      if (relativeAccessStorage.cast == AccessStorageCast::Type)
        return true;
      address = source;
      copies.push_back(copy);
    }
  }
  if (address) {
    // We found a relevant instruction that is operating on a projection out
    // of the root storage which would be folded if folding were possible.
    // Find its nontrivial product leaves and remove them from the set of
    // leaves of the root storage which we're wating to see.
    bool alreadySawLeaf = false;
    bool alreadySawTrivialSubleaf = false;
    visitProductLeafAccessPathNodes(
        address, typeExpansionContext, module,
        [&](AccessPath::PathNode node, SILType ty) {
          if (ty.isTrivial(*function)) {
            bool inserted = !trivialLeaves.insert(node).second;
            alreadySawTrivialSubleaf = alreadySawTrivialSubleaf || inserted;
            return;
          }
          bool erased = leaves.erase(node);
          alreadySawLeaf = alreadySawLeaf || !erased;
        });
    if (alreadySawLeaf) {
      // We saw this non-trivial product leaf already.  That means there are
      // multiple load [copy]s or copy_addrs of at least one product leaf
      // before (walking backwards from the hoisting point) there are
      // instructions that load or copy from all the non-trivial leaves.
      // Give up on folding.
      return true;
    }
    if (alreadySawTrivialSubleaf) {
      // We saw this trivial leaf already.  That means there was some later
      // load [copy] or copy_addr of it.  Give up on folding.
      return true;
    }
  } else if (deinitBarriers.isBarrier(instruction)) {
    // We didn't find an instruction that was both
    // - relevant (i.e. a copy_addr or a load [take])
    // - operating on a projection of the root storage
    // Additionally:
    // - we can't ignore whether it's a barrier
    // - and it IS a barrier.
    // We can't fold.
    return true;
  }
  return false;
}

bool HoistDestroys::foldBarrier(SILInstruction *barrier,
                                const AccessStorage &storage,
                                const KnownStorageUses &knownUses,
                                const DeinitBarriers &deinitBarriers) {
  if (auto *eai = dyn_cast<EndAccessInst>(barrier)) {
    auto *bai = eai->getBeginAccess();
    // Don't hoist a destroy into an unrelated access scope.
    if (stripAccessMarkers(bai) != stripAccessMarkers(storageRoot))
      return false;
    SILInstruction *instruction = eai;
    while ((instruction = instruction->getPreviousInstruction())) {
      if (instruction == bai)
        return false;
      if (foldBarrier(instruction, storage, deinitBarriers))
        return true;
      if (deinitBarriers.isBarrier(instruction))
        return false;
    }
  }
  return foldBarrier(barrier, storage, deinitBarriers);
}

// \p barrier may be null if the destroy is at function entry.
void HoistDestroys::insertDestroy(SILInstruction *barrier,
                                  SILInstruction *insertBefore,
                                  const KnownStorageUses &knownUses) {
  if (auto *branch = dyn_cast<BranchInst>(insertBefore)) {
    destroyMergeBlocks.insert(branch->getDestBB());
  }
  // Avoid mutating SIL for no reason. This could lead to infinite loops.
  if (isa<DestroyAddrInst>(insertBefore) ||
      isa<DestroyValueInst>(insertBefore)) {
    if (llvm::find(knownUses.originalDestroys, insertBefore) !=
        knownUses.originalDestroys.end()) {
      reusedDestroys.insert(insertBefore);
      return;
    }
  }
  const SILDebugScope *scope =
      barrier ? barrier->getDebugScope() : getFunction()->getDebugScope();
  createDestroy(insertBefore, scope);
}

void HoistDestroys::createDestroy(SILInstruction *insertBefore,
                                  const SILDebugScope *scope) {
  auto loc = RegularLocation::getAutoGeneratedLocation();
  SILInstruction *newDestroy;
  if (storageRoot->getType().isAddress()) {
    newDestroy =
        SILBuilder(insertBefore, scope).createDestroyAddr(loc, storageRoot);
  } else {
    newDestroy =
        SILBuilder(insertBefore, scope).createDestroyValue(loc, storageRoot);
  }
  deleter.getCallbacks().createdNewInst(newDestroy);
}

void HoistDestroys::mergeDestroys(SILBasicBlock *mergeBlock) {
  SmallVector<SILInstruction *, 4> deadDestroys;
  for (auto *predecessors : mergeBlock->getPredecessorBlocks()) {
    auto *tailDestroy = predecessors->getTerminator()->getPreviousInstruction();
    if (!tailDestroy || (!isa<DestroyAddrInst>(tailDestroy) &&
                         !isa<DestroyValueInst>(tailDestroy))) {
      return;
    }
    if (tailDestroy->getOperand(0) != storageRoot)
      return;

    deadDestroys.push_back(tailDestroy);
  }
  if (deadDestroys.size() < 2) // ignore trivial fall-thru
    return;

  createDestroy(&mergeBlock->front(), deadDestroys[0]->getDebugScope());

  for (auto *deadDestroy : deadDestroys) {
    remainingDestroyAddrs.erase(deadDestroy);
    deleter.forceDelete(deadDestroy);
  }
}

// =============================================================================
// Top-Level API
// =============================================================================

bool hoistDestroys(SILValue root, bool ignoreDeinitBarriers,
                   SmallPtrSetImpl<SILInstruction *> &remainingDestroyAddrs,
                   InstructionDeleter &deleter,
                   BasicCalleeAnalysis *calleeAnalysis) {
  LLVM_DEBUG(llvm::dbgs() << "Performing destroy hoisting on " << root);

  // Don't canonicalize the lifetimes of addresses of move-only type.
  // According to language rules, they are fixed.
  if (root->getType().isMoveOnly())
    return false;

  SILFunction *function = root->getFunction();
  if (!function)
    return false;

  // The algorithm assumes no critical edges.
  assert(function->hasOwnership() && "requires OSSA");

  // If lexical lifetimes aren't enabled, then deinit barriers aren't respected.
  auto &module = function->getModule();
  auto enableLexicalLifetimes =
      module.getASTContext().SILOpts.supportsLexicalLifetimes(module);
  ignoreDeinitBarriers = ignoreDeinitBarriers || !enableLexicalLifetimes;

  return HoistDestroys(root, ignoreDeinitBarriers, remainingDestroyAddrs,
                       deleter, calleeAnalysis)
      .perform();
}

// =============================================================================
// Pipeline Pass
// =============================================================================

namespace {
class DestroyAddrHoisting : public swift::SILFunctionTransform {
  void run() override;
};
} // end anonymous namespace

// TODO: Handle alloc_box the same way, as long as the box doesn't escape.
//
// TODO: Handle address and boxes that are captured in no-escape closures.
void DestroyAddrHoisting::run() {
  if (!getFunction()->hasOwnership())
    return;

  InstructionDeleter deleter;
  bool changed = false;

  llvm::SmallVector<AllocStackInst *, 4> asis;
  llvm::SmallVector<BeginAccessInst *, 4> bais;
  llvm::SmallVector<StoreInst *, 4> sis;
  llvm::SmallVector<CopyAddrInst *, 4> cais;

  // Collect the instructions that we'll be transforming.
  for (auto &block : *getFunction()) {
    for (auto &inst : block) {
      if (auto *asi = dyn_cast<AllocStackInst>(&inst)) {
        asis.push_back(asi);
      } else if (auto *bai = dyn_cast<BeginAccessInst>(&inst)) {
        if (bai->getAccessKind() == SILAccessKind::Modify) {
          bais.push_back(bai);
        }
      } else if (auto *si = dyn_cast<StoreInst>(&inst)) {
        if (si->getOwnershipQualifier() == StoreOwnershipQualifier::Assign) {
          sis.push_back(si);
        }
      } else if (auto *cai = dyn_cast<CopyAddrInst>(&inst)) {
        if (cai->isInitializationOfDest() == IsNotInitialization) {
          cais.push_back(cai);
        }
      }
    }
  }

  // Before hoisting, expand all
  //
  //     store [assign]
  //
  // instructions into
  //
  //     destroy_addr
  //     store [init]
  //
  // sequences to create more destroy_addrs to hoist.
  //
  // Record the newly created destroy_addrs and the stores they were split off
  // of.  After hoisting, if they have not been hoisted away from the store
  // instruction, we will merge them back together.
  llvm::SmallVector<std::pair<DestroyAddrInst *, StoreInst *>, 8>
      splitDestroysAndStores;
  // The destroy_addrs that were created that have not been deleted.  Items are
  // erased from the set as the destroy_addrs are deleted.
  SmallPtrSet<SILInstruction *, 8> remainingDestroyAddrs;
  // The number of destroys that were split off of store [init]s and not
  // recombined.
  int splitDestroys = 0;
  for (auto *si : sis) {
    auto builder = SILBuilderWithScope(si);
    auto *dai = builder.createDestroyAddr(
        RegularLocation::getAutoGeneratedLocation(si->getLoc()),
        si->getOperand(1));
    si->setOwnershipQualifier(StoreOwnershipQualifier::Init);
    splitDestroysAndStores.push_back({dai, si});
    remainingDestroyAddrs.insert(dai);
    ++splitDestroys;
  }
  // Similarly, also expand each
  //
  //     copy_addr to
  //
  // instruction into
  //
  //     destroy_addr
  //     copy_addr to [init]
  //
  // sequences to create still more destroy_addrs to hoist.
  //
  // As above, record the newly created destroy_addrs and copy_addrs off of
  // which they were split.  After hoisting, we'll merge them back together when
  // possible.
  llvm::SmallVector<std::pair<DestroyAddrInst *, CopyAddrInst *>, 8>
      splitDestroysAndCopies;
  for (auto *cai : cais) {
    auto builder = SILBuilderWithScope(cai);
    auto *dai = builder.createDestroyAddr(
        RegularLocation::getAutoGeneratedLocation(cai->getLoc()),
        cai->getOperand(1));
    cai->setIsInitializationOfDest(IsInitialization);
    splitDestroysAndCopies.push_back({dai, cai});
    remainingDestroyAddrs.insert(dai);
    ++splitDestroys;
  }

  auto *calleeAnalysis = getAnalysis<BasicCalleeAnalysis>();

  // We assume that the function is in reverse post order so visiting the
  // blocks and pushing begin_access as we see them and then popping them off
  // the end will result in hoisting inner begin_access' destroy_addrs first.
  for (auto *bai : llvm::reverse(bais)) {
    changed |= hoistDestroys(bai, /*ignoreDeinitBarriers=*/true,
                             remainingDestroyAddrs, deleter, calleeAnalysis);
  }
  // Alloc stacks always enclose their accesses.
  for (auto *asi : asis) {
    changed |= hoistDestroys(asi,
                             /*ignoreDeinitBarriers=*/!asi->isLexical(),
                             remainingDestroyAddrs, deleter, calleeAnalysis);
  }
  // Arguments enclose everything.
  for (auto *uncastArg : getFunction()->getArguments()) {
    auto *arg = cast<SILFunctionArgument>(uncastArg);
    if (arg->getType().isAddress()) {
      auto convention = arg->getArgumentConvention();
      // This is equivalent to writing
      //
      //     convention == SILArgumentConvention::Indirect_Inout
      //
      // but communicates the rationale: in order to ignore deinit barriers, the
      // address must be exclusively accessed and be a modification.
      bool ignoredByConvention = convention.isInoutConvention() &&
                                 convention.isExclusiveIndirectParameter();
      auto lifetime = arg->getLifetime();
      bool ignoreDeinitBarriers = ignoredByConvention || lifetime.isEagerMove();
      changed |= hoistDestroys(arg, ignoreDeinitBarriers, remainingDestroyAddrs,
                               deleter, calleeAnalysis);
    }
  }

  for (auto pair : splitDestroysAndStores) {
    auto *dai = pair.first;
    if (!remainingDestroyAddrs.contains(dai))
      continue;
    auto *si = pair.second;
    if (dai->getNextInstruction() != si)
      continue;
    // No stores should have been rewritten during hoisting.  Their ownership
    // qualifiers were set to [init] when splitting off the destroy_addrs.
    assert(si->getOwnershipQualifier() == StoreOwnershipQualifier::Init);
    // If a newly created destroy_addr has not been hoisted from its previous
    // location, combine it back together with the store [init] which it was
    // split off from.
    deleter.forceDelete(dai);
    si->setOwnershipQualifier(StoreOwnershipQualifier::Assign);
    --splitDestroys;
  }
  for (auto pair : splitDestroysAndCopies) {
    auto *dai = pair.first;
    if (!remainingDestroyAddrs.contains(dai))
      continue;
    auto *cai = pair.second;
    if (dai->getNextInstruction() != cai)
      continue;
    assert(cai->isInitializationOfDest() == IsInitialization);
    deleter.forceDelete(dai);
    cai->setIsInitializationOfDest(IsNotInitialization);
    --splitDestroys;
  }
  // If there were any destroy_addrs split off of stores and not recombined
  // with them, then the function has changed.
  changed |= splitDestroys > 0;

  if (changed) {
    invalidateAnalysis(SILAnalysis::InvalidationKind::Instructions);
  }
}

SILTransform *swift::createDestroyAddrHoisting() {
  return new DestroyAddrHoisting();
}
