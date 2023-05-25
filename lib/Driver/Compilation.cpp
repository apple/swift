//===--- Compilation.cpp - Compilation Task Data Structure ----------------===//
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

#include "swift/Driver/Compilation.h"

#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/Basic/OutputFileMap.h"
#include "swift/Basic/ParseableOutput.h"
#include "swift/Basic/Program.h"
#include "swift/Basic/STLExtras.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/TaskQueue.h"
#include "swift/Basic/Version.h"
#include "swift/Basic/type_traits.h"
#include "swift/Driver/Action.h"
#include "swift/Driver/Driver.h"
#include "swift/Driver/FineGrainedDependencyDriverGraph.h"
#include "swift/Driver/Job.h"
#include "swift/Driver/ToolChain.h"
#include "swift/Option/Options.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Option/Arg.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/raw_ostream.h"

#include "CompilationRecord.h"

#include <fstream>
#include <signal.h>
#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

#define DEBUG_TYPE "batch-mode"

// Batch-mode has a sub-mode for testing that randomizes batch partitions,
// by user-provided seed. That is the only thing randomized here.
#include <random>

using namespace swift;
using namespace swift::sys;
using namespace swift::driver;
using namespace swift::parseable_output;
using namespace llvm::opt;

struct LogJob {
  const Job *j;
  LogJob(const Job *j) : j(j) {}
};

struct LogJobArray {
  const ArrayRef<const Job *> js;
  LogJobArray(const ArrayRef<const Job *> js) : js(js) {}
};

struct LogJobSet {
  const SmallPtrSetImpl<const Job*> &js;
  LogJobSet(const SmallPtrSetImpl<const Job*> &js) : js(js) {}
};

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const LogJob &lj) {
  lj.j->printSummary(os);
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const LogJobArray &ljs) {
  os << "[";
  interleave(ljs.js,
             [&](Job const *j) { os << LogJob(j); },
             [&]() { os << ' '; });
  os << "]";
  return os;
}

llvm::raw_ostream &operator<<(llvm::raw_ostream &os, const LogJobSet &ljs) {
  os << "{";
  interleave(ljs.js,
             [&](Job const *j) { os << LogJob(j); },
             [&]() { os << ' '; });
  os << "}";
  return os;
}

// clang-format off
Compilation::Compilation(DiagnosticEngine &Diags,
                         const ToolChain &TC,
                         OutputInfo const &OI,
                         OutputLevel Level,
                         std::unique_ptr<InputArgList> InputArgs,
                         std::unique_ptr<DerivedArgList> TranslatedArgs,
                         InputFileList InputsWithTypes,
                         std::string CompilationRecordPath,
                         StringRef ArgsHash,
                         llvm::sys::TimePoint<> StartTime,
                         llvm::sys::TimePoint<> LastBuildTime,
                         size_t FilelistThreshold,
                         bool EnableIncrementalBuild,
                         bool EnableBatchMode,
                         unsigned BatchSeed,
                         Optional<unsigned> BatchCount,
                         Optional<unsigned> BatchSizeLimit,
                         bool SaveTemps,
                         bool ShowDriverTimeCompilation,
                         std::unique_ptr<UnifiedStatsReporter> StatsReporter,
                         bool OnlyOneDependencyFile,
                         bool VerifyFineGrainedDependencyGraphAfterEveryImport,
                         bool EmitFineGrainedDependencyDotFileAfterEveryImport,
                         bool EnableCrossModuleIncrementalBuild)
  : Diags(Diags), TheToolChain(TC),
    TheOutputInfo(OI),
    Level(Level),
    RawInputArgs(std::move(InputArgs)),
    TranslatedArgs(std::move(TranslatedArgs)),
    InputFilesWithTypes(std::move(InputsWithTypes)),
    CompilationRecordPath(CompilationRecordPath),
    ArgsHash(ArgsHash),
    BuildStartTime(StartTime),
    LastBuildTime(LastBuildTime),
    EnableIncrementalBuild(EnableIncrementalBuild),
    EnableBatchMode(EnableBatchMode),
    BatchSeed(BatchSeed),
    BatchCount(BatchCount),
    BatchSizeLimit(BatchSizeLimit),
    SaveTemps(SaveTemps),
    ShowDriverTimeCompilation(ShowDriverTimeCompilation),
    Stats(std::move(StatsReporter)),
    FilelistThreshold(FilelistThreshold),
    OnlyOneDependencyFile(OnlyOneDependencyFile),
    VerifyFineGrainedDependencyGraphAfterEveryImport(
      VerifyFineGrainedDependencyGraphAfterEveryImport),
    EmitFineGrainedDependencyDotFileAfterEveryImport(
      EmitFineGrainedDependencyDotFileAfterEveryImport),
    EnableCrossModuleIncrementalBuild(EnableCrossModuleIncrementalBuild)
    { }
// clang-format on

static bool writeFilelistIfNecessary(const Job *job, const ArgList &args,
                                     DiagnosticEngine &diags);

using CommandSetVector = llvm::SetVector<const Job*>;
using BatchPartition = std::vector<std::vector<const Job*>>;

using InputInfoMap = llvm::SmallMapVector<const llvm::opt::Arg *,
                                          CompileJobAction::InputInfo, 16>;

namespace {
static DetailedTaskDescription
constructDetailedTaskDescription(const driver::Job &Cmd) {
  std::string Executable = Cmd.getExecutable();
  SmallVector<std::string, 16> Arguments;
  std::string CommandLine;
  SmallVector<CommandInput, 4> Inputs;
  SmallVector<OutputPair, 8> Outputs;
  for (const auto &A : Cmd.getArguments()) {
    Arguments.push_back(A);
  }
  llvm::raw_string_ostream wrapper(CommandLine);
  Cmd.printCommandLine(wrapper, "");
  wrapper.flush();

  for (const Action *A : Cmd.getSource().getInputs()) {
    if (const auto *IA = dyn_cast<InputAction>(A))
      Inputs.push_back(CommandInput(IA->getInputArg().getValue()));
  }

  for (const driver::Job *J : Cmd.getInputs()) {
    auto OutFiles = J->getOutput().getPrimaryOutputFilenames();
    if (const auto *BJAction = dyn_cast<BackendJobAction>(&Cmd.getSource())) {
      Inputs.push_back(CommandInput(OutFiles[BJAction->getInputIndex()]));
    } else {
      for (llvm::StringRef FileName : OutFiles) {
        Inputs.push_back(CommandInput(FileName));
      }
    }
  }

  // TODO: set up Outputs appropriately.
  file_types::ID PrimaryOutputType = Cmd.getOutput().getPrimaryOutputType();
  if (PrimaryOutputType != file_types::TY_Nothing) {
    for (llvm::StringRef OutputFileName :
         Cmd.getOutput().getPrimaryOutputFilenames()) {
      Outputs.push_back(OutputPair(PrimaryOutputType, OutputFileName.str()));
    }
  }
  file_types::forAllTypes([&](file_types::ID Ty) {
    for (auto Output : Cmd.getOutput().getAdditionalOutputsForType(Ty)) {
      Outputs.push_back(OutputPair(Ty, Output.str()));
    }
  });

  return DetailedTaskDescription{Executable, Arguments, CommandLine, Inputs,
                                 Outputs};
}
} // namespace

namespace swift {
namespace driver {
  class PerformJobsState {

    /// The containing Compilation object.
    Compilation &Comp;

    /// All jobs which have been scheduled for execution (whether or not
    /// they've finished execution), or which have been determined that they
    /// don't need to run.
    CommandSet ScheduledCommands;

    /// A temporary buffer to hold commands that were scheduled but haven't been
    /// added to the Task Queue yet, because we might try batching them together
    /// first.
    CommandSetVector PendingExecution;

    /// Set of synthetic BatchJobs that serve to cluster subsets of jobs waiting
    /// in PendingExecution. Also used to identify (then unpack) BatchJobs back
    /// to their underlying non-Batch Jobs, when running a callback from
    /// TaskQueue.
    CommandSet BatchJobs;

    /// Persistent counter for allocating quasi-PIDs to Jobs combined into
    /// BatchJobs. Quasi-PIDs are _negative_ PID-like unique keys used to
    /// masquerade BatchJob constituents as (quasi)processes, when writing
    /// parseable output to consumers that don't understand the idea of a batch
    /// job. They are negative in order to avoid possibly colliding with real
    /// PIDs (which are always positive). We start at -1000 here as a crude but
    /// harmless hedge against colliding with an errno value that might slip
    /// into the stream of real PIDs (say, due to a TaskQueue bug).
    int64_t NextBatchQuasiPID = parseable_output::QUASI_PID_START;

    /// All jobs which have finished execution or which have been determined
    /// that they don't need to run.
    CommandSet FinishedCommands;

    /// A map from a Job to the commands it is known to be blocking.
    ///
    /// The blocked jobs should be scheduled as soon as possible.
    llvm::SmallDenseMap<const Job *, TinyPtrVector<const Job *>, 16>
        BlockingCommands;

    /// A map from commands that didn't get to run to whether or not they affect
    /// downstream commands.
    ///
    /// Only intended for source files.
    llvm::SmallDenseMap<const Job *, bool, 16> UnfinishedCommands;

    /// Jobs that incremental-mode has decided it can skip.
    CommandSet DeferredCommands;
  public:
    /// Dependency graphs for deciding which jobs are dirty (need running)
    /// or clean (can be skipped).
    fine_grained_dependencies::ModuleDepGraph FineGrainedDepGraph;

  private:
    /// TaskQueue for execution.
    std::unique_ptr<TaskQueue> TQ;

    /// Cumulative result of PerformJobs(), accumulated from subprocesses.
    int ResultCode = EXIT_SUCCESS;

    /// True if any Job crashed.
    bool AnyAbnormalExit = false;

    /// Timers for monitoring execution time of subprocesses.
    llvm::TimerGroup DriverTimerGroup {"driver", "Driver Compilation Time"};
    llvm::SmallDenseMap<const Job *, std::unique_ptr<llvm::Timer>, 16>
    DriverTimers;

    void noteBuilding(const Job *cmd, const bool willBeBuilding,
                      StringRef reason) const {
      if (!Comp.getShowIncrementalBuildDecisions())
        return;
      if (ScheduledCommands.count(cmd))
        return;
      if (!willBeBuilding)
        return;

      llvm::outs() << "Queuing " << reason << ": " << LogJob(cmd) << "\n";
      getFineGrainedDepGraph().printPath(llvm::outs(), cmd);
    }

    template <typename JobsCollection>
    void noteBuildingJobs(const JobsCollection &unsortedJobsArg,
                          const StringRef reason) const {
      if (!Comp.getShowIncrementalBuildDecisions() &&
          !Comp.getShowJobLifecycle())
        return;
      // Sigh, must manually convert SmallPtrSet to ArrayRef-able container
      llvm::SmallVector<const Job *, 16> unsortedJobs;
      for (const Job *j : unsortedJobsArg)
        unsortedJobs.push_back(j);
      llvm::SmallVector<const Job *, 16> sortedJobs;
      Comp.sortJobsToMatchCompilationInputs(unsortedJobs, sortedJobs);
      for (const Job *j : sortedJobs)
        noteBuilding(j, /*willBeBuilding=*/true, reason);
    }

    const Job *findUnfinishedJob(ArrayRef<const Job *> JL) {
      for (const Job *Cmd : JL) {
        if (!FinishedCommands.count(Cmd))
          return Cmd;
      }
      return nullptr;
    }

    /// Schedule the given Job if it has not been scheduled and if all of
    /// its inputs are in FinishedCommands.
    void scheduleCommandIfNecessaryAndPossible(const Job *Cmd) {
      if (ScheduledCommands.count(Cmd)) {
        if (Comp.getShowJobLifecycle()) {
          llvm::outs() << "Already scheduled: " << LogJob(Cmd) << "\n";
        }
        return;
      }

      if (auto Blocking = findUnfinishedJob(Cmd->getInputs())) {
        BlockingCommands[Blocking].push_back(Cmd);
        if (Comp.getShowJobLifecycle()) {
          llvm::outs() << "Blocked by: " << LogJob(Blocking)
                       << ", now blocking jobs: "
                       << LogJobArray(BlockingCommands[Blocking]) << "\n";
        }
        return;
      }

#ifndef NDEBUG
      // If we can, assert that no compile jobs are scheduled beyond the second
      // wave. If this assertion fails, it indicates one of:
      // 1) A failure of the driver's job tracing machinery to follow a
      // dependency arc.
      // 2) A failure of the frontend to emit a dependency arc.
      if (isa<CompileJobAction>(Cmd->getSource()) && Cmd->getWave() > 2) {
        llvm_unreachable("Scheduled a command into a third wave!");
      }
#endif

      // Adding to scheduled means we've committed to its completion (not
      // distinguished from skipping). We never remove it once inserted.
      ScheduledCommands.insert(Cmd);

      // Adding to pending means it should be in the next round of additions to
      // the task queue (either batched or singularly); we remove Jobs from
      // PendingExecution once we hand them over to the TaskQueue.
      PendingExecution.insert(Cmd);
    }

    // Sort for ease of testing
    template <typename Jobs>
    void scheduleCommandsInSortedOrder(const Jobs &jobs) {
      llvm::SmallVector<const Job *, 16> sortedJobs;
      Comp.sortJobsToMatchCompilationInputs(jobs, sortedJobs);
      for (const Job *Cmd : sortedJobs)
        scheduleCommandIfNecessaryAndPossible(Cmd);
    }

    void addPendingJobToTaskQueue(const Job *Cmd) {
      // FIXME: Failing here should not take down the whole process.
      bool success =
          writeFilelistIfNecessary(Cmd, Comp.getArgs(), Comp.getDiags());
      assert(success && "failed to write filelist");
      (void)success;

      assert(Cmd->getExtraEnvironment().empty() &&
             "not implemented for compilations with multiple jobs");
      if (Comp.getShowJobLifecycle())
        llvm::outs() << "Added to TaskQueue: " << LogJob(Cmd) << "\n";
      TQ->addTask(Cmd->getExecutable(), Cmd->getArgumentsForTaskExecution(),
                  llvm::None, (void *)Cmd);
    }

    /// When a task finishes, check other Jobs that may be blocked.
    void markFinished(const Job *Cmd, bool Skipped=false) {
      if (Comp.getShowJobLifecycle()) {
        llvm::outs() << "Job "
                     << (Skipped ? "skipped" : "finished")
                     << ": " << LogJob(Cmd) << "\n";
      }
      FinishedCommands.insert(Cmd);
      if (auto *Stats = Comp.getStatsReporter()) {
          auto &D = Stats->getDriverCounters();
          if (Skipped)
            ++D.NumDriverJobsSkipped;
          else
            ++D.NumDriverJobsRun;
      }
      auto BlockedIter = BlockingCommands.find(Cmd);
      if (BlockedIter != BlockingCommands.end()) {
        auto AllBlocked = std::move(BlockedIter->second);
        if (Comp.getShowJobLifecycle()) {
          llvm::outs() << "Scheduling maybe-unblocked jobs: "
                       << LogJobArray(AllBlocked) << "\n";
        }
        BlockingCommands.erase(BlockedIter);
        scheduleCommandsInSortedOrder(AllBlocked);
      }
    }

    bool isBatchJob(const Job *MaybeBatchJob) const {
      return BatchJobs.count(MaybeBatchJob) != 0;
    }

    /// Callback which will be called immediately after a task has started. This
    /// callback may be used to provide output indicating that the task began.
    void taskBegan(ProcessId Pid, void *Context) {
      // TODO: properly handle task began.
      const Job *BeganCmd = (const Job *)Context;

      if (Comp.getShowDriverTimeCompilation()) {
        llvm::SmallString<128> TimerName;
        llvm::raw_svector_ostream OS(TimerName);
        OS << LogJob(BeganCmd);
        DriverTimers.insert({
            BeganCmd,
              std::unique_ptr<llvm::Timer>(
                new llvm::Timer("task", OS.str(), DriverTimerGroup))
              });
        DriverTimers[BeganCmd]->startTimer();
      }

      switch (Comp.getOutputLevel()) {
      case OutputLevel::Normal:
        break;
        // For command line or verbose output, print out each command as it
        // begins execution.
      case OutputLevel::PrintJobs:
        BeganCmd->printCommandLineAndEnvironment(llvm::outs());
        break;
      case OutputLevel::Verbose:
        BeganCmd->printCommandLine(llvm::errs());
        break;
      case OutputLevel::Parseable:
        BeganCmd->forEachContainedJobAndPID(Pid, [&](const Job *J, Job::PID P) {
          auto TascDesc = constructDetailedTaskDescription(*J);
          parseable_output::emitBeganMessage(llvm::errs(),
                                             J->getSource().getClassName(),
                                             TascDesc, P,
                                             TaskProcessInformation(Pid));
        });
        break;
      }
    }

    /// Note that a .swiftdeps file failed to load and take corrective actions:
    /// disable incremental logic and schedule all existing deferred commands.
    void
    dependencyLoadFailed(StringRef DependenciesFile, bool Warn=true) {
      if (Warn && Comp.getShowIncrementalBuildDecisions())
        Comp.getDiags().diagnose(SourceLoc(),
                                 diag::warn_unable_to_load_dependencies,
                                 DependenciesFile);
      Comp.disableIncrementalBuild(
          Twine("malformed swift dependencies file '") + DependenciesFile +
          "'");
    }

    /// Helper that attempts to reload a job's .swiftdeps file after the job
    /// exits, and re-run transitive marking to ensure everything is properly
    /// invalidated by any new dependency edges introduced by it. If reloading
    /// fails, this can cause deferred jobs to be immediately scheduled.

    std::vector<const Job*>
    reloadAndRemarkDeps(const Job *FinishedCmd, int ReturnCode) {
      const CommandOutput &Output = FinishedCmd->getOutput();
      StringRef DependenciesFile =
          Output.getAdditionalOutputForType(file_types::TY_SwiftDeps);

      if (DependenciesFile.empty()) {
        // If this job doesn't track dependencies, it must always be run.
        // Note: In theory CheckDependencies makes sense as well (for a leaf
        // node in the dependency graph), and maybe even NewlyAdded (for very
        // coarse dependencies that always affect downstream nodes), but we're
        // not using either of those right now, and this logic should probably
        // be revisited when we are.
        assert(isa<MergeModuleJobAction>(FinishedCmd->getSource()) ||
               FinishedCmd->getCondition() == Job::Condition::Always);
        return {};
      }
      const bool compileExitedNormally =
          ReturnCode == EXIT_SUCCESS || ReturnCode == EXIT_FAILURE;
      return !compileExitedNormally
                 ? reloadAndRemarkDepsOnAbnormalExit(FinishedCmd)
                 : reloadAndRemarkDepsOnNormalExit(FinishedCmd, /*cmdFailed=*/
                                                   ReturnCode != EXIT_SUCCESS,
                                                   DependenciesFile);
    }

    // If we have a dependency file /and/ the frontend task exited normally,
    // we can be discerning about what downstream files to rebuild.
    std::vector<const Job *>
    reloadAndRemarkDepsOnNormalExit(const Job *FinishedCmd,
                                    const bool cmdFailed,
                                    StringRef DependenciesFile) {
      const auto changedNodes = getFineGrainedDepGraph().loadFromPath(
          FinishedCmd, DependenciesFile, Comp.getDiags());
      const bool loadFailed = !changedNodes;
      if (loadFailed) {
        handleDependenciesReloadFailure(cmdFailed, DependenciesFile);
        return {};
      }
      return getFineGrainedDepGraph()
          .findJobsToRecompileWhenNodesChange(changedNodes.value());
    }

    void handleDependenciesReloadFailure(const bool cmdFailed,
                                         const StringRef DependenciesFile) {
      if (cmdFailed) {
        // let the next build handle it.
        return;
      }
      dependencyLoadFailed(DependenciesFile);
      // Better try compiling whatever was waiting on more info.
      for (const Job *Cmd : DeferredCommands)
        scheduleCommandIfNecessaryAndPossible(Cmd);
      DeferredCommands.clear();
    };

    std::vector<const Job *>
    reloadAndRemarkDepsOnAbnormalExit(const Job *FinishedCmd) {
      // If there's an abnormal exit (a crash), assume the worst.
      switch (FinishedCmd->getCondition()) {
      case Job::Condition::NewlyAdded:
        // The job won't be treated as newly added next time. Conservatively
        // mark it as affecting other jobs, because some of them may have
        // completed already.
        return findJobsToRecompileWhenWholeJobChanges(FinishedCmd);
      case Job::Condition::Always:
        // Any incremental task that shows up here has already been marked;
        // we didn't need to wait for it to finish to start downstream
        // tasks.
        break;
      case Job::Condition::RunWithoutCascading:
        // If this file changed, it might have been a non-cascading change
        // and it might not. Unfortunately, the interface hash has been
        // updated or compromised, so we don't actually know anymore; we
        // have to conservatively assume the changes could affect other
        // files.
        return findJobsToRecompileWhenWholeJobChanges(FinishedCmd);

      case Job::Condition::CheckDependencies:
        // If the only reason we're running this is because something else
        // changed, then we can trust the dependency graph as to whether
        // it's a cascading or non-cascading change. That is, if whatever
        // /caused/ the error isn't supposed to affect other files, and
        // whatever /fixes/ the error isn't supposed to affect other files,
        // then there's no need to recompile any other inputs. If either of
        // those are false, we /do/ need to recompile other inputs.
        break;
      }
      return {};
    }

    /// Check to see if a job produced a zero-length serialized diagnostics
    /// file, which is used to indicate batch-constituents that were batched
    /// together with a failing constituent but did not, themselves, produce any
    /// errors.
    bool jobWasBatchedWithFailingJobs(const Job *J) const {
      auto DiaPath =
        J->getOutput().getAnyOutputForType(file_types::TY_SerializedDiagnostics);
      if (DiaPath.empty())
        return false;
      if (!llvm::sys::fs::is_regular_file(DiaPath))
        return false;
      uint64_t Size;
      auto EC = llvm::sys::fs::file_size(DiaPath, Size);
      if (EC)
        return false;
      return Size == 0;
    }

    /// If a batch-constituent job happens to be batched together with a job
    /// that exits with an error, the batch-constituent may be considered
    /// "cancelled".
    bool jobIsCancelledBatchConstituent(int ReturnCode,
                                        const Job *ContainerJob,
                                        const Job *ConstituentJob) {
      return ReturnCode != 0 &&
        isBatchJob(ContainerJob) &&
        jobWasBatchedWithFailingJobs(ConstituentJob);
    }

    /// Unpack a \c BatchJob that has finished into its constituent \c Job
    /// members, and call \c taskFinished on each, propagating any \c
    /// TaskFinishedResponse other than \c
    /// TaskFinishedResponse::ContinueExecution from any of the constituent
    /// calls.
    TaskFinishedResponse
    unpackAndFinishBatch(int ReturnCode, StringRef Output,
                         StringRef Errors, const BatchJob *B) {
      if (Comp.getShowJobLifecycle())
        llvm::outs() << "Batch job finished: " << LogJob(B) << "\n";
      auto res = TaskFinishedResponse::ContinueExecution;
      for (const Job *J : B->getCombinedJobs()) {
        if (Comp.getShowJobLifecycle())
          llvm::outs() << "  ==> Unpacked batch constituent finished: "
                       << LogJob(J) << "\n";
        auto r = taskFinished(
            llvm::sys::ProcessInfo::InvalidPid, ReturnCode, Output, Errors,
            TaskProcessInformation(llvm::sys::ProcessInfo::InvalidPid),
            (void *)J);
        if (r != TaskFinishedResponse::ContinueExecution)
          res = r;
      }
      return res;
    }

    void
    emitParseableOutputForEachFinishedJob(ProcessId Pid, int ReturnCode,
                                          StringRef Output,
                                          const Job *FinishedCmd,
                                          TaskProcessInformation ProcInfo) {
      FinishedCmd->forEachContainedJobAndPID(Pid, [&](const Job *J,
                                                      Job::PID P) {
        if (jobIsCancelledBatchConstituent(ReturnCode, FinishedCmd, J)) {
          // Simulate SIGINT-interruption to parseable-output consumer for any
          // constituent of a failing batch job that produced no errors of its
          // own.
          parseable_output::emitSignalledMessage(llvm::errs(),
                                                 J->getSource().getClassName(),
                                                 "cancelled batch constituent",
                                                 "", SIGINT, P, ProcInfo);
        } else {
          parseable_output::emitFinishedMessage(llvm::errs(),
                                                J->getSource().getClassName(),
                                                Output.str(), ReturnCode,
                                                P, ProcInfo);
        }
      });
    }

    /// Callback which will be called immediately after a task has finished
    /// execution. Determines if execution should continue, and also schedule
    /// any additional Jobs which we now know we need to run.
    TaskFinishedResponse taskFinished(ProcessId Pid, int ReturnCode,
                                      StringRef Output, StringRef Errors,
                                      TaskProcessInformation ProcInfo,
                                      void *Context) {
      const Job *const FinishedCmd = (const Job *)Context;

      if (Pid != llvm::sys::ProcessInfo::InvalidPid) {

        if (Comp.getShowDriverTimeCompilation()) {
          DriverTimers[FinishedCmd]->stopTimer();
        }
        processOutputOfFinishedProcess(Pid, ReturnCode, FinishedCmd, Output,
                                       ProcInfo);
      }

      if (Comp.getStatsReporter() && ProcInfo.getResourceUsage().has_value())
        Comp.getStatsReporter()->recordJobMaxRSS(
            ProcInfo.getResourceUsage()->Maxrss);

      if (isBatchJob(FinishedCmd)) {
        return unpackAndFinishBatch(ReturnCode, Output, Errors,
                                    static_cast<const BatchJob *>(FinishedCmd));
      }

      CommandSet DependentsInEffect =
            subsequentJobsNeeded(FinishedCmd, ReturnCode);

      if (ReturnCode != EXIT_SUCCESS)
        return taskFailed(FinishedCmd, ReturnCode);

      // When a task finishes, we need to reevaluate the other commands that
      // might have been blocked.
      markFinished(FinishedCmd);

      noteBuildingJobs(DependentsInEffect,
                       "because of dependencies discovered later");

      scheduleCommandsInSortedOrder(DependentsInEffect);
      for (const Job *Cmd : DependentsInEffect) {
        if (DeferredCommands.erase(Cmd)) {
#ifndef NDEBUG
          if (isa<CompileJobAction>(FinishedCmd->getSource()))
            Cmd->setWave(FinishedCmd->getWave() + 1);
#else
          continue;
#endif
        }
      }
      return TaskFinishedResponse::ContinueExecution;
    }

    TaskFinishedResponse taskFailed(const Job *FinishedCmd,
                                    const int ReturnCode) {
      // The task failed, so return true without performing any further
      // dependency analysis.

      // Store this task's ReturnCode as our Result if we haven't stored
      // anything yet.

      if (ResultCode == EXIT_SUCCESS)
        ResultCode = ReturnCode;

      if (!isa<CompileJobAction>(FinishedCmd->getSource()) ||
          ReturnCode != EXIT_FAILURE) {
        Comp.getDiags().diagnose(SourceLoc(), diag::error_command_failed,
                                 FinishedCmd->getSource().getClassName(),
                                 ReturnCode);
      }

      // See how ContinueBuildingAfterErrors gets set up in Driver.cpp for
      // more info.
      assert((Comp.getContinueBuildingAfterErrors() ||
              !Comp.getBatchModeEnabled()) &&
             "batch mode diagnostics require ContinueBuildingAfterErrors");

      return Comp.getContinueBuildingAfterErrors()
                 ? TaskFinishedResponse::ContinueExecution
                 : TaskFinishedResponse::StopExecution;
    }

#if defined(_WIN32)
    struct FileBinaryModeRAII {
      FileBinaryModeRAII(FILE *F) : F(F) {
        PrevMode = _setmode(_fileno(F), _O_BINARY);
      }
      ~FileBinaryModeRAII() {
        _setmode(_fileno(F), PrevMode);
      }
      FILE *F;
      int PrevMode;
    };
#else
    struct FileBinaryModeRAII {
      FileBinaryModeRAII(FILE *) {}
    };
#endif

    void processOutputOfFinishedProcess(ProcessId Pid, int ReturnCode,
                                        const Job *const FinishedCmd,
                                        StringRef Output,
                                        TaskProcessInformation ProcInfo) {
      switch (Comp.getOutputLevel()) {
      case OutputLevel::PrintJobs:
        // Only print the jobs, not the outputs
        break;
      case OutputLevel::Normal:
      case OutputLevel::Verbose:
        // Send the buffered output to stderr, though only if we
        // support getting buffered output.
        if (TaskQueue::supportsBufferingOutput()) {
          // Temporarily change stderr to binary mode to avoid double
          // LF -> CR LF conversions on the outputs from child
          // processes, which have already this conversion appplied.
          // This makes a difference only for Windows.
          FileBinaryModeRAII F(stderr);
          llvm::errs() << Output;
        }
        break;
      case OutputLevel::Parseable:
        emitParseableOutputForEachFinishedJob(Pid, ReturnCode, Output,
                                              FinishedCmd, ProcInfo);
        break;
      }
    }

    /// In order to handle both old dependencies that have disappeared and new
    /// dependencies that have arisen, we need to reload the dependency file.
    /// Do this whether or not the build succeeded.
    ///
    /// FIXME: too much global state floating around, e.g.
    /// getIncrementalBuildEnabled
    CommandSet subsequentJobsNeeded(const Job *FinishedCmd,
                                    const int ReturnCode) {
      if (!Comp.getIncrementalBuildEnabled())
        return {};
      auto Dependents = reloadAndRemarkDeps(FinishedCmd, ReturnCode);
      CommandSet DepSet;
      for (const Job *Cmd : Dependents)
        DepSet.insert(Cmd);
      return DepSet;
    }

    TaskFinishedResponse taskSignalled(ProcessId Pid, StringRef ErrorMsg,
                                       StringRef Output, StringRef Errors,
                                       void *Context, Optional<int> Signal,
                                       TaskProcessInformation ProcInfo) {
      const Job *SignalledCmd = (const Job *)Context;

      if (Comp.getShowDriverTimeCompilation()) {
        DriverTimers[SignalledCmd]->stopTimer();
      }

      if (Comp.getOutputLevel() == OutputLevel::Parseable) {
        // Parseable output was requested.
        SignalledCmd->forEachContainedJobAndPID(Pid, [&](const Job *J,
                                                         Job::PID P) {
          parseable_output::emitSignalledMessage(llvm::errs(),
                                                 J->getSource().getClassName(),
                                                 ErrorMsg, Output, Signal, P,
                                                 ProcInfo);
        });
      } else {
        // Otherwise, send the buffered output to stderr, though only if we
        // support getting buffered output.
        if (TaskQueue::supportsBufferingOutput())
          llvm::errs() << Output;
      }

      if (Comp.getStatsReporter() && ProcInfo.getResourceUsage().has_value())
        Comp.getStatsReporter()->recordJobMaxRSS(
            ProcInfo.getResourceUsage()->Maxrss);

      if (!ErrorMsg.empty())
        Comp.getDiags().diagnose(SourceLoc(),
                                 diag::error_unable_to_execute_command,
                                 ErrorMsg);

      if (Signal.has_value()) {
        Comp.getDiags().diagnose(SourceLoc(), diag::error_command_signalled,
                                 SignalledCmd->getSource().getClassName(),
                                 Signal.value());
      } else {
        Comp.getDiags()
            .diagnose(SourceLoc(),
                      diag::error_command_signalled_without_signal_number,
                      SignalledCmd->getSource().getClassName());
      }

      // Since the task signalled, unconditionally set result to -2.
      ResultCode = -2;
      AnyAbnormalExit = true;

      return TaskFinishedResponse::StopExecution;
    }

  public:
    PerformJobsState(Compilation &Comp, std::unique_ptr<TaskQueue> &&TaskQueue)
        : Comp(Comp),
          FineGrainedDepGraph(
              Comp.getVerifyFineGrainedDependencyGraphAfterEveryImport(),
              Comp.getEmitFineGrainedDependencyDotFileAfterEveryImport(),
              Comp.getTraceDependencies(),
              Comp.getStatsReporter()),
          TQ(std::move(TaskQueue)) {}

    /// Schedule and run initial, additional, and batch jobs.
    void runJobs() {
      scheduleJobsBeforeBatching();
      formBatchJobsAndAddPendingJobsToTaskQueue();
      runTaskQueueToCompletion();
      checkUnfinishedJobs();
    }

  private:
    void scheduleJobsBeforeBatching() {
      if (Comp.getIncrementalBuildEnabled())
        scheduleFirstRoundJobsForIncrementalCompilation();
      else
        scheduleJobsForNonIncrementalCompilation();
    }

    void scheduleJobsForNonIncrementalCompilation() {
      for (const Job *Cmd : Comp.getJobs())
        scheduleCommandIfNecessaryAndPossible(Cmd);
    }

    void scheduleFirstRoundJobsForIncrementalCompilation() {

      CommandSet compileJobsToSchedule =
          computeFirstRoundCompileJobsForIncrementalCompilation();

      for (const Job *Cmd : Comp.getJobs()) {
        if (!isa<IncrementalJobAction>(Cmd->getSource()) ||
            compileJobsToSchedule.count(Cmd)) {
          scheduleCommandIfNecessaryAndPossible(Cmd);
          noteBuilding(Cmd, /*willBeBuilding*/ true, "");
        } else {
          DeferredCommands.insert(Cmd);
          noteBuilding(Cmd, /*willBeBuilding*/ false, "");
        }
      }
    }

    /// Figure out the best strategy and return those jobs. May return
    /// duplicates.
    CommandSet computeFirstRoundCompileJobsForIncrementalCompilation() {
      auto getEveryCompileJob = [&] {
        CommandSet everyIncrementalJob;
        for (const Job *Cmd : Comp.getJobs()) {
          if (isa<IncrementalJobAction>(Cmd->getSource()))
            everyIncrementalJob.insert(Cmd);
        }
        return everyIncrementalJob;
      };

      bool sawModuleWrapJob = false;
      const Job *mergeModulesJob = nullptr;
      CommandSet jobsToSchedule;
      CommandSet initialCascadingCommands;
      for (const Job *cmd : Comp.getJobs()) {
        // A modulewrap job consumes the output of merge-modules. If it is
        // in the queue, we must run merge-modules or empty temporary files
        // will be consumed by the job instead.
        // FIXME: We should be able to ditch this if we compare the timestamps
        // of the temporary file to the build record, if it exists.
        sawModuleWrapJob |= isa<ModuleWrapJobAction>(cmd->getSource());

        // Skip jobs that have no associated incremental info.
        if (!isa<IncrementalJobAction>(cmd->getSource())) {
          continue;
        }

        if (isa<MergeModuleJobAction>(cmd->getSource())) {
          assert(!mergeModulesJob && "multiple scheduled merge-modules jobs?");
          mergeModulesJob = cmd;
        }

        const Optional<std::pair<bool, bool>> shouldSchedAndIsCascading =
            computeShouldInitiallyScheduleJobAndDependents(cmd);
        if (!shouldSchedAndIsCascading)
          return getEveryCompileJob(); // Load error, just run them all
        const bool &shouldSchedule = shouldSchedAndIsCascading->first;
        const bool &isCascading = shouldSchedAndIsCascading->second;
        if (shouldSchedule)
          jobsToSchedule.insert(cmd);
        if (isCascading)
          initialCascadingCommands.insert(cmd);
      }
      for (const auto *cmd : collectCascadedJobsFromDependencyGraph(
               initialCascadingCommands))
        jobsToSchedule.insert(cmd);
      for (const auto cmd :
           collectExternallyDependentJobsFromDependencyGraph())
        jobsToSchedule.insert(cmd);

      // The merge-modules job is special: it *must* be scheduled if any other
      // job has been scheduled because any other job can influence the
      // structure of the resulting module. Additionally, the initial scheduling
      // predicate above is only aware of intra-module changes. External
      // dependencies changing *must* cause merge-modules to be scheduled.
      if ((!jobsToSchedule.empty() || sawModuleWrapJob) && mergeModulesJob) {
        jobsToSchedule.insert(mergeModulesJob);
      }
      return jobsToSchedule;
    }

    /// Return whether \p Cmd should be scheduled when using dependencies, and if
    /// the job is cascading. Or if there was a dependency-read error, return
    /// \c None to indicate don't-know.
    Optional<std::pair<bool, bool>>
    computeShouldInitiallyScheduleJobAndDependents(const Job *Cmd) {
      auto CondAndHasDepsIfNoError =
          loadDependenciesAndComputeCondition(Cmd);
      if (!CondAndHasDepsIfNoError)
        return None; // swiftdeps read error, abandon dependencies

      Job::Condition Cond;
      bool HasDependenciesFileName;
      std::tie(Cond, HasDependenciesFileName) =
          CondAndHasDepsIfNoError.value();

      const bool shouldSched = shouldScheduleCompileJobAccordingToCondition(
          Cmd, Cond, HasDependenciesFileName);

      const bool isCascading = isCascadingJobAccordingToCondition(
          Cmd, Cond, HasDependenciesFileName);
      return std::make_pair(shouldSched, isCascading);
    }

    /// Returns job condition, and whether a dependency file was specified.
    /// But returns None if there was a dependency read error.
    Optional<std::pair<Job::Condition, bool>>
    loadDependenciesAndComputeCondition(const Job *const Cmd) {
      // merge-modules Jobs do not have .swiftdeps files associated with them,
      // however, their compilation condition is computed as a function of their
      // inputs, so their condition can be used as normal.
      if (isa<MergeModuleJobAction>(Cmd->getSource())) {
        return std::make_pair(Cmd->getCondition(), true);
      }

      // Try to load the dependencies file for this job. If there isn't one, we
      // always have to run the job, but it doesn't affect any other jobs. If
      // there should be one but it's not present or can't be loaded, we have to
      // run all the jobs.
      // FIXME: We can probably do better here!

      const StringRef DependenciesFile =
          Cmd->getOutput().getAdditionalOutputForType(file_types::TY_SwiftDeps);
      if (DependenciesFile.empty())
        return std::make_pair(Job::Condition::Always, false);
      if (Cmd->getCondition() == Job::Condition::NewlyAdded) {
        registerJobToDepGraph(Cmd);
        return std::make_pair(Job::Condition::NewlyAdded, true);
      }
      const bool depGraphLoadError =
          loadDepGraphFromPath(Cmd, DependenciesFile);
      if (depGraphLoadError) {
        dependencyLoadFailed(DependenciesFile, /*Warn=*/true);
        return None;
      }
      return std::make_pair(Cmd->getCondition(), true);
    }

    bool shouldScheduleCompileJobAccordingToCondition(
        const Job *const Cmd, const Job::Condition Condition,
        const bool hasDependenciesFileName) {

      switch (Condition) {
      case Job::Condition::Always:
      case Job::Condition::NewlyAdded:
        if (Comp.getIncrementalBuildEnabled() && hasDependenciesFileName) {
          // No need to do anything since after this jos is run and its
          // dependencies reloaded, they will show up as changed nodes
        }
        LLVM_FALLTHROUGH;
      case Job::Condition::RunWithoutCascading:
        noteBuilding(Cmd, /*willBeBuilding=*/true,
                     "(initial)");
        return true;
      case Job::Condition::CheckDependencies:
        noteBuilding(Cmd, /*willBeBuilding=*/false,
                     "file is up-to-date and output exists");
        return false;
      }
      llvm_unreachable("invalid job condition");
    }

    bool isCascadingJobAccordingToCondition(
        const Job *const Cmd, const Job::Condition Condition,
        const bool hasDependenciesFileName) const {
      switch (Condition) {
      case Job::Condition::Always:
      case Job::Condition::NewlyAdded:
        return Comp.getIncrementalBuildEnabled() && hasDependenciesFileName;
      case Job::Condition::RunWithoutCascading:
      case Job::Condition::CheckDependencies:
        return false;
      }
      llvm_unreachable("invalid job condition");
    }

    void forEachOutOfDateExternalDependency(
        function_ref<void(StringRef)> consumeExternalSwiftDeps) {
      for (StringRef dependency : getExternalDependencies()) {
        // If the dependency has been modified since the oldest built file,
        // or if we can't stat it for some reason (perhaps it's been
        // deleted?), trigger rebuilds through the dependency graph.
        llvm::sys::fs::file_status depStatus;
        if (llvm::sys::fs::status(dependency, depStatus) ||
            Comp.getLastBuildTime() < depStatus.getLastModificationTime())
          consumeExternalSwiftDeps(dependency);
      }
    }

    CommandSet collectCascadedJobsFromDependencyGraph(
        const CommandSet &InitialCascadingCommands) {
      CommandSet CascadedJobs;
      // We scheduled all of the files that have actually changed. Now add the
      // files that haven't changed, so that they'll get built in parallel if
      // possible and after the first set of files if it's not.
      for (auto *Cmd : InitialCascadingCommands) {
        for (const auto *transitiveCmd : findJobsToRecompileWhenWholeJobChanges(
                 Cmd))
          CascadedJobs.insert(transitiveCmd);
      }
      noteBuildingJobs(CascadedJobs, "because of the initial set");
      return CascadedJobs;
    }

    /// Return jobs dependent on other modules, and jobs dependent on those jobs
    SmallVector<const Job *, 16>
    collectExternallyDependentJobsFromDependencyGraph() {
      SmallVector<const Job *, 16> ExternallyDependentJobs;
      // Check all cross-module dependencies as well.
      forEachOutOfDateExternalDependency([&](StringRef dependency) {
        // If the dependency has been modified since the oldest built file,
        // or if we can't stat it for some reason (perhaps it's been
        // deleted?), trigger rebuilds through the dependency graph.
        for (const Job * marked: markExternalInDepGraph(dependency))
          ExternallyDependentJobs.push_back(marked);
      });
      noteBuildingJobs(ExternallyDependentJobs,
                       "because of external dependencies");
      return ExternallyDependentJobs;
    }

    /// Insert all jobs in \p Cmds (of descriptive name \p Kind) to the \c
    /// TaskQueue, and clear \p Cmds.
    template <typename Container>
    void transferJobsToTaskQueue(Container &Cmds, StringRef Kind) {
      for (const Job *Cmd : Cmds) {
        if (Comp.getShowJobLifecycle())
          llvm::outs() << "Adding " << Kind
                       << " job to task queue: "
                       << LogJob(Cmd) << "\n";
        addPendingJobToTaskQueue(Cmd);
      }
      Cmds.clear();
    }

    /// Partition the jobs in \c PendingExecution into those that are \p
    /// Batchable and those that are \p NonBatchable, clearing \p
    /// PendingExecution.
    void getPendingBatchableJobs(CommandSetVector &Batchable,
                                 CommandSetVector &NonBatchable) {
      for (const Job *Cmd : PendingExecution) {
        if (Comp.getToolChain().jobIsBatchable(Comp, Cmd)) {
          if (Comp.getShowJobLifecycle())
            llvm::outs() << "Batchable: " << LogJob(Cmd) << "\n";
          Batchable.insert(Cmd);
        } else {
          if (Comp.getShowJobLifecycle())
            llvm::outs() << "Not batchable: " << LogJob(Cmd) << "\n";
          NonBatchable.insert(Cmd);
        }
      }
    }

    /// If \p Batch is nonempty, construct a new \c BatchJob from its
    /// contents by calling \p ToolChain::constructBatchJob, then insert the
    /// new \c BatchJob into \p Batches.
    void
    formBatchJobFromPartitionBatch(std::vector<const Job *> &Batches,
                                   std::vector<const Job *> const &Batch) {
      if (Batch.empty())
        return;
      if (Comp.getShowJobLifecycle())
        llvm::outs() << "Forming batch job from "
                     << Batch.size() << " constituents\n";
      auto const &TC = Comp.getToolChain();
      auto J = TC.constructBatchJob(Batch, NextBatchQuasiPID, Comp);
      if (J)
        Batches.push_back(Comp.addJob(std::move(J)));
    }

    /// Build a vector of partition indices, one per Job: the i'th index says
    /// which batch of the partition the i'th Job will be assigned to. If we are
    /// shuffling due to -driver-batch-seed, the returned indices will not be
    /// arranged in contiguous runs. We shuffle partition-indices here, not
    /// elements themselves, to preserve the invariant that each batch is a
    /// subsequence of the full set of inputs, not just a subset.
    std::vector<size_t>
    assignJobsToPartitions(size_t PartitionSize,
                           size_t NumJobs) {
      size_t Remainder = NumJobs % PartitionSize;
      size_t TargetSize = NumJobs / PartitionSize;
      std::vector<size_t> PartitionIndex;
      PartitionIndex.reserve(NumJobs);
      for (size_t P = 0; P < PartitionSize; ++P) {
        // Spread remainder evenly across partitions by adding 1 to the target
        // size of the first Remainder of them.
        size_t FillCount = TargetSize + ((P < Remainder) ? 1 : 0);
        std::fill_n(std::back_inserter(PartitionIndex), FillCount, P);
      }
      if (Comp.getBatchSeed() != 0) {
        std::minstd_rand gen(Comp.getBatchSeed());
        std::shuffle(PartitionIndex.begin(), PartitionIndex.end(), gen);
      }
      assert(PartitionIndex.size() == NumJobs);
      return PartitionIndex;
    }

    /// Create \c NumberOfParallelCommands batches and assign each job to a
    /// batch either filling each partition in order or, if seeded with a
    /// nonzero value, pseudo-randomly (but deterministically and nearly-evenly).
    void partitionIntoBatches(std::vector<const Job *> Batchable,
                              BatchPartition &Partition) {
      if (Comp.getShowJobLifecycle()) {
        llvm::outs() << "Found " << Batchable.size() << " batchable jobs\n";
        llvm::outs() << "Forming into " << Partition.size() << " batches\n";
      }

      assert(!Partition.empty());
      auto PartitionIndex = assignJobsToPartitions(Partition.size(),
                                                   Batchable.size());
      assert(PartitionIndex.size() == Batchable.size());
      auto const &TC = Comp.getToolChain();
      for_each(Batchable, PartitionIndex, [&](const Job *Cmd, size_t Idx) {
          assert(Idx < Partition.size());
          std::vector<const Job*> &P = Partition[Idx];
          if (P.empty() || TC.jobsAreBatchCombinable(Comp, P[0], Cmd)) {
            if (Comp.getShowJobLifecycle())
              llvm::outs() << "Adding " << LogJob(Cmd)
                           << " to batch " << Idx << '\n';
            P.push_back(Cmd);
          } else {
            // Strange but theoretically possible that we have a batchable job
            // that's not combinable with others; tack a new batch on for it.
            if (Comp.getShowJobLifecycle())
              llvm::outs() << "Adding " << LogJob(Cmd)
                           << " to new batch " << Partition.size() << '\n';
            Partition.push_back(std::vector<const Job*>());
            Partition.back().push_back(Cmd);
          }
        });
    }

    // Selects the number of partitions based on the user-provided batch
    // count and/or the number of parallel tasks we can run, subject to a
    // fixed per-batch safety cap, to avoid overcommitting memory.
    size_t pickNumberOfPartitions() {

      // If the user asked for something, use that.
      if (Comp.getBatchCount().has_value())
        return Comp.getBatchCount().value();

      // This is a long comment to justify a simple calculation.
      //
      // Because there is a secondary "outer" build system potentially also
      // scheduling multiple drivers in parallel on separate build targets
      // -- while we, the driver, schedule our own subprocesses -- we might
      // be creating up to $NCPU^2 worth of _memory pressure_.
      //
      // Oversubscribing CPU is typically no problem these days, but
      // oversubscribing memory can lead to paging, which on modern systems
      // is quite bad.
      //
      // In practice, $NCPU^2 processes doesn't _quite_ happen: as core
      // count rises, it usually exceeds the number of large targets
      // without any dependencies between them (which are the only thing we
      // have to worry about): you might have (say) 2 large independent
      // modules * 2 architectures, but that's only an $NTARGET value of 4,
      // which is much less than $NCPU if you're on a 24 or 36-way machine.
      //
      //  So the actual number of concurrent processes is:
      //
      //     NCONCUR := $NCPU * min($NCPU, $NTARGET)
      //
      // Empirically, a frontend uses about 512kb RAM per non-primary file
      // and about 10mb per primary. The number of non-primaries per
      // process is a constant in a given module, but the number of
      // primaries -- the "batch size" -- is inversely proportional to the
      // batch count (default: $NCPU). As a result, the memory pressure
      // we can expect is:
      //
      //  $NCONCUR * (($NONPRIMARYMEM * $NFILE) +
      //              ($PRIMARYMEM * ($NFILE/$NCPU)))
      //
      // If we tabulate this across some plausible values, we see
      // unfortunate memory-pressure results:
      //
      //                          $NFILE
      //                  +---------------------
      //  $NTARGET $NCPU  |  100    500    1000
      //  ----------------+---------------------
      //     2        2   |  2gb   11gb    22gb
      //     4        4   |  4gb   24gb    48gb
      //     4        8   |  5gb   28gb    56gb
      //     4       16   |  7gb   36gb    72gb
      //     4       36   | 11gb   56gb   112gb
      //
      // As it happens, the lower parts of the table are dominated by
      // number of processes rather than the files-per-batch (the batches
      // are already quite small due to the high core count) and the left
      // side of the table is dealing with modules too small to worry
      // about. But the middle and upper-right quadrant is problematic: 4
      // and 8 core machines do not typically have 24-48gb of RAM, it'd be
      // nice not to page on them when building a 4-target project with
      // 500-file modules.
      //
      // Turns we can do that if we just cap the batch size statically at,
      // say, 25 files per batch, we get a better formula:
      //
      //  $NCONCUR * (($NONPRIMARYMEM * $NFILE) +
      //              ($PRIMARYMEM * min(25, ($NFILE/$NCPU))))
      //
      //                          $NFILE
      //                  +---------------------
      //  $NTARGET $NCPU  |  100    500    1000
      //  ----------------+---------------------
      //     2        2   |  1gb    2gb     3gb
      //     4        4   |  4gb    8gb    12gb
      //     4        8   |  5gb   16gb    24gb
      //     4       16   |  7gb   32gb    48gb
      //     4       36   | 11gb   56gb   108gb
      //
      // This means that the "performance win" of batch mode diminishes
      // slightly: the batching factor in the equation drops from
      // ($NFILE/$NCPU) to min(25, $NFILE/$NCPU). In practice this seems to
      // not cost too much: the additional factor in number of subprocesses
      // run is the following:
      //
      //                          $NFILE
      //                  +---------------------
      //  $NTARGET $NCPU  |  100    500    1000
      //  ----------------+---------------------
      //     2        2   |  2x    10x      20x
      //     4        4   |   -     5x      10x
      //     4        8   |   -   2.5x       5x
      //     4       16   |   -  1.25x     2.5x
      //     4       36   |   -      -     1.1x
      //
      // Where - means "no difference" because the batches were already
      // smaller than 25.
      //
      // Even in the worst case here, the 1000-file module on 2-core
      // machine is being built with only 40 subprocesses, rather than the
      // pre-batch-mode 1000. I.e. it's still running 96% fewer
      // subprocesses than before. And significantly: it's doing so while
      // not exceeding the RAM of a typical 2-core laptop.

      // An explanation of why the partition calculation isn't integer division.
      // Using an example, a module of 26 files exceeds the limit of 25 and must
      // be compiled in 2 batches. Integer division yields 26/25 = 1 batch, but
      // a single batch of 26 exceeds the limit. The calculation must round up,
      // which can be calculated using: `(x + y - 1) / y`
      auto DivideRoundingUp = [](size_t Num, size_t Div) -> size_t {
        return (Num + Div - 1) / Div;
      };

      size_t DefaultSizeLimit = 25;
      size_t NumTasks = TQ->getNumberOfParallelTasks();
      size_t NumFiles = PendingExecution.size();
      size_t SizeLimit = Comp.getBatchSizeLimit().value_or(DefaultSizeLimit);
      return std::max(NumTasks, DivideRoundingUp(NumFiles, SizeLimit));
    }

    /// Select jobs that are batch-combinable from \c PendingExecution, combine
    /// them together into \p BatchJob instances (also inserted into \p
    /// BatchJobs), and enqueue all \c PendingExecution jobs (whether batched or
    /// not) into the \c TaskQueue for execution.
    void formBatchJobsAndAddPendingJobsToTaskQueue() {

      // If batch mode is not enabled, just transfer the set of pending jobs to
      // the task queue, as-is.
      if (!Comp.getBatchModeEnabled()) {
        transferJobsToTaskQueue(PendingExecution, "standard");
        return;
      }

      size_t NumPartitions = pickNumberOfPartitions();
      CommandSetVector Batchable, NonBatchable;
      std::vector<const Job *> Batches;

      // Split the batchable from non-batchable pending jobs.
      getPendingBatchableJobs(Batchable, NonBatchable);

      // Partition the batchable jobs into sets.
      BatchPartition Partition(NumPartitions);
      partitionIntoBatches(Batchable.takeVector(), Partition);

      // Construct a BatchJob from each batch in the partition.
      for (auto const &Batch : Partition) {
        formBatchJobFromPartitionBatch(Batches, Batch);
      }

      PendingExecution.clear();

      // Save batches so we can locate and decompose them on task-exit.
      for (const Job *Cmd : Batches)
        BatchJobs.insert(Cmd);

      // Enqueue the resulting jobs, batched and non-batched alike.
      transferJobsToTaskQueue(Batches, "batch");
      transferJobsToTaskQueue(NonBatchable, "non-batch");
    }

    void runTaskQueueToCompletion() {
      do {
        using namespace std::placeholders;
        // Ask the TaskQueue to execute.
        if (TQ->execute(std::bind(&PerformJobsState::taskBegan, this, _1, _2),
                        std::bind(&PerformJobsState::taskFinished, this, _1, _2,
                                  _3, _4, _5, _6),
                        std::bind(&PerformJobsState::taskSignalled, this, _1,
                                  _2, _3, _4, _5, _6, _7))) {
          if (ResultCode == EXIT_SUCCESS) {
            // FIXME: Error from task queue while Result == EXIT_SUCCESS most
            // likely means some fork/exec or posix_spawn failed; TaskQueue saw
            // "an error" at some stage before even calling us with a process
            // exit / signal (or else a poll failed); unfortunately the task
            // causing it was dropped on the floor and we have no way to recover
            // it here, so we report a very poor, generic error.
            Comp.getDiags().diagnose(SourceLoc(),
                                     diag::error_unable_to_execute_command,
                                     "<unknown>");
            ResultCode = -2;
            AnyAbnormalExit = true;
            return;
          }
        }

        // Returning without error from TaskQueue::execute should mean either an
        // empty TaskQueue or a failed subprocess.
        assert(!(ResultCode == 0 && TQ->hasRemainingTasks()));

        // Task-exit callbacks from TaskQueue::execute may have unblocked jobs,
        // which means there might be PendingExecution jobs to enqueue here. If
        // there are, we need to continue trying to make progress on the
        // TaskQueue before we start marking deferred jobs as skipped, below.
        if (!PendingExecution.empty() && ResultCode == 0) {
          formBatchJobsAndAddPendingJobsToTaskQueue();
          continue;
        }

        // If we got here, all the queued and pending work we know about is
        // done; mark anything still in deferred state as skipped.
        for (const Job *Cmd : DeferredCommands) {
          if (Comp.getOutputLevel() == OutputLevel::Parseable) {
            // Provide output indicating this command was skipped if parseable
            // output was requested.
            auto TaskDesc = constructDetailedTaskDescription(*Cmd);
            parseable_output::emitSkippedMessage(llvm::errs(),
                                                 Cmd->getSource().getClassName(),
                                                 TaskDesc);
          }
          ScheduledCommands.insert(Cmd);
          markFinished(Cmd, /*Skipped=*/true);
        }
        DeferredCommands.clear();

        // It's possible that by marking some jobs as skipped, we unblocked
        // some jobs and thus have entries in PendingExecution again; push
        // those through to the TaskQueue.
        formBatchJobsAndAddPendingJobsToTaskQueue();

        // If we added jobs to the TaskQueue, and we are not in an error state,
        // we want to give the TaskQueue another run.
      } while (ResultCode == 0 && TQ->hasRemainingTasks());
    }

    void checkUnfinishedJobs() {
      if (ResultCode == 0) {
        assert(BlockingCommands.empty() &&
               "some blocking commands never finished properly");
      } else {
        // Make sure we record any files that still need to be rebuilt.
        for (const Job *Cmd : Comp.getJobs()) {
          // Skip files that don't use dependency analysis.
          bool shouldHaveOutput = false;
          file_types::forEachIncrementalOutputType(
              [&](const file_types::ID type) {
                shouldHaveOutput |=
                    !Cmd->getOutput().getAdditionalOutputForType(type).empty();
              });
          if (!shouldHaveOutput)
            continue;

          // Don't worry about commands that finished or weren't going to run.
          if (FinishedCommands.count(Cmd))
            continue;
          if (!ScheduledCommands.count(Cmd))
            continue;

          const bool needsCascadingBuild =
              computeNeedsCascadingBuildForUnfinishedCommand(Cmd);
          UnfinishedCommands.insert({Cmd, needsCascadingBuild});
        }
      }
    }

    /// When the driver next runs, it will read the build record, and the
    /// unfinished job status will be set to either \c NeedsCascading... or
    /// \c NeedsNonCascading...
    /// Decide which it will be.
    /// As far as I can tell, the only difference the result of this function
    /// makes is how soon
    /// required dependents are recompiled. Here's my reasoning:
    ///
    /// When the driver next runs, the condition will be filtered through
    /// \c loadDependenciesAndComputeCondition .
    /// Then, the cascading predicate is returned from
    /// \c isCompileJobInitiallyNeededForDependencyBasedIncrementalCompilation.
    /// Then, in \c computeShouldInitiallyScheduleJobAndDependents
    /// if the job needs a cascading build, it's dependents will be scheduled
    /// immediately. After the job finishes, it's dependencies will be processed
    /// again. If a non-cascading job failed, the driver will schedule all of
    /// its dependents. (All of its dependents are assumed to have already been
    /// scheduled.) If the job succeeds, the revised dependencies are consulted
    /// to schedule any needed jobs.

    bool computeNeedsCascadingBuildForUnfinishedCommand(const Job *Cmd) {
      if (!Comp.getIncrementalBuildEnabled())
        return true;
      // See the comment on the whole function above
      return false;
    }

  public:
    void populateInputInfoMap(InputInfoMap &inputs) const {
      for (auto &entry : UnfinishedCommands) {
        for (auto *action : entry.first->getSource().getInputs()) {
          auto inputFile = dyn_cast<InputAction>(action);
          if (!inputFile)
            continue;

          CompileJobAction::InputInfo info;
          info.previousModTime = entry.first->getInputModTime();
          info.status = entry.second ?
            CompileJobAction::InputInfo::Status::NeedsCascadingBuild :
            CompileJobAction::InputInfo::Status::NeedsNonCascadingBuild;
          inputs[&inputFile->getInputArg()] = info;
        }
      }

      for (const Job *entry : FinishedCommands) {
        const auto *compileAction = dyn_cast<CompileJobAction>(&entry->getSource());
        if (!compileAction)
          continue;

        for (auto *action : compileAction->getInputs()) {
          auto inputFile = dyn_cast<InputAction>(action);
          if (!inputFile)
            continue;

          CompileJobAction::InputInfo info;
          info.previousModTime = entry->getInputModTime();
          info.status = CompileJobAction::InputInfo::Status::UpToDate;
          inputs[&inputFile->getInputArg()] = info;
        }
      }

      // Sort the entries by input order.
      static_assert(IsTriviallyCopyable<CompileJobAction::InputInfo>::value,
                    "llvm::array_pod_sort relies on trivially-copyable data");
      using InputInfoEntry = std::decay<decltype(inputs.front())>::type;
      llvm::array_pod_sort(inputs.begin(), inputs.end(),
                           [](const InputInfoEntry *lhs,
                              const InputInfoEntry *rhs) -> int {
                             auto lhsIndex = lhs->first->getIndex();
                             auto rhsIndex = rhs->first->getIndex();
                             return (lhsIndex < rhsIndex) ? -1 : (lhsIndex > rhsIndex) ? 1 : 0;
                           });
    }

    Compilation::Result takeResult() && {
      if (ResultCode == 0)
        ResultCode = Comp.getDiags().hadAnyError();
      const bool hadAbnormalExit = hadAnyAbnormalExit();
      const auto resultCode = ResultCode;
      auto &&graph = std::move(*this).takeFineGrainedDepGraph();
      return Compilation::Result{hadAbnormalExit, resultCode, std::move(graph)};
    }

    bool hadAnyAbnormalExit() {
      return AnyAbnormalExit;
    }

    // MARK: dependency graph interface

    std::vector<StringRef> getExternalDependencies() const {
      return getFineGrainedDepGraph().getExternalDependencies();
    }

    std::vector<const Job*>
    markExternalInDepGraph(StringRef externalDependency) {
      return getFineGrainedDepGraph()
          .findExternallyDependentUntracedJobs(externalDependency);
    }

    std::vector<const Job *> findJobsToRecompileWhenWholeJobChanges(const Job *Cmd) {
      return getFineGrainedDepGraph()
          .findJobsToRecompileWhenWholeJobChanges(Cmd);
    }

    void registerJobToDepGraph(const Job *Cmd) {
      getFineGrainedDepGraph().registerJob(Cmd);
    }

    /// Return hadError
    bool loadDepGraphFromPath(const Job *Cmd, const StringRef DependenciesFile) {
      const auto changes = getFineGrainedDepGraph().loadFromPath(
          Cmd, DependenciesFile, Comp.getDiags());
      const bool didDependencyLoadSucceed = changes.has_value();
      return !didDependencyLoadSucceed;
    }

    fine_grained_dependencies::ModuleDepGraph &
    getFineGrainedDepGraph() {
      return FineGrainedDepGraph;
    }
    const fine_grained_dependencies::ModuleDepGraph &
    getFineGrainedDepGraph() const {
      return FineGrainedDepGraph;
    }

    fine_grained_dependencies::ModuleDepGraph &&
    takeFineGrainedDepGraph() && {
      return std::move(FineGrainedDepGraph);
    }
  };
} // namespace driver
} // namespace swift

Compilation::~Compilation() = default;

Job *Compilation::addJob(std::unique_ptr<Job> J) {
  Job *result = J.get();
  Jobs.emplace_back(std::move(J));
  return result;
}

Job *Compilation::addExternalJob(std::unique_ptr<Job> J) {
  Job *result = J.get();
  ExternalJobs.emplace_back(std::move(J));
  return result;
}

static void checkForOutOfDateInputs(DiagnosticEngine &diags,
                                    const InputInfoMap &inputs) {
  for (const auto &inputPair : inputs) {
    auto recordedModTime = inputPair.second.previousModTime;
    if (recordedModTime == llvm::sys::TimePoint<>::max())
      continue;

    const char *input = inputPair.first->getValue();

    llvm::sys::fs::file_status inputStatus;
    if (auto statError = llvm::sys::fs::status(input, inputStatus)) {
      diags.diagnose(SourceLoc(), diag::warn_cannot_stat_input,
                     llvm::sys::path::filename(input), statError.message());
      continue;
    }

    if (recordedModTime != inputStatus.getLastModificationTime()) {
      diags.diagnose(SourceLoc(), diag::error_input_changed_during_build,
                     llvm::sys::path::filename(input));
    }
  }
}

static void writeCompilationRecord(StringRef path, StringRef argsHash,
                                   llvm::sys::TimePoint<> buildTime,
                                   const InputInfoMap &inputs) {
  // Before writing to the dependencies file path, preserve any previous file
  // that may have been there. No error handling -- this is just a nicety, it
  // doesn't matter if it fails.
  llvm::sys::fs::rename(path, path + "~");

  std::error_code error;
  llvm::raw_fd_ostream out(path, error, llvm::sys::fs::OF_None);
  if (out.has_error()) {
    // FIXME: How should we report this error?
    out.clear_error();
    return;
  }

  auto writeTimeValue = [](llvm::raw_ostream &out,
                           llvm::sys::TimePoint<> time) {
    using namespace std::chrono;
    auto secs = time_point_cast<seconds>(time);
    time -= secs.time_since_epoch(); // remainder in nanoseconds
    out << "[" << secs.time_since_epoch().count()
        << ", " << time.time_since_epoch().count() << "]";
  };

  using compilation_record::TopLevelKey;
  // NB: We calculate effective version from getCurrentLanguageVersion()
  // here because any -swift-version argument is handled in the
  // argsHash that follows.
  out << compilation_record::getName(TopLevelKey::Version) << ": \""
      << llvm::yaml::escape(version::getSwiftFullVersion(
                              swift::version::Version::getCurrentLanguageVersion()))
      << "\"\n";
  out << compilation_record::getName(TopLevelKey::Options) << ": \""
      << llvm::yaml::escape(argsHash) << "\"\n";
  out << compilation_record::getName(TopLevelKey::BuildTime) << ": ";
  writeTimeValue(out, buildTime);
  out << "\n";
  out << compilation_record::getName(TopLevelKey::Inputs) << ":\n";

  for (auto &entry : inputs) {
    out << "  \"" << llvm::yaml::escape(entry.first->getValue()) << "\": ";

    using compilation_record::getIdentifierForInputInfoStatus;
    auto Name = getIdentifierForInputInfoStatus(entry.second.status);
    if (!Name.empty()) {
      out << Name << " ";
    }

    writeTimeValue(out, entry.second.previousModTime);
    out << "\n";
  }
}

static void writeInputJobsToFilelist(llvm::raw_fd_ostream &out, const Job *job,
                                     const file_types::ID infoType) {
  // FIXME: Duplicated from ToolChains.cpp.
  for (const Job *input : job->getInputs()) {
    const CommandOutput &outputInfo = input->getOutput();
    if (outputInfo.getPrimaryOutputType() == infoType) {
      for (auto &output : outputInfo.getPrimaryOutputFilenames())
        out << output << "\n";
    } else {
      auto output = outputInfo.getAnyOutputForType(infoType);
      if (!output.empty())
        out << output << "\n";
    }
  }
}
static void writeSourceInputActionsToFilelist(llvm::raw_fd_ostream &out,
                                              const Job *job,
                                              const ArgList &args) {
  // Ensure that -index-file-path works in conjunction with
  // -driver-use-filelists. It needs to be the only primary.
  if (Arg *A = args.getLastArg(options::OPT_index_file_path))
    out << A->getValue() << "\n";
  else {
    // The normal case for non-single-compile jobs.
    for (const Action *A : job->getSource().getInputs()) {
      // A could be a GeneratePCHJobAction
      if (!isa<InputAction>(A))
        continue;
      const auto *IA = cast<InputAction>(A);
      out << IA->getInputArg().getValue() << "\n";
    }
  }
}
static void writeOutputToFilelist(llvm::raw_fd_ostream &out, const Job *job,
                                  const file_types::ID infoType) {
  const CommandOutput &outputInfo = job->getOutput();
  assert(outputInfo.getPrimaryOutputType() == infoType);
  for (auto &output : outputInfo.getPrimaryOutputFilenames())
    out << output << "\n";
}
static void writeIndexUnitOutputPathsToFilelist(llvm::raw_fd_ostream &out,
                                                const Job *job) {
  const CommandOutput &outputInfo = job->getOutput();
  for (auto &output : outputInfo.getIndexUnitOutputFilenames())
    out << output << "\n";
}
static void writeSupplementaryOutputToFilelist(llvm::raw_fd_ostream &out,
                                               const Job *job) {
  job->getOutput().writeOutputFileMap(out);
}

static bool writeFilelistIfNecessary(const Job *job, const ArgList &args,
                                     DiagnosticEngine &diags) {
  bool ok = true;
  for (const FilelistInfo &filelistInfo : job->getFilelistInfos()) {
    if (filelistInfo.path.empty())
      return true;

    std::error_code error;
    llvm::raw_fd_ostream out(filelistInfo.path, error, llvm::sys::fs::OF_None);
    if (out.has_error()) {
      out.clear_error();
      diags.diagnose(SourceLoc(), diag::error_unable_to_make_temporary_file,
                     error.message());
      ok = false;
      continue;
    }

    switch (filelistInfo.whichFiles) {
    case FilelistInfo::WhichFiles::InputJobs:
      writeInputJobsToFilelist(out, job, filelistInfo.type);
      break;
    case FilelistInfo::WhichFiles::SourceInputActions:
      writeSourceInputActionsToFilelist(out, job, args);
      break;
    case FilelistInfo::WhichFiles::InputJobsAndSourceInputActions:
      writeInputJobsToFilelist(out, job, filelistInfo.type);
      writeSourceInputActionsToFilelist(out, job, args);
      break;
    case FilelistInfo::WhichFiles::Output:
      writeOutputToFilelist(out, job, filelistInfo.type);
      break;
    case FilelistInfo::WhichFiles::IndexUnitOutputPaths:
      writeIndexUnitOutputPathsToFilelist(out, job);
      break;
    case FilelistInfo::WhichFiles::SupplementaryOutput:
      writeSupplementaryOutputToFilelist(out, job);
      break;
    }
  }
  return ok;
}

Compilation::Result
Compilation::performJobsImpl(std::unique_ptr<TaskQueue> &&TQ) {
  PerformJobsState State(*this, std::move(TQ));

  State.runJobs();

  if (!CompilationRecordPath.empty()) {
    InputInfoMap InputInfo;
    State.populateInputInfoMap(InputInfo);
    checkForOutOfDateInputs(Diags, InputInfo);

    writeCompilationRecord(CompilationRecordPath, ArgsHash, BuildStartTime,
                           InputInfo);
  }
  return std::move(State).takeResult();
}

Compilation::Result Compilation::performSingleCommand(const Job *Cmd) {
  assert(Cmd->getInputs().empty() &&
         "This can only be used to run a single command with no inputs");

  switch (Cmd->getCondition()) {
  case Job::Condition::CheckDependencies:
    return Compilation::Result::code(0);
  case Job::Condition::RunWithoutCascading:
  case Job::Condition::Always:
  case Job::Condition::NewlyAdded:
    break;
  }

  if (!writeFilelistIfNecessary(Cmd, *TranslatedArgs.get(), Diags))
    return Compilation::Result::code(1);

  switch (Level) {
  case OutputLevel::Normal:
  case OutputLevel::Parseable:
    break;
  case OutputLevel::PrintJobs:
    Cmd->printCommandLineAndEnvironment(llvm::outs());
    return Compilation::Result::code(0);
  case OutputLevel::Verbose:
    Cmd->printCommandLine(llvm::errs());
    break;
  }

  SmallVector<const char *, 128> Argv;
  Argv.push_back(Cmd->getExecutable());
  Argv.append(Cmd->getArguments().begin(), Cmd->getArguments().end());
  Argv.push_back(nullptr);

  const char *ExecPath = Cmd->getExecutable();
  const char **argv = Argv.data();

  for (auto &envPair : Cmd->getExtraEnvironment()) {
#if defined(_MSC_VER)
    int envResult =_putenv_s(envPair.first, envPair.second);
#else
    int envResult = setenv(envPair.first, envPair.second, /*replacing=*/true);
#endif
    assert(envResult == 0 &&
          "expected environment variable to be set successfully");
    // Bail out early in release builds.
    if (envResult != 0) {
      return Compilation::Result::code(envResult);
    }
  }

  const auto returnCode = ExecuteInPlace(ExecPath, argv);
  return Compilation::Result::code(returnCode);
}

static bool writeAllSourcesFile(DiagnosticEngine &diags, StringRef path,
                                ArrayRef<InputPair> inputFiles) {
  std::error_code error;
  llvm::raw_fd_ostream out(path, error, llvm::sys::fs::OF_None);
  if (out.has_error()) {
    out.clear_error();
    diags.diagnose(SourceLoc(), diag::error_unable_to_make_temporary_file,
                   error.message());
    return false;
  }

  for (auto inputPair : inputFiles) {
    if (!file_types::isPartOfSwiftCompilation(inputPair.first))
      continue;
    out << inputPair.second->getValue() << "\n";
  }

  return true;
}

Compilation::Result Compilation::performJobs(std::unique_ptr<TaskQueue> &&TQ) {
  if (AllSourceFilesPath)
    if (!writeAllSourcesFile(Diags, AllSourceFilesPath, getInputFiles()))
      return Compilation::Result::code(EXIT_FAILURE);

  // If we don't have to do any cleanup work, just exec the subprocess.
  if (Level < OutputLevel::Parseable &&
      !ShowDriverTimeCompilation &&
      (SaveTemps || TempFilePaths.empty()) &&
      CompilationRecordPath.empty() &&
      Jobs.size() == 1) {
    return performSingleCommand(Jobs.front().get());
  }

  if (!TaskQueue::supportsParallelExecution() && TQ->getNumberOfParallelTasks() > 1) {
    Diags.diagnose(SourceLoc(), diag::warning_parallel_execution_not_supported);
  }

  auto result = performJobsImpl(std::move(TQ));

  if (!SaveTemps) {
    for (const auto &pathPair : TempFilePaths) {
      if (!result.hadAbnormalExit || pathPair.getValue() == PreserveOnSignal::No)
        (void)llvm::sys::fs::remove(pathPair.getKey());
    }
  }
  if (Stats)
    Stats->noteCurrentProcessExitStatus(result.exitCode);
  return result;
}

const char *Compilation::getAllSourcesPath() const {
  if (!AllSourceFilesPath) {
    SmallString<128> Buffer;
    std::error_code EC =
        llvm::sys::fs::createTemporaryFile("sources", "", Buffer);
    if (EC) {
      // Use the constructor that prints both the error code and the
      // description.
      // FIXME: This should not take down the entire process.
      auto error = llvm::make_error<llvm::StringError>(
          EC,
          "- unable to create list of input sources");
      llvm::report_fatal_error(std::move(error));
    }
    auto *mutableThis = const_cast<Compilation *>(this);
    mutableThis->addTemporaryFile(Buffer.str(), PreserveOnSignal::Yes);
    mutableThis->AllSourceFilesPath = getArgs().MakeArgString(Buffer);
  }
  return AllSourceFilesPath;
}

void Compilation::disableIncrementalBuild(Twine why) {
  if (getShowIncrementalBuildDecisions())
    llvm::outs() << "Disabling incremental build: " << why << "\n";

  EnableIncrementalBuild = false;
}

unsigned Compilation::countSwiftInputs() const {
  unsigned inputCount = 0;
  for (const auto &p : InputFilesWithTypes)
    if (p.first == file_types::TY_Swift)
      ++inputCount;
  return inputCount;
}

void Compilation::addDependencyPathOrCreateDummy(
    StringRef depPath, function_ref<void()> addDependencyPath) {

  if (!OnlyOneDependencyFile) {
    addDependencyPath();
    return;
  }
  if (!HaveAlreadyAddedDependencyPath) {
    addDependencyPath();
    HaveAlreadyAddedDependencyPath = true;
  } else if (!depPath.empty()) {
    // Create dummy empty file
    std::error_code EC;
    llvm::raw_fd_ostream(depPath, EC, llvm::sys::fs::OF_None);
  }
}

template <typename JobCollection>
void Compilation::sortJobsToMatchCompilationInputs(
    const JobCollection &unsortedJobs,
    SmallVectorImpl<const Job *> &sortedJobs) const {
  llvm::DenseMap<StringRef, const Job *> jobsByInput;
  for (const Job *J : unsortedJobs) {
    // Only worry about sorting compilation jobs
    if (const CompileJobAction *CJA =
            dyn_cast<CompileJobAction>(&J->getSource())) {
      const InputAction *IA = CJA->findSingleSwiftInput();
      jobsByInput.insert(std::make_pair(IA->getInputArg().getValue(), J));
    } else
      sortedJobs.push_back(J);
  }
  for (const InputPair &P : getInputFiles()) {
    auto I = jobsByInput.find(P.second->getValue());
    if (I != jobsByInput.end()) {
      sortedJobs.push_back(I->second);
    }
  }
}

template void
Compilation::sortJobsToMatchCompilationInputs<ArrayRef<const Job *>>(
    const ArrayRef<const Job *> &,
    SmallVectorImpl<const Job *> &sortedJobs) const;
