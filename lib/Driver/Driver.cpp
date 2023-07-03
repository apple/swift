//===--- Driver.cpp - Swift compiler driver -------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file contains implementations of parts of the compiler driver.
//
//===----------------------------------------------------------------------===//

#include "swift/Driver/Driver.h"

#include "ToolChains.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/OutputFileMap.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/Range.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/Version.h"
#include "swift/Config.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Compilation.h"
#include "swift/Driver/Job.h"
#include "swift/Driver/PrettyStackTrace.h"
#include "swift/Driver/ToolChain.h"
#include "swift/Option/Options.h"
#include "swift/Option/SanitizerOptions.h"
#include "swift/Parse/Lexer.h"
#include "swift/Strings.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Config/config.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Remarks/RemarkFormat.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Host.h"
#include "llvm/Support/MD5.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/PrettyStackTrace.h"
#include "llvm/Support/raw_ostream.h"

#include "CompilationRecord.h"

#include <memory>

using namespace swift;
using namespace swift::driver;
using namespace llvm::opt;

Driver::Driver(StringRef DriverExecutable,
               StringRef Name,
               ArrayRef<const char *> Args,
               DiagnosticEngine &Diags)
  : Opts(createSwiftOptTable()), Diags(Diags),
    Name(Name), DriverExecutable(DriverExecutable),
    DefaultTargetTriple(llvm::sys::getDefaultTargetTriple()) {
      
  // The driver kind must be parsed prior to parsing arguments, since that
  // affects how arguments are parsed.
  parseDriverKind(Args.slice(1));
}

Driver::~Driver() = default;

void Driver::parseDriverKind(ArrayRef<const char *> Args) {
  // The default driver kind is determined by Name.
  StringRef DriverName = Name;

  std::string OptName;
  // However, the driver kind may be overridden if the first argument is
  // --driver-mode.
  if (!Args.empty()) {
    OptName = getOpts().getOption(options::OPT_driver_mode).getPrefixedName();

    StringRef FirstArg(Args[0]);
    if (FirstArg.startswith(OptName))
      DriverName = FirstArg.drop_front(OptName.size());
  }

  llvm::Optional<DriverKind> Kind =
      llvm::StringSwitch<llvm::Optional<DriverKind>>(DriverName)
          .Case("swift", DriverKind::Interactive)
          .Case("swiftc", DriverKind::Batch)
          .Case("sil-opt", DriverKind::SILOpt)
          .Case("sil-func-extractor", DriverKind::SILFuncExtractor)
          .Case("sil-nm", DriverKind::SILNM)
          .Case("sil-llvm-gen", DriverKind::SILLLVMGen)
          .Case("sil-passpipeline-dumper", DriverKind::SILPassPipelineDumper)
          .Case("swift-dependency-tool", DriverKind::SwiftDependencyTool)
          .Case("swift-llvm-opt", DriverKind::SwiftLLVMOpt)
          .Case("swift-autolink-extract", DriverKind::AutolinkExtract)
          .Case("swift-indent", DriverKind::SwiftIndent)
          .Case("swift-symbolgraph-extract", DriverKind::SymbolGraph)
          .Case("swift-api-extract", DriverKind::APIExtract)
          .Case("swift-api-digester", DriverKind::APIDigester)
          .Case("swift-cache-tool", DriverKind::CacheTool)
          .Default(llvm::None);

  if (Kind.has_value())
    driverKind = Kind.value();
  else if (!OptName.empty())
    Diags.diagnose({}, diag::error_invalid_arg_value, OptName, DriverName);
}

ArrayRef<const char *> Driver::getArgsWithoutProgramNameAndDriverMode(
                                      ArrayRef<const char *> Args) const {
  Args = Args.slice(1);
  if (Args.empty())
    return Args;

  const std::string OptName =
    getOpts().getOption(options::OPT_driver_mode).getPrefixedName();
  if (StringRef(Args[0]).startswith(OptName))
    Args = Args.slice(1);
  return Args;
}

static void validateBridgingHeaderArgs(DiagnosticEngine &diags,
                                       const ArgList &args) {
  if (!args.hasArgNoClaim(options::OPT_import_objc_header))
    return;

  if (args.hasArgNoClaim(options::OPT_import_underlying_module))
    diags.diagnose({}, diag::error_framework_bridging_header);

  if (args.hasArgNoClaim(options::OPT_emit_module_interface,
                         options::OPT_emit_module_interface_path)) {
    diags.diagnose({}, diag::error_bridging_header_module_interface);
  }
}

static void validateWarningControlArgs(DiagnosticEngine &diags,
                                       const ArgList &args) {
  if (args.hasArg(options::OPT_suppress_warnings) &&
      args.hasFlag(options::OPT_warnings_as_errors,
                   options::OPT_no_warnings_as_errors, false)) {
    diags.diagnose(SourceLoc(), diag::error_conflicting_options,
                   "-warnings-as-errors", "-suppress-warnings");
  }
}

static void validateProfilingArgs(DiagnosticEngine &diags,
                                  const ArgList &args) {
  const Arg *ProfileGenerate = args.getLastArg(options::OPT_profile_generate);
  const Arg *ProfileUse = args.getLastArg(options::OPT_profile_use);
  if (ProfileGenerate && ProfileUse) {
    diags.diagnose(SourceLoc(), diag::error_conflicting_options,
                   "-profile-generate", "-profile-use");
  }

  // Check if the profdata is missing
  if (ProfileUse && !llvm::sys::fs::exists(ProfileUse->getValue())) {
    diags.diagnose(SourceLoc(), diag::error_profile_missing,
                   ProfileUse->getValue());
  }
}

static void validateDependencyScanningArgs(DiagnosticEngine &diags,
                                           const ArgList &args) {
  const Arg *ExternalDependencyMap =
      args.getLastArg(options::OPT_placeholder_dependency_module_map);
  const Arg *ScanDependencies = args.getLastArg(options::OPT_scan_dependencies);
  const Arg *Prescan = args.getLastArg(options::OPT_import_prescan);

  const Arg *SerializeCache =
      args.getLastArg(options::OPT_serialize_dependency_scan_cache);
  const Arg *ReuseCache =
      args.getLastArg(options::OPT_reuse_dependency_scan_cache);
  const Arg *CacheSerializationPath =
      args.getLastArg(options::OPT_dependency_scan_cache_path);

  if (ExternalDependencyMap && !ScanDependencies) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-placeholder-dependency-module-map-file",
                   "-scan-dependencies");
  }
  if (Prescan && !ScanDependencies) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-import-prescan", "-scan-dependencies");
  }
  if (Prescan && !ScanDependencies) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-import-prescan", "-scan-dependencies");
  }
  if (SerializeCache && !ScanDependencies) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-serialize-dependency-scan-cache", "-scan-dependencies");
  }
  if (ReuseCache && !ScanDependencies) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-load-dependency-scan-cache", "-scan-dependencies");
  }
  if (SerializeCache && !CacheSerializationPath) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-serialize-dependency-scan-cache",
                   "-dependency-scan-cache-path");
  }
  if (ReuseCache && !CacheSerializationPath) {
    diags.diagnose(SourceLoc(), diag::error_requirement_not_met,
                   "-serialize-dependency-scan-cache",
                   "-dependency-scan-cache-path");
  }
}

static void validateDebugInfoArgs(DiagnosticEngine &diags,
                                  const ArgList &args) {
  // Check for missing debug option when verifying debug info.
  if (args.hasArg(options::OPT_verify_debug_info)) {
    Arg *debugOpt = args.getLastArg(swift::options::OPT_g_Group);
    if (!debugOpt || debugOpt->getOption().matches(swift::options::OPT_gnone)) {
      diags.diagnose(SourceLoc(),
                     diag::verify_debug_info_requires_debug_option);
    }
  }

  // Check for any -*-prefix-map options that aren't of the form
  // 'original=remapped' (either side can be empty, however).
  for (const Arg *A : args.filtered(options::OPT_debug_prefix_map,
                                    options::OPT_coverage_prefix_map,
                                    options::OPT_file_prefix_map)) {
    StringRef val = A->getValue();
    if (val.find('=') == StringRef::npos)
      diags.diagnose(SourceLoc(), diag::error_opt_invalid_mapping,
                     A->getOption().getPrefixedName(), val);
  }
}

static void validateVerifyIncrementalDependencyArgs(DiagnosticEngine &diags,
                                                    const ArgList &args) {
  // No option? No problem!
  if (!args.hasArg(options::OPT_verify_incremental_dependencies)) {
    return;
  }

  // Make sure we see -incremental but not -wmo, no matter in what order they're
  // in - the build systems can pass them both and just hope the last one wins.
  if (args.hasArg(options::OPT_incremental) &&
      !args.hasArg(options::OPT_whole_module_optimization)) {
    return;
  }

  diags.diagnose(SourceLoc(),
                 diag::verify_incremental_dependencies_needs_incremental);
}

static void validateCompilationConditionArgs(DiagnosticEngine &diags,
                                             const ArgList &args) {
  for (const Arg *A : args.filtered(options::OPT_D)) {
    StringRef name = A->getValue();
    if (name.contains('=')) {
      diags.diagnose(SourceLoc(),
                     diag::cannot_assign_value_to_conditional_compilation_flag,
                     name);
    } else if (name.startswith("-D")) {
      diags.diagnose(SourceLoc(), diag::redundant_prefix_compilation_flag,
                     name);
    } else if (!Lexer::isIdentifier(name)) {
      diags.diagnose(SourceLoc(), diag::invalid_conditional_compilation_flag,
                     name);
    }
  }
}

static void validateSearchPathArgs(DiagnosticEngine &diags,
                                   const ArgList &args) {
  for (const Arg *A : args.filtered(options::OPT_F, options::OPT_Fsystem)) {
    StringRef name = A->getValue();
    if (name.endswith(".framework") || name.endswith(".framework/"))
      diags.diagnose(SourceLoc(),
                     diag::framework_search_path_includes_framework_extension,
                     name);
  }
}

static void validateLinkArgs(DiagnosticEngine &diags, const ArgList &args) {
  if (args.hasArg(options::OPT_experimental_hermetic_seal_at_link)) {
    if (args.hasArg(options::OPT_enable_library_evolution)) {
      diags.diagnose(SourceLoc(),
                     diag::error_hermetic_seal_cannot_have_library_evolution);
    }

    bool ltoOk = false;
    if (const Arg *A = args.getLastArg(options::OPT_lto)) {
      StringRef name = A->getValue();
      if (name == "llvm-thin" || name == "llvm-full") {
        ltoOk = true;
      }
    }

    if (!ltoOk) {
      diags.diagnose(SourceLoc(),
                     diag::error_hermetic_seal_requires_lto);
    }
  }
}

/// Perform miscellaneous early validation of arguments.
static void validateArgs(DiagnosticEngine &diags, const ArgList &args,
                         const llvm::Triple &T) {
  validateBridgingHeaderArgs(diags, args);
  validateWarningControlArgs(diags, args);
  validateProfilingArgs(diags, args);
  validateDependencyScanningArgs(diags, args);
  validateDebugInfoArgs(diags, args);
  validateCompilationConditionArgs(diags, args);
  validateSearchPathArgs(diags, args);
  validateVerifyIncrementalDependencyArgs(diags, args);
  validateLinkArgs(diags, args);
}

std::unique_ptr<ToolChain>
Driver::buildToolChain(const llvm::opt::InputArgList &ArgList) {

  if (const Arg *A = ArgList.getLastArg(options::OPT_target)) {
    DefaultTargetTriple = llvm::Triple::normalize(A->getValue());
  }

  llvm::Triple target(DefaultTargetTriple);

  // Backward compatibility hack: infer "simulator" environment for x86
  // iOS/tvOS/watchOS.
  if (tripleInfersSimulatorEnvironment(target)) {
    // Set the simulator environment.
    target.setEnvironment(llvm::Triple::EnvironmentType::Simulator);

    auto newTargetTriple = target.normalize();
    Diags.diagnose(SourceLoc(), diag::warning_inferred_simulator_target,
                   DefaultTargetTriple, newTargetTriple);

    DefaultTargetTriple = newTargetTriple;
  }

  switch (target.getOS()) {
  case llvm::Triple::IOS:
  case llvm::Triple::TvOS:
  case llvm::Triple::WatchOS:
  case llvm::Triple::Darwin:
  case llvm::Triple::MacOSX: {
    llvm::Optional<llvm::Triple> targetVariant;
    if (const Arg *A = ArgList.getLastArg(options::OPT_target_variant))
      targetVariant = llvm::Triple(llvm::Triple::normalize(A->getValue()));

    return std::make_unique<toolchains::Darwin>(*this, target, targetVariant);
  }
  case llvm::Triple::Linux:
    if (target.isAndroid())
      return std::make_unique<toolchains::Android>(*this, target);
    return std::make_unique<toolchains::GenericUnix>(*this, target);
  case llvm::Triple::FreeBSD:
    return std::make_unique<toolchains::FreeBSD>(*this, target);
  case llvm::Triple::OpenBSD:
    return std::make_unique<toolchains::OpenBSD>(*this, target);
  case llvm::Triple::Win32:
    if (target.isWindowsCygwinEnvironment())
      return std::make_unique<toolchains::Cygwin>(*this, target);
    return std::make_unique<toolchains::Windows>(*this, target);
  case llvm::Triple::Haiku:
    return std::make_unique<toolchains::GenericUnix>(*this, target);
  case llvm::Triple::WASI:
    return std::make_unique<toolchains::WebAssembly>(*this, target);
  case llvm::Triple::UnknownOS:
    return std::make_unique<toolchains::GenericUnix>(*this, target);
  default:
    Diags.diagnose(SourceLoc(), diag::error_unknown_target,
                   ArgList.getLastArg(options::OPT_target)->getValue());
    break;
  }
  return nullptr;
}

std::unique_ptr<sys::TaskQueue> Driver::buildTaskQueue(const Compilation &C) {
  const auto &ArgList = C.getArgs();
  unsigned NumberOfParallelCommands = 1;
  if (const Arg *A = ArgList.getLastArg(options::OPT_j)) {
    if (StringRef(A->getValue()).getAsInteger(10, NumberOfParallelCommands)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
      return nullptr;
    }
  }
  if (environmentVariableRequestedMaximumDeterminism()) {
      NumberOfParallelCommands = 1;
      Diags.diagnose(SourceLoc(), diag::remark_max_determinism_overriding,
                     "-j");
  }

  const bool DriverSkipExecution =
    ArgList.hasArg(options::OPT_driver_skip_execution,
                   options::OPT_driver_print_jobs);
  if (DriverSkipExecution) {
    return std::make_unique<sys::DummyTaskQueue>(NumberOfParallelCommands);
  } else {
    return std::make_unique<sys::TaskQueue>(NumberOfParallelCommands,
                                             C.getStatsReporter());
  }
}

static void computeArgsHash(SmallString<32> &out, const DerivedArgList &args) {
  SmallVector<const Arg *, 32> interestingArgs;
  interestingArgs.reserve(args.size());
  std::copy_if(args.begin(), args.end(), std::back_inserter(interestingArgs),
               [](const Arg *arg) {
    return !arg->getOption().hasFlag(options::DoesNotAffectIncrementalBuild) &&
           arg->getOption().getKind() != Option::InputClass;
  });

  llvm::array_pod_sort(interestingArgs.begin(), interestingArgs.end(),
                       [](const Arg * const *lhs, const Arg * const *rhs)->int {
    auto cmpID = (*lhs)->getOption().getID() - (*rhs)->getOption().getID();
    if (cmpID != 0)
      return cmpID;
    return (*lhs)->getIndex() - (*rhs)->getIndex();
  });

  llvm::MD5 hash;
  for (const Arg *arg : interestingArgs) {
    hash.update(arg->getOption().getID());
    for (const char *value : const_cast<Arg *>(arg)->getValues())
      hash.update(value);
  }

  llvm::MD5::MD5Result hashBuf;
  hash.final(hashBuf);
  llvm::MD5::stringifyResult(hashBuf, out);
}

class Driver::InputInfoMap
    : public llvm::SmallDenseMap<const Arg *, CompileJobAction::InputInfo, 16> {
};
using InputInfoMap = Driver::InputInfoMap;

/// Get the filename for build record. Returns true if failed.
static bool getCompilationRecordPath(std::string &buildRecordPath,
                                     const OutputInfo &OI,
                                     const llvm::Optional<OutputFileMap> &OFM,
                                     DiagnosticEngine *Diags) {
  if (!OFM) {
    // FIXME: This should work without an output file map. We should have
    // another way to specify a build record and where to put intermediates.
    if (Diags)
      Diags->diagnose(SourceLoc(), diag::incremental_requires_output_file_map);
    return true;
  }

  if (auto *masterOutputMap = OFM->getOutputMapForSingleOutput())
    buildRecordPath = masterOutputMap->lookup(file_types::TY_SwiftDeps);

  if (buildRecordPath.empty()) {
    if (Diags)
      Diags->diagnose(SourceLoc(),
                      diag::incremental_requires_build_record_entry,
                      file_types::getTypeName(file_types::TY_SwiftDeps));
    return true;
  }

  return false;
}

static std::string failedToReadOutOfDateMap(bool ShowIncrementalBuildDecisions,
                                          StringRef buildRecordPath,
                                          StringRef reason = "") {
  std::string why = "malformed build record file";
  if (!reason.empty()) {
    why += " ";
    why += reason;
  }
  if (ShowIncrementalBuildDecisions) {
    llvm::outs() << "Incremental compilation has been disabled due to " << why
                 << " '" << buildRecordPath << "'.\n";
  }
  return why;
}

static SmallVector<StringRef, 8> findRemovedInputs(
    const InputFileList &inputs,
    const llvm::StringMap<CompileJobAction::InputInfo> &previousInputs);

static void dealWithRemovedInputs(ArrayRef<StringRef> removedInputs,
                                  bool ShowIncrementalBuildDecisions);

/// Returns why ignore incrementality
static std::string
populateOutOfDateMap(InputInfoMap &map, llvm::sys::TimePoint<> &LastBuildTime,
                     StringRef argsHashStr, const InputFileList &inputs,
                     StringRef buildRecordPath,
                     const bool ShowIncrementalBuildDecisions) {
  // Treat a missing file as "no previous build".
  auto buffer = llvm::MemoryBuffer::getFile(buildRecordPath);
  if (!buffer) {
    if (ShowIncrementalBuildDecisions)
      llvm::outs() << "Incremental compilation could not read build record.\n";
    return "could not read build record";
  }

  namespace yaml = llvm::yaml;
  using InputInfo = CompileJobAction::InputInfo;

  llvm::SourceMgr SM;
  yaml::Stream stream(buffer.get()->getMemBufferRef(), SM);

  auto I = stream.begin();
  if (I == stream.end() || !I->getRoot())
    return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                    buildRecordPath);

  auto *topLevelMap = dyn_cast<yaml::MappingNode>(I->getRoot());
  if (!topLevelMap)
    return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                    buildRecordPath);
  SmallString<64> scratch;

  llvm::StringMap<InputInfo> previousInputs;
  bool versionValid = false;
  bool optionsMatch = true;

  auto readTimeValue = [&scratch](yaml::Node *node,
                                  llvm::sys::TimePoint<> &timeValue) -> bool {
    auto *seq = dyn_cast<yaml::SequenceNode>(node);
    if (!seq)
      return true;

    auto seqI = seq->begin(), seqE = seq->end();
    if (seqI == seqE)
      return true;

    auto *secondsRaw = dyn_cast<yaml::ScalarNode>(&*seqI);
    if (!secondsRaw)
      return true;
    std::time_t parsedSeconds;
    if (secondsRaw->getValue(scratch).getAsInteger(10, parsedSeconds))
      return true;

    ++seqI;
    if (seqI == seqE)
      return true;

    auto *nanosecondsRaw = dyn_cast<yaml::ScalarNode>(&*seqI);
    if (!nanosecondsRaw)
      return true;
    std::chrono::system_clock::rep parsedNanoseconds;
    if (nanosecondsRaw->getValue(scratch).getAsInteger(10, parsedNanoseconds))
      return true;

    ++seqI;
    if (seqI != seqE)
      return true;

    timeValue = llvm::sys::TimePoint<>(std::chrono::seconds(parsedSeconds));
    timeValue += std::chrono::nanoseconds(parsedNanoseconds);
    return false;
  };

  // FIXME: LLVM's YAML support does incremental parsing in such a way that
  // for-range loops break.
  SmallString<64> CompilationRecordSwiftVersion;
  for (auto i = topLevelMap->begin(), e = topLevelMap->end(); i != e; ++i) {
    auto *key = cast<yaml::ScalarNode>(i->getKey());
    StringRef keyStr = key->getValue(scratch);

    using compilation_record::TopLevelKey;
    if (keyStr == compilation_record::getName(TopLevelKey::Version)) {
      auto *value = dyn_cast<yaml::ScalarNode>(i->getValue());
      if (!value) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }

      // NB: We check against
      // swift::version::Version::getCurrentLanguageVersion() here because any
      // -swift-version argument is handled in the argsHashStr check that
      // follows.
      CompilationRecordSwiftVersion = value->getValue(scratch);
      versionValid = (CompilationRecordSwiftVersion
                      == version::getSwiftFullVersion(
                        version::Version::getCurrentLanguageVersion()));

    } else if (keyStr == compilation_record::getName(TopLevelKey::Options)) {
      auto *value = dyn_cast<yaml::ScalarNode>(i->getValue());
      if (!value)
        return "no name node in build record";
      optionsMatch = (argsHashStr == value->getValue(scratch));

    } else if (keyStr == compilation_record::getName(TopLevelKey::BuildTime)) {
      auto *value = dyn_cast<yaml::SequenceNode>(i->getValue());
      if (!value) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }
      llvm::sys::TimePoint<> timeVal;
      if (readTimeValue(i->getValue(), timeVal))
        return "could not read time value in build record";
      LastBuildTime = timeVal;

    } else if (keyStr == compilation_record::getName(TopLevelKey::Inputs)) {
      auto *inputMap = dyn_cast<yaml::MappingNode>(i->getValue());
      if (!inputMap) {
        auto reason = ("Malformed value for key '" + keyStr + "'.")
          .toStringRef(scratch);
        return failedToReadOutOfDateMap(ShowIncrementalBuildDecisions,
                                        buildRecordPath, reason);
      }

      // FIXME: LLVM's YAML support does incremental parsing in such a way that
      // for-range loops break.
      for (auto i = inputMap->begin(), e = inputMap->end(); i != e; ++i) {
        auto *key = dyn_cast<yaml::ScalarNode>(i->getKey());
        if (!key)
          return "no input entry in build record";

        auto *value = dyn_cast<yaml::SequenceNode>(i->getValue());
        if (!value)
          return "no sequence node for input entry in build record";

        using compilation_record::getInfoStatusForIdentifier;
        auto previousBuildState =
          getInfoStatusForIdentifier(value->getRawTag());
        if (!previousBuildState)
          return "no previous build state in build record";

        llvm::sys::TimePoint<> timeValue;
        if (readTimeValue(value, timeValue))
          return "could not read time value in build record";

        auto inputName = key->getValue(scratch);
        previousInputs[inputName] = { *previousBuildState, timeValue };
      }
    }
  }

  if (!versionValid) {
    if (ShowIncrementalBuildDecisions) {
      auto v = version::getSwiftFullVersion(
          version::Version::getCurrentLanguageVersion());
      llvm::outs() << "Incremental compilation has been disabled, due to a "
                   << "compiler version mismatch.\n"
                   << "\tCompiling with: " << v << "\n"
                   << "\tPreviously compiled with: "
                   << CompilationRecordSwiftVersion << "\n";
    }
    return "compiler version mismatch";
  }

  if (!optionsMatch) {
    if (ShowIncrementalBuildDecisions) {
      llvm::outs() << "Incremental compilation has been disabled, because "
                   << "different arguments were passed to the compiler.\n";
    }
    return "different arguments passed to compiler";
  }

  unsigned numMatchingPreviouslyCompiledInputs = 0;
  for (auto &inputPair : inputs) {
    auto iter = previousInputs.find(inputPair.second->getValue());
    if (iter == previousInputs.end())
      map[inputPair.second] = CompileJobAction::InputInfo::makeNewlyAdded();
    else {
      map[inputPair.second] = iter->getValue();
      ++numMatchingPreviouslyCompiledInputs;
    }
  }
  assert(numMatchingPreviouslyCompiledInputs <= previousInputs.size());
  auto const wereAnyInputsRemoved =
      numMatchingPreviouslyCompiledInputs < previousInputs.size();
  if (!wereAnyInputsRemoved)
    return "";

  const auto removedInputs = findRemovedInputs(inputs, previousInputs);
  assert(!removedInputs.empty());

  dealWithRemovedInputs(removedInputs, ShowIncrementalBuildDecisions);
  return "an input was removed"; // recompile everything; could do better
                                 // someday
}

static SmallVector<StringRef, 8> findRemovedInputs(
    const InputFileList &inputs,
    const llvm::StringMap<CompileJobAction::InputInfo> &previousInputs) {
  llvm::DenseSet<StringRef> inputArgs;
  for (auto &inputPair : inputs) {
    inputArgs.insert(inputPair.second->getValue());
  }
  SmallVector<StringRef, 8> missingInputs;
  for (auto &previousInput : previousInputs) {
    auto previousInputArg = previousInput.getKey();
    if (!inputArgs.contains(previousInputArg)) {
      missingInputs.push_back(previousInputArg);
    }
  }
  return missingInputs;
}

static void showRemovedInputs(ArrayRef<StringRef> removedInputs);

/// Return true if hadError
static void dealWithRemovedInputs(ArrayRef<StringRef> removedInputs,
                                  const bool ShowIncrementalBuildDecisions) {
  // If a file was removed, we've lost its dependency info. Rebuild everything.
  // FIXME: Can we do better?
  if (ShowIncrementalBuildDecisions)
    showRemovedInputs(removedInputs);
}

static void showRemovedInputs(ArrayRef<StringRef> removedInputs) {

  llvm::outs() << "Incremental compilation has been disabled, because "
               << "the following inputs were used in the previous "
               << "compilation, but not in the current compilation:\n";

  for (auto &missing : removedInputs)
    llvm::outs() << "\t" << missing << "\n";
}

// warn if -embed-bitcode is set and the output type is not an object
static void validateEmbedBitcode(DerivedArgList &Args, const OutputInfo &OI,
                                 DiagnosticEngine &Diags) {
  if (Args.hasArg(options::OPT_embed_bitcode) &&
      OI.CompilerOutputType != file_types::TY_Object) {
    Diags.diagnose(SourceLoc(), diag::warn_ignore_embed_bitcode);
    Args.eraseArg(options::OPT_embed_bitcode);
  }

  if (Args.hasArg(options::OPT_embed_bitcode_marker) &&
      OI.CompilerOutputType != file_types::TY_Object) {
    Diags.diagnose(SourceLoc(), diag::warn_ignore_embed_bitcode_marker);
    Args.eraseArg(options::OPT_embed_bitcode_marker);
  }
}

/// Gets the filelist threshold to use. Diagnoses and returns true on error.
static bool getFilelistThreshold(DerivedArgList &Args, size_t &FilelistThreshold,
                                   DiagnosticEngine &Diags) {
  FilelistThreshold = 128;

  // claim and diagnose deprecated -driver-use-filelists
  bool HasUseFilelists = Args.hasArg(options::OPT_driver_use_filelists);
  if (HasUseFilelists)
    Diags.diagnose(SourceLoc(), diag::warn_use_filelists_deprecated);

  if (const Arg *A = Args.getLastArg(options::OPT_driver_filelist_threshold)) {
    // Use the supplied threshold
    if (StringRef(A->getValue()).getAsInteger(10, FilelistThreshold)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
      return true;
    }
  } else if (HasUseFilelists) {
    // Treat -driver-use-filelists as -driver-filelist-threshold=0
    FilelistThreshold = 0;
  } // else stick with the default

  return false;
}

static unsigned
getDriverBatchSeed(llvm::opt::InputArgList &ArgList,
                   DiagnosticEngine &Diags) {
  unsigned DriverBatchSeed = 0;
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_seed)) {
    if (StringRef(A->getValue()).getAsInteger(10, DriverBatchSeed)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    }
  }
  return DriverBatchSeed;
}

static llvm::Optional<unsigned>
getDriverBatchCount(llvm::opt::InputArgList &ArgList, DiagnosticEngine &Diags) {
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_count)) {
    unsigned Count = 0;
    if (StringRef(A->getValue()).getAsInteger(10, Count)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    } else {
      return Count;
    }
  }
  return llvm::None;
}

static bool computeIncremental(const llvm::opt::InputArgList *ArgList,
                               const bool ShowIncrementalBuildDecisions) {
  if (!ArgList->hasArg(options::OPT_incremental))
    return false;

  const char *ReasonToDisable =
      ArgList->hasFlag(options::OPT_whole_module_optimization,
                       options::OPT_no_whole_module_optimization,
                       false)
          ? "is not compatible with whole module optimization."
          : ArgList->hasArg(options::OPT_embed_bitcode)
                ? "is not currently compatible with embedding LLVM IR bitcode."
                : nullptr;

  if (!ReasonToDisable)
    return true;

  if (ShowIncrementalBuildDecisions) {
    llvm::outs() << "Incremental compilation has been disabled, because it "
                 << ReasonToDisable
                 << "\n";
  }
  return false;
}

static std::string
computeWorkingDirectory(const llvm::opt::InputArgList *ArgList) {
  if (auto *A = ArgList->getLastArg(options::OPT_working_directory)) {
    SmallString<128> workingDirectory;
    workingDirectory = A->getValue();
    llvm::sys::fs::make_absolute(workingDirectory);
    std::string result = workingDirectory.str().str();
    return result;
  }
  return std::string();
}

static std::unique_ptr<UnifiedStatsReporter>
createStatsReporter(const llvm::opt::InputArgList *ArgList,
                    const InputFileList &Inputs, const OutputInfo OI,
                    StringRef DefaultTargetTriple) {
  const Arg *A = ArgList->getLastArgNoClaim(options::OPT_stats_output_dir);
  if (!A)
    return nullptr;

  StringRef OptType;
  if (const Arg *OptA = ArgList->getLastArgNoClaim(options::OPT_O_Group)) {
    OptType = OptA->getSpelling();
  }
  StringRef InputName;
  if (Inputs.size() == 1) {
    InputName = Inputs[0].second->getSpelling();
  }
  StringRef OutputType = file_types::getExtension(OI.CompilerOutputType);
  return std::make_unique<UnifiedStatsReporter>("swift-driver",
                                                 OI.ModuleName,
                                                 InputName,
                                                 DefaultTargetTriple,
                                                 OutputType,
                                                 OptType,
                                                 A->getValue());
}

static bool
computeContinueBuildingAfterErrors(const bool BatchMode,
                                   const llvm::opt::InputArgList *ArgList) {
  // Note: Batch mode handling of serialized diagnostics requires that all
  // batches get to run, in order to make sure that all diagnostics emitted
  // during the compilation end up in at least one serialized diagnostic file.
  // Therefore, treat batch mode as implying -continue-building-after-errors.
  // (This behavior could be limited to only when serialized diagnostics are
  // being emitted, but this seems more consistent and less surprising for
  // users.)
  // FIXME: We don't really need (or want) a full ContinueBuildingAfterErrors.
  // If we fail to precompile a bridging header, for example, there's no need
  // to go on to compilation of source files, and if compilation of source files
  // fails, we shouldn't try to link. Instead, we'd want to let all jobs finish
  // but not schedule any new ones.
  return BatchMode ||
         ArgList->hasArg(options::OPT_continue_building_after_errors);

}

static llvm::Optional<unsigned>
getDriverBatchSizeLimit(llvm::opt::InputArgList &ArgList,
                        DiagnosticEngine &Diags) {
  if (const Arg *A = ArgList.getLastArg(options::OPT_driver_batch_size_limit)) {
    unsigned Limit = 0;
    if (StringRef(A->getValue()).getAsInteger(10, Limit)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(ArgList), A->getValue());
    } else {
      return Limit;
    }
  }
  return llvm::None;
}

std::unique_ptr<Compilation>
Driver::buildCompilation(const ToolChain &TC,
                         std::unique_ptr<llvm::opt::InputArgList> ArgList,
                         bool AllowErrors) {
  llvm::PrettyStackTraceString CrashInfo("Compilation construction");

  llvm::sys::TimePoint<> StartTime = std::chrono::system_clock::now();

  // Claim --driver-mode here, since it's already been handled.
  (void) ArgList->hasArg(options::OPT_driver_mode);

  DriverPrintBindings = ArgList->hasArg(options::OPT_driver_print_bindings);

  const std::string workingDirectory = computeWorkingDirectory(ArgList.get());

  std::unique_ptr<DerivedArgList> TranslatedArgList(
      translateInputAndPathArgs(*ArgList, workingDirectory));

  validateArgs(Diags, *TranslatedArgList, TC.getTriple());
    
  // Perform toolchain specific args validation.
  TC.validateArguments(Diags, *TranslatedArgList, DefaultTargetTriple);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  if (!handleImmediateArgs(*TranslatedArgList, TC)) {
    return nullptr;
  }

  // Construct the list of inputs.
  InputFileList Inputs;
  buildInputs(TC, *TranslatedArgList, Inputs);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  // Determine the OutputInfo for the driver.
  OutputInfo OI;
  bool BatchMode = false;
  OI.CompilerMode = computeCompilerMode(*TranslatedArgList, Inputs, BatchMode);
  buildOutputInfo(TC, *TranslatedArgList, BatchMode, Inputs, OI);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  assert(OI.CompilerOutputType != file_types::ID::TY_INVALID &&
         "buildOutputInfo() must set a valid output type!");

  TC.validateOutputInfo(Diags, OI);
  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  validateEmbedBitcode(*TranslatedArgList, OI, Diags);

  if (OI.CompilerMode == OutputInfo::Mode::REPL)
    // REPL mode expects no input files, so suppress the error.
    SuppressNoInputFilesError = true;

  llvm::Optional<OutputFileMap> OFM =
      buildOutputFileMap(*TranslatedArgList, workingDirectory);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  if (ArgList->hasArg(options::OPT_driver_print_output_file_map)) {
    if (OFM)
      OFM->dump(llvm::errs(), true);
    else
      Diags.diagnose(SourceLoc(), diag::error_no_output_file_map_specified);
    if (!AllowErrors) {
      return nullptr;
    }
  }

  const bool ShowIncrementalBuildDecisions =
      ArgList->hasArg(options::OPT_driver_show_incremental);
  const bool Incremental =
      computeIncremental(ArgList.get(), ShowIncrementalBuildDecisions);

  std::string buildRecordPath;
  getCompilationRecordPath(buildRecordPath,
                           OI, OFM, Incremental ? &Diags : nullptr);

  SmallString<32> ArgsHash;
  computeArgsHash(ArgsHash, *TranslatedArgList);
  llvm::sys::TimePoint<> LastBuildTime = llvm::sys::TimePoint<>::min();
  InputInfoMap outOfDateMap;
  std::string whyIgnoreIncrementality =
      !Incremental
          ? ""
          : buildRecordPath.empty()
                ? "no build record path"
                : populateOutOfDateMap(outOfDateMap, LastBuildTime, ArgsHash,
                                       Inputs, buildRecordPath,
                                       ShowIncrementalBuildDecisions);
  // FIXME: Distinguish errors from "file removed", which is benign.

  size_t DriverFilelistThreshold;
  if (getFilelistThreshold(*TranslatedArgList, DriverFilelistThreshold, Diags))
    return nullptr;

  OutputLevel Level = OutputLevel::Normal;
  if (const Arg *A =
          ArgList->getLastArg(options::OPT_driver_print_jobs, options::OPT_v,
                              options::OPT_parseable_output)) {
    if (A->getOption().matches(options::OPT_driver_print_jobs))
      Level = OutputLevel::PrintJobs;
    else if (A->getOption().matches(options::OPT_v))
      Level = OutputLevel::Verbose;
    else if (A->getOption().matches(options::OPT_parseable_output))
      Level = OutputLevel::Parseable;
    else
      llvm_unreachable("Unknown OutputLevel argument!");
  }

  
  // About to move argument list, so capture some flags that will be needed
  // later.
  const bool DriverPrintActions =
      ArgList->hasArg(options::OPT_driver_print_actions);
  const bool DriverPrintDerivedOutputFileMap =
      ArgList->hasArg(options::OPT_driver_print_derived_output_file_map);
  const bool ContinueBuildingAfterErrors =
      computeContinueBuildingAfterErrors(BatchMode, ArgList.get());
  const bool ShowJobLifecycle =
      ArgList->hasArg(options::OPT_driver_show_job_lifecycle);

  // In order to confine the values below, while still moving the argument
  // list, and preserving the interface to Compilation, enclose the call to the
  // constructor in a block:
  std::unique_ptr<Compilation> C;
  {
    const unsigned DriverBatchSeed = getDriverBatchSeed(*ArgList, Diags);
    const llvm::Optional<unsigned> DriverBatchCount =
        getDriverBatchCount(*ArgList, Diags);
    const llvm::Optional<unsigned> DriverBatchSizeLimit =
        getDriverBatchSizeLimit(*ArgList, Diags);
    const bool SaveTemps = ArgList->hasArg(options::OPT_save_temps);
    const bool ShowDriverTimeCompilation =
        ArgList->hasArg(options::OPT_driver_time_compilation);
    std::unique_ptr<UnifiedStatsReporter> StatsReporter =
        createStatsReporter(ArgList.get(), Inputs, OI, DefaultTargetTriple);

    const bool OnlyOneDependencyFile =
        ArgList->hasFlag(options::OPT_enable_only_one_dependency_file,
                         options::OPT_disable_only_one_dependency_file, false);

    const bool VerifyFineGrainedDependencyGraphAfterEveryImport = ArgList->hasArg(
        options::
            OPT_driver_verify_fine_grained_dependency_graph_after_every_import);
    const bool EmitFineGrainedDependencyDotFileAfterEveryImport = ArgList->hasArg(
        options::
            OPT_driver_emit_fine_grained_dependency_dot_file_after_every_import);
    const bool EnableCrossModuleDependencies
        = ArgList->hasArg(options::OPT_enable_incremental_imports,
                          options::OPT_disable_incremental_imports, true);

    // clang-format off
    C = std::make_unique<Compilation>(
        Diags, TC, OI, Level,
        std::move(ArgList),
        std::move(TranslatedArgList),
        std::move(Inputs),
        buildRecordPath,
        ArgsHash,
        StartTime,
        LastBuildTime,
        DriverFilelistThreshold,
        Incremental,
        BatchMode,
        DriverBatchSeed,
        DriverBatchCount,
        DriverBatchSizeLimit,
        SaveTemps,
        ShowDriverTimeCompilation,
        std::move(StatsReporter),
        OnlyOneDependencyFile,
        VerifyFineGrainedDependencyGraphAfterEveryImport,
        EmitFineGrainedDependencyDotFileAfterEveryImport,
        EnableCrossModuleDependencies);
    // clang-format on
  }

  // Construct the graph of Actions.
  SmallVector<const Action *, 8> TopLevelActions;
  buildActions(TopLevelActions, TC, OI,
               whyIgnoreIncrementality.empty() ? &outOfDateMap : nullptr, *C);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  if (DriverPrintActions) {
    printActions(*C);
    return nullptr;
  }

  buildJobs(TopLevelActions, OI, OFM ? OFM.getPointer() : nullptr,
            workingDirectory, TC, *C);

  if (DriverPrintDerivedOutputFileMap) {
    C->getDerivedOutputFileMap().dump(llvm::outs(), true);
    return nullptr;
  }

  // For getting bulk fixits, or for when users explicitly request to continue
  // building despite errors.
  if (ContinueBuildingAfterErrors)
    C->setContinueBuildingAfterErrors();

  if (ShowIncrementalBuildDecisions || ShowJobLifecycle)
    C->setShowIncrementalBuildDecisions();

  if (ShowJobLifecycle)
    C->setShowJobLifecycle();

  // This has to happen after building jobs, because otherwise we won't even
  // emit .swiftdeps files for the next build.
  if (!whyIgnoreIncrementality.empty())
    C->disableIncrementalBuild(whyIgnoreIncrementality);

  if (Diags.hadAnyError() && !AllowErrors)
    return nullptr;

  if (DriverPrintBindings)
    return nullptr;

  return C;
}

static Arg *makeInputArg(const DerivedArgList &Args, OptTable &Opts,
                         StringRef Value) {
  Arg *A = new Arg(Opts.getOption(options::OPT_INPUT), Value,
                   Args.getBaseArgs().MakeIndex(Value), Value.data());
  A->claim();
  return A;
}

using RemainingArgsHandler = llvm::function_ref<void(InputArgList &, unsigned)>;

std::unique_ptr<InputArgList>
parseArgsUntil(const llvm::opt::OptTable& Opts,
               const char *const *ArgBegin,
               const char *const *ArgEnd,
               unsigned &MissingArgIndex,
               unsigned &MissingArgCount,
               unsigned FlagsToInclude,
               unsigned FlagsToExclude,
               llvm::opt::OptSpecifier UntilOption,
               RemainingArgsHandler RemainingHandler) {
  auto Args = std::make_unique<InputArgList>(ArgBegin, ArgEnd);

  // FIXME: Handle '@' args (or at least error on them).

  bool CheckUntil = UntilOption != options::OPT_INVALID;
  MissingArgIndex = MissingArgCount = 0;
  unsigned Index = 0, End = ArgEnd - ArgBegin;
  while (Index < End) {
    // Ignore empty arguments (other things may still take them as arguments).
    StringRef Str = Args->getArgString(Index);
    if (Str == "") {
      ++Index;
      continue;
    }

    unsigned Prev = Index;
    Arg *A = Opts.ParseOneArg(*Args, Index, FlagsToInclude, FlagsToExclude)
                 .release();
    assert(Index > Prev && "Parser failed to consume argument.");

    // Check for missing argument error.
    if (!A) {
      assert(Index >= End && "Unexpected parser error.");
      assert(Index - Prev - 1 && "No missing arguments!");
      MissingArgIndex = Prev;
      MissingArgCount = Index - Prev - 1;
      break;
    }

    Args->append(A);

    if (CheckUntil && A->getOption().matches(UntilOption)) {
      if (Index < End)
        RemainingHandler(*Args, Index);
      return Args;
    }
  }

  return Args;
}

// Parse all args until we see an input, and then collect the remaining
// arguments into a synthesized "--" option.
static std::unique_ptr<InputArgList>
parseArgStringsForInteractiveDriver(const llvm::opt::OptTable& Opts,
                                    ArrayRef<const char *> Args,
                                    unsigned &MissingArgIndex,
                                    unsigned &MissingArgCount,
                                    unsigned FlagsToInclude,
                                    unsigned FlagsToExclude) {
  return parseArgsUntil(Opts, Args.begin(), Args.end(), MissingArgIndex,
                        MissingArgCount, FlagsToInclude, FlagsToExclude,
                        options::OPT_INPUT,
                        [&](InputArgList &Args, unsigned NextIndex) {
    assert(NextIndex < Args.getNumInputArgStrings());
    // Synthesize -- remaining args...
    Arg *Remaining =
        new Arg(Opts.getOption(options::OPT__DASH_DASH), "--", NextIndex);
    for (unsigned N = Args.getNumInputArgStrings(); NextIndex != N;
         ++NextIndex) {
      Remaining->getValues().push_back(Args.getArgString(NextIndex));
    }
    Args.append(Remaining);
  });
}

std::unique_ptr<InputArgList>
Driver::parseArgStrings(ArrayRef<const char *> Args) {
  unsigned IncludedFlagsBitmask = 0;
  unsigned ExcludedFlagsBitmask = options::NoDriverOption;
  unsigned MissingArgIndex, MissingArgCount;
  std::unique_ptr<InputArgList> ArgList;

  if (driverKind == DriverKind::Interactive) {
    ArgList = parseArgStringsForInteractiveDriver(getOpts(), Args,
        MissingArgIndex, MissingArgCount, IncludedFlagsBitmask,
        ExcludedFlagsBitmask);

  } else {
    ArgList = std::make_unique<InputArgList>(
        getOpts().ParseArgs(Args, MissingArgIndex, MissingArgCount,
                            IncludedFlagsBitmask, ExcludedFlagsBitmask));
  }

  assert(ArgList && "no argument list");

  // Check for missing argument error.
  if (MissingArgCount) {
    Diags.diagnose(SourceLoc(), diag::error_missing_arg_value,
                   ArgList->getArgString(MissingArgIndex), MissingArgCount);
    return nullptr;
  }

  // Check for unknown arguments.
  for (const Arg *A :  ArgList->filtered(options::OPT_UNKNOWN)) {
    Diags.diagnose(SourceLoc(), diag::error_unknown_arg,
                   A->getAsString(*ArgList));
  }

  // Check for unsupported options
  unsigned UnsupportedFlag = 0;
  if (driverKind == DriverKind::Interactive)
    UnsupportedFlag = options::NoInteractiveOption;
  else if (driverKind == DriverKind::Batch)
    UnsupportedFlag = options::NoBatchOption;

  if (UnsupportedFlag)
    for (const Arg *A : *ArgList)
      if (A->getOption().hasFlag(UnsupportedFlag))
        Diags.diagnose(SourceLoc(), diag::error_unsupported_option,
            ArgList->getArgString(A->getIndex()), Name,
            UnsupportedFlag == options::NoBatchOption ? "swift" : "swiftc");

  return ArgList;
}

DerivedArgList *
Driver::translateInputAndPathArgs(const InputArgList &ArgList,
                                  StringRef workingDirectory) const {
  DerivedArgList *DAL = new DerivedArgList(ArgList);

  auto addPath = [workingDirectory, DAL](Arg *A) {
    assert(A->getNumValues() == 1 && "multiple values not handled");
    StringRef path = A->getValue();
    if (workingDirectory.empty() || path == "-" ||
        llvm::sys::path::is_absolute(path)) {
      DAL->append(A);
      return;
    }

    SmallString<64> fullPath{workingDirectory};
    llvm::sys::path::append(fullPath, path);
    unsigned index = DAL->getBaseArgs().MakeIndex(fullPath);
    Arg *newArg = new Arg(A->getOption(), A->getSpelling(), index,
                          DAL->getBaseArgs().getArgString(index), A);
    DAL->AddSynthesizedArg(newArg);
    DAL->append(newArg);
  };

  for (Arg *A : ArgList) {
    if (A->getOption().hasFlag(options::ArgumentIsPath) ||
        A->getOption().matches(options::OPT_INPUT)) {
      addPath(A);
      continue;
    }

    // If we're not in immediate mode, pick up inputs via the -- option.
    if (driverKind != DriverKind::Interactive && A->getOption().matches(options::OPT__DASH_DASH)) {
      A->claim();
      for (unsigned i = 0, e = A->getNumValues(); i != e; ++i) {
        addPath(makeInputArg(*DAL, *Opts, A->getValue(i)));
      }
      continue;
    }

    DAL->append(A);
  }

  return DAL;
}

/// Check that the file referenced by \p Input exists. If it doesn't,
/// issue a diagnostic and return false.
static bool checkInputExistence(const Driver &D, const DerivedArgList &Args,
                                DiagnosticEngine &Diags, StringRef Input) {
  if (!D.getCheckInputFilesExist())
    return true;

  // stdin always exists.
  if (Input == "-")
    return true;

  if (llvm::sys::fs::exists(Input))
    return true;

  Diags.diagnose(SourceLoc(), diag::error_no_such_file_or_directory, Input);
  return false;
}

void Driver::buildInputs(const ToolChain &TC,
                         const DerivedArgList &Args,
                         InputFileList &Inputs) const {
  llvm::DenseMap<StringRef, StringRef> SourceFileNames;
  bool HasVFS = Args.hasArg(options::OPT_vfsoverlay);

  for (Arg *A : Args) {
    if (A->getOption().getKind() == Option::InputClass) {
      StringRef Value = A->getValue();
      file_types::ID Ty = file_types::TY_INVALID;

      // stdin must be handled specially.
      if (Value.equals("-")) {
        // By default, treat stdin as Swift input.
        Ty = file_types::TY_Swift;
      } else {
        // Otherwise lookup by extension.
        Ty = TC.lookupTypeForExtension(llvm::sys::path::extension(Value));

        if (Ty == file_types::TY_INVALID) {
          // By default, treat inputs with no extension, or with an
          // extension that isn't recognized, as object files.
          Ty = file_types::TY_Object;
        }
      }

      if (HasVFS || checkInputExistence(*this, Args, Diags, Value))
        Inputs.push_back(std::make_pair(Ty, A));

      if (Ty == file_types::TY_Swift) {
        StringRef Basename = llvm::sys::path::filename(Value);
        if (!SourceFileNames.insert({Basename, Value}).second) {
          Diags.diagnose(SourceLoc(), diag::error_two_files_same_name,
                         Basename, SourceFileNames[Basename], Value);
          Diags.diagnose(SourceLoc(), diag::note_explain_two_files_same_name);
        }
      }
    }
  }
}

static bool maybeBuildingExecutable(const OutputInfo &OI,
                                    const DerivedArgList &Args,
                                    const InputFileList &Inputs) {
  switch (OI.LinkAction) {
  case LinkKind::Executable:
    return true;
  case LinkKind::DynamicLibrary:
    return false;
  case LinkKind::StaticLibrary:
    return false;
  case LinkKind::None:
    break;
  }

  if (Args.hasArg(options::OPT_parse_as_library, options::OPT_parse_stdlib))
    return false;
  return Inputs.size() == 1;
}

static void diagnoseOutputModeArg(DiagnosticEngine &diags, const Arg *arg,
                                  bool hasInputs, const DerivedArgList &args,
                                  bool isInteractiveDriver,
                                  StringRef driverName) {
  switch (arg->getOption().getID()) {

  case options::OPT_i:
    diags.diagnose(SourceLoc(), diag::error_i_mode,
                   isInteractiveDriver ? driverName : "swift");
    break;

  case options::OPT_repl:
    if (isInteractiveDriver && !hasInputs)
      diags.diagnose(SourceLoc(), diag::warning_unnecessary_repl_mode,
                     args.getArgString(arg->getIndex()), driverName);
    break;

  default:
    break;
  }
}

static bool isSDKTooOld(StringRef sdkPath, llvm::VersionTuple minVersion,
                        StringRef firstPrefix, StringRef secondPrefix = {}) {
  // FIXME: This is a hack.
  // We should be looking at the SDKSettings.plist.
  StringRef sdkDirName = llvm::sys::path::filename(sdkPath);

  size_t versionStart = sdkDirName.rfind(firstPrefix);
  if (versionStart != StringRef::npos) {
    versionStart += firstPrefix.size();
  } else if (!secondPrefix.empty()) {
    versionStart = sdkDirName.rfind(secondPrefix);
    if (versionStart != StringRef::npos)
      versionStart += secondPrefix.size();
  }
  if (versionStart == StringRef::npos)
    return false;

  size_t versionEnd = sdkDirName.rfind(".Internal");
  if (versionEnd == StringRef::npos)
    versionEnd = sdkDirName.rfind(".sdk");
  if (versionEnd == StringRef::npos)
    return false;

  llvm::VersionTuple version;
  if (version.tryParse(sdkDirName.slice(versionStart, versionEnd)))
    return false;
  return version < minVersion;
}

/// Returns true if the given SDK path points to an SDK that is too old for
/// the given target.
static bool isSDKTooOld(StringRef sdkPath, const llvm::Triple &target) {
  if (target.isMacOSX()) {
    return isSDKTooOld(sdkPath, llvm::VersionTuple(10, 15), "OSX");

  } else if (target.isiOS()) {
    // Includes both iOS and TVOS.
    return isSDKTooOld(sdkPath, llvm::VersionTuple(13, 0), "Simulator", "OS");

  } else if (target.isWatchOS()) {
    return isSDKTooOld(sdkPath, llvm::VersionTuple(6, 0), "Simulator", "OS");

  } else {
    return false;
  }
}

void Driver::buildOutputInfo(const ToolChain &TC, const DerivedArgList &Args,
                             const bool BatchMode, const InputFileList &Inputs,
                             OutputInfo &OI) const {

  if (const Arg *A = Args.getLastArg(options::OPT_lto)) {
    auto LTOVariant =
        llvm::StringSwitch<llvm::Optional<OutputInfo::LTOKind>>(A->getValue())
            .Case("llvm-thin", OutputInfo::LTOKind::LLVMThin)
            .Case("llvm-full", OutputInfo::LTOKind::LLVMFull)
            .Default(llvm::None);
    if (LTOVariant)
      OI.LTOVariant = LTOVariant.value();
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  }
  
  if (const Arg *A = Args.getLastArg(options::OPT_lto_library)) {
    OI.LibLTOPath = A->getValue();
  } else {
    OI.LibLTOPath = "";
  }

  auto CompilerOutputType = OI.LTOVariant != OutputInfo::LTOKind::None
                             ? file_types::TY_LLVM_BC
                             : file_types::TY_Object;
  // By default, the driver does not link its output; this will be updated
  // appropriately below if linking is required.

  OI.CompilerOutputType = driverKind == DriverKind::Interactive
                              ? file_types::TY_Nothing
                              : CompilerOutputType;

  if (const Arg *A = Args.getLastArg(options::OPT_num_threads)) {
    if (BatchMode) {
      Diags.diagnose(SourceLoc(), diag::warning_cannot_multithread_batch_mode);
    } else if (StringRef(A->getValue()).getAsInteger(10, OI.numThreads)) {
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
    }
  }

  const Arg *const OutputModeArg = Args.getLastArg(options::OPT_modes_Group);

  if (!OutputModeArg) {
    if (Args.hasArg(options::OPT_emit_module, options::OPT_emit_module_path)) {
      OI.CompilerOutputType = file_types::TY_SwiftModuleFile;
    } else if (driverKind != DriverKind::Interactive) {
      OI.LinkAction = LinkKind::Executable;
    }
  } else {
    diagnoseOutputModeArg(Diags, OutputModeArg, !Inputs.empty(), Args,
                          driverKind == DriverKind::Interactive, Name);

    switch (OutputModeArg->getOption().getID()) {
    case options::OPT_emit_executable:
      if (Args.hasArg(options::OPT_static))
        Diags.diagnose(SourceLoc(),
                       diag::error_static_emit_executable_disallowed);
                       
      OI.LinkAction = LinkKind::Executable;
      OI.CompilerOutputType = CompilerOutputType;
      break;

    case options::OPT_emit_library:
      OI.LinkAction = Args.hasArg(options::OPT_static) ?
                      LinkKind::StaticLibrary :
                      LinkKind::DynamicLibrary;
      OI.CompilerOutputType = CompilerOutputType;
      break;

    case options::OPT_static:
      break;

    case options::OPT_emit_object:
      OI.CompilerOutputType = file_types::TY_Object;
      if (OI.LTOVariant != OutputInfo::LTOKind::None)
        OI.CompilerOutputType = file_types::TY_LLVM_BC;
      break;

    case options::OPT_emit_assembly:
      OI.CompilerOutputType = file_types::TY_Assembly;
      break;

    case options::OPT_emit_sil:
      OI.CompilerOutputType = file_types::TY_SIL;
      break;

    case options::OPT_emit_silgen:
      OI.CompilerOutputType = file_types::TY_RawSIL;
      break;

    case options::OPT_emit_sib:
      OI.CompilerOutputType = file_types::TY_SIB;
      break;

    case options::OPT_emit_sibgen:
      OI.CompilerOutputType = file_types::TY_RawSIB;
      break;

    case options::OPT_emit_irgen:
    case options::OPT_emit_ir:
      OI.CompilerOutputType = file_types::TY_LLVM_IR;
      break;

    case options::OPT_emit_bc:
      OI.CompilerOutputType = file_types::TY_LLVM_BC;
      break;

    case options::OPT_dump_ast:
      OI.CompilerOutputType = file_types::TY_ASTDump;
      break;

    case options::OPT_emit_pch:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_PCH;
      break;

    case options::OPT_emit_pcm:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_ClangModuleFile;
      break;

    case options::OPT_emit_imported_modules:
      OI.CompilerOutputType = file_types::TY_ImportedModules;
      // We want the imported modules from the module as a whole, not individual
      // files, so let's do it in one invocation rather than having to collate
      // later.
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      break;

    case options::OPT_scan_dependencies:
      OI.CompilerOutputType = file_types::TY_JSONDependencies;
      // We want the imported modules from the module as a whole, not individual
      // files, so let's do it in one invocation rather than having to collate
      // later.
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      break;

    case options::OPT_index_file:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_IndexData;
      break;

    case options::OPT_update_code:
      OI.CompilerOutputType = file_types::TY_Remapping;
      OI.LinkAction = LinkKind::None;
      break;

    case options::OPT_parse:
    case options::OPT_resolve_imports:
    case options::OPT_typecheck:
    case options::OPT_dump_parse:
    case options::OPT_print_ast:
    case options::OPT_dump_type_refinement_contexts:
    case options::OPT_dump_scope_maps:
    case options::OPT_dump_interface_hash:
    case options::OPT_dump_type_info:
    case options::OPT_verify_debug_info:
      OI.CompilerOutputType = file_types::TY_Nothing;
      break;

    case options::OPT_dump_pcm:
      OI.CompilerMode = OutputInfo::Mode::SingleCompile;
      OI.CompilerOutputType = file_types::TY_Nothing;
      break;

    case options::OPT_i:
      // Keep the default output/mode; this flag was removed and should already
      // have been diagnosed above.
      assert(Diags.hadAnyError() && "-i flag was removed");
      break;

    case options::OPT_repl:
    case options::OPT_deprecated_integrated_repl:
    case options::OPT_lldb_repl:
      OI.CompilerOutputType = file_types::TY_Nothing;
      OI.CompilerMode = OutputInfo::Mode::REPL;
      break;

    default:
      llvm_unreachable("unknown mode");
    }
  }

  assert(OI.CompilerOutputType != file_types::ID::TY_INVALID);

  if (const Arg *A = Args.getLastArg(options::OPT_g_Group)) {
    if (A->getOption().matches(options::OPT_g))
      OI.DebugInfoLevel = IRGenDebugInfoLevel::Normal;
    else if (A->getOption().matches(options::OPT_gline_tables_only))
      OI.DebugInfoLevel = IRGenDebugInfoLevel::LineTables;
    else if (A->getOption().matches(options::OPT_gdwarf_types))
      OI.DebugInfoLevel = IRGenDebugInfoLevel::DwarfTypes;
    else
      assert(A->getOption().matches(options::OPT_gnone) &&
             "unknown -g<kind> option");
  }

  if (const Arg *A = Args.getLastArg(options::OPT_debug_info_format)) {
    if (strcmp(A->getValue(), "dwarf") == 0)
      OI.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
    else if (strcmp(A->getValue(), "codeview") == 0)
      OI.DebugInfoFormat = IRGenDebugInfoFormat::CodeView;
    else
      Diags.diagnose(SourceLoc(), diag::error_invalid_arg_value,
                     A->getAsString(Args), A->getValue());
  } else if (OI.DebugInfoLevel > IRGenDebugInfoLevel::None) {
    // If -g was specified but not -debug-info-format, DWARF is assumed.
    OI.DebugInfoFormat = IRGenDebugInfoFormat::DWARF;
  }
  if (Args.hasArg(options::OPT_debug_info_format) &&
      !Args.hasArg(options::OPT_g_Group)) {
    const Arg *debugFormatArg = Args.getLastArg(options::OPT_debug_info_format);
    Diags.diagnose(SourceLoc(), diag::error_option_missing_required_argument,
                   debugFormatArg->getAsString(Args), "-g");
  }
  if (OI.DebugInfoFormat == IRGenDebugInfoFormat::CodeView &&
      (OI.DebugInfoLevel == IRGenDebugInfoLevel::LineTables ||
       OI.DebugInfoLevel == IRGenDebugInfoLevel::DwarfTypes)) {
    const Arg *debugFormatArg = Args.getLastArg(options::OPT_debug_info_format);
    Diags.diagnose(SourceLoc(), diag::error_argument_not_allowed_with,
                   debugFormatArg->getAsString(Args),
                   OI.DebugInfoLevel == IRGenDebugInfoLevel::LineTables
                     ? "-gline-tables-only"
                     : "-gdwarf_types");
  }

  if (Args.hasArg(options::OPT_emit_module, options::OPT_emit_module_path)) {
    // The user has requested a module, so generate one and treat it as
    // top-level output.
    OI.ShouldGenerateModule = true;
    OI.ShouldTreatModuleAsTopLevelOutput = true;
  } else if (OI.DebugInfoLevel > IRGenDebugInfoLevel::LineTables &&
             OI.shouldLink()) {
    // An option has been passed which requires a module, but the user hasn't
    // requested one. Generate a module, but treat it as an intermediate output.
    OI.ShouldGenerateModule = true;
    OI.ShouldTreatModuleAsTopLevelOutput = false;
  } else if (Args.hasArg(options::OPT_emit_objc_header,
                         options::OPT_emit_objc_header_path,
                         options::OPT_emit_module_interface,
                         options::OPT_emit_module_interface_path,
                         options::OPT_emit_private_module_interface_path) &&
             OI.CompilerMode != OutputInfo::Mode::SingleCompile) {
    // An option has been passed which requires whole-module knowledge, but we
    // don't have that. Generate a module, but treat it as an intermediate
    // output.
    OI.ShouldGenerateModule = true;
    OI.ShouldTreatModuleAsTopLevelOutput = false;
  } else {
    // No options require a module, so don't generate one.
    OI.ShouldGenerateModule = false;
    OI.ShouldTreatModuleAsTopLevelOutput = false;
  }

  if (OI.ShouldGenerateModule &&
      (OI.CompilerMode == OutputInfo::Mode::REPL ||
       OI.CompilerMode == OutputInfo::Mode::Immediate)) {
    Diags.diagnose(SourceLoc(), diag::error_mode_cannot_emit_module);
    return;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_module_name)) {
    OI.ModuleName = A->getValue();
  } else if (OI.CompilerMode == OutputInfo::Mode::REPL) {
    // REPL mode should always use the REPL module.
    OI.ModuleName = "REPL";
  } else if (const Arg *A = Args.getLastArg(options::OPT_o)) {
    OI.ModuleName = llvm::sys::path::stem(A->getValue()).str();
    if ((OI.LinkAction == LinkKind::DynamicLibrary ||
         OI.LinkAction == LinkKind::StaticLibrary) &&
        !llvm::sys::path::extension(A->getValue()).empty() &&
        StringRef(OI.ModuleName).startswith("lib")) {
      // Chop off a "lib" prefix if we're building a library.
      OI.ModuleName.erase(0, strlen("lib"));
    }
  } else if (Inputs.size() == 1) {
    OI.ModuleName =
        llvm::sys::path::stem(Inputs.front().second->getValue()).str();
  }

  if (!Lexer::isIdentifier(OI.ModuleName) ||
      (OI.ModuleName == STDLIB_NAME &&
       !Args.hasArg(options::OPT_parse_stdlib))) {
    OI.ModuleNameIsFallback = true;
    if (OI.CompilerOutputType == file_types::TY_Nothing ||
        maybeBuildingExecutable(OI, Args, Inputs))
      OI.ModuleName = "main";
    else if (!Inputs.empty() || OI.CompilerMode == OutputInfo::Mode::REPL) {
      // Having an improper module name is only bad if we have inputs or if
      // we're in REPL mode.
      auto DID = (OI.ModuleName == STDLIB_NAME) ? diag::error_stdlib_module_name
                                                : diag::error_bad_module_name;
      Diags.diagnose(SourceLoc(), DID,
                     OI.ModuleName, !Args.hasArg(options::OPT_module_name));
      OI.ModuleName = "__bad__";
    }
  }

  {
    if (const Arg *A = Args.getLastArg(options::OPT_sdk)) {
      OI.SDKPath = A->getValue();
    } else if (const char *SDKROOT = getenv("SDKROOT")) {
      OI.SDKPath = SDKROOT;
    } else if (OI.CompilerMode == OutputInfo::Mode::Immediate ||
               OI.CompilerMode == OutputInfo::Mode::REPL) {
      if (TC.getTriple().isMacOSX()) {
        // In immediate modes, use the SDK provided by xcrun.
        // This will prefer the SDK alongside the Swift found by "xcrun swift".
        // We don't do this in compilation modes because defaulting to the
        // latest SDK may not be intended.
        auto xcrunPath = llvm::sys::findProgramByName("xcrun");
        if (!xcrunPath.getError()) {
          const char *args[] = {
            "--show-sdk-path", "--sdk", "macosx", nullptr
          };
          sys::TaskQueue queue;
          queue.addTask(xcrunPath->c_str(), args);
          queue.execute(nullptr,
                        [&OI](sys::ProcessId PID, int returnCode,
                              StringRef output, StringRef errors,
                              sys::TaskProcessInformation ProcInfo,
                              void *unused) -> sys::TaskFinishedResponse {
            if (returnCode == 0) {
              output = output.rtrim();
              auto lastLineStart = output.find_last_of("\n\r");
              if (lastLineStart != StringRef::npos)
                output = output.substr(lastLineStart+1);
              if (output.empty())
                OI.SDKPath = "/";
              else
                OI.SDKPath = output.str();
            }
            return sys::TaskFinishedResponse::ContinueExecution;
          });
        }
      }
    }

    if (!OI.SDKPath.empty()) {
      // Delete a trailing /.
      if (OI.SDKPath.size() > 1 &&
          llvm::sys::path::is_separator(OI.SDKPath.back())) {
        OI.SDKPath.erase(OI.SDKPath.end()-1);
      }

      if (!llvm::sys::fs::exists(OI.SDKPath)) {
        Diags.diagnose(SourceLoc(), diag::warning_no_such_sdk, OI.SDKPath);
      } else if (isSDKTooOld(OI.SDKPath, TC.getTriple())) {
        Diags.diagnose(SourceLoc(), diag::error_sdk_too_old,
                       llvm::sys::path::filename(OI.SDKPath));
      }
    }
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_EQ))
    OI.SelectedSanitizers = parseSanitizerArgValues(
        Args, A, TC.getTriple(), Diags,
        [&](StringRef sanitizerName, bool shared) {
          return TC.sanitizerRuntimeLibExists(Args, sanitizerName, shared);
        });

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_recover_EQ)) {
    // Just validate the args. The frontend will parse these again and actually
    // use them. To avoid emitting warnings multiple times we suppress warnings
    // here but not in the frontend.
    (void)parseSanitizerRecoverArgValues(A, OI.SelectedSanitizers, Diags,
                                         /*emitWarnings=*/false);
  }

  if (const Arg *A = Args.getLastArg(options::OPT_sanitize_coverage_EQ)) {

    // Check that the sanitizer coverage flags are supported if supplied.
    // Dismiss the output, as we will grab the value later.
    (void)parseSanitizerCoverageArgValue(A, TC.getTriple(), Diags,
                                         OI.SelectedSanitizers);

  }

  if (TC.getTriple().isOSWindows()) {
    if (const Arg *A = Args.getLastArg(options::OPT_libc)) {
      OI.RuntimeVariant =
          llvm::StringSwitch<llvm::Optional<OutputInfo::MSVCRuntime>>(
              A->getValue())
              .Cases("MD", "MultiThreadedDLL", "shared-ucrt",
                     OutputInfo::MSVCRuntime::MultiThreadedDLL)
              .Cases("MDd", "MultiThreadedDebugDLL", "shared-debug-ucrt",
                     OutputInfo::MSVCRuntime::MultiThreadedDebugDLL)
              .Cases("MT", "MultiThreaded", "static-ucrt",
                     OutputInfo::MSVCRuntime::MultiThreaded)
              .Cases("MTd", "MultiThreadedDebug", "static-debug-ucrt",
                     OutputInfo::MSVCRuntime::MultiThreadedDebug)
              .Default(llvm::None);
      if (!OI.RuntimeVariant)
        Diags.diagnose({}, diag::error_invalid_arg_value, A->getSpelling(),
                       A->getValue());
    } else {
      // NOTE: default to `/MD`.  This is different from `cl`'s default
      // behaviour of `/MT` on the command line, however, Visual Studio 2015 and
      // newer will default `/MD` as well.  Furthermore, this is far more useful
      // of a mode since the `/MT` mode requires that everything is statically
      // linked.
      OI.RuntimeVariant = OutputInfo::MSVCRuntime::MultiThreadedDLL;
    }
  }
}

OutputInfo::Mode
Driver::computeCompilerMode(const DerivedArgList &Args,
                            const InputFileList &Inputs,
                            bool &BatchModeOut) const {

  if (driverKind == Driver::DriverKind::Interactive)
    return Inputs.empty() ? OutputInfo::Mode::REPL
                          : OutputInfo::Mode::Immediate;

  bool UseWMO = Args.hasFlag(options::OPT_whole_module_optimization,
                             options::OPT_no_whole_module_optimization,
                             false);

  const Arg *ArgRequiringSingleCompile = Args.getLastArg(
      options::OPT_index_file,
      UseWMO ? options::OPT_whole_module_optimization : llvm::opt::OptSpecifier());

  BatchModeOut = Args.hasFlag(options::OPT_enable_batch_mode,
                              options::OPT_disable_batch_mode,
                              false);

  // AST dump doesn't work with `-wmo`. Since it's not common to want to dump
  // the AST, we assume that's the priority and ignore `-wmo`, but we warn the
  // user about this decision.
  // FIXME: AST dump also doesn't work with `-index-file`, but that fix is a bit
  // more complicated than this.
  if (UseWMO && Args.hasArg(options::OPT_dump_ast)) {
    Diags.diagnose(SourceLoc(), diag::warn_ignoring_wmo);
    return OutputInfo::Mode::StandardCompile;
  }

  // Override batch mode if given -wmo or -index-file.
  if (ArgRequiringSingleCompile) {
    if (BatchModeOut) {
      BatchModeOut = false;
      // Emit a warning about such overriding (FIXME: we might conditionalize
      // this based on the user or xcode passing -disable-batch-mode).
      Diags.diagnose(SourceLoc(), diag::warn_ignoring_batch_mode,
                     ArgRequiringSingleCompile->getOption().getPrefixedName());
    }
    return OutputInfo::Mode::SingleCompile;
  }

  return OutputInfo::Mode::StandardCompile;
}

namespace {
/// Encapsulates the computation of input jobs that are relevant to the
/// merge-modules job the scheduler can insert if we are not in a single compile
/// mode.
class ModuleInputs final {
private:
  using InputInfo = IncrementalJobAction::InputInfo;
  SmallVector<const Action *, 2> AllModuleInputs;
  InputInfo StatusBound;

public:
  explicit ModuleInputs()
      : StatusBound
            {InputInfo::Status::UpToDate, llvm::sys::TimePoint<>::min()} {}

public:
  void addInput(const Action *inputAction) {
    if (auto *IJA = dyn_cast<IncrementalJobAction>(inputAction)) {
      // Take the upper bound of the status of any incremental inputs to
      // ensure that the merge-modules job gets run if *any* input job is run.
      const auto conservativeStatus =
          std::max(StatusBound.status, IJA->getInputInfo().status);
      // The modification time here is not important to the rest of the
      // incremental build. We take the upper bound in case an attempt to
      // compare the swiftmodule output's mod time and any input files is
      // made. If the compilation has been correctly scheduled, the
      // swiftmodule's mod time will always strictly exceed the mod time of
      // any of its inputs when we are able to skip it.
      const auto conservativeModTime = std::max(
          StatusBound.previousModTime, IJA->getInputInfo().previousModTime);
      StatusBound = InputInfo{conservativeStatus, conservativeModTime};
    }
    AllModuleInputs.push_back(inputAction);
  }

public:
  /// Returns \c true if no inputs have been registered with this instance.
  bool empty() const { return AllModuleInputs.empty(); }

public:
  /// Consumes this \c ModuleInputs instance and returns a merge-modules action
  /// from the list of input actions and status it has computed thus far.
  JobAction *intoAction(Compilation &C) && {
    return C.createAction<MergeModuleJobAction>(AllModuleInputs, StatusBound);
  }
};
} // namespace

void Driver::buildActions(SmallVectorImpl<const Action *> &TopLevelActions,
                          const ToolChain &TC, const OutputInfo &OI,
                          const InputInfoMap *OutOfDateMap,
                          Compilation &C) const {
  const DerivedArgList &Args = C.getArgs();
  ArrayRef<InputPair> Inputs = C.getInputFiles();

  if (!SuppressNoInputFilesError && Inputs.empty()) {
    Diags.diagnose(SourceLoc(), diag::error_no_input_files);
    return;
  }

  ModuleInputs AllModuleInputs;
  SmallVector<const Action *, 2> AllLinkerInputs;

  switch (OI.CompilerMode) {
  case OutputInfo::Mode::StandardCompile: {

    // If the user is importing a textual (.h) bridging header and we're in
    // standard-compile (non-WMO) mode, we take the opportunity to precompile
    // the header into a temporary PCH, and replace the import argument with the
    // PCH in the subsequent frontend jobs.
    JobAction *PCH = nullptr;
    if (Args.hasFlag(options::OPT_enable_bridging_pch,
                     options::OPT_disable_bridging_pch,
                     true)) {
      if (Arg *A = Args.getLastArg(options::OPT_import_objc_header)) {
        StringRef Value = A->getValue();
        auto Ty = TC.lookupTypeForExtension(llvm::sys::path::extension(Value));
        if (Ty == file_types::TY_ClangHeader) {
          auto *HeaderInput = C.createAction<InputAction>(*A, Ty);
          StringRef PersistentPCHDir;
          if (const Arg *A = Args.getLastArg(options::OPT_pch_output_dir)) {
            PersistentPCHDir = A->getValue();
          }
          PCH = C.createAction<GeneratePCHJobAction>(HeaderInput,
                                                     PersistentPCHDir);
        }
      }
    }

    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      Action *Current = C.createAction<InputAction>(*InputArg, InputType);
      switch (InputType) {
      case file_types::TY_Swift:
      case file_types::TY_SIL:
      case file_types::TY_SIB: {
        // Source inputs always need to be compiled.
        assert(file_types::isPartOfSwiftCompilation(InputType));

        auto previousBuildState =
            IncrementalJobAction::InputInfo::makeNeedsCascadingRebuild();
        if (OutOfDateMap)
          previousBuildState = OutOfDateMap->lookup(InputArg);
        if (Args.hasArg(options::OPT_embed_bitcode)) {
          Current = C.createAction<CompileJobAction>(
              Current, file_types::TY_LLVM_BC, previousBuildState);
          if (PCH)
            cast<JobAction>(Current)->addInput(PCH);
          AllModuleInputs.addInput(Current);
          Current = C.createAction<BackendJobAction>(Current,
                                                     OI.CompilerOutputType, 0);
        } else {
          Current = C.createAction<CompileJobAction>(Current,
                                                     OI.CompilerOutputType,
                                                     previousBuildState);
          if (PCH)
            cast<JobAction>(Current)->addInput(PCH);
          AllModuleInputs.addInput(Current);
        }
        AllLinkerInputs.push_back(Current);
        break;
      }
      case file_types::TY_SwiftModuleFile:
      case file_types::TY_SwiftModuleDocFile:
      case file_types::TY_SwiftSourceInfoFile:
        if (OI.ShouldGenerateModule && !OI.shouldLink()) {
          // When generating a .swiftmodule as a top-level output (as opposed
          // to, for example, linking an image), treat .swiftmodule files as
          // inputs to a MergeModule action.
          AllModuleInputs.addInput(Current);
          break;
        } else if (OI.shouldLink()) {
          // Otherwise, if linking, pass .swiftmodule files as inputs to the
          // linker, so that their debug info is available.
          AllLinkerInputs.push_back(Current);
          break;
        } else {
          Diags.diagnose(SourceLoc(), diag::error_unexpected_input_file,
                         InputArg->getValue());
          continue;
        }
      case file_types::TY_AutolinkFile:
      case file_types::TY_Object:
      case file_types::TY_TBD:
        // Object inputs are only okay if linking.
        if (OI.shouldLink()) {
          AllLinkerInputs.push_back(Current);
          break;
        }
        LLVM_FALLTHROUGH;
      case file_types::TY_ASTDump:
      case file_types::TY_Image:
      case file_types::TY_dSYM:
      case file_types::TY_Dependencies:
      case file_types::TY_Assembly:
      case file_types::TY_LLVM_IR:
      case file_types::TY_LLVM_BC:
      case file_types::TY_SerializedDiagnostics:
      case file_types::TY_ClangHeader:
      case file_types::TY_ClangModuleFile:
      case file_types::TY_SwiftDeps:
      case file_types::TY_ExternalSwiftDeps:
      case file_types::TY_Remapping:
      case file_types::TY_IndexData:
      case file_types::TY_PCH:
      case file_types::TY_ImportedModules:
      case file_types::TY_ModuleTrace:
      case file_types::TY_YAMLOptRecord:
      case file_types::TY_BitstreamOptRecord:
      case file_types::TY_SwiftModuleInterfaceFile:
      case file_types::TY_PrivateSwiftModuleInterfaceFile:
      case file_types::TY_SwiftModuleSummaryFile:
      case file_types::TY_SwiftCrossImportDir:
      case file_types::TY_SwiftOverlayFile:
      case file_types::TY_JSONDependencies:
      case file_types::TY_JSONFeatures:
      case file_types::TY_SwiftABIDescriptor:
      case file_types::TY_ConstValues:
      case file_types::TY_SwiftFixIt:
      case file_types::TY_ModuleSemanticInfo:
      case file_types::TY_CachedDiagnostics:
        // We could in theory handle assembly or LLVM input, but let's not.
        // FIXME: What about LTO?
        Diags.diagnose(SourceLoc(), diag::error_unexpected_input_file,
                       InputArg->getValue());
        continue;
      case file_types::TY_RawSIB:
      case file_types::TY_RawSIL:
      case file_types::TY_Nothing:
      case file_types::TY_IndexUnitOutputPath:
      case file_types::TY_INVALID:
        llvm_unreachable("these types should never be inferred");
      }
    }
    break;
  }
  case OutputInfo::Mode::SingleCompile: {
    if (Inputs.empty()) break;
    if (Args.hasArg(options::OPT_emit_pcm) && Inputs.size() != 1) {
      // -emit-pcm mode requires exactly one input (the module map).
      Diags.diagnose(SourceLoc(), diag::error_mode_requires_one_input_file);
      return;
    }
    if (Args.hasArg(options::OPT_embed_bitcode)) {
      // Make sure we can handle the inputs.
      bool HandledHere = true;
      for (const InputPair &Input : Inputs) {
        file_types::ID InputType = Input.first;
        if (!file_types::isPartOfSwiftCompilation(InputType)) {
          HandledHere = false;
          break;
        }
      }
      if (HandledHere) {
        // Create a single CompileJobAction and a single BackendJobAction.
        JobAction *CA =
            C.createAction<CompileJobAction>(file_types::TY_LLVM_BC);
        AllModuleInputs.addInput(CA);

        int InputIndex = 0;
        for (const InputPair &Input : Inputs) {
          file_types::ID InputType = Input.first;
          const Arg *InputArg = Input.second;

          CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
          if (OI.isMultiThreading()) {
            // With multi-threading we need a backend job for each output file
            // of the compilation.
            auto *BJA = C.createAction<BackendJobAction>(CA,
                                                         OI.CompilerOutputType,
                                                         InputIndex);
            AllLinkerInputs.push_back(BJA);
          }
          ++InputIndex;
        }
        if (!OI.isMultiThreading()) {
          // No multi-threading: the compilation only produces a single output
          // file.
          CA = C.createAction<BackendJobAction>(CA, OI.CompilerOutputType, 0);
          AllLinkerInputs.push_back(CA);
        }
        break;
      }
    }

    // Create a single CompileJobAction for all of the driver's inputs.
    auto *CA = C.createAction<CompileJobAction>(OI.CompilerOutputType);
    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
    }
    AllModuleInputs.addInput(CA);
    AllLinkerInputs.push_back(CA);
    break;
  }
  case OutputInfo::Mode::BatchModeCompile: {
    llvm_unreachable("Batch mode should not be used to build actions");
  }
  case OutputInfo::Mode::Immediate: {
    if (Inputs.empty())
      return;

    assert(OI.CompilerOutputType == file_types::TY_Nothing);
    auto *CA = C.createAction<InterpretJobAction>();
    for (const InputPair &Input : Inputs) {
      file_types::ID InputType = Input.first;
      const Arg *InputArg = Input.second;

      CA->addInput(C.createAction<InputAction>(*InputArg, InputType));
    }
    TopLevelActions.push_back(CA);
    return;
  }
  case OutputInfo::Mode::REPL: {
    if (!Inputs.empty()) {
      // REPL mode requires no inputs.
      Diags.diagnose(SourceLoc(), diag::error_repl_requires_no_input_files);
      return;
    }

    REPLJobAction::Mode Mode = REPLJobAction::Mode::PreferLLDB;
    if (const Arg *A = Args.getLastArg(options::OPT_lldb_repl,
                                       options::OPT_deprecated_integrated_repl)) {
      if (A->getOption().matches(options::OPT_lldb_repl))
        Mode = REPLJobAction::Mode::RequireLLDB;
      else
        Mode = REPLJobAction::Mode::Integrated;
    }

    TopLevelActions.push_back(C.createAction<REPLJobAction>(Mode));
    return;
  }
  }

  JobAction *MergeModuleAction = nullptr;
  if (OI.ShouldGenerateModule &&
      OI.CompilerMode != OutputInfo::Mode::SingleCompile &&
      !AllModuleInputs.empty()) {
    // We're performing multiple compilations; set up a merge module step
    // so we generate a single swiftmodule as output.
    MergeModuleAction = std::move(AllModuleInputs).intoAction(C);
  }

  bool shouldPerformLTO = OI.LTOVariant != OutputInfo::LTOKind::None;
  if (OI.shouldLink() && !AllLinkerInputs.empty()) {
    JobAction *LinkAction = nullptr;

    if (OI.LinkAction == LinkKind::StaticLibrary) {
      LinkAction =
          C.createAction<StaticLinkJobAction>(AllLinkerInputs, OI.LinkAction);
    } else {
      LinkAction = C.createAction<DynamicLinkJobAction>(
          AllLinkerInputs, OI.LinkAction, shouldPerformLTO);
    }

    // On ELF platforms there's no built in autolinking mechanism, so we
    // pull the info we need from the .o files directly and pass them as an
    // argument input file to the linker.
    const auto &Triple = TC.getTriple();
    SmallVector<const Action *, 2> AutolinkExtractInputs;
    for (const Action *A : AllLinkerInputs)
      if (A->getType() == file_types::TY_Object) {
        // Shared objects on ELF platforms don't have a swift1_autolink_entries
        // section in them because the section in the .o files is marked as
        // SHF_EXCLUDE.
        if (auto *IA = dyn_cast<InputAction>(A)) {
          StringRef ObjectName = IA->getInputArg().getValue();
          if (Triple.getObjectFormat() == llvm::Triple::ELF &&
              ObjectName.endswith(".so"))
            continue;
        }
        AutolinkExtractInputs.push_back(A);
      }
    const bool AutolinkExtractRequired =
        ((Triple.getObjectFormat() == llvm::Triple::ELF && !Triple.isPS4()) ||
         Triple.getObjectFormat() == llvm::Triple::Wasm ||
         Triple.isOSCygMing()) &&
        !shouldPerformLTO;
    if (!AutolinkExtractInputs.empty() && AutolinkExtractRequired) {
      auto *AutolinkExtractAction =
          C.createAction<AutolinkExtractJobAction>(AutolinkExtractInputs);
      // Takes the same inputs as the linker...
      // ...and gives its output to the linker.
      LinkAction->addInput(AutolinkExtractAction);
    }

    if (MergeModuleAction) {
      if (OI.DebugInfoLevel == IRGenDebugInfoLevel::Normal) {
        const bool ModuleWrapRequired =
            Triple.getObjectFormat() != llvm::Triple::MachO;
        if (ModuleWrapRequired) {
          auto *ModuleWrapAction =
              C.createAction<ModuleWrapJobAction>(MergeModuleAction);
          LinkAction->addInput(ModuleWrapAction);
        } else {
          LinkAction->addInput(MergeModuleAction);
        }
        // FIXME: Adding the MergeModuleAction as top-level regardless would
        // allow us to get rid of the special case flag for that.
      } else {
        TopLevelActions.push_back(MergeModuleAction);
      }
    }
    TopLevelActions.push_back(LinkAction);

    if (TC.getTriple().isOSDarwin()) {
      switch (OI.LinkAction) {
      case LinkKind::Executable:
      case LinkKind::DynamicLibrary:
        if (OI.DebugInfoLevel > IRGenDebugInfoLevel::None) {
          auto *dSYMAction = C.createAction<GenerateDSYMJobAction>(LinkAction);
          TopLevelActions.push_back(dSYMAction);
          if (Args.hasArg(options::OPT_verify_debug_info)) {
            TopLevelActions.push_back(
                C.createAction<VerifyDebugInfoJobAction>(dSYMAction));
          }
        }
        break;

      case LinkKind::None:
        LLVM_FALLTHROUGH;
      case LinkKind::StaticLibrary:
        // Cannot build the DSYM bundle for non-image targets.
        break;
      }
    }
  } else {
    // We can't rely on the merge module action being the only top-level
    // action that needs to run. There may be other actions (e.g.
    // BackendJobActions) that are not merge-module inputs but should be run
    // anyway.
    if (MergeModuleAction)
      TopLevelActions.push_back(MergeModuleAction);
    TopLevelActions.append(AllLinkerInputs.begin(), AllLinkerInputs.end());
  }

#ifdef NDEBUG
  bool verifyInterfacesByDefault = false;
#else
  bool verifyInterfacesByDefault = true;
#endif

  if (MergeModuleAction
      && Args.hasArg(options::OPT_enable_library_evolution)
      && Args.hasFlag(options::OPT_verify_emitted_module_interface,
                      options::OPT_no_verify_emitted_module_interface,
                      verifyInterfacesByDefault)) {
    if (Args.hasArgNoClaim(options::OPT_emit_module_interface,
                           options::OPT_emit_module_interface_path)) {
      TopLevelActions.push_back(
          C.createAction<VerifyModuleInterfaceJobAction>(MergeModuleAction,
              file_types::TY_SwiftModuleInterfaceFile));
    }

    if (Args.hasArgNoClaim(options::OPT_emit_private_module_interface_path)) {
      TopLevelActions.push_back(
          C.createAction<VerifyModuleInterfaceJobAction>(MergeModuleAction,
              file_types::TY_PrivateSwiftModuleInterfaceFile));
    }
  }
}

bool Driver::handleImmediateArgs(const ArgList &Args, const ToolChain &TC) {
  if (Args.hasArg(options::OPT_help)) {
    printHelp(false);
    return false;
  }

  if (Args.hasArg(options::OPT_help_hidden)) {
    printHelp(true);
    return false;
  }

  if (Args.hasArg(options::OPT_version)) {
    // Follow gcc/clang behavior and use stdout for --version and stderr for -v.
    printVersion(TC, llvm::outs());
    return false;
  }

  if (Args.hasArg(options::OPT_v)) {
    printVersion(TC, llvm::errs());
    SuppressNoInputFilesError = true;
  }

  if (const Arg *A = Args.getLastArg(options::OPT_driver_use_frontend_path)) {
    DriverExecutable = A->getValue();
    std::string commandString =
        Args.getLastArgValue(options::OPT_driver_use_frontend_path).str();
    SmallVector<StringRef, 10> commandArgs;
    StringRef(commandString).split(commandArgs, ';', -1, false);
    DriverExecutable = commandArgs[0].str();
    DriverExecutableArgs.assign(std::begin(commandArgs) + 1,
                                std::end(commandArgs));
  }

  if (Args.hasArg(options::OPT_print_target_info)) {
    SmallVector<const char *, 5> commandLine;
    commandLine.push_back("-frontend");
    commandLine.push_back("-print-target-info");
    if (const Arg *targetArg = Args.getLastArg(options::OPT_target)) {
      commandLine.push_back("-target");
      commandLine.push_back(targetArg->getValue());
    }
    if (const Arg *targetVariantArg =
            Args.getLastArg(options::OPT_target_variant)) {
      commandLine.push_back("-target-variant");
      commandLine.push_back(targetVariantArg->getValue());
    }
    if (const Arg *sdkArg = Args.getLastArg(options::OPT_sdk)) {
      commandLine.push_back("-sdk");
      commandLine.push_back(sdkArg->getValue());
    }

    if (const Arg *resourceDirArg = Args.getLastArg(options::OPT_resource_dir)) {
      commandLine.push_back("-resource-dir");
      commandLine.push_back(resourceDirArg->getValue());
    }

    if (Args.hasFlag(options::OPT_static_executable,
                     options::OPT_no_static_executable, false) ||
        Args.hasFlag(options::OPT_static_stdlib, options::OPT_no_static_stdlib,
                     false)) {
      commandLine.push_back("-use-static-resource-dir");
    }

    std::string executable = getSwiftProgramPath();

    // FIXME(https://github.com/apple/swift/issues/54554): This bypasses mechanisms like -v and -###.
    sys::TaskQueue queue;
    queue.addTask(executable.c_str(), commandLine);
    queue.execute(nullptr,
                  [](sys::ProcessId PID, int returnCode,
                      StringRef output, StringRef errors,
                      sys::TaskProcessInformation ProcInfo,
                      void *unused) -> sys::TaskFinishedResponse {
      llvm::outs() << output;
      llvm::errs() << errors;
      return sys::TaskFinishedResponse::ContinueExecution;
    });
    return false;
  }

  return true;
}

llvm::Optional<OutputFileMap>
Driver::buildOutputFileMap(const llvm::opt::DerivedArgList &Args,
                           StringRef workingDirectory) const {
  const Arg *A = Args.getLastArg(options::OPT_output_file_map);
  if (!A)
    return llvm::None;

  // TODO: perform some preflight checks to ensure the file exists.
  llvm::Expected<OutputFileMap> OFM = OutputFileMap::loadFromPath(
      A->getValue(), workingDirectory);
  if (auto Err = OFM.takeError()) {
    Diags.diagnose(SourceLoc(), diag::error_unable_to_load_output_file_map,
                   llvm::toString(std::move(Err)), A->getValue());
    return llvm::None;
  }
  return *OFM;
}

void Driver::buildJobs(ArrayRef<const Action *> TopLevelActions,
                       const OutputInfo &OI, const OutputFileMap *OFM,
                       StringRef workingDirectory, const ToolChain &TC,
                       Compilation &C) const {
  llvm::PrettyStackTraceString CrashInfo("Building compilation jobs");

  const DerivedArgList &Args = C.getArgs();
  JobCacheMap JobCache;

  if (Args.hasArg(options::OPT_o) && !OI.shouldLink() &&
      !OI.ShouldTreatModuleAsTopLevelOutput) {
    bool ShouldComplain;
    if (OI.isMultiThreading()) {
      // Multi-threading compilation has multiple outputs unless there's only
      // one input.
      ShouldComplain = C.getInputFiles().size() > 1;
    } else {
      // Single-threaded compilation is a problem if we're compiling more than
      // one file.
      ShouldComplain = 1 < llvm::count_if(C.getActions(), [](const Action *A) {
        return isa<CompileJobAction>(A);
      });
    }

    if (ShouldComplain) {
      Diags.diagnose(SourceLoc(),
                     diag::error_cannot_specify__o_for_multiple_outputs);
    }
  }

  for (const Action *A : TopLevelActions) {
    if (auto *JA = dyn_cast<JobAction>(A)) {
      (void)buildJobsForAction(C, JA, OFM, workingDirectory, /*TopLevel=*/true,
                               JobCache);
    }
  }
}

/// Form a filename based on \p base in \p result, optionally setting its
/// extension to \p newExt and in \p workingDirectory.
static void formFilenameFromBaseAndExt(StringRef base, StringRef newExt,
                                       StringRef workingDirectory,
                                       SmallVectorImpl<char> &result) {
  if (workingDirectory.empty() || llvm::sys::path::is_absolute(base)) {
    result.assign(base.begin(), base.end());
  } else {
    assert(!base.empty() && base != "-" && "unexpected basename");
    result.assign(workingDirectory.begin(), workingDirectory.end());
    llvm::sys::path::append(result, base);
  }

  if (!newExt.empty()) {
    llvm::sys::path::replace_extension(result, newExt);
  }
}

static llvm::Optional<StringRef> getOutputFilenameFromPathArgOrAsTopLevel(
    const OutputInfo &OI, const llvm::opt::DerivedArgList &Args,
    llvm::opt::OptSpecifier PathArg, file_types::ID ExpectedOutputType,
    bool TreatAsTopLevelOutput, StringRef workingDirectory,
    llvm::SmallString<128> &Buffer) {
  if (const Arg *A = Args.getLastArg(PathArg))
    return StringRef(A->getValue());

  if (TreatAsTopLevelOutput) {
    if (const Arg *A = Args.getLastArg(options::OPT_o)) {
      if (OI.CompilerOutputType == ExpectedOutputType)
        return StringRef(A->getValue());

      // Otherwise, put the file next to the top-level output.
      Buffer = A->getValue();
      llvm::sys::path::remove_filename(Buffer);
      llvm::sys::path::append(Buffer, OI.ModuleName);
      llvm::sys::path::replace_extension(
          Buffer, file_types::getExtension(ExpectedOutputType));
      return Buffer.str();
    }

    // A top-level output wasn't specified, so just output to
    // <ModuleName>.<ext>.
    formFilenameFromBaseAndExt(OI.ModuleName,
                               file_types::getExtension(ExpectedOutputType),
                               workingDirectory,
                               Buffer);
    return Buffer.str();
  }

  return llvm::None;
}

static StringRef assignOutputName(Compilation &C, const JobAction *JA,
                                  llvm::SmallString<128> &Buffer,
                                  StringRef BaseName,
                                  PreserveOnSignal ShouldPreserveOnSignal) {
  // We should output to a temporary file, since we're not at the top level
  // (or are generating a bridging PCH, which is currently always a temp).
  StringRef Stem = llvm::sys::path::stem(BaseName);
  StringRef Suffix = file_types::getExtension(JA->getType());
  std::error_code EC = llvm::sys::fs::createTemporaryFile(Stem, Suffix, Buffer);
  if (EC) {
    C.getDiags().diagnose(SourceLoc(),
                          diag::error_unable_to_make_temporary_file,
                          EC.message());
    return {};
  }
  C.addTemporaryFile(Buffer.str(), ShouldPreserveOnSignal);

  return Buffer.str();
}

static StringRef baseNameForImage(const JobAction *JA, const OutputInfo &OI,
                                  const llvm::Triple &Triple,
                                  llvm::SmallString<128> &Buffer,
                                  StringRef BaseInput, StringRef BaseName) {
  if (JA->size() == 1 && OI.ModuleNameIsFallback && BaseInput != "-")
    return llvm::sys::path::stem(BaseInput);
  
  if (isa<StaticLinkJobAction>(JA)) {
    Buffer = "lib";
    Buffer.append(BaseName);
    Buffer.append(Triple.isOSWindows() ? ".lib" : ".a");
    return Buffer.str();
  }
  
  auto link = dyn_cast<DynamicLinkJobAction>(JA);
  if (!link)
    return BaseName;
  if (link->getKind() != LinkKind::DynamicLibrary)
    return BaseName;

  Buffer = Triple.isOSWindows() ? "" : "lib";
  Buffer.append(BaseName);

  if (Triple.isOSDarwin())
    Buffer.append(".dylib");
  else if (Triple.isOSWindows())
    Buffer.append(".dll");
  else
    Buffer.append(".so");
  return Buffer.str();
}

static StringRef getIndexUnitOutputFilename(Compilation &C,
                                            const JobAction *JA,
                                            const TypeToPathMap *OutputMap,
                                            bool AtTopLevel) {
  if (JA->getType() == file_types::TY_Nothing)
    return {};

  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_IndexUnitOutputPath);
    if (iter != OutputMap->end())
      return iter->second;
  }

  if (AtTopLevel) {
    const llvm::opt::DerivedArgList &Args = C.getArgs();
    if (Arg *FinalOutput = Args.getLastArg(options::OPT_index_unit_output_path))
      return FinalOutput->getValue();
  }

  return {};
}

static StringRef getOutputFilename(Compilation &C,
                                   const JobAction *JA,
                                   const TypeToPathMap *OutputMap,
                                   StringRef workingDirectory,
                                   bool AtTopLevel,
                                   StringRef BaseInput,
                                   StringRef PrimaryInput,
                                   llvm::SmallString<128> &Buffer) {
  if (JA->getType() == file_types::TY_Nothing)
    return {};

  // If available, check the OutputMap first.
  if (OutputMap) {
    auto iter = OutputMap->find(JA->getType());
    if (iter != OutputMap->end())
      return iter->second;
  }

  // Process Action-specific output-specifying options next,
  // since we didn't find anything applicable in the OutputMap.
  const OutputInfo &OI = C.getOutputInfo();
  const llvm::opt::DerivedArgList &Args = C.getArgs();

  if (isa<MergeModuleJobAction>(JA)) {
    auto optFilename = getOutputFilenameFromPathArgOrAsTopLevel(
        OI, Args, options::OPT_emit_module_path, file_types::TY_SwiftModuleFile,
        OI.ShouldTreatModuleAsTopLevelOutput, workingDirectory, Buffer);
    if (optFilename)
      return *optFilename;
  }

  // dSYM actions are never treated as top-level.
  if (isa<GenerateDSYMJobAction>(JA)) {
    Buffer = PrimaryInput;
    Buffer.push_back('.');
    Buffer.append(file_types::getExtension(JA->getType()));
    return Buffer.str();
  }

  auto ShouldPreserveOnSignal = PreserveOnSignal::No;

  if (auto *PCHAct = dyn_cast<GeneratePCHJobAction>(JA)) {
    // For a persistent PCH we don't use an output, the frontend determines
    // the filename to use for the PCH.
    assert(!PCHAct->isPersistentPCH());
    ShouldPreserveOnSignal = PreserveOnSignal::Yes;
  }

  // We don't have an output from an Action-specific command line option,
  // so figure one out using the defaults.
  if (AtTopLevel) {
    if (Arg *FinalOutput = Args.getLastArg(options::OPT_o))
      return FinalOutput->getValue();
    if (file_types::isTextual(JA->getType()))
      return "-";
  }

  assert(!BaseInput.empty() &&
         "A Job which produces output must have a BaseInput!");
  StringRef BaseName(BaseInput);
  if (isa<MergeModuleJobAction>(JA) ||
      (OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
       !OI.isMultiThreading()) ||
      JA->getType() == file_types::TY_Image)
    BaseName = OI.ModuleName;

  // We don't yet have a name, assign one.
  if (!AtTopLevel)
    return assignOutputName(C, JA, Buffer, BaseName, ShouldPreserveOnSignal);

  if (JA->getType() == file_types::TY_Image) {
    const llvm::Triple &Triple = C.getToolChain().getTriple();
    SmallString<16> Base =
        baseNameForImage(JA, OI, Triple, Buffer, BaseInput, BaseName);
    formFilenameFromBaseAndExt(Base, /*newExt=*/"", workingDirectory, Buffer);
    return Buffer.str();
  }

  StringRef Suffix = file_types::getExtension(JA->getType());
  assert(Suffix.data() &&
         "All types used for output should have a suffix.");

  formFilenameFromBaseAndExt(llvm::sys::path::filename(BaseName), Suffix,
                             workingDirectory, Buffer);
  return Buffer.str();
}

static bool hasExistingAdditionalOutput(CommandOutput &output,
                                        file_types::ID outputType,
                                        StringRef outputPath = StringRef()) {

  auto existing = output.getAdditionalOutputForType(outputType);
  if (!existing.empty()) {
    assert(outputPath.empty() || outputPath == existing);
    return true;
  }
  return false;
}

static llvm::SmallString<128> computeAuxiliaryOutputPath(
    Compilation &C, CommandOutput &output, file_types::ID outputType,
    const TypeToPathMap *outputMap, StringRef workingDirectory,
    StringRef outputPath = StringRef(),
    llvm::opt::OptSpecifier requireArg = llvm::opt::OptSpecifier()) {

  if (hasExistingAdditionalOutput(output, outputType, outputPath))
    return {};

  if (outputMap) {
    auto iter = outputMap->find(outputType);
    if (iter != outputMap->end()) {
      StringRef outputMapPath = iter->second;
      // Prefer a path from the OutputMap.
      if (!outputMapPath.empty())
        return outputMapPath;
    }
  }
  if (!outputPath.empty())
    return outputPath;

  if (requireArg.isValid() && !C.getArgs().getLastArg(requireArg)) {
    // This auxiliary output only exists if requireArg is passed, but it
    // wasn't this time.
    return {};
  }

  // Put the auxiliary output file next to "the" primary output file.
  //
  // FIXME: when we're in WMO and have multiple primary outputs, we derive the
  // additional filename here from the _first_ primary output name, which
  // means that in the derived OFM (in Job.cpp) the additional output will
  // have a possibly-surprising name. But that's only half the problem: it
  // also get associated with the first primary _input_, even when there are
  // multiple primary inputs; really it should be associated with the build as
  // a whole -- derived OFM input "" -- but that's a more general thing to
  // fix.
  llvm::SmallString<128> path;
  if (output.getPrimaryOutputType() != file_types::TY_Nothing)
    path = output.getPrimaryOutputFilenames()[0];
  else if (!output.getBaseInput(0).empty())
    path = llvm::sys::path::filename(output.getBaseInput(0));
  else {
    formFilenameFromBaseAndExt(C.getOutputInfo().ModuleName, /*newExt=*/"",
                               workingDirectory, path);
  }
  assert(!path.empty());

  const bool isTempFile = C.isTemporaryFile(path);
  llvm::sys::path::replace_extension(path,
                                     file_types::getExtension(outputType));
  if (isTempFile)
    C.addTemporaryFile(path);
  return path;
}

static void addAuxiliaryOutput(
    Compilation &C, CommandOutput &output, file_types::ID outputType,
    const TypeToPathMap *outputMap, StringRef workingDirectory,
    StringRef outputPath = StringRef(),
    llvm::opt::OptSpecifier requireArg = llvm::opt::OptSpecifier()) {

  const auto path =
      computeAuxiliaryOutputPath(C, output, outputType, outputMap,
                                 workingDirectory, outputPath, requireArg);
  if (path.empty())
    return;
  output.setAdditionalOutputForType(outputType, path);
}

static void addDiagFileOutputForPersistentPCHAction(
    Compilation &C, const GeneratePCHJobAction *JA, CommandOutput &output,
    const TypeToPathMap *outputMap, StringRef workingDirectory) {
  assert(JA->isPersistentPCH());

  // For a persistent PCH we don't use an output, the frontend determines
  // the filename to use for the PCH. For the diagnostics file, try to
  // determine an invocation-specific path inside the directory where the
  // PCH is going to be written, and fallback to a temporary file if we
  // cannot determine such a path.

  StringRef pchOutDir = JA->getPersistentPCHDir();
  StringRef headerPath = output.getBaseInput(JA->getInputIndex());
  StringRef stem = llvm::sys::path::stem(headerPath);
  StringRef suffix =
      file_types::getExtension(file_types::TY_SerializedDiagnostics);
  SmallString<256> outPathBuf;

  if (const Arg *A = C.getArgs().getLastArg(options::OPT_emit_module_path)) {
    // The module file path is unique for a specific module and architecture
    // (it won't be concurrently written to) so we can use the path as hash
    // for determining the filename to use for the diagnostic file.
    StringRef ModuleOutPath = A->getValue();
    outPathBuf = pchOutDir;
    llvm::sys::path::append(outPathBuf, stem);
    outPathBuf += '-';
    auto code = llvm::hash_value(ModuleOutPath);
    llvm::APInt(64, code).toString(outPathBuf, 36, /*Signed=*/false);
    llvm::sys::path::replace_extension(outPathBuf, suffix);
  }

  if (outPathBuf.empty()) {
    // Fallback to creating a temporary file.
    std::error_code EC =
    llvm::sys::fs::createTemporaryFile(stem, suffix, outPathBuf);
    if (EC) {
      C.getDiags().diagnose(SourceLoc(),
                            diag::error_unable_to_make_temporary_file,
                            EC.message());
      return;
    }
    C.addTemporaryFile(outPathBuf.str());
  }

  if (!outPathBuf.empty()) {
    addAuxiliaryOutput(C, output, file_types::TY_SerializedDiagnostics,
                       outputMap, workingDirectory, outPathBuf.str());
  }
}

/// If the file at \p input has not been modified since the last build (i.e. its
/// mtime has not changed), adjust the Job's condition accordingly.
static void handleCompileJobCondition(Job *J,
                                      CompileJobAction::InputInfo inputInfo,
                                      llvm::Optional<StringRef> input,
                                      bool alwaysRebuildDependents) {
  using InputStatus = CompileJobAction::InputInfo::Status;

  if (inputInfo.status == InputStatus::NewlyAdded) {
    J->setCondition(Job::Condition::NewlyAdded);
    return;
  }

  auto output = J->getOutput().getPrimaryOutputFilename();
  bool hasValidModTime = false;
  llvm::sys::fs::file_status inputStatus;
  if (input.has_value() && !llvm::sys::fs::status(*input, inputStatus)) {
    J->setInputModTime(inputStatus.getLastModificationTime());
    hasValidModTime = J->getInputModTime() == inputInfo.previousModTime;
  } else if (!llvm::sys::fs::status(output, inputStatus)) {
    J->setInputModTime(inputStatus.getLastModificationTime());
    hasValidModTime = true;
  }

  Job::Condition condition;
  if (hasValidModTime) {
    switch (inputInfo.status) {
    case InputStatus::UpToDate:
      if (llvm::sys::fs::exists(output))
        condition = Job::Condition::CheckDependencies;
      else
        condition = Job::Condition::RunWithoutCascading;
      break;
    case InputStatus::NeedsCascadingBuild:
      condition = Job::Condition::Always;
      break;
    case InputStatus::NeedsNonCascadingBuild:
      condition = Job::Condition::RunWithoutCascading;
      break;
    case InputStatus::NewlyAdded:
      llvm_unreachable("handled above");
    }
  } else {
    if (alwaysRebuildDependents ||
        inputInfo.status == InputStatus::NeedsCascadingBuild) {
      condition = Job::Condition::Always;
    } else {
      condition = Job::Condition::RunWithoutCascading;
    }
  }

  J->setCondition(condition);
}

Job *Driver::buildJobsForAction(Compilation &C, const JobAction *JA,
                                const OutputFileMap *OFM,
                                StringRef workingDirectory,
                                bool AtTopLevel, JobCacheMap &JobCache) const {

  PrettyStackTraceDriverAction CrashInfo("building jobs", JA);

  // 1. See if we've already got this cached.
  const ToolChain &TC = C.getToolChain();
  std::pair<const Action *, const ToolChain *> Key(JA, &TC);
  {
    auto CacheIter = JobCache.find(Key);
    if (CacheIter != JobCache.end()) {
      return CacheIter->second;
    }
  }

  // 2. Build up the list of input jobs.
  SmallVector<const Action *, 4> InputActions;
  SmallVector<const Job *, 4> InputJobs;
  for (const Action *Input : *JA) {
    if (auto *InputJobAction = dyn_cast<JobAction>(Input)) {
      InputJobs.push_back(buildJobsForAction(
          C, InputJobAction, OFM, workingDirectory, false, JobCache));
    } else {
      InputActions.push_back(Input);
    }
  }

  // 3. Determine the CommandOutput for the job.
  StringRef BaseInput;
  StringRef PrimaryInput;
  if (!InputActions.empty()) {
    // Use the first InputAction as our BaseInput and PrimaryInput.
    const InputAction *IA = cast<InputAction>(InputActions[0]);
    BaseInput = IA->getInputArg().getValue();
    PrimaryInput = BaseInput;
  } else if (!InputJobs.empty()) {
    const CommandOutput &Out = InputJobs.front()->getOutput();
    size_t i = JA->getInputIndex();
    // Use the first Job's BaseInput as our BaseInput.
    BaseInput = Out.getBaseInput(i);
    // Use the first Job's Primary Output as our PrimaryInput.
    PrimaryInput = Out.getPrimaryOutputFilenames()[i];
  }

  // With -index-file option, the primary input is the one passed with
  // -index-file-path.
  // FIXME: Figure out how this better fits within the driver infrastructure.
  if (JA->getType() == file_types::TY_IndexData) {
    if (Arg *A = C.getArgs().getLastArg(options::OPT_index_file_path)) {
      BaseInput = A->getValue();
      PrimaryInput = A->getValue();
    }
  }

  const OutputInfo &OI = C.getOutputInfo();
  const TypeToPathMap *OutputMap = nullptr;
  if (OFM) {
    if (isa<CompileJobAction>(JA)) {
      if (OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
        OutputMap = OFM->getOutputMapForSingleOutput();
      } else {
        OutputMap = OFM->getOutputMapForInput(BaseInput);
      }
    } else if (isa<BackendJobAction>(JA)) {
      OutputMap = OFM->getOutputMapForInput(BaseInput);
    }
  }

  std::unique_ptr<CommandOutput> Output(
      new CommandOutput(JA->getType(), C.getDerivedOutputFileMap()));

  PrettyStackTraceDriverCommandOutput CrashInfo2("determining output",
                                                 Output.get());
  llvm::SmallString<128> Buf;
  computeMainOutput(C, JA, OFM, AtTopLevel, InputActions, InputJobs, OutputMap,
                    workingDirectory, BaseInput, PrimaryInput, Buf,
                    Output.get());

  if (OI.ShouldGenerateModule && isa<CompileJobAction>(JA))
    chooseSwiftModuleOutputPath(C, OutputMap, workingDirectory, Output.get());

  if (OI.ShouldGenerateModule &&
      (isa<CompileJobAction>(JA) || isa<MergeModuleJobAction>(JA))) {
    chooseSwiftModuleDocOutputPath(C, OutputMap, workingDirectory, Output.get());
    if (!C.getArgs().hasArg(options::OPT_avoid_emit_module_source_info)) {
      chooseSwiftSourceInfoOutputPath(C, OutputMap, workingDirectory,
                                      Output.get());
    }
  }

  if (C.getArgs().hasArg(options::OPT_emit_module_interface,
                         options::OPT_emit_module_interface_path))
    chooseModuleInterfacePath(C, JA, workingDirectory, Buf,
      file_types::TY_SwiftModuleInterfaceFile, Output.get());

  if (C.getArgs().hasArg(options::OPT_emit_private_module_interface_path))
    chooseModuleInterfacePath(C, JA, workingDirectory, Buf,
      file_types::TY_PrivateSwiftModuleInterfaceFile, Output.get());

  if (C.getArgs().hasArg(options::OPT_update_code) && isa<CompileJobAction>(JA))
    chooseRemappingOutputPath(C, OutputMap, Output.get());

  if (isa<CompileJobAction>(JA) || isa<GeneratePCHJobAction>(JA)) {
    // Choose the serialized diagnostics output path.
    if (C.getArgs().hasArg(options::OPT_serialize_diagnostics))
      chooseSerializedDiagnosticsPath(C, JA, OutputMap, workingDirectory,
                                      Output.get());
  }

  if (isa<CompileJobAction>(JA)) {
    chooseModuleSummaryPath(C, OutputMap, workingDirectory, Buf, Output.get());
  }

  if (isa<MergeModuleJobAction>(JA) ||
      (isa<CompileJobAction>(JA) &&
       OI.CompilerMode == OutputInfo::Mode::SingleCompile)) {
    // An emit-tbd argument gets passed down to a job that sees the whole
    // module, either the -merge-modules job or a -wmo compiler invocation.
    chooseTBDPath(C, OutputMap, workingDirectory, Buf, Output.get());
  }

  if (isa<CompileJobAction>(JA))
    chooseDependenciesOutputPaths(C, OutputMap, workingDirectory, Buf,
                                  Output.get());

  if (C.getArgs().hasArg(options::OPT_save_optimization_record,
                         options::OPT_save_optimization_record_EQ,
                         options::OPT_save_optimization_record_path))
    chooseOptimizationRecordPath(C, workingDirectory, Buf, Output.get());

  if ((isa<MergeModuleJobAction>(JA) ||
       (isa<CompileJobAction>(JA) &&
        OI.CompilerMode == OutputInfo::Mode::SingleCompile)) &&
      C.getArgs().hasArg(options::OPT_emit_objc_header,
                         options::OPT_emit_objc_header_path))
    chooseObjectiveCHeaderOutputPath(C, OutputMap, workingDirectory,
                                     Output.get());

  // 4. Construct a Job which produces the right CommandOutput.
  std::unique_ptr<Job> ownedJob = TC.constructJob(*JA, C, std::move(InputJobs),
                                                  InputActions, 
                                                  std::move(Output), OI);
  Job *J = C.addJob(std::move(ownedJob));

  // If we track dependencies for this job, we may be able to avoid running it.
  if (auto incrementalJob = dyn_cast<IncrementalJobAction>(JA)) {
    const bool alwaysRebuildDependents =
        C.getArgs().hasArg(options::OPT_driver_always_rebuild_dependents);
    if (!J->getOutput()
             .getAdditionalOutputForType(file_types::TY_SwiftDeps)
             .empty()) {
      if (InputActions.size() == 1) {
        handleCompileJobCondition(J, incrementalJob->getInputInfo(), BaseInput,
                                  alwaysRebuildDependents);
      }
    } else if (isa<MergeModuleJobAction>(JA)) {
      handleCompileJobCondition(J, incrementalJob->getInputInfo(), llvm::None,
                                alwaysRebuildDependents);
    }
  }

  // 5. Add it to the JobCache, so we don't construct the same Job multiple
  // times.
  JobCache[Key] = J;

  if (DriverPrintBindings) {
    llvm::outs() << "# \"" << TC.getTriple().str()
      << "\" - \"" << llvm::sys::path::filename(J->getExecutable())
      << "\", inputs: [";

    llvm::interleave(
        InputActions.begin(), InputActions.end(),
        [](const Action *A) {
          auto Input = cast<InputAction>(A);
          llvm::outs() << '"' << Input->getInputArg().getValue() << '"';
        },
        [] { llvm::outs() << ", "; });
    if (!InputActions.empty() && !J->getInputs().empty())
      llvm::outs() << ", ";
    llvm::interleave(
        J->getInputs().begin(), J->getInputs().end(),
        [](const Job *Input) {
          auto FileNames = Input->getOutput().getPrimaryOutputFilenames();
          interleave(
              FileNames.begin(), FileNames.end(),
              [](StringRef FileName) {
                llvm::outs() << '"' << FileName << '"';
              },
              [] { llvm::outs() << ", "; });
        },
        [] { llvm::outs() << ", "; });

    llvm::outs() << "], output: {";
    auto OutputFileNames = J->getOutput().getPrimaryOutputFilenames();
    StringRef TypeName =
        file_types::getTypeName(J->getOutput().getPrimaryOutputType());
    interleave(
        OutputFileNames.begin(), OutputFileNames.end(),
        [TypeName](StringRef FileName) {
          llvm::outs() << TypeName << ": \"" << FileName << '"';
        },
        [] { llvm::outs() << ", "; });

    file_types::forAllTypes([&J](file_types::ID Ty) {
      StringRef AdditionalOutput =
        J->getOutput().getAdditionalOutputForType(Ty);
      if (!AdditionalOutput.empty()) {
        llvm::outs() << ", " << file_types::getTypeName(Ty) << ": \""
                     << AdditionalOutput << '"';
      }
    });
    llvm::outs() << '}';

    switch (J->getCondition()) {
    case Job::Condition::Always:
      break;
    case Job::Condition::RunWithoutCascading:
      llvm::outs() << ", condition: run-without-cascading";
      break;
    case Job::Condition::CheckDependencies:
      llvm::outs() << ", condition: check-dependencies";
      break;
    case Job::Condition::NewlyAdded:
      llvm::outs() << ", condition: newly-added";
      break;
    }

    llvm::outs() << '\n';
  }

  return J;
}

void Driver::computeMainOutput(
    Compilation &C, const JobAction *JA, const OutputFileMap *OFM,
    bool AtTopLevel, SmallVectorImpl<const Action *> &InputActions,
    SmallVectorImpl<const Job *> &InputJobs, const TypeToPathMap *OutputMap,
    StringRef workingDirectory, StringRef BaseInput, StringRef PrimaryInput,
    llvm::SmallString<128> &Buf, CommandOutput *Output) const {
  StringRef OutputFile, IndexUnitOutputFile;
  if (C.getOutputInfo().isMultiThreading() && isa<CompileJobAction>(JA) &&
      file_types::isAfterLLVM(JA->getType())) {
    // Multi-threaded compilation: A single frontend command produces multiple
    // output file: one for each input files.

    // In batch mode, the driver will try to reserve multiple differing main
    // outputs to a bridging header. Another assertion will trip, but the cause
    // will be harder to track down. Since the driver now ignores -num-threads
    // in batch mode, the user should never be able to falsify this assertion.
    assert(!C.getBatchModeEnabled() && "Batch mode produces only one main "
                                       "output per input action, cannot have "
                                       "batch mode & num-threads");

    auto OutputFunc = [&](StringRef Base, StringRef Primary) {
      const TypeToPathMap *OMForInput = nullptr;
      if (OFM)
        OMForInput = OFM->getOutputMapForInput(Base);

      OutputFile = getOutputFilename(C, JA, OMForInput, workingDirectory,
                                     AtTopLevel, Base, Primary, Buf);
      IndexUnitOutputFile = getIndexUnitOutputFilename(C, JA, OMForInput,
                                                       AtTopLevel);
      Output->addPrimaryOutput(CommandInputPair(Base, Primary),
                               OutputFile, IndexUnitOutputFile);
    };
    // Add an output file for each input action.
    for (const Action *A : InputActions) {
      const InputAction *IA = cast<InputAction>(A);
      StringRef IV = IA->getInputArg().getValue();
      OutputFunc(IV, IV);
    }
    // Add an output file for each primary output of each input job.
    for (const Job *IJ : InputJobs) {
      size_t i = 0;
      CommandOutput const &Out = IJ->getOutput();
      for (auto OutPrimary : Out.getPrimaryOutputFilenames()) {
        OutputFunc(Out.getBaseInput(i++), OutPrimary);
      }
    }
  } else {
    // The common case: there is a single output file.
    OutputFile = getOutputFilename(C, JA, OutputMap, workingDirectory,
                                   AtTopLevel, BaseInput, PrimaryInput, Buf);
    IndexUnitOutputFile = getIndexUnitOutputFilename(C, JA, OutputMap,
                                                     AtTopLevel);
    Output->addPrimaryOutput(CommandInputPair(BaseInput, PrimaryInput),
                             OutputFile, IndexUnitOutputFile);
  }
}

void Driver::chooseSwiftModuleOutputPath(Compilation &C,
                                         const TypeToPathMap *OutputMap,
                                         StringRef workingDirectory,
                                         CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_SwiftModuleFile))
    return;

  StringRef OFMModuleOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_SwiftModuleFile);
    if (iter != OutputMap->end())
      OFMModuleOutputPath = iter->second;
  }

  const OutputInfo &OI = C.getOutputInfo();
  const Arg *A = C.getArgs().getLastArg(options::OPT_emit_module_path);
  using file_types::TY_SwiftModuleFile;

  if (!OFMModuleOutputPath.empty()) {
    // Prefer a path from the OutputMap.
    Output->setAdditionalOutputForType(TY_SwiftModuleFile, OFMModuleOutputPath);
  } else if (A && OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
    // We're performing a single compilation (and thus no merge module step),
    // so prefer to use -emit-module-path, if present.
    Output->setAdditionalOutputForType(TY_SwiftModuleFile, A->getValue());
  } else if (Output->getPrimaryOutputType() == TY_SwiftModuleFile) {
    // If the primary type is already a module type, we're out of
    // options for overriding the primary name choice: stop now.
    assert(!Output->getPrimaryOutputFilename().empty());
    return;
  } else if (OI.CompilerMode == OutputInfo::Mode::SingleCompile &&
             OI.ShouldTreatModuleAsTopLevelOutput) {
    // We're performing a single compile and don't have -emit-module-path,
    // but have been told to treat the module as a top-level output.
    // Determine an appropriate path.
    llvm::SmallString<128> Path;
    if (const Arg *A = C.getArgs().getLastArg(options::OPT_o)) {
      // Put the module next to the top-level output.
      Path = A->getValue();
      llvm::sys::path::remove_filename(Path);
    } else {
      // A top-level output wasn't specified, so just output to
      // <ModuleName>.swiftmodule in the current directory.
    }
    llvm::sys::path::append(Path, OI.ModuleName);
    llvm::sys::path::replace_extension(
        Path, file_types::getExtension(TY_SwiftModuleFile));
    Output->setAdditionalOutputForType(TY_SwiftModuleFile, Path);
  } else if (Output->getPrimaryOutputType() != file_types::TY_Nothing) {
    // We're only generating the module as an intermediate, so put it next
    // to the primary output of the compile command.
    llvm::SmallString<128> Path(Output->getPrimaryOutputFilenames()[0]);
    assert(!Path.empty());
    bool isTempFile = C.isTemporaryFile(Path);
    llvm::sys::path::replace_extension(
        Path, file_types::getExtension(TY_SwiftModuleFile));
    Output->setAdditionalOutputForType(TY_SwiftModuleFile, Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

static void chooseModuleAuxiliaryOutputFilePath(
    Compilation &C, const TypeToPathMap *OutputMap, StringRef workingDirectory,
    CommandOutput *Output, file_types::ID fileID,
    bool shouldUseProjectFolder = false,
    llvm::Optional<options::ID> optId = llvm::None) {
  if (hasExistingAdditionalOutput(*Output, fileID))
    return;
  // Honor driver option for this path if it's given
  if (optId.has_value()) {
    if (const Arg *A = C.getArgs().getLastArg(*optId)) {
      Output->setAdditionalOutputForType(fileID, StringRef(A->getValue()));
      return;
    }
  }

  StringRef OFMOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(fileID);
    if (iter != OutputMap->end())
      OFMOutputPath = iter->second;
  }
  if (!OFMOutputPath.empty()) {
    // Prefer a path from the OutputMap.
    Output->setAdditionalOutputForType(fileID, OFMOutputPath);
  } else if (Output->getPrimaryOutputType() != file_types::TY_Nothing) {
    auto ModulePath = Output->getAnyOutputForType(file_types::TY_SwiftModuleFile);
    bool isTempFile = C.isTemporaryFile(ModulePath);
    auto ModuleName = llvm::sys::path::filename(ModulePath);
    llvm::SmallString<128> Path(llvm::sys::path::parent_path(ModulePath));
    if (shouldUseProjectFolder) {
      llvm::sys::path::append(Path, "Project");
      // If the build system has created a Project dir for us to include the file, use it.
      if (!llvm::sys::fs::exists(Path)) {
        llvm::sys::path::remove_filename(Path);
      }
    }
    llvm::sys::path::append(Path, ModuleName);
    llvm::sys::path::replace_extension(Path, file_types::getExtension(fileID));
    Output->setAdditionalOutputForType(fileID, Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

void Driver::chooseSwiftSourceInfoOutputPath(Compilation &C,
                                             const TypeToPathMap *OutputMap,
                                             StringRef workingDirectory,
                                             CommandOutput *Output) const {
  chooseModuleAuxiliaryOutputFilePath(C, OutputMap, workingDirectory, Output,
                                      file_types::TY_SwiftSourceInfoFile,
                                      /*shouldUseProjectFolder*/true,
                                      options::OPT_emit_module_source_info_path);
}

void Driver::chooseSwiftModuleDocOutputPath(Compilation &C,
                                            const TypeToPathMap *OutputMap,
                                            StringRef workingDirectory,
                                            CommandOutput *Output) const {
  chooseModuleAuxiliaryOutputFilePath(C, OutputMap, workingDirectory, Output,
                                      file_types::TY_SwiftModuleDocFile);
}

void Driver::chooseRemappingOutputPath(Compilation &C,
                                       const TypeToPathMap *OutputMap,
                                       CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_Remapping))
    return;

  StringRef OFMFixitsOutputPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_Remapping);
    if (iter != OutputMap->end())
      OFMFixitsOutputPath = iter->second;
  }
  if (!OFMFixitsOutputPath.empty()) {
    Output->setAdditionalOutputForType(file_types::ID::TY_Remapping,
                                       OFMFixitsOutputPath);
  } else {
    llvm::SmallString<128> Path(Output->getPrimaryOutputFilenames()[0]);
    bool isTempFile = C.isTemporaryFile(Path);
    llvm::sys::path::replace_extension(Path,
        file_types::getExtension(file_types::ID::TY_Remapping));
    Output->setAdditionalOutputForType(file_types::ID::TY_Remapping, Path);
    if (isTempFile)
      C.addTemporaryFile(Path);
  }
}

void Driver::chooseModuleInterfacePath(Compilation &C, const JobAction *JA,
                                       StringRef workingDirectory,
                                       llvm::SmallString<128> &buffer,
                                       file_types::ID fileType,
                                       CommandOutput *output) const {
  switch (C.getOutputInfo().CompilerMode) {
  case OutputInfo::Mode::StandardCompile:
  case OutputInfo::Mode::BatchModeCompile:
    if (!isa<MergeModuleJobAction>(JA))
      return;
    break;
  case OutputInfo::Mode::SingleCompile:
    if (!isa<CompileJobAction>(JA))
      return;
    break;
  case OutputInfo::Mode::Immediate:
  case OutputInfo::Mode::REPL:
    llvm_unreachable("these modes aren't usable with 'swiftc'");
  }

  auto pathOpt = fileType == file_types::TY_SwiftModuleInterfaceFile?
    options::OPT_emit_module_interface_path:
    options::OPT_emit_private_module_interface_path;

  StringRef outputPath = *getOutputFilenameFromPathArgOrAsTopLevel(
      C.getOutputInfo(), C.getArgs(), pathOpt, fileType,
      /*TreatAsTopLevelOutput*/true, workingDirectory, buffer);
  output->setAdditionalOutputForType(fileType, outputPath);
}

void Driver::chooseModuleSummaryPath(Compilation &C,
                                     const TypeToPathMap *OutputMap,
                                     StringRef workingDirectory,
                                     llvm::SmallString<128> &Buf,
                                     CommandOutput *Output) const {
  StringRef pathFromArgs;
  if (const Arg *A =
          C.getArgs().getLastArg(options::OPT_emit_module_summary_path)) {
    pathFromArgs = A->getValue();
  }

  addAuxiliaryOutput(C, *Output, file_types::TY_SwiftModuleSummaryFile,
                     OutputMap, workingDirectory, pathFromArgs,
                     /*requireArg=*/options::OPT_emit_module_summary);
}

void Driver::chooseSerializedDiagnosticsPath(Compilation &C,
                                             const JobAction *JA,
                                             const TypeToPathMap *OutputMap,
                                             StringRef workingDirectory,
                                             CommandOutput *Output) const {
  if (C.getArgs().hasArg(options::OPT_serialize_diagnostics)) {
    auto pchJA = dyn_cast<GeneratePCHJobAction>(JA);
    if (pchJA && pchJA->isPersistentPCH()) {
      addDiagFileOutputForPersistentPCHAction(C, pchJA, *Output, OutputMap,
                                              workingDirectory);
    } else {
      addAuxiliaryOutput(C, *Output, file_types::TY_SerializedDiagnostics,
                         OutputMap, workingDirectory);
    }

    // Remove any existing diagnostics files so that clients can detect their
    // presence to determine if a command was run.
    StringRef OutputPath =
        Output->getAnyOutputForType(file_types::TY_SerializedDiagnostics);
    if (llvm::sys::fs::is_regular_file(OutputPath))
      llvm::sys::fs::remove(OutputPath);
  }
}

void Driver::chooseDependenciesOutputPaths(Compilation &C,
                                           const TypeToPathMap *OutputMap,
                                           StringRef workingDirectory,
                                           llvm::SmallString<128> &Buf,
                                           CommandOutput *Output) const {
  if (C.getArgs().hasArg(options::OPT_emit_dependencies)) {
    auto depPath = computeAuxiliaryOutputPath(
        C, *Output, file_types::TY_Dependencies, OutputMap, workingDirectory);
    C.addDependencyPathOrCreateDummy(depPath, [&] {
      addAuxiliaryOutput(C, *Output, file_types::TY_Dependencies, OutputMap,
                         workingDirectory);
    });
  }
  if (C.getIncrementalBuildEnabled()) {
    file_types::forEachIncrementalOutputType([&](file_types::ID type) {
      if (type == file_types::TY_SwiftDeps)
        addAuxiliaryOutput(C, *Output, type, OutputMap, workingDirectory);
    });
  }
  chooseLoadedModuleTracePath(C, workingDirectory, Buf, Output);
}

void Driver::chooseLoadedModuleTracePath(Compilation &C,
                                         StringRef workingDirectory,
                                         llvm::SmallString<128> &Buf,
                                         CommandOutput *Output) const {
  // The loaded-module-trace is the same for all compile jobs: all `import`
  // statements are processed, even ones from non-primary files. Thus, only
  // one of those jobs needs to emit the file, and we can get it to write
  // straight to the desired final location.
  auto tracePathEnvVar = getenv("SWIFT_LOADED_MODULE_TRACE_FILE");
  auto shouldEmitTrace =
      tracePathEnvVar ||
      C.getArgs().hasArg(options::OPT_emit_loaded_module_trace,
                         options::OPT_emit_loaded_module_trace_path);

  if (shouldEmitTrace &&
      C.requestPermissionForFrontendToEmitLoadedModuleTrace()) {
    StringRef filename;
    // Prefer the environment variable.
    if (tracePathEnvVar)
      filename = StringRef(tracePathEnvVar);
    else {
      // By treating this as a top-level output, the return value always
      // exists.
      filename = *getOutputFilenameFromPathArgOrAsTopLevel(
          C.getOutputInfo(), C.getArgs(),
          options::OPT_emit_loaded_module_trace_path,
          file_types::TY_ModuleTrace,
          /*TreatAsTopLevelOutput=*/true, workingDirectory, Buf);
    }

    Output->setAdditionalOutputForType(file_types::TY_ModuleTrace, filename);
  }
}

void Driver::chooseTBDPath(Compilation &C, const TypeToPathMap *OutputMap,
                           StringRef workingDirectory,
                           llvm::SmallString<128> &Buf,
                           CommandOutput *Output) const {
  StringRef pathFromArgs;
  if (const Arg *A = C.getArgs().getLastArg(options::OPT_emit_tbd_path)) {
    pathFromArgs = A->getValue();
  }

  addAuxiliaryOutput(C, *Output, file_types::TY_TBD, OutputMap,
                     workingDirectory, pathFromArgs,
                     /*requireArg=*/options::OPT_emit_tbd);
}

void Driver::chooseOptimizationRecordPath(Compilation &C,
                                          StringRef workingDirectory,
                                          llvm::SmallString<128> &Buf,
                                          CommandOutput *Output) const {
  const OutputInfo &OI = C.getOutputInfo();
  if (OI.CompilerMode == OutputInfo::Mode::SingleCompile) {
    llvm::Expected<file_types::ID> FileType =
        C.getToolChain().remarkFileTypeFromArgs(C.getArgs());
    if (!FileType) {
      Diags.diagnose({}, diag::error_creating_remark_serializer,
                     llvm::toString(FileType.takeError()));
      return;
    }
    auto filename = *getOutputFilenameFromPathArgOrAsTopLevel(
        OI, C.getArgs(), options::OPT_save_optimization_record_path, *FileType,
        /*TreatAsTopLevelOutput=*/true, workingDirectory, Buf);

    Output->setAdditionalOutputForType(*FileType, filename);
  } else
    // FIXME: We should use the OutputMap in this case.
    Diags.diagnose({}, diag::warn_opt_remark_disabled);
}

void Driver::chooseObjectiveCHeaderOutputPath(Compilation &C,
                                              const TypeToPathMap *OutputMap,
                                              StringRef workingDirectory,
                                              CommandOutput *Output) const {

  if (hasExistingAdditionalOutput(*Output, file_types::TY_ClangHeader))
    return;

  StringRef ObjCHeaderPath;
  if (OutputMap) {
    auto iter = OutputMap->find(file_types::TY_ClangHeader);
    if (iter != OutputMap->end())
      ObjCHeaderPath = iter->second;
  }

  if (ObjCHeaderPath.empty())
    if (auto A = C.getArgs().getLastArg(options::OPT_emit_objc_header_path))
      ObjCHeaderPath = A->getValue();

  if (!ObjCHeaderPath.empty()) {
    Output->setAdditionalOutputForType(file_types::TY_ClangHeader,
                                       ObjCHeaderPath);
  } else {
    // Put the header next to the primary output file.
    // FIXME: That's not correct if the user /just/ passed -emit-header
    // and not -emit-module.
    addAuxiliaryOutput(C, *Output, file_types::TY_ClangHeader,
                       /*output file map*/ nullptr, workingDirectory);
  }
}

static unsigned printActions(const Action *A,
                             llvm::DenseMap<const Action *, unsigned> &Ids) {
  if (Ids.count(A))
    return Ids[A];

  std::string str;
  llvm::raw_string_ostream os(str);

  os << Action::getClassName(A->getKind()) << ", ";
  if (const auto *IA = dyn_cast<InputAction>(A)) {
    os << "\"" << IA->getInputArg().getValue() << "\"";
  } else {
    os << "{";
    llvm::interleave(
        *cast<JobAction>(A),
        [&](const Action *Input) { os << printActions(Input, Ids); },
        [&] { os << ", "; });
    os << "}";
  }

  unsigned Id = Ids.size();
  Ids[A] = Id;
  llvm::errs() << Id << ": " << os.str() << ", "
               << file_types::getTypeName(A->getType()) << "\n";

  return Id;
}

void Driver::printActions(const Compilation &C) const {
  llvm::DenseMap<const Action *, unsigned> Ids;
  for (const Action *A : C.getActions()) {
    ::printActions(A, Ids);
  }
}

void Driver::printVersion(const ToolChain &TC, raw_ostream &OS) const {
  OS << version::getSwiftFullVersion(
    version::Version::getCurrentLanguageVersion()) << '\n';
  OS << "Target: " << TC.getTriple().str() << '\n';
}

void Driver::printHelp(bool ShowHidden) const {
  unsigned IncludedFlagsBitmask = 0;
  unsigned ExcludedFlagsBitmask = options::NoDriverOption;

  switch (driverKind) {
  case DriverKind::Interactive:
    ExcludedFlagsBitmask |= options::NoInteractiveOption;
    break;
  case DriverKind::Batch:
  case DriverKind::SILOpt:
  case DriverKind::SILFuncExtractor:
  case DriverKind::SILNM:
  case DriverKind::SILLLVMGen:
  case DriverKind::SILPassPipelineDumper:
  case DriverKind::SwiftDependencyTool:
  case DriverKind::SwiftLLVMOpt:
  case DriverKind::AutolinkExtract:
  case DriverKind::SwiftIndent:
  case DriverKind::SymbolGraph:
  case DriverKind::APIExtract:
  case DriverKind::APIDigester:
  case DriverKind::CacheTool:
    ExcludedFlagsBitmask |= options::NoBatchOption;
    break;
  }

  if (!ShowHidden)
    ExcludedFlagsBitmask |= HelpHidden;

  getOpts().printHelp(llvm::outs(), Name.c_str(), "Swift compiler",
                      IncludedFlagsBitmask, ExcludedFlagsBitmask,
                      /*ShowAllAliases*/false);

  // These strings match the descriptions found in the corresponding swiftpm 
  // help pages
  if (driverKind == DriverKind::Interactive) {
    llvm::outs() << "\nSEE ALSO - PACKAGE MANAGER COMMANDS: \n"
        "\t\"swift build\" Build sources into binary products \n"
        "\t\"swift package\" Perform operations on Swift packages \n"
        "\t\"swift run\" Build and run an executable product \n"
        "\t\"swift test\" Build and run tests \n";
  } else {
    llvm::outs() << "\nSEE ALSO: swift build, swift run, swift package, " 
                    "swift test \n";
  }

}

bool OutputInfo::mightHaveExplicitPrimaryInputs(
    const CommandOutput &Output) const {
  switch (CompilerMode) {
  case Mode::StandardCompile:
  case Mode::BatchModeCompile:
    return true;
  case Mode::SingleCompile:
    return false;
  case Mode::Immediate:
  case Mode::REPL:
    llvm_unreachable("REPL and immediate modes handled elsewhere");
  }
  llvm_unreachable("unhandled mode");
}
