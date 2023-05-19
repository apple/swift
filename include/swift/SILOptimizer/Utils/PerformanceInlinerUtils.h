//===--- PerformanceInlinerUtils.h - Common performance inliner utils.  ---===//
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

#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SILOptimizer/Analysis/ColdBlockInfo.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/Analysis/LoopAnalysis.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/CommandLine.h"


using namespace swift;

extern llvm::cl::opt<bool> EnableSILInliningOfGenerics;

namespace swift {
class BasicCalleeAnalysis;

// Controls the decision to inline functions with @_semantics, @effect and
// global_init attributes.
enum class InlineSelection {
  Everything,
  NoSemanticsAndEffects,
  OnlyInlineAlways,
};

/// Check if this ApplySite is eligible for inlining. If so, return the callee.
SILFunction *getEligibleFunction(FullApplySite AI,
                                 InlineSelection WhatToInline);

// Returns true if this is a pure call, i.e. the callee has no side-effects
// and all arguments are constants.
bool isPureCall(FullApplySite AI, BasicCalleeAnalysis *BCA);

/// Fundamental @_semantic tags provide additional semantics for optimization
/// beyond what SIL analysis can discover from the function body. They should be
/// preserved until semantic passes can process them. For example, they may need
/// to be hoisted by LICM after inlining up to the highest level caller.
///
/// Nested @_semantic tags also provide semantics beyond the function body but
/// they hide the semantics of underlying calls. These need to be inlined into
/// the highest level caller before semantics passes can process the underlying
/// Fundamental semantic functions.
///
/// Transient @_semantic tags are not required for optimization.
enum class SemanticFunctionLevel { Fundamental, Transient, Nested };

/// Return the SemanticFunctionLevel of \p callee.
SemanticFunctionLevel getSemanticFunctionLevel(SILFunction *function);

inline bool isOptimizableSemanticFunction(SILFunction *function) {
  return getSemanticFunctionLevel(function) != SemanticFunctionLevel::Transient;
}

/// Return true if \p apply calls into an optimizable semantic function from
/// within another semantic function, or from a "trivial" wrapper.
bool isNestedSemanticCall(FullApplySite apply);

} // end swift namespace

//===----------------------------------------------------------------------===//
//                               ConstantTracker
//===----------------------------------------------------------------------===//

// Tracks constants in the caller and callee to get an estimation of what
// values get constant if the callee is inlined.
// This can be seen as a "simulation" of several optimizations: SROA, mem2reg
// and constant propagation.
// Note that this is only a simplified model and not correct in all cases.
// For example aliasing information is not taken into account.
class ConstantTracker {

  // Represents a value in integer constant evaluation.
  struct IntConst {
    IntConst() : isValid(false), isFromCaller(false) { }

    IntConst(const APInt &value, bool isFromCaller) :
    value(value), isValid(true), isFromCaller(isFromCaller) { }

    // The actual value.
    APInt value;

    // True if the value is valid, i.e. could be evaluated to a constant.
    bool isValid;

    // True if the value is only valid, because a constant is passed to the
    // callee. False if constant propagation could do the same job inside the
    // callee without inlining it.
    bool isFromCaller;
  };
  
  // Links between loaded and stored values.
  // The key is a load instruction, the value is the corresponding store
  // instruction which stores the loaded value. Both, key and value can also
  // be copy_addr instructions.
  llvm::DenseMap<SILInstruction *, SILInstruction *> links;
  
  // The current stored values at memory addresses.
  // The key is the base address of the memory (after skipping address
  // projections). The value are store (or copy_addr) instructions, which
  // store the current value.
  // This is only an estimation, because e.g. it does not consider potential
  // aliasing.
  llvm::DenseMap<SILValue, SILInstruction *> memoryContent;
  
  // Cache for evaluated constants.
  llvm::SmallDenseMap<BuiltinInst *, IntConst> constCache;

  // The caller/callee function which is tracked.
  SILFunction *F;
  
  // The constant tracker of the caller function (null if this is the
  // tracker of the callee).
  ConstantTracker *callerTracker;
  
  // The apply instruction in the caller (null if this is the tracker of the
  // callee).
  FullApplySite AI;
  
  // Walks through address projections and (optionally) collects them.
  // Returns the base address, i.e. the first address which is not a
  // projection.
  SILValue scanProjections(SILValue addr,
                           SmallVectorImpl<Projection> *Result = nullptr);
  
  // Get the stored value for a load. The loadInst can be either a real load
  // or a copy_addr.
  SILValue getStoredValue(SILInstruction *loadInst,
                          ProjectionPath &projStack);

  // Gets the parameter in the caller for a function argument.
  SILValue getParam(SILValue value) {
    if (auto *Arg = dyn_cast<SILFunctionArgument>(value)) {
      if (AI && Arg->getFunction() == F) {
        // Continue at the caller.
        return AI.getArgument(Arg->getIndex());
      }
    }
    return SILValue();
  }
  
  SILInstruction *getMemoryContent(SILValue addr) {
    // The memory content can be stored in this ConstantTracker or in the
    // caller's ConstantTracker.
    SILInstruction *storeInst = memoryContent[addr];
    if (storeInst)
      return storeInst;
    if (callerTracker)
      return callerTracker->getMemoryContent(addr);
    return nullptr;
  }
  
  // Gets the estimated definition of a value.
  SILInstruction *getDef(SILValue val, ProjectionPath &projStack);

  // Gets the estimated integer constant result of a builtin.
  IntConst getBuiltinConst(BuiltinInst *BI, int depth);
  
public:
  
  // Constructor for the caller function.
  ConstantTracker(SILFunction *function) :
    F(function), callerTracker(nullptr), AI()
  { }
  
  // Constructor for the callee function.
  ConstantTracker(SILFunction *function, ConstantTracker *caller,
                  FullApplySite callerApply) :
     F(function), callerTracker(caller), AI(callerApply)
  { }
  
  void beginBlock() {
    // Currently we don't do any sophisticated dataflow analysis, so we keep
    // the memoryContent alive only for a single block.
    memoryContent.clear();
  }

  // Must be called for each instruction visited in dominance order.
  void trackInst(SILInstruction *inst);
  
  // Gets the estimated definition of a value.
  SILInstruction *getDef(SILValue val) {
    ProjectionPath projStack(val->getType());
    return getDef(val, projStack);
  }
  
  // Gets the estimated definition of a value if it is in the caller.
  SILInstruction *getDefInCaller(SILValue val) {
    SILInstruction *def = getDef(val);
    if (def && def->getFunction() != F)
      return def;
    return nullptr;
  }

  bool isStackAddrInCaller(SILValue val) {
    if (SILValue Param = getParam(stripAddressProjections(val))) {
      return isa<AllocStackInst>(stripAddressProjections(Param));
    }
    return false;
  }

  // Gets the estimated integer constant of a value.
  IntConst getIntConst(SILValue val, int depth = 0);

  SILBasicBlock *getTakenBlock(TermInst *term);
};

//===----------------------------------------------------------------------===//
//                           Shortest path analysis
//===----------------------------------------------------------------------===//

/// Computes the length of the shortest path through a block in a scope.
///
/// A scope is either a loop or the whole function. The "length" is an
/// estimation of the execution time. For example if the shortest path of a
/// block B is N, it means that the execution time of the scope (either
/// function or a single loop iteration), when execution includes the block, is
/// at least N, regardless of what branches are taken.
///
/// The shortest path of a block is the sum of the shortest path from the scope
/// entry and the shortest path to the scope exit.
class ShortestPathAnalysis {
public:
  enum {
    /// We assume that cold block take very long to execute.
    ColdBlockLength = 1000,

    /// Our assumption on how many times a loop is executed.
    LoopCount = 10,

    /// To keep things simple we only analyze up to this number of nested loops.
    MaxNumLoopLevels = 4,

    /// The "weight" for the benefit which a single loop nest gives.
    SingleLoopWeight = 4,

    /// Pretty large but small enough to add something without overflowing.
    InitialDist = (1 << 29)
  };

  /// A weight for an inlining benefit.
  class Weight {
    /// How long does it take to execute the scope (either loop or the whole
    /// function) of the call to inline? The larger it is the less important it
    /// is to inline.
    int ScopeLength;

    /// Represents the loop nest. The larger it is the more important it is to
    /// inline
    int LoopWeight;

    friend class ShortestPathAnalysis;

  public:
    Weight(int ScopeLength, int LoopWeight) : ScopeLength(ScopeLength),
                                              LoopWeight(LoopWeight) { }

    Weight(const Weight &RHS, int AdditionalLoopWeight) :
      Weight(RHS.ScopeLength, RHS.LoopWeight + AdditionalLoopWeight) { }

    Weight() : ScopeLength(-1), LoopWeight(0) {
      assert(!isValid());
    }

    bool isValid() const { return ScopeLength >= 0; }

    /// Updates the \p Benefit by weighting the \p Importance.
    void updateBenefit(int &Benefit, int Importance) const;

#ifndef NDEBUG
    friend raw_ostream &operator<<(raw_ostream &os, const Weight &W) {
      os << W.LoopWeight << '/' << W.ScopeLength;
      return os;
    }
#endif
  };

private:
  /// The distances for a block in it's scope (either loop or function).
  struct Distances {
    /// The shortest distance from the scope entry to the block entry.
    int DistFromEntry = InitialDist;

    /// The shortest distance from the block entry to the scope exit.
    int DistToExit = InitialDist;

    /// Additional length to account for loop iterations. It's != 0 for loop
    /// headers (or loop predecessors).
    int LoopHeaderLength = 0;
  };

  /// Holds the distance information for a block in all it's scopes, i.e. in all
  /// its containing loops and the function itself.
  struct BlockInfo {
  private:
    /// Distances for all scopes. Element 0 refers to the function, elements
    /// > 0 to loops.
    Distances Dists[MaxNumLoopLevels];
  public:
    /// The length of the block itself.
    int Length = 0;

    /// Returns the distances for the loop with nesting level \p LoopDepth.
    Distances &getDistances(int LoopDepth) {
      assert(LoopDepth >= 0 && LoopDepth < MaxNumLoopLevels);
      return Dists[LoopDepth];
    }

    /// Returns the length including the LoopHeaderLength for a given
    /// \p LoopDepth.
    int getLength(int LoopDepth) {
      return Length + getDistances(LoopDepth).LoopHeaderLength;
    }

    /// Returns the length of the shortest path of this block in the loop with
    /// nesting level \p LoopDepth.
    int getScopeLength(int LoopDepth) {
      const Distances &D = getDistances(LoopDepth);
      return D.DistFromEntry + D.DistToExit;
    }
  };

  SILFunction *F;
  SILLoopInfo *LI;
  llvm::DenseMap<const SILBasicBlock *, BlockInfo *> BlockInfos;
  std::vector<BlockInfo> BlockInfoStorage;
  bool valid = false;

  BlockInfo *getBlockInfo(const SILBasicBlock *BB) {
    BlockInfo *BI = BlockInfos[BB];
    assert(BI);
    return BI;
  }

  // Utility functions to make the template solveDataFlow compilable for a block
  // list containing references _and_ a list containing pointers.

  const SILBasicBlock *getBlock(const SILBasicBlock *BB) { return BB; }
  const SILBasicBlock *getBlock(const SILBasicBlock &BB) { return &BB; }

  /// Returns the minimum distance from all predecessor blocks of \p BB.
  int getEntryDistFromPreds(const SILBasicBlock *BB, int LoopDepth);

  /// Returns the minimum distance from all successor blocks of \p BB.
  int getExitDistFromSuccs(const SILBasicBlock *BB, int LoopDepth);

  /// Computes the distances by solving the dataflow problem for all \p Blocks
  /// in a scope.
  template <typename BlockList>
  void solveDataFlow(const BlockList &Blocks, int LoopDepth) {
    bool Changed = false;

    // Compute the distances from the entry block.
    do {
      Changed = false;
      for (auto &Block : Blocks) {
        const SILBasicBlock *BB = getBlock(Block);
        BlockInfo *BBInfo = getBlockInfo(BB);
        int DistFromEntry = getEntryDistFromPreds(BB, LoopDepth);
        Distances &BBDists = BBInfo->getDistances(LoopDepth);
        if (DistFromEntry < BBDists.DistFromEntry) {
          BBDists.DistFromEntry = DistFromEntry;
          Changed = true;
        }
      }
    } while (Changed);

    // Compute the distances to the exit block.
    do {
      Changed = false;
      for (auto &Block : reverse(Blocks)) {
        const SILBasicBlock *BB = getBlock(Block);
        BlockInfo *BBInfo = getBlockInfo(BB);
        int DistToExit =
          getExitDistFromSuccs(BB, LoopDepth) + BBInfo->getLength(LoopDepth);
        Distances &BBDists = BBInfo->getDistances(LoopDepth);
        if (DistToExit < BBDists.DistToExit) {
          BBDists.DistToExit = DistToExit;
          Changed = true;
        }
      }
    } while (Changed);
  }

  /// Analyze \p Loop and all its inner loops.
  void analyzeLoopsRecursively(SILLoop *Loop, int LoopDepth);

  void printFunction(llvm::raw_ostream &OS);

  void printLoop(llvm::raw_ostream &OS, SILLoop *Loop, int LoopDepth);

  void printBlockInfo(llvm::raw_ostream &OS, SILBasicBlock *BB, int LoopDepth);

public:
  ShortestPathAnalysis(SILFunction *F, SILLoopInfo *LI) : F(F), LI(LI) { }

  bool isValid() const { return valid; }

  /// Compute the distances. The function \p getApplyLength returns the length
  /// of a function call.
  template <typename Func>
  void analyze(ColdBlockInfo &CBI, Func getApplyLength) {
    assert(!isValid());
    valid = true;
    unsigned numBlocks = F->size();

    // As the complexity of the analysis is more than linear with the number of blocks,
    // disable it for huge functions. In this case inlining will be less aggressive.
    if (numBlocks > 2000)
      return;

    BlockInfoStorage.resize(numBlocks);

    // First step: compute the length of the blocks.
    unsigned BlockIdx = 0;
    for (SILBasicBlock &BB : *F) {
      int Length = 0;
      if (CBI.isCold(&BB)) {
        Length = ColdBlockLength;
      } else {
        for (SILInstruction &I : BB) {
          if (auto FAS = FullApplySite::isa(&I)) {
            Length += getApplyLength(FAS);
          } else {
            Length += (int)instructionInlineCost(I);
          }
        }
      }
      BlockInfo *BBInfo = &BlockInfoStorage[BlockIdx++];
      BlockInfos[&BB] = BBInfo;
      BBInfo->Length = Length;

      // Initialize the distances for the entry and exit blocks, used when
      // computing the distances for the function itself.
      if (&BB == &F->front())
        BBInfo->getDistances(0).DistFromEntry = 0;

      if (isa<ReturnInst>(BB.getTerminator()))
        BBInfo->getDistances(0).DistToExit = Length;
      else if (isa<ThrowInst>(BB.getTerminator()))
        BBInfo->getDistances(0).DistToExit = Length + ColdBlockLength;
    }
    // Compute the distances for all loops in the function.
    for (SILLoop *Loop : *LI) {
      analyzeLoopsRecursively(Loop, 1);
    }
    // Compute the distances for the function itself.
    solveDataFlow(*F, 0);
  }

  /// Returns the length of the shortest path of the block \p BB in the loop
  /// with nesting level \p LoopDepth. If \p LoopDepth is 0 ir returns the
  /// shortest path in the function.
  int getScopeLength(SILBasicBlock *BB, int LoopDepth) {
    assert(BB->getParent() == F);

    // Return a conservative default if the analysis was not done due to a high number of blocks.
    if (BlockInfos.empty())
      return ColdBlockLength;

    if (LoopDepth >= MaxNumLoopLevels)
      LoopDepth = MaxNumLoopLevels - 1;
    return getBlockInfo(BB)->getScopeLength(LoopDepth);
  }

  /// Returns the weight of block \p BB also considering the \p CallerWeight
  /// which is the weight of the call site's block in the caller.
  Weight getWeight(SILBasicBlock *BB, Weight CallerWeight);

  void dump();
};
