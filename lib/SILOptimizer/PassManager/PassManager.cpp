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
#include "swift/AST/SILOptimizerRequests.h"
#include "swift/Demangling/Demangle.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/SILBridgingUtils.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/BasicCalleeAnalysis.h"
#include "swift/SILOptimizer/Analysis/FunctionOrder.h"
#include "swift/SILOptimizer/PassManager/PrettyStackTrace.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/OptimizerStatsUtils.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/Chrono.h"
extern "C" {
#include "swift/SILOptimizer/OptimizerBridging.h"
}

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

llvm::cl::opt<unsigned> SILNumOptPassesToRun(
    "sil-opt-pass-count", llvm::cl::init(UINT_MAX),
    llvm::cl::desc("Stop optimizing after <N> optimization passes"));

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

static bool isFunctionSelectedForPrinting(SILFunction *F) {
  if (!SILPrintFunction.empty() && SILPrintFunction.end() ==
      std::find(SILPrintFunction.begin(), SILPrintFunction.end(), F->getName()))
    return false;

  if (!F->getName().contains(SILPrintFunctions))
    return false;

  return true;
}

static bool functionSelectionEmpty() {
  return SILPrintFunction.empty() && SILPrintFunctions.empty();
}

static bool doPrintBefore(SILTransform *T, SILFunction *F) {
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

static bool doPrintAfter(SILTransform *T, SILFunction *F, bool PassChangedSIL) {
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

static bool isDisabled(SILTransform *T, SILFunction *F = nullptr) {
  if (!SILDisablePassOnlyFun.empty() && F &&
      SILDisablePassOnlyFun.end() == std::find(SILDisablePassOnlyFun.begin(),
                                               SILDisablePassOnlyFun.end(),
                                               F->getName()))
    return false;

  for (const std::string &NamePattern : SILDisablePass) {
    if (T->getTag().contains(NamePattern) || T->getID().contains(NamePattern)) {
      return true;
    }
  }
  return false;
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
      libswiftPassInvocation(this, /*SILCombiner*/ nullptr),
      isMandatory(isMandatory), deserializationNotificationHandler(nullptr) {
#define ANALYSIS(NAME) \
  Analyses.push_back(create##NAME##Analysis(Mod));
#include "swift/SILOptimizer/Analysis/Analysis.def"

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
  return NumPassesRun < SILNumOptPassesToRun;
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
                                  SILFunction *F) {
  llvm::dbgs() << "  " << Title << " #" << NumPassesRun << ", stage "
               << StageName << ", pass : " << Tr->getID()
               << " (" << Tr->getTag() << ")";
  if (F)
    llvm::dbgs() << ", Function: " << F->getName();
  llvm::dbgs() << '\n';
}

void SILPassManager::dumpPassInfo(const char *Title, unsigned TransIdx,
                                  SILFunction *F) {
  SILTransform *Tr = Transformations[TransIdx];
  llvm::dbgs() << "  " << Title << " #" << NumPassesRun << ", stage "
    << StageName << ", pass " << TransIdx << ": " << Tr->getID()
    << " (" << Tr->getTag() << ")";
  if (F)
    llvm::dbgs() << ", Function: " << F->getName();
  llvm::dbgs() << '\n';
}

bool SILPassManager::isMandatoryFunctionPass(SILFunctionTransform *sft) {
  return isMandatory || sft->getPassKind() ==
             PassKind::NonTransparentFunctionOwnershipModelEliminator ||
         sft->getPassKind() == PassKind::OwnershipModelEliminator ||
         sft->getPassKind() ==
             PassKind::NonStdlibNonTransparentFunctionOwnershipModelEliminator;
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

  auto MatchFun = [&](const std::string &Str) -> bool {
    return SFT->getTag().contains(Str) || SFT->getID().contains(Str);
  };
  if ((SILVerifyBeforePass.end() != std::find_if(SILVerifyBeforePass.begin(),
                                                 SILVerifyBeforePass.end(),
                                                 MatchFun)) ||
      (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                 SILVerifyAroundPass.end(),
                                                 MatchFun))) {
    F->verify();
    verifyAnalyses();
  }

  if (SILPrintPassName)
    dumpPassInfo("Run", TransIdx, F);

  if (doPrintBefore(SFT, F)) {
    dumpPassInfo("*** SIL function before ", TransIdx);
    F->dump(getOptions().EmitVerboseSIL);
  }

  llvm::sys::TimePoint<> StartTime = std::chrono::system_clock::now();
  if (breakBeforeRunning(F->getName(), SFT))
    LLVM_BUILTIN_DEBUGTRAP;
  if (SILForceVerifyAll ||
      SILForceVerifyAroundPass.end() !=
          std::find_if(SILForceVerifyAroundPass.begin(),
                       SILForceVerifyAroundPass.end(), MatchFun)) {
    forcePrecomputeAnalyses(F);
  }
  
  assert(changeNotifications == SILAnalysis::InvalidationKind::Nothing
         && "change notifications not cleared");
  
  // Run it!
  SFT->run();

  if (changeNotifications != SILAnalysis::InvalidationKind::Nothing) {
    invalidateAnalysis(F, changeNotifications);
    changeNotifications = SILAnalysis::InvalidationKind::Nothing;
  }
  libswiftPassInvocation.finishedPassRun();

  if (SILForceVerifyAll ||
      SILForceVerifyAroundPass.end() !=
          std::find_if(SILForceVerifyAroundPass.begin(),
                       SILForceVerifyAroundPass.end(), MatchFun)) {
    verifyAnalyses(F);
  }
  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");
  Mod->flushDeletedInsts();

  auto Delta = (std::chrono::system_clock::now() - StartTime).count();
  if (SILPrintPassTime) {
    llvm::dbgs() << Delta << " (" << SFT->getID() << "," << F->getName()
                 << ")\n";
  }

  // If this pass invalidated anything, print and verify.
  if (doPrintAfter(SFT, F, CurrentPassHasInvalidated)) {
    dumpPassInfo("*** SIL function after ", TransIdx);
    F->dump(getOptions().EmitVerboseSIL);
  }

  updateSILModuleStatsAfterTransform(F->getModule(), SFT, *this, NumPassesRun,
                                     Delta);

  // Remember if this pass didn't change anything.
  if (!CurrentPassHasInvalidated)
    completedPasses.set((size_t)SFT->getPassKind());

  if (getOptions().VerifyAll &&
      (CurrentPassHasInvalidated || SILVerifyWithoutInvalidation)) {
    F->verify();
    verifyAnalyses(F);
  } else {
    if ((SILVerifyAfterPass.end() != std::find_if(SILVerifyAfterPass.begin(),
                                                  SILVerifyAfterPass.end(),
                                                  MatchFun)) ||
        (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                   SILVerifyAroundPass.end(),
                                                   MatchFun))) {
      F->verify();
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
    Mod->verify();
    verifyAnalyses();
  }

  llvm::sys::TimePoint<> StartTime = std::chrono::system_clock::now();
  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");
  SMT->run();
  assert(analysesUnlocked() && "Expected all analyses to be unlocked!");
  Mod->flushDeletedInsts();

  auto Delta = (std::chrono::system_clock::now() - StartTime).count();
  if (SILPrintPassTime) {
    llvm::dbgs() << Delta << " (" << SMT->getID() << ",Module)\n";
  }

  // If this pass invalidated anything, print and verify.
  if (doPrintAfter(SMT, nullptr, CurrentPassHasInvalidated)) {
    dumpPassInfo("*** SIL module after", TransIdx);
    printModule(Mod, Options.EmitVerboseSIL);
  }

  updateSILModuleStatsAfterTransform(*Mod, SMT, *this, NumPassesRun, Delta);

  if (Options.VerifyAll &&
      (CurrentPassHasInvalidated || !SILVerifyWithoutInvalidation)) {
    Mod->verify();
    verifyAnalyses();
  } else {
    if ((SILVerifyAfterPass.end() != std::find_if(SILVerifyAfterPass.begin(),
                                                  SILVerifyAfterPass.end(),
                                                  MatchFun)) ||
        (SILVerifyAroundPass.end() != std::find_if(SILVerifyAroundPass.begin(),
                                                   SILVerifyAroundPass.end(),
                                                   MatchFun))) {
      Mod->verify();
      verifyAnalyses();
    }
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
      F->verify();
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

    struct child_iterator
        : public std::iterator<std::random_access_iterator_tag, Node *,
                               ptrdiff_t> {
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
//                           LibswiftPassInvocation
//===----------------------------------------------------------------------===//

static_assert(BridgedSlabCapacity == FixedSizeSlab::capacity,
              "wrong bridged slab capacity");

FixedSizeSlab *LibswiftPassInvocation::allocSlab(FixedSizeSlab *afterSlab) {
  FixedSizeSlab *slab = passManager->getModule()->allocSlab();
  if (afterSlab) {
    allocatedSlabs.insert(std::next(afterSlab->getIterator()), *slab);
  } else {
    allocatedSlabs.push_back(*slab);
  }
  return slab;
}

FixedSizeSlab *LibswiftPassInvocation::freeSlab(FixedSizeSlab *slab) {
  FixedSizeSlab *prev = std::prev(&*slab->getIterator());
  allocatedSlabs.remove(*slab);
  passManager->getModule()->freeSlab(slab);
  return prev;
}

void LibswiftPassInvocation::finishedPassRun() {
  assert(allocatedSlabs.empty() && "StackList is leaking slabs");
}

//===----------------------------------------------------------------------===//
//                            Swift Bridging
//===----------------------------------------------------------------------===//

inline LibswiftPassInvocation *castToPassInvocation(BridgedPassContext ctxt) {
  return const_cast<LibswiftPassInvocation *>(
    static_cast<const LibswiftPassInvocation *>(ctxt.opaqueCtxt));
}

inline FixedSizeSlab *castToSlab(BridgedSlab slab) {
  if (slab.data)
    return static_cast<FixedSizeSlab *>((FixedSizeSlabPayload *)slab.data);
  return nullptr;
}

inline BridgedSlab toBridgedSlab(FixedSizeSlab *slab) {
  if (slab) {
    FixedSizeSlabPayload *payload = slab;
    assert((void *)payload == slab->dataFor<void>());
    return {payload};
  }
  return {nullptr};
}


BridgedSlab PassContext_getNextSlab(BridgedSlab slab) {
  return toBridgedSlab(&*std::next(castToSlab(slab)->getIterator()));
}

BridgedSlab PassContext_allocSlab(BridgedPassContext passContext,
                                  BridgedSlab afterSlab) {
  auto *inv = castToPassInvocation(passContext);
  return toBridgedSlab(inv->allocSlab(castToSlab(afterSlab)));
}

BridgedSlab PassContext_freeSlab(BridgedPassContext passContext,
                                 BridgedSlab slab) {
  auto *inv = castToPassInvocation(passContext);
  return toBridgedSlab(inv->freeSlab(castToSlab(slab)));
}

void PassContext_notifyChanges(BridgedPassContext passContext,
                               enum ChangeNotificationKind changeKind) {
  LibswiftPassInvocation *inv = castToPassInvocation(passContext);
  switch (changeKind) {
  case instructionsChanged:
    inv->notifyChanges(SILAnalysis::InvalidationKind::Instructions);
    break;
  case callsChanged:
    inv->notifyChanges(SILAnalysis::InvalidationKind::CallsAndInstructions);
    break;
  case branchesChanged:
    inv->notifyChanges(SILAnalysis::InvalidationKind::BranchesAndInstructions);
    break;
  }
}

void PassContext_eraseInstruction(BridgedPassContext passContext,
                                   BridgedInstruction inst) {
  castToPassInvocation(passContext)->eraseInstruction(castToInst(inst));
}

