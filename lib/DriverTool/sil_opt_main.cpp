//===--- sil_opt_main.cpp - SIL Optimization Driver -----------------------===//
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
#include "swift/Basic/InitializeSwiftModules.h"
#include "swift/Basic/QuotedString.h"
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
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
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

llvm::Optional<bool> toOptionalBool(llvm::cl::boolOrDefault defaultable) {
  switch (defaultable) {
  case llvm::cl::BOU_TRUE:
    return true;
  case llvm::cl::BOU_FALSE:
    return false;
  case llvm::cl::BOU_UNSET:
    return llvm::None;
  }
  llvm_unreachable("Bad case for llvm::cl::boolOrDefault!");
}

enum class EnforceExclusivityMode {
  Unchecked, // static only
  Checked,   // static and dynamic
  DynamicOnly,
  None,
};
} // end anonymous namespace

namespace llvm {

inline raw_ostream &
operator<<(raw_ostream &os,
           const llvm::Optional<CopyPropagationOption> option) {
  if (option) {
    switch (*option) {
    case CopyPropagationOption::Off:
      os << "off";
      break;
    case CopyPropagationOption::RequestedPassesOnly:
      os << "requested-passes-only";
      break;
    case CopyPropagationOption::On:
      os << "on";
      break;
    }
  } else {
    os << "<none>";
  }
  return os;
}

namespace cl {
template <>
class parser<llvm::Optional<CopyPropagationOption>>
    : public basic_parser<llvm::Optional<CopyPropagationOption>> {
public:
  parser(Option &O) : basic_parser<llvm::Optional<CopyPropagationOption>>(O) {}

  // parse - Return true on error.
  bool parse(Option &O, StringRef ArgName, StringRef Arg,
             llvm::Optional<CopyPropagationOption> &Value) {
    if (Arg == "" || Arg == "true" || Arg == "TRUE" || Arg == "True" ||
        Arg == "1") {
      Value = CopyPropagationOption::On;
      return false;
    }
    if (Arg == "false" || Arg == "FALSE" || Arg == "False" || Arg == "0") {
      Value = CopyPropagationOption::Off;
      return false;
    }
    if (Arg == "requested-passes-only" || Arg == "REQUESTED-PASSES-ONLY" ||
        Arg == "Requested-Passes-Only") {
      Value = CopyPropagationOption::RequestedPassesOnly;
      return false;
    }

    return O.error("'" + Arg +
                   "' is invalid for CopyPropagationOption! Try true, false, "
                   "or requested-passes-only.");
  }

  void initialize() {}

  enum ValueExpected getValueExpectedFlagDefault() const {
    return ValueOptional;
  }

  StringRef getValueName() const override { return "CopyPropagationOption"; }

  // Instantiate the macro PRINT_OPT_DIFF of llvm_project's CommandLine.cpp at
  // Optional<CopyPropagationOption>.
  void printOptionDiff(const Option &O, llvm::Optional<CopyPropagationOption> V,
                       OptionValue<llvm::Optional<CopyPropagationOption>> D,
                       size_t GlobalWidth) const {
    size_t MaxOptWidth = 8;
    printOptionName(O, GlobalWidth);
    std::string Str;
    {
      raw_string_ostream SS(Str);
      SS << V;
    }
    outs() << "= " << Str;
    size_t NumSpaces = MaxOptWidth > Str.size() ? MaxOptWidth - Str.size() : 0;
    outs().indent(NumSpaces) << " (default:";
    if (D.hasValue())
      outs() << D.getValue();
    else
      outs() << "*no default*";
    outs() << ")\n";
  }
};
} // end namespace cl
} // end namespace llvm

struct SILOptOptions {
  llvm::cl::opt<std::string>
  InputFilename = llvm::cl::opt<std::string>(llvm::cl::desc("input file"), llvm::cl::init("-"),
                llvm::cl::Positional);

  llvm::cl::opt<std::string>
  OutputFilename = llvm::cl::opt<std::string>("o", llvm::cl::desc("output filename"));

  llvm::cl::list<std::string>
  ImportPaths = llvm::cl::list<std::string>("I", llvm::cl::desc("add a directory to the import search path"));

  llvm::cl::list<std::string>
  FrameworkPaths = llvm::cl::list<std::string>("F", llvm::cl::desc("add a directory to the framework search path"));

  llvm::cl::list<std::string>
  VFSOverlays = llvm::cl::list<std::string>("vfsoverlay", llvm::cl::desc("add a VFS overlay"));

  llvm::cl::opt<std::string>
  ModuleName = llvm::cl::opt<std::string>("module-name", llvm::cl::desc("The name of the module if processing"
                                           " a module. Necessary for processing "
                                           "stdin."));

  llvm::cl::opt<bool>
  EnableLibraryEvolution = llvm::cl::opt<bool>("enable-library-evolution",
                         llvm::cl::desc("Compile the module to export resilient "
                                        "interfaces for all public declarations by "
                                        "default"));

  llvm::cl::opt<bool>
  DisableSILOwnershipVerifier = llvm::cl::opt<bool>(
      "disable = llvm::cl::opt<bool> DisableSILOwnershipVerifier(-sil-ownership-verifier",
      llvm::cl::desc(
          "Do not verify SIL ownership invariants during SIL verification"));

  llvm::cl::opt<bool>
  EnableSILOpaqueValues = llvm::cl::opt<bool>("enable-sil-opaque-values",
                        llvm::cl::desc("Compile the module with sil-opaque-values enabled."));

  llvm::cl::opt<bool>
  EnableOSSACompleteLifetimes = llvm::cl::opt<bool>("enable-ossa-complete-lifetimes",
                        llvm::cl::desc("Compile the module with sil-opaque-values enabled."));

  llvm::cl::opt<bool>
  EnableObjCInterop = llvm::cl::opt<bool>("enable-objc-interop",
                    llvm::cl::desc("Enable Objective-C interoperability."));

  llvm::cl::opt<bool>
  DisableObjCInterop = llvm::cl::opt<bool>("disable-objc-interop",
                     llvm::cl::desc("Disable Objective-C interoperability."));

  llvm::cl::list<std::string>
  ExperimentalFeatures = llvm::cl::list<std::string>("enable-experimental-feature",
                       llvm::cl::desc("Enable the given experimental feature."));

  llvm::cl::opt<bool>
  EnableExperimentalConcurrency = llvm::cl::opt<bool>("enable-experimental-concurrency",
                     llvm::cl::desc("Enable experimental concurrency model."));

  llvm::cl::opt<llvm::cl::boolOrDefault>
  EnableLexicalLifetimes = llvm::cl::opt<llvm::cl::boolOrDefault>(
      "enable-lexical-lifetimes", llvm::cl::init(llvm::cl::BOU_UNSET),
      llvm::cl::desc("Enable lexical lifetimes. Mutually exclusive with "
                     "enable-lexical-borrow-scopes and "
                     "disable-lexical-lifetimes."));

  llvm::cl::opt<llvm::cl::boolOrDefault>
      EnableLexicalBorrowScopes = llvm::cl::opt<llvm::cl::boolOrDefault>("enable-lexical-borrow-scopes",
                                llvm::cl::init(llvm::cl::BOU_UNSET),
                                llvm::cl::desc("Enable lexical borrow scopes."));

  llvm::cl::opt<llvm::cl::boolOrDefault>
  EnableExperimentalMoveOnly = llvm::cl::opt<llvm::cl::boolOrDefault>(
      "enable-experimental-move-only", llvm::cl::init(llvm::cl::BOU_UNSET),
      llvm::cl::desc("Enable experimental move-only semantics."));

  llvm::cl::opt<bool> EnablePackMetadataStackPromotion = llvm::cl::opt<bool>(
      "enable-pack-metadata-stack-promotion", llvm::cl::init(true),
      llvm::cl::desc(
          "Whether to skip heapifying stack metadata packs when possible."));

  llvm::cl::opt<bool>
  EnableExperimentalDistributed = llvm::cl::opt<bool>("enable-experimental-distributed",
                     llvm::cl::desc("Enable experimental distributed actors."));

  llvm::cl::opt<bool>
  VerifyExclusivity = llvm::cl::opt<bool>("enable-verify-exclusivity",
                    llvm::cl::desc("Verify the access markers used to enforce exclusivity."));

  llvm::cl::opt<bool>
  EnableSpeculativeDevirtualization = llvm::cl::opt<bool>("enable-spec-devirt",
                    llvm::cl::desc("Enable Speculative Devirtualization pass."));

  llvm::cl::opt<bool>
  EnableMoveInoutStackProtection = llvm::cl::opt<bool>("enable-move-inout-stack-protector",
                    llvm::cl::desc("Enable the stack protector by moving values to temporaries."));

  llvm::cl::opt<bool>
  EnableOSSAModules = llvm::cl::opt<bool>(
      "enable-ossa-modules",
      llvm::cl::desc("Do we always serialize SIL in OSSA form? If "
                     "this is disabled we do not serialize in OSSA "
                     "form when optimizing."));

  cl::opt<EnforceExclusivityMode>
    EnforceExclusivity = cl::opt<EnforceExclusivityMode>(
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

  llvm::cl::opt<std::string>
  ResourceDir = llvm::cl::opt<std::string>("resource-dir",
      llvm::cl::desc("The directory that holds the compiler resource files"));

  llvm::cl::opt<std::string>
  SDKPath = llvm::cl::opt<std::string>("sdk", llvm::cl::desc("The path to the SDK for use with the clang "
                                "importer."),
          llvm::cl::init(""));

  llvm::cl::opt<std::string>
  Target = llvm::cl::opt<std::string>("target", llvm::cl::desc("target triple"),
         llvm::cl::init(llvm::sys::getDefaultTargetTriple()));

  // This primarily determines semantics of debug information. The compiler does
  // not directly expose a "preserve debug info mode". It is derived from the
  // optimization level. At -Onone, all debug info must be preserved. At higher
  // levels, debug info cannot change the compiler output.
  //
  // Diagnostics should be "equivalent" at all levels. For example, programs that
  // compile at -Onone should compile at -O. However, it is difficult to guarantee
  // identical diagnostic output given the changes in SIL caused by debug info
  // preservation.
  llvm::cl::opt<OptimizationMode>
    OptModeFlag = llvm::cl::opt<OptimizationMode>(
      "opt-mode", llvm::cl::desc("optimization mode"),
      llvm::cl::values(clEnumValN(OptimizationMode::NoOptimization, "none",
                                  "preserve debug info"),
                       clEnumValN(OptimizationMode::ForSize, "size",
                                  "ignore debug info, reduce size"),
                       clEnumValN(OptimizationMode::ForSpeed, "speed",
                                  "ignore debug info, reduce runtime")),
      llvm::cl::init(OptimizationMode::NotSet));

  llvm::cl::opt<IRGenDebugInfoLevel>
    IRGenDebugInfoLevelArg = llvm::cl::opt<IRGenDebugInfoLevel>(
      "irgen-debuginfo-level", llvm::cl::desc("IRGen debug info level"),
      llvm::cl::values(clEnumValN(IRGenDebugInfoLevel::None, "none",
                                  "No debug info"),
                       clEnumValN(IRGenDebugInfoLevel::LineTables, "line-tables",
                                  "Line tables only"),
                       clEnumValN(IRGenDebugInfoLevel::ASTTypes, "ast-types",
                                  "Line tables + AST type references"),
                       clEnumValN(IRGenDebugInfoLevel::DwarfTypes, "dwarf-types",
                                  "Line tables + AST type refs + Dwarf types")),
      llvm::cl::init(IRGenDebugInfoLevel::ASTTypes));

  llvm::cl::opt<OptGroup>
    OptimizationGroup = llvm::cl::opt<OptGroup>(
      llvm::cl::desc("Predefined optimization groups:"),
      llvm::cl::values(
          clEnumValN(OptGroup::Diagnostics, "diagnostics",
                     "Run diagnostic passes"),
          clEnumValN(OptGroup::Performance, "O", "Run performance passes"),
          clEnumValN(OptGroup::OnonePerformance, "Onone-performance",
                     "Run Onone perf passes"),
          clEnumValN(OptGroup::Lowering, "lowering", "Run lowering passes")),
      llvm::cl::init(OptGroup::Unknown));

  llvm::cl::list<PassKind>
  Passes = llvm::cl::list<PassKind>(llvm::cl::desc("Passes:"),
         llvm::cl::values(
  #define PASS(ID, TAG, NAME) clEnumValN(PassKind::ID, TAG, NAME),
  #include "swift/SILOptimizer/PassManager/Passes.def"
         clEnumValN(0, "", "")));

  llvm::cl::opt<bool>
  PrintStats = llvm::cl::opt<bool>("print-stats", llvm::cl::desc("Print various statistics"));

  llvm::cl::opt<bool>
  VerifyMode = llvm::cl::opt<bool>("verify",
             llvm::cl::desc("verify diagnostics against expected-"
                            "{error|warning|note} annotations"));

  llvm::cl::opt<unsigned>
  AssertConfId = llvm::cl::opt<unsigned>("assert-conf-id", llvm::cl::Hidden,
               llvm::cl::init(0));

  llvm::cl::opt<int>
  SILInlineThreshold = llvm::cl::opt<int>("sil-inline-threshold", llvm::cl::Hidden,
                     llvm::cl::init(-1));

  // Legacy option name still in use. The frontend uses -sil-verify-all.
  llvm::cl::opt<bool>
  EnableSILVerifyAll = llvm::cl::opt<bool>("enable-sil-verify-all",
                     llvm::cl::Hidden,
                     llvm::cl::init(true),
                     llvm::cl::desc("Run sil verifications after every pass."));

  llvm::cl::opt<bool>
  SILVerifyAll = llvm::cl::opt<bool>("sil-verify-all",
               llvm::cl::Hidden,
               llvm::cl::init(true),
               llvm::cl::desc("Run sil verifications after every pass."));

  llvm::cl::opt<bool>
  SILVerifyNone = llvm::cl::opt<bool>("sil-verify-none",
                llvm::cl::Hidden,
                llvm::cl::init(false),
                llvm::cl::desc("Completely disable SIL verification"));

  /// Customize the default behavior
  llvm::cl::opt<bool>
    EnableASTVerifier = llvm::cl::opt<bool>(
      "enable-ast-verifier", llvm::cl::Hidden, llvm::cl::init(false),
      llvm::cl::desc("Override the default behavior and Enable the ASTVerifier"));

  llvm::cl::opt<bool>
    DisableASTVerifier = llvm::cl::opt<bool>(
      "disable-ast-verifier", llvm::cl::Hidden, llvm::cl::init(false),
      llvm::cl::desc(
          "Override the default behavior and force disable the ASTVerifier"));

  llvm::cl::opt<bool>
  RemoveRuntimeAsserts = llvm::cl::opt<bool>("remove-runtime-asserts",
                       llvm::cl::Hidden,
                       llvm::cl::init(false),
                       llvm::cl::desc("Remove runtime assertions (cond_fail)."));

  llvm::cl::opt<bool>
  EmitVerboseSIL = llvm::cl::opt<bool>("emit-verbose-sil",
                 llvm::cl::desc("Emit locations during sil emission."));

  llvm::cl::opt<bool>
  EmitSIB = llvm::cl::opt<bool>("emit-sib", llvm::cl::desc("Emit serialized AST + SIL file(s)"));

  llvm::cl::opt<bool>
  Serialize = llvm::cl::opt<bool>("serialize", llvm::cl::desc("Emit serialized AST + SIL file(s)"));

  llvm::cl::opt<std::string>
  ModuleCachePath = llvm::cl::opt<std::string>("module-cache-path", llvm::cl::desc("Clang module cache path"));

  llvm::cl::opt<bool>
      EmitSortedSIL = llvm::cl::opt<bool>("emit-sorted-sil", llvm::cl::Hidden, llvm::cl::init(false),
                    llvm::cl::desc("Sort Functions, VTables, Globals, "
                                   "WitnessTables by name to ease diffing."));

  llvm::cl::opt<bool>
  DisableASTDump = llvm::cl::opt<bool>("sil-disable-ast-dump", llvm::cl::Hidden,
                 llvm::cl::init(false),
                 llvm::cl::desc("Do not dump AST."));

  llvm::cl::opt<bool>
  PerformWMO = llvm::cl::opt<bool>("wmo", llvm::cl::desc("Enable whole-module optimizations"));

  llvm::cl::opt<bool>
  EnableExperimentalStaticAssert = llvm::cl::opt<bool>(
      "enable-experimental-static-assert", llvm::cl::Hidden,
      llvm::cl::init(false), llvm::cl::desc("Enable experimental #assert"));

  llvm::cl::opt<bool>
  EnableExperimentalDifferentiableProgramming = llvm::cl::opt<bool>(
      "enable-experimental-differentiable-programming", llvm::cl::Hidden,
      llvm::cl::init(false),
      llvm::cl::desc("Enable experimental differentiable programming"));

  cl::opt<std::string>
    PassRemarksPassed = cl::opt<std::string>(
      "sil-remarks", cl::value_desc("pattern"),
      cl::desc(
          "Enable performed optimization remarks from passes whose name match "
          "the given regular expression"),
      cl::Hidden);

  cl::opt<std::string>
    PassRemarksMissed = cl::opt<std::string>(
      "sil-remarks-missed", cl::value_desc("pattern"),
      cl::desc("Enable missed optimization remarks from passes whose name match "
               "the given regular expression"),
      cl::Hidden);

  cl::opt<std::string>
      RemarksFilename = cl::opt<std::string>("save-optimization-record-path",
                      cl::desc("YAML output filename for pass remarks"),
                      cl::value_desc("filename"));

  cl::opt<std::string>
    RemarksPasses = cl::opt<std::string>(
      "save-optimization-record-passes",
      cl::desc("Only include passes which match a specified regular expression "
               "in the generated optimization record (by default, include all "
               "passes)"),
      cl::value_desc("regex"));

  // sil-opt doesn't have the equivalent of -save-optimization-record=<format>.
  // Instead, use -save-optimization-record-format <format>.
  cl::opt<std::string>
    RemarksFormat = cl::opt<std::string>(
      "save-optimization-record-format",
      cl::desc("The format used for serializing remarks (default: YAML)"),
      cl::value_desc("format"), cl::init("yaml"));

  llvm::cl::opt<bool>
      EnableCxxInterop = llvm::cl::opt<bool>("enable-experimental-cxx-interop",
                       llvm::cl::desc("Enable C++ interop."),
                       llvm::cl::init(false));

  llvm::cl::opt<bool>
      IgnoreAlwaysInline = llvm::cl::opt<bool>("ignore-always-inline",
                         llvm::cl::desc("Ignore [always_inline] attribute."),
                         llvm::cl::init(false));
  using CPStateOpt =
      llvm::cl::opt<llvm::Optional<CopyPropagationOption>,
                    /*ExternalStorage*/ false,
                    llvm::cl::parser<llvm::Optional<CopyPropagationOption>>>;
  CPStateOpt
  CopyPropagationState = CPStateOpt(
        "enable-copy-propagation",
        llvm::cl::desc("Whether to run the copy propagation pass: "
                       "'true', 'false', or 'requested-passes-only'."));
};

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

static void runCommandLineSelectedPasses(SILModule *Module,
                                         irgen::IRGenModule *IRGenMod,
                                         const SILOptOptions &options) {
  const SILOptions &opts = Module->getOptions();
  // If a specific pass was requested with -opt-mode=None, run the pass as a
  // mandatory pass.
  bool isMandatory = opts.OptMode == OptimizationMode::NoOptimization;
  executePassPipelinePlan(
      Module, SILPassPipelinePlan::getPassPipelineForKinds(opts, options.Passes),
      isMandatory, IRGenMod);

  if (Module->getOptions().VerifyAll)
    Module->verify();
}

namespace {
using ASTVerifierOverrideKind = LangOptions::ASTVerifierOverrideKind;
} // end anonymous namespace

static llvm::Optional<ASTVerifierOverrideKind>
getASTOverrideKind(const SILOptOptions &options) {
  assert(!(options.EnableASTVerifier && options.DisableASTVerifier) &&
         "Can only set one of EnableASTVerifier/DisableASTVerifier?!");
  if (options.EnableASTVerifier)
    return ASTVerifierOverrideKind::EnableVerifier;

  if (options.DisableASTVerifier)
    return ASTVerifierOverrideKind::DisableVerifier;

  return llvm::None;
}

int sil_opt_main(ArrayRef<const char *> argv, void *MainAddr) {
  INITIALIZE_LLVM();
  llvm::setBugReportMsg(SWIFT_CRASH_BUG_REPORT_MESSAGE  "\n");
  llvm::EnablePrettyStackTraceOnSigInfoForThisThread();

  SILOptOptions options;

  llvm::cl::ParseCommandLineOptions(argv.size(), argv.data(), "Swift SIL optimizer\n");

  if (options.PrintStats)
    llvm::EnableStatistics();

  CompilerInvocation Invocation;

  Invocation.setMainExecutablePath(
      llvm::sys::fs::getMainExecutable(argv[0], MainAddr));

  // Give the context the list of search paths to use for modules.
  Invocation.setImportSearchPaths(options.ImportPaths);
  std::vector<SearchPathOptions::FrameworkSearchPath> FramePaths;
  for (const auto &path : options.FrameworkPaths) {
    FramePaths.push_back({path, /*isSystem=*/false});
  }
  Invocation.setFrameworkSearchPaths(FramePaths);

  Invocation.setVFSOverlays(options.VFSOverlays);

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
  Invocation.getFrontendOptions().EnableLibraryEvolution
    = options.EnableLibraryEvolution;
  // Set the module cache path. If not passed in we use the default swift module
  // cache.
  Invocation.getClangImporterOptions().ModuleCachePath = options.ModuleCachePath;
  Invocation.setParseStdlib();
  Invocation.getLangOptions().DisableAvailabilityChecking = true;
  Invocation.getLangOptions().EnableAccessControl = false;
  Invocation.getLangOptions().EnableObjCAttrRequiresFoundation = false;
  Invocation.getLangOptions().EnableDeserializationSafety = false;
  if (auto overrideKind = getASTOverrideKind(options)) {
    Invocation.getLangOptions().ASTVerifierOverride = *overrideKind;
  }
  Invocation.getLangOptions().EnableExperimentalConcurrency =
    options.EnableExperimentalConcurrency;
  llvm::Optional<bool> enableExperimentalMoveOnly =
      toOptionalBool(options.EnableExperimentalMoveOnly);
  if (enableExperimentalMoveOnly && *enableExperimentalMoveOnly) {
    // FIXME: drop addition of Feature::MoveOnly once its queries are gone.
    Invocation.getLangOptions().Features.insert(Feature::MoveOnly);
    Invocation.getLangOptions().Features.insert(Feature::NoImplicitCopy);
    Invocation.getLangOptions().Features.insert(
        Feature::OldOwnershipOperatorSpellings);
  }
  for (auto &featureName : options.ExperimentalFeatures) {
    if (auto feature = getExperimentalFeature(featureName)) {
      Invocation.getLangOptions().Features.insert(*feature);
    } else {
      llvm::errs() << "error: unknown feature "
                   << QuotedString(featureName) << "\n";
      exit(-1);
    }
  }

  Invocation.getLangOptions().EnableObjCInterop =
    options.EnableObjCInterop ? true :
    options.DisableObjCInterop ? false : llvm::Triple(options.Target).isOSDarwin();

  Invocation.getLangOptions().Features.insert(Feature::LayoutPrespecialization);

  Invocation.getLangOptions().OptimizationRemarkPassedPattern =
      createOptRemarkRegex(options.PassRemarksPassed);
  Invocation.getLangOptions().OptimizationRemarkMissedPattern =
      createOptRemarkRegex(options.PassRemarksMissed);

  if (options.EnableExperimentalStaticAssert)
    Invocation.getLangOptions().Features.insert(Feature::StaticAssert);

  if (options.EnableExperimentalDifferentiableProgramming) {
    Invocation.getLangOptions().Features.insert(
        Feature::DifferentiableProgramming);
  }

  Invocation.getLangOptions().EnableCXXInterop = options.EnableCxxInterop;

  Invocation.getDiagnosticOptions().VerifyMode =
    options.VerifyMode ? DiagnosticOptions::Verify : DiagnosticOptions::NoVerify;

  // Setup the SIL Options.
  SILOptions &SILOpts = Invocation.getSILOptions();
  SILOpts.InlineThreshold = options.SILInlineThreshold;
  SILOpts.VerifyAll = options.SILVerifyAll || options.EnableSILVerifyAll;
  SILOpts.VerifyNone = options.SILVerifyNone;
  SILOpts.RemoveRuntimeAsserts = options.RemoveRuntimeAsserts;
  SILOpts.AssertConfig = options.AssertConfId;
  SILOpts.VerifySILOwnership = !options.DisableSILOwnershipVerifier;
  SILOpts.OptRecordFile = options.RemarksFilename;
  SILOpts.OptRecordPasses = options.RemarksPasses;
  SILOpts.checkSILModuleLeaks = true;
  SILOpts.EnableStackProtection = true;
  SILOpts.EnableMoveInoutStackProtection = options.EnableMoveInoutStackProtection;

  SILOpts.VerifyExclusivity = options.VerifyExclusivity;
  if (options.EnforceExclusivity.getNumOccurrences() != 0) {
    switch (options.EnforceExclusivity) {
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
  SILOpts.EmitVerboseSIL |= options.EmitVerboseSIL;
  SILOpts.EmitSortedSIL |= options.EmitSortedSIL;

  SILOpts.EnableSpeculativeDevirtualization = options.EnableSpeculativeDevirtualization;
  SILOpts.IgnoreAlwaysInline = options.IgnoreAlwaysInline;
  SILOpts.EnableOSSAModules = options.EnableOSSAModules;
  SILOpts.EnableSILOpaqueValues = options.EnableSILOpaqueValues;
  SILOpts.OSSACompleteLifetimes = options.EnableOSSACompleteLifetimes;

  if (options.CopyPropagationState) {
    SILOpts.CopyPropagation = *options.CopyPropagationState;
  }

  // Unless overridden below, enabling copy propagation means enabling lexical
  // lifetimes.
  if (SILOpts.CopyPropagation == CopyPropagationOption::On)
    SILOpts.LexicalLifetimes = LexicalLifetimesOption::On;

  // Unless overridden below, disable copy propagation means disabling lexical
  // lifetimes.
  if (SILOpts.CopyPropagation == CopyPropagationOption::Off)
    SILOpts.LexicalLifetimes = LexicalLifetimesOption::DiagnosticMarkersOnly;

  llvm::Optional<bool> enableLexicalLifetimes =
      toOptionalBool(options.EnableLexicalLifetimes);
  llvm::Optional<bool> enableLexicalBorrowScopes =
      toOptionalBool(options.EnableLexicalBorrowScopes);

  // Enable lexical lifetimes if it is set or if experimental move only is
  // enabled. This is because move only depends on lexical lifetimes being
  // enabled and it saved some typing ; ).
  bool specifiedLexicalLifetimesEnabled =
      enableExperimentalMoveOnly && *enableExperimentalMoveOnly &&
      enableLexicalLifetimes && *enableLexicalLifetimes;
  if (specifiedLexicalLifetimesEnabled && enableLexicalBorrowScopes &&
      !*enableLexicalBorrowScopes) {
    fprintf(
        stderr,
        "Error! Cannot specify both -enable-lexical-borrow-scopes=false and "
        "either -enable-lexical-lifetimes or -enable-experimental-move-only.");
    exit(-1);
  }
  if (enableLexicalLifetimes)
    SILOpts.LexicalLifetimes =
        *enableLexicalLifetimes ? LexicalLifetimesOption::On
                                : LexicalLifetimesOption::DiagnosticMarkersOnly;
  if (enableLexicalBorrowScopes)
    SILOpts.LexicalLifetimes =
        *enableLexicalBorrowScopes
            ? LexicalLifetimesOption::DiagnosticMarkersOnly
            : LexicalLifetimesOption::Off;

  SILOpts.EnablePackMetadataStackPromotion =
      options.EnablePackMetadataStackPromotion;

  if (options.OptModeFlag == OptimizationMode::NotSet) {
    if (options.OptimizationGroup == OptGroup::Diagnostics)
      SILOpts.OptMode = OptimizationMode::NoOptimization;
    else
      SILOpts.OptMode = OptimizationMode::ForSpeed;
  } else {
    SILOpts.OptMode = options.OptModeFlag;
  }

  auto &IRGenOpts = Invocation.getIRGenOptions();
  if (options.OptModeFlag == OptimizationMode::NotSet) {
    if (options.OptimizationGroup == OptGroup::Diagnostics)
      IRGenOpts.OptMode = OptimizationMode::NoOptimization;
    else
      IRGenOpts.OptMode = OptimizationMode::ForSpeed;
  } else {
    IRGenOpts.OptMode = options.OptModeFlag;
  }
  IRGenOpts.DebugInfoLevel = options.IRGenDebugInfoLevelArg;

  // Note: SILOpts, LangOpts, and IRGenOpts must be set before the
  // CompilerInstance is initializer below based on Invocation.

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

  if (options.VerifyMode)
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
    if (options.VerifyMode && !diagnosticsError) {
      return 0;
    }
    return retValue ? retValue : diagnosticsError;
  };

  std::string InstanceSetupError;
  if (CI.setup(Invocation, InstanceSetupError)) {
    llvm::errs() << InstanceSetupError << '\n';
    return finishDiagProcessing(1);
  }

  CI.performSema();

  // If parsing produced an error, don't run any passes.
  bool HadError = CI.getASTContext().hadError();
  if (HadError)
    return finishDiagProcessing(1);

  auto *mod = CI.getMainModule();
  assert(mod->getFiles().size() == 1);

  std::unique_ptr<SILModule> SILMod;
  if (options.PerformWMO) {
    SILMod = performASTLowering(mod, CI.getSILTypes(), CI.getSILOptions());
  } else {
    SILMod = performASTLowering(*mod->getFiles()[0], CI.getSILTypes(),
                                CI.getSILOptions());
  }
  SILMod->setSerializeSILAction([]{});

  if (!options.RemarksFilename.empty()) {
    llvm::Expected<llvm::remarks::Format> formatOrErr =
        llvm::remarks::parseFormat(options.RemarksFormat);
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

  switch (options.OptimizationGroup) {
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
        Invocation.getMainInputFilenameForDebugInfoForAtMostOnePrimary(), "",
        IRGenOpts);
    runCommandLineSelectedPasses(SILMod.get(), T.second, options);
    irgen::deleteIRGenModule(T);
    break;
  }
  }

  if (options.EmitSIB || options.Serialize) {
    llvm::SmallString<128> OutputFile;
    if (options.OutputFilename.size()) {
      OutputFile = options.OutputFilename;
    } else if (options.ModuleName.size()) {
      OutputFile = options.ModuleName;
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    } else {
      OutputFile = CI.getMainModule()->getName().str();
      llvm::sys::path::replace_extension(
          OutputFile, file_types::getExtension(file_types::TY_SIB));
    }

    SerializationOptions serializationOpts;
    serializationOpts.OutputPath = OutputFile;
    serializationOpts.SerializeAllSIL = options.EmitSIB;
    serializationOpts.IsSIB = options.EmitSIB;

    symbolgraphgen::SymbolGraphOptions symbolGraphOptions;

    serialize(CI.getMainModule(), serializationOpts, symbolGraphOptions, SILMod.get());
  } else {
    const StringRef OutputFile = options.OutputFilename.size() ?
                                   StringRef(options.OutputFilename) : "-";
    auto SILOpts = SILOptions();
    SILOpts.EmitVerboseSIL = options.EmitVerboseSIL;
    SILOpts.EmitSortedSIL = options.EmitSortedSIL;
    if (OutputFile == "-") {
      SILMod->print(llvm::outs(), CI.getMainModule(), SILOpts, !options.DisableASTDump);
    } else {
      std::error_code EC;
      llvm::raw_fd_ostream OS(OutputFile, EC, llvm::sys::fs::OF_None);
      if (EC) {
        llvm::errs() << "while opening '" << OutputFile << "': "
                     << EC.message() << '\n';
        return finishDiagProcessing(1);
      }
      SILMod->print(OS, CI.getMainModule(), SILOpts, !options.DisableASTDump);
    }
  }

  HadError |= CI.getASTContext().hadError();

  if (options.VerifyMode) {
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
