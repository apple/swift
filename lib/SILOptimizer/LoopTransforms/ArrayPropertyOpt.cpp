//===--- ArrayPropertyOpt.cpp - Optimize Array Properties -----------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// Optimize array property access by specializing loop bodies.
///
/// This optimization specializes loops with calls to
/// "array.props.isNative/needsElementTypeCheck".
///
/// The "array.props.isNative/needsElementTypeCheck" predicate has the property
/// that if it is true/false respectively for the array struct it is true/false
/// respectively until somebody writes a new array struct over the memory
/// location. Less abstractly, a fast native swift array does not transition to
/// a slow array (be it a cocoa array, or be it an array that needs type
/// checking) except if we store a new array to the variable that holds it.
///
/// Using this property we can hoist the predicate above a region where no such
/// store can take place.
///
///  func f(a : A[AClass]) {
///     for i in 0..a.count {
///       let b = a.props.isNative()
///        .. += _getElement(i, b)
///     }
///  }
///
///   ==>
///
///  func f(a : A[AClass]) {
///    let b = a.props.isNative
///    if (b) {
///      for i in 0..a.count {
///         .. += _getElement(i, false)
///      }
///    } else {
///      for i in 0..a.count {
///        let a = a.props.isNative
///        .. += _getElement(i, a)
///      }
///    }
///  }
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "array-property-opt"

#include "ArrayOpt.h"
#include "swift/SIL/CFG.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/LoopInfo.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SILOptimizer/Analysis/ArraySemantic.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/LoopUtils.h"
#include "swift/SILOptimizer/Utils/SILSSAUpdater.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
using namespace swift;

namespace {
/// Analysis whether it is safe to specialize this loop nest based on the
/// array.props function calls it contains. It is safe to hoist array.props
/// calls if the array does not escape such that the array container could be
/// overwritten in the hoisted region.
/// This analysis also checks if we can clone the instructions in the loop nest.
class ArrayPropertiesAnalysis {
  using UserList = StructUseCollector::UserList;
  using UserOperList = StructUseCollector::UserOperList;

  SILFunction *Fun;
  SILLoop *Loop;
  SILBasicBlock *Preheader;
  DominanceInfo *DomTree;

  SinkAddressProjections sinkProj;

  llvm::DenseMap<SILFunction *, uint32_t> InstCountCache;
  llvm::SmallSet<SILValue, 16> HoistableArray;

  BasicBlockSet ReachingBlocks;
  SmallVector<SILBasicBlock *, 16> CachedExitingBlocks;

  // This controls the max instructions the analysis can scan before giving up
  const uint32_t AnalysisThreshold = 5000;
  // This controls the max threshold for instruction count in the loop
  const uint32_t LoopInstCountThreshold = 500;

  bool reachingBlocksComputed = false;

public:
  ArrayPropertiesAnalysis(SILLoop *L, DominanceAnalysis *DA)
      : Fun(L->getHeader()->getParent()), Loop(L), Preheader(nullptr),
        DomTree(DA->get(Fun)), ReachingBlocks(Fun) {}

  /// Check if it is profitable to specialize a loop when you see an apply
  /// instruction. We consider it is not profitable to specialize the loop when:
  /// 1. The callee is not found in the module, or cannot be determined
  /// 2. The number of instructions the analysis scans has exceeded the
  /// AnalysisThreshold
  uint32_t checkProfitabilityRecursively(SILFunction *Callee) {
    if (!Callee)
      return AnalysisThreshold;

    auto CacheEntry = InstCountCache.find(Callee);
    if (CacheEntry != InstCountCache.end())
      return CacheEntry->second;

    InstCountCache.insert(std::make_pair(Callee, 0));

    uint32_t InstCount = 0;

    for (auto &BB : *Callee) {
      for (auto &I : BB) {
        if (InstCount++ >= AnalysisThreshold) {
          LLVM_DEBUG(llvm::dbgs() << "ArrayPropertyOpt: Disabled Reason - "
                                     "Exceeded Analysis Threshold in "
                                  << BB.getParent()->getName() << "\n");
          InstCountCache[Callee] = AnalysisThreshold;
          return AnalysisThreshold;
        }
        if (auto Apply = FullApplySite::isa(&I)) {
          auto Callee = Apply.getReferencedFunctionOrNull();
          if (!Callee) {
            LLVM_DEBUG(
                llvm::dbgs()
                << "ArrayPropertyOpt: Disabled Reason - Found opaque code in "
                << BB.getParent()->getName() << "\n");
            LLVM_DEBUG(Apply.dump());
            LLVM_DEBUG(I.getOperand(0)->dump());
          }
          const auto CalleeInstCount = checkProfitabilityRecursively(Callee);
          InstCount += CalleeInstCount;
        }
      }
    }
    InstCountCache[Callee] = InstCount;

    return InstCount;
  }

  bool run() {
    Preheader = Loop->getLoopPreheader();
    if (!Preheader) {
      LLVM_DEBUG(llvm::dbgs() << "ArrayPropertiesAnalysis: "
                                 "Missing preheader for "
                              << *Loop);
      return false;
    }

    // Check whether this is a 'array.props' instruction and whether we
    // can hoist it. Heuristic: We only want to hoist array.props instructions
    // if we can hoist all of them - only then can we get rid of all the
    // control-flow if we specialize. Hoisting some but not others is not as
    // beneficial. This heuristic also simplifies which regions we want to
    // specialize on. We will specialize the outermost loopnest that has
    // 'array.props' instructions in its preheader.

    bool FoundHoistable = false;
    uint32_t LoopInstCount = 0;

    for (auto *BB : Loop->getBlocks()) {
      for (auto &Inst : *BB) {
        // Can't clone alloc_stack instructions whose dealloc_stack is outside
        // the loop.
        if (!canDuplicateLoopInstruction(Loop, &Inst))
          return false;

        if (!sinkProj.analyzeAddressProjections(&Inst)) {
          return false;
        }

        ArraySemanticsCall ArrayPropsInst(&Inst, "array.props", true);
        if (!ArrayPropsInst)
          continue;

        if (!canHoistArrayPropsInst(ArrayPropsInst))
          return false;

        ++LoopInstCount;
        FoundHoistable = true;
      }
    }

    if (!FoundHoistable)
      return false;

    // If the LoopInstCount exceeds the threshold, we will disable the
    // optimization on this loop For loops of deeper nesting we increase the
    // threshold by an additional 10%
    if (LoopInstCount >
        LoopInstCountThreshold * (1 + (Loop->getLoopDepth() - 1) / 10)) {
      LLVM_DEBUG(llvm::dbgs() << "Exceeded LoopInstCountThreshold\n");
      return false;
    }

    // Additionally, we don't specialize the loop if we find opaque code or
    // the analysis scans instructions greater than a threshold
    // Since only few loops qualify as hoistable, and the profitability check
    // can run long in cases of large thresholds, these checks are not folded
    // along with the legality checks above.
    for (auto *BB : Loop->getBlocks()) {
      for (auto &Inst : *BB) {
        if (auto Apply = FullApplySite::isa(&Inst)) {
          const auto Callee = Apply.getReferencedFunctionOrNull();
          auto CalleeInstCount = checkProfitabilityRecursively(Callee);
          if (CalleeInstCount >= AnalysisThreshold)
            return false;
        }
      }
    }

    LLVM_DEBUG(llvm::dbgs()
               << "Profitable ArrayPropertyOpt in "
               << Loop->getLoopPreheader()->getParent()->getName() << "\n");
    LLVM_DEBUG(Loop->dump());
    return true;
  }

private:

  /// Strip the struct load and the address projection to the location
  /// holding the array struct.
  SILValue stripArrayStructLoad(SILValue V) {
    if (auto LI = dyn_cast<LoadInst>(lookThroughCopyValueInsts(V))) {
      auto Val = LI->getOperand();
      // We could have two arrays in a surrounding container so we can only
      // strip off the 'array struct' project.
      // struct Container {
      //   var a1 : [ClassA]
      //   var a2 : [ClassA]
      // }
      // 'a1' and 'a2' are different arrays.
      if (auto SEAI = dyn_cast<StructElementAddrInst>(Val))
        Val = SEAI->getOperand();
      return Val;
    }
    return V;
  }

  BasicBlockSet &getReachingBlocks() {
    if (!reachingBlocksComputed) {
      SmallVector<SILBasicBlock *, 8> Worklist;
      ReachingBlocks.insert(Preheader);
      Worklist.push_back(Preheader);
      while (!Worklist.empty()) {
        SILBasicBlock *BB = Worklist.pop_back_val();
        for (auto PI = BB->pred_begin(), PE = BB->pred_end(); PI != PE; ++PI) {
          if (ReachingBlocks.insert(*PI))
            Worklist.push_back(*PI);
        }
      }
      reachingBlocksComputed = true;
    }
    return ReachingBlocks;
  }

  /// Array address uses are safe if they don't store to the array struct. We
  /// could for example store an NSArray array struct on top of the array. For
  /// example, an opaque function that uses the array's address could store a
  /// new array onto it.
  bool checkSafeArrayAddressUses(UserList &AddressUsers) {
    for (auto *UseInst : AddressUsers) {
      if (UseInst->isDebugInstruction())
        continue;

      if (isa<DeallocStackInst>(UseInst)) {
        // Handle destruction of a local array.
        continue;
      }

      if (isa<LoadInst>(UseInst)) {
        continue;
      }

      if (auto *AI = dyn_cast<ApplyInst>(UseInst)) {
        if (ArraySemanticsCall(AI))
          continue;

        // Check if this escape can reach the current loop.
        if (!Loop->contains(UseInst->getParent()) &&
            !getReachingBlocks().contains(UseInst->getParent())) {
          continue;
        }
        LLVM_DEBUG(llvm::dbgs()
                   << "    Skipping Array: may escape through call!\n"
                   << "    " << *UseInst);
        return false;
      }

      if (auto *StInst = dyn_cast<StoreInst>(UseInst)) {
        // Allow a local array to be initialized outside the loop via a by-value
        // argument or return value. The array value may be returned by its
        // initializer or some other factory function.
        if (Loop->contains(StInst->getParent())) {
          LLVM_DEBUG(llvm::dbgs() << "    Skipping Array: store inside loop!\n"
                                  << "    " << *StInst);
          return false;
        }
        SILValue InitArray = StInst->getSrc();
        if (isa<SILArgument>(InitArray) || isa<ApplyInst>(InitArray))
          continue;

        return false;
      }

      LLVM_DEBUG(llvm::dbgs() << "    Skipping Array: unknown Array use!\n"
                              << "    " << *UseInst);
      // Found an unsafe or unknown user. The Array may escape here.
      return false;
    }

    // Otherwise, all of our users are sane. The array does not escape.
    return true;
  }

  /// Value uses are generally safe. We can't change the state of an array
  /// through a value use.
  bool checkSafeArrayValueUses(UserList &ValueUsers) {
    return true;
  }
  bool checkSafeElementValueUses(UserOperList &ElementValueUsers) {
    return true;
  }

  // We have a safe container if the array container is passed as a function
  // argument by-value or by inout reference. In either case there can't be an
  // alias of the container. Alternatively, we can have a local variable. We
  // will check in checkSafeArrayAddressUses that all initialization stores to
  // this variable are safe (i.e the store dominates the loop etc).
  bool isSafeArrayContainer(SILValue V) {
    if (auto *Arg = dyn_cast<SILArgument>(V)) {
      // Check that the argument is passed as an inout or by value type. This
      // means there are no aliases accessible within this function scope.
      auto Params = Fun->getLoweredFunctionType()->getParameters();
      ArrayRef<SILArgument *> FunctionArgs = Fun->begin()->getArguments();
      for (unsigned ArgIdx = 0, ArgEnd = Params.size(); ArgIdx != ArgEnd;
           ++ArgIdx) {
        if (FunctionArgs[ArgIdx] != Arg)
          continue;

        if (!Params[ArgIdx].isIndirectInOut()
            && Params[ArgIdx].isFormalIndirect()) {
          LLVM_DEBUG(llvm::dbgs() << "    Skipping Array: Not an inout or "
                                     "by val argument!\n");
          return false;
        }
      }
      return true;
    } else if (isa<AllocStackInst>(V))
      return true;

    LLVM_DEBUG(llvm::dbgs()
               << "    Skipping Array: Not a know array container type!\n");

    return false;
  }

  SmallVectorImpl<SILBasicBlock *> &getLoopExitingBlocks() {
    if (!CachedExitingBlocks.empty())
      return CachedExitingBlocks;
    Loop->getExitingBlocks(CachedExitingBlocks);
    return CachedExitingBlocks;
  }

  bool isConditionallyExecuted(ArraySemanticsCall Call) {
    auto CallBB = (*Call).getParent();
    for (auto *ExitingBlk : getLoopExitingBlocks())
      if (!DomTree->dominates(CallBB, ExitingBlk))
          return true;
    return false;
  }

  bool isClassElementTypeArray(SILValue Arr) {
    auto Ty = Arr->getType();
    if (auto BGT = Ty.getAs<BoundGenericStructType>()) {
      // Check the array element type parameter.
      bool isClass = false;
      for (auto EltTy : BGT->getGenericArgs()) {
        if (!EltTy->hasReferenceSemantics())
          return false;
        isClass = true;
      }
      return isClass;
    }
    return false;
  }

  bool canHoistArrayPropsInst(ArraySemanticsCall Call) {
    // TODO: This is way conservative. If there is an unconditionally
    // executed call to the same array we can still hoist it.
    if (isConditionallyExecuted(Call))
      return false;

    SILValue Arr = Call.getSelf();

    // We don't attempt to hoist non-class element type arrays.
    if (!isClassElementTypeArray(Arr))
      return false;

    // We can strip the load that might even occur in the loop because we make
    // sure that no unsafe store to the array's address takes place.
    Arr = stripArrayStructLoad(Arr);

    // Have we already seen this array and deemed it safe?
    if (HoistableArray.count(Arr))
      return true;

    // Do we know how to hoist the arguments of this call.
    if (!Call.canHoist(Preheader->getTerminator(), DomTree))
      return false;

    SmallVector<int, 4> AccessPath;
    SILValue ArrayContainer =
      StructUseCollector::getAccessPath(Arr, AccessPath);

    if (!isSafeArrayContainer(ArrayContainer))
      return false;

    StructUseCollector StructUses;
    StructUses.collectUses(ArrayContainer, AccessPath);

    if (!checkSafeArrayAddressUses(StructUses.AggregateAddressUsers) ||
        !checkSafeArrayAddressUses(StructUses.StructAddressUsers) ||
        !checkSafeArrayValueUses(StructUses.StructValueUsers) ||
        !checkSafeElementValueUses(StructUses.ElementValueUsers) ||
        !StructUses.ElementAddressUsers.empty())
    return false;

    HoistableArray.insert(Arr);
    return true;
  }
};
} // end anonymous namespace

namespace {
/// Clone a single exit multiple exit region starting at basic block and ending
/// in a set of basic blocks. Updates the dominator tree with the cloned blocks.
/// However, the client needs to update the dominator of the exit blocks.
///
/// FIXME: All functionality for generating valid SIL (including the DomTree)
/// should be handled by the common SILCloner. Currently, SILCloner only updates
/// the DomTree for original (non-cloned) blocks when splitting edges. The
/// cloned blocks won't be mapped to dominator nodes until fixDomTree()
/// runs. However, since SILCloner always handles single-entry regions,
/// fixDomTree() could be part of SILCloner itself.
class RegionCloner : public SILCloner<RegionCloner> {
  SILBasicBlock *StartBB;

  friend class SILInstructionVisitor<RegionCloner>;
  friend class SILCloner<RegionCloner>;

public:
  RegionCloner(SILBasicBlock *EntryBB, DominanceInfo &DT)
      : SILCloner<RegionCloner>(*EntryBB->getParent(), &DT), StartBB(EntryBB) {}

  SILBasicBlock *cloneRegion(ArrayRef<SILBasicBlock *> exitBBs) {
    assert(DomTree->getNode(StartBB) != nullptr && "Can't cloned dead code");

    cloneReachableBlocks(StartBB, exitBBs);

    // Add dominator tree nodes for the new basic blocks.
    fixDomTree();

    // Update SSA form for values used outside of the copied region.
    updateSSAForm();
    return getOpBasicBlock(StartBB);
  }

protected:
  /// Clone the dominator tree from the original region to the cloned region.
  void fixDomTree() {
    for (auto *BB : originalPreorderBlocks()) {
      auto *ClonedBB = getOpBasicBlock(BB);
      auto *OrigDomBB = DomTree->getNode(BB)->getIDom()->getBlock();
      if (BB == StartBB) {
        // The cloned start node shares the same dominator as the original node.
        auto *ClonedNode = DomTree->addNewBlock(ClonedBB, OrigDomBB);
        (void)ClonedNode;
        assert(ClonedNode);
        continue;
      }
      // Otherwise, map the dominator structure using the mapped block.
      DomTree->addNewBlock(ClonedBB, getOpBasicBlock(OrigDomBB));
    }
  }

  SILValue getMappedValue(SILValue V) {
    if (auto *BB = V->getParentBlock()) {
      if (!DomTree->dominates(StartBB, BB)) {
        // Must be a value that dominates the start basic block.
        assert(DomTree->dominates(BB, StartBB)
               && "Must dominated the start of the cloned region");
        return V;
      }
    }
    return SILCloner<RegionCloner>::getMappedValue(V);
  }

  void postProcess(SILInstruction *Orig, SILInstruction *Cloned) {
    SILCloner<RegionCloner>::postProcess(Orig, Cloned);
  }

  /// Update SSA form for values that are used outside the region.
  void updateSSAForValue(SILBasicBlock *OrigBB, SILValue V,
                         SILSSAUpdater &SSAUp) {
    // Collect outside uses.
    SmallVector<UseWrapper, 16> UseList;
    for (auto Use : V->getUses()) {
      if (!isBlockCloned(Use->getUser()->getParent())) {
        UseList.push_back(UseWrapper(Use));
      }
    }
    if (UseList.empty())
      return;

    // Update SSA form.
    SSAUp.initialize(V->getType(), V->getOwnershipKind());
    SSAUp.addAvailableValue(OrigBB, V);
    SILValue NewVal = getMappedValue(V);
    SSAUp.addAvailableValue(getOpBasicBlock(OrigBB), NewVal);
    for (auto U : UseList) {
      Operand *Use = U;
      SSAUp.rewriteUse(*Use);
    }
  }

  void updateSSAForm() {
    SILSSAUpdater SSAUp;
    SmallVector<SingleValueInstruction *, 4> newProjections;
    SinkAddressProjections sinkProj(&newProjections);

    for (auto *origBB : originalPreorderBlocks()) {
      // Update outside used phi values.
      for (auto *arg : origBB->getArguments()) {
        updateSSAForValue(origBB, arg, SSAUp);
      }

      // Update outside used instruction values.
      for (auto &inst : *origBB) {
        for (auto result : inst.getResults()) {
          bool success = sinkProj.analyzeAddressProjections(&inst);
          assert(success);
          // Sink address projections by cloning to avoid address phis.
          sinkProj.cloneProjections();

          // If no new projections were created, update ssa for the result only.
          if (newProjections.empty()) {
            updateSSAForValue(origBB, result, SSAUp);
            continue;
          }

          for (auto *newProj : newProjections) {
            // Operand values of new projections may need ssa update.
            for (auto opVal : newProj->getOperandValues()) {
              if (!isBlockCloned(opVal->getParentBlock())) {
                continue;
              }
              updateSSAForValue(origBB, opVal, SSAUp);
            }
          }
          newProjections.clear();
        }
      }
    }
  }
};
} // end anonymous namespace

namespace {
/// This class transforms a hoistable loop nest into a speculatively specialized
/// loop based on array.props calls.
class ArrayPropertiesSpecializer {
  DominanceInfo *DomTree;
  SILLoopAnalysis *LoopAnalysis;
  SILBasicBlock *HoistableLoopPreheader;

public:
  ArrayPropertiesSpecializer(DominanceInfo *DT, SILLoopAnalysis *LA,
                             SILBasicBlock *Hoistable)
      : DomTree(DT), LoopAnalysis(LA), HoistableLoopPreheader(Hoistable) {}

  void run() {
    specializeLoopNest();
  }

  SILLoop *getLoop() {
    auto *LoopInfo = LoopAnalysis->get(HoistableLoopPreheader->getParent());
    return LoopInfo->getLoopFor(
        HoistableLoopPreheader->getSingleSuccessorBlock());
  }

protected:
  void specializeLoopNest();
};
} // end anonymous namespace

static SILValue createStructExtract(SILBuilder &B, SILLocation Loc,
                                    SILValue Opd, unsigned FieldNo) {
  SILType Ty = Opd->getType();
  auto SD = Ty.getStructOrBoundGenericStruct();
  auto Properties = SD->getStoredProperties();
  unsigned Counter = 0;
  for (auto *D : Properties)
    if (Counter++ == FieldNo)
      return B.createStructExtract(Loc, Opd, D);
  llvm_unreachable("Wrong field number");
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

/// Create a binary and function.
static SILValue createAnd(SILBuilder &B, SILLocation Loc, SILValue Opd1,
                          SILValue Opd2) {
  auto AndFn = getBinaryFunction("and", Opd1->getType(), B.getASTContext());
  SILValue Args[] = {Opd1, Opd2};
  return B.createBuiltin(Loc, AndFn, Opd1->getType(), {}, Args);
}

/// Create a check over all array.props calls that they have the 'fast native
/// swift' array value: isNative && !needsElementTypeCheck must be true.
static SILValue
createFastNativeArraysCheck(SmallVectorImpl<ArraySemanticsCall> &ArrayProps,
                            SILBuilder &B) {
  assert(!ArrayProps.empty() && "Must have array.pros calls");

  SILType IntBoolTy = SILType::getBuiltinIntegerType(1, B.getASTContext());
  SILValue Result =
      B.createIntegerLiteral((*ArrayProps[0]).getLoc(), IntBoolTy, 1);

  for (auto Call : ArrayProps) {
    auto Loc = (*Call).getLoc();
    auto CallKind = Call.getKind();
    if (CallKind == ArrayCallKind::kArrayPropsIsNativeTypeChecked) {
      auto Val = createStructExtract(B, Loc, SILValue(Call), 0);
      Result = createAnd(B, Loc, Result, Val);
    }
  }
  return Result;
}

/// Collect all array.props calls in the cloned basic blocks stored in the map,
/// asserting that we found at least one.
static void collectArrayPropsCalls(RegionCloner &Cloner,
                                   SmallVectorImpl<SILBasicBlock *> &ExitBlocks,
                                   SmallVectorImpl<ArraySemanticsCall> &Calls) {
  for (auto *origBB : Cloner.originalPreorderBlocks()) {
    auto clonedBB = Cloner.getOpBasicBlock(origBB);
    for (auto &Inst : *clonedBB) {
      ArraySemanticsCall ArrayProps(&Inst, "array.props", true);
      if (!ArrayProps)
        continue;
      Calls.push_back(ArrayProps);
    }
  }
  assert(!Calls.empty() && "Should have a least one array.props call");
}

/// Replace an array.props call by the 'fast swift array' value.
///
/// This is true for array.props.isNative and false for
/// array.props.needsElementTypeCheck.
static void replaceArrayPropsCall(SILBuilder &B, ArraySemanticsCall C) {
  assert(C.getKind() == ArrayCallKind::kArrayPropsIsNativeTypeChecked);
  ApplyInst *AI = C;

  SILType IntBoolTy = SILType::getBuiltinIntegerType(1, B.getASTContext());

  auto BoolTy = AI->getType();
  auto C0 = B.createIntegerLiteral(AI->getLoc(), IntBoolTy, 1);
  auto BoolVal = B.createStruct(AI->getLoc(), BoolTy, {C0});

  (*C).replaceAllUsesWith(BoolVal);
    // Remove call to array.props.read/write.
  C.removeCall();
}

/// Collects all loop dominated blocks outside the loop that are immediately
/// dominated by the loop.
static void
collectImmediateLoopDominatedBlocks(const SILLoop *Lp, DominanceInfoNode *Node,
                                    SmallVectorImpl<SILBasicBlock *> &Blocks) {
  SILBasicBlock *BB = Node->getBlock();

  // Base case: First loop dominated block outside of loop.
  if (!Lp->contains(BB)) {
    Blocks.push_back(BB);
    return;
  }

  // Loop contains the basic block. Look at immediately dominated nodes.
  for (auto *Child : *Node)
    collectImmediateLoopDominatedBlocks(Lp, Child, Blocks);
}

void ArrayPropertiesSpecializer::specializeLoopNest() {
  auto *Lp = getLoop();
  assert(Lp);

  // Split of a new empty preheader. We don't want to duplicate the whole
  // original preheader it might contain instructions that we can't clone.
  // This will be block that will contain the check whether to execute the
  // 'native swift array' loop or the original loop.
  SILBuilder B(HoistableLoopPreheader);
  auto *CheckBlock = splitBasicBlockAndBranch(B,
      HoistableLoopPreheader->getTerminator(), DomTree, nullptr);

  auto *Header = CheckBlock->getSingleSuccessorBlock();
  assert(Header);

  // Collect all loop dominated blocks (e.g exit blocks could be among them). We
  // need to update their dominator.
  SmallVector<SILBasicBlock *, 16> LoopDominatedBlocks;
  collectImmediateLoopDominatedBlocks(Lp, DomTree->getNode(Header),
                                      LoopDominatedBlocks);

  // Collect all exit blocks.
  SmallVector<SILBasicBlock *, 16> ExitBlocks;
  Lp->getExitBlocks(ExitBlocks);

  // Split the preheader before the first instruction.
  SILBasicBlock *NewPreheader =
    splitBasicBlockAndBranch(B, &*CheckBlock->begin(), DomTree, nullptr);

  // Clone the region from the new preheader up to (not including) the exit
  // blocks. This creates a second loop nest.
  RegionCloner Cloner(NewPreheader, *DomTree);
  auto *ClonedPreheader = Cloner.cloneRegion(ExitBlocks);

  // Collect the array.props call that we will specialize on that we have
  // cloned in the cloned loop.
  SmallVector<ArraySemanticsCall, 16> ArrayPropCalls;
  collectArrayPropsCalls(Cloner, ExitBlocks, ArrayPropCalls);

  // Move them to the check block.
  SmallVector<ArraySemanticsCall, 16> HoistedArrayPropCalls;
  for (auto C: ArrayPropCalls)
    HoistedArrayPropCalls.push_back(
        ArraySemanticsCall(C.copyTo(CheckBlock->getTerminator(), DomTree)));

  // Create a conditional branch on the fast condition being true.
  B.setInsertionPoint(CheckBlock->getTerminator());
  auto IsFastNativeArray =
      createFastNativeArraysCheck(HoistedArrayPropCalls, B);
  B.createCondBranch(CheckBlock->getTerminator()->getLoc(),
                     IsFastNativeArray, ClonedPreheader, NewPreheader);
  CheckBlock->getTerminator()->eraseFromParent();

  // Fixup the loop dominated blocks. They are now dominated by the check block.
  for (auto *BB : LoopDominatedBlocks)
    DomTree->changeImmediateDominator(DomTree->getNode(BB),
                                      DomTree->getNode(CheckBlock));

  // Replace the array.props calls uses in the cloned loop by their 'fast'
  // value.
  SILBuilder B2(ClonedPreheader->getTerminator());
  for (auto C : ArrayPropCalls)
    replaceArrayPropsCall(B2, C);

  // We have potentially cloned a loop - invalidate loop info.
  LoopAnalysis->invalidate(Header->getParent(),
                           SILAnalysis::InvalidationKind::FunctionBody);
}

namespace {
class SwiftArrayPropertyOptPass : public SILFunctionTransform {

  void run() override {
    auto *Fn = getFunction();

    // Don't hoist array property calls at Osize.
    if (Fn->optimizeForSize())
      return;

    DominanceAnalysis *DA = PM->getAnalysis<DominanceAnalysis>();
    SILLoopAnalysis *LA = PM->getAnalysis<SILLoopAnalysis>();
    SILLoopInfo *LI = LA->get(Fn);

    bool HasChanged = false;

    // Check whether we can hoist 'array.props' calls out of loops, collecting
    // the preheader we can hoist to. We only hoist out of loops if 'all'
    // array.props call can be hoisted for a given loop nest.
    // We process the loop tree preorder (top-down) to hoist over the biggest
    // possible loop-nest.
    SmallVector<SILBasicBlock *, 16> HoistableLoopNests;
    std::function<void(SILLoop *)> processChildren = [&](SILLoop *L) {
      ArrayPropertiesAnalysis Analysis(L, DA);
      if (Analysis.run()) {
        // Hoist in the current loop nest.
        HasChanged = true;
        HoistableLoopNests.push_back(L->getLoopPreheader());
      } else {
        // Otherwise, try hoisting sub-loops.
        for (auto *SubLoop : *L)
          processChildren(SubLoop);
      }
    };
    for (auto *L : *LI)
      processChildren(L);

    // Specialize the identified loop nest based on the 'array.props' calls.
    if (HasChanged) {
      DominanceInfo *DT = DA->get(getFunction());

      // Process specialized loop-nests in loop-tree post-order (bottom-up).
      std::reverse(HoistableLoopNests.begin(), HoistableLoopNests.end());

      // Hoist the loop nests.
      for (auto &HoistableLoopNest : HoistableLoopNests)
        ArrayPropertiesSpecializer(DT, LA, HoistableLoopNest).run();

      // Verify that no illegal critical edges were created.
      if (getFunction()->getModule().getOptions().VerifyAll)
        getFunction()->verifyCriticalEdges();

      // We preserve the dominator tree. Let's invalidate everything
      // else.
      DA->lockInvalidation();
      invalidateAnalysis(SILAnalysis::InvalidationKind::FunctionBody);
      DA->unlockInvalidation();
    }
  }

};
} // end anonymous namespace

SILTransform *swift::createSwiftArrayPropertyOpt() {
  return new SwiftArrayPropertyOptPass();
}
