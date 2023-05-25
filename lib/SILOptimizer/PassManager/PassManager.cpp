//===--- PassManager.cpp - Swift Pass Manager -----------------------------===//
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

#define DEBUG_TYPE "sil-passmanager"

#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/SILOptimizerRequests.h"
#include "swift/Basic/BridgingUtils.h"
#include "swift/Demangling/Demangle.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/AliasAnalysis.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/DeadEndBlocksAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/Analysis/DominanceAnalysis.h"
#include "swift/SILOptimizer/OptimizerBridging.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/BasicBlockOptUtils.h"
#include "swift/SILOptimizer/Utils/ConstantFolding.h"
#include "swift/SILOptimizer/Utils/CFGOptUtils.h"
#include "swift/SILOptimizer/Utils/Devirtualize.h"
#include "swift/SILOptimizer/Utils/OptimizerStatsUtils.h"
#include "swift/SILOptimizer/Utils/SILInliner.h"
#include "swift/SILOptimizer/Utils/SILOptFunctionBuilder.h"
#include "swift/SILOptimizer/Utils/StackNesting.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/ManagedStatic.h"

using namespace swift;

llvm::cl::opt<bool> SILPrintAll(
    "sil-print-all", llvm::cl::init(false),
    llvm::cl::desc("Print SIL after each pass"));

llvm::cl::opt<bool> SILPrintPassName(
    "sil-print-pass-name", llvm::cl::init(false),
    llvm::cl::desc("Print the name of each SIL pass before it runs"));

llvm::cl::opt<bool> SILPrintPassTime(
    "sil-print-pass-time", llvm::cl::init(false),
    llvm::cl::desc("Print the execution time of each SIL pass"));

llvm::cl::opt<unsigned> SILMinPassTime(
    "sil-min-pass-time", llvm::cl::init(0),
    llvm::cl::desc("The minimum number of milliseconds for which a pass is printed with -sil-print-pass-time"));

llvm::cl::opt<bool> SILPrintLast(
    "sil-print-last", llvm::cl::init(false),
    llvm::cl::desc("Print the last optimized function before and after the last pass"));

llvm::cl::opt<std::string> SILNumOptPassesToRun(
    "sil-opt-pass-count", llvm::cl::init(""),
    llvm::cl::desc("Stop optimizing after <N> passes or <N>.<M> passes/sub-passes"));

llvm::cl::opt<unsigned> SILOptProfileRepeat(
    "sil-opt-profile-repeat", llvm::cl::init(1),
    llvm::cl::desc("repeat passes N times and report the run time"));

llvm::cl::opt<std::string> SILBreakOnFun(
    "sil-break-on-function", llvm::cl::init(""),
    llvm::cl::desc(
        "Break before running each function pass on a particular function"));

llvm::cl::opt<std::string> SILBreakOnPass(
    "sil-break-on-pass", llvm::cl::init(""),
    llvm::cl::desc("Break before running a particular function pass"));

llvm::cl::list<std::string>
    SILPrintFunction("sil-print-function", llvm::cl::CommaSeparated,
                    llvm::cl::desc("Only print out the sil for this function"));

llvm::cl::opt<std::string>
    SILPrintFunctions("sil-print-functions", llvm::cl::init(""),
                     llvm::cl::desc("Only print out the sil for the functions "
                                    "whose name contains this substring"));

llvm::cl::list<std::string>
    SILPrintBefore("sil-print-before", llvm::cl::CommaSeparated,
                   llvm::cl::desc("Print out the sil before passes which "
                                  "contain a string from this list."));

llvm::cl::list<std::string>
    SILPrintAfter("sil-print-after", llvm::cl::CommaSeparated,
                  llvm::cl::desc("Print out the sil after passes which contain "
                                 "a string from this list."));

llvm::cl::list<std::string>
    SILPrintAround("sil-print-around", llvm::cl::CommaSeparated,
                   llvm::cl::desc("Print out the sil before and after passes "
                                  "which contain a string from this list"));

llvm::cl::list<std::string>
    SILDisablePass("sil-disable-pass", llvm::cl::CommaSeparated,
                     llvm::cl::desc("Disable passes "
                                    "which contain a string from this list"));
llvm::cl::list<std::string> SILDisablePassOnlyFun(
    "sil-disable-pass-only-function", llvm::cl::CommaSeparated,
    llvm::cl::desc("Apply -sil-disable-pass only on this function"));

llvm::cl::list<std::string> SILVerifyBeforePass(
    "sil-verify-before-pass", llvm::cl::CommaSeparated,
    llvm::cl::desc("Verify the module/analyses before we run "
                   "a pass from this list"));

llvm::cl::list<std::string> SILVerifyAroundPass(
    "sil-verify-around-pass", llvm::cl::CommaSeparated,
    llvm::cl::desc("Verify the module/analyses before/after we run "
                   "a pass from this list"));

llvm::cl::list<std::string>
    SILVerifyAfterPass("sil-verify-after-pass", llvm::cl::CommaSeparated,
                       llvm::cl::desc("Verify the module/analyses after we run "
                                      "a pass from this list"));

llvm::cl::list<std::string> SILForceVerifyAroundPass(
    "sil-verify-force-analysis-around-pass", llvm::cl::CommaSeparated,
    llvm::cl::desc("For the given passes, precompute analyses before the pass "
                   "and verify analyses after the pass"));

llvm::cl::opt<bool> SILVerifyWithoutInvalidation(
    "sil-verify-without-invalidation", llvm::cl::init(false),
    llvm::cl::desc("Verify after passes even if the pass has not invalidated"));

llvm::cl::opt<bool> SILDisableSkippingPasses(
    "sil-disable-skipping-passes", llvm::cl::init(false),
    llvm::cl::desc("Do not skip passes even if nothing was changed"));

llvm::cl::opt<bool> SILForceVerifyAll(
    "sil-verify-force-analysis", llvm::cl::init(false),
    llvm::cl::desc("For all passes, precompute analyses before the pass and "
                   "verify analyses after the pass"));

llvm::cl::list<std::string>
    SimplifyInstructionTest("simplify-instruction", llvm::cl::CommaSeparated,
                     llvm::cl::desc("Simplify instruction of specified kind(s)"));

static llvm::ManagedStatic<std::vector<unsigned>> DebugPassNumbers;

namespace {

struct DebugOnlyPassNumberOpt {
  void operator=(const std::string &Val) const {
    if (Val.empty())
      return;
    SmallVector<StringRef, 8> dbgPassNumbers;
    StringRef(Val).split(dbgPassNumbers, ',', -1, false);
    for (auto dbgPassNumber : dbgPassNumbers) {
      int PassNumber;
      if (dbgPassNumber.getAsInteger(10, PassNumber) || PassNumber < 0)
        llvm_unreachable("The pass number should be an integer number >= 0");
      DebugPassNumbers->push_back(static_cast<unsigned>(PassNumber));
    }
  }
};

} // end anonymous namespace

static DebugOnlyPassNumberOpt DebugOnlyPassNumberOptLoc;

static llvm::cl::opt<DebugOnlyPassNumberOpt, true,
                     llvm::cl::parser<std::string>>
    DebugOnly("debug-only-pass-number",
              llvm::cl::desc("Enable a specific type of debug output (comma "
                             "separated list pass numbers)"),
              llvm::cl::Hidden, llvm::cl::ZeroOrMore,
              llvm::cl::value_desc("pass number"),
              llvm::cl::location(DebugOnlyPassNumberOptLoc),
              llvm::cl::ValueRequired);

bool isFunctionSelectedForPrinting(SILFunction *F) {
  if (!SILPrintFunction.empty() && SILPrintFunction.end() ==
      std::find(SILPrintFunction.begin(), SILPrintFunction.end(), F->getName()))
    return false;

  if (!F->getName().contains(SILPrintFunctions))
    return false;

  return true;
}

void printInliningDetails(StringRef passName, SILFunction *caller,
                          SILFunction *callee, bool isCaller,
                          bool alreadyInlined) {
  if (!isFunctionSelectedForPrinting(caller))
    return;
  llvm::dbgs() << "  " << passName
               << (alreadyInlined ? " has inlined " : " will inline ")
               << callee->getName() << " into " << caller->getName() << ".\n";
  auto *printee = isCaller ? caller : callee;
  printee->dump(caller->getModule().getOptions().EmitVerboseSIL);
  llvm::dbgs() << '\n';
}

void printInliningDetailsCallee(StringRef passName, SILFunction *caller,
                                SILFunction *callee) {
  printInliningDetails(passName, caller, callee, /*isCaller=*/false,
                       /*alreadyInlined=*/false);
}

void printInliningDetailsCallerBefore(StringRef passName, SILFunction *caller,
                                      SILFunction *callee) {
  printInliningDetails(passName, caller, callee, /*isCaller=*/true,
                       /*alreadyInlined=*/false);
}

void printInliningDetailsCallerAfter(StringRef passName, SILFunction *caller,
                                     SILFunction *callee) {
  printInliningDetails(passName, caller, callee, /*isCaller=*/true,
                       /*alreadyInlined=*/true);
}

static bool functionSelectionEmpty() {
  return SILPrintFunction.empty() && SILPrintFunctions.empty();
}

bool SILPassManager::doPrintBefore(SILTransform *T, SILFunction *F) {
  if (NumPassesRun == maxNumPassesToRun - 1 && SILPrintLast &&
      maxNumSubpassesToRun == UINT_MAX && !isMandatory)
    return true;

  if (F && !isFunctionSelectedForPrinting(F))
    return false;

  auto MatchFun = [&](const std::string &Str) -> bool {
    return T->getTag().contains(Str) || T->getID().contains(Str);
  };

  if (SILPrintBefore.end() !=
      std::find_if(SILPrintBefore.begin(), SILPrintBefore.end(), MatchFun))
    return true;
  if (!SILPrintBefore.empty())
    return false;

  if (SILPrintAround.end() !=
      std::find_if(SILPrintAround.begin(), SILPrintAround.end(), MatchFun))
    return true;
  if (!SILPrintAround.empty())
    return false;

  return false;
}

bool SILPassManager::doPrintAfter(SILTransform *T, SILFunction *F, bool PassChangedSIL) {
  if (NumPassesRun == maxNumPassesToRun - 1 && SILPrintLast && !isMandatory)
    return true;

  if (F && !isFunctionSelectedForPrinting(F))
    return false;

  auto MatchFun = [&](const std::string &Str) -> bool {
    return T->getTag().contains(Str) || T->getID().contains(Str);
  };

  if (SILPrintAfter.end() !=
      std::find_if(SILPrintAfter.begin(), SILPrintAfter.end(), MatchFun))
    return true;
  if (!SILPrintAfter.empty())
    return false;

  if (SILPrintAround.end() !=
      std::find_if(SILPrintAround.begin(), SILPrintAround.end(), MatchFun))
    return true;
  if (!SILPrintAround.empty())
    return false;

  return PassChangedSIL && (SILPrintAll || !functionSelectionEmpty());
}

static void printModule(SILModule *Mod, bool EmitVerboseSIL) {
  if (functionSelectionEmpty()) {
    Mod->dump();
    return;
  }
  for (auto &F : *Mod) {
    if (isFunctionSelectedForPrinting(&F))
      F.dump(EmitVerboseSIL);
  }
}

class DebugPrintEnabler {
#ifndef NDEBUG
  bool OldDebugFlag;
#endif
public:
  DebugPrintEnabler(unsigned PassNumber) {
#ifndef NDEBUG
    OldDebugFlag = llvm::DebugFlag;
    if (llvm::DebugFlag)
      return;
    if (DebugPassNumbers->empty())
      return;
    // Enable debug printing if the pass number matches
    // one of the pass numbers provided as a command line option.
    for (auto DebugPassNumber : *DebugPassNumbers) {
      if (DebugPassNumber == PassNumber) {
        llvm::DebugFlag = true;
        return;
      }
    }
#endif
  }

  ~DebugPrintEnabler() {
#ifndef NDEBUG
    llvm::DebugFlag = OldDebugFlag;
#endif
  }
};

//===----------------------------------------------------------------------===//
//                 Serialization Notification Implementation
//===----------------------------------------------------------------------===//

namespace {

class PassManagerDeserializationNotificationHandler final
    : public DeserializationNotificationHandler {
  NullablePtr<SILPassManager> pm;

public:
  PassManagerDeserializationNotificationHandler(SILPassManager *pm) : pm(pm) {}
  ~PassManagerDeserializationNotificationHandler() override = default;

  StringRef getName() const override {
    return "PassManagerDeserializationNotificationHandler";
  }

  /// Observe that we deserialized a function declaration.
  void didDeserialize(ModuleDecl *mod, SILFunction *fn) override {
    pm.get()->notifyAnalysisOfFunction(fn);
  }
};

} // end anonymous namespace

evaluator::SideEffect ExecuteSILPipelineRequest::evaluate(
    Evaluator &evaluator, SILPipelineExecutionDescriptor desc) const {
  SILPassManager PM(desc.SM, desc.IsMandatory, desc.IRMod);
  PM.executePassPipelinePlan(desc.Plan);
  return std::make_tuple<>();
}

void swift::executePassPipelinePlan(SILModule *SM,
                                    const SILPassPipelinePlan &plan,
                                    bool isMandatory,
                                    irgen::IRGenModule *IRMod) {
  auto &evaluator = SM->getASTContext().evaluator;
  SILPipelineExecutionDescriptor desc{SM, plan, isMandatory, IRMod};
  (void)llvm::cantFail(evaluator(ExecuteSILPipelineRequest{desc}));
}

SILPassManager::SILPassManager(SILModule *M, bool isMandatory,
                               irgen::IRGenModule *IRMod)
    : Mod(M), IRMod(IRMod),
      swiftPassInvocation(this),
      isMandatory(isMandatory), deserializationNotificationHandler(nullptr) {
#define SIL_ANALYSIS(NAME) \
  Analyses.push_back(create##NAME##Analysis(Mod));
#include "swift/SILOptimizer/Analysis/Analysis.def"

  if (!SILNumOptPassesToRun.empty()) {
    StringRef countsStr = SILNumOptPassesToRun;
    bool validFormat = true;
    if (countsStr.consumeInteger(10, maxNumPassesToRun))
      validFormat = false;
    if (countsStr.startswith(".")) {
      countsStr = countsStr.drop_front(1);
      if (countsStr.consumeInteger(10, maxNumSubpassesToRun))
        validFormat = false;
    }
    if (!validFormat || !countsStr.empty()) {
      llvm::errs() << "error: wrong format of -sil-opt-pass-count option\n";
      exit(1);
    }
  }

  for (SILAnalysis *A : Analyses) {
    A->initialize(this);
  }

  std::unique_ptr<DeserializationNotificationHandler> handler(
      new PassManagerDeserializationNotificationHandler(this));
  deserializationNotificationHandler = handler.get();
  M->registerDeserializationNotificationHandler(std::move(handler));
}

bool SILPassManager::continueTransforming() {
  if (isMandatory)
    return true;
  return NumPassesRun < maxNumPassesToRun;
}

bool SILPassManager::continueWithNextSubpassRun(SILInstruction *forInst,
                                                SILFunction *function,
                                                SILTransform *trans) {
  if (isMandatory)
    return true;
  if (NumPassesRun != maxNumPassesToRun - 1)
    return true;

  unsigned subPass = numSubpassesRun++;
  
  if (subPass == maxNumSubpassesToRun - 1 && SILPrintLast) {
    dumpPassInfo("*** SIL function before ", trans, function);
    if (forInst) {
      llvm::dbgs() << "  *** sub-pass " << subPass << " for " << *forInst;
    }
    function->dump(getOptions().EmitVerboseSIL);
  }
  return subPass < maxNumSubpassesToRun;
}

bool SILPassManager::analysesUnlocked() {
  for (auto *A : Analyses)
    if (A->isLocked())
      return false;

  return true;
}

// Test the function and pass names we're given against the debug
// options that force us to break prior to a given pass and/or on a
// given function.
static bool breakBeforeRunning(StringRef fnName, SILFunctionTransform *SFT) {
  if (SILBreakOnFun.empty() && SILBreakOnPass.empty())
    return false;

  if (SILBreakOnFun.empty()
      && (SFT->getID() == SILBreakOnPass || SFT->getTag() == SILBreakOnPass))
    return true;

  if (SILBreakOnPass.empty() && fnName == SILBreakOnFun)
    return true;

  return fnName == SILBreakOnFun
    && (SFT->getID() == SILBreakOnPass || SFT->getTag() == SILBreakOnPass);
}

void SILPassManager::dumpPassInfo(const char *Title, SILTransform *Tr,
                                  SILFunction *F, int passIdx) {
  llvm::dbgs() << "  " << Title << " #" << NumPassesRun
               << ", stage " << StageName << ", pass";
  if (passIdx >= 0)
    llvm::dbgs() << ' ' << passIdx;
  llvm::dbgs() << ": " << Tr->getID() << " (" << Tr->getTag() << ")";
  if (F)
    llvm::dbgs() << ", Function: " << F->getName();
  llvm::dbgs() << '\n';
}

void SILPassManager::dumpPassInfo(const char *Title, unsigned TransIdx,
                                  SILFunction *F) {
  dumpPassInfo(Title, Transformations[TransIdx], F, (int)TransIdx);
}

bool SILPassManager::isMandatoryFunctionPass(SILFunctionTransform *sft) {
  return isMandatory ||
         sft->getPassKind() ==
             PassKind::NonTransparentFunctionOwnershipModelEliminator ||
         sft->getPassKind() == PassKind::OwnershipModelEliminator;
}

static bool isDisabled(SILTransform *T, SILFunction *F = nullptr) {
  if (SILDisablePass.empty())
    return false;

  if (SILPassManager::isPassDisabled(T->getTag()) ||
      SILPassManager::isPassDisabled(T->getID())) {
    if (F && !SILPassManager::disablePassesForFunction(F))
      return false;
    return true;
  }
  return false;
}

bool SILPassManager::isPassDisabled(StringRef passName) {
  for (const std::string &namePattern : SILDisablePass) {
    if (passName.contains(namePattern))
      return true;
  }
  return false;
}

bool SILPassManager::isInstructionPassDisabled(StringRef instName) {
  StringRef prefix("simplify-");
  for (const std::string &namePattern : SILDisablePass) {
    StringRef pattern(namePattern);
    if (pattern.startswith(prefix) && pattern.endswith(instName) &&
        pattern.size() == prefix.size() + instName.size()) {
      return true;
    }
  }
  return false;
}

bool SILPassManager::disablePassesForFunction(SILFunction *function) {
  if (SILDisablePassOnlyFun.empty())
    return true;

  return std::find(SILDisablePassOnlyFun.begin(), SILDisablePassOnlyFun.end(),
                   function->getName()) != SILDisablePassOnlyFun.end();
}

void SILPassManager::runPassOnFunction(unsigned TransIdx, SILFunction *F) {

  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");

  auto *SFT = cast<SILFunctionTransform>(Transformations[TransIdx]);

  if (!F->shouldOptimize() && !isMandatoryFunctionPass(SFT)) {
    return;
  }

  SFT->injectPassManager(this);
  SFT->injectFunction(F);

  PrettyStackTraceSILFunctionTransform X(SFT, NumPassesRun);
  DebugPrintEnabler DebugPrint(NumPassesRun);

  // If nothing changed since the last run of this pass, we can skip this
  // pass if it is not mandatory
  CompletedPasses &completedPasses = CompletedPassesMap[F];
  if (!isMandatoryFunctionPass(SFT) &&
      completedPasses.test((size_t)SFT->getPassKind()) &&
      !SILDisableSkippingPasses) {
    if (SILPrintPassName)
      dumpPassInfo("(Skip)", TransIdx, F);
    return;
  }

  if (isDisabled(SFT, F)) {
    if (SILPrintPassName)
      dumpPassInfo("(Disabled)", TransIdx, F);
    return;
  }

  updateSILModuleStatsBeforeTransform(F->getModule(), SFT, *this, NumPassesRun);

  CurrentPassHasInvalidated = false;
  currentPassDependsOnCalleeBodies = false;
  numSubpassesRun = 0;

  auto MatchFun = [&](const std::string &Str) -> bool {
    return SFT->getTag().contains(Str) || SFT->getID().contains(Str);
  };
  if ((SILVerifyBeforePass.end() != std::find_if(SILVerifyBeforePass.begin(),
                                                 SILVerifyBeforePass.end(),
                                                 MatchFun)) ||
      (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                 SILVerifyAroundPass.end(),
                                                 MatchFun))) {
    F->verify(this);
    verifyAnalyses();
  }

  if (SILPrintPassName)
    dumpPassInfo("Run", TransIdx, F);

  if (doPrintBefore(SFT, F)) {
    dumpPassInfo("*** SIL function before ", TransIdx);
    F->dump(getOptions().EmitVerboseSIL);
  }

  if (breakBeforeRunning(F->getName(), SFT))
    LLVM_BUILTIN_DEBUGTRAP;
  if (SILForceVerifyAll ||
      SILForceVerifyAroundPass.end() !=
          std::find_if(SILForceVerifyAroundPass.begin(),
                       SILForceVerifyAroundPass.end(), MatchFun)) {
    forcePrecomputeAnalyses(F);
  }
  
  llvm::sys::TimePoint<> startTime = std::chrono::system_clock::now();
  std::chrono::nanoseconds duration(0);

  enum {
    // In future we might want to make snapshots with positive number (e.g.
    // corresponding to pass indices). Therefore use -1 here to avoid collisions.
    SnapshotID = -1
  };

  unsigned numRepeats = SILOptProfileRepeat;
  if (numRepeats > 1) {
    // Need to create a snapshot to restore the original state for consecutive runs.
    F->createSnapshot(SnapshotID);
  }
  for (unsigned runIdx = 0; runIdx < numRepeats; runIdx++) {
    swiftPassInvocation.startFunctionPassRun(SFT);

    // Run it!
    SFT->run();

    swiftPassInvocation.finishedFunctionPassRun();

    if (CurrentPassHasInvalidated) {
      // Pause time measurement while invalidating analysis and restoring the snapshot.
      duration += (std::chrono::system_clock::now() - startTime);

      if (runIdx < numRepeats - 1) {
        invalidateAnalysis(F, SILAnalysis::InvalidationKind::Everything);
        F->restoreFromSnapshot(SnapshotID);
      }
      
      // Continue time measurement (including flushing deleted instructions).
      startTime = std::chrono::system_clock::now();
    }
    Mod->flushDeletedInsts();
  }

  duration += (std::chrono::system_clock::now() - startTime);
  totalPassRuntime += duration;
  if (SILPrintPassTime) {
    double milliSecs = (double)duration.count() / 1000000.;
    if (milliSecs > (double)SILMinPassTime) {
      llvm::dbgs() << llvm::format("%9.3f", milliSecs) << " ms: " << SFT->getTag()
                   << " #" << NumPassesRun << " @" << F->getName() << "\n";
    }
  }

  if (numRepeats > 1)
    F->deleteSnapshot(SnapshotID);

  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");

  if (SILForceVerifyAll ||
      SILForceVerifyAroundPass.end() !=
          std::find_if(SILForceVerifyAroundPass.begin(),
                       SILForceVerifyAroundPass.end(), MatchFun)) {
    verifyAnalyses(F);
  }

  // If this pass invalidated anything, print and verify.
  if (doPrintAfter(SFT, F, CurrentPassHasInvalidated)) {
    dumpPassInfo("*** SIL function after ", TransIdx);
    F->dump(getOptions().EmitVerboseSIL);
  }

  updateSILModuleStatsAfterTransform(F->getModule(), SFT, *this, NumPassesRun,
                                     duration.count());

  // Remember if this pass didn't change anything.
  if (!CurrentPassHasInvalidated && !currentPassDependsOnCalleeBodies)
    completedPasses.set((size_t)SFT->getPassKind());

  if (getOptions().VerifyAll &&
      (CurrentPassHasInvalidated || SILVerifyWithoutInvalidation)) {
    F->verify(this);
    verifyAnalyses(F);
  } else {
    if ((SILVerifyAfterPass.end() != std::find_if(SILVerifyAfterPass.begin(),
                                                  SILVerifyAfterPass.end(),
                                                  MatchFun)) ||
        (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                   SILVerifyAroundPass.end(),
                                                   MatchFun))) {
      F->verify(this);
      verifyAnalyses();
    }
  }

  ++NumPassesRun;
}

void SILPassManager::
runFunctionPasses(unsigned FromTransIdx, unsigned ToTransIdx) {
  if (ToTransIdx <= FromTransIdx)
    return;

  BasicCalleeAnalysis *BCA = getAnalysis<BasicCalleeAnalysis>();
  BottomUpFunctionOrder BottomUpOrder(*Mod, BCA);
  auto BottomUpFunctions = BottomUpOrder.getFunctions();

  assert(FunctionWorklist.empty() && "Expected empty function worklist!");

  FunctionWorklist.reserve(BottomUpFunctions.size());
  for (auto I = BottomUpFunctions.rbegin(), E = BottomUpFunctions.rend();
       I != E; ++I) {
    auto &F = **I;

    // Only include functions that are definitions, and which have not
    // been intentionally excluded from optimization.
    if (F.isDefinition())
      FunctionWorklist.push_back(*I);
  }

  DerivationLevels.clear();

  // The maximum number of times the pass pipeline can be restarted for a
  // function. This is used to ensure we are not going into an infinite loop in
  // cases where (for example) we have recursive type-based specialization
  // happening.
  const unsigned MaxNumRestarts = 20;

  if (SILPrintPassName)
    llvm::dbgs() << "Start function passes at stage: " << StageName << "\n";

  // Run all transforms for all functions, starting at the tail of the worklist.
  while (!FunctionWorklist.empty() && continueTransforming()) {
    unsigned TailIdx = FunctionWorklist.size() - 1;
    unsigned PipelineIdx = FunctionWorklist[TailIdx].PipelineIdx;
    SILFunction *F = FunctionWorklist[TailIdx].F;

    if (PipelineIdx >= (ToTransIdx - FromTransIdx)) {
      // All passes did already run for the function. Pop it off the worklist.
      FunctionWorklist.pop_back();
      continue;
    }
    assert(!shouldRestartPipeline() &&
        "Did not expect function pipeline set up to restart from beginning!");

    runPassOnFunction(FromTransIdx + PipelineIdx, F);

    // Note: Don't get entry reference prior to runPassOnFunction().
    // A pass can push a new function to the worklist which may cause a
    // reallocation of the buffer and that would invalidate the reference.
    WorklistEntry &Entry = FunctionWorklist[TailIdx];
    if (shouldRestartPipeline() && Entry.NumRestarts < MaxNumRestarts) {
      ++Entry.NumRestarts;
      Entry.PipelineIdx = 0;
    } else {
      ++Entry.PipelineIdx;
    }
    clearRestartPipeline();
  }
}

void SILPassManager::runModulePass(unsigned TransIdx) {
  auto *SMT = cast<SILModuleTransform>(Transformations[TransIdx]);
  if (isDisabled(SMT))
    return;

  const SILOptions &Options = getOptions();

  SMT->injectPassManager(this);
  SMT->injectModule(Mod);

  PrettyStackTraceSILModuleTransform X(SMT, NumPassesRun);
  DebugPrintEnabler DebugPrint(NumPassesRun);

  updateSILModuleStatsBeforeTransform(*Mod, SMT, *this, NumPassesRun);

  CurrentPassHasInvalidated = false;
  numSubpassesRun = 0;

  if (SILPrintPassName)
    dumpPassInfo("Run module pass", TransIdx);

  if (doPrintBefore(SMT, nullptr)) {
    dumpPassInfo("*** SIL module before", TransIdx);
    printModule(Mod, Options.EmitVerboseSIL);
  }

  auto MatchFun = [&](const std::string &Str) -> bool {
    return SMT->getTag().contains(Str) || SMT->getID().contains(Str);
  };
  if ((SILVerifyBeforePass.end() != std::find_if(SILVerifyBeforePass.begin(),
                                                 SILVerifyBeforePass.end(),
                                                 MatchFun)) ||
      (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                 SILVerifyAroundPass.end(),
                                                 MatchFun))) {
    Mod->verify(this);
    verifyAnalyses();
  }

  swiftPassInvocation.startModulePassRun(SMT);

  llvm::sys::TimePoint<> StartTime = std::chrono::system_clock::now();
  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");
  SMT->run();
  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");
  Mod->flushDeletedInsts();
  swiftPassInvocation.finishedModulePassRun();

  std::chrono::nanoseconds duration = std::chrono::system_clock::now() - StartTime;
  totalPassRuntime += duration;

  if (SILPrintPassTime) {
    double milliSecs = (double)duration.count() / 1000000.;
    if (milliSecs > (double)SILMinPassTime) {
      llvm::dbgs() << llvm::format("%9.3f", milliSecs) << " ms: " << SMT->getTag()
                   << " #" << NumPassesRun << "\n";
    }
  }

  // If this pass invalidated anything, print and verify.
  if (doPrintAfter(SMT, nullptr, CurrentPassHasInvalidated)) {
    dumpPassInfo("*** SIL module after", TransIdx);
    printModule(Mod, Options.EmitVerboseSIL);
  }

  updateSILModuleStatsAfterTransform(*Mod, SMT, *this, NumPassesRun, duration.count());

  if (Options.VerifyAll &&
      (CurrentPassHasInvalidated || !SILVerifyWithoutInvalidation)) {
    Mod->verify(this);
    verifyAnalyses();
  } else {
    if ((SILVerifyAfterPass.end() != std::find_if(SILVerifyAfterPass.begin(),
                                                  SILVerifyAfterPass.end(),
                                                  MatchFun)) ||
        (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                   SILVerifyAroundPass.end(),
                                                   MatchFun))) {
      Mod->verify(this);
      verifyAnalyses();
    }
  }
}

void SILPassManager::verifyAnalyses() const {
  if (Mod->getOptions().VerifyNone)
    return;

  for (auto *A : Analyses) {
    A->verify();
  }
}

void SILPassManager::verifyAnalyses(SILFunction *F) const {
  if (Mod->getOptions().VerifyNone)
    return;
    
  for (auto *A : Analyses) {
    A->verify(F);
  }
}

void SILPassManager::executePassPipelinePlan(const SILPassPipelinePlan &Plan) {
  for (const SILPassPipeline &Pipeline : Plan.getPipelines()) {
    setStageName(Pipeline.Name);
    resetAndRemoveTransformations();
    for (PassKind Kind : Plan.getPipelinePasses(Pipeline)) {
      addPass(Kind);
      assert(!Pipeline.isFunctionPassPipeline
             || isa<SILFunctionTransform>(Transformations.back()));
    }
    execute();
  }
}

void SILPassManager::execute() {
  const SILOptions &Options = getOptions();

  LLVM_DEBUG(llvm::dbgs() << "*** Optimizing the module (" << StageName
                          << ") *** \n");
  if (SILPrintAll) {
    llvm::dbgs() << "*** SIL module before "  << StageName << " ***\n";
    printModule(Mod, Options.EmitVerboseSIL);
  }

  // Run the transforms by alternating between function transforms and
  // module transforms. We'll queue up all the function transforms
  // that we see in a row and then run the entire group of transforms
  // on each function in turn. Then we move on to running the next set
  // of consecutive module transforms.
  unsigned Idx = 0, NumTransforms = Transformations.size();

  while (Idx < NumTransforms && continueTransforming()) {
    SILTransform *Tr = Transformations[Idx];
    assert((isa<SILFunctionTransform>(Tr) || isa<SILModuleTransform>(Tr)) &&
           "Unexpected pass kind!");
    (void)Tr;

    unsigned FirstFuncTrans = Idx;
    while (Idx < NumTransforms && isa<SILFunctionTransform>(Transformations[Idx]))
      ++Idx;

    runFunctionPasses(FirstFuncTrans, Idx);

    while (Idx < NumTransforms && isa<SILModuleTransform>(Transformations[Idx])
           && continueTransforming()) {
      runModulePass(Idx);

      ++Idx;
      ++NumPassesRun;
    }
  }
}

/// D'tor.
SILPassManager::~SILPassManager() {

  if (SILOptProfileRepeat > 1) {
    double milliSecs = (double)totalPassRuntime.count() / 1000000.;
    llvm::dbgs() << llvm::format("%9.3f", milliSecs) << " ms: total runtime of all passes\n";
  }

  // Before we do anything further, verify the module and our analyses. These
  // are natural points with which to verify.
  //
  // TODO: We currently do not verify the module here since the verifier asserts
  // in the normal build. This should be enabled and those problems resolved
  // either by changing the verifier or treating those asserts as signs of a
  // bug.
  for (auto *A : Analyses) {
    // We use verify full instead of just verify to ensure that passes that want
    // to run more expensive verification after a pass manager is destroyed
    // properly trigger.
    //
    // NOTE: verifyFull() has a default implementation that just calls
    // verify(). So functionally, there is no difference here.
    A->verifyFull();
  }

  // Remove our deserialization notification handler.
  Mod->removeDeserializationNotificationHandler(
      deserializationNotificationHandler);

  // Free all transformations.
  for (auto *T : Transformations)
    delete T;

  // delete the analysis.
  for (auto *A : Analyses) {
    assert(!A->isLocked() &&
           "Deleting a locked analysis. Did we forget to unlock ?");
    delete A;
  }
}

void SILPassManager::notifyOfNewFunction(SILFunction *F, SILTransform *T) {
  if (doPrintAfter(T, F, /*PassChangedSIL*/ true)) {
    dumpPassInfo("*** New SIL function in ", T, F);
    F->dump(getOptions().EmitVerboseSIL);
  }
}

void SILPassManager::addFunctionToWorklist(SILFunction *F,
                                           SILFunction *DerivedFrom) {
  assert(F && F->isDefinition() && (isMandatory || F->shouldOptimize()) &&
         "Expected optimizable function definition!");

  constexpr int MaxDeriveLevels = 10;

  int NewLevel = 1;
  if (DerivedFrom) {
    if (!functionSelectionEmpty() && isFunctionSelectedForPrinting(F)) {
      llvm::dbgs() << F->getName() << " was derived from "
                   << DerivedFrom->getName() << "\n";
    }
    // When SILVerifyAll is enabled, individual functions are verified after
    // function passes are run upon them. This means that any functions created
    // by a function pass will not be verified after the pass runs. Thus
    // specialization errors that cause the verifier to trip will be
    // misattributed to the first pass that makes a change to the specialized
    // function. This is very misleading and increases triage time.
    //
    // As a result, when SILVerifyAll is enabled, we always verify newly
    // specialized functions as they are added to the worklist.
    //
    // TODO: Currently, all specialized functions are added to the function
    // worklist in this manner. This is all well and good, but we should really
    // add support for verifying that all specialized functions are added via
    // this function to the pass manager to ensure that we perform this
    // verification.
    if (getOptions().VerifyAll) {
      F->verify(this);
    }

    NewLevel = DerivationLevels[DerivedFrom] + 1;
    // Limit the number of derivations, i.e. don't allow that a pass specializes
    // a specialized function which is itself a specialized function, and so on.
    if (NewLevel >= MaxDeriveLevels)
      return;
  }
  int &StoredLevel = DerivationLevels[F];

  // Only allow a function to be pushed on the worklist a single time
  // (not counting the initial population of the worklist with the bottom-up
  // function order).
  if (StoredLevel > 0)
    return;

  StoredLevel = NewLevel;
  FunctionWorklist.push_back(F);
}

void SILPassManager::restartWithCurrentFunction(SILTransform *T) {
  assert(isa<SILFunctionTransform>(T) &&
         "Can only restart the pipeline from function passes");
  RestartPipeline = true;
}

/// Reset the state of the pass manager and remove all transformation
/// owned by the pass manager. Analysis passes will be kept.
void SILPassManager::resetAndRemoveTransformations() {
  for (auto *T : Transformations)
    delete T;

  Transformations.clear();
}

void SILPassManager::setStageName(llvm::StringRef NextStage) {
  StageName = NextStage.str();
}

StringRef SILPassManager::getStageName() const {
  return StageName;
}

const SILOptions &SILPassManager::getOptions() const {
  return Mod->getOptions();
}

namespace {
enum class IRGenPasses : uint8_t {
#define PASS(ID, TAG, NAME)
#define IRGEN_PASS(ID, TAG, NAME) ID,
#include "swift/SILOptimizer/PassManager/Passes.def"
};
} // end anonymous namespace

void SILPassManager::addPass(PassKind Kind) {
  assert(unsigned(PassKind::AllPasses_Last) >= unsigned(Kind) &&
         "Invalid pass kind");
  switch (Kind) {
#define PASS(ID, TAG, NAME)                                                    \
  case PassKind::ID: {                                                         \
    SILTransform *T = swift::create##ID();                                     \
    T->setPassKind(PassKind::ID);                                              \
    Transformations.push_back(T);                                              \
    break;                                                                     \
  }
#define IRGEN_PASS(ID, TAG, NAME)                                              \
  case PassKind::ID: {                                                         \
    auto &ctx = Mod->getASTContext();                                          \
    auto irPasses = ctx.getIRGenSILTransforms();                               \
    SILTransform *T = irPasses[static_cast<unsigned>(IRGenPasses::ID)]();      \
    assert(T && "Missing IRGen pass?");                                        \
    T->setPassKind(PassKind::ID);                                              \
    Transformations.push_back(T);                                              \
    break;                                                                     \
  }
#include "swift/SILOptimizer/PassManager/Passes.def"
  case PassKind::invalidPassKind:
    llvm_unreachable("invalid pass kind");
  }
}

void SILPassManager::addPassForName(StringRef Name) {
  PassKind P = llvm::StringSwitch<PassKind>(Name)
#define PASS(ID, TAG, NAME) .Case(#ID, PassKind::ID)
#include "swift/SILOptimizer/PassManager/Passes.def"
  ;
  addPass(P);
}

//===----------------------------------------------------------------------===//
//                          View Call-Graph Implementation
//===----------------------------------------------------------------------===//

#ifndef NDEBUG

namespace {

  /// An explicit graph data structure for the call graph.
  /// Used for viewing the callgraph as dot file with llvm::ViewGraph.
  struct CallGraph {

    struct Node;

    struct Edge {
      FullApplySite FAS;
      Node *Child;
      bool Incomplete;
    };

    struct Node {
      SILFunction *F;
      CallGraph *CG;
      int NumCallSites = 0;
      SmallVector<Edge, 8> Children;
    };

    struct child_iterator {
      using iterator_category = std::random_access_iterator_tag;
      using value_type = Node*;
      using difference_type = std::ptrdiff_t;
      using pointer = value_type*;
      using reference = value_type&;    

      SmallVectorImpl<Edge>::iterator baseIter;

      child_iterator(SmallVectorImpl<Edge>::iterator baseIter) :
      baseIter(baseIter)
      { }

      child_iterator &operator++() { baseIter++; return *this; }
      child_iterator operator++(int) {
        auto tmp = *this;
        ++baseIter;
        return tmp;
      }
      Node *operator*() const { return baseIter->Child; }
      bool operator==(const child_iterator &RHS) const {
        return baseIter == RHS.baseIter;
      }
      bool operator!=(const child_iterator &RHS) const {
        return baseIter != RHS.baseIter;
      }
      difference_type operator-(const child_iterator &RHS) const {
        return baseIter - RHS.baseIter;
      }
    };

    CallGraph(SILModule *M, BasicCalleeAnalysis *BCA);

    std::vector<Node> Nodes;

    /// The SILValue IDs which are printed as edge source labels.
    llvm::DenseMap<const SILNode *, unsigned> InstToIDMap;

    typedef std::vector<Node>::iterator iterator;
  };

  CallGraph::CallGraph(SILModule *M, BasicCalleeAnalysis *BCA) {
    Nodes.resize(M->getFunctionList().size());
    llvm::DenseMap<SILFunction *, Node *> NodeMap;
    int idx = 0;
    for (SILFunction &F : *M) {
      Node &Nd = Nodes[idx++];
      Nd.F = &F;
      Nd.CG = this;
      NodeMap[&F] = &Nd;

      F.numberValues(InstToIDMap);
    }

    for (Node &Nd : Nodes) {
      for (SILBasicBlock &BB : *Nd.F) {
        for (SILInstruction &I : BB) {
          if (FullApplySite FAS = FullApplySite::isa(&I)) {
            auto CList = BCA->getCalleeList(FAS);
            for (SILFunction *Callee : CList) {
              Node *CalleeNode = NodeMap[Callee];
              Nd.Children.push_back({FAS, CalleeNode,CList.isIncomplete()});
            }
          }
        }
      }
    }
  }

} // end anonymous namespace

namespace llvm {

  /// Wraps a dot node label string to multiple lines. The \p NumEdgeLabels
  /// gives an estimate on the minimum width of the node shape.
  static void wrap(std::string &Str, int NumEdgeLabels) {
    unsigned ColNum = 0;
    unsigned LastSpace = 0;
    unsigned MaxColumns = std::max(60, NumEdgeLabels * 8);
    for (unsigned i = 0; i != Str.length(); ++i) {
      if (ColNum == MaxColumns) {
        if (!LastSpace)
          LastSpace = i;
        Str.insert(LastSpace + 1, "\\l");
        ColNum = i - LastSpace - 1;
        LastSpace = 0;
      } else
        ++ColNum;
      if (Str[i] == ' ' || Str[i] == '.')
        LastSpace = i;
    }
  }

  /// CallGraph GraphTraits specialization so the CallGraph can be
  /// iterable by generic graph iterators.
  template <> struct GraphTraits<CallGraph::Node *> {
    typedef CallGraph::child_iterator ChildIteratorType;
    typedef CallGraph::Node *NodeRef;

    static NodeRef getEntryNode(NodeRef N) { return N; }
    static inline ChildIteratorType child_begin(NodeRef N) {
      return N->Children.begin();
    }
    static inline ChildIteratorType child_end(NodeRef N) {
      return N->Children.end();
    }
  };

  template <> struct GraphTraits<CallGraph *>
  : public GraphTraits<CallGraph::Node *> {
    typedef CallGraph *GraphType;
    typedef CallGraph::Node *NodeRef;

    static NodeRef getEntryNode(GraphType F) { return nullptr; }

    typedef pointer_iterator<CallGraph::iterator> nodes_iterator;
    static nodes_iterator nodes_begin(GraphType CG) {
      return nodes_iterator(CG->Nodes.begin());
    }
    static nodes_iterator nodes_end(GraphType CG) {
      return nodes_iterator(CG->Nodes.end());
    }
    static unsigned size(GraphType CG) { return CG->Nodes.size(); }
  };

  /// This is everything the llvm::GraphWriter needs to write the call graph in
  /// a dot file.
  template <>
  struct DOTGraphTraits<CallGraph *> : public DefaultDOTGraphTraits {

    DOTGraphTraits(bool isSimple = false) : DefaultDOTGraphTraits(isSimple) {}

    std::string getNodeLabel(const CallGraph::Node *Node,
                             const CallGraph *Graph) {
      std::string Label = Node->F->getName().str();
      wrap(Label, Node->NumCallSites);
      return Label;
    }

    std::string getNodeDescription(const CallGraph::Node *Node,
                                   const CallGraph *Graph) {
      std::string Label = Demangle::
      demangleSymbolAsString(Node->F->getName());
      wrap(Label, Node->NumCallSites);
      return Label;
    }

    static std::string getEdgeSourceLabel(const CallGraph::Node *Node,
                                          CallGraph::child_iterator I) {
      std::string Label;
      raw_string_ostream O(Label);
      SILInstruction *Inst = I.baseIter->FAS.getInstruction();
      O << '%' << Node->CG->InstToIDMap[Inst->asSILNode()];
      return Label;
    }

    static std::string getEdgeAttributes(const CallGraph::Node *Node,
                                         CallGraph::child_iterator I,
                                         const CallGraph *Graph) {
      CallGraph::Edge *Edge = I.baseIter;
      if (Edge->Incomplete)
        return "color=\"red\"";
      return "";
    }
  };
} // namespace llvm
#endif

void SILPassManager::viewCallGraph() {
  /// When asserts are disabled, this should be a NoOp.
#ifndef NDEBUG
  CallGraph OCG(getModule(), getAnalysis<BasicCalleeAnalysis>());
  llvm::ViewGraph(&OCG, "callgraph");
#endif
}

//===----------------------------------------------------------------------===//
//                           SwiftPassInvocation
//===----------------------------------------------------------------------===//

FixedSizeSlab *SwiftPassInvocation::allocSlab(FixedSizeSlab *afterSlab) {
  FixedSizeSlab *slab = passManager->getModule()->allocSlab();
  if (afterSlab) {
    allocatedSlabs.insert(std::next(afterSlab->getIterator()), *slab);
  } else {
    allocatedSlabs.push_back(*slab);
  }
  return slab;
}

FixedSizeSlab *SwiftPassInvocation::freeSlab(FixedSizeSlab *slab) {
  FixedSizeSlab *prev = nullptr;
  assert(!allocatedSlabs.empty());
  if (&allocatedSlabs.front() != slab)
    prev = &*std::prev(slab->getIterator());

  allocatedSlabs.remove(*slab);
  passManager->getModule()->freeSlab(slab);
  return prev;
}

BasicBlockSet *SwiftPassInvocation::allocBlockSet() {
  assert(numBlockSetsAllocated < BlockSetCapacity - 1 &&
         "too many BasicBlockSets allocated");

  auto *storage = (BasicBlockSet *)blockSetStorage + numBlockSetsAllocated;
  BasicBlockSet *set = new (storage) BasicBlockSet(function);
  aliveBlockSets[numBlockSetsAllocated] = true;
  ++numBlockSetsAllocated;
  return set;
}

void SwiftPassInvocation::freeBlockSet(BasicBlockSet *set) {
  int idx = set - (BasicBlockSet *)blockSetStorage;
  assert(idx >= 0 && idx < numBlockSetsAllocated);
  assert(aliveBlockSets[idx] && "double free of BasicBlockSet");
  aliveBlockSets[idx] = false;

  while (numBlockSetsAllocated > 0 && !aliveBlockSets[numBlockSetsAllocated - 1]) {
    auto *set = (BasicBlockSet *)blockSetStorage + numBlockSetsAllocated - 1;
    set->~BasicBlockSet();
    --numBlockSetsAllocated;
  }
}

NodeSet *SwiftPassInvocation::allocNodeSet() {
  assert(numNodeSetsAllocated < NodeSetCapacity - 1 &&
         "too many BasicNodeSets allocated");

  auto *storage = (NodeSet *)nodeSetStorage + numNodeSetsAllocated;
  NodeSet *set = new (storage) NodeSet(function);
  aliveNodeSets[numNodeSetsAllocated] = true;
  ++numNodeSetsAllocated;
  return set;
}

void SwiftPassInvocation::freeNodeSet(NodeSet *set) {
  int idx = set - (NodeSet *)nodeSetStorage;
  assert(idx >= 0 && idx < numNodeSetsAllocated);
  assert(aliveNodeSets[idx] && "double free of NodeSet");
  aliveNodeSets[idx] = false;

  while (numNodeSetsAllocated > 0 && !aliveNodeSets[numNodeSetsAllocated - 1]) {
    auto *set = (NodeSet *)nodeSetStorage + numNodeSetsAllocated - 1;
    set->~NodeSet();
    --numNodeSetsAllocated;
  }
}

void SwiftPassInvocation::startModulePassRun(SILModuleTransform *transform) {
  assert(!this->function && !this->transform && "a pass is already running");
  this->function = nullptr;
  this->transform = transform;
}

void SwiftPassInvocation::startFunctionPassRun(SILFunctionTransform *transform) {
  assert(!this->transform && "a pass is already running");
  this->transform = transform;
  beginTransformFunction(transform->getFunction());
}

void SwiftPassInvocation::startInstructionPassRun(SILInstruction *inst) {
  assert(inst->getFunction() == function &&
         "running instruction pass on wrong function");
}

void SwiftPassInvocation::finishedModulePassRun() {
  endPassRunChecks();
  assert(!function && transform && "not running a pass");
  assert(changeNotifications == SILAnalysis::InvalidationKind::Nothing
         && "unhandled change notifications at end of module pass");
  transform = nullptr;
}

void SwiftPassInvocation::finishedFunctionPassRun() {
  endPassRunChecks();
  endTransformFunction();
  assert(allocatedSlabs.empty() && "StackList is leaking slabs");
  transform = nullptr;
}

void SwiftPassInvocation::finishedInstructionPassRun() {
  endPassRunChecks();
}

void SwiftPassInvocation::endPassRunChecks() {
  assert(allocatedSlabs.empty() && "StackList is leaking slabs");
  assert(numBlockSetsAllocated == 0 && "Not all BasicBlockSets deallocated");
  assert(numNodeSetsAllocated == 0 && "Not all NodeSets deallocated");
  assert(numClonersAllocated == 0 && "Not all cloners deallocated");
  assert(!needFixStackNesting && "Stack nesting not fixed");
}

void SwiftPassInvocation::beginTransformFunction(SILFunction *function) {
  assert(!this->function && transform && "not running a pass");
  assert(changeNotifications == SILAnalysis::InvalidationKind::Nothing
         && "change notifications not cleared");
  this->function = function;
}

void SwiftPassInvocation::endTransformFunction() {
  assert(function && transform && "not running a pass");
  if (changeNotifications != SILAnalysis::InvalidationKind::Nothing) {
    passManager->invalidateAnalysis(function, changeNotifications);
    changeNotifications = SILAnalysis::InvalidationKind::Nothing;
  }
  function = nullptr;
  assert(numBlockSetsAllocated == 0 && "Not all BasicBlockSets deallocated");
  assert(numNodeSetsAllocated == 0 && "Not all NodeSets deallocated");
}

//===----------------------------------------------------------------------===//
//                            Swift Bridging
//===----------------------------------------------------------------------===//

void BridgedChangeNotificationHandler::notifyChanges(Kind changeKind) const {
  switch (changeKind) {
  case Kind::instructionsChanged:
    invocation->notifyChanges(SILAnalysis::InvalidationKind::Instructions);
    break;
  case Kind::callsChanged:
    invocation->notifyChanges(SILAnalysis::InvalidationKind::CallsAndInstructions);
    break;
  case Kind::branchesChanged:
    invocation->notifyChanges(SILAnalysis::InvalidationKind::BranchesAndInstructions);
    break;
  case Kind::effectsChanged:
    invocation->notifyChanges(SILAnalysis::InvalidationKind::Effects);
    break;
  }
}

std::string BridgedPassContext::getModuleDescription() const {
  std::string str;
  llvm::raw_string_ostream os(str);
  invocation->getPassManager()->getModule()->print(os);
  str.pop_back(); // Remove trailing newline.
  return str;
}

bool BridgedPassContext::tryOptimizeApplyOfPartialApply(BridgedInstruction closure) const {
  auto *pa = closure.getAs<PartialApplyInst>();
  SILBuilder builder(pa);
  return ::tryOptimizeApplyOfPartialApply(pa, builder.getBuilderContext(), InstModCallbacks());
}

bool BridgedPassContext::tryDeleteDeadClosure(BridgedInstruction closure, bool needKeepArgsAlive) const {
  return ::tryDeleteDeadClosure(closure.getAs<SingleValueInstruction>(), InstModCallbacks(), needKeepArgsAlive);
}

BridgedPassContext::DevirtResult BridgedPassContext::tryDevirtualizeApply(BridgedInstruction apply,
                                                                          bool isMandatory) const {
  auto cha = invocation->getPassManager()->getAnalysis<ClassHierarchyAnalysis>();
  auto result = ::tryDevirtualizeApply(ApplySite(apply.getInst()), cha, nullptr, isMandatory);
  if (result.first) {
    OptionalBridgedInstruction newApply(result.first.getInstruction()->asSILNode());
    return {newApply, result.second};
  }
  return {{nullptr}, false};
}

OptionalBridgedValue BridgedPassContext::constantFoldBuiltin(BridgedInstruction builtin) const {
  auto bi = builtin.getAs<BuiltinInst>();
  Optional<bool> resultsInError;
  return {::constantFoldBuiltin(bi, resultsInError)};
}

void BridgedPassContext::inlineFunction(BridgedInstruction apply, bool mandatoryInline) const {
  SILOptFunctionBuilder funcBuilder(*invocation->getTransform());
  InstructionDeleter deleter;
  SILInliner::inlineFullApply(FullApplySite(apply.getInst()),
                              mandatoryInline ? SILInliner::InlineKind::MandatoryInline
                                              : SILInliner::InlineKind::PerformanceInline,
                              funcBuilder,
                              deleter);
}

bool BridgedPassContext::specializeAppliesInFunction(BridgedFunction function, bool isMandatory) const {
  return ::specializeAppliesInFunction(*function.getFunction(), invocation->getTransform(), isMandatory);
}

namespace  {
class GlobalVariableMangler : public Mangle::ASTMangler {
public:
  std::string mangleOutlinedVariable(SILFunction *F, int &uniqueIdx) {
    std::string GlobName;
    do {
      beginManglingWithoutPrefix();
      appendOperator(F->getName());
      appendOperator("Tv", Index(uniqueIdx++));
      GlobName = finalize();
    } while (F->getModule().lookUpGlobalVariable(GlobName));

    return GlobName;
  }
};
} // namespace

std::string BridgedPassContext::mangleOutlinedVariable(BridgedFunction function) const {
  int idx = 0;
  SILFunction *f = function.getFunction();
  SILModule &mod = f->getModule();
  while (true) {
    GlobalVariableMangler mangler;
    std::string name = mangler.mangleOutlinedVariable(f, idx);
    if (!mod.lookUpGlobalVariable(name))
      return name;
    idx++;
  }
}

BridgedGlobalVar BridgedPassContext::createGlobalVariable(StringRef name, SILType type, bool isPrivate) const {
  return {SILGlobalVariable::create(*invocation->getPassManager()->getModule(),
                                    isPrivate ? SILLinkage::Private : SILLinkage::Public,
                                    IsNotSerialized,
                                    name, type)};
}

void BridgedPassContext::fixStackNesting(BridgedFunction function) const {
  switch (StackNesting::fixNesting(function.getFunction())) {
    case StackNesting::Changes::None:
      break;
    case StackNesting::Changes::Instructions:
      invocation->notifyChanges(SILAnalysis::InvalidationKind::Instructions);
      break;
    case StackNesting::Changes::CFG:
      invocation->notifyChanges(SILAnalysis::InvalidationKind::BranchesAndInstructions);
      break;
  }
  invocation->setNeedFixStackNesting(false);
}

OptionalBridgedFunction BridgedPassContext::lookupStdlibFunction(llvm::StringRef name) const {
  swift::SILModule *mod = invocation->getPassManager()->getModule();
  SmallVector<ValueDecl *, 1> results;
  mod->getASTContext().lookupInSwiftModule(name, results);
  if (results.size() != 1)
    return {nullptr};

  auto *decl = dyn_cast<FuncDecl>(results.front());
  if (!decl)
    return {nullptr};

  SILDeclRef declRef(decl, SILDeclRef::Kind::Func);
  SILOptFunctionBuilder funcBuilder(*invocation->getTransform());
  return {funcBuilder.getOrCreateFunction(SILLocation(decl), declRef, NotForDefinition)};
}

bool BridgedPassContext::enableSimplificationFor(BridgedInstruction inst) const {
  // Fast-path check.
  if (SimplifyInstructionTest.empty() && SILDisablePass.empty())
    return true;

  StringRef instName = getSILInstructionName(inst.getInst()->getKind());

  if (SILPassManager::isInstructionPassDisabled(instName))
    return false;

  if (SimplifyInstructionTest.empty())
    return true;

  for (const std::string &testName : SimplifyInstructionTest) {
    if (testName == instName)
      return true;
  }
  return false;
}

// TODO: can't be inlined to work around https://github.com/apple/swift/issues/64502
CalleeList BridgedCalleeAnalysis::getCallees(BridgedValue callee) const {
  return ca->getCalleeListOfValue(callee.getSILValue());
}

// TODO: can't be inlined to work around https://github.com/apple/swift/issues/64502
CalleeList BridgedCalleeAnalysis::getDestructors(SILType type, bool isExactType) const {
  return ca->getDestructors(type, isExactType);
}

// Need to put ClonerWithFixedLocation into namespace swift to forward reference
// it in OptimizerBridging.h.
namespace swift {

class ClonerWithFixedLocation : public SILCloner<ClonerWithFixedLocation> {
  friend class SILInstructionVisitor<ClonerWithFixedLocation>;
  friend class SILCloner<ClonerWithFixedLocation>;

  SILDebugLocation insertLoc;

public:
  ClonerWithFixedLocation(SILGlobalVariable *gVar)
  : SILCloner<ClonerWithFixedLocation>(gVar),
  insertLoc(ArtificialUnreachableLocation(), nullptr) {}

  ClonerWithFixedLocation(SILInstruction *insertionPoint)
  : SILCloner<ClonerWithFixedLocation>(*insertionPoint->getFunction()),
  insertLoc(insertionPoint->getDebugLocation()) {
    Builder.setInsertionPoint(insertionPoint);
  }

  SILValue getClonedValue(SILValue v) {
    return getMappedValue(v);
  }

  void cloneInst(SILInstruction *inst) {
    visit(inst);
  }

protected:

  SILLocation remapLocation(SILLocation loc) {
    return insertLoc.getLocation();
  }

  const SILDebugScope *remapScope(const SILDebugScope *DS) {
    return insertLoc.getScope();
  }
};

} // namespace swift

BridgedCloner::BridgedCloner(BridgedGlobalVar var, BridgedPassContext context)
  : cloner(new ClonerWithFixedLocation(var.getGlobal())) {
  context.invocation->notifyNewCloner();
}

BridgedCloner::BridgedCloner(BridgedInstruction inst, BridgedPassContext context)
  : cloner(new ClonerWithFixedLocation(inst.getInst())) {
  context.invocation->notifyNewCloner();
}

void BridgedCloner::destroy(BridgedPassContext context) {
  delete cloner;
  cloner = nullptr;
  context.invocation->notifyClonerDestroyed();
}

BridgedValue BridgedCloner::getClonedValue(BridgedValue v) {
  return {cloner->getClonedValue(v.getSILValue())};
}

bool BridgedCloner::isValueCloned(BridgedValue v) const {
  return cloner->isValueCloned(v.getSILValue());
}

void BridgedCloner::clone(BridgedInstruction inst) {
  cloner->cloneInst(inst.getInst());
}
