//===--- SILOpt.cpp - SIL Optimization Driver -----------------------------===//
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
//
// This is a tool for reading sil files and running sil passes on them. The
// targeted usecase is debugging and testing SIL passes.
//
//===----------------------------------------------------------------------===//

#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/SILOptions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/InitializeLibSwift.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/SIL/SILRemarkStreamer.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/SerializedSILLoader.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/IRGen/IRGenPublic.h"
#include "swift/IRGen/IRGenSILPasses.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/ManagedStatic.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/YAMLTraits.h"
#include <cstdio>
using namespace swift;

namespace cl = llvm::cl;

namespace {

enum class OptGroup {
  Unknown,
  Diagnostics,
  OnonePerformance,
  Performance,
  Lowering
};

} // end anonymous namespace

static llvm::cl::opt<std::string>
InputFilename(llvm::cl::desc("input file"), llvm::cl::init("-"),
              llvm::cl::Positional);

static llvm::cl::opt<std::string>
OutputFilename("o", llvm::cl::desc("output filename"));

static llvm::cl::list<std::string>
ImportPaths("I", llvm::cl::desc("add a directory to the import search path"));

static llvm::cl::list<std::string>
FrameworkPaths("F", llvm::cl::desc("add a directory to the framework search path"));

static llvm::cl::opt<std::string>
ModuleName("module-name", llvm::cl::desc("The name of the module if processing"
                                         " a module. Necessary for processing "
                                         "stdin."));

static llvm::cl::opt<bool>
EnableLibraryEvolution("enable-library-evolution",
                       llvm::cl::desc("Compile the module to export resilient "
                                      "interfaces for all public declarations by "
                                      "default"));

static llvm::cl::opt<bool> DisableSILOwnershipVerifier(
    "disable-sil-ownership-verifier",
    llvm::cl::desc(
        "Do not verify SIL ownership invariants during SIL verification"));

static llvm::cl::opt<bool>
EnableSILOpaqueValues("enable-sil-opaque-values",
                      llvm::cl::desc("Compile the module with sil-opaque-values enabled."));

static llvm::cl::opt<bool>
EnableObjCInterop("enable-objc-interop",
                  llvm::cl::desc("Enable Objective-C interoperability."));

static llvm::cl::opt<bool>
DisableObjCInterop("disable-objc-interop",
                   llvm::cl::desc("Disable Objective-C interoperability."));

static llvm::cl::opt<bool>
EnableExperimentalConcurrency("enable-experimental-concurrency",
                   llvm::cl::desc("Enable experimental concurrency model."));

static llvm::cl::opt<bool>
EnableExperimentalDistributed("enable-experimental-distributed",
                   llvm::cl::desc("Enable experimental distributed actors."));

static llvm::cl::opt<bool>
VerifyExclusivity("enable-verify-exclusivity",
                  llvm::cl::desc("Verify the access markers used to enforce exclusivity."));

static llvm::cl::opt<bool>
EnableSpeculativeDevirtualization("enable-spec-devirt",
                  llvm::cl::desc("Enable Speculative Devirtualization pass."));

static llvm::cl::opt<bool> EnableOSSAModules(
    "enable-ossa-modules",
    llvm::cl::desc("Do we always serialize SIL in OSSA form? If "
                   "this is disabled we do not serialize in OSSA "
                   "form when optimizing."));

namespace {
enum class EnforceExclusivityMode {
  Unchecked, // static only
  Checked,   // static and dynamic
  DynamicOnly,
  None,
};
} // end anonymous namespace

static cl::opt<EnforceExclusivityMode> EnforceExclusivity(
  "enforce-exclusivity", cl::desc("Enforce law of exclusivity "
                                  "(and support memory access markers)."),
    cl::init(EnforceExclusivityMode::Checked),
    cl::values(clEnumValN(EnforceExclusivityMode::Unchecked, "unchecked",
                          "Static checking only."),
               clEnumValN(EnforceExclusivityMode::Checked, "checked",
                          "Static and dynamic checking."),
               clEnumValN(EnforceExclusivityMode::DynamicOnly, "dynamic-only",
                          "Dynamic checking only."),
               clEnumValN(EnforceExclusivityMode::None, "none",
                          "No exclusivity checking.")));

static llvm::cl::opt<std::string>
ResourceDir("resource-dir",
    llvm::cl::desc("The directory that holds the compiler resource files"));

static llvm::cl::opt<std::string>
SDKPath("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                              "importer."),
        llvm::cl::init(""));

static llvm::cl::opt<std::string>
Target("target", llvm::cl::desc("target triple"),
       llvm::cl::init(llvm::sys::getDefaultTargetTriple()));

static llvm::cl::opt<OptGroup> OptimizationGroup(
    llvm::cl::desc("Predefined optimization groups:"),
    llvm::cl::values(
        clEnumValN(OptGroup::Diagnostics, "diagnostics",
                   "Run diagnostic passes"),
        clEnumValN(OptGroup::Performance, "O", "Run performance passes"),
        clEnumValN(OptGroup::OnonePerformance, "Onone-performance",
                   "Run Onone perf passes"),
        clEnumValN(OptGroup::Lowering, "lowering", "Run lowering passes")),
    llvm::cl::init(OptGroup::Unknown));

static llvm::cl::list<PassKind>
Passes(llvm::cl::desc("Passes:"),
       llvm::cl::values(
#define PASS(ID, TAG, NAME) clEnumValN(PassKind::ID, TAG, NAME),
#include "swift/SILOptimizer/PassManager/Passes.def"
       clEnumValN(0, "", "")));

static llvm::cl::opt<bool>
PrintStats("print-stats", llvm::cl::desc("Print various statistics"));

static llvm::cl::opt<bool>
VerifyMode("verify",
           llvm::cl::desc("verify diagnostics against expected-"
                          "{error|warning|note} annotations"));

static llvm::cl::opt<unsigned>
AssertConfId("assert-conf-id", llvm::cl::Hidden,
             llvm::cl::init(0));

static llvm::cl::opt<int>
SILInlineThreshold("sil-inline-threshold", llvm::cl::Hidden,
                   llvm::cl::init(-1));

// Legacy option name still in use. The frontend uses -sil-verify-all.
static llvm::cl::opt<bool>
EnableSILVerifyAll("enable-sil-verify-all",
                   llvm::cl::Hidden,
                   llvm::cl::init(true),
                   llvm::cl::desc("Run sil verifications after every pass."));

static llvm::cl::opt<bool>
SILVerifyAll("sil-verify-all",
             llvm::cl::Hidden,
             llvm::cl::init(true),
             llvm::cl::desc("Run sil verifications after every pass."));

static llvm::cl::opt<bool>
SILVerifyNone("sil-verify-none",
              llvm::cl::Hidden,
              llvm::cl::init(false),
              llvm::cl::desc("Completely disable SIL verification"));

/// Customize the default behavior
static llvm::cl::opt<bool> EnableASTVerifier(
    "enable-ast-verifier", llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc("Override the default behavior and Enable the ASTVerifier"));

static llvm::cl::opt<bool> DisableASTVerifier(
    "disable-ast-verifier", llvm::cl::Hidden, llvm::cl::init(false),
    llvm::cl::desc(
        "Override the default behavior and force disable the ASTVerifier"));

static llvm::cl::opt<bool>
RemoveRuntimeAsserts("remove-runtime-asserts",
                     llvm::cl::Hidden,
                     llvm::cl::init(false),
                     llvm::cl::desc("Remove runtime assertions (cond_fail)."));

static llvm::cl::opt<bool>
EmitVerboseSIL("emit-verbose-sil",
               llvm::cl::desc("Emit locations during sil emission."));

static llvm::cl::opt<bool>
EmitSIB("emit-sib", llvm::cl::desc("Emit serialized AST + SIL file(s)"));

static llvm::cl::opt<std::string>
ModuleCachePath("module-cache-path", llvm::cl::desc("Clang module cache path"));

static llvm::cl::opt<bool>
    EmitSortedSIL("emit-sorted-sil", llvm::cl::Hidden, llvm::cl::init(false),
                  llvm::cl::desc("Sort Functions, VTables, Globals, "
                                 "WitnessTables by name to ease diffing."));

static llvm::cl::opt<bool>
DisableASTDump("sil-disable-ast-dump", llvm::cl::Hidden,
               llvm::cl::init(false),
               llvm::cl::desc("Do not dump AST."));

static llvm::cl::opt<bool>
PerformWMO("wmo", llvm::cl::desc("Enable whole-module optimizations"));

static llvm::cl::opt<bool>
EnableExperimentalStaticAssert(
    "enable-experimental-static-assert", llvm::cl::Hidden,
    llvm::cl::init(false), llvm::cl::desc("Enable experimental #assert"));

static llvm::cl::opt<bool> EnableExperimentalDifferentiableProgramming(
    "enable-experimental-differentiable-programming", llvm::cl::Hidden,
    llvm::cl::init(false),
    llvm::cl::desc("Enable experimental differentiable programming"));

/// Regular expression corresponding to the value given in one of the
/// -pass-remarks* command line flags. Passes whose name matches this regexp
/// will emit a diagnostic.
static std::shared_ptr<llvm::Regex> createOptRemarkRegex(StringRef Val) {
  std::shared_ptr<llvm::Regex> Pattern = std::make_shared<llvm::Regex>(Val);
  if (!Val.empty()) {
    std::string RegexError;
    if (!Pattern->isValid(RegexError))
      llvm::report_fatal_error("Invalid regular expression '" + Val +
                                   "' in -sil-remarks: " + RegexError,
                               false);
  }
  return Pattern;
}

static cl::opt<std::string> PassRemarksPassed(
    "sil-remarks", cl::value_desc("pattern"),
    cl::desc(
        "Enable performed optimization remarks from passes whose name match "
        "the given regular expression"),
    cl::Hidden);

static cl::opt<std::string> PassRemarksMissed(
    "sil-remarks-missed", cl::value_desc("pattern"),
    cl::desc("Enable missed optimization remarks from passes whose name match "
             "the given regular expression"),
    cl::Hidden);

static cl::opt<std::string>
    RemarksFilename("save-optimization-record-path",
                    cl::desc("YAML output filename for pass remarks"),
                    cl::value_desc("filename"));

static cl::opt<std::string> RemarksPasses(
    "save-optimization-record-passes",
    cl::desc("Only include passes which match a specified regular expression "
             "in the generated optimization record (by default, include all "
             "passes)"),
    cl::value_desc("regex"));

// sil-opt doesn't have the equivalent of -save-optimization-record=<format>.
// Instead, use -save-optimization-record-format <format>.
static cl::opt<std::string> RemarksFormat(
    "save-optimization-record-format",
    cl::desc("The format used for serializing remarks (default: YAML)"),
    cl::value_desc("format"), cl::init("yaml"));

static llvm::cl::opt<bool>
    EnableCxxInterop("enable-cxx-interop",
                     llvm::cl::desc("Enable C++ interop."),
                     llvm::cl::init(false));

static llvm::cl::opt<bool>
    IgnoreAlwaysInline("ignore-always-inline",
                       llvm::cl::desc("Ignore [always_inline] attribute."),
                       llvm::cl::init(false));

static void runCommandLineSelectedPasses(SILModule *Module,
                                         irgen::IRGenModule *IRGenMod) {
  auto &opts = Module->getOptions();
  executePassPipelinePlan(
      Module, SILPassPipelinePlan::getPassPipelineForKinds(opts, Passes),
      /*isMandatory*/ false, IRGenMod);

  if (Module->getOptions().VerifyAll)
    Module->verify();
}

namespace {
using ASTVerifierOverrideKind = LangOptions::ASTVerifierOverrideKind;
} // end anonymous namespace

static Optional<ASTVerifierOverrideKind> getASTOverrideKind() {
  assert(!(EnableASTVerifier && DisableASTVerifier) &&
         "Can only set one of EnableASTVerifier/DisableASTVerifier?!");
  if (EnableASTVerifier)
    return ASTVerifierOverrideKind::EnableVerifier;

  if (DisableASTVerifier)
    return ASTVerifierOverrideKind::DisableVerifier;

  return None;
}

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// getMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement getMainExecutable
// without being given the address of a function in the main executable).
void anchorForGetMainExecutable() {}

int main(int argc, char **argv) {
  PROGRAM_START(argc, argv);
  INITIALIZE_LLVM();
  llvm::setBugReportMsg(SWIFT_CRASH_BUG_REPORT_MESSAGE  "\n");
  llvm::EnablePrettyStackTraceOnSigInfoForThisThread();

  llvm::cl::ParseCommandLineOptions(argc, argv, "Swift SIL optimizer\n");

  initializeLibSwift();

  if (PrintStats)
    llvm::EnableStatistics();

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(
      llvm::sys::fs::getMainExecutable(argv[0],
          reinterpret_cast<void *>(&anchorForGetMainExecutable)));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(ImportPaths);
  std::vector<SearchPathOptions::FrameworkSearchPath> FramePaths;
  for (const auto &path : FrameworkPaths) {
    FramePaths.push_back({path, /*isSystem=*/false});
  }
  Invocation.setFrameworkSearchPaths(FramePaths);
  // Set the SDK path and target if given.
  if (SDKPath.getNumOccurrences() == 0) {
    const char *SDKROOT = getenv("SDKROOT");
    if (SDKROOT)
      SDKPath = SDKROOT;
  }
  if (!SDKPath.empty())
    Invocation.setSDKPath(SDKPath);
  if (!Target.empty())
    Invocation.setTargetTriple(Target);
  if (!ResourceDir.empty())
    Invocation.setRuntimeResourcePath(ResourceDir);
  Invocation.getFrontendOptions().EnableLibraryEvolution
    = EnableLibraryEvolution;
  // Set the module cache path. If not passed in we use the default swift module
  // cache.
  Invocation.getClangImporterOptions().ModuleCachePath = ModuleCachePath;
  Invocation.setParseStdlib();
  Invocation.getLangOptions().DisableAvailabilityChecking = true;
  Invocation.getLangOptions().EnableAccessControl = false;
  Invocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;
  if (auto overrideKind = getASTOverrideKind()) {
    Invocation.getLangOptions().ASTVerifierOverride = *overrideKind;
  }
  Invocation.getLangOptions().EnableExperimentalConcurrency =
    EnableExperimentalConcurrency;

  Invocation.getLangOptions().EnableObjCInterop =
    EnableObjCInterop ? true :
    DisableObjCInterop ? false : llvm::Triple(Target).isOSDarwin();

  Invocation.getLangOptions().EnableSILOpaqueValues = EnableSILOpaqueValues;

  Invocation.getLangOptions().OptimizationRemarkPassedPattern =
      createOptRemarkRegex(PassRemarksPassed);
  Invocation.getLangOptions().OptimizationRemarkMissedPattern =
      createOptRemarkRegex(PassRemarksMissed);

  Invocation.getLangOptions().EnableExperimentalStaticAssert =
      EnableExperimentalStaticAssert;

  Invocation.getLangOptions().EnableExperimentalDifferentiableProgramming =
      EnableExperimentalDifferentiableProgramming;

  Invocation.getLangOptions().EnableCXXInterop = EnableCxxInterop;

  Invocation.getDiagnosticOptions().VerifyMode =
      VerifyMode ? DiagnosticOptions::Verify : DiagnosticOptions::NoVerify;

  // Setup the SIL Options.
  SILOptions &SILOpts = Invocation.getSILOptions();
  SILOpts.InlineThreshold = SILInlineThreshold;
  SILOpts.VerifyAll = SILVerifyAll || EnableSILVerifyAll;
  SILOpts.VerifyNone = SILVerifyNone;
  SILOpts.RemoveRuntimeAsserts = RemoveRuntimeAsserts;
  SILOpts.AssertConfig = AssertConfId;
  if (OptimizationGroup != OptGroup::Diagnostics)
    SILOpts.OptMode = OptimizationMode::ForSpeed;
  SILOpts.VerifySILOwnership = !DisableSILOwnershipVerifier;
  SILOpts.OptRecordFile = RemarksFilename;
  SILOpts.OptRecordPasses = RemarksPasses;

  SILOpts.VerifyExclusivity = VerifyExclusivity;
  if (EnforceExclusivity.getNumOccurrences() != 0) {
    switch (EnforceExclusivity) {
    case EnforceExclusivityMode::Unchecked:
      // This option is analogous to the -Ounchecked optimization setting.
      // It will disable dynamic checking but still diagnose statically.
      SILOpts.EnforceExclusivityStatic = true;
      SILOpts.EnforceExclusivityDynamic = false;
      break;
    case EnforceExclusivityMode::Checked:
      SILOpts.EnforceExclusivityStatic = true;
      SILOpts.EnforceExclusivityDynamic = true;
      break;
    case EnforceExclusivityMode::DynamicOnly:
      // This option is intended for staging purposes. The intent is that
      // it will eventually be removed.
      SILOpts.EnforceExclusivityStatic = false;
      SILOpts.EnforceExclusivityDynamic = true;
      break;
    case EnforceExclusivityMode::None:
      // This option is for staging purposes.
      SILOpts.EnforceExclusivityStatic = false;
      SILOpts.EnforceExclusivityDynamic = false;
      break;
    }
  }
  SILOpts.EmitVerboseSIL |= EmitVerboseSIL;
  SILOpts.EmitSortedSIL |= EmitSortedSIL;

  SILOpts.EnableSpeculativeDevirtualization = EnableSpeculativeDevirtualization;
  SILOpts.IgnoreAlwaysInline = IgnoreAlwaysInline;
  SILOpts.EnableOSSAModules = EnableOSSAModules;

  serialization::ExtendedValidationInfo extendedInfo;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      Invocation.setUpInputForSILTool(InputFilename, ModuleName,
                                      /*alwaysSetModuleToMain*/ false,
                                      /*bePrimary*/ !PerformWMO, extendedInfo);
  if (!FileBufOrErr) {
    fprintf(stderr, "Error! Failed to open file: %s\n", InputFilename.c_str());
    exit(-1);
  }

  CompilerInstance CI;
  PrintingDiagnosticConsumer PrintDiags;
  CI.addDiagnosticConsumer(&PrintDiags);

  if (VerifyMode)
    PrintDiags.setSuppressOutput(true);

  struct FinishDiagProcessingCheckRAII {
    bool CalledFinishDiagProcessing = false;
    ~FinishDiagProcessingCheckRAII() {
      assert(CalledFinishDiagProcessing &&
             "returned from the function "
             "without calling finishDiagProcessing");
    }
  } FinishDiagProcessingCheckRAII;

  auto finishDiagProcessing = [&](int retValue) -> int {
    FinishDiagProcessingCheckRAII.CalledFinishDiagProcessing = true;
    PrintDiags.setSuppressOutput(false);
    bool diagnosticsError = CI.getDiags().finishProcessing();
    // If the verifier is enabled and did not encounter any verification errors,
    // return 0 even if the compile failed. This behavior isn't ideal, but large
    // parts of the test suite are reliant on it.
    if (VerifyMode && !diagnosticsError) {
      return 0;
    }
    return retValue ? retValue : diagnosticsError;
  };

  if (CI.setup(Invocation))
    return finishDiagProcessing(1);

  CI.performSema();

  // If parsing produced an error, don't run any passes.
  bool HadError = CI.getASTContext().hadError();
  if (HadError)
    return finishDiagProcessing(1);

  auto *mod = CI.getMainModule();
  assert(mod->getFiles().size() == 1);

  std::unique_ptr<SILModule> SILMod;
  if (PerformWMO) {
    SILMod = performASTLowering(mod, CI.getSILTypes(), CI.getSILOptions());
  } else {
    SILMod = performASTLowering(*mod->getFiles()[0], CI.getSILTypes(),
                                CI.getSILOptions());
  }
  SILMod->setSerializeSILAction([]{});

  if (!RemarksFilename.empty()) {
    llvm::Expected<llvm::remarks::Format> formatOrErr =
        llvm::remarks::parseFormat(RemarksFormat);
    if (llvm::Error E = formatOrErr.takeError()) {
      CI.getDiags().diagnose(SourceLoc(),
                             diag::error_creating_remark_serializer,
                             toString(std::move(E)));
      HadError = true;
      SILOpts.OptRecordFormat = llvm::remarks::Format::YAML;
    } else {
      SILOpts.OptRecordFormat = *formatOrErr;
    }

    SILMod->installSILRemarkStreamer();
  }

  switch (OptimizationGroup) {
  case OptGroup::Diagnostics:
    runSILDiagnosticPasses(*SILMod.get());
    break;
  case OptGroup::Performance:
    runSILOptimizationPasses(*SILMod.get());
    break;
  case OptGroup::Lowering:
    runSILLoweringPasses(*SILMod.get());
    break;
  case OptGroup::OnonePerformance:
    runSILPassesForOnone(*SILMod.get());
    break;
  case OptGroup::Unknown: {
    auto T = irgen::createIRGenModule(
        SILMod.get(), Invocation.getOutputFilenameForAtMostOnePrimary(),
        Invocation.getMainInputFilenameForDebugInfoForAtMostOnePrimary(), "");
    runCommandLineSelectedPasses(SILMod.get(), T.second);
    irgen::deleteIRGenModule(T);
    break;
  }
  }

  if (EmitSIB) {
    llvm::SmallString<128> OutputFile;
    if (OutputFilename.size()) {
      OutputFile = OutputFilename;
    } else if (ModuleName.size()) {
      OutputFile = ModuleName;
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    } else {
      OutputFile = CI.getMainModule()->getName().str();
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    }

    SerializationOptions serializationOpts;
    serializationOpts.OutputPath = OutputFile.c_str();
    serializationOpts.SerializeAllSIL = true;
    serializationOpts.IsSIB = true;

    serialize(CI.getMainModule(), serializationOpts, SILMod.get());
  } else {
    const StringRef OutputFile = OutputFilename.size() ?
                                   StringRef(OutputFilename) : "-";
    auto SILOpts = SILOptions();
    SILOpts.EmitVerboseSIL = EmitVerboseSIL;
    SILOpts.EmitSortedSIL = EmitSortedSIL;
    if (OutputFile == "-") {
      SILMod->print(llvm::outs(), CI.getMainModule(), SILOpts, !DisableASTDump);
    } else {
      std::error_code EC;
      llvm::raw_fd_ostream OS(OutputFile, EC, llvm::sys::fs::F_None);
      if (EC) {
        llvm::errs() << "while opening '" << OutputFile << "': "
                     << EC.message() << '\n';
        return finishDiagProcessing(1);
      }
      SILMod->print(OS, CI.getMainModule(), SILOpts, !DisableASTDump);
    }
  }

  HadError |= CI.getASTContext().hadError();

  if (VerifyMode) {
    DiagnosticEngine &diags = CI.getDiags();
    if (diags.hasFatalErrorOccurred() &&
        !Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
      diags.resetHadAnyError();
      diags.diagnose(SourceLoc(), diag::verify_encountered_fatal);
      HadError = true;
    }
  }

  return finishDiagProcessing(HadError);
}
