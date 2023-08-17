//===--- DefiniteInitialization.cpp - Perform definite init analysis ------===//
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

#define DEBUG_TYPE "definite-init"

#include "DIMemoryUseCollector.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Stmt.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/SIL/BasicBlockBits.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILValue.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/DistributedActor.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallBitVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"

using namespace swift;
using namespace ownership;

llvm::cl::opt<bool> TriggerUnreachableOnFailure(
    "sil-di-assert-on-failure", llvm::cl::init(false),
    llvm::cl::desc("After emitting a DI error, assert instead of continuing. "
                   "Meant for debugging ONLY!"),
    llvm::cl::Hidden);

template<typename ...ArgTypes>
static InFlightDiagnostic diagnose(SILModule &M, SILLocation loc,
                                   ArgTypes... args) {
  auto diag = M.getASTContext().Diags.diagnose(loc.getSourceLoc(),
                                               Diagnostic(args...));
  if (TriggerUnreachableOnFailure)
    llvm_unreachable("Triggering standard assertion failure routine");
  return diag;
}

/// Insert a CFG diamond at the position specified by the SILBuilder, with a
/// conditional branch based on "Cond".
///
/// This returns the true, false, and continuation block. The SILBuilder is left
/// at the start of the ContBB block.
static void InsertCFGDiamond(SILValue Cond, SILLocation Loc, SILBuilder &B,
                             SILBasicBlock *&TrueBB,
                             SILBasicBlock *&FalseBB,
                             SILBasicBlock *&ContBB) {
  SILBasicBlock *StartBB = B.getInsertionBB();
  
  // Start by splitting the current block.
  ContBB = StartBB->split(B.getInsertionPoint());

  TrueBB = StartBB->getParent()->createBasicBlock();
  TrueBB->getParent()->moveBlockBefore(TrueBB, ContBB->getIterator());
  B.setInsertionPoint(TrueBB);
  B.createBranch(Loc, ContBB);

  FalseBB = StartBB->getParent()->createBasicBlock();
  FalseBB->getParent()->moveBlockBefore(FalseBB, ContBB->getIterator());
  B.setInsertionPoint(FalseBB);
  B.createBranch(Loc, ContBB);

  // Now that we have our destinations, insert a conditional branch on the
  // condition.
  B.setInsertionPoint(StartBB);
  B.createCondBranch(Loc, Cond, TrueBB, FalseBB);

  B.setInsertionPoint(ContBB, ContBB->begin());
}


//===----------------------------------------------------------------------===//
// Per-Element Promotion Logic
//===----------------------------------------------------------------------===//

namespace {
enum class DIKind : uint8_t { No, Yes, Partial };
} // end anonymous namespace

/// This implements the lattice merge operation for 2 optional DIKinds.
static llvm::Optional<DIKind> mergeKinds(llvm::Optional<DIKind> OK1,
                                         llvm::Optional<DIKind> OK2) {
  // If OK1 is unset, ignore it.
  if (!OK1.has_value())
    return OK2;

  DIKind K1 = OK1.value();

  // If "this" is already partial, we won't learn anything.
  if (K1 == DIKind::Partial)
    return K1;

  // If OK2 is unset, take K1.
  if (!OK2.has_value())
    return K1;

  DIKind K2 = OK2.value();

  // If "K1" is yes, or no, then switch to partial if we find a different
  // answer.
  if (K1 != K2)
    return DIKind::Partial;

  // Otherwise, we're still consistently Yes or No.
  return K1;
}

namespace {
  /// AvailabilitySet - This class stores an array of lattice values for tuple
  /// elements being analyzed for liveness computations.  Each element is
  /// represented with two bits in a bitvector, allowing this to represent the
  /// lattice values corresponding to "Unknown" (bottom), "Live" or "Not Live",
  /// which are the middle elements of the lattice, and "Partial" which is the
  /// top element.
  class AvailabilitySet {
    // We store two bits per element, encoded in the following form:
    //   T,T -> Nothing/Unknown
    //   F,F -> No
    //   F,T -> Yes
    //   T,F -> Partial
    SmallBitVector Data;
  public:
    AvailabilitySet() {}
  
    AvailabilitySet(unsigned NumElts) { init(NumElts); }
    
    void init(unsigned NumElts) {
      Data.set();
      Data.resize(NumElts*2, true);
    }

    bool empty() const { return Data.empty(); }
    unsigned size() const { return Data.size()/2; }

    DIKind get(unsigned Elt) const {
      return getConditional(Elt).value();
    }

    llvm::Optional<DIKind> getConditional(unsigned Elt) const {
      bool V1 = Data[Elt*2], V2 = Data[Elt*2+1];
      if (V1 == V2)
        return V1 ? llvm::Optional<DIKind>(llvm::None) : DIKind::No;
      return V2 ? DIKind::Yes : DIKind::Partial;
    }

    void set(unsigned Elt, DIKind K) {
      switch (K) {
      case DIKind::No:      Data[Elt*2] = false; Data[Elt*2+1] = false; break;
      case DIKind::Yes:     Data[Elt*2] = false, Data[Elt*2+1] = true; break;
      case DIKind::Partial: Data[Elt*2] = true,  Data[Elt*2+1] = false; break;
      }
    }

    void set(unsigned Elt, llvm::Optional<DIKind> K) {
      if (!K.has_value())
        Data[Elt*2] = true, Data[Elt*2+1] = true;
      else
        set(Elt, K.value());
    }

    /// containsUnknownElements - Return true if there are any elements that are
    /// unknown.
    bool containsUnknownElements() const {
      // Check that we didn't get any unknown values.
      for (unsigned i = 0, e = size(); i != e; ++i)
        if (!getConditional(i).has_value())
          return true;
      return false;
    }

    bool isAll(DIKind K) const {
      for (unsigned i = 0, e = size(); i != e; ++i) {
        auto Elt = getConditional(i);
        if (!Elt.has_value() || Elt.value() != K)
          return false;
      }
      return true;
    }
    
    bool hasAny(DIKind K) const {
      for (unsigned i = 0, e = size(); i != e; ++i) {
        auto Elt = getConditional(i);
        if (Elt.has_value() && Elt.value() == K)
          return true;
      }
      return false;
    }
    
    bool isAllYes() const { return isAll(DIKind::Yes); }
    bool isAllNo() const { return isAll(DIKind::No); }
    
    /// changeUnsetElementsTo - If any elements of this availability set are not
    /// known yet, switch them to the specified value.
    void changeUnsetElementsTo(DIKind K) {
      for (unsigned i = 0, e = size(); i != e; ++i)
        if (!getConditional(i).has_value())
          set(i, K);
    }
    
    void mergeIn(const AvailabilitySet &RHS) {
      // Logically, this is an elementwise "this = merge(this, RHS)" operation,
      // using the lattice merge operation for each element.
      for (unsigned i = 0, e = size(); i != e; ++i)
        set(i, mergeKinds(getConditional(i), RHS.getConditional(i)));
    }

    void dump(llvm::raw_ostream &OS) const {
      OS << '(';
      for (unsigned i = 0, e = size(); i != e; ++i) {
        if (llvm::Optional<DIKind> Elt = getConditional(i)) {
          switch (Elt.value()) {
            case DIKind::No:      OS << 'n'; break;
            case DIKind::Yes:     OS << 'y'; break;
            case DIKind::Partial: OS << 'p'; break;
          }
        } else {
          OS << '.';
        }
      }
      OS << ')';
    }
  };
 
  LLVM_ATTRIBUTE_USED
  inline llvm::raw_ostream &operator<<(llvm::raw_ostream &OS,
                                       const AvailabilitySet &AS) {
    AS.dump(OS);
    return OS;
  }
} // end anonymous namespace


namespace {
  /// LiveOutBlockState - Keep track of information about blocks that have
  /// already been analyzed.  Since this is a global analysis, we need this to
  /// cache information about different paths through the CFG.
  struct LiveOutBlockState {
    /// Keep track of whether there is a Store, InOutUse, or Escape locally in
    /// this block.
    bool HasNonLoadUse : 1;

    /// Helper flag used during building the worklist for the dataflow analysis.
    bool isInWorkList : 1;

    /// Availability of elements within the block.
    /// Not "empty" for all blocks which have non-load uses or contain the
    /// definition of the memory object.
    AvailabilitySet LocalAvailability;

    /// The live out information of the block. This is the LocalAvailability
    /// plus the information merged-in from the predecessor blocks.
    AvailabilitySet OutAvailability;

    /// Keep track of blocks where the contents of the self box are stored to
    /// as a result of a successful self.init or super.init call.
    llvm::Optional<DIKind> LocalSelfInitialized;

    /// The live out information of the block. This is the LocalSelfInitialized
    /// plus the information merged-in from the predecessor blocks.
    llvm::Optional<DIKind> OutSelfInitialized;

    LiveOutBlockState() { init(0); }

    void init(unsigned NumElements) {
      HasNonLoadUse = false;
      isInWorkList = false;
      LocalAvailability.init(NumElements);
      OutAvailability.init(NumElements);
      LocalSelfInitialized = llvm::None;
      OutSelfInitialized = llvm::None;
    }

    /// Sets all unknown elements to not-available.
    void setUnknownToNotAvailable() {
      LocalAvailability.changeUnsetElementsTo(DIKind::No);
      OutAvailability.changeUnsetElementsTo(DIKind::No);
      if (!LocalSelfInitialized.has_value())
        LocalSelfInitialized = DIKind::No;
      if (!OutSelfInitialized.has_value())
        OutSelfInitialized = DIKind::No;
    }

    /// Transfer function for dataflow analysis.
    ///
    /// \param pred Value from a predecessor block
    /// \param out Current live-out
    /// \param local Value from current block, overrides predecessor
    /// \param result Out parameter
    ///
    /// \return True if the result was different from the live-out
    bool transferAvailability(const llvm::Optional<DIKind> pred,
                              const llvm::Optional<DIKind> out,
                              const llvm::Optional<DIKind> local,
                              llvm::Optional<DIKind> &result) {
      if (local.has_value()) {
        // A local availability overrides the incoming value.
        result = local;
      } else {
        result = mergeKinds(out, pred);
      }
      if (result.has_value() &&
          (!out.has_value() || result.value() != out.value())) {
        return true;
      }
      return false;
    }

    /// Merge the state from a predecessor block into the OutAvailability.
    /// Returns true if the live out set changed.
    bool mergeFromPred(const LiveOutBlockState &Pred) {
      bool changed = false;
      for (unsigned i = 0, e = OutAvailability.size(); i != e; ++i) {
        llvm::Optional<DIKind> result;
        if (transferAvailability(Pred.OutAvailability.getConditional(i),
                                 OutAvailability.getConditional(i),
                                 LocalAvailability.getConditional(i),
                                 result)) {
          changed = true;
          OutAvailability.set(i, result);
        }
      }

      llvm::Optional<DIKind> result;
      if (transferAvailability(Pred.OutSelfInitialized,
                               OutSelfInitialized,
                               LocalSelfInitialized,
                               result)) {
        changed = true;
        OutSelfInitialized = result;
      }

      return changed;
    }

    /// Sets the elements of a use to available.
    void markAvailable(const DIMemoryUse &Use) {
      // If the memory object has nothing in it (e.g., is an empty tuple)
      // ignore.
      if (LocalAvailability.empty()) return;
      
      for (unsigned i = 0; i != Use.NumElements; ++i) {
        LocalAvailability.set(Use.FirstElement+i, DIKind::Yes);
        OutAvailability.set(Use.FirstElement+i, DIKind::Yes);
      }
    }

    /// Mark the block as storing to self, indicating the self box has been
    /// initialized.
    void markStoreToSelf() {
      LocalSelfInitialized = DIKind::Yes;
      OutSelfInitialized = DIKind::Yes;
    }

    /// If true, we're not done with our dataflow analysis yet.
    bool containsUndefinedValues() {
      return (!OutSelfInitialized.has_value() ||
              OutAvailability.containsUnknownElements());
    }
  };

  struct ConditionalDestroy {
    unsigned ReleaseID;
    AvailabilitySet Availability;
    DIKind SelfInitialized;
  };

  using BlockStates = BasicBlockData<LiveOutBlockState>;

  /// LifetimeChecker - This is the main heavy lifting for definite
  /// initialization checking of a memory object.
  class LifetimeChecker {
    SILFunction &F;
    SILModule &Module;

    /// TheMemory - This holds information about the memory object being
    /// analyzed.
    DIMemoryObjectInfo TheMemory;

    SmallVectorImpl<DIMemoryUse> &Uses;
    TinyPtrVector<SILInstruction *> &StoresToSelf;
    SmallVectorImpl<SILInstruction *> &Destroys;
    SmallVector<unsigned, 8> NeedsUpdateForInitState;
    std::vector<ConditionalDestroy> ConditionalDestroys;

    BlockStates &blockStates;
    BasicBlockFlag blockStateInitialized;

    /// This is a map of uses that are not loads (i.e., they are Stores,
    /// InOutUses, and Escapes), to their entry in Uses.
    llvm::SmallDenseMap<SILInstruction*, SmallVector<unsigned, 1>, 16> NonLoadUses;

    /// This is true when there is an ambiguous store, which may be an init or
    /// assign, depending on the CFG path.
    bool HasConditionalInitAssign = false;

    /// This is true when there is an ambiguous destroy, which may be a release
    /// of a fully-initialized or a partially-initialized value.
    bool HasConditionalDestroy = false;
    
    /// This is true when there is a destroy on a path where the self value may
    /// have been consumed, in which case there is nothing to do.
    bool HasConditionalSelfInitialized = false;
    
    /// This is true when the object being checked is a 'self' parameter for a
    /// struct in a non-delegating cross-module initializer. In this case, the
    /// initializer is not allowed to be fieldwise in Swift 5, so we produce a
    /// warning in Swift 4 and earlier.
    bool WantsCrossModuleStructInitializerDiagnostic = false;

    /// This is true if any diagnostics have offered a fix-it to insert
    /// `self.init()`. While the first diagnostic to offer this may not be
    /// suggesting it in the best place, offering it more than once is clearly
    /// wrong.
    bool HasSuggestedNoArgSelfInit = false;

    // Keep track of whether we've emitted an error.  We only emit one error per
    // location as a policy decision.
    std::vector<SILLocation> EmittedErrorLocs;
    SmallPtrSet<const SILBasicBlock *, 16> BlocksReachableFromEntry;
    
  public:
    LifetimeChecker(const DIMemoryObjectInfo &TheMemory,
                    DIElementUseInfo &UseInfo,
                    BlockStates &blockStates);

    void doIt();

  private:
    /// Find all the points where \c TheMemory has been fully initialized
    /// by a store to its element. If there are no elements then
    /// initialization point is located right after the mark_uninitialized
    /// instruction.
    void
    findFullInitializationPoints(SmallVectorImpl<SILInstruction *> &points);

    /// Injects `hop_to_executor` instructions into the function after
    /// `self` becomes fully initialized, only if the current function
    /// is an actor initializer that requires this, and if TheMemory
    /// corresponds to `self`.
    void injectActorHops();

    void emitSelfConsumedDiagnostic(SILInstruction *Inst);

    LiveOutBlockState &getBlockInfo(SILBasicBlock *BB) {
      auto &state = blockStates.get(BB, []() { return LiveOutBlockState(); });
      if (!blockStateInitialized.testAndSet(BB))
        state.init(TheMemory.getNumElements());
      return state;
    }

    AvailabilitySet getLivenessAtInst(SILInstruction *Inst, unsigned FirstElt,
                                      unsigned NumElts);
    AvailabilitySet getLivenessAtNonTupleInst(SILInstruction *Inst,
                                              SILBasicBlock *InstBB,
                                              AvailabilitySet &CurrentSet);
    int getAnyUninitializedMemberAtInst(SILInstruction *Inst, unsigned FirstElt,
                                        unsigned NumElts);

    DIKind getSelfInitializedAtInst(SILInstruction *Inst);

    bool isInitializedAtUse(const DIMemoryUse &Use,
                            bool *SuperInitDone = nullptr,
                            bool *FailedSelfUse = nullptr,
                            bool *FullyUninitialized = nullptr);

    void handleStoreUse(unsigned UseID);
    void handleLoadUse(const DIMemoryUse &Use);
    void handleLoadForTypeOfSelfUse(DIMemoryUse &Use);
    void handleTypeOfSelfUse(DIMemoryUse &Use);
    void handleInOutUse(const DIMemoryUse &Use);
    void handleEscapeUse(const DIMemoryUse &Use);

    bool diagnoseReturnWithoutInitializingStoredProperties(
        const SILInstruction *Inst, SILLocation loc, const DIMemoryUse &Use);

    void handleLoadUseFailure(const DIMemoryUse &Use,
                              bool SuperInitDone,
                              bool FailedSelfUse);

    void handleSelfInitUse(unsigned UseID);

    void updateInstructionForInitState(unsigned UseID);


    void processUninitializedRelease(SILInstruction *Release,
                                     bool consumed,
                                     SILBasicBlock::iterator InsertPt);

    /// Process a mark_uninitialized of an alloc_box that is uninitialized and
    /// needs a dealloc_box.
    void processUninitializedReleaseOfBox(MarkUninitializedInst *MUI,
                                          SILInstruction *Release,
                                          bool consumed,
                                          SILBasicBlock::iterator InsertPt);

    void deleteDeadRelease(unsigned ReleaseID);

    void processNonTrivialRelease(unsigned ReleaseID);

    SILValue handleConditionalInitAssign();
    void handleConditionalDestroys(SILValue ControlVariableAddr);

    typedef SmallVector<SILBasicBlock *, 16> WorkListType;
    void putIntoWorkList(SILBasicBlock *BB, WorkListType &WorkList);
    void computePredsLiveOut(SILBasicBlock *BB);
    void getOutAvailability(SILBasicBlock *BB, AvailabilitySet &Result);
    void getOutSelfInitialized(SILBasicBlock *BB,
                               llvm::Optional<DIKind> &Result);

    bool shouldEmitError(const SILInstruction *Inst);
    std::string getUninitElementName(const DIMemoryUse &Use);
    void noteUninitializedMembers(const DIMemoryUse &Use);
    void diagnoseInitError(const DIMemoryUse &Use,
                           Diag<StringRef, bool> DiagMessage);
    void diagnoseRefElementAddr(RefElementAddrInst *REI);
    bool diagnoseMethodCall(const DIMemoryUse &Use,
                            bool SuperInitDone);
    void diagnoseBadExplicitStore(SILInstruction *Inst);
    
    bool isBlockIsReachableFromEntry(const SILBasicBlock *BB);
  };
} // end anonymous namespace

LifetimeChecker::LifetimeChecker(const DIMemoryObjectInfo &TheMemory,
                                 DIElementUseInfo &UseInfo,
                                 BlockStates &blockStates)
    : F(TheMemory.getFunction()), Module(TheMemory.getModule()),
      TheMemory(TheMemory), Uses(UseInfo.Uses),
      StoresToSelf(UseInfo.StoresToSelf), Destroys(UseInfo.Releases),
      blockStates(blockStates), blockStateInitialized(&F) {

  // The first step of processing an element is to collect information about the
  // element into data structures we use later.
  for (unsigned ui = 0, e = Uses.size(); ui != e; ++ui) {
    auto &Use = Uses[ui];
    assert(Use.Inst && "No instruction identified?");

    // Keep track of all the uses that aren't loads or escapes.  These are
    // important uses that we'll visit, but we don't consider them definition
    // points for liveness computation purposes.
    switch (Use.Kind) {
    case DIUseKind::Load:
    case DIUseKind::LoadForTypeOfSelf:
    case DIUseKind::TypeOfSelf:
    case DIUseKind::Escape:
      continue;
    case DIUseKind::Assign:
    case DIUseKind::Set:
    case DIUseKind::IndirectIn:
    case DIUseKind::InitOrAssign:
    case DIUseKind::InOutArgument:
    case DIUseKind::Initialization:
    case DIUseKind::InOutSelfArgument:
    case DIUseKind::PartialStore:
    case DIUseKind::SelfInit:
    case DIUseKind::BadExplicitStore:
      break;
    }

    NonLoadUses[Use.Inst].push_back(ui);

    auto &BBInfo = getBlockInfo(Use.Inst->getParent());
    BBInfo.HasNonLoadUse = true;

    // Each of the non-load instructions will each be checked to make sure that
    // they are live-in or a full element store.  This means that the block they
    // are in should be treated as a live out for cross-block analysis purposes.
    BBInfo.markAvailable(Use);
  }

  // Mark blocks where the self box is initialized.
  for (auto *I : StoresToSelf) {
    // FIXME: critical edges?
    auto *bb = I->getParent();
    getBlockInfo(bb).markStoreToSelf();
  }

  // It isn't really a use, but we account for the mark_uninitialized or
  // project_box as a use so we see it in our dataflow walks.
  auto &MemBBInfo = getBlockInfo(TheMemory.getParentBlock());
  MemBBInfo.HasNonLoadUse = true;

  // There is no scanning required (or desired) for the block that defines the
  // memory object itself.  Its live-out properties are whatever are trivially
  // locally inferred by the loop above.  Mark any unset elements as not
  // available.
  MemBBInfo.setUnknownToNotAvailable();

  // Finally, check if we need to emit compatibility diagnostics for cross-module
  // non-delegating struct initializers.
  if (TheMemory.isCrossModuleStructInitSelf())
    WantsCrossModuleStructInitializerDiagnostic = true;
}

/// Determine whether the specified block is reachable from the entry of the
/// containing function's entrypoint.  This allows us to avoid diagnosing DI
/// errors in synthesized code that turns out to be unreachable.
bool LifetimeChecker::isBlockIsReachableFromEntry(const SILBasicBlock *BB) {
  // Lazily compute reachability, so we only have to do it in the case of an
  // error.
  if (BlocksReachableFromEntry.empty()) {
    SmallVector<const SILBasicBlock*, 128> Worklist;
    Worklist.push_back(&BB->getParent()->front());
    BlocksReachableFromEntry.insert(Worklist.back());
    
    // Collect all reachable blocks by walking the successors.
    while (!Worklist.empty()) {
      const SILBasicBlock *BB = Worklist.pop_back_val();
      for (auto &Succ : BB->getSuccessors()) {
        if (BlocksReachableFromEntry.insert(Succ).second)
          Worklist.push_back(Succ);
      }
    }
  }
  
  return BlocksReachableFromEntry.count(BB);
}


/// shouldEmitError - Check to see if we've already emitted an error at the
/// specified instruction.  If so, return false.  If not, remember the
/// instruction and return true.
bool LifetimeChecker::shouldEmitError(const SILInstruction *Inst) {
  // If this instruction is in a dead region, don't report the error.  This can
  // occur because we haven't run DCE before DI and this may be a synthesized
  // statement.  If it isn't synthesized, then DCE will report an error on the
  // dead code.
  if (!isBlockIsReachableFromEntry(Inst->getParent()))
    return false;

  // Check to see if we've already emitted an error at this location.  If so,
  // swallow the error.
  SILLocation InstLoc = Inst->getLoc();
  if (llvm::any_of(EmittedErrorLocs, [&](SILLocation L) -> bool {
        return L.getSourceLoc() == InstLoc.getSourceLoc();
      }))
    return false;

  // Ignore loads used only by an assign_by_wrapper setter. This
  // is safe to ignore because assign_by_wrapper will only be
  // re-written to use the setter if the value is fully initialized.
  if (auto *load = dyn_cast<SingleValueInstruction>(Inst)) {
    if (auto Op = load->getSingleUse()) {
      if (auto PAI = dyn_cast<PartialApplyInst>(Op->getUser())) {
        if (std::find_if(PAI->use_begin(), PAI->use_end(),
                         [](auto PAIUse) {
                           return isa<AssignByWrapperInst>(PAIUse->getUser());
                         }) != PAI->use_end()) {
          return false;
        }
      }
    }
  }

  EmittedErrorLocs.push_back(InstLoc);
  return true;
}


/// Emit notes for each uninitialized stored property in a designated
/// initializer.
void LifetimeChecker::noteUninitializedMembers(const DIMemoryUse &Use) {
  assert(TheMemory.isAnyInitSelf() && !TheMemory.isDelegatingInit() &&
         "Not a designated initializer");

  // Determine which members, specifically are uninitialized.
  AvailabilitySet Liveness =
    getLivenessAtInst(Use.Inst, Use.FirstElement, Use.NumElements);

  SmallVector<std::function<void()>, 2> delayedNotes;
  bool emittedNote = false;

  for (unsigned i = Use.FirstElement, e = Use.FirstElement+Use.NumElements;
       i != e; ++i) {
    if (Liveness.get(i) == DIKind::Yes) continue;

    // Ignore a failed super.init requirement.
    if (i == TheMemory.getNumElements() - 1 && TheMemory.isDerivedClassSelf())
      continue;

    std::string Name;
    auto *Decl = TheMemory.getPathStringToElement(i, Name);
    SILLocation Loc = Use.Inst->getLoc();

    if (Decl) {
      // If we found a non-implicit declaration, use its source location.
      if (!Decl->isImplicit())
        Loc = SILLocation(Decl);

      // If it's marked @_compilerInitialized, delay emission of the note.
      if (Decl->getAttrs().hasAttribute<CompilerInitializedAttr>()) {
        delayedNotes.push_back([=](){
          diagnose(Module, Loc, diag::stored_property_not_initialized,
             StringRef(Name));
        });
        continue;
      }
    }

    diagnose(Module, Loc, diag::stored_property_not_initialized,
             StringRef(Name));
    emittedNote = true;
  }

  // Drop the notes for @_compilerInitialized decls if we emitted a note for
  // other ones that do not have that attr.
  if (emittedNote)
    return;

  // otherwise, emit delayed notes.
  for (auto &emitter : delayedNotes)
    emitter();
}

/// Given a use that has at least one uninitialized element in it, produce a
/// nice symbolic name for the element being accessed.
std::string LifetimeChecker::getUninitElementName(const DIMemoryUse &Use) {

  // If the overall memory allocation has multiple elements, then dive in to
  // explain *which* element is being used uninitialized.  Start by rerunning
  // the query, to get a bitmask of exactly which elements are uninitialized.
  // In a multi-element query, the first element may already be defined and
  // we want to point to the second one.
  unsigned firstUndefElement =
    getAnyUninitializedMemberAtInst(Use.Inst, Use.FirstElement,Use.NumElements);
  assert(firstUndefElement != ~0U && "No undef elements found?");
  
  // Verify that it isn't the super.init marker that failed.  The client should
  // handle this, not pass it down to diagnoseInitError.
  assert((!TheMemory.isDerivedClassSelf() ||
          firstUndefElement != TheMemory.getNumElements() - 1) &&
         "super.init failure not handled in the right place");

  // If the definition is a declaration, try to reconstruct a name and
  // optionally an access path to the uninitialized element.
  //
  // TODO: Given that we know the range of elements being accessed, we don't
  // need to go all the way deep into a recursive tuple here.  We could print
  // an error about "v" instead of "v.0" when "v" has tuple type and the whole
  // thing is accessed inappropriately.
  std::string Name;
  TheMemory.getPathStringToElement(firstUndefElement, Name);

  return Name;
}

void LifetimeChecker::diagnoseInitError(const DIMemoryUse &Use,
                                        Diag<StringRef, bool> DiagMessage) {
  auto *Inst = Use.Inst;
  if (!shouldEmitError(Inst))
    return;

  // If the definition is a declaration, try to reconstruct a name and
  // optionally an access path to the uninitialized element.
  std::string Name = getUninitElementName(Use);

  // Figure out the source location to emit the diagnostic to.  If this is null,
  // it is probably implicitly generated code, so we'll adjust it.
  SILLocation DiagLoc = Inst->getLoc();
  if (DiagLoc.isNull() || DiagLoc.getSourceLoc().isInvalid())
    DiagLoc = Inst->getFunction()->getLocation();

  // Determine whether the field we're touching is a let property.
  bool isLet = true;
  for (unsigned i = 0, e = Use.NumElements; i != e; ++i)
    isLet &= TheMemory.isElementLetProperty(i);

  diagnose(Module, DiagLoc, DiagMessage, StringRef(Name), isLet);

  // As a debugging hack, print the instruction itself if there is no location
  // information.  This should never happen.
  if (Inst->getLoc().isNull())
    llvm::dbgs() << "  the instruction: " << *Inst << "\n";

  // Provide context as note diagnostics.

  // TODO: The QoI could be improved in many different ways here.  For example,
  // We could give some path information where the use was uninitialized, like
  // the static analyzer.
  if (!TheMemory.isAnyInitSelf())
    diagnose(Module, TheMemory.getLoc(), diag::variable_defined_here, isLet);
}

void LifetimeChecker::diagnoseBadExplicitStore(SILInstruction *Inst) {
  if (!shouldEmitError(Inst))
    return;

  diagnose(Module, Inst->getLoc(), diag::explicit_store_of_compilerinitialized);
}

/// Determines whether the given function is a constructor that belongs to a
/// distributed actor declaration.
/// \returns nullptr if false, and the class decl for the actor otherwise.
static ClassDecl* getDistributedActorOfCtor(SILFunction &F) {
  auto *context = F.getDeclContext();
  if (auto *ctor = dyn_cast_or_null<ConstructorDecl>(context->getAsDecl()))
    if (auto *cls = dyn_cast<ClassDecl>(ctor->getDeclContext()->getAsDecl()))
      if (cls->isDistributedActor())
        return cls;
  return nullptr;
}

static bool isFailableInitReturnUseOfEnum(EnumInst *EI);

void LifetimeChecker::findFullInitializationPoints(
    SmallVectorImpl<SILInstruction *> &points) {
  auto recordLocations = [&](SILInstruction *inst) {
    // While insertAfter can handle terminators, it cannot handle ones that lead
    // to a block with multiple predecessors. I don't expect that a terminator
    // could initialize a stored property at all: a try_apply passed the
    // property as an inout would not be a valid use until _after_ the property
    // has been initialized.
    assert(!isa<TermInst>(inst) && "unexpected terminator");

    //////
    // NOTE: We prefer to inject code outside of any access regions, so that
    // the dynamic access-set is empty. This is a best-effort to avoid injecting
    // it inside of a region, but does not account for overlapping accesses,
    // etc. But, I cannot think of a way to create an overlapping access with a
    // stored property when it is first initialized, because it's not valid to
    // pass those inout or capture them in a closure. - kavon

    BeginAccessInst *access = nullptr;

    // Finds begin_access instructions that need hops placed after its
    // end_access.
    auto getBeginAccess = [](SILValue v) -> BeginAccessInst * {
      return dyn_cast<BeginAccessInst>(getAccessScope(v));
    };

    // If this insertion-point is after a store-like instruction, look for a
    // begin_access corresponding to the destination.
    if (auto *store = dyn_cast<StoreInst>(inst)) {
      access = getBeginAccess(store->getDest());
    } else if (auto *assign = dyn_cast<AssignInst>(inst)) {
      access = getBeginAccess(assign->getDest());
    }

    // If we found a begin_access, then we need to inject the hop after
    // all of the corresponding end_accesses.
    if (access) {
      for (auto *endAccess : access->getEndAccesses())
        points.push_back(endAccess);
    } else {
      points.push_back(inst);
    }
  };

  // Even if there are no stored properties to initialize, we still need
  // to mark full initialization point.
  //
  // We insert this directly after the mark_uninitialized instruction, so
  // that it happens as early as `self` is available.
  if (TheMemory.getNumElements() == 0) {
    // FIXME: this might be wrong for convenience inits (rdar://87485045)
    auto *selfDef = TheMemory.getUninitializedValue();
    recordLocations(&*selfDef->getIterator());
    return;
  }

  // Returns true iff a block returns normally from the initializer,
  // which means that it returns `self` in some way (perhaps optional-wrapped).
  auto returnsSelf = [](SILBasicBlock &block) -> bool {
    auto term = block.getTerminator();
    auto kind = term->getTermKind();

    // Does this block return directly?
    if (kind == TermKind::ReturnInst)
      return true;

    // Does this block return `self` wrapped in an Optional?
    // The pattern would look like:
    //
    // thisBB:
    //   ...
    //   %x = enum $Optional<Dactor>, #Optional.some!enumelt
    //   br exitBB(%x : $Optional<Dactor>)
    //
    // exitBB(%y : $Optional<Dactor>):
    //   return %y : $Optional<Dactor>
    //
    if (kind == TermKind::BranchInst)
      if (term->getNumOperands() == 1)
        if (auto *passedVal = term->getOperand(0)->getDefiningInstruction())
          if (auto *ei = dyn_cast<EnumInst>(passedVal))
            if (isFailableInitReturnUseOfEnum(ei))
              // Once we've reached this point, we know it's an Optional enum.
              // To determine whether it's .some or .none, we can just check
              // the number of operands.
              return ei->getNumOperands() == 1; // is it .some ?

    return false;
  };

  for (auto &block : F) {
    /////
    // Step 1: Find initializing blocks, which are blocks that contain a store
    // to TheMemory that fully-initializes it, and build the Map.

    // We determine whether a block is "initializing" by inspecting the "in" and
    // "out" availability sets of the block. If the block goes from No / Partial
    // "in" to Yes "out", then some instruction in the block caused TheMemory to
    // become fully-initialized, so we record that block and its in-availability
    // to scan the block more precisely later in the next Step.

    auto &info = getBlockInfo(&block);

    if (!info.HasNonLoadUse) {
      LLVM_DEBUG(llvm::dbgs()
                 << "full-init-finder: rejecting bb" << block.getDebugID()
                 << " b/c no non-load uses.\n");
      continue; // could not be an initializing block.
    }

    // Determine if this `block` is initializing, that is:
    //
    //     InAvailability ≡ merge(OutAvailability(predecessors(block)))
    //                    ≠ Yes
    //               AND
    //     OutAvailability(block) = Yes OR returnsSelf(block)
    //
    // A block with no predecessors has in-avail of non-Yes.
    // A block with no successors has an out-avail of non-Yes, since
    // availability is not computed for it.

    auto outSet = info.OutAvailability;
    if (!outSet.isAllYes() && !returnsSelf(block)) {
      LLVM_DEBUG(llvm::dbgs()
                 << "full-init-finder: rejecting bb" << block.getDebugID()
                 << " b/c non-Yes OUT avail\n");
      continue; // then this block never sees TheMemory initialized.
    }

    AvailabilitySet inSet(outSet.size());
    auto const &predecessors = block.getPredecessorBlocks();
    for (auto *pred : predecessors)
      inSet.mergeIn(getBlockInfo(pred).OutAvailability);

    if (inSet.isAllYes()) {
      LLVM_DEBUG(llvm::dbgs()
                 << "full-init-finder: rejecting bb" << block.getDebugID()
                 << " b/c all-Yes IN avail\n");
      continue; // then this block always sees TheMemory initialized.
    }

    LLVM_DEBUG(llvm::dbgs() << "full-init-finder: bb" << block.getDebugID()
                            << " is initializing block with in-availability: "
                            << inSet << "\n");

    // Step 2: Scan the initializing block to find the first non-load use that
    // fully-initializes TheMemory.
    {
      // Tracks status of each element of TheMemory as we scan through the
      // block, starting with the initial availability at the block's
      // entry-point.
      AvailabilitySet localAvail = inSet;

      auto bbi = block.begin(); // our cursor and eventual insertion-point.
      const auto bbe = block.end();
      for (; bbi != bbe; ++bbi) {
        auto *inst = &*bbi;

        auto result = NonLoadUses.find(inst);
        if (result == NonLoadUses.end())
          continue; // not a possible store

        // Mark the tuple elements involved in this use as defined.
        for (unsigned use : result->second) {
          auto const &instUse = Uses[use];
          for (unsigned i = instUse.FirstElement;
               i < instUse.FirstElement + instUse.NumElements; ++i)
            localAvail.set(i, DIKind::Yes);
        }

        // Stop if we found the instruction that initializes TheMemory.
        if (localAvail.isAllYes())
          break;
      }

      // Make sure we found the initializing use of TheMemory.
      assert(bbi != bbe && "this block is not initializing?");
      recordLocations(&*bbi);
    }
  }
}

void LifetimeChecker::injectActorHops() {
  auto ctor = TheMemory.getActorInitSelf();

  // Must be `self` within an actor's initializer.
  if (!ctor)
    return;

  // Must not be an init that uses flow-sensitive isolation.
  if (usesFlowSensitiveIsolation(ctor))
    return;

  // Must be an async initializer.
  if (!ctor->hasAsync())
    return;

  // Must be an initializer that is isolated to self.
  switch (getActorIsolation(ctor)) {
  case ActorIsolation::ActorInstance:
    break;

  case ActorIsolation::Unspecified:
  case ActorIsolation::Independent:
  case ActorIsolation::GlobalActorUnsafe:
  case ActorIsolation::GlobalActor:
    return;
  }

  SmallVector<SILInstruction *> hopToActorAfter;
  findFullInitializationPoints(hopToActorAfter);

  auto injectExecutorHopAfter = [&](SILInstruction *insertPt) -> void {
    LLVM_DEBUG(llvm::dbgs() << "hop-injector: injecting after " << *insertPt);
    SILBuilderWithScope::insertAfter(insertPt, [&](SILBuilder &b) {
      SILLocation genLoc = SILLocation(ctor).asAutoGenerated();
      const bool delegating = !TheMemory.isNonDelegatingInit();
      SILValue val = TheMemory.getUninitializedValue();
      auto &F = b.getFunction();

      // delegating inits always have an alloc we need to load it from.
      if (delegating)
        val = b.createLoad(genLoc, val, LoadOwnershipQualifier::Copy);

      SILValue actor = b.createBeginBorrow(genLoc, val);

      b.createHopToExecutor(genLoc, actor, /*mandatory=*/false);

      // Distributed actors also need to notify their transport immediately
      // after performing the hop.
      if (!delegating) {
        if (auto *actorDecl = getDistributedActorOfCtor(F)) {
          SILValue systemRef =
              refDistributedActorSystem(b, genLoc, actorDecl, actor);
          emitActorReadyCall(b, genLoc, actor, systemRef);
        }
      }

      b.createEndBorrow(genLoc, actor);

      if (delegating)
        b.createDestroyValue(genLoc, val);
    });
  };

  for (auto *point : hopToActorAfter)
    injectExecutorHopAfter(point);
}

void LifetimeChecker::doIt() {
  // With any escapes tallied up, we can work through all the uses, checking
  // for definitive initialization, promoting loads, rewriting assigns, and
  // performing other tasks.

  // Note that this should not use a for-each loop, as the Uses list can grow
  // and reallocate as we iterate over it.
  for (unsigned i = 0; i != Uses.size(); ++i) {
    auto &Use = Uses[i];
    auto *Inst = Uses[i].Inst;
    // Ignore entries for instructions that got expanded along the way.
    if (Inst == nullptr) continue;
    
    switch (Use.Kind) {
    case DIUseKind::Initialization:
      // We assume that SILGen knows what it is doing when it produces
      // initializations of variables, because it only produces them when it
      // knows they are correct, and this is a super common case for "var x = y"
      // cases.
      continue;
        
    case DIUseKind::Assign:
    case DIUseKind::Set:
      // Instructions classified as assign are only generated when lowering
      // InitOrAssign instructions in regions known to be initialized.  Since
      // they are already known to be definitely init, don't reprocess them.
      continue;
    case DIUseKind::InitOrAssign:
      // FIXME: This is a hack because DI is not understanding SILGen's
      // stack values that have multiple init and destroy lifetime cycles with
      // one allocation.  This happens in foreach silgen (see rdar://15532779)
      // and needs to be resolved someday, either by changing silgen or by
      // teaching DI about destroy events.  In the meantime, just assume that
      // all stores of trivial type are ok.
      if (isa<StoreInst>(Inst))
        continue;
        
      LLVM_FALLTHROUGH;
    case DIUseKind::PartialStore:
      handleStoreUse(i);
      break;

    case DIUseKind::IndirectIn:
    case DIUseKind::Load:
      handleLoadUse(Use);
      break;
    case DIUseKind::InOutArgument:
    case DIUseKind::InOutSelfArgument:
      handleInOutUse(Use);
      break;
    case DIUseKind::Escape:
      handleEscapeUse(Use);
      break;
    case DIUseKind::SelfInit:
      handleSelfInitUse(i);
      break;
    case DIUseKind::LoadForTypeOfSelf:
      handleLoadForTypeOfSelfUse(Use);
      break;
    case DIUseKind::TypeOfSelf:
      handleTypeOfSelfUse(Use);
      break;

    case DIUseKind::BadExplicitStore:
      diagnoseBadExplicitStore(Inst);
      break;
    }
  }

  // If we emitted an error, there is no reason to proceed with load promotion.
  if (!EmittedErrorLocs.empty()) {
    // Since we failed DI, for now, turn off the move checker on the entire
    // function. With time, we should be able to allow for move checker checks
    // to be emitted on unrelated allocations, but given where we are this is a
    // good enough fix.
    TheMemory.getFunction().addSemanticsAttr(
        semantics::NO_MOVEONLY_DIAGNOSTICS);
    return;
  }

  // All of the indirect results marked as "out" have to be fully initialized
  // before their lifetime ends.
  if (TheMemory.isOut()) {
    auto diagnoseMissingInit = [&]() {
      std::string propertyName;
      auto *property = TheMemory.getPathStringToElement(0, propertyName);
      diagnose(Module, F.getLocation(),
               diag::ivar_not_initialized_by_init_accessor,
               property->getName());
      EmittedErrorLocs.push_back(TheMemory.getLoc());
    };

    // No uses means that there was no initialization.
    if (Uses.empty()) {
      diagnoseMissingInit();
      return;
    }

    // Go over every return block and check whether member is fully initialized
    // because it's possible that there is branch that doesn't have any use of
    // the memory and nothing else is going to diagnose that. This is different
    // from `self`, for example, because it would always have either `copy_addr`
    // or `load` before return.

    auto returnBB = F.findReturnBB();

    while (returnBB != F.end()) {
      auto *terminator = returnBB->getTerminator();

      if (!isInitializedAtUse(DIMemoryUse(terminator, DIUseKind::Load, 0, 1)))
        diagnoseMissingInit();

      ++returnBB;
    }
  }

  // If the memory object has nontrivial type, then any destroy/release of the
  // memory object will destruct the memory.  If the memory (or some element
  // thereof) is not initialized on some path, the bad things happen.  Process
  // releases to adjust for this.
  if (!TheMemory.hasTrivialType()) {
    // NOTE: This array may increase in size!
    for (unsigned i = 0, e = Destroys.size(); i != e; ++i)
      processNonTrivialRelease(i);
  }

  /// At this point, we should have computed enough liveness information to
  /// provide accurate information about initialization points, even for
  /// local variables within a function, because we've now processed the
  /// destroy/releases.

  // Insert hop_to_executor instructions for actor initializers, if needed.
  injectActorHops();

  // If the memory object had any non-trivial stores that are init or assign
  // based on the control flow path reaching them, then insert dynamic control
  // logic and CFG diamonds to handle this.
  SILValue ControlVariable;
  if (HasConditionalInitAssign ||
      HasConditionalDestroy ||
      HasConditionalSelfInitialized) {
    ControlVariable = handleConditionalInitAssign();
    SILValue memAddr = TheMemory.getUninitializedValue()->getOperand(0);
    if (auto *ASI = dyn_cast<AllocStackInst>(memAddr)) {
      ASI->setDynamicLifetime();
    } else if (auto *ABI = dyn_cast<AllocBoxInst>(memAddr)) {
      ABI->setDynamicLifetime();
    }
    // We don't support noncopyable types with dynamic lifetimes currently.
    if (TheMemory.getType().isMoveOnly()) {
      diagnose(Module, TheMemory.getUninitializedValue()->getLoc(),
               diag::noncopyable_dynamic_lifetime_unsupported);
    }
  }
  if (!ConditionalDestroys.empty())
    handleConditionalDestroys(ControlVariable);

  // handleStoreUse(), handleSelfInitUse() and handleConditionalInitAssign()
  // postpone lowering of assignment instructions to avoid deleting
  // instructions that still appear in the Uses list.
  for (unsigned UseID : NeedsUpdateForInitState)
    updateInstructionForInitState(UseID);
}

void LifetimeChecker::handleLoadUse(const DIMemoryUse &Use) {
  bool IsSuperInitComplete, FailedSelfUse;
  // If the value is not definitively initialized, emit an error.
  if (!isInitializedAtUse(Use, &IsSuperInitComplete, &FailedSelfUse))
    return handleLoadUseFailure(Use, IsSuperInitComplete, FailedSelfUse);
}

static void replaceValueMetatypeInstWithMetatypeArgument(
    ValueMetatypeInst *valueMetatype) {
  SILValue metatypeArgument = valueMetatype->getFunction()->getSelfArgument();

  // SILFunction parameter types never have a DynamicSelfType, since it only
  // makes sense in the context of a given method's body. Since the
  // value_metatype instruction might produce a DynamicSelfType we have to
  // cast the metatype argument.
  //
  // FIXME: Semantically, we're "opening" the class metatype here to produce
  // the "opened" DynamicSelfType. Ideally it would be modeled as an opened
  // archetype associated with the original metatype or class instance value,
  // instead of as a "global" type.
  auto metatypeSelfType = metatypeArgument->getType()
      .castTo<MetatypeType>().getInstanceType();
  auto valueSelfType = valueMetatype->getType()
      .castTo<MetatypeType>().getInstanceType();
  if (metatypeSelfType != valueSelfType) {
    assert(metatypeSelfType ==
           cast<DynamicSelfType>(valueSelfType).getSelfType());

    SILBuilderWithScope B(valueMetatype);
    metatypeArgument = B.createUncheckedTrivialBitCast(
        valueMetatype->getLoc(), metatypeArgument,
        valueMetatype->getType());
  }
  InstModCallbacks callbacks;
  replaceAllSimplifiedUsesAndErase(valueMetatype, metatypeArgument, callbacks);
}

void LifetimeChecker::handleLoadForTypeOfSelfUse(DIMemoryUse &Use) {
  bool IsSuperInitComplete, FailedSelfUse;
  // If the value is not definitively initialized, replace the
  // value_metatype instruction with the metatype argument that was passed into
  // the initializer.
  if (!isInitializedAtUse(Use, &IsSuperInitComplete, &FailedSelfUse)) {
    auto load = cast<SingleValueInstruction>(Use.Inst);
    
    ValueMetatypeInst *valueMetatype = nullptr;
    for (auto use : load->getUses()) {
      valueMetatype = dyn_cast<ValueMetatypeInst>(use->getUser());
      if (valueMetatype)
        break;
    }
    replaceValueMetatypeInstWithMetatypeArgument(valueMetatype);

    // Dead loads for type-of-self must be removed.
    // Otherwise it's a violation of memory lifetime.
    if (isa<LoadBorrowInst>(load)) {
      assert(load->hasOneUse() && isa<EndBorrowInst>(load->getSingleUse()->getUser()));
      load->getSingleUse()->getUser()->eraseFromParent();
    }
    assert(load->use_empty());
    load->eraseFromParent();
    // Clear the Inst pointer just to be sure to avoid use-after-free.
    Use.Inst = nullptr;
  }
}

void LifetimeChecker::handleTypeOfSelfUse(DIMemoryUse &Use) {
  bool IsSuperInitComplete, FailedSelfUse;
  // If the value is not definitively initialized, replace the
  // value_metatype instruction with the metatype argument that was passed into
  // the initializer.
  if (!isInitializedAtUse(Use, &IsSuperInitComplete, &FailedSelfUse)) {
    auto *valueMetatype = cast<ValueMetatypeInst>(Use.Inst);
    replaceValueMetatypeInstWithMetatypeArgument(valueMetatype);

    // Clear the Inst pointer just to be sure to avoid use-after-free.
    Use.Inst = nullptr;
  }
}

void LifetimeChecker::emitSelfConsumedDiagnostic(SILInstruction *Inst) {
  if (!shouldEmitError(Inst))
    return;

  diagnose(Module, Inst->getLoc(),
           diag::self_inside_catch_superselfinit,
           (unsigned)TheMemory.isDelegatingInit());
}

/// If \p theStruct is imported from C and has a zeroing no-argument
/// initializer, add a note to suggest calling it ahead of \p loc.
///
/// Most (but not all) C structs have a zeroing no-argument initializer;
/// the ones that don't have fields don't make sense to zero.
static void maybeSuggestNoArgSelfInit(SILModule &module, SILLocation loc,
                                      StructDecl *theStruct) {
  if (!theStruct || !theStruct->hasClangNode())
    return;

  ASTContext &ctx = module.getASTContext();
  DeclName noArgInit(ctx, DeclBaseName::createConstructor(),
                     ArrayRef<Identifier>());

  auto lookupResults = theStruct->lookupDirect(noArgInit);
  if (lookupResults.size() != 1)
    return;
  if (lookupResults.front()->getDeclContext() != theStruct)
    return;

  diagnose(module, loc, diag::designated_init_c_struct_fix)
    .fixItInsert(loc.getStartSourceLoc(), "self.init()\n");
}

void LifetimeChecker::handleStoreUse(unsigned UseID) {
  DIMemoryUse &Use = Uses[UseID];

  // Determine the liveness state of the element that we care about.
  auto Liveness = getLivenessAtInst(Use.Inst, Use.FirstElement,
                                    Use.NumElements);

  // Check to see if the stored location is either fully uninitialized or fully
  // initialized.
  bool isFullyInitialized = true;
  bool isFullyUninitialized = true;
  for (unsigned i = Use.FirstElement, e = i+Use.NumElements;
       i != e;++i) {
    auto DI = Liveness.get(i);
    if (DI != DIKind::Yes)
      isFullyInitialized = false;
    if (DI != DIKind::No)
      isFullyUninitialized = false;
  }

  if (TheMemory.isNonRootClassSelf()) {
    if (getSelfInitializedAtInst(Use.Inst) != DIKind::Yes) {
      auto SelfLiveness =
          getLivenessAtInst(Use.Inst, 0, TheMemory.getNumElements());
      if (SelfLiveness.isAllYes()) {
        emitSelfConsumedDiagnostic(Use.Inst);
        return;
      }
    }
  }

  // If this is a partial store into a struct and the whole struct hasn't been
  // initialized, diagnose this as an error.
  if (Use.Kind == DIUseKind::PartialStore && !isFullyInitialized) {
    assert(Use.NumElements == 1 && "partial stores are intra-element");
    diagnoseInitError(Use, diag::struct_not_fully_initialized);
    return;
  }

  // If this is a store to a 'let' property in an initializer, then we only
  // allow the assignment if the property was completely uninitialized.
  // Overwrites are not permitted.
  if (Use.Kind == DIUseKind::PartialStore || !isFullyUninitialized) {
    for (unsigned i = Use.FirstElement, e = i+Use.NumElements;
         i != e; ++i) {
      if (Liveness.get(i) == DIKind::No || !TheMemory.isElementLetProperty(i))
        continue;

      // Don't emit errors for unreachable code, or if we have already emitted
      // a diagnostic.
      if (!shouldEmitError(Use.Inst))
        continue;

      std::string PropertyName;
      auto *VD = TheMemory.getPathStringToElement(i, PropertyName);
      diagnose(Module, Use.Inst->getLoc(),
               diag::immutable_property_already_initialized,
               StringRef(PropertyName));

      if (auto *Var = dyn_cast<VarDecl>(VD)) {
        if (Var->getParentExecutableInitializer())
          diagnose(Module, SILLocation(VD),
                   diag::initial_value_provided_in_let_decl);
        Var->emitLetToVarNoteIfSimple(nullptr);
      }
      return;
    }
  }

  // Check if we're in a struct initializer that uses CrossModuleRootSelf rather
  // than DelegatingSelf for Swift 4 compatibility. We look for a problem case by
  // seeing if there are any assignments to individual fields that might be
  // initializations; that is, that they're not dominated by `self = other`.

  auto isFullValueAssignment = [this](const SILInstruction *inst) -> bool {
    SILValue addr;
    if (auto *copyAddr = dyn_cast<CopyAddrInst>(inst))
      addr = copyAddr->getDest();
    else if (auto *moveAddr = dyn_cast<MarkUnresolvedMoveAddrInst>(inst))
      addr = moveAddr->getDest();
    else if (auto *assign = dyn_cast<AssignInst>(inst))
      addr = assign->getDest();
    else if (auto *assign = dyn_cast<AssignByWrapperInst>(inst))
      addr = assign->getDest();
    else
      return false;

    if (auto *access = dyn_cast<BeginAccessInst>(addr))
      addr = access->getSource();
    if (auto *projection = dyn_cast<ProjectBoxInst>(addr))
      addr = projection->getOperand();

    return addr == TheMemory.getUninitializedValue();
  };

  if (!isFullyInitialized && WantsCrossModuleStructInitializerDiagnostic &&
      !isFullValueAssignment(Use.Inst)) {
    // Deliberately don't check shouldEmitError here; we're using DI to approximate
    // whether this would be a valid delegating initializer, but the error when it
    // /is/ a delegating initializer won't be path-sensitive.

    Type selfTy;
    SILLocation fnLoc = TheMemory.getFunction().getLocation();
    if (auto *ctor = fnLoc.getAsASTNode<ConstructorDecl>())
      selfTy = ctor->getImplicitSelfDecl()->getTypeInContext();
    else
      selfTy = TheMemory.getASTType();

    StructDecl *theStruct = selfTy->getStructOrBoundGenericStruct();
    assert(theStruct);

    diagnose(Module, Use.Inst->getLoc(),
             diag::designated_init_in_cross_module_extension,
             selfTy, !isFullyUninitialized,
             theStruct->getParentModule()->getName(),
             theStruct->hasClangNode());
    if (!HasSuggestedNoArgSelfInit && isFullyUninitialized) {
      maybeSuggestNoArgSelfInit(Module, Use.Inst->getLoc(), theStruct);
      HasSuggestedNoArgSelfInit = true;
    }

    // Don't emit more than one of these diagnostics per initializer.
    WantsCrossModuleStructInitializerDiagnostic = false;
  }

  // If this is an initialization or a normal assignment, upgrade the store to
  // an initialization or assign in the uses list so that clients know about it.
  if (isFullyUninitialized) {
    // If this is a placeholder use of `assign_or_init` instruction,
    // check whether all of the fields are initialized - if so, call a setter,
    // otherwise call init accessor.
    if (isa<AssignOrInitInst>(Use.Inst) && Use.NumElements == 0) {
      auto allFieldsInitialized =
          getAnyUninitializedMemberAtInst(Use.Inst, 0,
                                          TheMemory.getNumElements()) == -1;
      Use.Kind =
          allFieldsInitialized ? DIUseKind::Set : DIUseKind::Initialization;
    } else {
      Use.Kind = DIUseKind::Initialization;
    }
  } else if (isFullyInitialized && isa<AssignByWrapperInst>(Use.Inst)) {
    // If some fields are uninitialized, re-write assign_by_wrapper to assignment
    // of the backing wrapper. If all fields are initialized, assign to the wrapped
    // value.
    auto allFieldsInitialized =
        getAnyUninitializedMemberAtInst(Use.Inst, 0, TheMemory.getNumElements()) == -1;
    Use.Kind = allFieldsInitialized ? DIUseKind::Set : DIUseKind::Assign;
  } else if (isFullyInitialized && isa<AssignOrInitInst>(Use.Inst)) {
    auto allFieldsInitialized =
        getAnyUninitializedMemberAtInst(Use.Inst, 0,
                                        TheMemory.getNumElements()) == -1;

    auto *AOI = cast<AssignOrInitInst>(Use.Inst);
    // init accessor properties without setters behave like `let` properties
    // and don't support re-initialization.
    if (isa<SILUndef>(AOI->getSetter())) {
      diagnose(Module, AOI->getLoc(),
               diag::immutable_property_already_initialized,
               AOI->getPropertyName());
    }

    Use.Kind = allFieldsInitialized ? DIUseKind::Set : DIUseKind::Assign;
  } else if (isFullyInitialized) {
    Use.Kind = DIUseKind::Assign;
  } else {
    // If it is initialized on some paths, but not others, then we have an
    // inconsistent initialization, which needs dynamic control logic in the
    // general case.

    // This is classified as InitOrAssign (not PartialStore), so there are only
    // a few instructions that could reach here.
    assert(Use.Kind == DIUseKind::InitOrAssign &&
           "should only have inconsistent InitOrAssign's here");

    // If this access stores something of non-trivial type, then keep track of
    // it for later.   Once we've collected all of the conditional init/assigns,
    // we can insert a single control variable for the memory object for the
    // whole function.
    //
    // For root class initializers, we must keep track of initializations of
    // trivial stored properties also, since we need to know when the object
    // has been fully initialized when deciding if a strong_release should
    // lower to a partial_dealloc_ref.
    if (TheMemory.isRootClassSelf() ||
        !Use.onlyTouchesTrivialElements(TheMemory))
      HasConditionalInitAssign = true;
    return;
  }

  // Otherwise, we have a definite init or assign.  Make sure the instruction
  // itself is tagged properly.
  NeedsUpdateForInitState.push_back(UseID);
}

/// Check whether the instruction is an application.
///
/// Looks through certain projections to find the application.
/// If this is done, updates isSelfParameter as appropriate; otherwise,
/// assumes it was properly set by the caller based on which operand
/// was accessed.
static FullApplySite findApply(SILInstruction *I, bool &isSelfParameter) {
  if (auto apply = FullApplySite::isa(I))
    return apply;

  // If this is an OpenExistentialAddrInst in preparation for applying
  // a witness method, analyze its use to make sure, that no mutation of
  // lvalue let constants occurs.
  if (auto *open = dyn_cast<OpenExistentialAddrInst>(I)) {
    for (auto use : open->getUses()) {
      // Stop at the first use in an apply we find.  We assume that we
      // won't find multiple interesting calls.
      if (auto apply = FullApplySite::isa(use->getUser())) {
        // The 'open' could also be a type dependency of the apply, so
        // instead of checking whether 'use' is exactly the self argument,
        // just check whether the self argument is the opened value.
        isSelfParameter =
          apply.hasSelfArgument() &&
          apply.getSelfArgument() == open;
        return apply;
      }
    }
  }

  return FullApplySite();
}

void LifetimeChecker::handleInOutUse(const DIMemoryUse &Use) {
  bool IsSuperInitDone, FailedSelfUse;

  // inout uses are generally straight-forward: the memory must be initialized
  // before the "address" is passed as an l-value.
  if (!isInitializedAtUse(Use, &IsSuperInitDone, &FailedSelfUse)) {
    if (FailedSelfUse) {
      emitSelfConsumedDiagnostic(Use.Inst);
      return;
    }

    auto diagID = diag::variable_inout_before_initialized;
    
    if (isa<AddressToPointerInst>(Use.Inst))
      diagID = diag::variable_addrtaken_before_initialized;

    diagnoseInitError(Use, diagID);
    return;
  }

  // One additional check: 'let' properties may never be passed inout, because
  // they are only allowed to have their initial value set, not a subsequent
  // overwrite.
  for (unsigned i = Use.FirstElement, e = i+Use.NumElements;
       i != e; ++i) {
    if (!TheMemory.isElementLetProperty(i))
      continue;

    std::string PropertyName;
    auto VD = TheMemory.getPathStringToElement(i, PropertyName);

    // Try to produce a specific error message about the inout use.  If this is
    // a call to a method or a mutating property access, indicate that.
    // Otherwise, we produce a generic error.
    FuncDecl *FD = nullptr;
    bool isAssignment = false;
    bool isSelfParameter = (Use.Kind == DIUseKind::InOutSelfArgument);

    auto Apply = findApply(Use.Inst, isSelfParameter);
    if (Apply) {
      // If this is a method application, produce a nice, specific, error.
      if (auto *WMI = dyn_cast<MethodInst>(Apply.getCallee()))
        FD = dyn_cast<FuncDecl>(WMI->getMember().getDecl());
      
      // If this is a direct/devirt method application, check the location info.
      if (auto *Fn = Apply.getReferencedFunctionOrNull()) {
        if (Fn->hasLocation()) {
          auto SILLoc = Fn->getLocation();
          FD = SILLoc.getAsASTNode<FuncDecl>();
        }
      }

      // If we failed to find the decl a clean and principled way, try hacks:
      // map back to the AST and look for some common patterns.
      if (!FD) {
        if (Apply.getLoc().getAsASTNode<AssignExpr>())
          isAssignment = true;
        else if (auto *CE = Apply.getLoc().getAsASTNode<ApplyExpr>()) {
          if (auto *DSCE = dyn_cast<SelfApplyExpr>(CE->getFn()))
            // Normal method calls are curried, so they are:
            // (call_expr (dot_syntax_call_expr (decl_ref_expr METHOD)))
            FD = dyn_cast_or_null<FuncDecl>(DSCE->getCalledValue());
          else
            // Operators and normal function calls are just (CallExpr DRE)
            FD = dyn_cast_or_null<FuncDecl>(CE->getCalledValue());
        }
      }
    }
    
    // If we were able to find a method or function call, emit a diagnostic
    // about the method.  The magic numbers used by the diagnostic are:
    // 0 -> method, 1 -> property, 2 -> subscript, 3 -> operator.
    auto accessor = dyn_cast_or_null<AccessorDecl>(FD);
    if (accessor && isSelfParameter) {
      bool isMutator = [&] {
        switch (accessor->getAccessorKind()) {
        case AccessorKind::Get:
        case AccessorKind::Read:
        case AccessorKind::Address:
          return false;
        case AccessorKind::Set:
        case AccessorKind::Modify:
        case AccessorKind::MutableAddress:
        case AccessorKind::DidSet:
        case AccessorKind::WillSet:
        case AccessorKind::Init:
          return true;
        }
        llvm_unreachable("bad kind");
      }();
      diagnose(Module, Use.Inst->getLoc(),
               isMutator
                 ? diag::mutation_of_property_of_immutable_value
                 : diag::using_mutating_accessor_on_immutable_value,
               accessor->getStorage()->getBaseName(),
               isa<SubscriptDecl>(accessor->getStorage()),
               StringRef(PropertyName));
    } else if (FD && FD->isOperator()) {
      diagnose(Module, Use.Inst->getLoc(),
               diag::mutating_method_called_on_immutable_value,
               FD->getBaseIdentifier(), /*operator*/ 1,
               StringRef(PropertyName));
    } else if (FD && isSelfParameter) {
      diagnose(Module, Use.Inst->getLoc(),
               diag::mutating_method_called_on_immutable_value,
               FD->getBaseIdentifier(), /*method*/ 0, StringRef(PropertyName));
    } else if (isAssignment) {
      diagnose(Module, Use.Inst->getLoc(),
               diag::assignment_to_immutable_value, StringRef(PropertyName));
    } else {
      diagnose(Module, Use.Inst->getLoc(),
               diag::immutable_value_passed_inout, StringRef(PropertyName));
    }

    if (auto *Var = dyn_cast<VarDecl>(VD)) {
      Var->emitLetToVarNoteIfSimple(nullptr);
    }
    return;
  }
}

/// Failable enum initializer produce a CFG for the return that looks like this,
/// where the load is the use of 'self'.  Detect this pattern so we can consider
/// it a 'return' use of self.
///
///   %3 = load %2 : $*Enum
///   %4 = enum $Optional<Enum>, #Optional.Some!enumelt, %3 : $Enum
///   br bb2(%4 : $Optional<Enum>)                    // id: %5
/// bb1:
///   %6 = enum $Optional<Enum>, #Optional.None!enumelt // user: %7
///   br bb2(%6 : $Optional<Enum>)                    // id: %7
/// bb2(%8 : $Optional<Enum>):                        // Preds: bb0 bb1
///   dealloc_stack %1 : $*Enum                       // id: %9
///   return %8 : $Optional<Enum>                     // id: %10
///
static bool isFailableInitReturnUseOfEnum(EnumInst *EI) {
  // Only allow enums forming an optional.
  if (!EI->getType().getOptionalObjectType())
    return false;

  if (!EI->hasOneUse())
    return false;
  auto *BI = dyn_cast<BranchInst>(EI->use_begin()->getUser());
  if (!BI || BI->getNumArgs() != 1)
    return false;

  auto *TargetArg = BI->getDestBB()->getArgument(0);
  if (!TargetArg->hasOneUse())
    return false;
  return isa<ReturnInst>(TargetArg->use_begin()->getUser());
}

/// Given a load instruction, return true iff the result of the load is used
/// in a return instruction directly or is lifted to an optional (i.e., wrapped
/// into .some) and returned. These conditions are used to detect whether the
/// given load instruction is autogenerated for a return from the initializers:
/// `init` or `init?`, respectively. In such cases, the load should not be
/// considered as a use of the value but rather as a part of the return
/// instruction. We emit a specific diagnostic in this case.
static bool isLoadForReturn(SingleValueInstruction *loadInst) {
  bool hasReturnUse = false, hasUnknownUses = false;

  for (auto LoadUse : loadInst->getUses()) {
    auto *User = LoadUse->getUser();
    // Ignore retains of the struct/enum before the return.
    if (isa<RetainValueInst>(User))
      continue;
    if (isa<ReturnInst>(User)) {
      hasReturnUse = true;
      continue;
    }
    if (auto *EI = dyn_cast<EnumInst>(User))
      if (isFailableInitReturnUseOfEnum(EI)) {
        hasReturnUse = true;
        continue;
      }
    hasUnknownUses = true;
    break;
  }
  return hasReturnUse && !hasUnknownUses;
}

void LifetimeChecker::handleEscapeUse(const DIMemoryUse &Use) {

  // The value must be fully initialized at all escape points.  If not, diagnose
  // the error.
  bool SuperInitDone, FailedSelfUse, FullyUninitialized;

  if (isInitializedAtUse(Use, &SuperInitDone, &FailedSelfUse,
                         &FullyUninitialized)) {
    return;
  }

  auto Inst = Use.Inst;

  if (FailedSelfUse) {
    emitSelfConsumedDiagnostic(Inst);
    return;
  }

  // This is a use of an uninitialized value.  Emit a diagnostic.
  if (TheMemory.isDelegatingInit() || TheMemory.isDerivedClassSelfOnly()) {
    if (diagnoseMethodCall(Use, false))
      return;

    if (!shouldEmitError(Inst)) return;

    // If this is a load with a single user that is a return, then this is
    // a return before self.init.   Emit a specific diagnostic.
    if (auto *LI = dyn_cast<LoadInst>(Inst))
      if (isLoadForReturn(LI)) {
        diagnose(Module, Inst->getLoc(),
                 diag::superselfinit_not_called_before_return,
                 (unsigned)TheMemory.isDelegatingInit());
        return;
      }
    if (isa<ReturnInst>(Inst)) {
      diagnose(Module, Inst->getLoc(),
               diag::superselfinit_not_called_before_return,
               (unsigned)TheMemory.isDelegatingInit());
      return;
    }

    if (!TheMemory.isClassInitSelf()) {
      // If this is a copy_addr into the indirect result, then we're looking at
      // the implicit "return self" in an address-only initializer.  Emit a
      // specific diagnostic.
      if (auto *CA = dyn_cast<CopyAddrInst>(Inst)) {
        if (CA->isInitializationOfDest() &&
            !CA->getFunction()->getArguments().empty() &&
            SILValue(CA->getFunction()->getArgument(0)) == CA->getDest()) {
          diagnose(Module, Inst->getLoc(),
                   diag::superselfinit_not_called_before_return,
                   (unsigned)TheMemory.isDelegatingInit());
          return;
        }
      }
    }

    if (TheMemory.isDelegatingInit()) {
      if (TheMemory.isClassInitSelf()) {
        diagnose(Module, Inst->getLoc(), diag::self_before_selfinit);
      } else {
        diagnose(Module, Inst->getLoc(), diag::self_before_selfinit_value_type);
        if (!HasSuggestedNoArgSelfInit && FullyUninitialized) {
          auto *maybeStruct =
              TheMemory.getASTType().getStructOrBoundGenericStruct();
          maybeSuggestNoArgSelfInit(Module, Inst->getLoc(), maybeStruct);
          HasSuggestedNoArgSelfInit = true;
        }
      }
    } else {
      diagnose(Module, Inst->getLoc(), diag::self_before_superinit);
    }
    return;
  }

  if (isa<ApplyInst>(Inst) && TheMemory.isAnyInitSelf() &&
      !TheMemory.isClassInitSelf()) {
    if (!shouldEmitError(Inst)) return;

    diagnose(Module, Inst->getLoc(), diag::use_of_self_before_fully_init);
    noteUninitializedMembers(Use);
    return;
  }
  
  if (isa<PartialApplyInst>(Inst) && TheMemory.isClassInitSelf()) {
    if (!shouldEmitError(Inst)) return;
    
    diagnose(Module, Inst->getLoc(), diag::self_closure_use_uninit);
    noteUninitializedMembers(Use);
    return;
  }

  // Extract the reason why this escape-use instruction exists and present
  // diagnostics. While an escape-use instruction generally corresponds to a
  // capture by a closure, there are the following special cases to consider:
  //
  // (a) A MarkFunctionEscapeInst with an operand say %var. This is introduced
  // by the SILGen phase when %var is the address of a global variable that
  // escapes because it is used by a closure or a defer statement or a function
  // definition appearing at the top-level. The specific reason why %var escapes
  // is recorded in MarkFunctionEscapeInst by making its SIL Location refer to
  // the AST of the construct that uses the global variable (namely, a closure
  // or a defer statement or a function definition). So, if %var is
  // uninitialized at MarkFunctionEscapeInst, extract and report the reason
  // why the variable escapes in the error message.
  //
  // (b) An UncheckedTakeEnumDataAddrInst takes the address of the data of
  // an optional and is introduced as an intermediate step in optional chaining.
  Diag<StringRef, bool> DiagMessage;
  if (isa<MarkFunctionEscapeInst>(Inst)) {
    if (Inst->getLoc().isASTNode<AbstractClosureExpr>()) {
      DiagMessage = diag::variable_closure_use_uninit;
    } else if (Inst->getLoc().isASTNode<DeferStmt>()) {
      DiagMessage = diag::variable_defer_use_uninit;
    } else {
      DiagMessage = diag::variable_function_use_uninit;
    }
  } else if (isa<UncheckedTakeEnumDataAddrInst>(Inst)) {
    DiagMessage = diag::variable_used_before_initialized;
  } else {
    DiagMessage = diag::variable_closure_use_uninit;
  }

  diagnoseInitError(Use, DiagMessage);
}

enum BadSelfUseKind {
  BeforeStoredPropertyInit,
  BeforeSuperInit,
  BeforeSelfInit
};

void LifetimeChecker::diagnoseRefElementAddr(RefElementAddrInst *REI) {
  if (!shouldEmitError(REI)) return;
  
  auto Kind = (TheMemory.isAnyDerivedClassSelf()
               ? BeforeSuperInit
               : BeforeSelfInit);
  diagnose(Module, REI->getLoc(),
           diag::self_use_before_fully_init,
           REI->getField()->getName(), true, Kind);
}

template <typename T>
static FuncDecl *
findMethodForStoreInitializationOfTemporary(const DIMemoryObjectInfo &TheMemory,
                                            T *SI) {
  // We unconditionally strip borrow since a store must take a consuming
  // argument, so the ownership verifier would trip. So we know that such a
  // thing can not happen. On the other hand, for store_borrow, we need to
  // strip the borrow, so lets use idempotence for correctness.
  if (stripBorrow(SI->getSrc()) != TheMemory.getUninitializedValue() ||
      !isa<AllocStackInst>(SI->getDest()) || !TheMemory.isClassInitSelf()) {
    return nullptr;
  }

  ApplyInst *TheApply = nullptr;

  auto addr =
      isa<StoreBorrowInst>(SI) ? cast<StoreBorrowInst>(SI) : SI->getDest();
  // Check to see if the address of the alloc_stack is only passed to one
  // apply_inst and gather the apply while we are at it.
  for (auto UI : addr->getUses()) {
    if (auto *ApplyUser = dyn_cast<ApplyInst>(UI->getUser())) {
      if (TheApply || UI->getOperandNumber() != 1) {
        return nullptr;
      }
      TheApply = ApplyUser;
    }
  }

  // If we didn't find an apply, just return nullptr. This isn't our pattern.
  if (!TheApply)
    return nullptr;

  // Otherwise, try to get the func decl from the referenced function if we can
  // find one.
  auto *Fn = TheApply->getReferencedFunctionOrNull();
  if (!Fn->hasLocation())
    return nullptr;

  return Fn->getLocation().getAsASTNode<FuncDecl>();
}

bool LifetimeChecker::diagnoseMethodCall(const DIMemoryUse &Use,
                                         bool SuperInitDone) {
  SILInstruction *Inst = Use.Inst;

  // All of these cases imply that Inst as at +0.
  if (auto *REI = dyn_cast<RefElementAddrInst>(Inst)) {
    diagnoseRefElementAddr(REI);
    return true;
  }

  // Check to see if this is a use of self or super, due to a method call.  If
  // so, emit a specific diagnostic.
  FuncDecl *Method = nullptr;

  // Check for an access to the base class through a borrow+cast.
  if (auto *BBI = dyn_cast<BeginBorrowInst>(Inst)) {
    llvm::SmallVector<Operand *, 8> Worklist(BBI->use_begin(), BBI->use_end());
    while (!Worklist.empty()) {
      auto *BBIOp = Worklist.pop_back_val();
      auto *BBIOpUser = BBIOp->getUser();

      // Skip over end_borrow.
      if (isa<EndBorrowInst>(BBIOpUser))
        continue;

      // Look through upcasts.
      if (auto upcast = dyn_cast<UpcastInst>(BBIOpUser)) {
        std::copy(upcast->use_begin(), upcast->use_end(),
                  std::back_inserter(Worklist));
        continue;
      }

      // Look through unchecked_ref_cast.
      if (auto cast = dyn_cast<UncheckedRefCastInst>(BBIOpUser)) {
        std::copy(cast->use_begin(), cast->use_end(),
                  std::back_inserter(Worklist));
        continue;
      }

      // If we have a ref_element_addr, then perform the diagnosis.
      if (auto *REI = dyn_cast<RefElementAddrInst>(BBIOpUser)) {
        diagnoseRefElementAddr(REI);
        return true;
      }

      // If we were not able to find a better error, return false.
      return false;
    }
  }

  if (auto UCI = dyn_cast<UpcastInst>(Inst)) {
    // If the upcast is used by a ref_element_addr, then it is an access to a
    // base ivar before super.init is called.
    if (UCI->hasOneUse() && !SuperInitDone) {
      if (auto *REI =
          dyn_cast<RefElementAddrInst>((*UCI->use_begin())->getUser())) {
        diagnoseRefElementAddr(REI);
        return true;
      }
    }

    // If the upcast is used by a class_method + apply, then this is a call of a
    // superclass method or property accessor. If we have a guaranteed method,
    // we will have a release due to a missing optimization in SILGen that will
    // be removed.
    //
    // TODO: Implement the SILGen fixes so this can be removed.
    MethodInst *MI = nullptr;
    ApplyInst *AI = nullptr;
    SILInstruction *Release = nullptr;
    for (auto UI : UCI->getUses()) {
      auto *User = UI->getUser();
      if (auto *TAI = dyn_cast<ApplyInst>(User)) {
        if (!AI) {
          AI = TAI;
          continue;
        }
      }
      if (auto *CMI = dyn_cast<ClassMethodInst>(User)) {
        if (!MI) {
          MI = CMI;
          continue;
        }
      }

      if (auto *OMI = dyn_cast<ObjCMethodInst>(User)) {
        if (!MI) {
          MI = OMI;
          continue;
        }
      }

      if (isa<ReleaseValueInst>(User) || isa<StrongReleaseInst>(User)) {
        if (!Release) {
          Release = User;
          continue;
        }
      }

      // Not a pattern we recognize, conservatively generate a generic
      // diagnostic.
      MI = nullptr;
      break;
    }

    // If we have a release, make sure that AI is guaranteed. If it is not, emit
    // the generic error that we would emit before.
    //
    // That is the only case where we support pattern matching a release.
    if (Release && AI /*
        && (!AI->getSubstCalleeType()->hasSelfParam()
            || !AI->getSubstCalleeType()->getSelfParameter().isGuaranteed())*/)
      MI = nullptr;

    if (AI && MI) {
      // TODO: Could handle many other members more specifically.
      Method = dyn_cast<FuncDecl>(MI->getMember().getDecl());
    }
  }

  // If this is an apply instruction and we're in a class initializer, we're
  // calling a method on self.
  if (isa<ApplyInst>(Inst) && TheMemory.isClassInitSelf()) {
    // If this is a method application, produce a nice, specific, error.
    if (auto *CMI = dyn_cast<ClassMethodInst>(Inst->getOperand(0)))
      Method = dyn_cast<FuncDecl>(CMI->getMember().getDecl());

    if (auto *OMI = dyn_cast<ObjCMethodInst>(Inst->getOperand(0)))
      Method = dyn_cast<FuncDecl>(OMI->getMember().getDecl());

    // If this is a direct/devirt method application, check the location info.
    if (auto *Fn = cast<ApplyInst>(Inst)->getReferencedFunctionOrNull()) {
      if (Fn->hasLocation())
        Method = Fn->getLocation().getAsASTNode<FuncDecl>();
    }
  }
  
  // If this is part of a call to a witness method for a non-class-bound
  // protocol in a root class, then we could have a store to a temporary whose
  // address is passed into an apply.  Look through this pattern.
  if (auto *SI = dyn_cast<StoreInst>(Inst)) {
    Method = findMethodForStoreInitializationOfTemporary(TheMemory, SI);
  }

  if (auto *SI = dyn_cast<StoreBorrowInst>(Inst)) {
    Method = findMethodForStoreInitializationOfTemporary(TheMemory, SI);
  }

  // If we were able to find a method call, emit a diagnostic about the method.
  if (Method) {
    if (!shouldEmitError(Inst)) return true;

    DeclBaseName Name;
    if (auto accessor = dyn_cast<AccessorDecl>(Method))
      Name = accessor->getStorage()->getBaseName();
    else
      Name = Method->getBaseIdentifier();

    // If this is a use of self before super.init was called, emit a diagnostic
    // about *that* instead of about individual properties not being
    // initialized.
    auto Kind = (SuperInitDone
                 ? BeforeStoredPropertyInit
                 : (TheMemory.isAnyDerivedClassSelf()
                    ? BeforeSuperInit
                    : BeforeSelfInit));
    diagnose(Module, Inst->getLoc(), diag::self_use_before_fully_init,
             Name, isa<AccessorDecl>(Method), Kind);

    if (SuperInitDone)
      noteUninitializedMembers(Use);
    return true;
  }

  return false;
}

bool LifetimeChecker::diagnoseReturnWithoutInitializingStoredProperties(
    const SILInstruction *Inst, SILLocation loc, const DIMemoryUse &Use) {
  if (!TheMemory.isAnyInitSelf())
    return false;
  if (TheMemory.isClassInitSelf() || TheMemory.isDelegatingInit())
    return false;

  if (!shouldEmitError(Inst))
    return true;

  if (TheMemory.isCrossModuleStructInitSelf() && TheMemory.hasDummyElement()) {
    Type selfTy = TheMemory.getASTType();
    const StructDecl *theStruct = selfTy->getStructOrBoundGenericStruct();
    assert(theStruct);

    bool fullyUnitialized;
    (void)isInitializedAtUse(Use, nullptr, nullptr, &fullyUnitialized);

    diagnose(Module, loc,
             diag::designated_init_in_cross_module_extension,
             selfTy, !fullyUnitialized,
             theStruct->getParentModule()->getName(),
             theStruct->hasClangNode());
  } else {
    diagnose(Module, loc,
             diag::return_from_init_without_initing_stored_properties);
    noteUninitializedMembers(Use);
  }

  return true;
}

/// Check and diagnose various failures when a load use is not fully
/// initialized.
///
/// TODO: In the "No" case, we can emit a fixit adding a default initialization
/// of the type.
void LifetimeChecker::handleLoadUseFailure(const DIMemoryUse &Use,
                                           bool SuperInitDone,
                                           bool FailedSelfUse) {
  SILInstruction *Inst = Use.Inst;

  // Stores back to the 'self' box are OK.
  if (auto store = dyn_cast<StoreInst>(Inst)) {
    if (store->getDest() == TheMemory.getUninitializedValue() &&
        TheMemory.isClassInitSelf())
      return;
  }

  if (FailedSelfUse) {
    emitSelfConsumedDiagnostic(Inst);
    return;
  }
  
  // If this is a load with a single user that is a return (and optionally a
  // retain_value for non-trivial structs/enums), then this is a return in the
  // enum/struct init case, and we haven't stored to self.   Emit a specific
  // diagnostic.
  if (isa<LoadInst>(Inst) || isa<LoadBorrowInst>(Inst)) {
    auto *LI = Inst;
    // If this load is part of a return sequence, diagnose it specially.
    if (isLoadForReturn(cast<SingleValueInstruction>(LI))) {
      // The load is probably part of the common epilog for the function, try to
      // find a more useful source location than the syntactic end of the
      // function.
      SILLocation returnLoc = Inst->getLoc();
      auto TermLoc = Inst->getParent()->getTerminator()->getLoc();
      if (TermLoc.getKind() == SILLocation::ReturnKind) {
        // Function has a single return that got merged into the epilog block.
        returnLoc = TermLoc;
      } else {
        // Otherwise, there are multiple paths to the epilog block, scan its
        // predecessors to see if there are any where the value is unavailable.
        // If so, we can use its location information for more precision.
        for (auto pred : LI->getParent()->getPredecessorBlocks()) {
          auto *TI = pred->getTerminator();
          // Check if this is an early return with uninitialized members.
          if (TI->getLoc().getKind() == SILLocation::ReturnKind &&
              getAnyUninitializedMemberAtInst(TI, Use.FirstElement,
                                              Use.NumElements) != -1)
            returnLoc = TI->getLoc();
        }
      }
      
      if (diagnoseReturnWithoutInitializingStoredProperties(Inst, returnLoc,
                                                            Use)) {
        return;
      }
    }
  }
  
  // If this is a copy_addr into the 'self' argument, and the memory object is a
  // rootself struct/enum or a non-delegating initializer, then we're looking at
  // the implicit "return self" in an address-only initializer.  Emit a specific
  // diagnostic.
  if (auto *CA = dyn_cast<CopyAddrInst>(Inst)) {
    if (CA->isInitializationOfDest() &&
        !CA->getFunction()->getArguments().empty() &&
        SILValue(CA->getFunction()->getArgument(0)) == CA->getDest()) {
      if (diagnoseReturnWithoutInitializingStoredProperties(Inst,
                                                            Inst->getLoc(),
                                                            Use)) {
        return;
      }
    }
  }

  // Check to see if we're returning self in a class initializer before all the
  // ivars/super.init are set up.
  if (isa<ReturnInst>(Inst) && TheMemory.isAnyInitSelf()) {
    if (!shouldEmitError(Inst)) return;
    if (!SuperInitDone) {
      diagnose(Module, Inst->getLoc(),
               diag::superselfinit_not_called_before_return,
               (unsigned)TheMemory.isDelegatingInit());
    } else {
      diagnose(Module, Inst->getLoc(),
               diag::return_from_init_without_initing_stored_properties);
      noteUninitializedMembers(Use);
    }
    return;
  }

  // Check to see if this is a use of self or super, due to a method call.  If
  // so, emit a specific diagnostic.
  if (diagnoseMethodCall(Use, SuperInitDone))
    return;

  // Otherwise, we couldn't find a specific thing to complain about, so emit a
  // generic error, depending on what kind of failure this is.
  if (!SuperInitDone) {
    if (!shouldEmitError(Inst)) return;
    if (TheMemory.isDelegatingInit()) {
      if (TheMemory.isClassInitSelf()) {
        diagnose(Module, Inst->getLoc(), diag::self_before_selfinit);
      } else {
        diagnose(Module, Inst->getLoc(), diag::self_before_selfinit_value_type);
      }
    } else {
      diagnose(Module, Inst->getLoc(), diag::self_before_superinit);
    }
    return;
  }

  // If this is a call to a method in a class initializer, then it must be a use
  // of self before the stored properties are set up.
  if (isa<ApplyInst>(Inst) && TheMemory.isClassInitSelf()) {
    if (!shouldEmitError(Inst)) return;
    diagnose(Module, Inst->getLoc(), diag::use_of_self_before_fully_init);
    noteUninitializedMembers(Use);
    return;
  }

  // If this is a load of self in a struct/enum/protocol initializer, then it
  // must be a use of 'self' before all the stored properties are set up.
  if ((isa<LoadInst>(Inst) || isa<LoadBorrowInst>(Inst)) &&
      TheMemory.isAnyInitSelf() && !TheMemory.isClassInitSelf()) {
    if (!shouldEmitError(Inst)) return;

    diagnose(Module, Inst->getLoc(), diag::use_of_self_before_fully_init);
    noteUninitializedMembers(Use);
    return;
  }
  
  // If this is a load into a promoted closure capture, diagnose properly as
  // a capture.
  if ((isa<LoadInst>(Inst) || isa<LoadBorrowInst>(Inst)) &&
      Inst->getLoc().isASTNode<AbstractClosureExpr>())
    diagnoseInitError(Use, diag::variable_closure_use_uninit);
  else
    diagnoseInitError(Use, diag::variable_used_before_initialized);
}

/// handleSelfInitUse - When processing a 'self' argument on a class, this is
/// a call to self.init or super.init.
void LifetimeChecker::handleSelfInitUse(unsigned UseID) {
  auto &Use = Uses[UseID];
  auto *Inst = Use.Inst;

  assert(TheMemory.isAnyInitSelf());
  assert(!TheMemory.isClassInitSelf() || TheMemory.isNonRootClassSelf());
  assert(TheMemory.getASTType()->hasReferenceSemantics());

  // Determine the liveness states of the memory object, including the
  // self/super.init state.
  AvailabilitySet Liveness =
      getLivenessAtInst(Inst, 0, TheMemory.getNumElements());

  // self/super.init() calls require that self/super.init has not already
  // been called. If it has, reject the program.
  switch (Liveness.get(TheMemory.getNumElements() - 1)) {
  case DIKind::No:  // This is good! Keep going.
    break;
  case DIKind::Yes:
  case DIKind::Partial:
    // This is bad, only one super.init call is allowed.
    if (getSelfInitializedAtInst(Inst) != DIKind::Yes) {
      emitSelfConsumedDiagnostic(Inst);
      return;
    }

    if (shouldEmitError(Inst))
      diagnose(Module, Inst->getLoc(), diag::selfinit_multiple_times,
               TheMemory.isDelegatingInit());
    return;
  }

  if (TheMemory.isDelegatingInit()) {
    assert(TheMemory.getNumElements() == 1 &&
           "delegating inits have a single elt");

    // Lower Assign instructions if needed.
    if (isa<AssignInst>(Use.Inst) || isa<AssignByWrapperInst>(Use.Inst) ||
        isa<AssignOrInitInst>(Use.Inst))
      NeedsUpdateForInitState.push_back(UseID);
  } else {
    // super.init also requires that all ivars are initialized before the
    // superclass initializer runs.
    for (unsigned i = 0, e = TheMemory.getNumElements() - 1; i != e; ++i) {
      if (Liveness.get(i) == DIKind::Yes) continue;

      // If the super.init call is implicit generated, produce a specific
      // diagnostic.
      bool isImplicit = Use.Inst->getLoc().getSourceLoc().isInvalid();
      auto diag = isImplicit ? diag::ivar_not_initialized_at_implicit_superinit :
                  diag::ivar_not_initialized_at_superinit;
      return diagnoseInitError(Use, diag);
    }

    // Otherwise everything is good!
  }
}

// In case of `var` initializations, SILGen creates a dynamic begin/end_access
// pair around the initialization store. If it's an initialization (and not
// a re-assign) it's guaranteed that it's an exclusive access and we can
// convert the access to an `[init] [static]` access.
static void setStaticInitAccess(SILValue memoryAddress) {
  if (auto *ba = dyn_cast<BeginAccessInst>(memoryAddress)) {
    if (ba->getEnforcement() == SILAccessEnforcement::Dynamic) {
      ba->setEnforcement(SILAccessEnforcement::Static);
      if (ba->getAccessKind() == SILAccessKind::Modify)
        ba->setAccessKind(SILAccessKind::Init);
    }
  }
}

/// updateInstructionForInitState - When an instruction being analyzed moves
/// from being InitOrAssign to some concrete state, update it for that state.
/// This includes rewriting them from assign instructions into their composite
/// operations.
void LifetimeChecker::updateInstructionForInitState(unsigned UseID) {
  DIMemoryUse &Use = Uses[UseID];
  SILInstruction *Inst = Use.Inst;

  IsInitialization_t InitKind;
  if (Use.Kind == DIUseKind::Initialization ||
      Use.Kind == DIUseKind::SelfInit)
    InitKind = IsInitialization;
  else {
    assert(Use.Kind == DIUseKind::Assign || Use.Kind == DIUseKind::Set);
    InitKind = IsNotInitialization;
  }

  // If this is a copy_addr or store_weak, we just set the initialization bit
  // depending on what we find.
  if (auto *CA = dyn_cast<CopyAddrInst>(Inst)) {
    assert(!CA->isInitializationOfDest() &&
           "should not modify copy_addr that already knows it is initialized");
    CA->setIsInitializationOfDest(InitKind);
    if (InitKind == IsInitialization)
      setStaticInitAccess(CA->getDest());

    // If we had an initialization and had an assignable_but_not_consumable
    // noncopyable type, convert it to be an initable_but_not_consumable so that
    // we do not consume an uninitialized value.
    if (InitKind == IsInitialization) {
      if (auto *mmci =
          dyn_cast<MarkMustCheckInst>(stripAccessMarkers(CA->getDest()))) {
        if (mmci->getCheckKind() ==
            MarkMustCheckInst::CheckKind::AssignableButNotConsumable) {
          mmci->setCheckKind(
              MarkMustCheckInst::CheckKind::InitableButNotConsumable);
        }
      }
    }

    return;
  }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, name, ...) \
  if (auto *SW = dyn_cast<Store##Name##Inst>(Inst)) { \
    if (SW->isInitializationOfDest()) \
           llvm_unreachable("should not modify store_" #name \
                            " that already knows it is initialized"); \
    SW->setIsInitializationOfDest(InitKind); \
    return; \
  }
#include "swift/AST/ReferenceStorage.def"
  
  // If this is an assign, rewrite it based on whether it is an initialization
  // or not.
  if (auto *AI = dyn_cast<AssignInst>(Inst)) {
    // Remove this instruction from our data structures, since we will be
    // removing it.
    Use.Inst = nullptr;
    llvm::erase_if(NonLoadUses[Inst], [&](unsigned id) { return id == UseID; });

    if (TheMemory.isClassInitSelf() &&
        Use.Kind == DIUseKind::SelfInit) {
      assert(InitKind == IsInitialization);
      AI->setOwnershipQualifier(AssignOwnershipQualifier::Reinit);
    } else {
      AI->setOwnershipQualifier((InitKind == IsInitialization
                                ? AssignOwnershipQualifier::Init
                                : AssignOwnershipQualifier::Reassign));
    }

    // Look and see if we are assigning a moveonly type into a mark_must_check
    // [assignable_but_not_consumable]. If we are, then we need to transition
    // its flag to initable_but_not_assignable.
    //
    // NOTE: We should only ever have to do this for a single level since SILGen
    // always initializes values completely and we enforce that invariant.
    if (InitKind == IsInitialization) {
      if (auto *mmci =
              dyn_cast<MarkMustCheckInst>(stripAccessMarkers(AI->getDest()))) {
        if (mmci->getCheckKind() ==
                MarkMustCheckInst::CheckKind::AssignableButNotConsumable) {
          mmci->setCheckKind(
              MarkMustCheckInst::CheckKind::InitableButNotConsumable);
        }
      }
      setStaticInitAccess(AI->getDest());
    }

    return;
  }

  if (auto *AI = dyn_cast<AssignOrInitInst>(Inst)) {
    // Remove this instruction from our data structures, since we will be
    // removing it.
    Use.Inst = nullptr;
    llvm::erase_if(NonLoadUses[Inst], [&](unsigned id) { return id == UseID; });

    switch (Use.Kind) {
    case DIUseKind::Assign:
      AI->markAsInitialized(Use.Field.get());
      LLVM_FALLTHROUGH;
    case DIUseKind::Initialization:
      AI->setMode(AssignOrInitInst::Init);
      break;
    case DIUseKind::Set:
      AI->setMode(AssignOrInitInst::Set);
      break;
    default:
      llvm_unreachable("Wrong use kind for assign_or_init");
    }

    return;
  }

  if (auto *AI = dyn_cast<AssignByWrapperInst>(Inst)) {
    // Remove this instruction from our data structures, since we will be
    // removing it.
    Use.Inst = nullptr;
    llvm::erase_if(NonLoadUses[Inst], [&](unsigned id) { return id == UseID; });

    switch (Use.Kind) {
    case DIUseKind::Initialization:
      AI->setMode(AssignByWrapperInst::Initialization);
      break;
    case DIUseKind::Assign:
      AI->setMode(AssignByWrapperInst::Assign);
      break;
    case DIUseKind::Set:
      AI->setMode(AssignByWrapperInst::AssignWrappedValue);
      break;
    default:
      llvm_unreachable("Wrong use kind for assign_by_wrapper");
    }

    return;
  }

  // Ignore non-stores for SelfInits.
  assert(isa<StoreInst>(Inst) && "Unknown store instruction!");
}

void LifetimeChecker::processUninitializedReleaseOfBox(
    MarkUninitializedInst *MUI, SILInstruction *Release, bool consumed,
    SILBasicBlock::iterator InsertPt) {
  assert(isa<AllocBoxInst>(MUI->getOperand()));
  assert(MUI == Release->getOperand(0));
  SILBuilderWithScope B(Release);
  B.setInsertionPoint(InsertPt);
  Destroys.push_back(B.createDeallocBox(Release->getLoc(), MUI));
}

static void emitDefaultActorDestroy(SILBuilder &B, SILLocation loc,
                                    SILValue self) {
  auto builtinName = B.getASTContext().getIdentifier(
    getBuiltinName(BuiltinValueKind::DestroyDefaultActor));
  auto resultTy = B.getModule().Types.getEmptyTupleType();

  self = B.createBeginBorrow(loc, self);
  B.createBuiltin(loc, builtinName, resultTy, /*subs*/{},
                  { self });
  B.createEndBorrow(loc, self);
}

void LifetimeChecker::processUninitializedRelease(SILInstruction *Release,
                                                  bool consumed,
                                             SILBasicBlock::iterator InsertPt) {
  // If this is an early release of a class instance, we need to emit a
  // dealloc_partial_ref to free the memory.  If this is a derived class, we
  // may have to do a load of the 'self' box to get the class reference.
  if (!TheMemory.isClassInitSelf()) {
    if (auto *MUI = dyn_cast<MarkUninitializedInst>(Release->getOperand(0))) {
      if (isa<AllocBoxInst>(MUI->getOperand())) {
        return processUninitializedReleaseOfBox(MUI, Release, consumed, InsertPt);
      }
    }
    return;
  }

  auto Loc = Release->getLoc();

  SILBuilderWithScope B(Release);
  B.setInsertionPoint(InsertPt);

  SILValue Pointer = Release->getOperand(0);

  // If we see an alloc_box as the pointer, then we're deallocating a 'box' for
  // self. Make sure that the box gets deallocated (not released) since the
  // pointer it contains will be manually cleaned up.
  auto *MUI = dyn_cast<MarkUninitializedInst>(Release->getOperand(0));

  if (MUI && isa<AllocBoxInst>(MUI->getOperand())) {
    Pointer = MUI->getSingleUserOfType<ProjectBoxInst>();
    assert(Pointer);
  } else {
    MUI = nullptr;
  }

  if (!consumed) {
    if (Pointer->getType().isAddress())
      Pointer = B.createLoad(Loc, Pointer, LoadOwnershipQualifier::Take);

    auto MetatypeTy = CanMetatypeType::get(TheMemory.getASTType(),
                                           MetatypeRepresentation::Thick);
    auto SILMetatypeTy = SILType::getPrimitiveObjectType(MetatypeTy);
    SILValue Metatype;

    // In an inherited convenience initializer, we must use the dynamic
    // type of the object since nothing is initialized yet.
    if (TheMemory.isDelegatingInit())
      Metatype = B.createValueMetatype(Loc, SILMetatypeTy, Pointer);
    else
      Metatype = B.createMetatype(Loc, SILMetatypeTy);

    // If this is a root default actor, destroy the default-actor state.
    // SILGen ensures that this is unconditionally initialized, so we
    // don't need to track it specially.
    if (!TheMemory.isDelegatingInit()) {
      auto classDecl = TheMemory.getASTType().getClassOrBoundGenericClass();
      if (classDecl && classDecl->isRootDefaultActor()) {
          emitDefaultActorDestroy(B, Loc, Pointer);
      }
    }

    // We've already destroyed any instance variables initialized by this
    // constructor, now destroy instance variables initialized by subclass
    // constructors that delegated to us, and finally free the memory.
    B.createDeallocPartialRef(Loc, Pointer, Metatype);
  }

  // dealloc_box the self box if necessary.
  if (MUI) {
    auto DB = B.createDeallocBox(Loc, MUI);
    Destroys.push_back(DB);
  }
}

void LifetimeChecker::deleteDeadRelease(unsigned ReleaseID) {
  SILInstruction *Release = Destroys[ReleaseID];
  if (isa<DestroyAddrInst>(Release)) {
    SILValue Addr = Release->getOperand(0);
    if (auto *AddrI = Addr->getDefiningInstruction()) {
      // FIXME: AddrI will not be deleted (nor its operands) when Release is
      // still using AddrI's result. Fix this, and migrate to using
      // InstructionDeleter utility instead of
      // recursivelyDeadTriviallyDeadInstructions.
      recursivelyDeleteTriviallyDeadInstructions(AddrI);
    }
  }
  Release->eraseFromParent();
  Destroys[ReleaseID] = nullptr;
}

/// processNonTrivialRelease - We handle two kinds of release instructions here:
/// destroy_addr for alloc_stack's and strong_release/dealloc_box for
/// alloc_box's.  By the time that DI gets here, we've validated that all uses
/// of the memory location are valid.  Unfortunately, the uses being valid
/// doesn't mean that the memory is actually initialized on all paths leading to
/// a release.  As such, we have to push the releases up the CFG to where the
/// value is initialized.
///
void LifetimeChecker::processNonTrivialRelease(unsigned ReleaseID) {
  SILInstruction *Release = Destroys[ReleaseID];
  
  // If the instruction is a deallocation of uninitialized memory, no action is
  // required (or desired).
  if (isa<DeallocStackInst>(Release) || isa<DeallocBoxInst>(Release) ||
      isa<DeallocRefInst>(Release) || isa<DeallocPartialRefInst>(Release))
    return;

  // We only handle strong_release, destroy_value, and destroy_addr here.  The
  // former is a
  // release of a class in an initializer, the later is used for local variable
  // destruction.
  assert(isa<StrongReleaseInst>(Release) || isa<DestroyValueInst>(Release) ||
         isa<DestroyAddrInst>(Release));

  auto Availability = getLivenessAtInst(Release, 0, TheMemory.getNumElements());
  DIKind SelfInitialized = DIKind::Yes;

  if (TheMemory.isNonRootClassSelf()) {
    SelfInitialized = getSelfInitializedAtInst(Release);

    if (SelfInitialized == DIKind::Yes) {
      assert(Availability.isAllYes() &&
             "Should not store 'self' with uninitialized members into the box");
    }
  }

  // If the memory object is completely initialized, then nothing needs to be
  // done at this release point.
  if (Availability.isAllYes() && SelfInitialized == DIKind::Yes)
    return;

  if (Availability.isAllYes() && SelfInitialized == DIKind::No) {
    // We're in an error path after performing a self.init or super.init
    // delegation. The value was already consumed so there's nothing to release.
    processUninitializedRelease(Release, true, Release->getIterator());
    deleteDeadRelease(ReleaseID);
    return;
  }

  // If it is all 'no' then we can handle it specially without conditional code.
  if (Availability.isAllNo() && SelfInitialized == DIKind::No) {
    processUninitializedRelease(Release, false, Release->getIterator());
    deleteDeadRelease(ReleaseID);
    return;
  }

  // Otherwise, it is partially live.

  // If any elements or the 'super.init' state are conditionally live, we need
  // to emit conditional logic.
  if (Availability.hasAny(DIKind::Partial))
    HasConditionalDestroy = true;

  // If the self value was conditionally consumed, we need to emit conditional
  // logic.
  if (SelfInitialized == DIKind::Partial)
    HasConditionalSelfInitialized = true;

  // Save it for later processing.
  ConditionalDestroys.push_back({ ReleaseID, Availability, SelfInitialized });
}

static Identifier getBinaryFunction(StringRef Name, SILType IntSILTy,
                                    ASTContext &C) {
  auto IntTy = IntSILTy.castTo<BuiltinIntegerType>();
  unsigned NumBits = IntTy->getWidth().getFixedWidth();
  // Name is something like: add_Int64
  std::string NameStr(Name);
  NameStr += "_Int" + llvm::utostr(NumBits);

  return C.getIdentifier(NameStr);
}
static Identifier getTruncateToI1Function(SILType IntSILTy, ASTContext &C) {
  auto IntTy = IntSILTy.castTo<BuiltinIntegerType>();
  unsigned NumBits = IntTy->getWidth().getFixedWidth();

  // Name is something like: trunc_Int64_Int8
  std::string NameStr = "trunc_Int" + llvm::utostr(NumBits) + "_Int1";
  return C.getIdentifier(NameStr);
}

/// Set a bit in the control variable at the current insertion point.
static void updateControlVariable(SILLocation Loc,
                                  const APInt &Bitmask,
                                  SILValue ControlVariable,
                                  Identifier &OrFn,
                                  SILBuilder &B) {
  SILType IVType = ControlVariable->getType().getObjectType();

  // Get the integer constant.
  SILValue MaskVal = B.createIntegerLiteral(Loc, IVType, Bitmask);
  
  // If the mask is all ones, do a simple store, otherwise do a
  // load/or/store sequence to mask in the bits.
  if (!Bitmask.isAllOnesValue()) {
    SILValue Tmp =
        B.createLoad(Loc, ControlVariable, LoadOwnershipQualifier::Trivial);
    if (!OrFn.get())
      OrFn = getBinaryFunction("or", IVType, B.getASTContext());
      
    SILValue Args[] = { Tmp, MaskVal };
    MaskVal = B.createBuiltin(Loc, OrFn, IVType, {}, Args);
  }

  B.createStore(Loc, MaskVal, ControlVariable,
                StoreOwnershipQualifier::Trivial);
}

/// Test a bit in the control variable at the current insertion point.
static SILValue testControlVariableBit(SILLocation Loc,
                                       unsigned Elt,
                                       SILValue ControlVariableAddr,
                                       Identifier &ShiftRightFn,
                                       Identifier &TruncateFn,
                                       SILBuilder &B) {
  SILValue ControlVariable =
        B.createLoad(Loc, ControlVariableAddr, LoadOwnershipQualifier::Trivial);

  SILValue CondVal = ControlVariable;
  CanBuiltinIntegerType IVType = CondVal->getType().castTo<BuiltinIntegerType>();

  // If this memory object has multiple tuple elements, we need to make sure
  // to test the right one.
  if (IVType->getFixedWidth() == 1)
    return CondVal;

  // Shift the mask down to this element.
  if (Elt != 0) {
    if (!ShiftRightFn.get())
      ShiftRightFn = getBinaryFunction("lshr", CondVal->getType(),
                                       B.getASTContext());
    SILValue Amt = B.createIntegerLiteral(Loc, CondVal->getType(), Elt);
    SILValue Args[] = { CondVal, Amt };
    
    CondVal = B.createBuiltin(Loc, ShiftRightFn,
                              CondVal->getType(), {},
                              Args);
  }
  
  if (!TruncateFn.get())
    TruncateFn = getTruncateToI1Function(CondVal->getType(),
                                         B.getASTContext());
  return B.createBuiltin(Loc, TruncateFn,
                         SILType::getBuiltinIntegerType(1, B.getASTContext()),
                         {}, CondVal);
}

/// Test if all bits in the control variable are set at the current
/// insertion point.
static SILValue testAllControlVariableBits(SILLocation Loc,
                                           SILValue ControlVariableAddr,
                                           Identifier &CmpEqFn,
                                           SILBuilder &B) {
  SILValue ControlVariable =
        B.createLoad(Loc, ControlVariableAddr, LoadOwnershipQualifier::Trivial);

  SILValue CondVal = ControlVariable;
  CanBuiltinIntegerType IVType = CondVal->getType().castTo<BuiltinIntegerType>();

  if (IVType->getFixedWidth() == 1)
    return CondVal;

  SILValue AllBitsSet = B.createIntegerLiteral(Loc, CondVal->getType(), -1);
  if (!CmpEqFn.get())
    CmpEqFn = getBinaryFunction("cmp_eq", CondVal->getType(),
                                B.getASTContext());
  SILValue Args[] = { CondVal, AllBitsSet };

  return B.createBuiltin(Loc, CmpEqFn,
                         SILType::getBuiltinIntegerType(1, B.getASTContext()),
                         {}, Args);
}

/// handleConditionalInitAssign - This memory object has some stores
/// into (some element of) it that is either an init or an assign based on the
/// control flow path through the function, or have a destroy event that happens
/// when the memory object may or may not be initialized.  Handle this by
/// inserting a bitvector that tracks the liveness of each tuple element
/// independently.
SILValue LifetimeChecker::handleConditionalInitAssign() {
  SILLocation Loc = TheMemory.getLoc();
  Loc.markAutoGenerated();

  unsigned NumMemoryElements = TheMemory.getNumElements();

  // We might need an extra bit to check if self was consumed.
  if (HasConditionalSelfInitialized)
    ++NumMemoryElements;

  // Create the control variable as the first instruction in the function (so
  // that it is easy to destroy the stack location.
  SILType IVType =
    SILType::getBuiltinIntegerType(NumMemoryElements, Module.getASTContext());
  // Use an empty location for the alloc_stack. If Loc is variable declaration
  // the alloc_stack would look like the storage of that variable.
  auto *ControlVariableBox =
      SILBuilderWithScope(TheMemory.getFunctionEntryPoint())
          .createAllocStack(RegularLocation::getAutoGeneratedLocation(),
                            IVType);

  // Find all the return blocks in the function, inserting a dealloc_stack
  // before the return.
  for (auto &BB : TheMemory.getFunction()) {
    auto *Term = BB.getTerminator();
    if (Term->isFunctionExiting()) {
      SILBuilderWithScope(Term).createDeallocStack(Loc, ControlVariableBox);
    }
  }
  
  // Before the memory allocation, store zero in the control variable.
  SILValue ControlVariableAddr = ControlVariableBox;
  {
    auto *InsertPoint =
        &*std::next(TheMemory.getUninitializedValue()->getIterator());
    SILBuilderWithScope B(InsertPoint);
    auto Zero = B.createIntegerLiteral(Loc, IVType, 0);
    B.createStore(Loc, Zero, ControlVariableAddr,
                  StoreOwnershipQualifier::Trivial);
  }

  Identifier OrFn;

  // At each initialization, mark the initialized elements live.  At each
  // conditional assign, resolve the ambiguity by inserting a CFG diamond.
  for (unsigned i = 0; i != Uses.size(); ++i) {
    auto &Use = Uses[i];
    
    // Ignore deleted uses.
    if (Use.Inst == nullptr) continue;

    // If this ambiguous store is only of trivial types, then we don't need to
    // do anything special.  We don't even need keep the init bit for the
    // element precise.
    //
    // For root class initializers, we must keep track of initializations of
    // trivial stored properties also, since we need to know when the object
    // has been fully initialized when deciding if a strong_release should
    // lower to a partial_dealloc_ref.
    if (!TheMemory.isRootClassSelf() &&
        Use.onlyTouchesTrivialElements(TheMemory))
      continue;
    
    SILBuilderWithScope B(Use.Inst);
    
    // Only full initializations make something live.  inout uses, escapes, and
    // assignments only happen when some kind of init made the element live.
    switch (Use.Kind) {
    default:
      // We can ignore most use kinds here.
      continue;
    case DIUseKind::InitOrAssign:
      // The dynamically unknown case is the interesting one, handle it below.
      break;

    case DIUseKind::SelfInit:
    case DIUseKind::Initialization:
      APInt Bitmask = Use.getElementBitmask(NumMemoryElements);
      SILBuilderWithScope SB(Use.Inst);
      updateControlVariable(Loc, Bitmask, ControlVariableAddr, OrFn, SB);
      continue;
    }

    // If this is the interesting case, we need to generate a CFG diamond for
    // each element touched, destroying any live elements so that the resulting
    // store is always an initialize.  This disambiguates the dynamic
    // uncertainty with a runtime check.
    SILValue ControlVariable;

    // If we have multiple tuple elements, we'll have to do some shifting and
    // truncating of the mask value.  These values cache the function_ref so we
    // don't emit multiple of them.
    Identifier ShiftRightFn, TruncateFn;
    
    // If the memory object has multiple tuple elements, we need to destroy any
    // live subelements, since they can each be in a different state of
    // initialization.
    for (unsigned Elt = Use.FirstElement, e = Elt+Use.NumElements;
         Elt != e; ++Elt) {
      auto CondVal = testControlVariableBit(Loc, Elt, ControlVariableAddr,
                                            ShiftRightFn, TruncateFn,
                                            B);
      
      SILBasicBlock *TrueBB, *FalseBB, *ContBB;
      InsertCFGDiamond(CondVal, Loc, B,
                       TrueBB, FalseBB, ContBB);

      // Emit a destroy_addr in the taken block.
      B.setInsertionPoint(TrueBB->begin());
      SILValue EltPtr;
      {
        using EndScopeKind = DIMemoryObjectInfo::EndScopeKind;
        SmallVector<std::pair<SILValue, EndScopeKind>, 4> EndScopeList;
        EltPtr =
            TheMemory.emitElementAddressForDestroy(Elt, Loc, B, EndScopeList);
        if (auto *DA = B.emitDestroyAddrAndFold(Loc, EltPtr))
          Destroys.push_back(DA);
        while (!EndScopeList.empty()) {
          SILValue value;
          EndScopeKind kind;
          std::tie(value, kind) = EndScopeList.pop_back_val();

          switch (kind) {
          case EndScopeKind::Borrow:
            B.createEndBorrow(Loc, value);
            continue;
          case EndScopeKind::Access:
            B.createEndAccess(Loc, value, false /*can abort*/);
            continue;
          }
          llvm_unreachable("Covered switch isn't covered!");
        }
      }
      B.setInsertionPoint(ContBB->begin());
    }
    
    // Finally, now that we know the value is uninitialized on all paths, it is
    // safe to do an unconditional initialization.
    Use.Kind = DIUseKind::Initialization;
    NeedsUpdateForInitState.push_back(i);

    // Update the control variable.
    APInt Bitmask = Use.getElementBitmask(NumMemoryElements);
    SILBuilderWithScope SB(Use.Inst);
    updateControlVariable(Loc, Bitmask, ControlVariableAddr, OrFn, SB);
  }

  // At each block that stores to self, mark the self value as having been
  // initialized.
  if (HasConditionalSelfInitialized) {
    for (auto *I : StoresToSelf) {
      auto *bb = I->getParent();
      SILBuilderWithScope B(bb->begin());

      // Set the most significant bit.
      APInt Bitmask = APInt::getHighBitsSet(NumMemoryElements, 1);
      updateControlVariable(Loc, Bitmask, ControlVariableAddr, OrFn, B);
    }
  }

  return ControlVariableAddr;
}

/// Move the end_borrow that guards an alloc_box's lifetime to before the
/// dealloc_box in the CFG diamond that is created for destruction when it is
/// not statically known whether the value is initialized.
///
/// In the following context
///
///    %box = alloc_box
///    %mark_uninit = mark_uninitialized %box
///    %lifetime = begin_borrow [lexical] %mark_uninit
///    %proj_box = project_box %lifetime
///
/// We are replacing a
///
///     destroy_value %mark_uninit
///
/// with a
///
///     destroy_addr %proj_box
///
/// That's a problem, though, because by SILGen construction the
/// destroy_value is always preceded by an end_borrow
///
///     end_borrow %lifetime
///     destroy_value %mark_uninit
///
/// Consequently, it's not sufficient to just replace the destroy_value
/// %mark_uninit with a destroy_addr %proj_box (or to replace it with a diamond
/// where one branch has that destroy_addr) because the destroy_addr is a use
/// of %proj_box which must be within the lexical lifetime of the box.
///
/// On the other side, we are hemmed in by the fact that the end_borrow must
/// precede the dealloc_box which will be created in the diamond.  So we
/// couldn't simply start inserting before the end_borrow (because the bottom
/// of the diamond contains a dealloc_box, so we would have an end_borrow after
/// the dealloc_box).
///
/// At this point, we have the following code:
///
///       end_borrow %lifetime
///       %initialized = load %addr
///       cond_br %initialized, yes, no
///
///     yes:
///       destroy_addr %proj_box
///       br bottom
///
///     no:
///       br bottom
///
///     bottom:
///       br keep_going
///
///     keep_going:
///
/// So just move the end_borrow to the right position, at the top of the bottom
/// block.  The caller will then add the dealloc_box.
static bool adjustAllocBoxEndBorrow(SILInstruction *previous,
                                    SILValue destroyedAddr,
                                    SILBuilderWithScope &builder) {
  // This fixup only applies if we're destroying a project_box.
  auto *pbi = dyn_cast<ProjectBoxInst>(destroyedAddr);
  if (!pbi)
    return false;

  // This fixup only applies if we're destroying a project_box of the lexical
  // lifetime of an alloc_box.
  auto *lifetime = dyn_cast<BeginBorrowInst>(pbi->getOperand());
  if (!lifetime)
    return false;
  assert(lifetime->isLexical());
  assert(isa<AllocBoxInst>(
      cast<MarkUninitializedInst>(lifetime->getOperand())->getOperand()));

  // Scan the block backwards from previous, looking for an end_borrow.  SILGen
  // will emit the sequence
  //
  //     end_borrow %lifetime
  //     destroy_value %mark_uninit
  //
  // but other passes may have moved them apart.
  EndBorrowInst *ebi = nullptr;
  for (auto *instruction = previous; instruction;
       instruction = instruction->getPreviousInstruction()) {
    auto *candidate = dyn_cast<EndBorrowInst>(instruction);
    if (!candidate)
      continue;
    auto *bbi = dyn_cast<BeginBorrowInst>(candidate->getOperand());
    if (bbi != lifetime)
      continue;
    ebi = candidate;
  }
  if (!ebi)
    return false;

  ebi->moveBefore(&*builder.getInsertionPoint());
  return true;
}

/// Process any destroy_addr and strong_release instructions that are invoked on
/// a partially initialized value.  This generates code to destroy the elements
/// that are known to be alive, ignore the ones that are known to be dead, and
/// to emit branching logic when an element may or may not be initialized.
void LifetimeChecker::
handleConditionalDestroys(SILValue ControlVariableAddr) {
  SILBuilderWithScope B(TheMemory.getUninitializedValue());
  Identifier ShiftRightFn, TruncateFn, CmpEqFn;

  unsigned SelfInitializedElt = TheMemory.getNumElements();
  unsigned SuperInitElt = TheMemory.getNumElements() - 1;

  // Utilities.

  auto destroyMemoryElement = [&](SILLocation Loc, unsigned Elt) -> SILValue {
    using EndScopeKind = DIMemoryObjectInfo::EndScopeKind;
    SmallVector<std::pair<SILValue, EndScopeKind>, 4> EndScopeList;
    SILValue EltPtr =
        TheMemory.emitElementAddressForDestroy(Elt, Loc, B, EndScopeList);
    if (auto *DA = B.emitDestroyAddrAndFold(Loc, EltPtr))
      Destroys.push_back(DA);

    while (!EndScopeList.empty()) {
      SILValue value;
      EndScopeKind kind;
      std::tie(value, kind) = EndScopeList.pop_back_val();

      switch (kind) {
      case EndScopeKind::Borrow:
        B.createEndBorrow(Loc, value);
        continue;
      case EndScopeKind::Access:
        B.createEndAccess(Loc, value, false /*can abort*/);
        continue;
      }
      llvm_unreachable("Covered switch isn't covered!");
    }
    return EltPtr;
  };

  // Destroy all the allocation's fields, not including the allocation
  // itself, if we have a class initializer.
  auto destroyMemoryElements = [&](SILInstruction *Release, SILLocation Loc,
                                   AvailabilitySet Availability) {
    auto *Previous = Release->getPreviousInstruction();
    // Delegating initializers don't model the fields of the class.
    if (TheMemory.isClassInitSelf() && TheMemory.isDelegatingInit())
      return;

    // Destroy those fields of TheMemory that are already initialized, skip
    // those fields that are known not to be initialized, and conditionally
    // destroy fields in a control-flow sensitive situation.
    for (unsigned Elt = 0; Elt < TheMemory.getNumMemoryElements(); ++Elt) {
      switch (Availability.get(Elt)) {
      case DIKind::No:
        // If an element is known to be uninitialized, then we know we can
        // completely ignore it.
        continue;

      case DIKind::Partial:
        // In the partially live case, we have to check our control variable to
        // destroy it.  Handle this below.
        break;

      case DIKind::Yes:
        // If an element is known to be initialized, then we can strictly
        // destroy its value at releases position.
        destroyMemoryElement(Loc, Elt);
        continue;
      }

      // Insert a load of the liveness bitmask and split the CFG into a diamond
      // right before the destroy_addr, if we haven't already loaded it.
      auto CondVal = testControlVariableBit(Loc, Elt, ControlVariableAddr,
                                            ShiftRightFn, TruncateFn,
                                            B);

      SILBasicBlock *ReleaseBlock, *DeallocBlock, *ContBlock;

      InsertCFGDiamond(CondVal, Loc, B,
                       ReleaseBlock, DeallocBlock, ContBlock);

      // Set up the initialized release block.
      B.setInsertionPoint(ReleaseBlock->begin());
      auto EltPtr = destroyMemoryElement(Loc, Elt);

      B.setInsertionPoint(ContBlock->begin());
      adjustAllocBoxEndBorrow(Previous, EltPtr, B);
    }
  };

  // Either release the self reference, or just deallocate the box,
  // depending on if the self box was initialized or not.
  auto emitReleaseOfSelfWhenNotConsumed = [&](SILLocation Loc,
                                              SILInstruction *Release) {
    auto CondVal = testControlVariableBit(Loc, SelfInitializedElt,
                                          ControlVariableAddr,
                                          ShiftRightFn,
                                          TruncateFn,
                                          B);

    SILBasicBlock *ReleaseBlock, *ConsumedBlock, *ContBlock;

    InsertCFGDiamond(CondVal, Loc, B,
                     ReleaseBlock, ConsumedBlock, ContBlock);

    // If true, self is fully initialized; just release it as usual.
    B.setInsertionPoint(ReleaseBlock->begin());
    Release->moveBefore(&*B.getInsertionPoint());

    // If false, self is consumed.
    B.setInsertionPoint(ConsumedBlock->begin());
    processUninitializedRelease(Release, true, B.getInsertionPoint());
  };

  // After handling any conditional initializations, check to see if we have any
  // cases where the value is only partially initialized by the time its
  // lifetime ends.  In this case, we have to make sure not to destroy an
  // element that wasn't initialized yet.
  for (auto &CDElt : ConditionalDestroys) {
    auto *Release = Destroys[CDElt.ReleaseID];
    auto Loc = Release->getLoc();
    auto &Availability = CDElt.Availability;

    B.setInsertionPoint(Release);
    B.setCurrentDebugScope(Release->getDebugScope());

    // Value types and root classes don't require any fancy handling.
    // Just conditionally destroy each memory element, and for classes,
    // also free the partially initialized object.
    if (!TheMemory.isNonRootClassSelf()) {
      assert(!Availability.isAllYes() &&
             "Should not end up here if fully initialized");

      // For root class initializers, we check if all properties were
      // dynamically initialized, and if so, treat this as a release of
      // an initialized 'self', instead of tearing down the fields
      // one by one and deallocating memory.
      //
      // This is required for correctness, since the condition that
      // allows 'self' to escape is that all stored properties were
      // initialized. So we cannot deallocate the memory if 'self' may
      // have escaped.
      //
      // This also means the deinitializer will run if all stored
      // properties were initialized.
      if (TheMemory.isClassInitSelf() &&
          Availability.hasAny(DIKind::Partial)) {
        auto CondVal = testAllControlVariableBits(Loc, ControlVariableAddr,
                                                  CmpEqFn, B);

        SILBasicBlock *ReleaseBlock, *DeallocBlock, *ContBlock;

        InsertCFGDiamond(CondVal, Loc, B,
                         ReleaseBlock, DeallocBlock, ContBlock);

        // If true, self was fully initialized and must be released.
        B.setInsertionPoint(ReleaseBlock->begin());
        B.setCurrentDebugScope(ReleaseBlock->begin()->getDebugScope());
        Release->moveBefore(&*B.getInsertionPoint());

        // If false, self is uninitialized and must be freed.
        B.setInsertionPoint(DeallocBlock->begin());
        B.setCurrentDebugScope(DeallocBlock->begin()->getDebugScope());
        destroyMemoryElements(Release, Loc, Availability);
        processUninitializedRelease(Release, false, B.getInsertionPoint());
      } else {
        destroyMemoryElements(Release, Loc, Availability);
        processUninitializedRelease(Release, false, B.getInsertionPoint());

        // The original strong_release or destroy_addr instruction is
        // always dead at this point.
        deleteDeadRelease(CDElt.ReleaseID);
      }

      continue;
    }

    // Hard case -- we have a self reference which requires additional
    // handling to deal with the 'self' value being consumed.
    bool isDeadRelease = true;

    auto SelfLive = Availability.get(SuperInitElt);

    switch (SelfLive) {
    case DIKind::No:
      assert(CDElt.SelfInitialized == DIKind::No &&
             "Impossible to have initialized the self box where "
             "self.init was not called");

      // self.init or super.init was not called. If we're in the super.init
      // case, destroy any initialized fields.
      destroyMemoryElements(Release, Loc, Availability);
      processUninitializedRelease(Release, false, B.getInsertionPoint());
      break;

    case DIKind::Yes:
      switch (CDElt.SelfInitialized) {
      case DIKind::No:
        llvm_unreachable("Impossible to have initialized the self box where "
                         "self.init was not called");
      case DIKind::Yes:
        llvm_unreachable("This should have been an unconditional destroy");

      case DIKind::Partial: {
        // self.init or super.init was called, but we don't know if the
        // self value was consumed or not.
        emitReleaseOfSelfWhenNotConsumed(Loc, Release);
        isDeadRelease = false;
        break;
      }
      }
      break;

    case DIKind::Partial:
      switch (CDElt.SelfInitialized) {
      case DIKind::No: {
        // self.init or super.init may or may not have been called.
        // We have not yet stored 'self' into the box.

        auto CondVal = testControlVariableBit(Loc, SuperInitElt,
                                              ControlVariableAddr,
                                              ShiftRightFn,
                                              TruncateFn,
                                              B);

        SILBasicBlock *ConsumedBlock, *DeallocBlock, *ContBlock;

        InsertCFGDiamond(CondVal, Loc, B,
                         ConsumedBlock, DeallocBlock, ContBlock);

        // If true, self.init or super.init was called and self was consumed.
        B.setInsertionPoint(ConsumedBlock->begin());
        B.setCurrentDebugScope(ConsumedBlock->begin()->getDebugScope());
        processUninitializedRelease(Release, true, B.getInsertionPoint());

        // If false, self is uninitialized and must be freed.
        B.setInsertionPoint(DeallocBlock->begin());
        B.setCurrentDebugScope(DeallocBlock->begin()->getDebugScope());
        destroyMemoryElements(Release, Loc, Availability);
        processUninitializedRelease(Release, false, B.getInsertionPoint());

        break;
      }

      case DIKind::Yes:
        llvm_unreachable("Impossible to have initialized the self box where "
                         "self.init may not have been called");
        break;

      case DIKind::Partial: {
        // self.init or super.init may or may not have been called.
        // We may or may have stored 'self' into the box.

        auto CondVal = testControlVariableBit(Loc, SuperInitElt,
                                              ControlVariableAddr,
                                              ShiftRightFn,
                                              TruncateFn,
                                              B);

        SILBasicBlock *LiveBlock, *DeallocBlock, *ContBlock;

        InsertCFGDiamond(CondVal, Loc, B,
                         LiveBlock, DeallocBlock, ContBlock);

        // If true, self was consumed or is fully initialized.
        B.setInsertionPoint(LiveBlock->begin());
        B.setCurrentDebugScope(LiveBlock->begin()->getDebugScope());
        emitReleaseOfSelfWhenNotConsumed(Loc, Release);
        isDeadRelease = false;

        // If false, self is uninitialized and must be freed.
        B.setInsertionPoint(DeallocBlock->begin());
        B.setCurrentDebugScope(DeallocBlock->begin()->getDebugScope());
        destroyMemoryElements(Release, Loc, Availability);
        processUninitializedRelease(Release, false, B.getInsertionPoint());

        break;
      }
      }
    }

    if (isDeadRelease)
      deleteDeadRelease(CDElt.ReleaseID);
  }
}

void LifetimeChecker::
putIntoWorkList(SILBasicBlock *BB, WorkListType &WorkList) {
  LiveOutBlockState &State = getBlockInfo(BB);
  if (!State.isInWorkList && State.containsUndefinedValues()) {
    LLVM_DEBUG(llvm::dbgs() << "    add block " << BB->getDebugID()
                            << " to worklist\n");
    WorkList.push_back(BB);
    State.isInWorkList = true;
  }
}

void LifetimeChecker::
computePredsLiveOut(SILBasicBlock *BB) {
  LLVM_DEBUG(llvm::dbgs() << "  Get liveness for block " << BB->getDebugID()
                          << "\n");
  
  // Collect blocks for which we have to calculate the out-availability.
  // These are the paths from blocks with known out-availability to the BB.
  WorkListType WorkList;
  for (auto Pred : BB->getPredecessorBlocks()) {
    putIntoWorkList(Pred, WorkList);
  }
  size_t idx = 0;
  while (idx < WorkList.size()) {
    SILBasicBlock *WorkBB = WorkList[idx++];
    for (auto Pred : WorkBB->getPredecessorBlocks()) {
      putIntoWorkList(Pred, WorkList);
    }
  }

  // Solve the dataflow problem.
#ifndef NDEBUG
  int iteration = 0;
  int upperIterationLimit = WorkList.size() * 2 + 10; // More than enough.
#endif
  bool changed;
  do {
    assert(iteration < upperIterationLimit &&
           "Infinite loop in dataflow analysis?");
    LLVM_DEBUG(llvm::dbgs() << "    Iteration " << iteration++ << "\n");
    
    changed = false;
    // We collected the blocks in reverse order. Since it is a forward dataflow-
    // problem, it is faster to go through the worklist in reverse order.
    for (auto iter = WorkList.rbegin(); iter != WorkList.rend(); ++iter) {
      SILBasicBlock *WorkBB = *iter;
      LiveOutBlockState &BBState = getBlockInfo(WorkBB);

      // Merge from the predecessor blocks.
      for (auto Pred : WorkBB->getPredecessorBlocks()) {
        changed |= BBState.mergeFromPred(getBlockInfo(Pred));
      }
      LLVM_DEBUG(llvm::dbgs() << "      Block " << WorkBB->getDebugID()
                              << " out: "
                              << BBState.OutAvailability << "\n");

      // Clear the worklist-flag for the next call to computePredsLiveOut().
      // This could be moved out of the outer loop, but doing it here avoids
      // another loop with getBlockInfo() calls.
      BBState.isInWorkList = false;
    }
  } while (changed);
}

void LifetimeChecker::
getOutAvailability(SILBasicBlock *BB, AvailabilitySet &Result) {
  computePredsLiveOut(BB);

  for (auto *Pred : BB->getPredecessorBlocks()) {
    auto &BBInfo = getBlockInfo(Pred);
    Result.mergeIn(BBInfo.OutAvailability);
  }
  LLVM_DEBUG(llvm::dbgs() << "    Result: " << Result << "\n");
}

void LifetimeChecker::getOutSelfInitialized(SILBasicBlock *BB,
                                            llvm::Optional<DIKind> &Result) {
  computePredsLiveOut(BB);

  for (auto *Pred : BB->getPredecessorBlocks())
    Result = mergeKinds(Result, getBlockInfo(Pred).OutSelfInitialized);
}

AvailabilitySet
LifetimeChecker::getLivenessAtNonTupleInst(swift::SILInstruction *Inst,
                                           swift::SILBasicBlock *InstBB,
                                           AvailabilitySet &Result) {
  // If there is a store in the current block, scan the block to see if the
  // store is before or after the load.  If it is before, it produces the value
  // we are looking for.
  if (getBlockInfo(InstBB).HasNonLoadUse) {
    for (auto BBI = Inst->getIterator(), E = InstBB->begin(); BBI != E;) {
      --BBI;
      SILInstruction *TheInst = &*BBI;

      if (TheInst == TheMemory.getUninitializedValue()) {
        Result.set(0, DIKind::No);
        return Result;
      }

      if (NonLoadUses.count(TheInst)) {
        // We've found a definition, or something else that will require that
        // the memory is initialized at this point.
        Result.set(0, DIKind::Yes);
        return Result;
      }
    }
  }

  getOutAvailability(InstBB, Result);

  // If the result element wasn't computed, we must be analyzing code within
  // an unreachable cycle that is not dominated by "TheMemory".  Just force
  // the unset element to yes so that clients don't have to handle this.
  if (!Result.getConditional(0))
    Result.set(0, DIKind::Yes);

  return Result;
}

/// getLivenessAtInst - Compute the liveness state for any number of tuple
/// elements at the specified instruction.  The elements are returned as an
/// AvailabilitySet.  Elements outside of the range specified may not be
/// computed correctly.
AvailabilitySet LifetimeChecker::getLivenessAtInst(SILInstruction *Inst,
                                                   unsigned FirstElt,
                                                   unsigned NumElts) {
  LLVM_DEBUG(llvm::dbgs() << "Get liveness " << FirstElt << ", #" << NumElts
                          << " at " << *Inst);

  AvailabilitySet Result(TheMemory.getNumElements());

  // Empty tuple queries return a completely "unknown" vector, since they don't
  // care about any of the elements.
  if (NumElts == 0)
    return Result;

  SILBasicBlock *InstBB = Inst->getParent();

  // The vastly most common case is memory allocations that are not tuples,
  // so special case this with a more efficient algorithm.
  if (TheMemory.getNumElements() == 1) {
    return getLivenessAtNonTupleInst(Inst, InstBB, Result);
  }

  // Check locally to see if any elements are satisfied within the block, and
  // keep track of which ones are still needed in the NeededElements set.
  SmallBitVector NeededElements(TheMemory.getNumElements());
  NeededElements.set(FirstElt, FirstElt+NumElts);
  
  // If there is a store in the current block, scan the block to see if the
  // store is before or after the load.  If it is before, it may produce some of
  // the elements we are looking for.
  if (getBlockInfo(InstBB).HasNonLoadUse) {
    for (auto BBI = Inst->getIterator(), E = InstBB->begin(); BBI != E;) {
      --BBI;
      SILInstruction *TheInst = &*BBI;

      // If we found the allocation itself, then we are loading something that
      // is not defined at all yet.  Scan no further.
      if (TheInst == TheMemory.getUninitializedValue()) {
        // The result is perfectly decided locally.
        for (unsigned i = FirstElt, e = i+NumElts; i != e; ++i)
          Result.set(i, NeededElements[i] ? DIKind::No : DIKind::Yes);
        return Result;
      }

      // If this instruction is unrelated to the memory, ignore it.
      auto It = NonLoadUses.find(TheInst);
      if (It == NonLoadUses.end())
        continue;

      // Check to see which tuple elements this instruction defines.  Clear them
      // from the set we're scanning from.
      for (unsigned TheUse : It->second) {
        auto &TheInstUse = Uses[TheUse];
        NeededElements.reset(TheInstUse.FirstElement,
                             TheInstUse.FirstElement+TheInstUse.NumElements);
      }

      // If that satisfied all of the elements we're looking for, then we're
      // done.  Otherwise, keep going.
      if (NeededElements.none()) {
        Result.changeUnsetElementsTo(DIKind::Yes);
        return Result;
      }
    }
  }

  // Compute the liveness of each element according to our predecessors.
  getOutAvailability(InstBB, Result);
  
  // If any of the elements was locally satisfied, make sure to mark them.
  for (unsigned i = FirstElt, e = i+NumElts; i != e; ++i) {
    if (!NeededElements[i] || !Result.getConditional(i)) {
      // If the result element wasn't computed, we must be analyzing code within
      // an unreachable cycle that is not dominated by "TheMemory".  Just force
      // the unset element to yes so that clients don't have to handle this.
      Result.set(i, DIKind::Yes);
    }
  }
  return Result;
}

/// If any of the elements in the specified range are uninitialized at the
/// specified instruction, return the first element that is uninitialized.  If
/// they are all initialized, return -1.
int LifetimeChecker::getAnyUninitializedMemberAtInst(SILInstruction *Inst,
                                                     unsigned FirstElt,
                                                     unsigned NumElts) {
  // Determine the liveness states of the elements that we care about.
  auto Liveness = getLivenessAtInst(Inst, FirstElt, NumElts);
  
  // Find uninitialized member.
  for (unsigned i = FirstElt, e = i+NumElts; i != e; ++i)
    if (Liveness.get(i) != DIKind::Yes)
      return i;
  
  return -1;
}

/// getSelfInitializedAtInst - Check if the self box in an initializer has
/// a fully initialized value at the specified instruction.
///
/// Possible outcomes:
/// - 'Yes' -- 'self' is fully initialized, and should be destroyed in the
///   usual manner in an error path
///
/// - 'No', and instruction is dominated by a SelfInit use -- this means
///   'self' was consumed by a self.init or super.init call, and we're in
///   an error path; there's nothing to clean up
///
/// - 'No', and instruction is not dominated by a SelfInit use -- this means
///   we have to do a partial cleanup, for example deallocating a class
///   instance without destroying its members
///
/// Also, the full range of conditional outcomes is possible above, if the
/// result is 'Partial'.
DIKind LifetimeChecker::
getSelfInitializedAtInst(SILInstruction *Inst) {
  LLVM_DEBUG(llvm::dbgs() << "Get self initialized at " << *Inst);

  SILBasicBlock *InstBB = Inst->getParent();
  auto &BlockInfo = getBlockInfo(InstBB);

  if (BlockInfo.LocalSelfInitialized.has_value())
    return *BlockInfo.LocalSelfInitialized;

  llvm::Optional<DIKind> Result;
  getOutSelfInitialized(InstBB, Result);

  // If the result wasn't computed, we must be analyzing code within
  // an unreachable cycle that is not dominated by "TheMemory".  Just force
  // the result to initialized so that clients don't have to handle this.
  if (!Result.has_value())
    Result = DIKind::Yes;

  return *Result;
}

/// The specified instruction is a use of some number of elements.  Determine
/// whether all of the elements touched by the instruction are definitely
/// initialized at this point or not.
bool LifetimeChecker::isInitializedAtUse(const DIMemoryUse &Use,
                                         bool *SuperInitDone,
                                         bool *FailedSelfUse,
                                         bool *FullyUninitialized) {
  if (FailedSelfUse) *FailedSelfUse = false;
  if (SuperInitDone) *SuperInitDone = true;
  if (FullyUninitialized) *FullyUninitialized = true;

  // Determine the liveness states of the elements that we care about.
  AvailabilitySet Liveness =
    getLivenessAtInst(Use.Inst, Use.FirstElement, Use.NumElements);

  // If the client wants to know about super.init, check to see if we failed
  // it or some other element.
  if (Use.FirstElement + Use.NumElements == TheMemory.getNumElements() &&
      TheMemory.isAnyDerivedClassSelf() &&
      Liveness.get(Liveness.size() - 1) != DIKind::Yes) {
    if (SuperInitDone) *SuperInitDone = false;
  }

  // Check all the results.
  bool isFullyInitialized = true;
  for (unsigned i = Use.FirstElement, e = i+Use.NumElements;
       i != e; ++i) {
    if (Liveness.get(i) != DIKind::Yes)
      isFullyInitialized = false;
    if (FullyUninitialized && Liveness.get(i) != DIKind::No)
      *FullyUninitialized = false;
  }
  if (!isFullyInitialized)
    return false;

  // If the self.init() or super.init() call threw an error and
  // we caught it, self is no longer available.
  if (TheMemory.isNonRootClassSelf()) {
    if (getSelfInitializedAtInst(Use.Inst) != DIKind::Yes) {
      auto SelfLiveness =
          getLivenessAtInst(Use.Inst, 0, TheMemory.getNumElements());
      if (SelfLiveness.isAllYes()) {
        if (FailedSelfUse) *FailedSelfUse = true;
        return false;
      }
    }
  }

  return true;
}

//===----------------------------------------------------------------------===//
//                           Top Level Driver
//===----------------------------------------------------------------------===//

static void processMemoryObject(MarkUninitializedInst *I,
                                BlockStates &blockStates) {
  LLVM_DEBUG(llvm::dbgs() << "*** Definite Init looking at: " << *I << "\n");
  DIMemoryObjectInfo MemInfo(I);

  // Set up the datastructure used to collect the uses of the allocation.
  DIElementUseInfo UseInfo;

  // Walk the use list of the pointer, collecting them into the Uses array.
  collectDIElementUsesFrom(MemInfo, UseInfo);

  LifetimeChecker(MemInfo, UseInfo, blockStates).doIt();
}

/// Check that all memory objects that require initialization before use are
/// properly set and transform the code as required for flow-sensitive
/// properties.
static bool checkDefiniteInitialization(SILFunction &Fn) {
  LLVM_DEBUG(llvm::dbgs() << "*** Definite Init visiting function: "
                          <<  Fn.getName() << "\n");
  bool Changed = false;

  BlockStates blockStates(&Fn);

  for (auto &BB : Fn) {
    for (auto I = BB.begin(), E = BB.end(); I != E;) {
      SILInstruction *Inst = &*I;

      auto *MUI = dyn_cast<MarkUninitializedInst>(Inst);
      if (!MUI) {
        ++I;
        continue;
      }

      // Then process the memory object.
      processMemoryObject(MUI, blockStates);

      // Move off of the MUI only after we have processed memory objects. The
      // lifetime checker may rewrite instructions, so it is important to not
      // move onto the next element until after it runs.
      ++I;
      MUI->replaceAllUsesWith(MUI->getOperand());
      MUI->eraseFromParent();
      Changed = true;
    }
  }

  return Changed;
}

namespace {

/// Perform definitive initialization analysis and promote alloc_box uses into
/// SSA registers for later SSA-based dataflow passes.
class DefiniteInitialization : public SILFunctionTransform {
  /// The entry point to the transformation.
  void run() override {
    // Don't rerun diagnostics on deserialized functions.
    if (getFunction()->wasDeserializedCanonical())
      return;

    // Walk through and promote all of the alloc_box's that we can.
    if (checkDefiniteInitialization(*getFunction())) {
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDefiniteInitialization() {
  return new DefiniteInitialization();
}
