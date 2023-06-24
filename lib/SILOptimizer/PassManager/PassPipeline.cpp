//===--- PassPipeline.cpp - Swift Compiler SIL Pass Entrypoints -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
///  \file
///  This file provides implementations of a few helper functions
///  which provide abstracted entrypoints to the SILPasses stage.
///
///  \note The actual SIL passes should be implemented in per-pass source files,
///  not in this file.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-passpipeline-plan"

#include "swift/SILOptimizer/PassManager/PassPipeline.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/SILOptions.h"
#include "swift/SIL/SILModule.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"

using namespace swift;

static llvm::cl::opt<bool>
    SILViewCFG("sil-view-cfg", llvm::cl::init(false),
               llvm::cl::desc("Enable the sil cfg viewer pass"));

static llvm::cl::opt<bool> SILViewCanonicalCFG(
    "sil-view-canonical-cfg", llvm::cl::init(false),
    llvm::cl::desc("Enable the sil cfg viewer pass after diagnostics"));

static llvm::cl::opt<bool> SILPrintCanonicalModule(
    "sil-print-canonical-module", llvm::cl::init(false),
    llvm::cl::desc("Print the textual SIL module after diagnostics"));

static llvm::cl::opt<bool> SILPrintFinalOSSAModule(
    "sil-print-final-ossa-module", llvm::cl::init(false),
    llvm::cl::desc("Print the textual SIL module before lowering from OSSA"));

static llvm::cl::opt<bool> SILViewSILGenCFG(
    "sil-view-silgen-cfg", llvm::cl::init(false),
    llvm::cl::desc("Enable the sil cfg viewer pass before diagnostics"));


llvm::cl::opt<bool> SILDisableLateOMEByDefault(
    "sil-disable-late-ome-by-default", llvm::cl::init(false),
    llvm::cl::desc(
        "Disable late OME for non-transparent functions by default"));

llvm::cl::opt<bool>
    EnableDestroyHoisting("enable-destroy-hoisting", llvm::cl::init(false),
                          llvm::cl::desc("Enable the DestroyHoisting pass."));

//===----------------------------------------------------------------------===//
//                          Diagnostic Pass Pipeline
//===----------------------------------------------------------------------===//

static void addCFGPrinterPipeline(SILPassPipelinePlan &P, StringRef Name) {
  P.startPipeline(Name);
  P.addCFGPrinter();
}

static void addModulePrinterPipeline(SILPassPipelinePlan &plan,
                                     StringRef name) {
  plan.startPipeline(name);
  plan.addModulePrinter();
}

static void addMandatoryDebugSerialization(SILPassPipelinePlan &P) {
  P.startPipeline("Mandatory Debug Serialization");
  P.addAddressLowering();
  P.addOwnershipModelEliminator();
  P.addMandatoryInlining();
}

static void addOwnershipModelEliminatorPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("Ownership Model Eliminator");
  P.addAddressLowering();
  P.addOwnershipModelEliminator();
}

/// Passes for performing definite initialization. Must be run together in this
/// order.
static void addDefiniteInitialization(SILPassPipelinePlan &P) {
  P.addDefiniteInitialization();
  P.addRawSILInstLowering();
}

// This pipeline defines a set of mandatory diagnostic passes and a set of
// supporting optimization passes that enable those diagnostics. These are run
// before any performance optimizations and in contrast to those optimizations
// _IS_ run when SourceKit emits diagnostics.
//
// Any passes not needed for diagnostic emission that need to run at -Onone
// should be in the -Onone pass pipeline and the prepare optimizations pipeline.
static void addMandatoryDiagnosticOptPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("Mandatory Diagnostic Passes + Enabling Optimization Passes");
  P.addDiagnoseInvalidEscapingCaptures();
  P.addReferenceBindingTransform();
  P.addDiagnoseStaticExclusivity();
  P.addNestedSemanticFunctionCheck();
  P.addCapturePromotion();

  // Select access kind after capture promotion and before stack promotion.
  // This guarantees that stack-promotable boxes have [static] enforcement.
  P.addAccessEnforcementSelection();

  P.addAllocBoxToStack();
  P.addNoReturnFolding();
  addDefiniteInitialization(P);

  P.addAddressLowering();

  P.addFlowIsolation();

  // Automatic differentiation: canonicalize all differentiability witnesses
  // and `differentiable_function` instructions.
  P.addDifferentiation();

  // Only run semantic arc opts if we are optimizing and if mandatory semantic
  // arc opts is explicitly enabled.
  //
  // NOTE: Eventually this pass will be split into a mandatory/more aggressive
  // pass. This will happen when OSSA is no longer eliminated before the
  // optimizer pipeline is run implying we can put a pass that requires OSSA
  // there.
  const auto &Options = P.getOptions();
  P.addClosureLifetimeFixup();

  //===---
  // Begin Ownership Optimizations
  //
  // These happen after ClosureLifetimeFixup because they depend on the
  // resolution of nonescaping closure lifetimes to correctly check the use
  // of move-only values as captures in nonescaping closures as borrows.

  // Check noImplicitCopy and move only types for objects and addresses.
  P.addMoveOnlyChecker();
  // Lower move only wrapped trivial types.
  P.addTrivialMoveOnlyTypeEliminator();
  // Check no uses after consume operator of a value in an address.
  P.addConsumeOperatorCopyableAddressesChecker();
  // No uses after consume operator of copyable value.
  P.addConsumeOperatorCopyableValuesChecker();

  //
  // End Ownership Optimizations
  //===---

#ifndef NDEBUG
  // Add a verification pass to check our work when skipping
  // function bodies.
  if (Options.SkipFunctionBodies != FunctionBodySkipping::None)
    P.addSILSkippingChecker();
#endif

  if (Options.shouldOptimize()) {
    if (EnableDestroyHoisting) {
      P.addDestroyHoisting();
    } else if (P.getOptions().DestroyHoisting == DestroyHoistingOption::On) {
      P.addDestroyAddrHoisting();
    }
  }
  P.addMandatoryInlining();
  P.addMandatorySILLinker();

  // Promote loads as necessary to ensure we have enough SSA formation to emit
  // SSA based diagnostics.
  P.addPredictableMemoryAccessOptimizations();

  // This phase performs optimizations necessary for correct interoperation of
  // Swift os log APIs with C os_log ABIs.
  // Pass dependencies: this pass depends on MandatoryInlining and Mandatory
  // Linking happening before this pass and ConstantPropagation happening after
  // this pass.
  P.addOSLogOptimization();

  // Diagnostic ConstantPropagation must be rerun on deserialized functions
  // because it is sensitive to the assert configuration.
  // Consequently, certain optimization passes beyond this point will also rerun.
  P.addDiagnosticConstantPropagation();

  // Now that we have emitted constant propagation diagnostics, try to eliminate
  // dead allocations.
  P.addPredictableDeadAllocationElimination();

  // Now that we have finished performing diagnostics that rely on lexical
  // scopes, if lexical lifetimes are not enabled, eliminate lexical lifetimes.
  if (Options.LexicalLifetimes != LexicalLifetimesOption::On) {
    P.addLexicalLifetimeEliminator();
  }

  P.addOptimizeHopToExecutor();

  // These diagnostic passes must run before OnoneSimplification because
  // they rely on completely unoptimized SIL.
  P.addDiagnoseUnreachable();
  P.addDiagnoseInfiniteRecursion();
  P.addYieldOnceCheck();
  P.addEmitDFDiagnostics();

  // Only issue weak lifetime warnings for users who select object lifetime
  // optimization. The risk of spurious warnings outweighs the benefits.
  if (P.getOptions().CopyPropagation == CopyPropagationOption::On) {
    P.addDiagnoseLifetimeIssues();
  }

  // Canonical swift requires all non cond_br critical edges to be split.
  P.addSplitNonCondBrCriticalEdges();

  // As a temporary measure, we also eliminate move only for non-trivial types
  // until we can audit the later part of the pipeline. Eventually, this should
  // occur before IRGen.
  P.addMoveOnlyTypeEliminator();

  P.addMandatoryPerformanceOptimizations();
  P.addOnoneSimplification();
  P.addInitializeStaticGlobals();
  P.addPerformanceDiagnostics();
}

SILPassPipelinePlan
SILPassPipelinePlan::getSILGenPassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("SILGen Passes");

  P.addSILGenCleanup();

  if (SILViewSILGenCFG) {
    addCFGPrinterPipeline(P, "SIL View SILGen CFG");
  }
  return P;
}

SILPassPipelinePlan
SILPassPipelinePlan::getDiagnosticPassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);

  // If we are asked do debug serialization, instead of running all diagnostic
  // passes, just run mandatory inlining with dead transparent function cleanup
  // disabled.
  if (Options.DebugSerialization) {
    addMandatoryDebugSerialization(P);
    return P;
  }

  // Otherwise run the rest of diagnostics.
  addMandatoryDiagnosticOptPipeline(P);

  if (SILViewCanonicalCFG) {
    addCFGPrinterPipeline(P, "SIL View Canonical CFG");
  }
  if (SILPrintCanonicalModule) {
    addModulePrinterPipeline(P, "SIL Print Canonical Module");
  }
  return P;
}

SILPassPipelinePlan SILPassPipelinePlan::getLowerHopToActorPassPipeline(
    const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("Lower Hop to Actor");
  P.addLowerHopToActor();
  return P;
}

//===----------------------------------------------------------------------===//
//                       Ownership Eliminator Pipeline
//===----------------------------------------------------------------------===//

SILPassPipelinePlan SILPassPipelinePlan::getOwnershipEliminatorPassPipeline(
    const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  addOwnershipModelEliminatorPipeline(P);
  return P;
}

//===----------------------------------------------------------------------===//
//                         Performance Pass Pipeline
//===----------------------------------------------------------------------===//

namespace {

// Enumerates the optimization kinds that we do in SIL.
enum OptimizationLevelKind {
  LowLevel,
  MidLevel,
  HighLevel,
};

} // end anonymous namespace

void addSimplifyCFGSILCombinePasses(SILPassPipelinePlan &P) {
  P.addSimplifyCFG();
  P.addConditionForwarding();
  // Jump threading can expose opportunity for silcombine (enum -> is_enum_tag->
  // cond_br).
  P.addSILCombine();
  // Which can expose opportunity for simplifycfg.
  P.addSimplifyCFG();
}

/// Perform semantic annotation/loop base optimizations.
void addHighLevelLoopOptPasses(SILPassPipelinePlan &P) {
  // Perform classic SSA optimizations for cleanup.
  P.addLowerAggregateInstrs();
  P.addSILCombine();
  P.addEarlySROA();
  P.addMem2Reg();
  P.addDCE();
  P.addSILCombine();
  addSimplifyCFGSILCombinePasses(P);

  // Run high-level loop opts.
  P.addLoopRotate();

  // Cleanup.
  P.addDCE();
  // Also CSE semantic calls.
  P.addHighLevelCSE();
  P.addSILCombine();
  P.addSimplifyCFG();
  // Optimize access markers for better LICM: might merge accesses
  // It will also set the no_nested_conflict for dynamic accesses
  // AccessEnforcementReleaseSinking results in non-canonical OSSA.
  // It is only used to expose opportunities in AccessEnforcementOpts
  // before CanonicalOSSA re-hoists destroys.
  P.addAccessEnforcementReleaseSinking();
  P.addAccessEnforcementOpts();
  P.addHighLevelLICM();
  // Simplify CFG after LICM that creates new exit blocks
  P.addSimplifyCFG();
  // LICM might have added new merging potential by hoisting
  // we don't want to restart the pipeline - ignore the
  // potential of merging out of two loops
  // AccessEnforcementReleaseSinking results in non-canonical OSSA.
  // It is only used to expose opportunities in AccessEnforcementOpts
  // before CanonicalOSSA re-hoists destroys.
  P.addAccessEnforcementReleaseSinking();
  P.addAccessEnforcementOpts();
  // Start of loop unrolling passes.
  P.addArrayCountPropagation();
  // To simplify induction variable.
  P.addSILCombine();
  P.addLoopUnroll();
  P.addSimplifyCFG();
  P.addPerformanceConstantPropagation();
  P.addSimplifyCFG();
  P.addArrayElementPropagation();
  // End of unrolling passes.
  P.addABCOpt();
  // Cleanup.
  P.addDCE();
  P.addCOWArrayOpts();
  // Cleanup.
  P.addDCE();
  P.addSwiftArrayPropertyOpt();
}

// Primary FunctionPass pipeline.
//
// Inserting a module passes within this pipeline would break the pipeline
// restart functionality.
void addFunctionPasses(SILPassPipelinePlan &P,
                       OptimizationLevelKind OpLevel) {
  // Promote box allocations to stack allocations.
  P.addAllocBoxToStack();

  if (P.getOptions().DestroyHoisting == DestroyHoistingOption::On) {
    P.addDestroyAddrHoisting();
  }

  // Propagate copies through stack locations.  Should run after
  // box-to-stack promotion since it is limited to propagating through
  // stack locations. Should run before aggregate lowering since that
  // splits up copy_addr.
  P.addCopyForwarding();

  // This DCE pass is the only DCE on ownership SIL. It can cleanup OSSA related
  // dead code, e.g. left behind by the ObjCBridgingOptimization.
  P.addDCE();

  // Optimize copies from a temporary (an "l-value") to a destination.
  P.addTempLValueOpt();

  // Split up opaque operations (copy_addr, retain_value, etc.).
  P.addLowerAggregateInstrs();

  // Split up operations on stack-allocated aggregates (struct, tuple).
  if (OpLevel == OptimizationLevelKind::HighLevel) {
    P.addEarlySROA();
  } else {
    P.addSROA();
  }

  // Promote stack allocations to values.
  P.addMem2Reg();

  // Run the existential specializer Pass.
  P.addExistentialSpecializer();

  // Cleanup, which is important if the inliner has restarted the pass pipeline.
  P.addPerformanceConstantPropagation();

  if (!P.getOptions().EnableOSSAModules && !SILDisableLateOMEByDefault) {
    if (P.getOptions().StopOptimizationBeforeLoweringOwnership)
      return;

    if (SILPrintFinalOSSAModule) {
      addModulePrinterPipeline(P, "SIL Print Final OSSA Module");
    }
    P.addNonTransparentFunctionOwnershipModelEliminator();
  }

  addSimplifyCFGSILCombinePasses(P);

  P.addArrayElementPropagation();

  // Perform a round of loop/array optimization in the mid-level pipeline after
  // potentially inlining semantic calls, e.g. Array append. The high level
  // pipeline only optimizes semantic calls *after* inlining (see
  // addHighLevelLoopOptPasses). For example, the high-level pipeline may
  // perform ArrayElementPropagation and after inlining a level of semantic
  // calls, the mid-level pipeline may handle uniqueness hoisting. Do this as
  // late as possible before inlining because it must run between runs of the
  // inliner when the pipeline restarts.
  if (OpLevel == OptimizationLevelKind::MidLevel) {
    P.addHighLevelLICM();
    P.addArrayCountPropagation();
    P.addABCOpt();
    P.addDCE();
    P.addCOWArrayOpts();
    P.addDCE();
    P.addSwiftArrayPropertyOpt();
    
    // This string optimization can catch additional opportunities, which are
    // exposed once optimized String interpolations (from the high-level string
    // optimization) are cleaned up. But before the mid-level inliner inlines
    // semantic calls.
    P.addStringOptimization();
  }

  // Run the devirtualizer, specializer, and inliner. If any of these
  // makes a change we'll end up restarting the function passes on the
  // current function (after optimizing any new callees).
  P.addDevirtualizer();
  P.addGenericSpecializer();
  // Run devirtualizer after the specializer, because many
  // class_method/witness_method instructions may use concrete types now.
  P.addDevirtualizer();
  P.addARCSequenceOpts();

  if (P.getOptions().EnableOSSAModules) {
    // We earlier eliminated ownership if we are not compiling the stdlib. Now
    // handle the stdlib functions, re-simplifying, eliminating ARC as we do.
    if (P.getOptions().CopyPropagation != CopyPropagationOption::Off) {
      P.addCopyPropagation();
    }
    P.addSemanticARCOpts();
  }

  switch (OpLevel) {
  case OptimizationLevelKind::HighLevel:
    // Does not inline functions with defined semantics or effects.
    P.addEarlyPerfInliner();
    break;
  case OptimizationLevelKind::MidLevel:
  case OptimizationLevelKind::LowLevel:
    // Inlines everything
    P.addPerfInliner();
    break;
  }

  // Clean up Semantic ARC before we perform additional post-inliner opts.
  if (P.getOptions().EnableOSSAModules) {
    if (P.getOptions().CopyPropagation != CopyPropagationOption::Off) {
      P.addCopyPropagation();
    }
    P.addSemanticARCOpts();
  }

  // Promote stack allocations to values and eliminate redundant
  // loads.
  P.addMem2Reg();
  P.addPerformanceConstantPropagation();
  //  Do a round of CFG simplification, followed by peepholes, then
  //  more CFG simplification.

  // Jump threading can expose opportunity for SILCombine (enum -> is_enum_tag->
  // cond_br).
  P.addJumpThreadSimplifyCFG();
  P.addPhiExpansion();
  P.addSILCombine();
  // SILCombine can expose further opportunities for SimplifyCFG.
  P.addSimplifyCFG();

  P.addCSE();
  if (OpLevel == OptimizationLevelKind::HighLevel) {
    // Early RLE does not touch loads from Arrays. This is important because
    // later array optimizations, like ABCOpt, get confused if an array load in
    // a loop is converted to a pattern with a phi argument.
    P.addEarlyRedundantLoadElimination();
  } else {
    P.addRedundantLoadElimination();
  }
  // Optimize copies created during RLE.
  P.addSemanticARCOpts();

  P.addCOWOpts();
  P.addPerformanceConstantPropagation();
  // Remove redundant arguments right before CSE and DCE, so that CSE and DCE
  // can cleanup redundant and dead instructions.
  P.addRedundantPhiElimination();
  P.addCSE();
  P.addDCE();

  // Perform retain/release code motion and run the first ARC optimizer.
  P.addEarlyCodeMotion();
  P.addReleaseHoisting();
  P.addARCSequenceOpts();
  P.addTempRValueOpt();

  P.addSimplifyCFG();
  if (OpLevel == OptimizationLevelKind::LowLevel) {
    // Only hoist releases very late.
    P.addLateCodeMotion();
  } else
    P.addEarlyCodeMotion();

  P.addRetainSinking();
  // Retain sinking does not sink all retains in one round.
  // Let it run one more time time, because it can be beneficial.
  // FIXME: Improve the RetainSinking pass to sink more/all
  // retains in one go.
  P.addRetainSinking();
  P.addReleaseHoisting();
  P.addARCSequenceOpts();

  // Run a final round of ARC opts when ownership is enabled.
  if (P.getOptions().EnableOSSAModules) {
    if (P.getOptions().CopyPropagation != CopyPropagationOption::Off) {
      P.addCopyPropagation();
    }
    P.addSemanticARCOpts();
  }
}

static void addPerfDebugSerializationPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("Performance Debug Serialization");
  P.addPerformanceSILLinker();
}


static void addPrepareOptimizationsPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("PrepareOptimizationPasses");

  // Verify AccessStorage once in OSSA before optimizing.
#ifndef NDEBUG
  P.addAccessPathVerification();
#endif

  P.addForEachLoopUnroll();
  P.addSimplification();
  P.addAccessMarkerElimination();
}

static void addPerfEarlyModulePassPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("EarlyModulePasses");

  // Get rid of apparently dead functions as soon as possible so that
  // we do not spend time optimizing them.
  P.addDeadFunctionAndGlobalElimination();

  // Cleanup after SILGen: remove trivial copies to temporaries.
  P.addTempRValueOpt();
  // Cleanup after SILGen: remove unneeded borrows/copies.
  if (P.getOptions().CopyPropagation == CopyPropagationOption::On) {
    P.addComputeSideEffects();
    P.addCopyPropagation();
  }
  P.addSemanticARCOpts();

  // Devirtualizes differentiability witnesses into functions that reference them.
  // This unblocks many other passes' optimizations (e.g. inlining) and this is
  // not blocked by any other passes' optimizations, so do it early.
  P.addDifferentiabilityWitnessDevirtualizer();

  if (!P.getOptions().EnableOSSAModules && SILDisableLateOMEByDefault) {
    if (P.getOptions().StopOptimizationBeforeLoweringOwnership)
      return;

    if (SILPrintFinalOSSAModule) {
      addModulePrinterPipeline(P, "SIL Print Final OSSA Module");
    }
    P.addNonTransparentFunctionOwnershipModelEliminator();
  }

  // Start by linking in referenced functions from other modules.
  P.addPerformanceSILLinker();

  // Cleanup after SILGen: remove trivial copies to temporaries. This version of
  // temp-rvalue opt is here so that we can hit copies from non-ossa code that
  // is linked in from the stdlib.
  P.addTempRValueOpt();

  // Add the outliner pass (Osize).
  P.addOutliner();
}

// The "high-level" pipeline serves two purposes:
//
// 1. Optimize the standard library Swift module prior to serialization. This
// reduces the amount of work during compilation of all non-stdlib clients.
//
// 2. Optimize caller functions before inlining semantic calls inside
// callees. This provides more precise escape analysis and side effect analysis
// of callee arguments.
static void addHighLevelFunctionPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("HighLevel,Function+EarlyLoopOpt");
  // FIXME: update EagerSpecializer to be a function pass!
  P.addEagerSpecializer();
  P.addObjCBridgingOptimization();

  addFunctionPasses(P, OptimizationLevelKind::HighLevel);

  addHighLevelLoopOptPasses(P);
  
  P.addStringOptimization();
  P.addComputeEscapeEffects();
  P.addComputeSideEffects();
}

// After "high-level" function passes have processed the entire call tree, run
// one round of module passes.
static void addHighLevelModulePipeline(SILPassPipelinePlan &P) {
  P.startPipeline("HighLevel,Module+StackPromote");
  P.addDeadFunctionAndGlobalElimination();
  P.addPerformanceSILLinker();
  P.addDeadObjectElimination();
  P.addGlobalPropertyOpt();

  // Do the first stack promotion on high-level SIL before serialization.
  //
  // FIXME: why does StackPromotion need to run in the module pipeline?
  P.addComputeEscapeEffects();
  P.addComputeSideEffects();
  P.addStackPromotion();

  P.addLetPropertiesOpt();
}

static void addMidLevelFunctionPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("MidLevel,Function", true /*isFunctionPassPipeline*/);

  addFunctionPasses(P, OptimizationLevelKind::MidLevel);

  // Specialize partially applied functions with dead arguments as a preparation
  // for CapturePropagation.
  P.addDeadArgSignatureOpt();

  // A LICM pass at mid-level is mainly needed to hoist addressors of globals.
  // It needs to be before global_init functions are inlined.
  P.addLICM();
  // Run loop unrolling after inlining and constant propagation, because loop
  // trip counts may have became constant.
  P.addLICM();
  P.addLoopUnroll();
}

static void addClosureSpecializePassPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("ClosureSpecialize");
  P.addDeadFunctionAndGlobalElimination();
  P.addReadOnlyGlobalVariablesPass();
  P.addTargetConstantFolding();
  P.addDeadStoreElimination();
  P.addDeadObjectElimination();

  // These few passes are needed to cleanup between loop unrolling and InitializeStaticGlobals.
  // This is needed to fully optimize static small String constants.
  P.addSimplifyCFG();
  P.addSILCombine();
  P.addPerformanceConstantPropagation();
  P.addSimplifyCFG();
  P.addSimplification();

  P.addInitializeStaticGlobals();

  // ComputeEffects should be done at the end of a function-pipeline. The next
  // pass (LetPropertiesOpt) is a module pass, so this is the end of a function-pipeline.
  P.addComputeEscapeEffects();
  P.addComputeSideEffects();

  P.addLetPropertiesOpt();

  // Propagate constants into closures and convert to static dispatch.  This
  // should run after specialization and inlining because we don't want to
  // specialize a call that can be inlined. It should run before
  // ClosureSpecialization, because constant propagation is more effective.  At
  // least one round of SSA optimization and inlining should run after this to
  // take advantage of static dispatch.
  P.addCapturePropagation();

  // Specialize closure.
  P.addClosureSpecializer();

  // Do the second stack promotion on low-level SIL.
  P.addStackPromotion();

  // Speculate virtual call targets.
  if (P.getOptions().EnableSpeculativeDevirtualization) {
    P.addSpeculativeDevirtualization();
  }

  // There should be at least one SILCombine+SimplifyCFG between the
  // ClosureSpecializer, etc. and the last inliner. Cleaning up after these
  // passes can expose more inlining opportunities.
  addSimplifyCFGSILCombinePasses(P);

  P.addComputeEscapeEffects();
  P.addComputeSideEffects();

  // We do this late since it is a pass like the inline caches that we only want
  // to run once very late. Make sure to run at least one round of the ARC
  // optimizer after this.
}

static void addLowLevelPassPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("LowLevel,Function", true /*isFunctionPassPipeline*/);

  // Should be after FunctionSignatureOpts and before the last inliner.
  P.addReleaseDevirtualizer();

  addFunctionPasses(P, OptimizationLevelKind::LowLevel);

  // The NamedReturnValueOptimization shouldn't be done before serialization.
  // For details see the comment for `namedReturnValueOptimization`.
  P.addNamedReturnValueOptimization();

  P.addDeadObjectElimination();
  P.addObjectOutliner();
  P.addDeadStoreElimination();

  // dead-store-elimination can expose opportunities for dead object elimination.
  P.addDeadObjectElimination();

  // We've done a lot of optimizations on this function, attempt to FSO.
  P.addFunctionSignatureOpts();
  P.addComputeEscapeEffects();
  P.addComputeSideEffects();
}

static void addLateLoopOptPassPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("LateLoopOpt");

  // Delete dead code and drop the bodies of shared functions.
  // Also, remove externally available witness tables. They are not needed
  // anymore after the last devirtualizer run.
  P.addLateDeadFunctionAndGlobalElimination();

  // Perform the final lowering transformations.
  P.addCodeSinking();
  // Optimize access markers for better LICM: might merge accesses
  // It will also set the no_nested_conflict for dynamic accesses
  P.addAccessEnforcementReleaseSinking();
  P.addAccessEnforcementOpts();
  P.addLICM();
  P.addCOWOpts();
  // Simplify CFG after LICM that creates new exit blocks
  P.addSimplifyCFG();
  // LICM might have added new merging potential by hoisting
  // we don't want to restart the pipeline - ignore the
  // potential of merging out of two loops
  P.addAccessEnforcementReleaseSinking();
  P.addAccessEnforcementOpts();

  // Sometimes stack promotion can catch cases only at this late stage of the
  // pipeline, after FunctionSignatureOpts.
  P.addComputeEscapeEffects();
  P.addComputeSideEffects();
  P.addStackPromotion();

  // Optimize overflow checks.
  P.addRedundantOverflowCheckRemoval();
  P.addMergeCondFails();

  // Remove dead code.
  P.addDCE();
  P.addSILCombine();
  P.addSimplifyCFG();

  // Try to hoist all releases, including epilogue releases. This should be
  // after FSO.
  P.addLateReleaseHoisting();
}

// Run passes that
// - should only run after all general SIL transformations.
// - have no reason to run before any other SIL optimizations.
// - don't require IRGen information.
static void addLastChanceOptPassPipeline(SILPassPipelinePlan &P) {
  // Optimize access markers for improved IRGen after all other optimizations.
  P.addOptimizeHopToExecutor();
  P.addAccessEnforcementReleaseSinking();
  P.addAccessEnforcementOpts();
  P.addAccessEnforcementWMO();
  P.addAccessEnforcementDom();
  // addAccessEnforcementDom might provide potential for LICM:
  // A loop might have only one dynamic access now, i.e. hoistable
  P.addLICM();

  // Verify AccessStorage once again after optimizing and lowering OSSA.
#ifndef NDEBUG
  P.addAccessPathVerification();
#endif

  // Only has an effect if the -assume-single-thread option is specified.
  if (P.getOptions().AssumeSingleThreaded) {
    P.addAssumeSingleThreaded();
  }

  // Emits remarks on all functions with @_assemblyVision attribute.
  P.addAssemblyVisionRemarkGenerator();

  // In optimized builds, do the inter-procedural analysis in a module pass.
  P.addStackProtection();

  // FIXME: rdar://72935649 (Miscompile on combining PruneVTables with WMO)
  // P.addPruneVTables();
}

static void addSILDebugInfoGeneratorPipeline(SILPassPipelinePlan &P) {
  P.startPipeline("SIL Debug Info Generator");
  P.addSILDebugInfoGenerator();
}

/// Mandatory IRGen preparation. It is the caller's job to set the set stage to
/// "lowered" after running this pipeline.
SILPassPipelinePlan
SILPassPipelinePlan::getLoweringPassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("Lowering");
  P.addLowerHopToActor(); // FIXME: earlier for more opportunities?
  P.addOwnershipModelEliminator();
  P.addIRGenPrepare();

  return P;
}

SILPassPipelinePlan
SILPassPipelinePlan::getIRGenPreparePassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("IRGen Preparation");
  // Insert SIL passes to run during IRGen.
  /*
  // Simplify partial_apply instructions by expanding box construction into
  // component operations.
  P.addPartialApplySimplification();
   */
  // Hoist generic alloc_stack instructions to the entry block to enable better
  // llvm-ir generation for dynamic alloca instructions.
  P.addAllocStackHoisting();
  // Change large loadable types to be passed indirectly across function
  // boundaries as required by the ABI.
  P.addLoadableByAddress();

  if (Options.EnablePackMetadataStackPromotion) {
    // Insert marker instructions indicating where on-stack pack metadata
    // deallocation must occur.
    //
    // No code motion may occur after this pass: alloc_pack_metadata must
    // directly precede the instruction on behalf of which metadata will
    // actually be emitted (e.g. apply).
    P.addPackMetadataMarkerInserter();
  }

  return P;
}

SILPassPipelinePlan
SILPassPipelinePlan::getPerformancePassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);

  if (Options.DebugSerialization) {
    addPerfDebugSerializationPipeline(P);
    return P;
  }
  
  // Passes which run once before all other optimizations run. Those passes are
  // _not_ intended to run later again.
  addPrepareOptimizationsPipeline(P);

  // Eliminate immediately dead functions and then clone functions from the
  // stdlib.
  //
  // This also performs early OSSA based optimizations on *all* swift code.
  addPerfEarlyModulePassPipeline(P);

  // Then run an iteration of the high-level SSA passes.
  //
  // FIXME: When *not* emitting a .swiftmodule, skip the high-level function
  // pipeline to save compile time.
  addHighLevelFunctionPipeline(P);

  // Then if we were asked to stop optimization before lowering OSSA (causing us
  // to exit early from addHighLevelFunctionPipeline), exit early.
  if (P.getOptions().StopOptimizationBeforeLoweringOwnership)
    return P;

  addHighLevelModulePipeline(P);

  // Run one last copy propagation/semantic arc opts run before serialization/us
  // lowering ownership.
  if (P.getOptions().EnableOSSAModules) {
    if (P.getOptions().CopyPropagation != CopyPropagationOption::Off) {
      P.addCopyPropagation();
    }
    P.addSemanticARCOpts();
  }

  P.addCrossModuleOptimization();

  // It is important to serialize before any of the @_semantics
  // functions are inlined, because otherwise the information about
  // uses of such functions inside the module is lost,
  // which reduces the ability of the compiler to optimize clients
  // importing this module.
  P.addSerializeSILPass();

  // Strip any transparent functions that still have ownership.
  P.addOwnershipModelEliminator();

  if (Options.StopOptimizationAfterSerialization)
    return P;

  // After serialization run the function pass pipeline to iteratively lower
  // high-level constructs like @_semantics calls.
  addMidLevelFunctionPipeline(P);

  // Perform optimizations that specialize.
  addClosureSpecializePassPipeline(P);

  // Run another iteration of the SSA optimizations to optimize the
  // devirtualized inline caches and constants propagated into closures
  // (CapturePropagation).
  addLowLevelPassPipeline(P);

  addLateLoopOptPassPipeline(P);

  addLastChanceOptPassPipeline(P);

  // Has only an effect if the -sil-based-debuginfo option is specified.
  addSILDebugInfoGeneratorPipeline(P);

  // Call the CFG viewer.
  if (SILViewCFG) {
    addCFGPrinterPipeline(P, "SIL Before IRGen View CFG");
  }

  return P;
}

//===----------------------------------------------------------------------===//
//                            Onone Pass Pipeline
//===----------------------------------------------------------------------===//

SILPassPipelinePlan
SILPassPipelinePlan::getOnonePassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);

  // These are optimizations that we do not need to enable diagnostics (or
  // depend on other passes needed for diagnostics). Thus we can run them later
  // and avoid having SourceKit run these passes when just emitting diagnostics
  // in the editor.
  P.startPipeline("Non-Diagnostic Mandatory Optimizations");
  P.addForEachLoopUnroll();

  // TODO: MandatoryARCOpts should be subsumed by CopyPropagation. There should
  // be no need to run another analysis of copies at -Onone.
  P.addMandatoryARCOpts();

  // Create pre-specializations.
  // This needs to run pre-serialization because it needs to identify native
  // inlinable functions from imported ones.
  P.addOnonePrespecializations();

  // First serialize the SIL if we are asked to.
  P.startPipeline("Serialization");
  P.addSerializeSILPass();

  // Now that we have serialized, propagate debug info.
  P.addMovedAsyncVarDebugInfoPropagator();

  // If we are asked to stop optimizing before lowering ownership, do so now.
  if (P.Options.StopOptimizationBeforeLoweringOwnership)
    return P;

  // Now strip any transparent functions that still have ownership.
  P.addOwnershipModelEliminator();

  // Finally perform some small transforms.
  P.startPipeline("Rest of Onone");
  P.addUsePrespecialized();

  // Needed to fold MemoryLayout constants in performance-annotated functions.
  P.addTargetConstantFolding();

  // Has only an effect if the -assume-single-thread option is specified.
  if (P.getOptions().AssumeSingleThreaded) {
    P.addAssumeSingleThreaded();
  }

  // In Onone builds, do a function-local analysis in a function pass.
  P.addFunctionStackProtection();

  // This is mainly there to optimize `Builtin.isConcrete`, which must not be
  // constant folded before any generic specialization.
  P.addLateOnoneSimplification();

  P.addCleanupDebugSteps();

  // Has only an effect if the -sil-based-debuginfo option is specified.
  P.addSILDebugInfoGenerator();

  return P;
}

//===----------------------------------------------------------------------===//
//                        Serialize SIL Pass Pipeline
//===----------------------------------------------------------------------===//

// Add to P a new pipeline that just serializes SIL. Meant to be used in
// situations where perf optzns are disabled, but we may need to serialize.
SILPassPipelinePlan
SILPassPipelinePlan::getSerializeSILPassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("Serialize SIL");
  P.addSerializeSILPass();
  return P;
}

//===----------------------------------------------------------------------===//
//                          Inst Count Pass Pipeline
//===----------------------------------------------------------------------===//

SILPassPipelinePlan
SILPassPipelinePlan::getInstCountPassPipeline(const SILOptions &Options) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("Inst Count");
  P.addInstCount();
  return P;
}

//===----------------------------------------------------------------------===//
//                          Pass Kind List Pipeline
//===----------------------------------------------------------------------===//

void SILPassPipelinePlan::addPasses(ArrayRef<PassKind> PassKinds) {
  for (auto K : PassKinds) {
    // We could add to the Kind list directly, but we want to allow for
    // additional code to be added to add* without this code needing to be
    // updated.
    switch (K) {
// Each pass gets its own add-function.
#define PASS(ID, TAG, NAME)                                                    \
  case PassKind::ID: {                                                         \
    add##ID();                                                                 \
    break;                                                                     \
  }
#include "swift/SILOptimizer/PassManager/Passes.def"
    case PassKind::invalidPassKind:
      llvm_unreachable("Unhandled pass kind?!");
    }
  }
}

SILPassPipelinePlan
SILPassPipelinePlan::getPassPipelineForKinds(const SILOptions &Options,
                                             ArrayRef<PassKind> PassKinds) {
  SILPassPipelinePlan P(Options);
  P.startPipeline("Pass List Pipeline");
  P.addPasses(PassKinds);
  return P;
}

//===----------------------------------------------------------------------===//
//                Dumping And Loading Pass Pipelines from Yaml
//===----------------------------------------------------------------------===//

namespace {

struct YAMLPassPipeline {
  std::string name;
  std::vector<PassKind> passes;

  YAMLPassPipeline() {}
  YAMLPassPipeline(const SILPassPipeline &pipeline,
                   SILPassPipelinePlan::PipelineKindRange pipelineKinds)
      : name(pipeline.Name), passes() {
    llvm::copy(pipelineKinds, std::back_inserter(passes));
  }
};

} // end anonymous namespace

namespace llvm {
namespace yaml {

template <> struct ScalarEnumerationTraits<PassKind> {
  static void enumeration(IO &io, PassKind &value) {
#define PASS(ID, TAG, NAME) io.enumCase(value, #TAG, PassKind::ID);
#include "swift/SILOptimizer/PassManager/Passes.def"
  }
};

template <> struct MappingTraits<YAMLPassPipeline> {
  static void mapping(IO &io, YAMLPassPipeline &info) {
    io.mapRequired("name", info.name);
    io.mapRequired("passes", info.passes);
  }
};

} // namespace yaml
} // namespace llvm

LLVM_YAML_IS_FLOW_SEQUENCE_VECTOR(PassKind)
LLVM_YAML_IS_DOCUMENT_LIST_VECTOR(YAMLPassPipeline)

void SILPassPipelinePlan::dump() {
  print(llvm::errs());
  llvm::errs() << '\n';
}

void SILPassPipelinePlan::print(llvm::raw_ostream &os) {
  llvm::yaml::Output out(os);
  std::vector<YAMLPassPipeline> data;
  transform(getPipelines(), std::back_inserter(data),
            [&](const SILPassPipeline &pipeline) {
              return YAMLPassPipeline(pipeline, getPipelinePasses(pipeline));
            });
  out << data;
}

SILPassPipelinePlan
SILPassPipelinePlan::getPassPipelineFromFile(const SILOptions &options,
                                             StringRef filename) {
  std::vector<YAMLPassPipeline> yamlPipelines;
  {
    // Load the input file.
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
        llvm::MemoryBuffer::getFileOrSTDIN(filename);
    if (!fileBufOrErr) {
      llvm_unreachable("Failed to read yaml file");
    }

    llvm::yaml::Input in(fileBufOrErr->get()->getBuffer());
    in >> yamlPipelines;
  }

  SILPassPipelinePlan silPlan(options);

  for (auto &pipeline : yamlPipelines) {
    silPlan.startPipeline(pipeline.name);
    silPlan.addPasses(pipeline.passes);
  }

  return silPlan;
}
