//===--- Passes.cpp - Swift Compiler SIL Pass Entrypoints -----------------===//
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
///
///  \file
///  This file provides implementations of a few helper functions
///  which provide abstracted entrypoints to the SILPasses stage.
///
///  \note The actual SIL passes should be implemented in per-pass source files,
///  not in this file.
///
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "sil-optimizer"

#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Module.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILBridgingUtils.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/InstOptUtils.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/YAMLParser.h"
extern "C" {
#include "swift/SILOptimizer/OptimizerBridging.h"
}

using namespace swift;

bool swift::runSILDiagnosticPasses(SILModule &Module) {
  auto &opts = Module.getOptions();

  // Verify the module, if required.
  if (opts.VerifyAll)
    Module.verify();

  // If we parsed a .sil file that is already in canonical form, don't rerun
  // the diagnostic passes.
  if (Module.getStage() != SILStage::Raw)
    return false;

  executePassPipelinePlan(&Module,
                          SILPassPipelinePlan::getDiagnosticPassPipeline(opts),
                          /*isMandatory*/ true);

  // If we were asked to debug serialization, exit now.
  auto &Ctx = Module.getASTContext();
  if (opts.DebugSerialization)
    return Ctx.hadError();

  // Generate diagnostics.
  Module.setStage(SILStage::Canonical);

  // Verify the module, if required.
  if (opts.VerifyAll)
    Module.verify();
  else {
    LLVM_DEBUG(Module.verify());
  }

  // If errors were produced during SIL analysis, return true.
  return Ctx.hadError();
}

bool swift::runSILLowerHopToActorPass(SILModule &Module) {
  auto &Ctx = Module.getASTContext();

  auto &opts = Module.getOptions();
  executePassPipelinePlan(
      &Module, SILPassPipelinePlan::getLowerHopToActorPassPipeline(opts));

  return Ctx.hadError();
}

bool swift::runSILOwnershipEliminatorPass(SILModule &Module) {
  auto &Ctx = Module.getASTContext();

  auto &opts = Module.getOptions();
  executePassPipelinePlan(
      &Module, SILPassPipelinePlan::getOwnershipEliminatorPassPipeline(opts));

  return Ctx.hadError();
}

void swift::runSILOptimizationPasses(SILModule &Module) {
  auto &opts = Module.getOptions();

  // Verify the module, if required.
  if (opts.VerifyAll)
    Module.verify();

  if (opts.DisableSILPerfOptimizations) {
    // If we are not supposed to run SIL perf optzns, we may still need to
    // serialize. So serialize now.
    executePassPipelinePlan(
        &Module, SILPassPipelinePlan::getSerializeSILPassPipeline(opts),
        /*isMandatory*/ true);
    return;
  }

  executePassPipelinePlan(
      &Module, SILPassPipelinePlan::getPerformancePassPipeline(opts));

  // Check if we actually serialized our module. If we did not, serialize now.
  if (!Module.isSerialized()) {
    executePassPipelinePlan(
        &Module, SILPassPipelinePlan::getSerializeSILPassPipeline(opts),
        /*isMandatory*/ true);
  }

  // If we were asked to debug serialization, exit now.
  if (opts.DebugSerialization)
    return;

  // Verify the module, if required.
  if (opts.VerifyAll)
    Module.verify();
  else {
    LLVM_DEBUG(Module.verify());
  }
}

void swift::runSILPassesForOnone(SILModule &Module) {
  // Verify the module, if required.
  if (Module.getOptions().VerifyAll)
    Module.verify();

  // We want to run the Onone passes also for function which have an explicit
  // Onone attribute.
  executePassPipelinePlan(
      &Module, SILPassPipelinePlan::getOnonePassPipeline(Module.getOptions()),
      /*isMandatory*/ true);

  // Verify the module, if required.
  if (Module.getOptions().VerifyAll)
    Module.verify();
  else {
    LLVM_DEBUG(Module.verify());
  }
}

void swift::runSILOptimizationPassesWithFileSpecification(SILModule &M,
                                                          StringRef Filename) {
  auto &opts = M.getOptions();
  executePassPipelinePlan(
      &M, SILPassPipelinePlan::getPassPipelineFromFile(opts, Filename));
}

/// Get the Pass ID enum value from an ID string.
PassKind swift::PassKindFromString(StringRef IDString) {
  return llvm::StringSwitch<PassKind>(IDString)
#define PASS(ID, TAG, DESCRIPTION) .Case(#ID, PassKind::ID)
#include "swift/SILOptimizer/PassManager/Passes.def"
      .Default(PassKind::invalidPassKind);
}

/// Get an ID string for the given pass Kind.
/// This is useful for tools that identify a pass
/// by its type name.
StringRef swift::PassKindID(PassKind Kind) {
  switch (Kind) {
#define PASS(ID, TAG, DESCRIPTION)                                             \
  case PassKind::ID:                                                           \
    return #ID;
#include "swift/SILOptimizer/PassManager/Passes.def"
  case PassKind::invalidPassKind:
    llvm_unreachable("Invalid pass kind?!");
  }

  llvm_unreachable("Unhandled PassKind in switch.");
}

/// Get a tag string for the given pass Kind.
/// This format is useful for command line options.
StringRef swift::PassKindTag(PassKind Kind) {
  switch (Kind) {
#define PASS(ID, TAG, DESCRIPTION)                                             \
  case PassKind::ID:                                                           \
    return TAG;
#include "swift/SILOptimizer/PassManager/Passes.def"
  case PassKind::invalidPassKind:
    llvm_unreachable("Invalid pass kind?!");
  }

  llvm_unreachable("Unhandled PassKind in switch.");
}

// During SIL Lowering, passes may see partially lowered SIL, which is
// inconsistent with the current (canonical) stage. We don't change the SIL
// stage until lowering is complete. Consequently, any pass added to this
// PassManager needs to be able to handle the output of the previous pass. If
// the function pass needs to read SIL from other functions, it may be best to
// convert it to a module pass to ensure that the SIL input is always at the
// same stage of lowering.
void swift::runSILLoweringPasses(SILModule &Module) {
  auto &opts = Module.getOptions();
  executePassPipelinePlan(&Module,
                          SILPassPipelinePlan::getLoweringPassPipeline(opts),
                          /*isMandatory*/ true);

  assert(Module.getStage() == SILStage::Lowered);
}

/// Registered briged pass run functions.
static llvm::StringMap<BridgedFunctionPassRunFn> bridgedPassRunFunctions;
static bool passesRegistered = false;

/// Runs a bridged function pass.
///
/// \p runFunction is a cache for the run function, so that it has to be looked
/// up only once in bridgedPassRunFunctions.
static void runBridgedFunctionPass(BridgedFunctionPassRunFn &runFunction,
                                   SILPassManager *passManager,
                                   SILFunction *f, StringRef passName) {
  if (!runFunction) {
    runFunction = bridgedPassRunFunctions[passName];
    if (!runFunction) {
      if (passesRegistered) {
        llvm::errs() << "Swift pass " << passName << " is not registered\n";
        abort();
      }
      return;
    }
  }
  if (!f->isBridged()) {
    llvm::errs() << "SILFunction metatype is not registered\n";
    abort();
  }
  runFunction({{f}, {passManager->getLibswiftPassInvocation()}});
}

// Called from libswift's initializeLibSwift().
void SILPassManager_registerFunctionPass(BridgedStringRef name,
                                         BridgedFunctionPassRunFn runFn) {
  bridgedPassRunFunctions[getStringRef(name)] = runFn;
  passesRegistered = true;
}

#define SWIFT_FUNCTION_PASS_COMMON(ID, TAG) \
class ID##Pass : public SILFunctionTransform {                             \
  static BridgedFunctionPassRunFn runFunction;                             \
  void run() override {                                                    \
    runBridgedFunctionPass(runFunction, PM, getFunction(), TAG);           \
  }                                                                        \
};                                                                         \
BridgedFunctionPassRunFn ID##Pass::runFunction = nullptr;                  \

#define PASS(ID, TAG, DESCRIPTION)
#define SWIFT_INSTRUCTION_PASS(INST, TAG)

#define SWIFT_FUNCTION_PASS(ID, TAG, DESCRIPTION) \
SWIFT_FUNCTION_PASS_COMMON(ID, TAG)                                        \
SILTransform *swift::create##ID() { return new ID##Pass(); }               \

#define SWIFT_FUNCTION_PASS_WITH_LEGACY(ID, TAG, DESCRIPTION) \
SWIFT_FUNCTION_PASS_COMMON(ID, TAG)                                        \
SILTransform *swift::create##ID() {                                        \
  if (passesRegistered)                                                    \
    return new ID##Pass();                                                 \
  return createLegacy##ID();                                               \
}                                                                          \

#include "swift/SILOptimizer/PassManager/Passes.def"

#undef SWIFT_FUNCTION_PASS_COMMON
