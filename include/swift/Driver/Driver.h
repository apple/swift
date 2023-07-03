//===--- Driver.h - Swift compiler driver -----------------------*- C++ -*-===//
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
// This file contains declarations of parts of the compiler driver.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DRIVER_DRIVER_H
#define SWIFT_DRIVER_DRIVER_H

#include "swift/AST/IRGenOptions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/OutputFileMap.h"
#include "swift/Basic/Sanitizers.h"
#include "swift/Driver/Util.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"

#include <functional>
#include <memory>
#include <string>

namespace llvm {
namespace opt {
  class Arg;
  class ArgList;
  class OptTable;
  class InputArgList;
  class DerivedArgList;
}
}

namespace swift {
  namespace sys {
    class TaskQueue;
  }
  class DiagnosticEngine;
namespace driver {
  class Action;
  class CommandOutput;
  class Compilation;
  class Job;
  class JobAction;
  class ToolChain;

/// A class encapsulating information about the outputs the driver
/// is expected to generate.
class OutputInfo {
public:
  enum class Mode {
    /// A standard compilation, using multiple frontend invocations and
    /// -primary-file.
    StandardCompile,

    /// A compilation using a single frontend invocation without -primary-file.
    SingleCompile,

    /// A single process that batches together multiple StandardCompile Jobs.
    ///
    /// Note: this is a transient value to use _only_ for the individual
    /// BatchJobs that are the temporary containers for multiple StandardCompile
    /// Jobs built by ToolChain::constructBatchJob.
    ///
    /// In particular, the driver treats a batch-mode-enabled Compilation as
    /// having OutputInfo::CompilerMode == StandardCompile, with the
    /// Compilation::BatchModeEnabled flag set to true, _not_ as a
    /// BatchModeCompile Compilation. The top-level OutputInfo::CompilerMode for
    /// a Compilation should never be BatchModeCompile.
    BatchModeCompile,

    /// Invoke the REPL
    REPL,

    /// Compile and execute the inputs immediately
    Immediate,
  };

  enum class MSVCRuntime {
    MultiThreaded,
    MultiThreadedDebug,
    MultiThreadedDLL,
    MultiThreadedDebugDLL,
  };

  /// The mode in which the driver should invoke the frontend.
  Mode CompilerMode = Mode::StandardCompile;

  llvm::Optional<MSVCRuntime> RuntimeVariant = llvm::None;

  /// The output type which should be used for compile actions.
  file_types::ID CompilerOutputType = file_types::ID::TY_INVALID;

  enum class LTOKind {
    None,
    LLVMThin,
    LLVMFull,
  };

  LTOKind LTOVariant = LTOKind::None;

  std::string LibLTOPath;
  
  /// Describes if and how the output of compile actions should be
  /// linked together.
  LinkKind LinkAction = LinkKind::None;

  /// Returns true if the linker will be invoked at all.
  bool shouldLink() const { return LinkAction != LinkKind::None; }

  /// Whether or not the output should contain debug info.
  // FIXME: Eventually this should be replaced by dSYM generation.
  IRGenDebugInfoLevel DebugInfoLevel = IRGenDebugInfoLevel::None;

  /// What kind of debug info to generate.
  IRGenDebugInfoFormat DebugInfoFormat = IRGenDebugInfoFormat::None;

  /// Whether or not the driver should generate a module.
  bool ShouldGenerateModule = false;

  /// Whether or not the driver should treat a generated module as a top-level
  /// output.
  bool ShouldTreatModuleAsTopLevelOutput = false;

  /// Whether the compiler picked the current module name, rather than the user.
  bool ModuleNameIsFallback = false;

  /// The number of threads for multi-threaded compilation.
  unsigned numThreads = 0;

  /// Returns true if multi-threading is enabled.
  bool isMultiThreading() const { return numThreads > 0; }
  
  /// The name of the module which we are building.
  std::string ModuleName;

  /// The path to the SDK against which to build.
  /// (If empty, this implies no SDK.)
  std::string SDKPath;

  OptionSet<SanitizerKind> SelectedSanitizers;

  /// Might this sort of compile have explicit primary inputs?
  /// When running a single compile for the whole module (in other words
  /// "whole-module-optimization" mode) there must be no -primary-input's and
  /// nothing in a (preferably non-existent) -primary-filelist. Left to its own
  /// devices, the driver would forget to omit the primary input files, so
  /// return a flag here.
  bool mightHaveExplicitPrimaryInputs(const CommandOutput &Output) const;
};

class Driver {
public:
  /// DriverKind determines how later arguments are parsed, as well as the
  /// allowable OutputInfo::Mode values.
  enum class DriverKind {
    Interactive,     // swift
    Batch,           // swiftc
    SILOpt,          // sil-opt
    SILFuncExtractor,// sil-func-extractor
    SILNM,           // sil-nm
    SILLLVMGen,      // sil-llvm-gen
    SILPassPipelineDumper, // sil-passpipeline-dumper
    SwiftDependencyTool,   // swift-dependency-tool
    SwiftLLVMOpt,    // swift-llvm-opt
    AutolinkExtract, // swift-autolink-extract
    SwiftIndent,     // swift-indent
    SymbolGraph,     // swift-symbolgraph
    APIExtract,      // swift-api-extract
    APIDigester,     // swift-api-digester
    CacheTool        // swift-cache-tool
  };

  class InputInfoMap;

private:
  std::unique_ptr<llvm::opt::OptTable> Opts;

  DiagnosticEngine &Diags;

  /// The name the driver was invoked as.
  std::string Name;

  /// The original path to the executable.
  std::string DriverExecutable;

  // Extra args to pass to the driver executable
  SmallVector<std::string, 2> DriverExecutableArgs;

  DriverKind driverKind = DriverKind::Interactive;

  /// Default target triple.
  std::string DefaultTargetTriple;

  /// Indicates whether the driver should print bindings.
  bool DriverPrintBindings;

  /// Indicates whether the driver should suppress the "no input files" error.
  bool SuppressNoInputFilesError = false;

  /// Indicates whether the driver should check that the input files exist.
  bool CheckInputFilesExist = true;

  /// Indicates that this driver never actually executes any commands but is
  /// just set up to retrieve the swift-frontend invocation that would be
  /// executed during compilation.
  bool IsDummyDriverForFrontendInvocation = false;

public:
  Driver(StringRef DriverExecutable, StringRef Name,
         ArrayRef<const char *> Args, DiagnosticEngine &Diags);
  ~Driver();

  const llvm::opt::OptTable &getOpts() const { return *Opts; }

  const DiagnosticEngine &getDiags() const { return Diags; }

  const std::string &getSwiftProgramPath() const {
    return DriverExecutable;
  }

  ArrayRef<std::string> getSwiftProgramArgs() const {
    return DriverExecutableArgs;
  }

  DriverKind getDriverKind() const { return driverKind; }
  
  ArrayRef<const char *> getArgsWithoutProgramNameAndDriverMode(
                                            ArrayRef<const char *> Args) const;

  bool getCheckInputFilesExist() const { return CheckInputFilesExist; }

  void setCheckInputFilesExist(bool Value) { CheckInputFilesExist = Value; }

  bool isDummyDriverForFrontendInvocation() const {
    return IsDummyDriverForFrontendInvocation;
  }

  void setIsDummyDriverForFrontendInvocation(bool Value) {
    IsDummyDriverForFrontendInvocation = Value;
  }

  /// Creates an appropriate ToolChain for a given driver, given the target
  /// specified in \p Args (or the default target). Sets the value of \c
  /// DefaultTargetTriple from \p Args as a side effect.
  ///
  /// \return A ToolChain, or nullptr if an unsupported target was specified
  /// (in which case a diagnostic error is also signalled).
  ///
  /// This uses a std::unique_ptr instead of returning a toolchain by value
  /// because ToolChain has virtual methods.
  std::unique_ptr<ToolChain>
  buildToolChain(const llvm::opt::InputArgList &ArgList);

  /// Compute the task queue for this compilation and command line argument
  /// vector.
  ///
  /// \return A TaskQueue, or nullptr if an invalid number of parallel jobs is
  /// specified.  This condition is signalled by a diagnostic.
  std::unique_ptr<sys::TaskQueue> buildTaskQueue(const Compilation &C);

  /// Construct a compilation object for a given ToolChain and command line
  /// argument vector.
  ///
  /// If \p AllowErrors is set to \c true, this method tries to build a
  /// compilation even if there were errors.
  ///
  /// \return A Compilation, or nullptr if none was built for the given argument
  /// vector. A null return value does not necessarily indicate an error
  /// condition; the diagnostics should be queried to determine if an error
  /// occurred.
  std::unique_ptr<Compilation>
  buildCompilation(const ToolChain &TC,
                   std::unique_ptr<llvm::opt::InputArgList> ArgList,
                   bool AllowErrors = false);

  /// Parse the given list of strings into an InputArgList.
  std::unique_ptr<llvm::opt::InputArgList>
  parseArgStrings(ArrayRef<const char *> Args);

  /// Resolve path arguments if \p workingDirectory is non-empty, and translate
  /// inputs from -- arguments into a DerivedArgList.
  llvm::opt::DerivedArgList *
  translateInputAndPathArgs(const llvm::opt::InputArgList &ArgList,
                            StringRef workingDirectory) const;

  /// Construct the list of inputs and their types from the given arguments.
  ///
  /// \param TC The current tool chain.
  /// \param Args The input arguments.
  /// \param[out] Inputs The list in which to store the resulting compilation
  /// inputs.
  void buildInputs(const ToolChain &TC, const llvm::opt::DerivedArgList &Args,
                   InputFileList &Inputs) const;

  /// Construct the OutputInfo for the driver from the given arguments.
  ///
  /// \param TC The current tool chain.
  /// \param Args The input arguments.
  /// \param BatchMode Whether the driver has been explicitly or implicitly
  /// instructed to use batch mode.
  /// \param Inputs The inputs to the driver.
  /// \param[out] OI The OutputInfo in which to store the resulting output
  /// information.
  void buildOutputInfo(const ToolChain &TC,
                       const llvm::opt::DerivedArgList &Args,
                       const bool BatchMode, const InputFileList &Inputs,
                       OutputInfo &OI) const;

  /// Construct the list of Actions to perform for the given arguments,
  /// which are only done for a single architecture.
  ///
  /// \param[out] TopLevelActions The main Actions to build Jobs for.
  /// \param TC the default host tool chain.
  /// \param OI The OutputInfo for which Actions should be generated.
  /// \param OutOfDateMap If present, information used to decide which files
  /// need to be rebuilt.
  /// \param C The Compilation to which Actions should be added.
  void buildActions(SmallVectorImpl<const Action *> &TopLevelActions,
                    const ToolChain &TC, const OutputInfo &OI,
                    const InputInfoMap *OutOfDateMap,
                    Compilation &C) const;

  /// Construct the OutputFileMap for the driver from the given arguments.
  llvm::Optional<OutputFileMap>
  buildOutputFileMap(const llvm::opt::DerivedArgList &Args,
                     StringRef workingDirectory) const;

  /// Add top-level Jobs to Compilation \p C for the given \p Actions and
  /// OutputInfo.
  ///
  /// \param TopLevelActions The main Actions to build Jobs for.
  /// \param OI The OutputInfo for which Jobs should be generated.
  /// \param OFM The OutputFileMap for which Jobs should be generated.
  /// \param workingDirectory If non-empty, used to resolve any generated paths.
  /// \param TC The ToolChain to build Jobs with.
  /// \param C The Compilation containing the Actions for which Jobs should be
  /// created.
  void buildJobs(ArrayRef<const Action *> TopLevelActions, const OutputInfo &OI,
                 const OutputFileMap *OFM, StringRef workingDirectory,
                 const ToolChain &TC, Compilation &C) const;

  /// A map for caching Jobs for a given Action/ToolChain pair
  using JobCacheMap =
    llvm::DenseMap<std::pair<const Action *, const ToolChain *>, Job *>;

  /// Create a Job for the given Action \p A, including creating any necessary
  /// input Jobs.
  ///
  /// \param C The Compilation which this Job will eventually be part of
  /// \param JA The Action for which a Job should be created
  /// \param OFM The OutputFileMap for which a Job should be created
  /// \param AtTopLevel indicates whether or not this is a top-level Job
  /// \param JobCache maps existing Action/ToolChain pairs to Jobs
  ///
  /// \returns a Job for the given Action/ToolChain pair
  Job *buildJobsForAction(Compilation &C, const JobAction *JA,
                          const OutputFileMap *OFM,
                          StringRef workingDirectory,
                          bool AtTopLevel, JobCacheMap &JobCache) const;

private:
  void computeMainOutput(Compilation &C, const JobAction *JA,
                         const OutputFileMap *OFM, bool AtTopLevel,
                         SmallVectorImpl<const Action *> &InputActions,
                         SmallVectorImpl<const Job *> &InputJobs,
                         const TypeToPathMap *OutputMap,
                         StringRef workingDirectory,
                         StringRef BaseInput,
                         StringRef PrimaryInput,
                         llvm::SmallString<128> &Buf,
                         CommandOutput *Output) const;

  void chooseSwiftModuleOutputPath(Compilation &C,
                                   const TypeToPathMap *OutputMap,
                                   StringRef workingDirectory,
                                   CommandOutput *Output) const;

  void chooseSwiftModuleDocOutputPath(Compilation &C,
                                      const TypeToPathMap *OutputMap,
                                      StringRef workingDirectory,
                                      CommandOutput *Output) const;

  void chooseSwiftSourceInfoOutputPath(Compilation &C,
                                       const TypeToPathMap *OutputMap,
                                       StringRef workingDirectory,
                                       CommandOutput *Output) const;

  void chooseModuleInterfacePath(Compilation &C, const JobAction *JA,
                                 StringRef workingDirectory,
                                 llvm::SmallString<128> &buffer,
                                 file_types::ID fileType,
                                 CommandOutput *output) const;

  void chooseModuleSummaryPath(Compilation &C, const TypeToPathMap *OutputMap,
                               StringRef workingDirectory,
                               llvm::SmallString<128> &Buf,
                               CommandOutput *Output) const;

  void chooseRemappingOutputPath(Compilation &C, const TypeToPathMap *OutputMap,
                                 CommandOutput *Output) const;

  void chooseSerializedDiagnosticsPath(Compilation &C, const JobAction *JA,
                                       const TypeToPathMap *OutputMap,
                                       StringRef workingDirectory,
                                       CommandOutput *Output) const;

  void chooseDependenciesOutputPaths(Compilation &C,
                                     const TypeToPathMap *OutputMap,
                                     StringRef workingDirectory,
                                     llvm::SmallString<128> &Buf,
                                     CommandOutput *Output) const;

  void chooseOptimizationRecordPath(Compilation &C,
                                    StringRef workingDirectory,
                                    llvm::SmallString<128> &Buf,
                                    CommandOutput *Output) const;

  void chooseObjectiveCHeaderOutputPath(Compilation &C,
                                        const TypeToPathMap *OutputMap,
                                        StringRef workingDirectory,
                                        CommandOutput *Output) const;

  void chooseLoadedModuleTracePath(Compilation &C,
                                   StringRef workingDirectory,
                                   llvm::SmallString<128> &Buf,
                                   CommandOutput *Output) const;

  void chooseTBDPath(Compilation &C, const TypeToPathMap *OutputMap,
                     StringRef workingDirectory, llvm::SmallString<128> &Buf,
                     CommandOutput *Output) const;

public:
  /// Handle any arguments which should be treated before building actions or
  /// binding tools.
  ///
  /// \return Whether any compilation should be built for this invocation
  bool handleImmediateArgs(const llvm::opt::ArgList &Args, const ToolChain &TC);

  /// Print the list of Actions in a Compilation.
  void printActions(const Compilation &C) const;

  /// Print the driver version.
  void printVersion(const ToolChain &TC, raw_ostream &OS) const;

  /// Print the help text.
  ///
  /// \param ShowHidden Show hidden options.
  void printHelp(bool ShowHidden) const;

private:
  /// Parse the driver kind.
  ///
  /// \param Args The arguments passed to the driver (excluding the path to the
  /// driver)
  void parseDriverKind(ArrayRef<const char *> Args);

  /// Examine potentially conficting arguments and warn the user if
  /// there is an actual conflict.
  /// \param Args The input arguments.
  /// \param Inputs The inputs to the driver.
  /// \param BatchModeOut An out-parameter flag that indicates whether to
  /// batch the jobs of the resulting \c Mode::StandardCompile compilation.
  OutputInfo::Mode computeCompilerMode(const llvm::opt::DerivedArgList &Args,
                                       const InputFileList &Inputs,
                                       bool &BatchModeOut) const;
};

} // end namespace driver
} // end namespace swift

#endif
