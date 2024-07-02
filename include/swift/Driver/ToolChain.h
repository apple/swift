//===--- ToolChain.h - Collections of tools for one platform ----*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_TOOLCHAIN_H
#define SWIFT_DRIVER_TOOLCHAIN_H

#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Job.h"
#include "swift/Option/Options.h"
#include "llvm/TargetParser/Triple.h"
#include "llvm/Option/Option.h"

#include <memory>

namespace swift {
class DiagnosticEngine;

namespace driver {
class CommandOutput;
class Compilation;
class Driver;
class Job;
class OutputInfo;

/// A ToolChain is responsible for turning abstract Actions into concrete,
/// runnable Jobs.
///
/// The primary purpose of a ToolChain is built around the
/// \c constructInvocation family of methods. This is a set of callbacks
/// following the Visitor pattern for the various JobAction subclasses, which
/// returns an executable name and arguments for the Job to be run. The base
/// ToolChain knows how to perform most operations, but some (like linking)
/// require platform-specific knowledge, provided in subclasses.
class ToolChain {
  const Driver &D;
  const llvm::Triple Triple;
  mutable llvm::StringMap<std::string> ProgramLookupCache;

protected:
  ToolChain(const Driver &D, const llvm::Triple &T) : D(D), Triple(T) {}

  /// A special name used to identify the Swift executable itself.
  constexpr static const char *const SWIFT_EXECUTABLE_NAME = "swift";

  /// Packs together the supplementary information about the job being created.
  class JobContext {
  private:
    Compilation &C;

  public:
    ArrayRef<const Job *> Inputs;
    ArrayRef<const Action *> InputActions;
    const CommandOutput &Output;
    const OutputInfo &OI;

    /// The arguments to the driver. Can also be used to create new strings with
    /// the same lifetime.
    ///
    /// This just caches C.getArgs().
    const llvm::opt::ArgList &Args;

  public:
    JobContext(Compilation &C, ArrayRef<const Job *> Inputs,
               ArrayRef<const Action *> InputActions,
               const CommandOutput &Output, const OutputInfo &OI);

    /// Forwards to Compilation::getInputFiles.
    ArrayRef<InputPair> getTopLevelInputFiles() const;

    /// Forwards to Compilation::getAllSourcesPath.
    const char *getAllSourcesPath() const;

    /// Creates a new temporary file for use by a job.
    ///
    /// The returned string already has its lifetime extended to match other
    /// arguments.
    const char *getTemporaryFilePath(const llvm::Twine &name,
                                     StringRef suffix = "") const;

    /// For frontend, merge-module, and link invocations.
    bool shouldUseInputFileList() const;

    bool shouldUsePrimaryInputFileListInFrontendInvocation() const;

    bool shouldUseMainOutputFileListInFrontendInvocation() const;

    bool shouldUseSupplementaryOutputFileMapInFrontendInvocation() const;

    /// Reify the existing behavior that SingleCompile compile actions do not
    /// filter, but batch-mode and single-file compilations do. Some clients are
    /// relying on this (i.e., they pass inputs that don't have ".swift" as an
    /// extension.) It would be nice to eliminate this distinction someday.
    bool shouldFilterFrontendInputsByType() const;

    const char *computeFrontendModeForCompile() const;

    void addFrontendInputAndOutputArguments(
        llvm::opt::ArgStringList &Arguments,
        std::vector<FilelistInfo> &FilelistInfos) const;

  private:
    void addFrontendCommandLineInputArguments(
        bool mayHavePrimaryInputs, bool useFileList, bool usePrimaryFileList,
        bool filterByType, llvm::opt::ArgStringList &arguments) const;
    void addFrontendSupplementaryOutputArguments(
        llvm::opt::ArgStringList &arguments) const;
  };

  /// Packs together information chosen by toolchains to create jobs.
  struct InvocationInfo {
    const char *ExecutableName;
    llvm::opt::ArgStringList Arguments;
    std::vector<std::pair<const char *, const char *>> ExtraEnvironment;
    std::vector<FilelistInfo> FilelistInfos;

    // Not all platforms and jobs support the use of response files, so assume
    // "false" by default. If the executable specified in the InvocationInfo
    // constructor supports response files, this can be overridden and set to
    // "true".
    bool allowsResponseFiles = false;

    InvocationInfo(const char *name, llvm::opt::ArgStringList args = {},
                   decltype(ExtraEnvironment) extraEnv = {})
        : ExecutableName(name), Arguments(std::move(args)),
          ExtraEnvironment(std::move(extraEnv)) {}
  };

  /// Handle arguments common to all invocations of the frontend (compilation,
  /// module-merging, LLDB's REPL, etc).
  virtual void addCommonFrontendArgs(const OutputInfo &OI,
                                     const CommandOutput &output,
                                     const llvm::opt::ArgList &inputArgs,
                                     llvm::opt::ArgStringList &arguments) const;

  virtual void addPlatformSpecificPluginFrontendArgs(
      const OutputInfo &OI,
      const CommandOutput &output,
      const llvm::opt::ArgList &inputArgs,
      llvm::opt::ArgStringList &arguments) const;
  virtual InvocationInfo constructInvocation(const CompileJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const InterpretJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const BackendJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const MergeModuleJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const ModuleWrapJobAction &job,
                                             const JobContext &context) const;

  virtual InvocationInfo constructInvocation(const REPLJobAction &job,
                                             const JobContext &context) const;

  virtual InvocationInfo constructInvocation(const GenerateDSYMJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo
  constructInvocation(const VerifyDebugInfoJobAction &job,
                      const JobContext &context) const;
  virtual InvocationInfo
  constructInvocation(const VerifyModuleInterfaceJobAction &job,
                      const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const GeneratePCHJobAction &job,
                                             const JobContext &context) const;
  virtual InvocationInfo
  constructInvocation(const AutolinkExtractJobAction &job,
                      const JobContext &context) const;
  virtual InvocationInfo constructInvocation(const DynamicLinkJobAction &job,
                                             const JobContext &context) const;

  virtual InvocationInfo constructInvocation(const StaticLinkJobAction &job,
                                             const JobContext &context) const;

  /// Searches for the given executable in appropriate paths relative to the
  /// Swift binary.
  ///
  /// This method caches its results.
  ///
  /// \sa findProgramRelativeToSwiftImpl
  std::string findProgramRelativeToSwift(StringRef name) const;

  /// An override point for platform-specific subclasses to customize how to
  /// do relative searches for programs.
  ///
  /// This method is invoked by findProgramRelativeToSwift().
  virtual std::string findProgramRelativeToSwiftImpl(StringRef name) const;

  void addInputsOfType(llvm::opt::ArgStringList &Arguments,
                       ArrayRef<const Action *> Inputs,
                       file_types::ID InputType,
                       const char *PrefixArgument = nullptr) const;

  void addInputsOfType(llvm::opt::ArgStringList &Arguments,
                       ArrayRef<const Job *> Jobs,
                       const llvm::opt::ArgList &Args, file_types::ID InputType,
                       const char *PrefixArgument = nullptr) const;

  void addPrimaryInputsOfType(llvm::opt::ArgStringList &Arguments,
                              ArrayRef<const Job *> Jobs,
                              const llvm::opt::ArgList &Args,
                              file_types::ID InputType,
                              const char *PrefixArgument = nullptr) const;

  /// Get the resource dir link path, which is platform-specific and found
  /// relative to the compiler.
  void getResourceDirPath(SmallVectorImpl<char> &runtimeLibPath,
                          const llvm::opt::ArgList &args, bool shared) const;

  /// Get the secondary runtime library link path given the primary path.
  void getSecondaryResourceDirPath(
      SmallVectorImpl<char> &secondaryResourceDirPath,
      StringRef primaryPath) const;

  /// Get the runtime library link paths, which typically include the resource
  /// dir path and the SDK.
  void getRuntimeLibraryPaths(SmallVectorImpl<std::string> &runtimeLibPaths,
                              const llvm::opt::ArgList &args,
                              StringRef SDKPath, bool shared) const;

  void addPathEnvironmentVariableIfNeeded(Job::EnvironmentVector &env,
                                          const char *name,
                                          const char *separator,
                                          options::ID optionID,
                                          const llvm::opt::ArgList &args,
                                          ArrayRef<std::string> extraEntries = {}) const;

  /// Specific toolchains should override this to provide additional conditions
  /// under which the compiler invocation should be written into debug info. For
  /// example, Darwin does this if the RC_DEBUG_OPTIONS environment variable is
  /// set to match the behavior of Clang.
  virtual bool shouldStoreInvocationInDebugInfo() const { return false; }

  /// Specific toolchains should override this to provide additional
  /// -debug-prefix-map entries. For example, Darwin has an RC_DEBUG_PREFIX_MAP
  /// environment variable that is also understood by Clang.
  virtual std::string getGlobalDebugPathRemapping() const { return {}; }
  
  /// Gets the response file path and command line argument for an invocation
  /// if the tool supports response files and if the command line length would
  /// exceed system limits.
  std::optional<Job::ResponseFileInfo>
  getResponseFileInfo(const Compilation &C, const char *executablePath,
                      const InvocationInfo &invocationInfo,
                      const JobContext &context) const;

  void addPluginArguments(const llvm::opt::ArgList &Args,
                          llvm::opt::ArgStringList &Arguments) const;

public:
  virtual ~ToolChain() = default;

  const Driver &getDriver() const { return D; }
  const llvm::Triple &getTriple() const { return Triple; }

  /// Special handling for passing down '-l' arguments.
  ///
  /// Not all downstream tools (lldb, ld etc.) consistently accept
  /// a space between the '-l' flag and its argument, so we remove
  /// the extra space if it was present in \c Args.
  static void addLinkedLibArgs(const llvm::opt::ArgList &Args,
                               llvm::opt::ArgStringList &FrontendArgs);

  /// Construct a Job for the action \p JA, taking the given information into
  /// account.
  ///
  /// This method dispatches to the various \c constructInvocation methods,
  /// which may be overridden by platform-specific subclasses.
  std::unique_ptr<Job> constructJob(const JobAction &JA, Compilation &C,
                                    SmallVectorImpl<const Job *> &&inputs,
                                    ArrayRef<const Action *> inputActions,
                                    std::unique_ptr<CommandOutput> output,
                                    const OutputInfo &OI) const;

  /// Return true iff the input \c Job \p A is an acceptable candidate for
  /// batching together into a BatchJob, via a call to \c
  /// constructBatchJob. This is true when the \c Job is a built from a \c
  /// CompileJobAction in a \c Compilation \p C running in \c
  /// OutputInfo::Mode::StandardCompile output mode, with a single \c TY_Swift
  /// \c InputAction.
  bool jobIsBatchable(const Compilation &C, const Job *A) const;

  /// Equivalence relation that holds iff the two input Jobs \p A and \p B are
  /// acceptable candidates for combining together into a \c BatchJob, via a
  /// call to \c constructBatchJob. This is true when each job independently
  /// satisfies \c jobIsBatchable, and the two jobs have identical executables,
  /// output types and environments (i.e. they are identical aside from their
  /// inputs).
  bool jobsAreBatchCombinable(const Compilation &C, const Job *A,
                              const Job *B) const;

  /// Construct a \c BatchJob that subsumes the work of a set of Jobs. Any pair
  /// of elements in \p Jobs are assumed to satisfy the equivalence relation \c
  /// jobsAreBatchCombinable, i.e. they should all be "the same" job in in all
  /// ways other than their choices of inputs. The provided \p NextQuasiPID
  /// should be a negative number that persists between calls; this method will
  /// decrement it to assign quasi-PIDs to each of the \p Jobs passed.
  std::unique_ptr<Job> constructBatchJob(ArrayRef<const Job *> Jobs,
                                         int64_t &NextQuasiPID,
                                         Compilation &C) const;

  /// Return the default language type to use for the given extension.
  /// If the extension is empty or is otherwise not recognized, return
  /// the invalid type \c TY_INVALID.
  file_types::ID lookupTypeForExtension(StringRef Ext) const;

  /// Copies the path for the directory clang libraries would be stored in on
  /// the current toolchain.
  void getClangLibraryPath(const llvm::opt::ArgList &Args,
                           SmallString<128> &LibPath) const;

  // Returns the Clang driver executable to use for linking.
  const char *getClangLinkerDriver(const llvm::opt::ArgList &Args) const;

  /// Returns the name the clang library for a given sanitizer would have on
  /// the current toolchain.
  ///
  /// \param Sanitizer Sanitizer name.
  /// \param shared Whether the library is shared
  virtual std::string sanitizerRuntimeLibName(StringRef Sanitizer,
                                              bool shared = true) const = 0;

  /// Returns whether a given sanitizer exists for the current toolchain.
  ///
  /// \param sanitizer Sanitizer name.
  /// \param shared Whether the library is shared
  bool sanitizerRuntimeLibExists(const llvm::opt::ArgList &args,
                                 StringRef sanitizer, bool shared = true) const;

  /// Adds a runtime library to the arguments list for linking.
  ///
  /// \param LibName The library name
  /// \param Arguments The arguments list to append to
  void addLinkRuntimeLib(const llvm::opt::ArgList &Args,
                         llvm::opt::ArgStringList &Arguments,
                         StringRef LibName) const;

  /// Validates arguments passed to the toolchain.
  ///
  /// An override point for platform-specific subclasses to customize the
  /// validations that should be performed.
  virtual void validateArguments(DiagnosticEngine &diags,
                                 const llvm::opt::ArgList &args,
                                 StringRef defaultTarget) const {}

  /// Validate the output information.
  ///
  /// An override point for platform-specific subclasses to customize their
  /// behavior once the outputs are known.
  virtual void validateOutputInfo(DiagnosticEngine &diags,
                                  const OutputInfo &outputInfo) const { }

  llvm::Expected<file_types::ID>
  remarkFileTypeFromArgs(const llvm::opt::ArgList &Args) const;
};
} // end namespace driver
} // end namespace swift

#endif
