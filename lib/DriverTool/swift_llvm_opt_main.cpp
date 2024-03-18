//===--- swift_llvm_opt_main.cpp ------------------------------------------===//
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
/// \file
///
/// This is a simple reimplementation of opt that includes support for Swift-
/// specific LLVM passes. It is meant to make it easier to handle issues related
/// to transitioning to the new LLVM pass manager (which lacks the dynamism of
/// the old pass manager) and also problems during the code base transition to
/// that pass manager. Additionally it will enable a user to exactly simulate
/// Swift's LLVM pass pipeline by using the same pass pipeline building
/// machinery in IRGen, something not possible with opt.
///
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/LLVMPasses/PassesFwd.h"
#include "swift/LLVMPasses/Passes.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/CallGraphSCCPass.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Bitcode/BitcodeWriterPass.h"
#include "llvm/CodeGen/CommandFlags.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Passes/StandardInstrumentations.h"
#include "llvm/IR/Verifier.h"
#include "llvm/IRPrinter/IRPrintingPasses.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/LinkAllIR.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/TargetParser/SubtargetFeature.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/PluginLoader.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/SystemUtils.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/TargetParser/Host.h"
#include "llvm/Transforms/Scalar/LoopPassManager.h"

using namespace swift;

static llvm::codegen::RegisterCodeGenFlags CGF;

//===----------------------------------------------------------------------===//
//                            Option Declarations
//===----------------------------------------------------------------------===//

struct SwiftLLVMOptOptions {
  llvm::cl::opt<bool>
    Optimized = llvm::cl::opt<bool>("O", llvm::cl::desc("Optimization level O. Similar to swift -O"));

  llvm::cl::opt<std::string>
    TargetTriple = llvm::cl::opt<std::string>("mtriple",
                   llvm::cl::desc("Override target triple for module"));

  llvm::cl::opt<bool>
    PrintStats = llvm::cl::opt<bool>("print-stats",
                 llvm::cl::desc("Should LLVM Statistics be printed"));

  llvm::cl::opt<std::string>
    InputFilename = llvm::cl::opt<std::string>(llvm::cl::Positional,
                                            llvm::cl::desc("<input file>"),
                                            llvm::cl::init("-"),
                                            llvm::cl::value_desc("filename"));

  llvm::cl::opt<std::string>
    OutputFilename = llvm::cl::opt<std::string>("o", llvm::cl::desc("Override output filename"),
                     llvm::cl::value_desc("filename"));

  llvm::cl::opt<std::string>
    DefaultDataLayout = llvm::cl::opt<std::string>(
      "default-data-layout",
      llvm::cl::desc("data layout string to use if not specified by module"),
      llvm::cl::value_desc("layout-string"), llvm::cl::init(""));
};

static llvm::cl::opt<std::string> PassPipeline(
    "passes",
    llvm::cl::desc(
        "A textual description of the pass pipeline. To have analysis passes "
        "available before a certain pass, add 'require<foo-analysis>'."));
//===----------------------------------------------------------------------===//
//                               Helper Methods
//===----------------------------------------------------------------------===//

static llvm::CodeGenOpt::Level GetCodeGenOptLevel(const SwiftLLVMOptOptions &options) {
  // TODO: Is this the right thing to do here?
  if (options.Optimized)
    return llvm::CodeGenOpt::Default;
  return llvm::CodeGenOpt::None;
}

// Returns the TargetMachine instance or zero if no triple is provided.
static llvm::TargetMachine *
getTargetMachine(llvm::Triple TheTriple, StringRef CPUStr,
                 StringRef FeaturesStr, const llvm::TargetOptions &targetOptions,
                 const SwiftLLVMOptOptions &options) {
  std::string Error;
  const auto *TheTarget = llvm::TargetRegistry::lookupTarget(
      llvm::codegen::getMArch(), TheTriple, Error);
  // Some modules don't specify a triple, and this is okay.
  if (!TheTarget) {
    return nullptr;
  }

  return TheTarget->createTargetMachine(
      TheTriple.getTriple(), CPUStr, FeaturesStr, targetOptions,
      std::optional<llvm::Reloc::Model>(llvm::codegen::getExplicitRelocModel()),
      llvm::codegen::getExplicitCodeModel(), GetCodeGenOptLevel(options));
}

//===----------------------------------------------------------------------===//
//                            Main Implementation
//===----------------------------------------------------------------------===//

int swift_llvm_opt_main(ArrayRef<const char *> argv, void *MainAddr) {
  INITIALIZE_LLVM();

  SwiftLLVMOptOptions options;

  llvm::cl::ParseCommandLineOptions(argv.size(), argv.data(), "Swift LLVM optimizer\n");

  if (options.PrintStats)
    llvm::EnableStatistics();

  llvm::SMDiagnostic Err;

  // Load the input module...
  auto LLVMContext = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> M =
      parseIRFile(options.InputFilename, Err, *LLVMContext.get());

  if (!M) {
    Err.print(argv[0], llvm::errs());
    return 1;
  }

  if (verifyModule(*M, &llvm::errs())) {
    llvm::errs() << argv[0] << ": " << options.InputFilename
           << ": error: input module is broken!\n";
    return 1;
  }

  // If we are supposed to override the target triple, do so now.
  if (!options.TargetTriple.empty())
    M->setTargetTriple(llvm::Triple::normalize(options.TargetTriple));

  // Figure out what stream we are supposed to write to...
  std::unique_ptr<llvm::ToolOutputFile> Out;
  // Default to standard output.
  if (options.OutputFilename.empty())
    options.OutputFilename = "-";

  std::error_code EC;
  Out.reset(
      new llvm::ToolOutputFile(options.OutputFilename, EC, llvm::sys::fs::OF_None));
  if (EC) {
    llvm::errs() << EC.message() << '\n';
    return 1;
  }

  llvm::Triple ModuleTriple(M->getTargetTriple());
  std::string CPUStr, FeaturesStr;
  llvm::TargetMachine *Machine = nullptr;
  const llvm::TargetOptions targetOptions =
      llvm::codegen::InitTargetOptionsFromCodeGenFlags(ModuleTriple);

  if (ModuleTriple.getArch()) {
    CPUStr = llvm::codegen::getCPUStr();
    FeaturesStr = llvm::codegen::getFeaturesStr();
    Machine = getTargetMachine(ModuleTriple, CPUStr, FeaturesStr, targetOptions, options);
  }

  std::unique_ptr<llvm::TargetMachine> TM(Machine);

  // Override function attributes based on CPUStr, FeaturesStr, and command line
  // flags.
  llvm::codegen::setFunctionAttributes(CPUStr, FeaturesStr, *M);

  if (options.Optimized) {
    IRGenOptions Opts;
    Opts.OptMode = OptimizationMode::ForSpeed;
    Opts.OutputKind = IRGenOutputKind::LLVMAssemblyAfterOptimization;

    // Then perform the optimizations.
    SourceManager SM;
    DiagnosticEngine Diags(SM);
    performLLVMOptimizations(Opts, Diags, nullptr, M.get(), TM.get(),
                             &Out->os());
  } else {
    std::string Pipeline = PassPipeline;
    llvm::TargetLibraryInfoImpl TLII(ModuleTriple);
    if (TM)
      TM->setPGOOption(std::nullopt);
    llvm::LoopAnalysisManager LAM;
    llvm::FunctionAnalysisManager FAM;
    llvm::CGSCCAnalysisManager CGAM;
    llvm::ModuleAnalysisManager MAM;

    std::optional<llvm::PGOOptions> P = std::nullopt;
    llvm::PassInstrumentationCallbacks PIC;
    llvm::PrintPassOptions PrintPassOpts;

    PrintPassOpts.Verbose = false;
    PrintPassOpts.SkipAnalyses = false;
    llvm::StandardInstrumentations SI(M->getContext(), false, false, PrintPassOpts);
    SI.registerCallbacks(PIC, &MAM);

    llvm::PipelineTuningOptions PTO;
    // LoopUnrolling defaults on to true and DisableLoopUnrolling is initialized
    // to false above so we shouldn't necessarily need to check whether or not the
    // option has been enabled.
    PTO.LoopUnrolling = true;
    llvm::PassBuilder PB(TM.get(), PTO, P, &PIC);

    PB.registerPipelineParsingCallback(
                [ModuleTriple](StringRef Name, llvm::ModulePassManager &PM,
                   ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "swift-merge-functions") {
                    if (ModuleTriple.isArm64e())
                      PM.addPass(SwiftMergeFunctionsPass(true, 0));
                    else
                      PM.addPass(SwiftMergeFunctionsPass(false, 0));
                    return true;
                  }
                  return false;
                });
    PB.registerPipelineParsingCallback(
                [ModuleTriple](StringRef Name, llvm::FunctionPassManager &PM,
                   ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "swift-llvm-arc-optimize") {
                      PM.addPass(SwiftARCOptPass());
                    return true;
                  }
                  return false;
                });
    PB.registerPipelineParsingCallback(
                [ModuleTriple](StringRef Name, llvm::FunctionPassManager &PM,
                   ArrayRef<llvm::PassBuilder::PipelineElement>) {
                  if (Name == "swift-llvm-arc-contract") {
                      PM.addPass(SwiftARCContractPass());
                    return true;
                  }
                  return false;
                });
    auto AA = PB.buildDefaultAAPipeline();
    AA.registerFunctionAnalysis<SwiftAA>();

    // Register the AA manager first so that our version is the one used.
    FAM.registerPass([&] { return std::move(AA); });
    FAM.registerPass([&] { return SwiftAA(); });
    // Register our TargetLibraryInfoImpl.
    FAM.registerPass([&] { return llvm::TargetLibraryAnalysis(TLII); });

    // Register all the basic analyses with the managers.
    PB.registerModuleAnalyses(MAM);
    PB.registerCGSCCAnalyses(CGAM);
    PB.registerFunctionAnalyses(FAM);
    PB.registerLoopAnalyses(LAM);
    PB.crossRegisterProxies(LAM, FAM, CGAM, MAM);

    llvm::ModulePassManager MPM;
    if (!Pipeline.empty()) {
      if (auto Err = PB.parsePassPipeline(MPM, Pipeline)) {
        llvm::errs() << argv[0] << ": " << toString(std::move(Err)) << "\n";
        return 1;
      }
    }
    MPM.addPass(llvm::PrintModulePass(Out.get()->os(), "", false, false));
    MPM.run(*M, MAM);
  }

  return 0;
}
