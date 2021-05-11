//===--- CompilerInvocation.cpp - CompilerInvocation methods --------------===//
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

#include "swift/Frontend/Frontend.h"

#include "ArgsToFrontendOptionsConverter.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Platform.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "swift/Strings.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"

using namespace swift;
using namespace llvm::opt;

/// The path for Swift libraries in the OS on Darwin.
#define DARWIN_OS_LIBRARY_PATH "/usr/lib/swift"

static constexpr const char *const localeCodes[] = {
#define SUPPORTED_LOCALE(Code, Language) #Code,
#include "swift/AST/LocalizationLanguages.def"
};

swift::CompilerInvocation::CompilerInvocation() {
  setTargetTriple(llvm::sys::getDefaultTargetTriple());
}

void CompilerInvocation::computeRuntimeResourcePathFromExecutablePath(
    StringRef mainExecutablePath, bool shared,
    llvm::SmallVectorImpl<char> &runtimeResourcePath) {
  runtimeResourcePath.append(mainExecutablePath.begin(),
                             mainExecutablePath.end());

  llvm::sys::path::remove_filename(runtimeResourcePath); // Remove /swift
  llvm::sys::path::remove_filename(runtimeResourcePath); // Remove /bin
  appendSwiftLibDir(runtimeResourcePath, shared);
}

void CompilerInvocation::appendSwiftLibDir(llvm::SmallVectorImpl<char> &path,
                                      bool shared) {
  llvm::sys::path::append(path, "lib", shared ? "swift" : "swift_static");
}

void CompilerInvocation::setMainExecutablePath(StringRef Path) {
  FrontendOpts.MainExecutablePath = Path.str();
  llvm::SmallString<128> LibPath;
  computeRuntimeResourcePathFromExecutablePath(
      Path, FrontendOpts.UseSharedResourceFolder, LibPath);
  setRuntimeResourcePath(LibPath.str());

  llvm::SmallString<128> DiagnosticDocsPath(Path);
  llvm::sys::path::remove_filename(DiagnosticDocsPath); // Remove /swift
  llvm::sys::path::remove_filename(DiagnosticDocsPath); // Remove /bin
  llvm::sys::path::append(DiagnosticDocsPath, "share", "doc", "swift",
                          "diagnostics");
  DiagnosticOpts.DiagnosticDocumentationPath = std::string(DiagnosticDocsPath.str());

  // Compute the path to the diagnostic translations in the toolchain/build.
  llvm::SmallString<128> DiagnosticMessagesDir(Path);
  llvm::sys::path::remove_filename(DiagnosticMessagesDir); // Remove /swift
  llvm::sys::path::remove_filename(DiagnosticMessagesDir); // Remove /bin
  llvm::sys::path::append(DiagnosticMessagesDir, "share", "swift", "diagnostics");
  DiagnosticOpts.LocalizationPath = std::string(DiagnosticMessagesDir.str());
}

static std::string
getVersionedPrebuiltModulePath(Optional<llvm::VersionTuple> sdkVer,
                               StringRef defaultPrebuiltPath) {
  if (!sdkVer.hasValue())
    return defaultPrebuiltPath.str();
  std::string versionStr = sdkVer->getAsString();
  StringRef vs = versionStr;
  do {
    SmallString<64> pathWithSDKVer = defaultPrebuiltPath;
    llvm::sys::path::append(pathWithSDKVer, vs);
    if (llvm::sys::fs::exists(pathWithSDKVer)) {
      return pathWithSDKVer.str().str();
    } else if (vs.endswith(".0")) {
      vs = vs.substr(0, vs.size() - 2);
    } else {
      return defaultPrebuiltPath.str();
    }
  } while(true);
}

std::string CompilerInvocation::computePrebuiltCachePath(
    StringRef RuntimeResourcePath, llvm::Triple target,
    Optional<llvm::VersionTuple> sdkVer) {
  SmallString<64> defaultPrebuiltPath{RuntimeResourcePath};
  StringRef platform;
  if (tripleIsMacCatalystEnvironment(target)) {
    // The prebuilt cache for macCatalyst is the same as the one for macOS, not
    // iOS or a separate location of its own.
    platform = "macosx";
  } else {
    platform = getPlatformNameForTriple(target);
  }
  llvm::sys::path::append(defaultPrebuiltPath, platform, "prebuilt-modules");

  // If the SDK version is given, we should check if SDK-versioned prebuilt
  // module cache is available and use it if so.
  return getVersionedPrebuiltModulePath(sdkVer, defaultPrebuiltPath);
}

void CompilerInvocation::setDefaultPrebuiltCacheIfNecessary() {

  if (!FrontendOpts.PrebuiltModuleCachePath.empty())
    return;
  if (SearchPathOpts.RuntimeResourcePath.empty())
    return;

  FrontendOpts.PrebuiltModuleCachePath = computePrebuiltCachePath(
      SearchPathOpts.RuntimeResourcePath, LangOpts.Target, LangOpts.SDKVersion);
}

static void updateRuntimeLibraryPaths(SearchPathOptions &SearchPathOpts,
                                      llvm::Triple &Triple) {
  llvm::SmallString<128> LibPath(SearchPathOpts.RuntimeResourcePath);

  StringRef LibSubDir = getPlatformNameForTriple(Triple);
  if (tripleIsMacCatalystEnvironment(Triple))
    LibSubDir = "maccatalyst";

  llvm::sys::path::append(LibPath, LibSubDir);
  SearchPathOpts.RuntimeLibraryPaths.clear();
  SearchPathOpts.RuntimeLibraryPaths.push_back(std::string(LibPath.str()));
  if (Triple.isOSDarwin())
    SearchPathOpts.RuntimeLibraryPaths.push_back(DARWIN_OS_LIBRARY_PATH);

  // Set up the import paths containing the swiftmodules for the libraries in
  // RuntimeLibraryPath.
  SearchPathOpts.RuntimeLibraryImportPaths.clear();

  // If this is set, we don't want any runtime import paths.
  if (SearchPathOpts.SkipRuntimeLibraryImportPaths)
    return;

  SearchPathOpts.RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));

  // This is compatibility for <=5.3
  if (!Triple.isOSDarwin()) {
    llvm::sys::path::append(LibPath, swift::getMajorArchitectureName(Triple));
    SearchPathOpts.RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
  }

  if (!SearchPathOpts.SDKPath.empty()) {
    if (tripleIsMacCatalystEnvironment(Triple)) {
      LibPath = SearchPathOpts.SDKPath;
      llvm::sys::path::append(LibPath, "System", "iOSSupport");
      llvm::sys::path::append(LibPath, "usr", "lib", "swift");
      SearchPathOpts.RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
    }

    LibPath = SearchPathOpts.SDKPath;
    llvm::sys::path::append(LibPath, "usr", "lib", "swift");
    if (!Triple.isOSDarwin()) {
      llvm::sys::path::append(LibPath, getPlatformNameForTriple(Triple));
      llvm::sys::path::append(LibPath, swift::getMajorArchitectureName(Triple));
    }
    SearchPathOpts.RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
  }
}

static void
setIRGenOutputOptsFromFrontendOptions(IRGenOptions &IRGenOpts,
                                      const FrontendOptions &FrontendOpts) {
  // Set the OutputKind for the given Action.
  IRGenOpts.OutputKind = [](FrontendOptions::ActionType Action) {
    switch (Action) {
    case FrontendOptions::ActionType::EmitIRGen:
      return IRGenOutputKind::LLVMAssemblyBeforeOptimization;
    case FrontendOptions::ActionType::EmitIR:
      return IRGenOutputKind::LLVMAssemblyAfterOptimization;
    case FrontendOptions::ActionType::EmitBC:
      return IRGenOutputKind::LLVMBitcode;
    case FrontendOptions::ActionType::EmitAssembly:
      return IRGenOutputKind::NativeAssembly;
    case FrontendOptions::ActionType::Immediate:
      return IRGenOutputKind::Module;
    case FrontendOptions::ActionType::EmitObject:
    default:
      // Just fall back to emitting an object file. If we aren't going to run
      // IRGen, it doesn't really matter what we put here anyways.
      return IRGenOutputKind::ObjectFile;
    }
  }(FrontendOpts.RequestedAction);

  // If we're in JIT mode, set the requisite flags.
  if (FrontendOpts.RequestedAction == FrontendOptions::ActionType::Immediate) {
    IRGenOpts.UseJIT = true;
    IRGenOpts.DebugInfoLevel = IRGenDebugInfoLevel::Normal;
    IRGenOpts.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
  }
}

static void
setBridgingHeaderFromFrontendOptions(ClangImporterOptions &ImporterOpts,
                                     const FrontendOptions &FrontendOpts) {
  if (FrontendOpts.RequestedAction != FrontendOptions::ActionType::EmitPCH)
    return;

  // If there aren't any inputs, there's nothing to do.
  if (!FrontendOpts.InputsAndOutputs.hasInputs())
    return;

  // If we aren't asked to output a bridging header, we don't need to set this.
  if (ImporterOpts.PrecompiledHeaderOutputDir.empty())
    return;

  ImporterOpts.BridgingHeader =
      FrontendOpts.InputsAndOutputs.getFilenameOfFirstInput();
}

void CompilerInvocation::setRuntimeResourcePath(StringRef Path) {
  SearchPathOpts.RuntimeResourcePath = Path.str();
  updateRuntimeLibraryPaths(SearchPathOpts, LangOpts.Target);
}

void CompilerInvocation::setTargetTriple(StringRef Triple) {
  setTargetTriple(llvm::Triple(Triple));
}

void CompilerInvocation::setTargetTriple(const llvm::Triple &Triple) {
  LangOpts.setTarget(Triple);
  updateRuntimeLibraryPaths(SearchPathOpts, LangOpts.Target);
}

void CompilerInvocation::setSDKPath(const std::string &Path) {
  SearchPathOpts.SDKPath = Path;
  updateRuntimeLibraryPaths(SearchPathOpts, LangOpts.Target);
}

static bool ParseFrontendArgs(
    FrontendOptions &opts, ArgList &args, DiagnosticEngine &diags,
    SmallVectorImpl<std::unique_ptr<llvm::MemoryBuffer>> *buffers) {
  ArgsToFrontendOptionsConverter converter(diags, args, opts);
  return converter.convert(buffers);
}

static void diagnoseSwiftVersion(Optional<version::Version> &vers, Arg *verArg,
                                 ArgList &Args, DiagnosticEngine &diags) {
  // General invalid version error
  diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                 verArg->getAsString(Args), verArg->getValue());

  // Note valid versions.
  auto validVers = version::Version::getValidEffectiveVersions();
  auto versStr = "'" + llvm::join(validVers, "', '") + "'";
  diags.diagnose(SourceLoc(), diag::note_valid_swift_versions, versStr);
}

/// Create a new Regex instance out of the string value in \p RpassArg.
/// It returns a pointer to the newly generated Regex instance.
static std::shared_ptr<llvm::Regex>
generateOptimizationRemarkRegex(DiagnosticEngine &Diags, ArgList &Args,
                                Arg *RpassArg) {
  StringRef Val = RpassArg->getValue();
  std::string RegexError;
  std::shared_ptr<llvm::Regex> Pattern = std::make_shared<llvm::Regex>(Val);
  if (!Pattern->isValid(RegexError)) {
    Diags.diagnose(SourceLoc(), diag::error_optimization_remark_pattern,
                   RegexError, RpassArg->getAsString(Args));
    Pattern.reset();
  }
  return Pattern;
}

// Lifted from the clang driver.
static void PrintArg(raw_ostream &OS, const char *Arg, StringRef TempDir) {
  const bool Escape = std::strpbrk(Arg, "\"\\$ ");

  if (!TempDir.empty()) {
    llvm::SmallString<256> ArgPath{Arg};
    llvm::sys::fs::make_absolute(ArgPath);
    llvm::sys::path::native(ArgPath);

    llvm::SmallString<256> TempPath{TempDir};
    llvm::sys::fs::make_absolute(TempPath);
    llvm::sys::path::native(TempPath);

    if (StringRef(ArgPath).startswith(TempPath)) {
      // Don't write temporary file names in the debug info. This would prevent
      // incremental llvm compilation because we would generate different IR on
      // every compiler invocation.
      Arg = "<temporary-file>";
    }
  }

  if (!Escape) {
    OS << Arg;
    return;
  }

  // Quote and escape. This isn't really complete, but good enough.
  OS << '"';
  while (const char c = *Arg++) {
    if (c == '"' || c == '\\' || c == '$')
      OS << '\\';
    OS << c;
  }
  OS << '"';
}

static void ParseModuleInterfaceArgs(ModuleInterfaceOptions &Opts,
                                     ArgList &Args) {
  using namespace options;

  Opts.PreserveTypesAsWritten |=
    Args.hasArg(OPT_module_interface_preserve_types_as_written);
  Opts.PrintFullConvention |=
    Args.hasArg(OPT_experimental_print_full_convention);
  Opts.ExperimentalSPIImports |=
    Args.hasArg(OPT_experimental_spi_imports);
  Opts.DebugPrintInvalidSyntax |=
    Args.hasArg(OPT_debug_emit_invalid_swiftinterface_syntax);

  if (const Arg *A = Args.getLastArg(OPT_library_level)) {
    StringRef contents = A->getValue();
    if (contents == "spi") {
      Opts.PrintSPIs = true;
    }
  }
}

/// Save a copy of any flags marked as ModuleInterfaceOption, if running
/// in a mode that is going to emit a .swiftinterface file.
static void SaveModuleInterfaceArgs(ModuleInterfaceOptions &Opts,
                                    FrontendOptions &FOpts,
                                    ArgList &Args, DiagnosticEngine &Diags) {
  if (!FOpts.InputsAndOutputs.hasModuleInterfaceOutputPath())
    return;
  ArgStringList RenderedArgs;
  for (auto A : Args) {
    if (A->getOption().hasFlag(options::ModuleInterfaceOption))
      A->render(Args, RenderedArgs);
  }
  llvm::raw_string_ostream OS(Opts.Flags);
  interleave(RenderedArgs,
             [&](const char *Argument) { PrintArg(OS, Argument, StringRef()); },
             [&] { OS << " "; });
}

static bool ParseLangArgs(LangOptions &Opts, ArgList &Args,
                          DiagnosticEngine &Diags,
                          const FrontendOptions &FrontendOpts) {
  using namespace options;
  bool HadError = false;

  if (auto A = Args.getLastArg(OPT_swift_version)) {
    auto vers = version::Version::parseVersionString(
      A->getValue(), SourceLoc(), &Diags);
    bool isValid = false;
    if (vers.hasValue()) {
      if (auto effectiveVers = vers.getValue().getEffectiveLanguageVersion()) {
        Opts.EffectiveLanguageVersion = effectiveVers.getValue();
        isValid = true;
      }
    }
    if (!isValid)
      diagnoseSwiftVersion(vers, A, Args, Diags);
  }

  if (auto A = Args.getLastArg(OPT_package_description_version)) {
    auto vers = version::Version::parseVersionString(
      A->getValue(), SourceLoc(), &Diags);
    if (vers.hasValue()) {
      Opts.PackageDescriptionVersion = vers.getValue();
    } else {
      return true;
    }
  }

  Opts.AttachCommentsToDecls |= Args.hasArg(OPT_dump_api_path);

  Opts.UseMalloc |= Args.hasArg(OPT_use_malloc);

  Opts.DiagnosticsEditorMode |= Args.hasArg(OPT_diagnostics_editor_mode,
                                            OPT_serialize_diagnostics_path);

  Opts.EnableExperimentalStaticAssert |=
    Args.hasArg(OPT_enable_experimental_static_assert);

  Opts.EnableExperimentalConcurrency |=
    Args.hasArg(OPT_enable_experimental_concurrency);
  Opts.EnableInferPublicSendable |=
    Args.hasFlag(OPT_enable_infer_public_concurrent_value,
                 OPT_disable_infer_public_concurrent_value,
                 false);
  Opts.EnableExperimentalAsyncHandler |=
    Args.hasArg(OPT_enable_experimental_async_handler);
  Opts.EnableExperimentalFlowSensitiveConcurrentCaptures |=
    Args.hasArg(OPT_enable_experimental_flow_sensitive_concurrent_captures);

  Opts.DisableImplicitConcurrencyModuleImport |=
    Args.hasArg(OPT_disable_implicit_concurrency_module_import);

  Opts.DiagnoseInvalidEphemeralnessAsError |=
      Args.hasArg(OPT_enable_invalid_ephemeralness_as_error);

  if (auto A = Args.getLastArg(OPT_enable_deserialization_recovery,
                               OPT_disable_deserialization_recovery)) {
    Opts.EnableDeserializationRecovery
      = A->getOption().matches(OPT_enable_deserialization_recovery);
  }

  Opts.DisableAvailabilityChecking |=
      Args.hasArg(OPT_disable_availability_checking);

  if (auto A = Args.getLastArg(OPT_enable_conformance_availability_errors,
                               OPT_disable_conformance_availability_errors)) {
    Opts.EnableConformanceAvailabilityErrors
      = A->getOption().matches(OPT_enable_conformance_availability_errors);
  }

  if (auto A = Args.getLastArg(OPT_enable_access_control,
                               OPT_disable_access_control)) {
    Opts.EnableAccessControl
      = A->getOption().matches(OPT_enable_access_control);
  }

  if (auto A = Args.getLastArg(OPT_disable_typo_correction,
                               OPT_typo_correction_limit)) {
    if (A->getOption().matches(OPT_disable_typo_correction))
      Opts.TypoCorrectionLimit = 0;
    else {
      unsigned limit;
      if (StringRef(A->getValue()).getAsInteger(10, limit)) {
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                       A->getAsString(Args), A->getValue());
        HadError = true;
      } else {
        Opts.TypoCorrectionLimit = limit;
      }
    }
  }

  Opts.CodeCompleteInitsInPostfixExpr |=
      Args.hasArg(OPT_code_complete_inits_in_postfix_expr);

  Opts.CodeCompleteCallPatternHeuristics |=
      Args.hasArg(OPT_code_complete_call_pattern_heuristics);

  if (auto A = Args.getLastArg(OPT_enable_target_os_checking,
                               OPT_disable_target_os_checking)) {
    Opts.EnableTargetOSChecking
      = A->getOption().matches(OPT_enable_target_os_checking);
  }
  
  Opts.EnableNewOperatorLookup = Args.hasFlag(OPT_enable_new_operator_lookup,
                                              OPT_disable_new_operator_lookup,
                                              /*default*/ false);
  Opts.UseClangFunctionTypes |= Args.hasArg(OPT_use_clang_function_types);

  Opts.NamedLazyMemberLoading &= !Args.hasArg(OPT_disable_named_lazy_member_loading);

  if (Args.hasArg(OPT_verify_syntax_tree)) {
    Opts.BuildSyntaxTree = true;
    Opts.VerifySyntaxTree = true;
  }

  if (Args.hasArg(OPT_emit_fine_grained_dependency_sourcefile_dot_files))
    Opts.EmitFineGrainedDependencySourcefileDotFiles = true;

  if (Args.hasArg(OPT_enable_experimental_additive_arithmetic_derivation))
    Opts.EnableExperimentalAdditiveArithmeticDerivedConformances = true;

  Opts.EnableExperimentalForwardModeDifferentiation |=
      Args.hasArg(OPT_enable_experimental_forward_mode_differentiation);

  Opts.DebuggerSupport |= Args.hasArg(OPT_debugger_support);
  if (Opts.DebuggerSupport)
    Opts.EnableDollarIdentifiers = true;

  Opts.DebuggerTestingTransform = Args.hasArg(OPT_debugger_testing_transform);

  Opts.Playground |= Args.hasArg(OPT_playground);
  Opts.PlaygroundTransform |= Args.hasArg(OPT_playground);
  if (Args.hasArg(OPT_disable_playground_transform))
    Opts.PlaygroundTransform = false;
  Opts.PlaygroundHighPerformance |=
      Args.hasArg(OPT_playground_high_performance);

  // This can be enabled independently of the playground transform.
  Opts.PCMacro |= Args.hasArg(OPT_pc_macro);

  Opts.EnableThrowWithoutTry |= Args.hasArg(OPT_enable_throw_without_try);

  if (auto A = Args.getLastArg(OPT_enable_objc_attr_requires_foundation_module,
                               OPT_disable_objc_attr_requires_foundation_module)) {
    Opts.EnableObjCAttrRequiresFoundation
      = A->getOption().matches(OPT_enable_objc_attr_requires_foundation_module);
  }

  if (auto A = Args.getLastArg(OPT_enable_testable_attr_requires_testable_module,
                               OPT_disable_testable_attr_requires_testable_module)) {
    Opts.EnableTestableAttrRequiresTestableModule
      = A->getOption().matches(OPT_enable_testable_attr_requires_testable_module);
  }

  if (Args.getLastArg(OPT_debug_cycles))
    Opts.DebugDumpCycles = true;

  if (Args.getLastArg(OPT_require_explicit_availability, OPT_require_explicit_availability_target)) {
    Opts.RequireExplicitAvailability = true;
    if (const Arg *A = Args.getLastArg(OPT_require_explicit_availability_target)) {
      Opts.RequireExplicitAvailabilityTarget = A->getValue();
    }
  }

  for (const Arg *A : Args.filtered(OPT_define_availability)) {
    Opts.AvailabilityMacros.push_back(A->getValue());
  }

  if (const Arg *A = Args.getLastArg(OPT_value_recursion_threshold)) {
    unsigned threshold;
    if (StringRef(A->getValue()).getAsInteger(10, threshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.MaxCircularityDepth = threshold;
    }
  }

  for (const Arg *A : Args.filtered(OPT_D)) {
    Opts.addCustomConditionalCompilationFlag(A->getValue());
  }

  Opts.EnableAppExtensionRestrictions |= Args.hasArg(OPT_enable_app_extension);

  Opts.EnableSwift3ObjCInference =
    Args.hasFlag(OPT_enable_swift3_objc_inference,
                 OPT_disable_swift3_objc_inference, false);

  if (const Arg *A = Args.getLastArg(OPT_library_level)) {
    StringRef contents = A->getValue();
    if (contents == "api") {
      Opts.LibraryLevel = LibraryLevel::API;
    } else if (contents == "spi") {
      Opts.LibraryLevel = LibraryLevel::SPI;
    } else {
      Opts.LibraryLevel = LibraryLevel::Other;
      if (contents != "other") {
        // Error on unknown library levels.
        auto inFlight = Diags.diagnose(SourceLoc(),
                                       diag::error_unknown_library_level,
                                       contents);

        // Only warn for "ipi" as we may use it in the future.
        if (contents == "ipi")
          inFlight.limitBehavior(DiagnosticBehavior::Warning);
      }
    }
  }

  if (Opts.EnableSwift3ObjCInference) {
    if (const Arg *A = Args.getLastArg(
                                   OPT_warn_swift3_objc_inference_minimal,
                                   OPT_warn_swift3_objc_inference_complete)) {
      if (A->getOption().getID() == OPT_warn_swift3_objc_inference_minimal)
        Opts.WarnSwift3ObjCInference = Swift3ObjCInferenceWarnings::Minimal;
      else
        Opts.WarnSwift3ObjCInference = Swift3ObjCInferenceWarnings::Complete;
    }
  }

  Opts.WarnConcurrency |= Args.hasArg(OPT_warn_concurrency);

  Opts.WarnImplicitOverrides =
    Args.hasArg(OPT_warn_implicit_overrides);

  Opts.EnableNSKeyedArchiverDiagnostics =
      Args.hasFlag(OPT_enable_nskeyedarchiver_diagnostics,
                   OPT_disable_nskeyedarchiver_diagnostics,
                   Opts.EnableNSKeyedArchiverDiagnostics);

  Opts.EnableNonFrozenEnumExhaustivityDiagnostics =
    Args.hasFlag(OPT_enable_nonfrozen_enum_exhaustivity_diagnostics,
                 OPT_disable_nonfrozen_enum_exhaustivity_diagnostics,
                 Opts.isSwiftVersionAtLeast(5));

  if (Arg *A = Args.getLastArg(OPT_Rpass_EQ))
    Opts.OptimizationRemarkPassedPattern =
        generateOptimizationRemarkRegex(Diags, Args, A);
  if (Arg *A = Args.getLastArg(OPT_Rpass_missed_EQ))
    Opts.OptimizationRemarkMissedPattern =
        generateOptimizationRemarkRegex(Diags, Args, A);

  Opts.EnableConcisePoundFile =
      Args.hasArg(OPT_enable_experimental_concise_pound_file);
  Opts.EnableFuzzyForwardScanTrailingClosureMatching =
      Args.hasFlag(OPT_enable_fuzzy_forward_scan_trailing_closure_matching,
                   OPT_disable_fuzzy_forward_scan_trailing_closure_matching,
                   true);

  Opts.EnableCrossImportOverlays =
      Args.hasFlag(OPT_enable_cross_import_overlays,
                   OPT_disable_cross_import_overlays,
                   Opts.EnableCrossImportOverlays);

  Opts.EnableCrossImportRemarks = Args.hasArg(OPT_emit_cross_import_remarks);

  Opts.EnableModuleLoadingRemarks = Args.hasArg(OPT_remark_loading_module);

  llvm::Triple Target = Opts.Target;
  StringRef TargetArg;
  std::string TargetArgScratch;

  if (const Arg *A = Args.getLastArg(OPT_target)) {
    Target = llvm::Triple(A->getValue());
    TargetArg = A->getValue();

    // Backward compatibility hack: infer "simulator" environment for x86
    // iOS/tvOS/watchOS. The driver takes care of this for the frontend
    // most of the time, but loading of old .swiftinterface files goes
    // directly to the frontend.
    if (tripleInfersSimulatorEnvironment(Target)) {
      // Set the simulator environment.
      Target.setEnvironment(llvm::Triple::EnvironmentType::Simulator);
      TargetArgScratch = Target.str();
      TargetArg = TargetArgScratch;
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_target_variant)) {
    Opts.TargetVariant = llvm::Triple(A->getValue());
  }

  Opts.EnableCXXInterop |= Args.hasArg(OPT_enable_cxx_interop);
  Opts.EnableObjCInterop =
      Args.hasFlag(OPT_enable_objc_interop, OPT_disable_objc_interop,
                   Target.isOSDarwin());
  Opts.EnableSILOpaqueValues |= Args.hasArg(OPT_enable_sil_opaque_values);

  Opts.VerifyAllSubstitutionMaps |= Args.hasArg(OPT_verify_all_substitution_maps);

  Opts.EnableVolatileModules |= Args.hasArg(OPT_enable_volatile_modules);

  Opts.UseDarwinPreStableABIBit =
    (Target.isMacOSX() && Target.isMacOSXVersionLT(10, 14, 4)) ||
    (Target.isiOS() && Target.isOSVersionLT(12, 2)) ||
    (Target.isTvOS() && Target.isOSVersionLT(12, 2)) ||
    (Target.isWatchOS() && Target.isOSVersionLT(5, 2));

  // Must be processed after any other language options that could affect
  // platform conditions.
  bool UnsupportedOS, UnsupportedArch;
  std::tie(UnsupportedOS, UnsupportedArch) = Opts.setTarget(Target);

  SmallVector<StringRef, 3> TargetComponents;
  TargetArg.split(TargetComponents, "-");

  if (UnsupportedArch) {
    auto TargetArgArch = TargetComponents.size() ? TargetComponents[0] : "";
    Diags.diagnose(SourceLoc(), diag::error_unsupported_target_arch, TargetArgArch);
  }

  if (UnsupportedOS) {
    auto TargetArgOS = TargetComponents.size() > 2 ? TargetComponents[2] : "";
    Diags.diagnose(SourceLoc(), diag::error_unsupported_target_os, TargetArgOS);
  }

  // Parse the SDK version.
  if (Arg *A = Args.getLastArg(options::OPT_target_sdk_version)) {
    auto vers = version::Version::parseVersionString(
      A->getValue(), SourceLoc(), &Diags);
    if (vers.hasValue()) {
      Opts.SDKVersion = *vers;
    } else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  }

  // Parse the target variant SDK version.
  if (Arg *A = Args.getLastArg(options::OPT_target_variant_sdk_version)) {
    auto vers = version::Version::parseVersionString(
      A->getValue(), SourceLoc(), &Diags);
    if (vers.hasValue()) {
      Opts.VariantSDKVersion = *vers;
    } else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_entry_point_function_name)) {
    Opts.entryPointFunctionName = A->getValue();
  }

  if (FrontendOpts.RequestedAction == FrontendOptions::ActionType::EmitSyntax) {
    Opts.BuildSyntaxTree = true;
    Opts.VerifySyntaxTree = true;
  }

  // Configure lexing to parse and remember comments if:
  //   - Emitting a swiftdoc/swiftsourceinfo
  //   - Performing index-while-building
  //   - Emitting a symbol graph file
  // If we are asked to emit a module documentation file, configure lexing and
  // parsing to remember comments.
  if (FrontendOpts.InputsAndOutputs.hasModuleDocOutputPath() ||
      FrontendOpts.InputsAndOutputs.hasModuleSourceInfoOutputPath() ||
      !FrontendOpts.IndexStorePath.empty() || FrontendOpts.EmitSymbolGraph) {
    Opts.AttachCommentsToDecls = true;
  }

  // If we're parsing SIL, access control doesn't make sense to enforce.
  if (Args.hasArg(OPT_parse_sil) ||
      FrontendOpts.InputsAndOutputs.shouldTreatAsSIL()) {
    Opts.EnableAccessControl = false;
    Opts.DisableAvailabilityChecking = true;
  }

  if (FrontendOpts.AllowModuleWithCompilerErrors) {
    Opts.AllowModuleWithCompilerErrors = true;
  }

  if (auto A =
          Args.getLastArg(OPT_enable_ast_verifier, OPT_disable_ast_verifier)) {
    using ASTVerifierOverrideKind = LangOptions::ASTVerifierOverrideKind;
    if (A->getOption().matches(OPT_enable_ast_verifier)) {
      Opts.ASTVerifierOverride = ASTVerifierOverrideKind::EnableVerifier;
    } else if (A->getOption().matches(OPT_disable_ast_verifier)) {
      Opts.ASTVerifierOverride = ASTVerifierOverrideKind::DisableVerifier;
    } else {
      // This is an assert since getLastArg should not have let us get here if
      // we did not have one of enable/disable specified.
      llvm_unreachable(
          "Should have found one of enable/disable ast verifier?!");
    }
  }

  return HadError || UnsupportedOS || UnsupportedArch;
}

static bool ParseTypeCheckerArgs(TypeCheckerOptions &Opts, ArgList &Args,
                                 DiagnosticEngine &Diags,
                                 const FrontendOptions &FrontendOpts) {
  using namespace options;

  bool HadError = false;
  auto setUnsignedIntegerArgument =
      [&Args, &Diags, &HadError](options::ID optionID, unsigned &valueToSet) {
        if (const Arg *A = Args.getLastArg(optionID)) {
          unsigned attempt;
          if (StringRef(A->getValue()).getAsInteger(/*radix*/ 10, attempt)) {
            Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                           A->getAsString(Args), A->getValue());
            HadError = true;
          } else {
            valueToSet = attempt;
          }
        }
      };

  setUnsignedIntegerArgument(OPT_warn_long_function_bodies,
                             Opts.WarnLongFunctionBodies);
  setUnsignedIntegerArgument(OPT_warn_long_expression_type_checking,
                             Opts.WarnLongExpressionTypeChecking);
  setUnsignedIntegerArgument(OPT_solver_expression_time_threshold_EQ,
                             Opts.ExpressionTimeoutThreshold);
  setUnsignedIntegerArgument(OPT_switch_checking_invocation_threshold_EQ,
                             Opts.SwitchCheckingInvocationThreshold);
  setUnsignedIntegerArgument(OPT_debug_constraints_attempt,
                             Opts.DebugConstraintSolverAttempt);
  setUnsignedIntegerArgument(OPT_solver_memory_threshold,
                             Opts.SolverMemoryThreshold);
  setUnsignedIntegerArgument(OPT_solver_shrink_unsolved_threshold,
                             Opts.SolverShrinkUnsolvedThreshold);

  Opts.DebugTimeFunctionBodies |= Args.hasArg(OPT_debug_time_function_bodies);
  Opts.DebugTimeExpressions |=
      Args.hasArg(OPT_debug_time_expression_type_checking);

  // Check for SkipFunctionBodies arguments in order from skipping less to
  // skipping more.
  if (Args.hasArg(
        OPT_experimental_skip_non_inlinable_function_bodies_without_types))
    Opts.SkipFunctionBodies = FunctionBodySkipping::NonInlinableWithoutTypes;

  // If asked to perform InstallAPI, go ahead and enable non-inlinable function
  // body skipping.
  if (Args.hasArg(OPT_experimental_skip_non_inlinable_function_bodies) ||
      Args.hasArg(OPT_tbd_is_installapi))
    Opts.SkipFunctionBodies = FunctionBodySkipping::NonInlinable;

  if (Args.hasArg(OPT_experimental_skip_all_function_bodies))
    Opts.SkipFunctionBodies = FunctionBodySkipping::All;

  if (Opts.SkipFunctionBodies != FunctionBodySkipping::None &&
      FrontendOpts.ModuleName == SWIFT_ONONE_SUPPORT) {
    // Disable these optimizations if we're compiling SwiftOnoneSupport,
    // because we _definitely_ need to look inside every declaration to figure
    // out what gets prespecialized.
    Opts.SkipFunctionBodies = FunctionBodySkipping::None;
    Diags.diagnose(
        SourceLoc(),
        diag::module_incompatible_with_skip_function_bodies,
        SWIFT_ONONE_SUPPORT);
  }

  Opts.DisableConstraintSolverPerformanceHacks |=
      Args.hasArg(OPT_disable_constraint_solver_performance_hacks);

  Opts.EnableOperatorDesignatedTypes |=
      Args.hasArg(OPT_enable_operator_designated_types);

  // Always enable operator designated types for the standard library.
  Opts.EnableOperatorDesignatedTypes |= FrontendOpts.ParseStdlib;

  Opts.EnableOneWayClosureParameters |=
      Args.hasArg(OPT_experimental_one_way_closure_params);

  Opts.PrintFullConvention |=
      Args.hasArg(OPT_experimental_print_full_convention);

  Opts.DebugConstraintSolver |= Args.hasArg(OPT_debug_constraints);
  Opts.DebugGenericSignatures |= Args.hasArg(OPT_debug_generic_signatures);

  for (const Arg *A : Args.filtered(OPT_debug_constraints_on_line)) {
    unsigned line;
    if (StringRef(A->getValue()).getAsInteger(/*radix*/ 10, line)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.DebugConstraintSolverOnLines.push_back(line);
    }
  }
  llvm::sort(Opts.DebugConstraintSolverOnLines);

  if (const Arg *A = Args.getLastArg(OPT_debug_forbid_typecheck_prefix)) {
    Opts.DebugForbidTypecheckPrefix = A->getValue();
  }

  if (Args.getLastArg(OPT_solver_disable_shrink))
    Opts.SolverDisableShrink = true;

  return HadError;
}

static bool ParseClangImporterArgs(ClangImporterOptions &Opts,
                                   ArgList &Args,
                                   DiagnosticEngine &Diags,
                                   StringRef workingDirectory) {
  using namespace options;

  if (const Arg *A = Args.getLastArg(OPT_module_cache_path)) {
    Opts.ModuleCachePath = A->getValue();
  }

  if (const Arg *A = Args.getLastArg(OPT_target_cpu))
    Opts.TargetCPU = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_index_store_path))
    Opts.IndexStorePath = A->getValue();

  for (const Arg *A : Args.filtered(OPT_Xcc)) {
    Opts.ExtraArgs.push_back(A->getValue());
  }

  for (auto A : Args.getAllArgValues(OPT_debug_prefix_map)) {
    // Forward -debug-prefix-map arguments from Swift to Clang as
    // -fdebug-prefix-map. This is required to ensure DIFiles created there,
    // like "<swift-imported-modules>", have their paths remapped properly.
    // (Note, however, that Clang's usage of std::map means that the remapping
    // may not be applied in the same order, which can matter if one mapping is
    // a prefix of another.)
    Opts.ExtraArgs.push_back("-fdebug-prefix-map=" + A);
  }

  if (!workingDirectory.empty()) {
    // Provide a working directory to Clang as well if there are any -Xcc
    // options, in case some of them are search-related. But do it at the
    // beginning, so that an explicit -Xcc -working-directory will win.
    Opts.ExtraArgs.insert(Opts.ExtraArgs.begin(),
                          {"-working-directory", workingDirectory.str()});
  }

  Opts.DumpClangDiagnostics |= Args.hasArg(OPT_dump_clang_diagnostics);

  if (Args.hasArg(OPT_embed_bitcode))
    Opts.Mode = ClangImporterOptions::Modes::EmbedBitcode;
  else if (Args.hasArg(OPT_emit_pcm) || Args.hasArg(OPT_dump_pcm))
    Opts.Mode = ClangImporterOptions::Modes::PrecompiledModule;

  if (auto *A = Args.getLastArg(OPT_import_objc_header))
    Opts.BridgingHeader = A->getValue();
  Opts.DisableSwiftBridgeAttr |= Args.hasArg(OPT_disable_swift_bridge_attr);

  Opts.DisableOverlayModules |= Args.hasArg(OPT_emit_imported_modules);

  Opts.ExtraArgsOnly |= Args.hasArg(OPT_extra_clang_options_only);

  if (const Arg *A = Args.getLastArg(OPT_pch_output_dir)) {
    Opts.PrecompiledHeaderOutputDir = A->getValue();
    Opts.PCHDisableValidation |= Args.hasArg(OPT_pch_disable_validation);
  }

  if (Args.hasFlag(options::OPT_warnings_as_errors,
                   options::OPT_no_warnings_as_errors, false))
    Opts.ExtraArgs.push_back("-Werror");

  Opts.DebuggerSupport |= Args.hasArg(OPT_debugger_support);

  Opts.DisableSourceImport |=
      Args.hasArg(OPT_disable_clangimporter_source_import);

  return false;
}

static bool ParseSearchPathArgs(SearchPathOptions &Opts,
                                ArgList &Args,
                                DiagnosticEngine &Diags,
                                StringRef workingDirectory) {
  using namespace options;
  namespace path = llvm::sys::path;

  auto resolveSearchPath =
      [workingDirectory](StringRef searchPath) -> std::string {
    if (workingDirectory.empty() || path::is_absolute(searchPath))
      return searchPath.str();
    SmallString<64> fullPath{workingDirectory};
    path::append(fullPath, searchPath);
    return std::string(fullPath.str());
  };

  for (const Arg *A : Args.filtered(OPT_I)) {
    Opts.ImportSearchPaths.push_back(resolveSearchPath(A->getValue()));
  }

  for (const Arg *A : Args.filtered(OPT_F, OPT_Fsystem)) {
    Opts.FrameworkSearchPaths.push_back({resolveSearchPath(A->getValue()),
                           /*isSystem=*/A->getOption().getID() == OPT_Fsystem});
  }

  for (const Arg *A : Args.filtered(OPT_L)) {
    Opts.LibrarySearchPaths.push_back(resolveSearchPath(A->getValue()));
  }

  for (const Arg *A : Args.filtered(OPT_vfsoverlay)) {
    Opts.VFSOverlayFiles.push_back(resolveSearchPath(A->getValue()));
  }

  if (const Arg *A = Args.getLastArg(OPT_sdk))
    Opts.SDKPath = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_resource_dir))
    Opts.RuntimeResourcePath = A->getValue();

  Opts.SkipRuntimeLibraryImportPaths |= Args.hasArg(OPT_nostdimport);

  Opts.DisableModulesValidateSystemDependencies |=
      Args.hasArg(OPT_disable_modules_validate_system_headers);

  for (auto A: Args.filtered(OPT_swift_module_file)) {
    Opts.ExplicitSwiftModules.push_back(resolveSearchPath(A->getValue()));
  }
  if (const Arg *A = Args.getLastArg(OPT_explict_swift_module_map))
    Opts.ExplicitSwiftModuleMap = A->getValue();
  for (auto A: Args.filtered(OPT_candidate_module_file)) {
    Opts.CandidateCompiledModules.push_back(resolveSearchPath(A->getValue()));
  }
  if (const Arg *A = Args.getLastArg(OPT_placeholder_dependency_module_map))
    Opts.PlaceholderDependencyModuleMap = A->getValue();
  if (const Arg *A = Args.getLastArg(OPT_batch_scan_input_file))
    Opts.BatchScanInputFilePath = A->getValue();
  // Opts.RuntimeIncludePath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath().
  // Opts.RuntimeImportPath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath() and
  // updated by calls to setTargetTriple() or parseArgs().
  // Assumes exactly one of setMainExecutablePath() or setRuntimeIncludePath()
  // is called before setTargetTriple() and parseArgs().
  // TODO: improve the handling of RuntimeIncludePath.

  return false;
}

static bool ParseDiagnosticArgs(DiagnosticOptions &Opts, ArgList &Args,
                                DiagnosticEngine &Diags) {
  using namespace options;

  if (Args.hasArg(OPT_verify))
    Opts.VerifyMode = DiagnosticOptions::Verify;
  if (Args.hasArg(OPT_verify_apply_fixes))
    Opts.VerifyMode = DiagnosticOptions::VerifyAndApplyFixes;
  Opts.VerifyIgnoreUnknown |= Args.hasArg(OPT_verify_ignore_unknown);
  Opts.SkipDiagnosticPasses |= Args.hasArg(OPT_disable_diagnostic_passes);
  Opts.ShowDiagnosticsAfterFatalError |=
    Args.hasArg(OPT_show_diagnostics_after_fatal);

  for (Arg *A : Args.filtered(OPT_verify_additional_file))
    Opts.AdditionalVerifierFiles.push_back(A->getValue());

  Opts.UseColor |=
      Args.hasFlag(OPT_color_diagnostics,
                   OPT_no_color_diagnostics,
                   /*Default=*/llvm::sys::Process::StandardErrHasColors());
  // If no style options are specified, default to LLVM style.
  Opts.PrintedFormattingStyle = DiagnosticOptions::FormattingStyle::LLVM;
  if (const Arg *arg = Args.getLastArg(OPT_diagnostic_style)) {
    StringRef contents = arg->getValue();
    if (contents == "llvm") {
      Opts.PrintedFormattingStyle = DiagnosticOptions::FormattingStyle::LLVM;
    } else if (contents == "swift") {
      Opts.PrintedFormattingStyle = DiagnosticOptions::FormattingStyle::Swift;
    } else {
      Diags.diagnose(SourceLoc(), diag::error_unsupported_option_argument,
                     arg->getOption().getPrefixedName(), arg->getValue());
      return true;
    }
  }

  Opts.FixitCodeForAllDiagnostics |= Args.hasArg(OPT_fixit_all);
  Opts.SuppressWarnings |= Args.hasArg(OPT_suppress_warnings);
  Opts.WarningsAsErrors = Args.hasFlag(options::OPT_warnings_as_errors,
                                       options::OPT_no_warnings_as_errors,
                                       false);
  Opts.PrintDiagnosticNames |= Args.hasArg(OPT_debug_diagnostic_names);
  Opts.PrintEducationalNotes |= Args.hasArg(OPT_print_educational_notes);
  if (Arg *A = Args.getLastArg(OPT_diagnostic_documentation_path)) {
    Opts.DiagnosticDocumentationPath = A->getValue();
  }
  if (Arg *A = Args.getLastArg(OPT_locale)) {
    std::string localeCode = A->getValue();

    // Check if the locale code is available.
    if (llvm::none_of(localeCodes, [&](const char *locale) {
          return localeCode == locale;
        })) {
      std::string availableLocaleCodes = "";
      llvm::interleave(
          std::begin(localeCodes), std::end(localeCodes),
          [&](std::string locale) { availableLocaleCodes += locale; },
          [&] { availableLocaleCodes += ", "; });

      Diags.diagnose(SourceLoc(), diag::warning_invalid_locale_code,
                     availableLocaleCodes);
    } else {
      Opts.LocalizationCode = localeCode;
    }
  }
  if (Arg *A = Args.getLastArg(OPT_localization_path)) {
    if (!llvm::sys::fs::exists(A->getValue())) {
      Diags.diagnose(SourceLoc(), diag::warning_locale_path_not_found,
                     A->getValue());
    } else if (!Opts.LocalizationCode.empty()) {
      // Check if the localization path exists but it doesn't have a file
      // for the specified locale code.
      llvm::SmallString<128> localizationPath(A->getValue());
      llvm::sys::path::append(localizationPath, Opts.LocalizationCode);
      llvm::sys::path::replace_extension(localizationPath, ".yaml");
      if (!llvm::sys::fs::exists(localizationPath)) {
        Diags.diagnose(SourceLoc(), diag::warning_cannot_find_locale_file,
                       Opts.LocalizationCode, localizationPath);
      }

      Opts.LocalizationPath = A->getValue();
    }
  }
  assert(!(Opts.WarningsAsErrors && Opts.SuppressWarnings) &&
         "conflicting arguments; should have been caught by driver");

  return false;
}

/// Parse -enforce-exclusivity=... options
void parseExclusivityEnforcementOptions(const llvm::opt::Arg *A,
                                        SILOptions &Opts,
                                        DiagnosticEngine &Diags) {
  StringRef Argument = A->getValue();
  if (Argument == "unchecked") {
    // This option is analogous to the -Ounchecked optimization setting.
    // It will disable dynamic checking but still diagnose statically.
    Opts.EnforceExclusivityStatic = true;
    Opts.EnforceExclusivityDynamic = false;
  } else if (Argument == "checked") {
    Opts.EnforceExclusivityStatic = true;
    Opts.EnforceExclusivityDynamic = true;
  } else if (Argument == "dynamic-only") {
    // This option is intended for staging purposes. The intent is that
    // it will eventually be removed.
    Opts.EnforceExclusivityStatic = false;
    Opts.EnforceExclusivityDynamic = true;
  } else if (Argument == "none") {
    // This option is for staging purposes.
    Opts.EnforceExclusivityStatic = false;
    Opts.EnforceExclusivityDynamic = false;
  } else {
    Diags.diagnose(SourceLoc(), diag::error_unsupported_option_argument,
        A->getOption().getPrefixedName(), A->getValue());
  }
}

static bool ParseSILArgs(SILOptions &Opts, ArgList &Args,
                         IRGenOptions &IRGenOpts,
                         const FrontendOptions &FEOpts,
                         const TypeCheckerOptions &TCOpts,
                         DiagnosticEngine &Diags,
                         const llvm::Triple &Triple,
                         ClangImporterOptions &ClangOpts) {
  using namespace options;


  if (const Arg *A = Args.getLastArg(OPT_sil_inline_threshold)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.InlineThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }
  if (const Arg *A = Args.getLastArg(OPT_sil_inline_caller_benefit_reduction_factor)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.CallerBaseBenefitReductionFactor)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }
  if (const Arg *A = Args.getLastArg(OPT_sil_unroll_threshold)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.UnrollThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  }

  // If we're only emitting a module, stop optimizations once we've serialized
  // the SIL for the module.
  if (FEOpts.RequestedAction == FrontendOptions::ActionType::EmitModuleOnly)
    Opts.StopOptimizationAfterSerialization = true;

  // Propagate the typechecker's understanding of
  // -experimental-skip-*-function-bodies to SIL.
  Opts.SkipFunctionBodies = TCOpts.SkipFunctionBodies;

  // Parse the optimization level.
  // Default to Onone settings if no option is passed.
  Opts.OptMode = OptimizationMode::NoOptimization;
  if (const Arg *A = Args.getLastArg(OPT_O_Group)) {
    if (A->getOption().matches(OPT_Onone)) {
      // Already set.
    } else if (A->getOption().matches(OPT_Ounchecked)) {
      // Turn on optimizations and remove all runtime checks.
      Opts.OptMode = OptimizationMode::ForSpeed;
      // Removal of cond_fail (overflow on binary operations).
      Opts.RemoveRuntimeAsserts = true;
      Opts.AssertConfig = SILOptions::Unchecked;
    } else if (A->getOption().matches(OPT_Oplayground)) {
      // For now -Oplayground is equivalent to -Onone.
      Opts.OptMode = OptimizationMode::NoOptimization;
    } else if (A->getOption().matches(OPT_Osize)) {
      Opts.OptMode = OptimizationMode::ForSize;
    } else {
      assert(A->getOption().matches(OPT_O));
      Opts.OptMode = OptimizationMode::ForSpeed;
    }

    if (Opts.shouldOptimize()) {
      ClangOpts.Optimization = "-Os";
    }
  }
  IRGenOpts.OptMode = Opts.OptMode;

  if (Args.getLastArg(OPT_AssumeSingleThreaded)) {
    Opts.AssumeSingleThreaded = true;
  }

  Opts.IgnoreAlwaysInline |= Args.hasArg(OPT_ignore_always_inline);

  // Parse the assert configuration identifier.
  if (const Arg *A = Args.getLastArg(OPT_AssertConfig)) {
    StringRef Configuration = A->getValue();
    if (Configuration == "DisableReplacement") {
      Opts.AssertConfig = SILOptions::DisableReplacement;
    } else if (Configuration == "Debug") {
      Opts.AssertConfig = SILOptions::Debug;
    } else if (Configuration == "Release") {
      Opts.AssertConfig = SILOptions::Release;
    } else if (Configuration == "Unchecked") {
      Opts.AssertConfig = SILOptions::Unchecked;
    } else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  } else if (FEOpts.ParseStdlib) {
    // Disable assertion configuration replacement when we build the standard
    // library.
    Opts.AssertConfig = SILOptions::DisableReplacement;
  } else if (Opts.AssertConfig == SILOptions::Debug) {
    // Set the assert configuration according to the optimization level if it
    // has not been set by the -Ounchecked flag.
    Opts.AssertConfig =
        (IRGenOpts.shouldOptimize() ? SILOptions::Release : SILOptions::Debug);
  }

  // -Ounchecked might also set removal of runtime asserts (cond_fail).
  Opts.RemoveRuntimeAsserts |= Args.hasArg(OPT_RemoveRuntimeAsserts);

  Opts.EnableCopyPropagation |= Args.hasArg(OPT_enable_copy_propagation);
  Opts.DisableCopyPropagation |= Args.hasArg(OPT_disable_copy_propagation);
  Opts.EnableARCOptimizations &= !Args.hasArg(OPT_disable_arc_opts);
  Opts.EnableOSSAModules |= Args.hasArg(OPT_enable_ossa_modules);
  Opts.EnableOSSAOptimizations &= !Args.hasArg(OPT_disable_ossa_opts);
  Opts.EnableSpeculativeDevirtualization |= Args.hasArg(OPT_enable_spec_devirt);
  Opts.EnableActorDataRaceChecks |= Args.hasFlag(
      OPT_enable_actor_data_race_checks,
      OPT_disable_actor_data_race_checks, /*default=*/false);
  Opts.DisableSILPerfOptimizations |= Args.hasArg(OPT_disable_sil_perf_optzns);
  Opts.CrossModuleOptimization |= Args.hasArg(OPT_CrossModuleOptimization);
  Opts.VerifyAll |= Args.hasArg(OPT_sil_verify_all);
  Opts.VerifyNone |= Args.hasArg(OPT_sil_verify_none);
  Opts.DebugSerialization |= Args.hasArg(OPT_sil_debug_serialization);
  Opts.EmitVerboseSIL |= Args.hasArg(OPT_emit_verbose_sil);
  Opts.EmitSortedSIL |= Args.hasArg(OPT_emit_sorted_sil);
  Opts.PrintFullConvention |=
      Args.hasArg(OPT_experimental_print_full_convention);
  Opts.PrintInstCounts |= Args.hasArg(OPT_print_inst_counts);
  Opts.StopOptimizationBeforeLoweringOwnership |=
      Args.hasArg(OPT_sil_stop_optzns_before_lowering_ownership);
  if (const Arg *A = Args.getLastArg(OPT_external_pass_pipeline_filename))
    Opts.ExternalPassPipelineFilename = A->getValue();

  Opts.GenerateProfile |= Args.hasArg(OPT_profile_generate);
  const Arg *ProfileUse = Args.getLastArg(OPT_profile_use);
  Opts.UseProfile = ProfileUse ? ProfileUse->getValue() : "";

  Opts.EmitProfileCoverageMapping |= Args.hasArg(OPT_profile_coverage_mapping);
  Opts.DisableSILPartialApply |=
    Args.hasArg(OPT_disable_sil_partial_apply);
  Opts.VerifySILOwnership &= !Args.hasArg(OPT_disable_sil_ownership_verifier);
  Opts.EnableDynamicReplacementCanCallPreviousImplementation = !Args.hasArg(
      OPT_disable_previous_implementation_calls_in_dynamic_replacements);

  if (const Arg *A = Args.getLastArg(OPT_save_optimization_record_EQ)) {
    llvm::Expected<llvm::remarks::Format> formatOrErr =
        llvm::remarks::parseFormat(A->getValue());
    if (llvm::Error err = formatOrErr.takeError()) {
      Diags.diagnose(SourceLoc(), diag::error_creating_remark_serializer,
                     toString(std::move(err)));
      return true;
    }
    Opts.OptRecordFormat = *formatOrErr;
  }

  if (const Arg *A = Args.getLastArg(OPT_save_optimization_record_passes))
    Opts.OptRecordPasses = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_save_optimization_record_path))
    Opts.OptRecordFile = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_module_summary_path)) {
    Opts.ModuleSummaryPath = A->getValue();
  }

  if (Args.hasArg(OPT_debug_on_sil)) {
    // Derive the name of the SIL file for debugging from
    // the regular outputfile.
    std::string BaseName = FEOpts.InputsAndOutputs.getSingleOutputFilename();
    // If there are no or multiple outputfiles, derive the name
    // from the module name.
    if (BaseName.empty())
      BaseName = FEOpts.ModuleName;
    Opts.SILOutputFileNameForDebugging = BaseName;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_EQ)) {
    Opts.Sanitizers = parseSanitizerArgValues(
        Args, A, Triple, Diags,
        /* sanitizerRuntimeLibExists= */[](StringRef libName, bool shared) {

          // The driver has checked the existence of the library
          // already.
          return true;
        });
    IRGenOpts.Sanitizers = Opts.Sanitizers;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_recover_EQ)) {
    IRGenOpts.SanitizersWithRecoveryInstrumentation =
        parseSanitizerRecoverArgValues(A, Opts.Sanitizers, Diags,
                                       /*emitWarnings=*/true);
  }

  if (const Arg *A =
          Args.getLastArg(options::OPT_sanitize_address_use_odr_indicator)) {
    IRGenOpts.SanitizeAddressUseODRIndicator =
        parseSanitizerAddressUseODRIndicator(A, Opts.Sanitizers, Diags);
  }

  if (auto A = Args.getLastArg(OPT_enable_verify_exclusivity,
                               OPT_disable_verify_exclusivity)) {
    Opts.VerifyExclusivity
      = A->getOption().matches(OPT_enable_verify_exclusivity);
  }
  // If runtime asserts are disabled in general, also disable runtime
  // exclusivity checks unless explicitly requested.
  if (Opts.RemoveRuntimeAsserts)
    Opts.EnforceExclusivityDynamic = false;

  if (const Arg *A = Args.getLastArg(options::OPT_enforce_exclusivity_EQ)) {
    parseExclusivityEnforcementOptions(A, Opts, Diags);
  }

  return false;
}

void CompilerInvocation::buildDebugFlags(std::string &Output,
                                         const ArrayRef<const char*> &Args,
                                         StringRef SDKPath,
                                         StringRef ResourceDir) {
  // This isn't guaranteed to be the same temp directory as what the driver
  // uses, but it's highly likely.
  llvm::SmallString<128> TDir;
  llvm::sys::path::system_temp_directory(true, TDir);

  llvm::raw_string_ostream OS(Output);
  interleave(Args,
             [&](const char *Argument) { PrintArg(OS, Argument, TDir.str()); },
             [&] { OS << " "; });

  // Inject the SDK path and resource dir if they are nonempty and missing.
  bool haveSDKPath = SDKPath.empty();
  bool haveResourceDir = ResourceDir.empty();
  for (auto A : Args) {
    StringRef Arg(A);
    // FIXME: this should distinguish between key and value.
    if (!haveSDKPath && Arg.equals("-sdk"))
      haveSDKPath = true;
    if (!haveResourceDir && Arg.equals("-resource-dir"))
      haveResourceDir = true;
  }
  if (!haveSDKPath) {
    OS << " -sdk ";
    PrintArg(OS, SDKPath.data(), TDir.str());
  }
  if (!haveResourceDir) {
    OS << " -resource-dir ";
    PrintArg(OS, ResourceDir.data(), TDir.str());
  }
}

static bool ParseTBDGenArgs(TBDGenOptions &Opts, ArgList &Args,
                            DiagnosticEngine &Diags,
                            CompilerInvocation &Invocation) {
  using namespace options;

  Opts.HasMultipleIGMs = Invocation.getIRGenOptions().hasMultipleIGMs();

  if (const Arg *A = Args.getLastArg(OPT_module_link_name)) {
    Opts.ModuleLinkName = A->getValue();
  }

  if (const Arg *A = Args.getLastArg(OPT_tbd_install_name)) {
    Opts.InstallName = A->getValue();
  }

  Opts.IsInstallAPI = Args.hasArg(OPT_tbd_is_installapi);

  if (const Arg *A = Args.getLastArg(OPT_tbd_compatibility_version)) {
    Opts.CompatibilityVersion = A->getValue();
  }

  if (const Arg *A = Args.getLastArg(OPT_tbd_current_version)) {
    Opts.CurrentVersion = A->getValue();
  }
  if (const Arg *A = Args.getLastArg(OPT_previous_module_installname_map_file)) {
    Opts.ModuleInstallNameMapPath = A->getValue();
  }
  for (auto A : Args.getAllArgValues(OPT_embed_tbd_for_module)) {
    Opts.embedSymbolsFromModules.push_back(StringRef(A).str());
  }
  return false;
}

static bool ParseIRGenArgs(IRGenOptions &Opts, ArgList &Args,
                           DiagnosticEngine &Diags,
                           const FrontendOptions &FrontendOpts,
                           const SILOptions &SILOpts,
                           StringRef SDKPath,
                           StringRef ResourceDir,
                           const llvm::Triple &Triple) {
  using namespace options;

  if (!SILOpts.SILOutputFileNameForDebugging.empty()) {
      Opts.DebugInfoLevel = IRGenDebugInfoLevel::LineTables;
  } else if (const Arg *A = Args.getLastArg(OPT_g_Group)) {
    if (A->getOption().matches(OPT_g))
      Opts.DebugInfoLevel = IRGenDebugInfoLevel::Normal;
    else if (A->getOption().matches(options::OPT_gline_tables_only))
      Opts.DebugInfoLevel = IRGenDebugInfoLevel::LineTables;
    else if (A->getOption().matches(options::OPT_gdwarf_types))
      Opts.DebugInfoLevel = IRGenDebugInfoLevel::DwarfTypes;
    else
      assert(A->getOption().matches(options::OPT_gnone) &&
             "unknown -g<kind> option");
  }
  if (Opts.DebugInfoLevel >= IRGenDebugInfoLevel::LineTables) {
    if (Args.hasArg(options::OPT_debug_info_store_invocation)) {
      ArgStringList RenderedArgs;
      for (auto A : Args)
        A->render(Args, RenderedArgs);
      CompilerInvocation::buildDebugFlags(Opts.DebugFlags,
                                          RenderedArgs, SDKPath,
                                          ResourceDir);
    }
    // TODO: Should we support -fdebug-compilation-dir?
    llvm::SmallString<256> cwd;
    llvm::sys::fs::current_path(cwd);
    Opts.DebugCompilationDir = std::string(cwd.str());
  }

  if (const Arg *A = Args.getLastArg(options::OPT_debug_info_format)) {
    if (A->containsValue("dwarf"))
      Opts.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
    else if (A->containsValue("codeview"))
      Opts.DebugInfoFormat = IRGenDebugInfoFormat::CodeView;
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  } else if (Opts.DebugInfoLevel > IRGenDebugInfoLevel::None) {
    // If -g was specified but not -debug-info-format, DWARF is assumed.
    Opts.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
  }
  if (Args.hasArg(options::OPT_debug_info_format) &&
      !Args.hasArg(options::OPT_g_Group)) {
    const Arg *debugFormatArg = Args.getLastArg(options::OPT_debug_info_format);
    Diags.diagnose(SourceLoc(), diag::error_option_missing_required_argument,
                   debugFormatArg->getAsString(Args), "-g");
  }
  if (Opts.DebugInfoFormat == IRGenDebugInfoFormat::CodeView &&
      (Opts.DebugInfoLevel == IRGenDebugInfoLevel::LineTables ||
       Opts.DebugInfoLevel == IRGenDebugInfoLevel::DwarfTypes)) {
    const Arg *debugFormatArg = Args.getLastArg(options::OPT_debug_info_format);
    Diags.diagnose(SourceLoc(), diag::error_argument_not_allowed_with,
                   debugFormatArg->getAsString(Args),
                   Opts.DebugInfoLevel == IRGenDebugInfoLevel::LineTables
                     ? "-gline-tables-only"
                     : "-gdwarf_types");
  }

  for (auto A : Args.getAllArgValues(options::OPT_debug_prefix_map)) {
    auto SplitMap = StringRef(A).split('=');
    Opts.DebugPrefixMap.addMapping(SplitMap.first, SplitMap.second);
  }

  for (auto A : Args.getAllArgValues(options::OPT_coverage_prefix_map)) {
    auto SplitMap = StringRef(A).split('=');
    Opts.CoveragePrefixMap.addMapping(SplitMap.first, SplitMap.second);
  }

  for (const Arg *A : Args.filtered(OPT_Xcc)) {
    StringRef Opt = A->getValue();
    if (Opt.startswith("-D") || Opt.startswith("-U"))
      Opts.ClangDefines.push_back(Opt.str());
  }

  for (const Arg *A : Args.filtered(OPT_l, OPT_framework)) {
    LibraryKind Kind;
    if (A->getOption().matches(OPT_l)) {
      Kind = LibraryKind::Library;
    } else if (A->getOption().matches(OPT_framework)) {
      Kind = LibraryKind::Framework;
    } else {
      llvm_unreachable("Unknown LinkLibrary option kind");
    }

    Opts.LinkLibraries.push_back(LinkLibrary(A->getValue(), Kind));
  }

  if (auto valueNames = Args.getLastArg(OPT_disable_llvm_value_names,
                                        OPT_enable_llvm_value_names)) {
    Opts.HasValueNamesSetting = true;
    Opts.ValueNames =
      valueNames->getOption().matches(OPT_enable_llvm_value_names);
  }

  Opts.DisableLLVMOptzns |= Args.hasArg(OPT_disable_llvm_optzns);
  Opts.DisableSwiftSpecificLLVMOptzns |=
      Args.hasArg(OPT_disable_swift_specific_llvm_optzns);
  Opts.DisableLLVMSLPVectorizer |= Args.hasArg(OPT_disable_llvm_slp_vectorizer);
  if (Args.hasArg(OPT_disable_llvm_verify))
    Opts.Verify = false;

  Opts.EmitStackPromotionChecks |= Args.hasArg(OPT_stack_promotion_checks);
  if (const Arg *A = Args.getLastArg(OPT_stack_promotion_limit)) {
    unsigned limit;
    if (StringRef(A->getValue()).getAsInteger(10, limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
    Opts.StackPromotionSizeLimit = limit;
  }

  Opts.FunctionSections = Args.hasArg(OPT_function_sections);

  if (Args.hasArg(OPT_autolink_force_load))
    Opts.ForceLoadSymbolName = Args.getLastArgValue(OPT_module_link_name).str();

  Opts.ModuleName = FrontendOpts.ModuleName;

  if (Args.hasArg(OPT_no_clang_module_breadcrumbs))
    Opts.DisableClangModuleSkeletonCUs = true;

  if (Args.hasArg(OPT_disable_debugger_shadow_copies))
    Opts.DisableDebuggerShadowCopies = true;

  if (Args.hasArg(OPT_disable_concrete_type_metadata_mangled_name_accessors))
    Opts.DisableConcreteTypeMetadataMangledNameAccessors = true;

  if (Args.hasArg(OPT_use_jit)) {
    Opts.UseJIT = true;
    if (const Arg *A = Args.getLastArg(OPT_dump_jit)) {
      llvm::Optional<swift::JITDebugArtifact> artifact =
          llvm::StringSwitch<llvm::Optional<swift::JITDebugArtifact>>(A->getValue())
              .Case("llvm-ir", JITDebugArtifact::LLVMIR)
              .Case("object", JITDebugArtifact::Object)
              .Default(None);
      if (!artifact) {
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                       A->getOption().getName(), A->getValue());
        return true;
      }
      Opts.DumpJIT = *artifact;
    }
  }

  for (const Arg *A : Args.filtered(OPT_verify_type_layout)) {
    Opts.VerifyTypeLayoutNames.push_back(A->getValue());
  }

  for (const Arg *A : Args.filtered(OPT_disable_autolink_framework)) {
    Opts.DisableAutolinkFrameworks.push_back(A->getValue());
  }

  Opts.GenerateProfile |= Args.hasArg(OPT_profile_generate);
  const Arg *ProfileUse = Args.getLastArg(OPT_profile_use);
  Opts.UseProfile = ProfileUse ? ProfileUse->getValue() : "";

  Opts.PrintInlineTree |= Args.hasArg(OPT_print_llvm_inline_tree);

  Opts.EnableDynamicReplacementChaining |=
      Args.hasArg(OPT_enable_dynamic_replacement_chaining);

  if (auto A = Args.getLastArg(OPT_enable_type_layouts,
                               OPT_disable_type_layouts)) {
    Opts.UseTypeLayoutValueHandling
      = A->getOption().matches(OPT_enable_type_layouts);
  } else if (Opts.OptMode == OptimizationMode::NoOptimization) {
    // Disable type layouts at Onone except if explictly requested.
    Opts.UseTypeLayoutValueHandling = false;
  }

  Opts.UseSwiftCall = Args.hasArg(OPT_enable_swiftcall);

  // This is set to true by default.
  Opts.UseIncrementalLLVMCodeGen &=
    !Args.hasArg(OPT_disable_incremental_llvm_codegeneration);

  if (Args.hasArg(OPT_embed_bitcode))
    Opts.EmbedMode = IRGenEmbedMode::EmbedBitcode;
  else if (Args.hasArg(OPT_embed_bitcode_marker))
    Opts.EmbedMode = IRGenEmbedMode::EmbedMarker;

  if (Opts.EmbedMode == IRGenEmbedMode::EmbedBitcode) {
    // Keep track of backend options so we can embed them in a separate data
    // section and use them when building from the bitcode. This can be removed
    // when all the backend options are recorded in the IR.
    for (const Arg *A : Args) {
      // Do not encode output and input.
      if (A->getOption().getID() == options::OPT_o ||
          A->getOption().getID() == options::OPT_INPUT ||
          A->getOption().getID() == options::OPT_primary_file ||
          A->getOption().getID() == options::OPT_embed_bitcode)
        continue;
      ArgStringList ASL;
      A->render(Args, ASL);
      for (ArgStringList::iterator it = ASL.begin(), ie = ASL.end();
          it != ie; ++ it) {
        StringRef ArgStr(*it);
        Opts.CmdArgs.insert(Opts.CmdArgs.end(), ArgStr.begin(), ArgStr.end());
        // using \00 to terminate to avoid problem decoding.
        Opts.CmdArgs.push_back('\0');
      }
    }
  }

  if (const Arg *A = Args.getLastArg(options::OPT_lto)) {
    auto LLVMLTOKind =
        llvm::StringSwitch<Optional<IRGenLLVMLTOKind>>(A->getValue())
            .Case("llvm-thin", IRGenLLVMLTOKind::Thin)
            .Case("llvm-full", IRGenLLVMLTOKind::Full)
            .Default(llvm::None);
    if (LLVMLTOKind)
      Opts.LLVMLTOKind = LLVMLTOKind.getValue();
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  }

  if (Args.hasArg(OPT_no_stdlib_link)) {
    Opts.DisableAutolinkStdlib = true;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_coverage_EQ)) {
    Opts.SanitizeCoverage =
        parseSanitizerCoverageArgValue(A, Triple, Diags, Opts.Sanitizers);
  } else if (Opts.Sanitizers & SanitizerKind::Fuzzer) {

    // Automatically set coverage flags, unless coverage type was explicitly
    // requested.
    // Updated to match clang at Jul 2019.
    Opts.SanitizeCoverage.IndirectCalls = true;
    Opts.SanitizeCoverage.TraceCmp = true;
    Opts.SanitizeCoverage.PCTable = true;
    if (Triple.isOSLinux()) {
      Opts.SanitizeCoverage.StackDepth = true;
    }
    Opts.SanitizeCoverage.Inline8bitCounters = true;
    Opts.SanitizeCoverage.CoverageType = llvm::SanitizerCoverageOptions::SCK_Edge;
  }

  if (Args.hasArg(OPT_disable_reflection_metadata)) {
    Opts.EnableReflectionMetadata = false;
    Opts.EnableReflectionNames = false;
  }

  if (Args.hasArg(OPT_enable_anonymous_context_mangled_names))
    Opts.EnableAnonymousContextMangledNames = true;

  if (Args.hasArg(OPT_disable_reflection_names)) {
    Opts.EnableReflectionNames = false;
  }

  if (Args.hasArg(OPT_force_public_linkage)) {
    Opts.ForcePublicLinkage = true;
  }

  // PE/COFF cannot deal with the cross-module reference to the metadata parent
  // (e.g. NativeObject).  Force the lazy initialization of the VWT always.
  Opts.LazyInitializeClassMetadata = Triple.isOSBinFormatCOFF();

  // PE/COFF cannot deal with cross-module reference to the protocol conformance
  // witness.  Use a runtime initialized value for the protocol conformance
  // witness.
  Opts.LazyInitializeProtocolConformances = Triple.isOSBinFormatCOFF();

  if (Args.hasArg(OPT_disable_legacy_type_info)) {
    Opts.DisableLegacyTypeInfo = true;
  }

  if (Args.hasArg(OPT_prespecialize_generic_metadata) &&
      !Args.hasArg(OPT_disable_generic_metadata_prespecialization)) {
    Opts.PrespecializeGenericMetadata = true;
  }

  if (const Arg *A = Args.getLastArg(OPT_read_legacy_type_info_path_EQ)) {
    Opts.ReadLegacyTypeInfoPath = A->getValue();
  }

  for (const auto &Lib : Args.getAllArgValues(options::OPT_autolink_library))
    Opts.LinkLibraries.push_back(LinkLibrary(Lib, LibraryKind::Library));

  if (const Arg *A = Args.getLastArg(OPT_type_info_dump_filter_EQ)) {
    StringRef mode(A->getValue());
    if (mode == "all")
      Opts.TypeInfoFilter = IRGenOptions::TypeInfoDumpFilter::All;
    else if (mode == "resilient")
      Opts.TypeInfoFilter = IRGenOptions::TypeInfoDumpFilter::Resilient;
    else if (mode == "fragile")
      Opts.TypeInfoFilter = IRGenOptions::TypeInfoDumpFilter::Fragile;
    else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  }

  auto getRuntimeCompatVersion = [&] () -> Optional<llvm::VersionTuple> {
    Optional<llvm::VersionTuple> runtimeCompatibilityVersion;
    if (auto versionArg = Args.getLastArg(
                                  options::OPT_runtime_compatibility_version)) {
      auto version = StringRef(versionArg->getValue());
      if (version.equals("none")) {
        runtimeCompatibilityVersion = None;
      } else if (version.equals("5.0")) {
        runtimeCompatibilityVersion = llvm::VersionTuple(5, 0);
      } else if (version.equals("5.1")) {
        runtimeCompatibilityVersion = llvm::VersionTuple(5, 1);
      } else {
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                       versionArg->getAsString(Args), version);
      }
    } else {
      runtimeCompatibilityVersion =
                           getSwiftRuntimeCompatibilityVersionForTarget(Triple);
    }
    return runtimeCompatibilityVersion;
  };

  // Autolink runtime compatibility libraries, if asked to.
  if (!Args.hasArg(options::OPT_disable_autolinking_runtime_compatibility)) {
    Opts.AutolinkRuntimeCompatibilityLibraryVersion = getRuntimeCompatVersion();
  }

  if (!Args.hasArg(options::
          OPT_disable_autolinking_runtime_compatibility_dynamic_replacements)) {
    Opts.AutolinkRuntimeCompatibilityDynamicReplacementLibraryVersion =
        getRuntimeCompatVersion();
  }

  if (const Arg *A = Args.getLastArg(OPT_num_threads)) {
    if (StringRef(A->getValue()).getAsInteger(10, Opts.NumThreads)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
    if (environmentVariableRequestedMaximumDeterminism()) {
      Opts.NumThreads = 1;
      Diags.diagnose(SourceLoc(), diag::remark_max_determinism_overriding,
                     "-num-threads");
    }
  }

  return false;
}

static std::string getScriptFileName(StringRef name, version::Version &ver) {
  if (ver.isVersionAtLeast(4, 2))
    return (Twine(name) + "42" + ".json").str();
  else
    return (Twine(name) + "4" + ".json").str();
}

static bool ParseMigratorArgs(MigratorOptions &Opts,
                              LangOptions &LangOpts,
                              const FrontendOptions &FrontendOpts,
                              StringRef ResourcePath, const ArgList &Args,
                              DiagnosticEngine &Diags) {
  using namespace options;

  Opts.KeepObjcVisibility |= Args.hasArg(OPT_migrate_keep_objc_visibility);
  Opts.DumpUsr = Args.hasArg(OPT_dump_usr);

  if (Args.hasArg(OPT_disable_migrator_fixits)) {
    Opts.EnableMigratorFixits = false;
  }

  if (auto RemapFilePath = Args.getLastArg(OPT_emit_remap_file_path)) {
    Opts.EmitRemapFilePath = RemapFilePath->getValue();
  }

  if (auto MigratedFilePath = Args.getLastArg(OPT_emit_migrated_file_path)) {
    Opts.EmitMigratedFilePath = MigratedFilePath->getValue();
  }

  if (auto Dumpster = Args.getLastArg(OPT_dump_migration_states_dir)) {
    Opts.DumpMigrationStatesDir = Dumpster->getValue();
  }

  if (auto DataPath = Args.getLastArg(OPT_api_diff_data_file)) {
    Opts.APIDigesterDataStorePaths.push_back(DataPath->getValue());
  } else {
    auto &Triple = LangOpts.Target;

    llvm::SmallString<128> basePath;
    if (auto DataDir = Args.getLastArg(OPT_api_diff_data_dir)) {
      basePath = DataDir->getValue();
    } else {
      basePath = ResourcePath;
      llvm::sys::path::append(basePath, "migrator");
    }

    bool Supported = true;
    llvm::SmallString<128> dataPath(basePath);
    auto &langVer = LangOpts.EffectiveLanguageVersion;
    if (Triple.isMacOSX())
      llvm::sys::path::append(dataPath, getScriptFileName("macos", langVer));
    else if (Triple.isiOS())
      llvm::sys::path::append(dataPath, getScriptFileName("ios", langVer));
    else if (Triple.isTvOS())
      llvm::sys::path::append(dataPath, getScriptFileName("tvos", langVer));
    else if (Triple.isWatchOS())
      llvm::sys::path::append(dataPath, getScriptFileName("watchos", langVer));
    else
      Supported = false;
    if (Supported) {
      llvm::SmallString<128> authoredDataPath(basePath);
      llvm::sys::path::append(authoredDataPath, getScriptFileName("overlay", langVer));
      // Add authored list first to take higher priority.
      Opts.APIDigesterDataStorePaths.push_back(std::string(authoredDataPath.str()));
      Opts.APIDigesterDataStorePaths.push_back(std::string(dataPath.str()));
    }
  }

  if (Opts.shouldRunMigrator()) {
    assert(!FrontendOpts.InputsAndOutputs.isWholeModule());
    // FIXME: In order to support batch mode properly, the migrator would have
    // to support having one remap file path and one migrated file path per
    // primary input. The easiest way to do this would be to move processing of
    // these paths into FrontendOptions, like other supplementary outputs, and
    // to call migrator::updateCodeAndEmitRemapIfNeeded once for each primary
    // file.
    //
    // Supporting WMO would be similar, but WMO is set up to only produce one
    // supplementary output for the whole compilation instead of one per input,
    // so it's probably not worth it.
    FrontendOpts.InputsAndOutputs.assertMustNotBeMoreThanOnePrimaryInput();

    // Always disable typo-correction in the migrator.
    LangOpts.TypoCorrectionLimit = 0;
  }

  return false;
}

bool CompilerInvocation::parseArgs(
    ArrayRef<const char *> Args, DiagnosticEngine &Diags,
    SmallVectorImpl<std::unique_ptr<llvm::MemoryBuffer>>
        *ConfigurationFileBuffers,
    StringRef workingDirectory, StringRef mainExecutablePath) {
  using namespace options;

  if (Args.empty())
    return false;

  // Parse frontend command line options using Swift's option table.
  unsigned MissingIndex;
  unsigned MissingCount;
  std::unique_ptr<llvm::opt::OptTable> Table = createSwiftOptTable();
  llvm::opt::InputArgList ParsedArgs =
      Table->ParseArgs(Args, MissingIndex, MissingCount, FrontendOption);
  if (MissingCount) {
    Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                   ParsedArgs.getArgString(MissingIndex), MissingCount);
    return true;
  }

  if (ParsedArgs.hasArg(OPT_UNKNOWN)) {
    for (const Arg *A : ParsedArgs.filtered(OPT_UNKNOWN)) {
      Diags.diagnose(SourceLoc(), diag::error_unknown_arg,
                     A->getAsString(ParsedArgs));
    }
    return true;
  }

  if (ParseFrontendArgs(FrontendOpts, ParsedArgs, Diags,
                        ConfigurationFileBuffers)) {
    return true;
  }

  if (!mainExecutablePath.empty()) {
    setMainExecutablePath(mainExecutablePath);
  }

  ParseModuleInterfaceArgs(ModuleInterfaceOpts, ParsedArgs);
  SaveModuleInterfaceArgs(ModuleInterfaceOpts, FrontendOpts, ParsedArgs, Diags);

  if (ParseLangArgs(LangOpts, ParsedArgs, Diags, FrontendOpts)) {
    return true;
  }

  if (ParseTypeCheckerArgs(TypeCheckerOpts, ParsedArgs, Diags, FrontendOpts)) {
    return true;
  }

  if (ParseClangImporterArgs(ClangImporterOpts, ParsedArgs, Diags,
                             workingDirectory)) {
    return true;
  }

  if (ParseSearchPathArgs(SearchPathOpts, ParsedArgs, Diags,
                          workingDirectory)) {
    return true;
  }

  if (ParseSILArgs(SILOpts, ParsedArgs, IRGenOpts, FrontendOpts,
                   TypeCheckerOpts, Diags,
                   LangOpts.Target, ClangImporterOpts)) {
    return true;
  }

  if (ParseIRGenArgs(IRGenOpts, ParsedArgs, Diags, FrontendOpts, SILOpts,
                     getSDKPath(), SearchPathOpts.RuntimeResourcePath,
                     LangOpts.Target)) {
    return true;
  }

  if (ParseTBDGenArgs(TBDGenOpts, ParsedArgs, Diags, *this)) {
    return true;
  }

  if (ParseDiagnosticArgs(DiagnosticOpts, ParsedArgs, Diags)) {
    return true;
  }

  if (ParseMigratorArgs(MigratorOpts, LangOpts, FrontendOpts,
                        SearchPathOpts.RuntimeResourcePath, ParsedArgs, Diags)) {
    return true;
  }

  updateRuntimeLibraryPaths(SearchPathOpts, LangOpts.Target);
  setDefaultPrebuiltCacheIfNecessary();

  // Now that we've parsed everything, setup some inter-option-dependent state.
  setIRGenOutputOptsFromFrontendOptions(IRGenOpts, FrontendOpts);
  setBridgingHeaderFromFrontendOptions(ClangImporterOpts, FrontendOpts);

  return false;
}

serialization::Status
CompilerInvocation::loadFromSerializedAST(StringRef data) {
  serialization::ExtendedValidationInfo extendedInfo;
  serialization::ValidationInfo info =
      serialization::validateSerializedAST(data, &extendedInfo);

  if (info.status != serialization::Status::Valid)
    return info.status;

  LangOpts.EffectiveLanguageVersion = info.compatibilityVersion;
  setTargetTriple(info.targetTriple);
  if (!extendedInfo.getSDKPath().empty())
    setSDKPath(extendedInfo.getSDKPath().str());

  auto &extraClangArgs = getClangImporterOptions().ExtraArgs;
  for (StringRef Arg : extendedInfo.getExtraClangImporterOptions())
    extraClangArgs.push_back(Arg.str());

  return info.status;
}

llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>
CompilerInvocation::setUpInputForSILTool(
    StringRef inputFilename, StringRef moduleNameArg,
    bool alwaysSetModuleToMain, bool bePrimary,
    serialization::ExtendedValidationInfo &extendedInfo) {
  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
      llvm::MemoryBuffer::getFileOrSTDIN(inputFilename);
  if (!fileBufOrErr) {
    return fileBufOrErr;
  }

  // If it looks like we have an AST, set the source file kind to SIL and the
  // name of the module to the file's name.
  getFrontendOptions().InputsAndOutputs.addInput(
      InputFile(inputFilename, bePrimary, fileBufOrErr.get().get(), file_types::TY_SIL));

  auto result = serialization::validateSerializedAST(
      fileBufOrErr.get()->getBuffer(), &extendedInfo);
  bool hasSerializedAST = result.status == serialization::Status::Valid;

  if (hasSerializedAST) {
    const StringRef stem = !moduleNameArg.empty()
                               ? moduleNameArg
                               : llvm::sys::path::stem(inputFilename);
    setModuleName(stem);
    getFrontendOptions().InputMode =
        FrontendOptions::ParseInputMode::SwiftLibrary;
  } else {
    const StringRef name = (alwaysSetModuleToMain || moduleNameArg.empty())
                               ? "main"
                               : moduleNameArg;
    setModuleName(name);
    getFrontendOptions().InputMode = FrontendOptions::ParseInputMode::SIL;
  }
  return fileBufOrErr;
}

bool CompilerInvocation::isModuleExternallyConsumed(
    const ModuleDecl *mod) const {
  // Modules for executables aren't expected to be consumed by other modules.
  // This picks up all kinds of entrypoints, including script mode,
  // @UIApplicationMain and @NSApplicationMain.
  if (mod->hasEntryPoint()) {
    return false;
  }

  // If an implicit Objective-C header was needed to construct this module, it
  // must be the product of a library target.
  if (!getFrontendOptions().ImplicitObjCHeaderPath.empty()) {
    return false;
  }

  // App extensions are special beasts because they build without entrypoints
  // like library targets, but they behave like executable targets because
  // their associated modules are not suitable for distribution.
  if (mod->getASTContext().LangOpts.EnableAppExtensionRestrictions) {
    return false;
  }

  // FIXME: This is still a lousy approximation of whether the module file will
  // be externally consumed.
  return true;
}
