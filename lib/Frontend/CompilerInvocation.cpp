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

#include "swift/AST/SILOptions.h"
#include "swift/Frontend/Frontend.h"

#include "ArgsToFrontendOptionsConverter.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Feature.h"
#include "swift/Basic/Platform.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "swift/Parse/ParseVersion.h"
#include "swift/Strings.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/LineIterator.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/WithColor.h"

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

/// Converts a llvm::Triple to a llvm::VersionTuple.
static llvm::VersionTuple
getVersionTuple(const llvm::Triple &triple) {
  if (triple.isMacOSX()) {
    llvm::VersionTuple OSVersion;
    triple.getMacOSXVersion(OSVersion);
    return OSVersion;
  }
  return triple.getOSVersion();
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

  llvm::SmallString<128> clangPath(Path);
  llvm::sys::path::remove_filename(clangPath);
  llvm::sys::path::append(clangPath, "clang");
  ClangImporterOpts.clangPath = std::string(clangPath);

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
  if (!sdkVer.has_value())
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
  if (!FrontendOpts.PrebuiltModuleCachePath.empty())
    return;
  StringRef anchor = "prebuilt-modules";
  assert(((StringRef)FrontendOpts.PrebuiltModuleCachePath).contains(anchor));
  auto pair = ((StringRef)FrontendOpts.PrebuiltModuleCachePath).split(anchor);
  FrontendOpts.BackupModuleInterfaceDir =
    (llvm::Twine(pair.first) + "preferred-interfaces" + pair.second).str();
}

void CompilerInvocation::setDefaultBlocklistsIfNecessary() {
  if (!LangOpts.BlocklistConfigFilePaths.empty())
    return;
  if (SearchPathOpts.RuntimeResourcePath.empty())
    return;
  // XcodeDefault.xctoolchain/usr/lib/swift
  SmallString<64> blocklistDir{SearchPathOpts.RuntimeResourcePath};
  // XcodeDefault.xctoolchain/usr/lib
  llvm::sys::path::remove_filename(blocklistDir);
  // XcodeDefault.xctoolchain/usr
  llvm::sys::path::remove_filename(blocklistDir);
  // XcodeDefault.xctoolchain/usr/local/lib/swift/blocklists
  llvm::sys::path::append(blocklistDir, "local", "lib", "swift", "blocklists");
  std::error_code EC;
  if (llvm::sys::fs::is_directory(blocklistDir)) {
    for (llvm::sys::fs::directory_iterator F(blocklistDir, EC), FE;
         F != FE; F.increment(EC)) {
      StringRef ext = llvm::sys::path::extension(F->path());
      if (ext == "yml" || ext == "yaml") {
        LangOpts.BlocklistConfigFilePaths.push_back(F->path());
      }
    }
  }
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

  // If this is set, we don't want any runtime import paths.
  if (SearchPathOpts.SkipRuntimeLibraryImportPaths) {
    SearchPathOpts.setRuntimeLibraryImportPaths({});
    return;
  }

  // Set up the import paths containing the swiftmodules for the libraries in
  // RuntimeLibraryPath.
  std::vector<std::string> RuntimeLibraryImportPaths;
  RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));

  // This is compatibility for <=5.3
  if (!Triple.isOSDarwin()) {
    llvm::sys::path::append(LibPath, swift::getMajorArchitectureName(Triple));
    RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
  }

  if (!SearchPathOpts.getSDKPath().empty()) {
    if (tripleIsMacCatalystEnvironment(Triple)) {
      LibPath = SearchPathOpts.getSDKPath();
      llvm::sys::path::append(LibPath, "System", "iOSSupport");
      llvm::sys::path::append(LibPath, "usr", "lib", "swift");
      RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
    }

    LibPath = SearchPathOpts.getSDKPath();
    llvm::sys::path::append(LibPath, "usr", "lib", "swift");
    if (!Triple.isOSDarwin()) {
      // Use the non-architecture suffixed form with directory-layout
      // swiftmodules.
      llvm::sys::path::append(LibPath, getPlatformNameForTriple(Triple));
      RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));

      // Compatibility with older releases - use the architecture suffixed form
      // for pre-directory-layout multi-architecture layout.  Note that some
      // platforms (e.g. Windows) will use this even with directory layout in
      // older releases.
      llvm::sys::path::append(LibPath, swift::getMajorArchitectureName(Triple));
    }
    RuntimeLibraryImportPaths.push_back(std::string(LibPath.str()));
  }
  SearchPathOpts.setRuntimeLibraryImportPaths(RuntimeLibraryImportPaths);
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
  SearchPathOpts.setSDKPath(Path);
  updateRuntimeLibraryPaths(SearchPathOpts, LangOpts.Target);
}

bool CompilerInvocation::setModuleAliasMap(std::vector<std::string> args,
                                           DiagnosticEngine &diags) {
  return ModuleAliasesConverter::computeModuleAliases(args, FrontendOpts, diags);
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
  Opts.AliasModuleNames |=
    Args.hasFlag(OPT_alias_module_names_in_module_interface,
                 OPT_disable_alias_module_names_in_module_interface,
                 ::getenv("SWIFT_ALIAS_MODULE_NAMES_IN_INTERFACES"));
  Opts.PrintFullConvention |=
    Args.hasArg(OPT_experimental_print_full_convention);
  Opts.ExperimentalSPIImports |=
    Args.hasArg(OPT_experimental_spi_imports);
  Opts.DebugPrintInvalidSyntax |=
    Args.hasArg(OPT_debug_emit_invalid_swiftinterface_syntax);
  Opts.PrintMissingImports =
    !Args.hasArg(OPT_disable_print_missing_imports_in_module_interface);

  if (const Arg *A = Args.getLastArg(OPT_library_level)) {
    StringRef contents = A->getValue();
    if (contents == "spi") {
      Opts.PrintPrivateInterfaceContent = true;
    }
  }
  for (auto val: Args.getAllArgValues(OPT_skip_import_in_public_interface)) {
    Opts.ModulesToSkipInPublicInterface.push_back(val);
  }
}

/// Checks if an arg is generally allowed to be included
/// in a module interface
static bool ShouldIncludeModuleInterfaceArg(const Arg *A) {
  if (!A->getOption().matches(options::OPT_enable_experimental_feature))
    return true;

  if (auto feature = getExperimentalFeature(A->getValue())) {
    return swift::includeInModuleInterface(*feature);
  }

  return true;
}

/// Save a copy of any flags marked as ModuleInterfaceOption, if running
/// in a mode that is going to emit a .swiftinterface file.
static void SaveModuleInterfaceArgs(ModuleInterfaceOptions &Opts,
                                    FrontendOptions &FOpts,
                                    ArgList &Args, DiagnosticEngine &Diags) {
  if (!FOpts.InputsAndOutputs.hasModuleInterfaceOutputPath())
    return;
  ArgStringList RenderedArgs;
  ArgStringList RenderedArgsIgnorable;
  ArgStringList RenderedArgsIgnorablePrivate;
  for (auto A : Args) {
    if (!ShouldIncludeModuleInterfaceArg(A))
      continue;

    if (A->getOption().hasFlag(options::ModuleInterfaceOptionIgnorablePrivate)) {
      A->render(Args, RenderedArgsIgnorablePrivate);
    } else if (A->getOption().hasFlag(options::ModuleInterfaceOptionIgnorable)) {
      A->render(Args, RenderedArgsIgnorable);
    } else if (A->getOption().hasFlag(options::ModuleInterfaceOption)) {
      A->render(Args, RenderedArgs);
    }
  }
  {
    llvm::raw_string_ostream OS(Opts.Flags);
    interleave(RenderedArgs,
               [&](const char *Argument) { PrintArg(OS, Argument, StringRef()); },
               [&] { OS << " "; });

    // Backward-compatibility hack: disable availability checking in the
    // _Concurrency module, so that older (Swift 5.5) compilers that did not
    // support back deployment of concurrency do not complain about 'async'
    // with older availability.
    if (FOpts.ModuleName == "_Concurrency")
      OS << " -disable-availability-checking";
  }
  {
    llvm::raw_string_ostream OS(Opts.IgnorablePrivateFlags);
    interleave(RenderedArgsIgnorablePrivate,
               [&](const char *Argument) { PrintArg(OS, Argument, StringRef()); },
               [&] { OS << " "; });
  }
  {
    llvm::raw_string_ostream OS(Opts.IgnorableFlags);
    interleave(RenderedArgsIgnorable,
               [&](const char *Argument) { PrintArg(OS, Argument, StringRef()); },
               [&] { OS << " "; });
  }
}

enum class CxxCompatMode {
  invalid,
  enabled,
  off
};

static CxxCompatMode validateCxxInteropCompatibilityMode(StringRef mode) {
  if (mode == "off")
    return CxxCompatMode::off;
  if (mode == "default")
    return CxxCompatMode::enabled;
  // FIXME: Drop swift-5.9.
  if (mode == "swift-5.9")
    return CxxCompatMode::enabled;
  return CxxCompatMode::invalid;
}

static void diagnoseCxxInteropCompatMode(Arg *verArg, ArgList &Args,
                                         DiagnosticEngine &diags) {
  // General invalid argument error
  diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                 verArg->getAsString(Args), verArg->getValue());

  // Note valid C++ interoperability modes.
  auto validVers = {llvm::StringRef("off"), llvm::StringRef("default")};
  auto versStr = "'" + llvm::join(validVers, "', '") + "'";
  diags.diagnose(SourceLoc(), diag::valid_cxx_interop_modes, versStr);
}

static bool ParseLangArgs(LangOptions &Opts, ArgList &Args,
                          DiagnosticEngine &Diags,
                          const FrontendOptions &FrontendOpts) {
  using namespace options;
  bool HadError = false;

  if (auto A = Args.getLastArg(OPT_swift_version)) {
    auto vers =
        VersionParser::parseVersionString(A->getValue(), SourceLoc(), &Diags);
    bool isValid = false;
    if (vers.has_value()) {
      if (auto effectiveVers = vers.value().getEffectiveLanguageVersion()) {
        Opts.EffectiveLanguageVersion = effectiveVers.value();
        isValid = true;
      }
    }
    if (!isValid)
      diagnoseSwiftVersion(vers, A, Args, Diags);
  }

  if (auto A = Args.getLastArg(OPT_package_description_version)) {
    auto vers =
        VersionParser::parseVersionString(A->getValue(), SourceLoc(), &Diags);
    if (vers.has_value()) {
      Opts.PackageDescriptionVersion = vers.value();
    } else {
      return true;
    }
  }

  Opts.AttachCommentsToDecls |= Args.hasArg(OPT_dump_api_path);

  Opts.UseMalloc |= Args.hasArg(OPT_use_malloc);

  Opts.DiagnosticsEditorMode |= Args.hasArg(OPT_diagnostics_editor_mode,
                                            OPT_serialize_diagnostics_path);

  Opts.EnableExperimentalConcurrency |=
    Args.hasArg(OPT_enable_experimental_concurrency);

  Opts.EnableInferPublicSendable |=
    Args.hasFlag(OPT_enable_infer_public_concurrent_value,
                 OPT_disable_infer_public_concurrent_value,
                 false);

  Opts.DisableExperimentalClangImporterDiagnostics |=
      Args.hasArg(OPT_disable_experimental_clang_importer_diagnostics);

  Opts.EnableExperimentalEagerClangModuleDiagnostics |=
      !Args.hasArg(OPT_disable_experimental_clang_importer_diagnostics) &&
      Args.hasArg(OPT_enable_experimental_eager_clang_module_diagnostics);

  Opts.DisableImplicitConcurrencyModuleImport |=
    Args.hasArg(OPT_disable_implicit_concurrency_module_import);

  Opts.DisableImplicitStringProcessingModuleImport |=
    Args.hasArg(OPT_disable_implicit_string_processing_module_import);

  Opts.DisableImplicitBacktracingModuleImport =
    Args.hasFlag(OPT_disable_implicit_backtracing_module_import,
                 OPT_enable_implicit_backtracing_module_import,
                 true);

  if (Args.hasArg(OPT_enable_experimental_async_top_level))
    Diags.diagnose(SourceLoc(), diag::warn_flag_deprecated,
                   "-enable-experimental-async-top-level");

  Opts.DiagnoseInvalidEphemeralnessAsError |=
      Args.hasArg(OPT_enable_invalid_ephemeralness_as_error);

  if (auto A = Args.getLastArg(OPT_enable_deserialization_recovery,
                               OPT_disable_deserialization_recovery)) {
    Opts.EnableDeserializationRecovery
      = A->getOption().matches(OPT_enable_deserialization_recovery);
  }

  if (auto A = Args.getLastArg(OPT_enable_deserialization_safety,
                               OPT_disable_deserialization_safety)) {
    Opts.EnableDeserializationSafety
      = A->getOption().matches(OPT_enable_deserialization_safety);
  } else if (auto A = Args.getLastArg(OPT_enable_access_control,
                                      OPT_disable_access_control)) {
    // Disable deserialization safety along with access control.
    Opts.EnableDeserializationSafety
      = A->getOption().matches(OPT_enable_access_control);
  }

  if (auto A = Args.getLastArg(OPT_enable_access_control,
                               OPT_disable_access_control)) {
    Opts.EnableAccessControl
      = A->getOption().matches(OPT_enable_access_control);
  }

  // Whether '/.../' regex literals are enabled. This implies experimental
  // string processing.
  if (Args.hasArg(OPT_enable_bare_slash_regex)) {
    Opts.EnableBareSlashRegexLiterals = true;
    Opts.EnableExperimentalStringProcessing = true;
  }

  // Experimental string processing.
  if (auto A = Args.getLastArg(OPT_enable_experimental_string_processing,
                               OPT_disable_experimental_string_processing)) {
    Opts.EnableExperimentalStringProcessing =
        A->getOption().matches(OPT_enable_experimental_string_processing);

    // When experimental string processing is explicitly disabled, also disable
    // forward slash regex `/.../`.
    if (!Opts.EnableExperimentalStringProcessing)
      Opts.EnableBareSlashRegexLiterals = false;
  }

  Opts.DisableAvailabilityChecking |=
      Args.hasArg(OPT_disable_availability_checking);
  if (Args.hasArg(OPT_check_api_availability_only))
    Diags.diagnose(SourceLoc(), diag::warn_flag_deprecated,
                   "-check-api-availability-only");

  if (const Arg *A = Args.getLastArg(OPT_unavailable_decl_optimization_EQ)) {
    auto value =
        llvm::StringSwitch<Optional<UnavailableDeclOptimization>>(A->getValue())
            .Case("none", UnavailableDeclOptimization::None)
            .Case("stub", UnavailableDeclOptimization::Stub)
            .Case("complete", UnavailableDeclOptimization::Complete)
            .Default(None);

    if (value)
      Opts.UnavailableDeclOptimizationMode = *value;
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  }

  Opts.WeakLinkAtTarget |= Args.hasArg(OPT_weak_link_at_target);

  if (auto A = Args.getLastArg(OPT_enable_conformance_availability_errors,
                               OPT_disable_conformance_availability_errors)) {
    Opts.EnableConformanceAvailabilityErrors
      = A->getOption().matches(OPT_enable_conformance_availability_errors);
  }

  Opts.WarnOnPotentiallyUnavailableEnumCase |=
      Args.hasArg(OPT_warn_on_potentially_unavailable_enum_case);
  Opts.WarnOnEditorPlaceholder |= Args.hasArg(OPT_warn_on_editor_placeholder);

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

  if (Args.hasArg(OPT_emit_fine_grained_dependency_sourcefile_dot_files))
    Opts.EmitFineGrainedDependencySourcefileDotFiles = true;

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

  Opts.RequireExplicitSendable |= Args.hasArg(OPT_require_explicit_sendable);
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

  // Determine whether string processing is enabled
  Opts.EnableExperimentalStringProcessing =
    Args.hasFlag(OPT_enable_experimental_string_processing,
                 OPT_disable_experimental_string_processing, /*Default=*/true);

  // Add a future feature if it is not already implied by the language version.
  auto addFutureFeatureIfNotImplied = [&](Feature feature) {
    // Check if this feature was introduced already in this language version.
    if (auto firstVersion = getFeatureLanguageVersion(feature)) {
      if (Opts.isSwiftVersionAtLeast(*firstVersion))
        return;
    }

    Opts.Features.insert(feature);
  };

  // Map historical flags over to future features.
  if (Args.hasArg(OPT_enable_experimental_concise_pound_file))
    addFutureFeatureIfNotImplied(Feature::ConciseMagicFile);
  if (Args.hasArg(OPT_enable_bare_slash_regex))
    addFutureFeatureIfNotImplied(Feature::BareSlashRegexLiterals);

  for (const Arg *A : Args.filtered(OPT_enable_experimental_feature)) {
    // If this is a known experimental feature, allow it in +Asserts
    // (non-release) builds for testing purposes.
    if (auto feature = getExperimentalFeature(A->getValue())) {
#ifdef NDEBUG
      if (!isFeatureAvailableInProduction(*feature)) {
        Diags.diagnose(SourceLoc(),
                       diag::error_experimental_feature_not_available,
                       A->getValue());
      }
#endif

      Opts.Features.insert(*feature);
    }

    // Hack: In order to support using availability macros in SPM packages, we
    // need to be able to use:
    //    .enableExperimentalFeature("AvailabilityMacro='...'")
    // within the package manifest and the feature recognizer can't recognize
    // this form of feature, so specially handle it here until features can
    // maybe have extra arguments in the future.
    auto strRef = StringRef(A->getValue());
    if (strRef.startswith("AvailabilityMacro=")) {
      auto availability = strRef.split("=").second;

      Opts.AvailabilityMacros.push_back(availability.str());
    }
  }

  // Map historical flags over to future features.
  for (const Arg *A : Args.filtered(OPT_enable_upcoming_feature)) {
    // Ignore unknown features.
    auto feature = getUpcomingFeature(A->getValue());
    if (!feature)
      continue;

    // Check if this feature was introduced already in this language version.
    if (auto firstVersion = getFeatureLanguageVersion(*feature)) {
      if (Opts.isSwiftVersionAtLeast(*firstVersion)) {
        Diags.diagnose(SourceLoc(), diag::error_upcoming_feature_on_by_default,
                       A->getValue(), *firstVersion);
        continue;
      }
    }

    // Add the feature.
    Opts.Features.insert(*feature);
  }

  // Map historical flags over to experimental features. We do this for all
  // compilers because that's how existing experimental feature flags work.
  if (Args.hasArg(OPT_enable_experimental_static_assert))
    Opts.Features.insert(Feature::StaticAssert);
  if (Args.hasArg(OPT_enable_experimental_named_opaque_types))
    Opts.Features.insert(Feature::NamedOpaqueTypes);
  if (Args.hasArg(OPT_enable_experimental_flow_sensitive_concurrent_captures))
    Opts.Features.insert(Feature::FlowSensitiveConcurrencyCaptures);
  if (Args.hasArg(OPT_enable_experimental_move_only)) {
    // FIXME: drop addition of Feature::MoveOnly once its queries are gone.
    Opts.Features.insert(Feature::MoveOnly);
    Opts.Features.insert(Feature::NoImplicitCopy);
    Opts.Features.insert(Feature::OldOwnershipOperatorSpellings);
  }
  if (Args.hasArg(OPT_experimental_one_way_closure_params))
    Opts.Features.insert(Feature::OneWayClosureParameters);
  if (Args.hasArg(OPT_enable_experimental_associated_type_inference))
    Opts.Features.insert(Feature::TypeWitnessSystemInference);
  if (Args.hasArg(OPT_enable_experimental_forward_mode_differentiation))
    Opts.Features.insert(Feature::ForwardModeDifferentiation);
  if (Args.hasArg(OPT_enable_experimental_additive_arithmetic_derivation))
    Opts.Features.insert(Feature::AdditiveArithmeticDerivedConformances);
  
  if (Args.hasArg(OPT_enable_experimental_opaque_type_erasure))
    Opts.Features.insert(Feature::OpaqueTypeErasure);

  if (Args.hasArg(OPT_enable_builtin_module))
    Opts.Features.insert(Feature::BuiltinModule);

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
    } else if (contents == "ipi") {
      Opts.LibraryLevel = LibraryLevel::IPI;
    } else {
      Opts.LibraryLevel = LibraryLevel::Other;
      if (contents != "other") {
        // Error on unknown library levels.
        Diags.diagnose(SourceLoc(),
                       diag::error_unknown_library_level,
                       contents);
      }
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_package_name)) {
    auto pkgName = A->getValue();
    if (StringRef(pkgName).empty())
      Diags.diagnose(SourceLoc(), diag::error_empty_package_name);
    else
      Opts.PackageName = pkgName;
  }

  if (const Arg *A = Args.getLastArg(OPT_require_explicit_availability_EQ)) {
    StringRef diagLevel = A->getValue();
    if (diagLevel == "warn") {
      Opts.RequireExplicitAvailability = DiagnosticBehavior::Warning;
    } else if (diagLevel == "error") {
      Opts.RequireExplicitAvailability = DiagnosticBehavior::Error;
    } else if (diagLevel == "ignore") {
      Opts.RequireExplicitAvailability = None;
    } else {
      Diags.diagnose(SourceLoc(),
                     diag::error_unknown_require_explicit_availability,
                     diagLevel);
    }
  } else if (Args.getLastArg(OPT_require_explicit_availability,
                             OPT_require_explicit_availability_target) ||
             Opts.LibraryLevel == LibraryLevel::API) {
    Opts.RequireExplicitAvailability = DiagnosticBehavior::Warning;
  }

  if (const Arg *A = Args.getLastArg(OPT_require_explicit_availability_target)) {
    Opts.RequireExplicitAvailabilityTarget = A->getValue();
  }

  Opts.EnableSPIOnlyImports = Args.hasArg(OPT_experimental_spi_only_imports);

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

  // Swift 6+ uses the strictest concurrency level.
  if (Opts.isSwiftVersionAtLeast(6)) {
    Opts.StrictConcurrencyLevel = StrictConcurrency::Complete;
  } else if (const Arg *A = Args.getLastArg(OPT_strict_concurrency)) {
    auto value = llvm::StringSwitch<Optional<StrictConcurrency>>(A->getValue())
      .Case("minimal", StrictConcurrency::Minimal)
      .Case("targeted", StrictConcurrency::Targeted)
      .Case("complete", StrictConcurrency::Complete)
      .Default(None);

    if (value)
      Opts.StrictConcurrencyLevel = *value;
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());

  } else if (Args.hasArg(OPT_warn_concurrency)) {
    Opts.StrictConcurrencyLevel = StrictConcurrency::Complete;
  } else {
    // Default to minimal checking in Swift 5.x.
    Opts.StrictConcurrencyLevel = StrictConcurrency::Minimal;
  }

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

  if (Arg *A = Args.getLastArg(OPT_Raccess_note)) {
    auto value = llvm::StringSwitch<Optional<AccessNoteDiagnosticBehavior>>(A->getValue())
      .Case("none", AccessNoteDiagnosticBehavior::Ignore)
      .Case("failures", AccessNoteDiagnosticBehavior::RemarkOnFailure)
      .Case("all", AccessNoteDiagnosticBehavior::RemarkOnFailureOrSuccess)
      .Case("all-validate", AccessNoteDiagnosticBehavior::ErrorOnFailureRemarkOnSuccess)
      .Default(None);

    if (value)
      Opts.AccessNoteBehavior = *value;
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  }

  Opts.EnableCrossImportOverlays =
      Args.hasFlag(OPT_enable_cross_import_overlays,
                   OPT_disable_cross_import_overlays,
                   Opts.EnableCrossImportOverlays);

  Opts.EnableCrossImportRemarks = Args.hasArg(OPT_emit_cross_import_remarks);

  Opts.EnableModuleLoadingRemarks = Args.hasArg(OPT_remark_loading_module);

  Opts.EnableIndexingSystemModuleRemarks = Args.hasArg(OPT_remark_indexing_system_module);

  Opts.EnableSkipExplicitInterfaceModuleBuildRemarks = Args.hasArg(OPT_remark_skip_explicit_interface_build);
  
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

  // Collect -clang-target value if specified in the front-end invocation.
  // Usually, the driver will pass down a clang target with the
  // exactly same value as the main target, so we could diagnose the usage of
  // unavailable APIs.
  // The reason we cannot infer clang target from -target is that not all
  // front-end invocation will include a -target to start with. For instance,
  // when compiling a Swift module from a textual interface, -target isn't
  // necessary because the textual interface hardcoded the proper target triple
  // to use. Inferring -clang-target there will always give us the default
  // target triple.
  if (const Arg *A = Args.getLastArg(OPT_clang_target)) {
    Opts.ClangTarget = llvm::Triple(A->getValue());
  }

  if (Arg *A = Args.getLastArg(OPT_cxx_interoperability_mode)) {
    if (Args.hasArg(OPT_enable_experimental_cxx_interop)) {
      Diags.diagnose(SourceLoc(), diag::dont_enable_interop_and_compat);
    }
    
    auto interopCompatMode = validateCxxInteropCompatibilityMode(A->getValue());
    Opts.EnableCXXInterop |= (interopCompatMode == CxxCompatMode::enabled);

    if (interopCompatMode == CxxCompatMode::invalid)
      diagnoseCxxInteropCompatMode(A, Args, Diags);
  }
  
  if (Args.hasArg(OPT_enable_experimental_cxx_interop)) {
    Diags.diagnose(SourceLoc(), diag::enable_interop_flag_deprecated);
    Diags.diagnose(SourceLoc(), diag::swift_will_maintain_compat);
    Opts.EnableCXXInterop |= true;
  }

  Opts.EnableObjCInterop =
      Args.hasFlag(OPT_enable_objc_interop, OPT_disable_objc_interop,
                   Target.isOSDarwin());

  Opts.CForeignReferenceTypes =
      Args.hasArg(OPT_experimental_c_foreign_reference_types);

  Opts.CxxInteropGettersSettersAsProperties = Args.hasArg(OPT_cxx_interop_getters_setters_as_properties);

  Opts.VerifyAllSubstitutionMaps |= Args.hasArg(OPT_verify_all_substitution_maps);

  Opts.EnableVolatileModules |= Args.hasArg(OPT_enable_volatile_modules);

  Opts.HermeticSealAtLink |= Args.hasArg(OPT_experimental_hermetic_seal_at_link);

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

  // First, set up default minimum inlining target versions.
  auto getDefaultMinimumInliningTargetVersion =
      [&](const llvm::Triple &triple) -> llvm::VersionTuple {
    const auto targetVersion = getVersionTuple(triple);
    
    // In API modules, default to the version when Swift first became available.
    if (Opts.LibraryLevel == LibraryLevel::API) {
      if (auto minVersion = minimumAvailableOSVersionForTriple(triple))
        return *minVersion;
    }

    // In other modules, assume that availability is used less consistently
    // and that library clients will generally raise deployment targets as the
    // library evolves so the min inlining version should be the deployment
    // target by default.
    return targetVersion;
  };

  Opts.MinimumInliningTargetVersion =
      getDefaultMinimumInliningTargetVersion(Opts.Target);

  // Parse OS version number arguments.
  auto parseVersionArg = [&](OptSpecifier opt) -> Optional<llvm::VersionTuple> {
    Arg *A = Args.getLastArg(opt);
    if (!A)
      return None;

    if (StringRef(A->getValue()) == "min")
      return minimumAvailableOSVersionForTriple(Opts.Target);
    if (StringRef(A->getValue()) == "target")
      return Opts.getMinPlatformVersion();

    if (auto vers = VersionParser::parseVersionString(A->getValue(),
                                                      SourceLoc(), &Diags))
      return (llvm::VersionTuple)*vers;

    Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                   A->getAsString(Args), A->getValue());
    return None;
  };

  if (auto vers = parseVersionArg(OPT_min_inlining_target_version))
    // FIXME: Should we diagnose if it's below the default?
    Opts.MinimumInliningTargetVersion = *vers;

  if (auto vers = parseVersionArg(OPT_target_sdk_version))
    Opts.SDKVersion = *vers;

  if (auto vers = parseVersionArg(OPT_target_variant_sdk_version))
    Opts.VariantSDKVersion = *vers;

  // Get the SDK name.
  if (Arg *A = Args.getLastArg(options::OPT_target_sdk_name)) {
    Opts.SDKName = A->getValue();
  }

  if (const Arg *A = Args.getLastArg(OPT_entry_point_function_name)) {
    Opts.entryPointFunctionName = A->getValue();
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

  Opts.DisableSubstSILFunctionTypes =
      Args.hasArg(OPT_disable_subst_sil_function_types);

  Opts.DumpRequirementMachine = Args.hasArg(
      OPT_dump_requirement_machine);
  Opts.AnalyzeRequirementMachine = Args.hasArg(
      OPT_analyze_requirement_machine);

  Opts.DumpMacroExpansions = Args.hasArg(
      OPT_dump_macro_expansions);

  if (const Arg *A = Args.getLastArg(OPT_debug_requirement_machine))
    Opts.DebugRequirementMachine = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_requirement_machine_max_rule_count)) {
    unsigned limit;
    if (StringRef(A->getValue()).getAsInteger(10, limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.RequirementMachineMaxRuleCount = limit;
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_requirement_machine_max_rule_length)) {
    unsigned limit;
    if (StringRef(A->getValue()).getAsInteger(10, limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.RequirementMachineMaxRuleLength = limit;
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_requirement_machine_max_concrete_nesting)) {
    unsigned limit;
    if (StringRef(A->getValue()).getAsInteger(10, limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.RequirementMachineMaxConcreteNesting = limit;
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_requirement_machine_max_split_concrete_equiv_class_attempts)) {
    unsigned limit;
    if (StringRef(A->getValue()).getAsInteger(10, limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      HadError = true;
    } else {
      Opts.RequirementMachineMaxSplitConcreteEquivClassAttempts = limit;
    }
  }

  if (Args.hasArg(OPT_disable_requirement_machine_concrete_contraction))
    Opts.EnableRequirementMachineConcreteContraction = false;

  if (Args.hasArg(OPT_disable_requirement_machine_loop_normalization))
    Opts.EnableRequirementMachineLoopNormalization = false;

  if (Args.hasArg(OPT_disable_requirement_machine_reuse))
    Opts.EnableRequirementMachineReuse = false;

  if (Args.hasArg(OPT_enable_requirement_machine_opaque_archetypes))
    Opts.EnableRequirementMachineOpaqueArchetypes = true;

  if (Args.hasArg(OPT_warn_redundant_requirements))
    Opts.WarnRedundantRequirements = true;

  Opts.DumpTypeWitnessSystems = Args.hasArg(OPT_dump_type_witness_systems);

  for (auto &block: FrontendOpts.BlocklistConfigFilePaths)
    Opts.BlocklistConfigFilePaths.push_back(block);
  if (const Arg *A = Args.getLastArg(options::OPT_concurrency_model)) {
    Opts.ActiveConcurrencyModel =
        llvm::StringSwitch<ConcurrencyModel>(A->getValue())
            .Case("standard", ConcurrencyModel::Standard)
            .Case("task-to-thread", ConcurrencyModel::TaskToThread)
            .Default(ConcurrencyModel::Standard);
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

  Opts.PrintFullConvention |=
      Args.hasArg(OPT_experimental_print_full_convention);

  Opts.DebugConstraintSolver |= Args.hasArg(OPT_debug_constraints);

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

  Opts.DebugGenericSignatures |= Args.hasArg(OPT_debug_generic_signatures);

  return HadError;
}

static bool ValidateModulesOnceOptions(const ClangImporterOptions &Opts,
                                       DiagnosticEngine &Diags) {
  if (Opts.ValidateModulesOnce && Opts.BuildSessionFilePath.empty()) {
    Diags.diagnose(SourceLoc(), diag::error_clang_validate_once_requires_session_file);
    return true;
  }
  return false;
}

static bool ParseClangImporterArgs(ClangImporterOptions &Opts,
                                   ArgList &Args,
                                   DiagnosticEngine &Diags,
                                   StringRef workingDirectory,
                                   const LangOptions &LangOpts,
                                   const FrontendOptions &FrontendOpts) {
  using namespace options;

  if (const Arg *a = Args.getLastArg(OPT_tools_directory)) {
    // If a custom tools directory is specified, try to find Clang there.
    // This is useful when the Swift executable is located in a different
    // directory than the Clang/LLVM executables, for example, when building
    // the Swift project itself.
    llvm::SmallString<128> clangPath(a->getValue());
    llvm::sys::path::append(clangPath, "clang");
    if (llvm::sys::fs::exists(clangPath)) {
      Opts.clangPath = std::string(clangPath);
    }
  }

  if (const Arg *A = Args.getLastArg(OPT_module_cache_path)) {
    Opts.ModuleCachePath = A->getValue();
  }

  if (const Arg *A = Args.getLastArg(OPT_target_cpu))
    Opts.TargetCPU = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_index_store_path))
    Opts.IndexStorePath = A->getValue();

  for (const Arg *A : Args.filtered(OPT_Xcc)) {
    StringRef clangArg = A->getValue();
    if (clangArg.consume_front("-working-directory")) {
      if (!clangArg.empty() && clangArg.front() != '=') {
        // Have an old -working-directory<path> argument. Convert it into
        // two separate arguments as Clang no longer supports that format.
        Opts.ExtraArgs.push_back("-working-directory");
        Opts.ExtraArgs.push_back(clangArg.str());
        continue;
      }
    }
    Opts.ExtraArgs.push_back(A->getValue());
  }

  for (const Arg *A : Args.filtered(OPT_file_prefix_map,
                                    OPT_debug_prefix_map)) {
    std::string Val(A->getValue());
    // Forward -debug-prefix-map arguments from Swift to Clang as
    // -fdebug-prefix-map= and -file-prefix-map as -ffile-prefix-map=.
    //
    // This is required to ensure DIFiles created there, like
    /// "<swift-imported-modules>", as well as index data, have their paths
    // remapped properly.
    //
    // (Note, however, that Clang's usage of std::map means that the remapping
    // may not be applied in the same order, which can matter if one mapping is
    // a prefix of another.)
    if (A->getOption().matches(OPT_file_prefix_map))
      Opts.ExtraArgs.push_back("-ffile-prefix-map=" + Val);
    else
      Opts.ExtraArgs.push_back("-fdebug-prefix-map=" + Val);
  }

  if (!workingDirectory.empty()) {
    // Provide a working directory to Clang as well if there are any -Xcc
    // options, in case some of them are search-related. But do it at the
    // beginning, so that an explicit -Xcc -working-directory will win.
    Opts.ExtraArgs.insert(Opts.ExtraArgs.begin(),
                          {"-working-directory", workingDirectory.str()});
  }

  Opts.DumpClangDiagnostics |= Args.hasArg(OPT_dump_clang_diagnostics);

  // When the repl is invoked directly (ie. `lldb --repl="..."`) the action
  // type seems to be NoneAction.
  if (FrontendOpts.RequestedAction != FrontendOptions::ActionType::REPL &&
      FrontendOpts.RequestedAction != FrontendOptions::ActionType::NoneAction &&
      LangOpts.hasFeature(Feature::ImportObjcForwardDeclarations)) {
    Opts.ImportForwardDeclarations = true;
  }

  if (Args.hasArg(OPT_embed_bitcode))
    Opts.Mode = ClangImporterOptions::Modes::EmbedBitcode;
  else if (Args.hasArg(OPT_emit_pcm) || Args.hasArg(OPT_dump_pcm))
    Opts.Mode = ClangImporterOptions::Modes::PrecompiledModule;

  if (auto *A = Args.getLastArg(OPT_import_objc_header))
    Opts.BridgingHeader = A->getValue();
  Opts.DisableSwiftBridgeAttr |= Args.hasArg(OPT_disable_swift_bridge_attr);

  Opts.DisableOverlayModules |= Args.hasArg(OPT_emit_imported_modules);

  if (Args.hasArg(OPT_disable_clang_spi)) {
    Opts.EnableClangSPI = false;
  }

  Opts.ExtraArgsOnly |= Args.hasArg(OPT_extra_clang_options_only);
  Opts.DirectClangCC1ModuleBuild |= Args.hasArg(OPT_direct_clang_cc1_module_build);

  if (const Arg *A = Args.getLastArg(OPT_pch_output_dir)) {
    Opts.PrecompiledHeaderOutputDir = A->getValue();
    Opts.PCHDisableValidation |= Args.hasArg(OPT_pch_disable_validation);
  }

  if (FrontendOpts.DisableImplicitModules)
    Opts.DisableImplicitClangModules = true;

  Opts.ValidateModulesOnce |= Args.hasArg(OPT_validate_clang_modules_once);
  if (auto *A = Args.getLastArg(OPT_clang_build_session_file))
    Opts.BuildSessionFilePath = A->getValue();
  if (ValidateModulesOnceOptions(Opts, Diags))
    return true;

  if (Args.hasFlag(options::OPT_warnings_as_errors,
                   options::OPT_no_warnings_as_errors, false))
    Opts.ExtraArgs.push_back("-Werror");

  Opts.DebuggerSupport |= Args.hasArg(OPT_debugger_support);

  Opts.DisableSourceImport |=
      Args.hasArg(OPT_disable_clangimporter_source_import);

  // Forward the FrontendOptions to clang importer option so it can be
  // accessed when creating clang module compilation invocation.
  if (FrontendOpts.EnableCAS)
    Opts.CASPath = FrontendOpts.CASPath;

  return false;
}

static void ParseSymbolGraphArgs(symbolgraphgen::SymbolGraphOptions &Opts,
                                 ArgList &Args,
                                 DiagnosticEngine &Diags,
                                 LangOptions &LangOpts) {
  using namespace options;

  if (const Arg *A = Args.getLastArg(OPT_emit_symbol_graph_dir)) {
    Opts.OutputDir = A->getValue();
  }

  Opts.Target = LangOpts.Target;

  Opts.SkipInheritedDocs = Args.hasArg(OPT_skip_inherited_docs);
  Opts.SkipProtocolImplementations = Args.hasArg(OPT_skip_protocol_implementations);
  Opts.IncludeSPISymbols = Args.hasArg(OPT_include_spi_symbols);
  Opts.EmitExtensionBlockSymbols =
      Args.hasFlag(OPT_emit_extension_block_symbols,
                   OPT_omit_extension_block_symbols, /*default=*/false);

  if (auto *A = Args.getLastArg(OPT_symbol_graph_minimum_access_level)) {
    Opts.MinimumAccessLevel =
        llvm::StringSwitch<AccessLevel>(A->getValue())
            .Case("open", AccessLevel::Open)
            .Case("public", AccessLevel::Public)
            .Case("package", AccessLevel::Package)
            .Case("internal", AccessLevel::Internal)
            .Case("fileprivate", AccessLevel::FilePrivate)
            .Case("private", AccessLevel::Private)
            .Default(AccessLevel::Public);
  } else {
    Opts.MinimumAccessLevel = AccessLevel::Public;
  }

  // default values for generating symbol graphs during a build
  Opts.PrettyPrint = false;
  Opts.EmitSynthesizedMembers = true;
  Opts.PrintMessages = false;
  Opts.IncludeClangDocs = false;
}

static bool validateSwiftModuleFileArgumentAndAdd(const std::string &swiftModuleArgument,
                                                  DiagnosticEngine &Diags,
                                                  std::vector<std::pair<std::string, std::string>> &ExplicitSwiftModuleInputs) {
  std::size_t foundDelimeterPos = swiftModuleArgument.find_first_of("=");
  if (foundDelimeterPos == std::string::npos) {
    Diags.diagnose(SourceLoc(), diag::error_swift_module_file_requires_delimeter,
                   swiftModuleArgument);
    return true;
  }
  std::string moduleName = swiftModuleArgument.substr(0, foundDelimeterPos),
              modulePath = swiftModuleArgument.substr(foundDelimeterPos+1);
  if (!Lexer::isIdentifier(moduleName)) {
    Diags.diagnose(SourceLoc(), diag::error_bad_module_name, moduleName, false);
    return true;
  }
  ExplicitSwiftModuleInputs.emplace_back(std::make_pair(moduleName, modulePath));
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

  std::vector<std::string> ImportSearchPaths(Opts.getImportSearchPaths());
  for (const Arg *A : Args.filtered(OPT_I)) {
    ImportSearchPaths.push_back(resolveSearchPath(A->getValue()));
  }
  Opts.setImportSearchPaths(ImportSearchPaths);

  std::vector<SearchPathOptions::FrameworkSearchPath> FrameworkSearchPaths(
      Opts.getFrameworkSearchPaths());
  for (const Arg *A : Args.filtered(OPT_F, OPT_Fsystem)) {
    FrameworkSearchPaths.push_back(
        {resolveSearchPath(A->getValue()),
         /*isSystem=*/A->getOption().getID() == OPT_Fsystem});
  }
  Opts.setFrameworkSearchPaths(FrameworkSearchPaths);

  for (const Arg *A : Args.filtered(OPT_plugin_path)) {
    Opts.PluginSearchPaths.push_back(resolveSearchPath(A->getValue()));
  }

  for (const Arg *A : Args.filtered(OPT_external_plugin_path)) {
    // '<plugin directory>#<plugin server executable path>'.
    // FIXME: '#' can be used in the paths.
    StringRef dylibPath;
    StringRef serverPath;
    std::tie(dylibPath, serverPath) = StringRef(A->getValue()).split('#');
    Opts.ExternalPluginSearchPaths.push_back(
        {resolveSearchPath(dylibPath), resolveSearchPath(serverPath)});
  }

  for (const Arg *A : Args.filtered(OPT_L)) {
    Opts.LibrarySearchPaths.push_back(resolveSearchPath(A->getValue()));
  }

  for (const Arg *A : Args.filtered(OPT_vfsoverlay)) {
    Opts.VFSOverlayFiles.push_back(resolveSearchPath(A->getValue()));
  }

  if (const Arg *A = Args.getLastArg(OPT_sdk))
    Opts.setSDKPath(A->getValue());

  if (const Arg *A = Args.getLastArg(OPT_windows_sdk_root))
    Opts.setWinSDKRoot(A->getValue());
  if (const Arg *A = Args.getLastArg(OPT_windows_sdk_version))
    Opts.setWinSDKVersion(A->getValue());
  if (const Arg *A = Args.getLastArg(OPT_visualc_tools_root))
    Opts.setVCToolsRoot(A->getValue());
  if (const Arg *A = Args.getLastArg(OPT_visualc_tools_version))
    Opts.setVCToolsVersion(A->getValue());

  if (const Arg *A = Args.getLastArg(OPT_resource_dir))
    Opts.RuntimeResourcePath = A->getValue();

  Opts.SkipRuntimeLibraryImportPaths |= Args.hasArg(OPT_nostdimport);

  Opts.DisableModulesValidateSystemDependencies |=
      Args.hasArg(OPT_disable_modules_validate_system_headers);

  if (const Arg *A = Args.getLastArg(OPT_explicit_swift_module_map))
    Opts.ExplicitSwiftModuleMap = A->getValue();
  for (auto A : Args.getAllArgValues(options::OPT_swift_module_file)) {
    if (validateSwiftModuleFileArgumentAndAdd(A, Diags,
                                              Opts.ExplicitSwiftModuleInputs))
      return true;
  }
  for (auto A: Args.filtered(OPT_candidate_module_file)) {
    Opts.CandidateCompiledModules.push_back(resolveSearchPath(A->getValue()));
  }
  if (const Arg *A = Args.getLastArg(OPT_placeholder_dependency_module_map))
    Opts.PlaceholderDependencyModuleMap = A->getValue();
  if (const Arg *A = Args.getLastArg(OPT_batch_scan_input_file))
    Opts.BatchScanInputFilePath = A->getValue();

  if (const Arg *A = Args.getLastArg(OPT_const_gather_protocols_file))
    Opts.ConstGatherProtocolListFilePath = A->getValue();

  for (auto A : Args.getAllArgValues(options::OPT_serialized_path_obfuscate)) {
    auto SplitMap = StringRef(A).split('=');
    Opts.DeserializedPathRecoverer.addMapping(SplitMap.first, SplitMap.second);
  }
  // Opts.RuntimeIncludePath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath().
  // Opts.RuntimeImportPath is set by calls to
  // setRuntimeIncludePath() or setMainExecutablePath() and
  // updated by calls to setTargetTriple() or parseArgs().
  // Assumes exactly one of setMainExecutablePath() or setRuntimeIncludePath()
  // is called before setTargetTriple() and parseArgs().
  // TODO: improve the handling of RuntimeIncludePath.

  std::vector<std::string> CompilerPluginLibraryPaths(
      Opts.getCompilerPluginLibraryPaths());
  for (const Arg *A : Args.filtered(OPT_load_plugin_library)) {
    CompilerPluginLibraryPaths.push_back(resolveSearchPath(A->getValue()));
  }
  Opts.setCompilerPluginLibraryPaths(CompilerPluginLibraryPaths);

  std::vector<PluginExecutablePathAndModuleNames> CompilerPluginExecutablePaths(
      Opts.getCompilerPluginExecutablePaths());
  for (const Arg *A : Args.filtered(OPT_load_plugin_executable)) {
    // 'A' is '<path to executable>#<module names>' where the module names are
    // comma separated.
    StringRef path;
    StringRef modulesStr;
    std::tie(path, modulesStr) = StringRef(A->getValue()).rsplit('#');
    std::vector<std::string> moduleNames;
    for (auto name : llvm::split(modulesStr, ',')) {
      moduleNames.emplace_back(name);
    }
    if (path.empty() || moduleNames.empty()) {
      Diags.diagnose(SourceLoc(), diag::error_load_plugin_executable,
                     A->getValue());
    } else {
      CompilerPluginExecutablePaths.push_back(
          {resolveSearchPath(path), std::move(moduleNames)});
    }
  }
  Opts.setCompilerPluginExecutablePaths(
      std::move(CompilerPluginExecutablePaths));

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

  for (const Arg *arg: Args.filtered(OPT_emit_macro_expansion_files)) {
    StringRef contents = arg->getValue();
    bool negated = contents.startswith("no-");
    if (negated)
      contents = contents.drop_front(3);
    if (contents == "diagnostics")
      Opts.EmitMacroExpansionFiles = !negated;
  }

  Opts.FixitCodeForAllDiagnostics |= Args.hasArg(OPT_fixit_all);
  Opts.SuppressWarnings |= Args.hasArg(OPT_suppress_warnings);
  Opts.SuppressRemarks |= Args.hasArg(OPT_suppress_remarks);
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
      llvm::sys::path::replace_extension(localizationPath, ".strings");
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

  Optional<DestroyHoistingOption> specifiedDestroyHoistingOption;
  if (Arg *A = Args.getLastArg(OPT_enable_destroy_hoisting)) {
    specifiedDestroyHoistingOption = 
        llvm::StringSwitch<Optional<DestroyHoistingOption>>(A->getValue())
          .Case("true", DestroyHoistingOption::On)
          .Case("false", DestroyHoistingOption::Off)
          .Default(None);
  }

  Optional<CopyPropagationOption> specifiedCopyPropagationOption;
  if (Arg *A = Args.getLastArg(OPT_copy_propagation_state_EQ)) {
    specifiedCopyPropagationOption =
        llvm::StringSwitch<Optional<CopyPropagationOption>>(A->getValue())
            .Case("true", CopyPropagationOption::On)
            .Case("false", CopyPropagationOption::Off)
            .Case("requested-passes-only",
                  CopyPropagationOption::RequestedPassesOnly)
            .Default(None);
  }
  if (Args.hasArg(OPT_enable_copy_propagation)) {
    if (specifiedCopyPropagationOption) {
      if (*specifiedCopyPropagationOption == CopyPropagationOption::Off) {
        // Error if copy propagation has been set to ::Off via the meta-var form
        // and enabled via the flag.
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_combination,
                       "enable-copy-propagation",
                       "enable-copy-propagation=false");
        return true;
      } else if (*specifiedCopyPropagationOption ==
                 CopyPropagationOption::RequestedPassesOnly) {
        // Error if copy propagation has been set to ::RequestedPassesOnly via
        // the meta-var form and enabled via the flag.
        Diags.diagnose(SourceLoc(), diag::error_invalid_arg_combination,
                       "enable-copy-propagation",
                       "enable-copy-propagation=requested-passes-only");
        return true;
      }
    } else {
      specifiedCopyPropagationOption = CopyPropagationOption::On;
    }
  }
  if (specifiedCopyPropagationOption) {
    Opts.CopyPropagation = *specifiedCopyPropagationOption;
  }

  Optional<bool> enableLexicalBorrowScopesFlag;
  if (Arg *A = Args.getLastArg(OPT_enable_lexical_borrow_scopes)) {
    enableLexicalBorrowScopesFlag =
        llvm::StringSwitch<Optional<bool>>(A->getValue())
            .Case("true", true)
            .Case("false", false)
            .Default(None);
  }

  // Allow command line flags to override the default value of
  // Opts.LexicalLifetimes. If no explicit flags are passed, then
  // Opts.LexicalLifetimes retains its initial value.
  Optional<bool> enableLexicalLifetimesFlag;
  if (Arg *A = Args.getLastArg(OPT_enable_lexical_lifetimes)) {
    enableLexicalLifetimesFlag =
        llvm::StringSwitch<Optional<bool>>(A->getValue())
            .Case("true", true)
            .Case("false", false)
            .Default(None);
  }
  if (Args.getLastArg(OPT_enable_lexical_lifetimes_noArg)) {
    if (!enableLexicalLifetimesFlag.value_or(true)) {
      // Error if lexical lifetimes have been disabled via the meta-var form
      // and enabled via the flag.
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_combination,
                     "enable-lexical-lifetimes",
                     "enable-lexical-lifetimes=false");
      return true;
    } else {
      enableLexicalLifetimesFlag = true;
    }
  }

  if (enableLexicalLifetimesFlag.value_or(false) &&
      !enableLexicalBorrowScopesFlag.value_or(true)) {
    // Error if lexical lifetimes have been enabled but lexical borrow scopes--
    // on which they are dependent--have been disabled.
    Diags.diagnose(SourceLoc(), diag::error_invalid_arg_combination,
                   "enable-lexical-lifetimes=true",
                   "enable-lexical-borrow-scopes=false");
    return true;
  }

  if (Args.hasArg(OPT_enable_experimental_move_only) &&
      !enableLexicalBorrowScopesFlag.value_or(true)) {
    // Error if move-only is enabled and lexical borrow scopes--on which it
    // depends--has been disabled.
    Diags.diagnose(SourceLoc(), diag::error_invalid_arg_combination,
                   "enable-experimental-move-only",
                   "enable-lexical-borrow-scopes=false");
    return true;
  }

  // Unless overridden below, enabling copy propagation means enabling lexical
  // lifetimes.
  if (Opts.CopyPropagation == CopyPropagationOption::On) {
    Opts.LexicalLifetimes = LexicalLifetimesOption::On;
    Opts.DestroyHoisting = DestroyHoistingOption::On;
  }

  // Unless overridden below, disable copy propagation means disabling lexical
  // lifetimes.
  if (Opts.CopyPropagation == CopyPropagationOption::Off) {
    Opts.LexicalLifetimes = LexicalLifetimesOption::DiagnosticMarkersOnly;
    Opts.DestroyHoisting = DestroyHoistingOption::Off;
  }

  // If move-only is enabled, always enable lexical lifetime as well.  Move-only
  // depends on lexical lifetimes.
  if (Args.hasArg(OPT_enable_experimental_move_only))
    Opts.LexicalLifetimes = LexicalLifetimesOption::On;

  if (enableLexicalLifetimesFlag) {
    if (*enableLexicalLifetimesFlag) {
      Opts.LexicalLifetimes = LexicalLifetimesOption::On;
    } else {
      Opts.LexicalLifetimes = LexicalLifetimesOption::DiagnosticMarkersOnly;
    }
  }
  if (enableLexicalBorrowScopesFlag) {
    if (*enableLexicalBorrowScopesFlag) {
      Opts.LexicalLifetimes = LexicalLifetimesOption::DiagnosticMarkersOnly;
    } else {
      Opts.LexicalLifetimes = LexicalLifetimesOption::Off;
    }
  }
  if (specifiedDestroyHoistingOption)
    Opts.DestroyHoisting = *specifiedDestroyHoistingOption;

  Opts.EnableARCOptimizations &= !Args.hasArg(OPT_disable_arc_opts);
  Opts.EnableOSSAModules |= Args.hasArg(OPT_enable_ossa_modules);
  Opts.EnableOSSAOptimizations &= !Args.hasArg(OPT_disable_ossa_opts);
  Opts.EnableSILOpaqueValues |= Args.hasArg(OPT_enable_sil_opaque_values);
  Opts.EnableSpeculativeDevirtualization |= Args.hasArg(OPT_enable_spec_devirt);
  Opts.EnableActorDataRaceChecks |= Args.hasFlag(
      OPT_enable_actor_data_race_checks,
      OPT_disable_actor_data_race_checks, /*default=*/false);
  Opts.DisableSILPerfOptimizations |= Args.hasArg(OPT_disable_sil_perf_optzns);
  if (Args.hasArg(OPT_CrossModuleOptimization)) {
    Opts.CMOMode = CrossModuleOptimizationMode::Aggressive;
  } else if (Args.hasArg(OPT_EnbaleDefaultCMO)) {
    Opts.CMOMode = CrossModuleOptimizationMode::Default;  
  }
  Opts.EnableStackProtection =
      Args.hasFlag(OPT_enable_stack_protector, OPT_disable_stack_protector,
                   Opts.EnableStackProtection);
  Opts.EnableMoveInoutStackProtection = Args.hasArg(
      OPT_enable_move_inout_stack_protector, OPT_disable_stack_protector,
      Opts.EnableMoveInoutStackProtection);
  Opts.EnableImportPtrauthFieldFunctionPointers =
      Args.hasArg(OPT_enable_import_ptrauth_field_function_pointers,
                  OPT_disable_import_ptrauth_field_function_pointers,
                  Opts.EnableImportPtrauthFieldFunctionPointers);
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
  Opts.ParseStdlib = FEOpts.ParseStdlib;

  Opts.emitTBD = FEOpts.InputsAndOutputs.hasTBDPath();

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

  // If any of the '-g<kind>', except '-gnone', is given,
  // tell the SILPrinter to print debug info as well
  if (const Arg *A = Args.getLastArg(OPT_g_Group)) {
    if (!A->getOption().matches(options::OPT_gnone))
      Opts.PrintDebugInfo = true;
  }

  if (Args.hasArg(OPT_legacy_gsil))
    llvm::WithColor::warning() << "'-gsil' is deprecated, "
                               << "use '-sil-based-debuginfo' instead\n";
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

  Opts.OSSACompleteLifetimes |= Args.hasArg(OPT_enable_ossa_complete_lifetimes);

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

  Opts.VirtualFunctionElimination = Args.hasArg(OPT_enable_llvm_vfe);
  Opts.WitnessMethodElimination = Args.hasArg(OPT_enable_llvm_wme);

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

    if (const Arg *A = Args.getLastArg(OPT_file_compilation_dir))
      Opts.DebugCompilationDir = A->getValue();
    else {
      llvm::SmallString<256> cwd;
      llvm::sys::fs::current_path(cwd);
      Opts.DebugCompilationDir = std::string(cwd.str());
    }
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

  for (auto A : Args.getAllArgValues(options::OPT_file_prefix_map)) {
    auto SplitMap = StringRef(A).split('=');
    Opts.FilePrefixMap.addMapping(SplitMap.first, SplitMap.second);
    Opts.DebugPrefixMap.addMapping(SplitMap.first, SplitMap.second);
    Opts.CoveragePrefixMap.addMapping(SplitMap.first, SplitMap.second);
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

  if (Args.hasArg(OPT_trap_function))
    Opts.TrapFuncName = Args.getLastArgValue(OPT_trap_function).str();

  Opts.FunctionSections = Args.hasArg(OPT_function_sections);

  if (Args.hasArg(OPT_autolink_force_load))
    Opts.ForceLoadSymbolName = Args.getLastArgValue(OPT_module_link_name).str();

  Opts.ModuleName = FrontendOpts.ModuleName;

  if (Args.hasArg(OPT_no_clang_module_breadcrumbs))
    Opts.DisableClangModuleSkeletonCUs = true;

  if (auto A = Args.getLastArg(OPT_enable_round_trip_debug_types,
                               OPT_disable_round_trip_debug_types)) {
    Opts.DisableRoundTripDebugTypes =
        Args.hasArg(OPT_disable_round_trip_debug_types);
  }

  if (Args.hasArg(OPT_disable_debugger_shadow_copies))
    Opts.DisableDebuggerShadowCopies = true;

  if (Args.hasArg(OPT_disable_concrete_type_metadata_mangled_name_accessors))
    Opts.DisableConcreteTypeMetadataMangledNameAccessors = true;

  if (Args.hasArg(OPT_disable_standard_substitutions_in_reflection_mangling))
    Opts.DisableStandardSubstitutionsInReflectionMangling = true;

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
  Opts.AlwaysCompile |= Args.hasArg(OPT_always_compile_output_files);

  Opts.EnableDynamicReplacementChaining |=
      Args.hasArg(OPT_enable_dynamic_replacement_chaining);

  if (auto A = Args.getLastArg(OPT_enable_type_layouts,
                               OPT_disable_type_layouts)) {
    Opts.UseTypeLayoutValueHandling
      = A->getOption().matches(OPT_enable_type_layouts);
  } else if (Opts.OptMode == OptimizationMode::NoOptimization) {
    // Disable type layouts at Onone except if explicitly requested.
    Opts.UseTypeLayoutValueHandling = false;
  }

  Opts.ForceStructTypeLayouts = Args.hasArg(OPT_force_struct_type_layouts) &&
                                Opts.UseTypeLayoutValueHandling;

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
      Opts.LLVMLTOKind = LLVMLTOKind.value();
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
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
    Opts.ReflectionMetadata = ReflectionMetadataMode::None;
    Opts.EnableReflectionNames = false;
  }

  if (Args.hasArg(OPT_reflection_metadata_for_debugger_only)) {
    Opts.ReflectionMetadata = ReflectionMetadataMode::DebuggerOnly;
    Opts.EnableReflectionNames = true;
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

  // PE/COFF cannot deal with the cross-module reference to the
  // AsyncFunctionPointer data block.  Force the use of indirect
  // AsyncFunctionPointer access.
  Opts.IndirectAsyncFunctionPointer = Triple.isOSBinFormatCOFF();

  // On some Harvard architectures that allow sliding code and data address space
  // offsets independently, it's impossible to make direct relative reference to
  // code from data because the relative offset between them is not representable.
  // Use absolute function references instead of relative ones on such targets.
  // TODO(katei): This is a short-term solution until the WebAssembly target stabilizes
  // PIC and 64-bit specifications and toolchain support.
  Opts.CompactAbsoluteFunctionPointer = Triple.isOSBinFormatWasm();

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

  for (const auto &Lib : Args.getAllArgValues(options::OPT_public_autolink_library)) {
    Opts.PublicLinkLibraries.push_back(Lib);
  }

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
      } else if (version.equals("5.5")) {
        runtimeCompatibilityVersion = llvm::VersionTuple(5, 5);
      } else if (version.equals("5.6")) {
        runtimeCompatibilityVersion = llvm::VersionTuple(5, 6);
      } else if (version.equals("5.8")) {
        runtimeCompatibilityVersion = llvm::VersionTuple(5, 8);
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

  if (!Args.hasArg(
          options::OPT_disable_autolinking_runtime_compatibility_concurrency)) {
    Opts.AutolinkRuntimeCompatibilityConcurrencyLibraryVersion =
        getRuntimeCompatVersion();
  }

  Opts.AutolinkRuntimeCompatibilityBytecodeLayoutsLibrary = Args.hasArg(
      options::OPT_enable_autolinking_runtime_compatibility_bytecode_layouts);

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
  Opts.UseSingleModuleLLVMEmission =
      Opts.NumThreads != 0 &&
      Args.hasArg(OPT_enable_single_module_llvm_emission);

  if (SWIFT_ENABLE_GLOBAL_ISEL_ARM64 &&
      Triple.getArch() == llvm::Triple::aarch64 &&
      Triple.getArchName() != "arm64e") {
    Opts.EnableGlobalISel = true;
  }

  if (Args.hasArg(OPT_enable_llvm_vfe)) {
    Opts.VirtualFunctionElimination = true;
  }

  if (Args.hasArg(OPT_enable_llvm_wme)) {
    Opts.WitnessMethodElimination = true;
  }

  if (Args.hasArg(OPT_conditional_runtime_records)) {
    Opts.ConditionalRuntimeRecords = true;
  }

  if (Args.hasArg(OPT_internalize_at_link)) {
    Opts.InternalizeAtLink = true;
  }

  Opts.InternalizeSymbols = FrontendOpts.Static;

  if (Args.hasArg(OPT_disable_preallocated_instantiation_caches)) {
    Opts.NoPreallocatedInstantiationCaches = true;
  }

  if (Args.hasArg(OPT_disable_readonly_static_objects)) {
    Opts.DisableReadonlyStaticObjects = true;
  }

  // Default to disabling swift async extended frame info on anything but
  // darwin. Other platforms are unlikely to have support for extended frame
  // pointer information.
  if (!Triple.isOSDarwin()) {
    Opts.SwiftAsyncFramePointer = SwiftAsyncFramePointerKind::Never;
  }
  if (const Arg *A = Args.getLastArg(OPT_swift_async_frame_pointer_EQ)) {
    StringRef mode(A->getValue());
    if (mode == "auto")
      Opts.SwiftAsyncFramePointer = SwiftAsyncFramePointerKind::Auto;
    else if (mode == "always")
      Opts.SwiftAsyncFramePointer = SwiftAsyncFramePointerKind::Always;
    else if (mode == "never")
      Opts.SwiftAsyncFramePointer = SwiftAsyncFramePointerKind::Never;
    else {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  } else if (Triple.isWatchOS() && !Triple.isSimulatorEnvironment()) {
    // watchOS does not support auto async frame pointers due to bitcode, so
    // silently override "auto" to "never" when back-deploying. This approach
    // sacrifices async backtraces when back-deploying but prevents crashes in
    // older tools that cannot handle the async frame bit in the frame pointer.
    llvm::VersionTuple OSVersion = Triple.getWatchOSVersion();
    if (OSVersion.getMajor() < 8)
      Opts.SwiftAsyncFramePointer = SwiftAsyncFramePointerKind::Never;
  }

  Opts.EmitGenericRODatas =
      Args.hasFlag(OPT_enable_emit_generic_class_ro_t_list,
                   OPT_disable_emit_generic_class_ro_t_list,
                   Opts.EmitGenericRODatas);

  Opts.ColocateTypeDescriptors = Args.hasFlag(OPT_enable_colocate_type_descriptors,
                                              OPT_disable_colocate_type_descriptors,
                                              Opts.ColocateTypeDescriptors);
  Opts.CollocatedMetadataFunctions =
      Args.hasFlag(OPT_enable_collocate_metadata_functions,
                   OPT_disable_collocate_metadata_functions,
                   Opts.CollocatedMetadataFunctions);
  Opts.UseRelativeProtocolWitnessTables =
    Args.hasFlag(OPT_enable_relative_protocol_witness_tables,
                 OPT_disable_relative_protocol_witness_tables,
                 Opts.UseRelativeProtocolWitnessTables);

  Opts.EnableLayoutStringValueWitnesses = Args.hasFlag(OPT_enable_layout_string_value_witnesses,
                                                       OPT_disable_layout_string_value_witnesses,
                                                       Opts.EnableLayoutStringValueWitnesses);

  Opts.EnableLayoutStringValueWitnessesInstantiation = Args.hasFlag(OPT_enable_layout_string_value_witnesses_instantiation,
                                      OPT_disable_layout_string_value_witnesses_instantiation,
                                      Opts.EnableLayoutStringValueWitnessesInstantiation);

  if (Opts.EnableLayoutStringValueWitnessesInstantiation &&
      !Opts.EnableLayoutStringValueWitnesses) {
    Diags.diagnose(SourceLoc(), diag::layout_string_instantiation_without_layout_strings);
    return true;
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
                             workingDirectory, LangOpts, FrontendOpts)) {
    return true;
  }

  ParseSymbolGraphArgs(SymbolGraphOpts, ParsedArgs, Diags, LangOpts);

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
  setDefaultBlocklistsIfNecessary();

  // Now that we've parsed everything, setup some inter-option-dependent state.
  setIRGenOutputOptsFromFrontendOptions(IRGenOpts, FrontendOpts);
  setBridgingHeaderFromFrontendOptions(ClangImporterOpts, FrontendOpts);

  return false;
}

serialization::Status
CompilerInvocation::loadFromSerializedAST(StringRef data) {
  serialization::ExtendedValidationInfo extendedInfo;
  serialization::ValidationInfo info = serialization::validateSerializedAST(
      data, getSILOptions().EnableOSSAModules, LangOpts.SDKName,
      !LangOpts.DebuggerSupport, &extendedInfo);

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
      fileBufOrErr.get()->getBuffer(), getSILOptions().EnableOSSAModules,
      LangOpts.SDKName, !LangOpts.DebuggerSupport, &extendedInfo);
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
