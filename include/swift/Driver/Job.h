//===--- Job.h - Commands to Execute ----------------------------*- C++ -*-===//
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

#ifndef SWIFT_DRIVER_JOB_H
#define SWIFT_DRIVER_JOB_H

#include "swift/Basic/Debug.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/OutputFileMap.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Util.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Program.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

namespace swift {
namespace driver {

class Job;
class JobAction;

/// \file Job.h
///
///Some terminology for the following sections (and especially Driver.cpp):
///
/// BaseInput: a filename provided by the user, upstream of the entire Job
///            graph, usually denoted by an InputAction. Every Job has access,
///            during construction, to a set of BaseInputs that are upstream of
///            its inputs and input jobs in the job graph, and from which it can
///            derive PrimaryInput names for itself.
///
/// BaseOutput: a filename that is a non-temporary, output at the bottom of a
///             Job graph, and often (though not always) directly specified by
///             the user in the form of a -o or -emit-foo-path name, or an entry
///             in a user-provided OutputFileMap. May also be an auxiliary,
///             derived from a BaseInput and a type.
///
/// PrimaryInput: one of the distinguished inputs-to-act-on (as opposed to
///               merely informative additional inputs) to a Job. May be a
///               BaseInput but may also be a temporary that doesn't live beyond
///               the execution of the Job graph.
///
/// PrimaryOutput: an output file matched 1:1 with a specific
///                PrimaryInput. Auxiliary outputs may also be produced. A
///                PrimaryOutput may be a BaseOutput, but may also be a
///                temporary that doesn't live beyond the execution of the Job
///                graph (that is: it exists in order to be the PrimaryInput
///                for a subsequent Job).
///
/// The user-provided OutputFileMap lists BaseInputs and BaseOutputs, but doesn't
/// describe the temporaries inside the Job graph.
///
/// The Compilation's DerivedOutputFileMap (shared by all CommandOutputs) lists
/// PrimaryInputs and maps them to PrimaryOutputs, including all the
/// temporaries. This means that in a multi-stage Job graph, the BaseInput =>
/// BaseOutput entries provided by the user are split in two (or more) steps,
/// one BaseInput => SomeTemporary and one SomeTemporary => BaseOutput.
///
/// To try to keep this as simple as possible (it's already awful) we associate
/// every PrimaryInput 1:1 with a specific BaseInput from which it was derived;
/// this way a CommandOutput will have a vector of _pairs_ of
/// {Base,Primary}Inputs rather than a pair of separate vectors. This arrangement
/// appears to cover all the graph topologies we encounter in practice.


struct CommandInputPair {
  /// A filename provided from the user, either on the command line or in an
  /// input file map. Feeds into a Job graph, from InputActions, and is
  /// _associated_ with a PrimaryInput for a given Job, but may be upstream of
  /// the Job (and its PrimaryInput) and thus not necessarily passed as a
  /// filename to the job. Used as a key into the user-provided OutputFileMap
  /// (of BaseInputs and BaseOutputs), and used to derive downstream names --
  /// both temporaries and auxiliaries -- but _not_ used as a key into the
  /// DerivedOutputFileMap.
  StringRef Base;

  /// A filename that _will be passed_ to the command as a designated primary
  /// input. Typically either equal to BaseInput or a temporary with a name
  /// derived from the BaseInput it is related to. Also used as a key into
  /// the DerivedOutputFileMap.
  StringRef Primary;

  /// Construct a CommandInputPair from a Base Input and, optionally, a Primary;
  /// if the Primary is empty, use the Base value for it.
  explicit CommandInputPair(StringRef BaseInput, StringRef PrimaryInput)
    : Base(BaseInput),
      Primary(PrimaryInput.empty() ? BaseInput : PrimaryInput)
    {}
};

class CommandOutput {

  /// A CommandOutput designates one type of output as primary, though there
  /// may be multiple outputs of that type.
  file_types::ID PrimaryOutputType;

  /// A CommandOutput also restricts its attention regarding additional-outputs
  /// to a subset of the PrimaryOutputs associated with its PrimaryInputs;
  /// sometimes multiple commands operate on the same PrimaryInput, in different
  /// phases (eg. autolink-extract and link both operate on the same .o file),
  /// so Jobs cannot _just_ rely on the presence of a primary output in the
  /// DerivedOutputFileMap.
  llvm::SmallSet<file_types::ID, 4> AdditionalOutputTypes;

  /// The list of inputs for this \c CommandOutput. Each input in the list has
  /// two names (often but not always the same), of which the second (\c
  /// CommandInputPair::Primary) acts as a key into \c DerivedOutputMap.  Each
  /// input thus designates an associated _set_ of outputs, one of which (the
  /// one of type \c PrimaryOutputType) is considered the "primary output" for
  /// the input.
  SmallVector<CommandInputPair, 1> Inputs;

  /// All CommandOutputs in a Compilation share the same \c
  /// DerivedOutputMap. This is computed both from any user-provided input file
  /// map, and any inference steps.
  OutputFileMap &DerivedOutputMap;

  // If there is an entry in the DerivedOutputMap for a given (\p
  // PrimaryInputFile, \p Type) pair, return a nonempty StringRef, otherwise
  // return an empty StringRef.
  StringRef getOutputForInputAndType(StringRef PrimaryInputFile,
                                     file_types::ID Type) const;

  /// Add an entry to the \c DerivedOutputMap if it doesn't exist. If an entry
  /// already exists for \p PrimaryInputFile of type \p type, then either
  /// overwrite the entry (if \p overwrite is \c true) or assert that it has
  /// the same value as \p OutputFile.
  void ensureEntry(StringRef PrimaryInputFile, file_types::ID Type,
                   StringRef OutputFile, bool Overwrite);

public:
  CommandOutput(file_types::ID PrimaryOutputType, OutputFileMap &Derived);

  /// For testing dependency graphs that use Jobs
  CommandOutput(StringRef dummyBaseName, OutputFileMap &);

  /// Return the primary output type for this CommandOutput.
  file_types::ID getPrimaryOutputType() const;

  /// Associate a new \p PrimaryOutputFile (of type \c getPrimaryOutputType())
  /// with the provided \p Input pair of Base and Primary inputs.
  void addPrimaryOutput(CommandInputPair Input, StringRef PrimaryOutputFile,
                        StringRef IndexUnitOutputPath);

  /// Return true iff the set of additional output types in \c this is
  /// identical to the set of additional output types in \p other.
  bool hasSameAdditionalOutputTypes(CommandOutput const &other) const;

  /// Copy all the input pairs from \p other to \c this. Assumes (and asserts)
  /// that \p other shares output file map and PrimaryOutputType with \c this
  /// already, as well as AdditionalOutputTypes if \c this has any.
  void addOutputs(CommandOutput const &other);

  /// Assuming (and asserting) that there is only one input pair, return the
  /// primary output file associated with it. Note that the returned StringRef
  /// may be invalidated by subsequent mutations to the \c CommandOutput.
  StringRef getPrimaryOutputFilename() const;

  /// Return a all of the outputs of type \c getPrimaryOutputType() associated
  /// with a primary input. The return value will contain one \c StringRef per
  /// primary input, _even if_ the primary output type is TY_Nothing, and the
  /// primary output filenames are therefore all empty strings.
  ///
  /// FIXME: This is not really ideal behaviour -- it would be better to return
  /// only nonempty strings in all cases, and have the callers differentiate
  /// contexts with absent primary outputs another way -- but this is currently
  /// assumed at several call sites.
  SmallVector<StringRef, 16> getPrimaryOutputFilenames() const;


  /// Returns the output file path to record in the index data for each input.
  /// The return value will contain one \c StringRef per primary input if any
  /// input had an output filename for the index data that was different to its
  /// primary output filename, and be empty otherwise.
  SmallVector<StringRef, 16> getIndexUnitOutputFilenames() const;

  /// Assuming (and asserting) that there are one or more input pairs, associate
  /// an additional output named \p OutputFilename of type \p type with the
  /// first primary input. If the provided \p type is the primary output type,
  /// overwrite the existing entry associated with the first primary input.
  void setAdditionalOutputForType(file_types::ID type,
                                  StringRef OutputFilename);

  /// Assuming (and asserting) that there are one or more input pairs, return
  /// the _additional_ (not primary) output of type \p type associated with the
  /// first primary input.
  StringRef getAdditionalOutputForType(file_types::ID type) const;

  /// Assuming (and asserting) that there are one or more input pairs, return true if there exists
  /// an _additional_ (not primary) output of type \p type associated with the
  /// first primary input.
  bool hasAdditionalOutputForType(file_types::ID type) const;

  /// Return a vector of additional (not primary) outputs of type \p type
  /// associated with the primary inputs.
  ///
  /// In contrast to \c getPrimaryOutputFilenames, this method does _not_ return
  /// any empty strings or ensure the return vector is matched in size with the
  /// set of primary inputs; however it _does_ assert that the return vector's
  /// length is _either_ zero, one, or equal to the size of the set of inputs,
  /// as these are the only valid arity relationships between primary and
  /// additional outputs.
  SmallVector<StringRef, 16>
  getAdditionalOutputsForType(file_types::ID type) const;

  /// Assuming (and asserting) that there is only one input pair, return any
  /// output -- primary or additional -- of type \p type associated with that
  /// the sole primary input.
  StringRef getAnyOutputForType(file_types::ID type) const;

  /// Return the whole derived output map.
  const OutputFileMap &getDerivedOutputMap() const;

  /// Return the BaseInput numbered by \p Index.
  StringRef getBaseInput(size_t Index) const;

  /// Write a file map naming the outputs for each primary input.
  void writeOutputFileMap(llvm::raw_ostream &out) const;

  void print(raw_ostream &Stream) const;
  SWIFT_DEBUG_DUMP;

  /// For use in assertions: check the CommandOutput's state is consistent with
  /// its invariants.
  void checkInvariants() const;
};

class Job {
public:
  enum class Condition {
    // There was no information about the previous build (i.e., an input map),
    // or the map marked this Job as dirty or needing a cascading build.
    // Be maximally conservative with dependencies.
    Always,
    // The input changed, or this job was scheduled as non-cascading in the last
    // build but didn't get to run.
    RunWithoutCascading,
    // The best case: input didn't change, output exists.
    // Only run if it depends on some other thing that changed.
    CheckDependencies,
    // Run no matter what (but may or may not cascade).
    NewlyAdded
  };

  /// Packs together information about response file usage for a job.
  ///
  /// The strings in this struct must be kept alive as long as the Job is alive
  /// (e.g., by calling MakeArgString on the arg list associated with the
  /// Compilation).
  struct ResponseFileInfo {
    /// The path to the response file that a job should use.
    const char *path;

    /// The '@'-prefixed argument string that should be passed to the tool to
    /// use the response file.
    const char *argString;
  };

  using EnvironmentVector = std::vector<std::pair<const char *, const char *>>;

  /// If positive, contains llvm::ProcessID for a real Job on the host OS. If
  /// negative, contains a quasi-PID, which identifies a Job that's a member of
  /// a BatchJob _without_ denoting an operating system process.
  using PID = int64_t;

private:
  /// The action which caused the creation of this Job, and the conditions
  /// under which it must be run.
  llvm::PointerIntPair<const JobAction *, 2, Condition> SourceAndCondition;

  /// The list of other Jobs which are inputs to this Job.
  SmallVector<const Job *, 4> Inputs;

  /// The output of this command.
  std::unique_ptr<CommandOutput> Output;

  /// The executable to run.
  const char *Executable;

  /// The list of program arguments (not including the implicit first argument,
  /// which will be the Executable).
  ///
  /// These argument strings must be kept alive as long as the Job is alive.
  llvm::opt::ArgStringList Arguments;

  /// Additional variables to set in the process environment when running.
  ///
  /// These strings must be kept alive as long as the Job is alive.
  EnvironmentVector ExtraEnvironment;

  /// Whether the job wants a list of input or output files created.
  std::vector<FilelistInfo> FilelistFileInfos;

  /// The path and argument string to use for the response file if the job's
  /// arguments should be passed using one.
  llvm::Optional<ResponseFileInfo> ResponseFile;

  /// The modification time of the main input file, if any.
  llvm::sys::TimePoint<> InputModTime = llvm::sys::TimePoint<>::max();

#ifndef NDEBUG
  /// The "wave" of incremental jobs that this \c Job was scheduled into.
  ///
  /// The first "wave" of jobs is computed by the driver from the set of inputs
  /// and external files that have been mutated by the user. From there, as
  /// jobs from the first wave finish executing, we reload their \c swiftdeps
  /// files and re-integrate them into the dependency graph to discover
  /// the jobs for the second "wave".
  ///
  /// In +asserts builds, we ensure that no more than two "waves" occur for
  /// any given incremental compilation session. This is a consequence of
  /// 1) transitivity in dependency arcs
  /// 2) dependency tracing from uses that affect a def's interfaces to that
  ///    def's uses.
  mutable unsigned Wave = 1;
#endif

public:
  Job(const JobAction &Source, SmallVectorImpl<const Job *> &&Inputs,
      std::unique_ptr<CommandOutput> Output, const char *Executable,
      llvm::opt::ArgStringList Arguments,
      EnvironmentVector ExtraEnvironment = {},
      std::vector<FilelistInfo> Infos = {},
      llvm::Optional<ResponseFileInfo> ResponseFile = llvm::None)
      : SourceAndCondition(&Source, Condition::Always),
        Inputs(std::move(Inputs)), Output(std::move(Output)),
        Executable(Executable), Arguments(std::move(Arguments)),
        ExtraEnvironment(std::move(ExtraEnvironment)),
        FilelistFileInfos(std::move(Infos)), ResponseFile(ResponseFile) {}

  /// For testing dependency graphs that use Jobs
  Job(OutputFileMap &OFM, StringRef dummyBaseName)
      : Job(CompileJobAction(file_types::TY_Object),
            SmallVector<const Job *, 4>(),
            std::make_unique<CommandOutput>(dummyBaseName, OFM), nullptr, {}) {}

  virtual ~Job();

  const JobAction &getSource() const {
    return *SourceAndCondition.getPointer();
  }

  const char *getExecutable() const { return Executable; }
  const llvm::opt::ArgStringList &getArguments() const { return Arguments; }
  ArrayRef<const char *> getResponseFileArg() const {
    assert(hasResponseFile());
    return ResponseFile->argString;
  }
  ArrayRef<FilelistInfo> getFilelistInfos() const { return FilelistFileInfos; }
  ArrayRef<const char *> getArgumentsForTaskExecution() const;

  ArrayRef<const Job *> getInputs() const { return Inputs; }
  const CommandOutput &getOutput() const { return *Output; }

  Condition getCondition() const {
    return SourceAndCondition.getInt();
  }
  void setCondition(Condition Cond) {
    SourceAndCondition.setInt(Cond);
  }

  void setInputModTime(llvm::sys::TimePoint<> time) {
    InputModTime = time;
  }

  llvm::sys::TimePoint<> getInputModTime() const {
    return InputModTime;
  }

  ArrayRef<std::pair<const char *, const char *>> getExtraEnvironment() const {
    return ExtraEnvironment;
  }

  /// Print the command line for this Job to the given \p stream,
  /// terminating output with the given \p terminator.
  void printCommandLine(raw_ostream &Stream, StringRef Terminator = "\n") const;

  /// Print a short summary of this Job to the given \p Stream.
  void printSummary(raw_ostream &Stream) const;

  /// Print the command line for this Job to the given \p stream,
  /// and include any extra environment variables that will be set.
  ///
  /// \sa printCommandLine
  void printCommandLineAndEnvironment(raw_ostream &Stream,
                                      StringRef Terminator = "\n") const;

  /// Call the provided Callback with any Jobs (and their possibly-quasi-PIDs)
  /// contained within this Job; if this job is not a BatchJob, just pass \c
  /// this and the provided \p OSPid back to the Callback.
  virtual void forEachContainedJobAndPID(
      llvm::sys::procid_t OSPid,
      llvm::function_ref<void(const Job *, Job::PID)> Callback) const {
    Callback(this, static_cast<Job::PID>(OSPid));
  }

  SWIFT_DEBUG_DUMP;

  static void printArguments(raw_ostream &Stream,
                             const llvm::opt::ArgStringList &Args);

  bool hasResponseFile() const { return ResponseFile.has_value(); }

  bool writeArgsToResponseFile() const;

  /// Assumes that, if a compile job, has one primary swift input
  /// May return empty if none.
  StringRef getFirstSwiftPrimaryInput() const;

#ifndef NDEBUG
  unsigned getWave() const { return Wave; }
  void setWave(unsigned WaveNum) const { Wave = WaveNum; }
#endif
};

/// A BatchJob comprises a _set_ of jobs, each of which is sufficiently similar
/// to the others that the whole set can be combined into a single subprocess
/// (and thus run potentially more-efficiently than running each Job in the set
/// individually).
///
/// Not all Jobs can be combined into a BatchJob: at present, only those Jobs
/// that come from CompileJobActions, and which otherwise have the exact same
/// input file list and arguments as one another, aside from their primary-file.
/// See ToolChain::jobsAreBatchCombinable for details.

class BatchJob : public Job {

  /// The set of constituents making up the batch.
  const SmallVector<const Job *, 4> CombinedJobs;

  /// A negative number to use as the base value for assigning quasi-PID to Jobs
  /// in the \c CombinedJobs array. Quasi-PIDs count _down_ from this value.
  const Job::PID QuasiPIDBase;

public:
  BatchJob(const JobAction &Source, SmallVectorImpl<const Job *> &&Inputs,
           std::unique_ptr<CommandOutput> Output, const char *Executable,
           llvm::opt::ArgStringList Arguments,
           EnvironmentVector ExtraEnvironment, std::vector<FilelistInfo> Infos,
           ArrayRef<const Job *> Combined, Job::PID &NextQuasiPID,
           llvm::Optional<ResponseFileInfo> ResponseFile = llvm::None);

  ArrayRef<const Job*> getCombinedJobs() const {
    return CombinedJobs;
  }

  /// Call the provided callback for each Job in the batch, passing the
  /// corresponding quasi-PID with each Job.
  void forEachContainedJobAndPID(
      llvm::sys::procid_t OSPid,
      llvm::function_ref<void(const Job *, Job::PID)> Callback) const override {
    Job::PID QPid = QuasiPIDBase;
    assert(QPid < 0);
    for (auto const *J : CombinedJobs) {
      assert(QPid != std::numeric_limits<Job::PID>::min());
      Callback(J, QPid--);
    }
  }
};

} // end namespace driver
} // end namespace swift

#endif
