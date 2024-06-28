//===--- sil_llvm_gen_main.cpp --------------------------------------------===//
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
/// This is a tool for reading sil files and running IRGen passes upon them. It
/// is not meant to be used to run llvm optimizations on llvm-ir.
///
//===----------------------------------------------------------------------===//

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/ScopeExit.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include <cstdio>
using namespace swift;

struct SILLLVMGenOptions {
  llvm::cl::opt<std::string>
    InputFilename = llvm::cl::opt<std::string>(llvm::cl::desc("input file"),
                                                  llvm::cl::init("-"),
                                                  llvm::cl::Positional);

  llvm::cl::opt<std::string>
    OutputFilename = llvm::cl::opt<std::string>("o", llvm::cl::init("-"), llvm::cl::desc("output filename"));

  llvm::cl::list<std::string>
    ImportPaths = llvm::cl::list<std::string>("I",
                  llvm::cl::desc("add a directory to the import search path"));

  llvm::cl::list<std::string>
    FrameworkPaths = llvm::cl::list<std::string>(
      "F", llvm::cl::desc("add a directory to the framework search path"));

  llvm::cl::opt<std::string>
    ModuleName = llvm::cl::opt<std::string>("module-name",
                 llvm::cl::desc("The name of the module if processing"
                                " a module. Necessary for processing "
                                "stdin."));

  llvm::cl::opt<std::string>
    ResourceDir = llvm::cl::opt<std::string>(
      "resource-dir",
      llvm::cl::desc("The directory that holds the compiler resource files"));

  llvm::cl::opt<std::string>
    SDKPath = llvm::cl::opt<std::string>("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                                                               "importer."),
              llvm::cl::init(""));

  llvm::cl::opt<std::string>
    Target = llvm::cl::opt<std::string>("target",
                                           llvm::cl::desc("target triple"));

  llvm::cl::opt<bool>
    PrintStats = llvm::cl::opt<bool>("print-stats", llvm::cl::desc("Print various statistics"));

  llvm::cl::opt<std::string>
    ModuleCachePath = llvm::cl::opt<std::string>("module-cache-path",
                      llvm::cl::desc("Clang module cache path"));

  llvm::cl::opt<bool>
    PerformWMO = llvm::cl::opt<bool>("wmo", llvm::cl::desc("Enable whole-module optimizations"));

  llvm::cl::opt<IRGenOutputKind>
    OutputKind = llvm::cl::opt<IRGenOutputKind>(
      "output-kind", llvm::cl::desc("Type of output to produce"),
      llvm::cl::values(clEnumValN(IRGenOutputKind::LLVMAssemblyAfterOptimization,
                                  "llvm-as", "Emit llvm assembly"),
                       clEnumValN(IRGenOutputKind::LLVMBitcode, "llvm-bc",
                                  "Emit llvm bitcode"),
                       clEnumValN(IRGenOutputKind::NativeAssembly, "as",
                                  "Emit native assembly"),
                       clEnumValN(IRGenOutputKind::ObjectFile, "object",
                                  "Emit an object file")),
      llvm::cl::init(IRGenOutputKind::ObjectFile));

  llvm::cl::opt<bool>
    DisableLegacyTypeInfo = llvm::cl::opt<bool>("disable-legacy-type-info",
          llvm::cl::desc("Don't try to load backward deployment layouts"));
};

int sil_llvm_gen_main(ArrayRef<const char *> argv, void *MainAddr) {
  INITIALIZE_LLVM();

  SILLLVMGenOptions options;

  llvm::cl::ParseCommandLineOptions(argv.size(), argv.data(), "Swift LLVM IR Generator\n");

  if (options.PrintStats)
    llvm::EnableStatistics();

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(llvm::sys::fs::getMainExecutable(argv[0], MainAddr));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(options.ImportPaths);
  std::vector<SearchPathOptions::FrameworkSearchPath> FramePaths;
  for (const auto &path : options.FrameworkPaths) {
    FramePaths.push_back({path, /*isSystem=*/false});
  }
  Invocation.setFrameworkSearchPaths(FramePaths);
  // Set the SDK path and target if given.
  if (options.SDKPath.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      options.SDKPath = SDKROOT;
  }
  if (!options.SDKPath.empty())
    Invocation.setSDKPath(options.SDKPath);
  if (!options.Target.empty())
    Invocation.setTargetTriple(options.Target);
  if (!options.ResourceDir.empty())
    Invocation.setRuntimeResourcePath(options.ResourceDir);
  // Set the module cache path. If not passed in we use the default swift module
  // cache.
  Invocation.getClangImporterOptions().ModuleCachePath = options.ModuleCachePath;
  Invocation.setParseStdlib();

  // Setup the language options
  auto &LangOpts = Invocation.getLangOptions();
  LangOpts.DisableAvailabilityChecking = true;
  LangOpts.EnableAccessControl = false;
  LangOpts.EnableObjCAttrRequiresFoundation = false;
  LangOpts.EnableObjCInterop = LangOpts.Target.isOSDarwin();

  // Setup the IRGen Options.
  IRGenOptions &Opts = Invocation.getIRGenOptions();
  Opts.OutputKind = options.OutputKind;
  Opts.DisableLegacyTypeInfo = options.DisableLegacyTypeInfo;

  serialization::ExtendedValidationInfo extendedInfo;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      Invocation.setUpInputForSILTool(options.InputFilename, options.ModuleName,
                                      /*alwaysSetModuleToMain*/ false,
                                      /*bePrimary*/ !options.PerformWMO, extendedInfo);
  if (!FileBufOrErr) {
    fprintf(stderr, "Error! Failed to open file: %s\n", options.InputFilename.c_str());
    exit(-1);
  }

  CompilerInstance CI;
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    llvm::errs() << InstanceSetupError << '\n';
    return 1;
  }

  llvm::vfs::OnDiskOutputBackend Backend;
  auto outFile = Backend.createFile(options.OutputFilename);
  if (!outFile) {
    CI.getDiags().diagnose(SourceLoc(), diag::error_opening_output,
                           options.OutputFilename, toString(outFile.takeError()));
    return 1;
  }
  auto closeFile = llvm::make_scope_exit([&]() {
    if (auto E = outFile->keep()) {
      CI.getDiags().diagnose(SourceLoc(), diag::error_closing_output,
                             options.OutputFilename, toString(std::move(E)));
    }
  });

  auto *mod = CI.getMainModule();
  assert(mod->getFiles().size() == 1);

  const auto &TBDOpts = Invocation.getTBDGenOptions();
  const auto &SILOpts = Invocation.getSILOptions();
  auto &SILTypes = CI.getSILTypes();
  auto moduleName = CI.getMainModule()->getName().str();
  const PrimarySpecificPaths PSPs(options.OutputFilename, options.InputFilename);

  auto getDescriptor = [&]() -> IRGenDescriptor {
    if (options.PerformWMO) {
      return IRGenDescriptor::forWholeModule(
          mod, Opts, TBDOpts, SILOpts, SILTypes,
          /*SILMod*/ nullptr, moduleName, PSPs);
    } else {
      return IRGenDescriptor::forFile(
          mod->getFiles()[0], Opts, TBDOpts, SILOpts, SILTypes,
          /*SILMod*/ nullptr, moduleName, PSPs, /*discriminator*/ "");
    }
  };

  auto &eval = CI.getASTContext().evaluator;
  auto desc = getDescriptor();
  desc.out = &outFile->getOS();
  auto generatedMod = evaluateOrFatal(eval, OptimizedIRRequest{desc});
  if (!generatedMod)
    return 1;

  return compileAndWriteLLVM(generatedMod.getModule(),
                             generatedMod.getTargetMachine(), Opts,
                             CI.getStatsReporter(), CI.getDiags(), *outFile);
}
