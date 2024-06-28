//==-- DiagnoseInfiniteRecursion.cpp - Find infinitely-recursive applies --==//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements a diagnostic pass that detects infinite recursive
// function calls.
//
// It detects simple forms of infinite recursions, like
//
//   func f() {
//     f()
//   }
//
// and can also deal with invariant conditions, like availability checks
//
//   func f() {
//     if #available(macOS 10.4.4, *) {
//       f()
//     }
//   }
//
// or invariant conditions due to forwarded arguments:
//
//   func f(_ x: Int) {
//     if x > 0 {
//       f(x)
//     }
//   }
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "infinite-recursion"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/Basic/Assertions.h"
#include "swift/SIL/CalleeCache.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/Basic/LLVMExtras.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/MemAccessUtils.h"
#include "swift/SIL/BasicBlockData.h"
#include "swift/SIL/BasicBlockDatastructures.h"
#include "swift/SIL/NodeBits.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "llvm/Support/Debug.h"

using namespace swift;

namespace {

/// Returns true if \p inst is a full-apply site which calls the containing
/// function.
static bool isRecursiveCall(FullApplySite applySite) {
  SILFunction *parentFunc = applySite.getFunction();
  if (SILFunction *calledFn = applySite.getReferencedFunctionOrNull())
    return calledFn == parentFunc;

  // Don't touch dynamic dispatch.
  const auto callee = applySite.getCallee();
  if (isa<SuperMethodInst>(callee) ||
      isa<ObjCSuperMethodInst>(callee) ||
      isa<ObjCMethodInst>(callee)) {
    return false;
  }

  if (auto *CMI = dyn_cast<ClassMethodInst>(callee)) {

    SILModule &module = parentFunc->getModule();
    CanType classType = CMI->getOperand()->getType().getASTType();
    if (auto mt = dyn_cast<MetatypeType>(classType)) {
      classType = mt.getInstanceType();
    }
    ClassDecl *classDecl = classType.getClassOrBoundGenericClass();

    // FIXME: If we're not inside the module context of the method,
    // we may have to deserialize vtables.  If the serialized tables
    // are damaged, the pass will crash.
    //
    // Though, this has the added bonus of not looking into vtables
    // outside the current module.  Because we're not doing IPA, let
    // alone cross-module IPA, this is all well and good.
    if (classDecl && classDecl->getModuleContext() != module.getSwiftModule())
      return false;

    SILFunction *method = getTargetClassMethod(module, classDecl, classType, CMI);
    if (method != parentFunc)
      return false;

    SILDeclRef member = CMI->getMember();
    if (calleesAreStaticallyKnowable(module, member) &&
        // The "statically knowable" check just means that we have all the
        // callee candidates available for analysis. We still need to check
        // if the current function has a known override point.
        !member.getAbstractFunctionDecl()->isOverridden()) {
      return true;
    }

    // Even if the method is (or could be) overridden, it's a recursive call if
    // it's called on the self argument:
    // ```
    // class X {
    //   // Even if foo() is overridden in a derived class, it'll end up in an
    //   // infinite recursion if initially called on an instance of `X`.
    //   func foo() { foo() }
    // }
    // ```
    if (parentFunc->hasSelfParam() &&
        CMI->getOperand() == SILValue(parentFunc->getSelfArgument())) {
      return true;
    }
    return false;
  }

  if (auto *WMI = dyn_cast<WitnessMethodInst>(callee)) {
    auto funcAndTable = parentFunc->getModule().lookUpFunctionInWitnessTable(
        WMI->getConformance(), WMI->getMember(), SILModule::LinkingMode::LinkNormal);
    return funcAndTable.first == parentFunc;
  }
  return false;
}

/// For the purpose of this analysis we can exclude certain memory-writing
/// instructions.
static bool mayWriteToMemory(SILInstruction *inst) {
  switch (inst->getKind()) {
  case SILInstructionKind::LoadInst:
    // A `load` is defined to write memory or have side effects in two cases:
    // * We don't care about retain instructions of a `load [copy]`.
    // * We don't care about a `load [take]` because it cannot occur in an
    //   infinite recursion loop without another write (which re-initializes
    //   the memory).
  case SILInstructionKind::BeginAccessInst:
  case SILInstructionKind::EndAccessInst:
    return false;
  default:
    return inst->mayWriteToMemory();
  }
}

/// Describes what is expected to be invariant in an infinite recursion loop.
///
/// * Memory: it's all or nothing. Either all memory is expected to be invariant
///   (= never written) or not. We could use AliasAnalysis to do a more fine-
///   grained analysis, but in mandatory optimizations we want to keep things
///   simple.
///
/// * Arguments: an argument is invariant if a recursive call forwards the
///   incoming argument. For example:
///   \code
///     func f(_ x: Int, _ y: Int) {
///       f(x, y - 1) // The first argument is invariant, the second is not
///     }
///   \endcode
class Invariants {
  enum {
    /// The first bit represents invariant memory.
    invariantMemoryBit = 0,
    /// The remaining bits are used for arguments.
    firstArgBit = 1,
    maxArgIndex = 16 // should be more than enough.
  };

  static_assert((unsigned)(1 << (firstArgBit + maxArgIndex)) != 0,
                "too many argument bits");

  unsigned bitMask;

  explicit Invariants(unsigned bitMask) : bitMask(bitMask) { }

  bool isBitSet(int bitNr) const { return (bitMask & (1 << bitNr)) != 0; }

  /// Recursively walks the use-def chain starting at \p value and returns
  /// true if all visited values are invariant.
  bool isInvariantValue(SILValue value,
                        InstructionSet &visited) const {
    if (SILInstruction *inst = value->getDefiningInstruction()) {
      // Avoid exponential complexity in case a value is used by multiple
      // operands.
      if (!visited.insert(inst))
        return true;

      if (!isMemoryInvariant() && inst->mayReadFromMemory())
        return false;

      for (Operand &op : inst->getAllOperands()) {
        if (!isInvariantValue(op.get(), visited))
          return false;
      }
      return true;
    }

    if (auto *funcArg = dyn_cast<SILFunctionArgument>(value)) {
      return isArgumentInvariant(funcArg->getIndex());
    }

    return false;
  }

  friend llvm::DenseMapInfo<Invariants>;

public:

  static Invariants noInvariants() { return Invariants(0); }

  /// Constructs invariants which include all forwarding arguments of
  /// \p recursiveApply.
  static Invariants fromForwardingArguments(FullApplySite recursiveApply) {
    unsigned bitMask = 0;
    auto incomingArgs = recursiveApply.getFunction()->getArguments();
    for (auto argAndIndex : llvm::enumerate(recursiveApply.getArguments())) {
      unsigned argIdx = argAndIndex.index();
      if (argIdx <= maxArgIndex &&
          stripAccessMarkers(argAndIndex.value()) == incomingArgs[argIdx])
        bitMask |= (1 << (argIdx + firstArgBit));
    }
    return Invariants(bitMask);
  }

  Invariants withInvariantMemory() const {
    return Invariants(bitMask | (1 << invariantMemoryBit));
  }

  bool isMemoryInvariant() const { return isBitSet(invariantMemoryBit); }

  bool isArgumentInvariant(unsigned argIdx) const {
    return argIdx <= maxArgIndex && isBitSet(argIdx + firstArgBit);
  }

  /// Returns true if \p term is a conditional terminator and has an invariant
  /// condition.
  bool isInvariant(TermInst *term) const {
    switch (term->getTermKind()) {
    case TermKind::SwitchEnumAddrInst:
    case TermKind::CheckedCastAddrBranchInst:
      if (!isMemoryInvariant())
        return false;
      LLVM_FALLTHROUGH;
    case TermKind::CondBranchInst:
    case TermKind::SwitchValueInst:
    case TermKind::SwitchEnumInst:
    case TermKind::CheckedCastBranchInst: {
      InstructionSet visited(term->getFunction());
      return isInvariantValue(term->getOperand(0), visited);
    }
    default:
      return false;
    }
  }

  /// Returns true if \p recursiveApply is forwarding all arguments which are
  /// expected to be invariant.
  bool hasInvariantArguments(FullApplySite recursiveApply) const {
    auto incomingArgs = recursiveApply.getFunction()->getArguments();
    for (auto argAndIndex : llvm::enumerate(recursiveApply.getArguments())) {
      unsigned argIdx = argAndIndex.index();
      if (isArgumentInvariant(argIdx) &&
          stripAccessMarkers(argAndIndex.value()) != incomingArgs[argIdx]) {
        return false;
      }
    }
    return true;
  }
};

} // end anonymous namespace

namespace  llvm {
  template<> struct DenseMapInfo<Invariants> {
    static Invariants getEmptyKey() {
      return Invariants(DenseMapInfo<unsigned>::getEmptyKey());
    }
    static Invariants getTombstoneKey() {
      return Invariants(DenseMapInfo<unsigned>::getTombstoneKey());
    }
    static unsigned getHashValue(Invariants deps) {
      return DenseMapInfo<unsigned>::getHashValue(deps.bitMask);
    }
    static bool isEqual(Invariants LHS, Invariants RHS) {
      return LHS.bitMask == RHS.bitMask;
    }
  };
}

namespace {

/// Contains block-specific info which is needed to do the analysis.
struct BlockInfo {
  /// non-null if this block contains a recursive call.
  SILInstruction *recursiveCall;

  /// The number of successors which reach a recursive call, but not the
  /// function exit, i.e. successors for which
  ///   reachesRecursiveCall && !reachesFunctionExit
  unsigned numSuccsReachingRecursiveCall;

  /// True if the block has a terminator with an invariant condition.
  ///
  /// Note: "invariant" means: invariant with respect to the expected invariants,
  ///       which are passed to the constructor.
  bool hasInvariantCondition;

  /// Is there any path from the this block to a function exit, without going
  /// through a recursive call?
  ///
  /// Note that if memory is expected to be invariant, all memory-writing
  /// instructions are also considered as a "function exit".
  bool reachesFunctionExit;

  /// Is there any path from the this block to a recursive call?
  bool reachesRecursiveCall;
  
  /// Get block information with expected \p invariants.
  BlockInfo(SILBasicBlock *block, Invariants invariants) :
      recursiveCall(nullptr),
      numSuccsReachingRecursiveCall(0),
      hasInvariantCondition(invariants.isInvariant(block->getTerminator())),
      reachesFunctionExit(false), reachesRecursiveCall(false) {
    for (SILInstruction &inst : *block) {
      if (auto applySite = FullApplySite::isa(&inst)) {
        // Ignore blocks which call a @_semantics("programtermination_point").
        // This is an assert-like program termination and we explicitly don't
        // want this call to disqualify the warning for infinite recursion,
        // because they're reserved for exceptional circumstances.
        if (applySite.isCalleeKnownProgramTerminationPoint())
          return;

        if (isRecursiveCall(applySite) &&
            invariants.hasInvariantArguments(applySite)) {
          recursiveCall = &inst;
          reachesRecursiveCall = true;
          return;
        }
      }
      if (invariants.isMemoryInvariant() && mayWriteToMemory(&inst)) {
        // If we are assuming that all memory is invariant, a memory-writing
        // instruction potentially breaks the infinite recursion loop. For the
        // sake of the analysis, it's like a function exit.
        reachesFunctionExit = true;
        return;
      }
    }
    TermInst *term = block->getTerminator();
    if (term->isFunctionExiting() ||
        // Also treat non-assert-like unreachables as returns, like "exit()".
        term->isProgramTerminating()) {
      reachesFunctionExit = true;
    }
  }
};

/// Performs the analysis to detect infinite recursion loops.
///
/// The basic idea is to see if there is a path from the entry block to a
/// function return without going through an infinite recursive call.
///
/// The analysis is done with a given set of invariants (see Invariants). The
/// correctness of the result (i.e. no false infinite recursion reported) does
/// _not_ depend on the chosen invariants. But it's a trade-off:
/// The more invariants we include, the more conditions might become invariant
/// (which is good). On the other hand, we have to ignore recursive calls which
/// don't forward all invariant arguments.
///
/// We don't know in advance which invariants will yield the best result, i.e.
/// let us detect an infinite recursion.
/// For example, in f() we can only detect the infinite recursion if we expect
/// that the parameter `x` is invariant.
///
///   func f(_ x: Int) {
///     if x > 0 {   // an invariant condition!
///       f(x)       // the call is forwarding the argument
///     }
///   }
///
/// But in g() we can only detect the infinite recursion if we _don't_ expect
/// that the parameter is invariant.
///
///   func g(_ x: Int) {
///     if x > 0 {   // no invariant condition
///       g(x - 1)   // argument is not forwarded
///     } else {
///       g(x - 2)   // argument is not forwarded
///     }
///   }
///
class InfiniteRecursionAnalysis {
  Invariants invariants;
  BasicBlockData<BlockInfo> blockInfos;

  InfiniteRecursionAnalysis(SILFunction *function, Invariants invariants) :
    invariants(invariants), blockInfos(function,
      [&](SILBasicBlock *block) -> BlockInfo {
        return BlockInfo(block, invariants);
      }) { }

  /// Propagates the `reachesRecursiveCall` flags up the control flow.
  void propagateRecursiveCalls() {
    StackList<SILBasicBlock *> workList(blockInfos.getFunction());

    // Initialize the workList with all blocks which contain recursive calls.
    for (auto bd : blockInfos) {
      if (bd.data.reachesRecursiveCall)
        workList.push_back(&bd.block);
    }

    while (!workList.empty()) {
      SILBasicBlock *block = workList.pop_back_val();
      assert(blockInfos[block].reachesRecursiveCall);
      for (auto *pred : block->getPredecessorBlocks()) {
        BlockInfo &predInfo = blockInfos[pred];
        predInfo.numSuccsReachingRecursiveCall += 1;
        if (!predInfo.reachesRecursiveCall) {
          predInfo.reachesRecursiveCall = true;
          workList.push_back(pred);
        }
      }
    }
  }
  
  /// Propagates the `reachesFunctionExit` flags up the control flow.
  void propagateFunctionExits() {
    StackList<SILBasicBlock *> workList(blockInfos.getFunction());

    // Initialize the workList with all function-exiting blocks.
    for (auto bd : blockInfos) {
      if (bd.data.reachesFunctionExit)
        workList.push_back(&bd.block);
    }

    while (!workList.empty()) {
      SILBasicBlock *block = workList.pop_back_val();
      BlockInfo &info = blockInfos[block];
      assert(info.reachesFunctionExit);
      for (auto *pred : block->getPredecessorBlocks()) {
        BlockInfo &predInfo = blockInfos[pred];

        if (info.reachesRecursiveCall) {
          // Update `numSuccsReachingRecursiveCall`, because this counter
          // excludes successors which reach a function exit.
          assert(predInfo.numSuccsReachingRecursiveCall > 0);
          predInfo.numSuccsReachingRecursiveCall -= 1;
        }

        if (predInfo.reachesFunctionExit ||
            // Recursive calls block the flag propagation.
            predInfo.recursiveCall != nullptr)
          continue;

        // This is the trick for handling invariant conditions: usually the
        // `reachesFunctionExit` flag is propagated if _any_ of the successors
        // has it set.
        // For invariant conditions, it's only propagated if _all_ successors
        // which reach recursive calls also reach a function exit.
        // If at least one of the successors reaches a recursive call (but not
        // a function exit) and this successor is taken once, it will be taken
        // forever (because the condition is invariant).
        if (predInfo.hasInvariantCondition &&
            predInfo.numSuccsReachingRecursiveCall > 0)
          continue;

        predInfo.reachesFunctionExit = true;
        workList.push_back(pred);
      }
    }
  }
  
  /// Finds all infinite recursive calls reachable from the entry and issues
  /// warnings.
  /// Returns true if the function contains infinite recursive calls.
  bool issueWarningsForInfiniteRecursiveCalls() {
    const BlockInfo &entryInfo = blockInfos.entry().data;
    if (!entryInfo.reachesRecursiveCall || entryInfo.reachesFunctionExit)
      return false;

    BasicBlockWorklist workList(blockInfos.getFunction());
    workList.push(&blockInfos.entry().block);

    while (SILBasicBlock *block = workList.pop()) {
      if (auto *recursiveCall = blockInfos[block].recursiveCall) {
        blockInfos.getFunction()->getModule().getASTContext().Diags.diagnose(
                 recursiveCall->getLoc().getSourceLoc(),
                 diag::warn_infinite_recursive_call);
        continue;
      }
      for (auto *succ : block->getSuccessorBlocks()) {
        BlockInfo &succInfo = blockInfos[succ];
        if (succInfo.reachesRecursiveCall && !succInfo.reachesFunctionExit)
          workList.pushIfNotVisited(succ);
      }
    }
    return true;
  }

public:

  LLVM_ATTRIBUTE_USED void dump() {
    for (auto bd : blockInfos) {
      llvm::dbgs() << "bb" << bd.block.getDebugID()
                   << ": numSuccs= " << bd.data.numSuccsReachingRecursiveCall;
      if (bd.data.recursiveCall)
        llvm::dbgs() << " hasRecursiveCall";
      if (bd.data.hasInvariantCondition)
        llvm::dbgs() << " hasInvariantCondition";
      if (bd.data.reachesFunctionExit)
        llvm::dbgs() << " reachesFunctionExit";
      if (bd.data.reachesRecursiveCall)
        llvm::dbgs() << " reachesRecursiveCall";
      llvm::dbgs() << '\n';
    }
  }

  /// Performs the analysis and issues a warnings for recursive calls.
  /// Returns true, if at least one recursive call is found.
  static bool analyzeAndDiagnose(SILFunction *function, Invariants invariants) {
    InfiniteRecursionAnalysis analysis(function, invariants);
    analysis.propagateRecursiveCalls();
    analysis.propagateFunctionExits();
    return analysis.issueWarningsForInfiniteRecursiveCalls();
  }
};

typedef swift::SmallSetVector<Invariants, 4> InvariantsSet;

/// Collect invariants with which we should try the analysis and return true if
/// there is at least one recursive call in the function.
static bool collectInvariantsToTry(SILFunction *function,
                                   InvariantsSet &invariantsToTry) {
  // Try with no invariants.
  invariantsToTry.insert(Invariants::noInvariants());

  bool recursiveCallsFound = false;

  // Scan the function for recursive calls.
  for (SILBasicBlock &block : *function) {
    for (auto &inst : block) {
      auto applySite = FullApplySite::isa(&inst);
      if (applySite && isRecursiveCall(applySite)) {
        recursiveCallsFound = true;

        // See what parameters the recursive call is forwarding and use that
        // as invariants.
        invariantsToTry.insert(Invariants::fromForwardingArguments(applySite));

        // Limit the size of the set to avoid quadratic complexity in corner
        // cases. Usually 4 invariants are more than enough.
        if (invariantsToTry.size() >= 4)
          return true;
      }
    }
  }
  return recursiveCallsFound;
}

class DiagnoseInfiniteRecursion : public SILFunctionTransform {
public:
  DiagnoseInfiniteRecursion() {}

private:
  void run() override {
    SILFunction *function = getFunction();
    // Don't rerun diagnostics on deserialized functions.
    if (function->wasDeserializedCanonical())
      return;

    // Try with different sets of invariants. To catch all cases we would need
    // to try all parameter/memory permutations.
    // But in practice, it's good enough to collect a reasonable set by finding
    // all recursive calls and see what arguments they are forwarding.
    InvariantsSet invariantsToTry;
    if (!collectInvariantsToTry(function, invariantsToTry)) {
      // There are no recursive calls in the function at all. We don't need to
      // ramp-up the analysis.
      // This is the case for most functions.
      return;
    }

    for (Invariants invariants : invariantsToTry) {
      if (InfiniteRecursionAnalysis::analyzeAndDiagnose(function, invariants))
        return;
      // Try again, assuming that memory is invariant.
      if (InfiniteRecursionAnalysis::analyzeAndDiagnose(
                               function, invariants.withInvariantMemory()))
        return;
    }
  }
};

} // end anonymous namespace

SILTransform *swift::createDiagnoseInfiniteRecursion() {
  return new DiagnoseInfiniteRecursion();
}
