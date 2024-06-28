//===-- OptimizerStatsUtils.cpp - Utils for collecting stats  -*- C++ ---*-===//
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
/// This file implements collection and dumping of statistics about SILModules,
/// SILFunctions and memory consumption during the execution of SIL
/// optimization pipelines.
///
/// The following statistics are collected:
/// - For SILFunctions: the number of SIL basic blocks, the number of SIL
/// instructions
///
/// - For SILModules: the number of SIL basic blocks, the number of SIL
/// instructions, the number of SILFunctions, the amount of memory used by the
/// compiler.
///
/// By default, any collection of statistics is disabled to avoid affecting
/// compile times.
///
/// One can enable the collection of statistics and dumping of these statistics
/// for the whole SILModule and/or for SILFunctions.
///
/// To reduce the amount of produced data, one can set thresholds in such a way
/// that changes in the statistics are only reported if the delta between the
/// old and the new values are at least X%. The deltas are computed using the
/// following formula:
///  Delta = (NewValue - OldValue) / OldValue
///
/// Thresholds provide a simple way to perform a simple filtering of the
/// collected statistics during the compilation. But if there is a need for a
/// more complex analysis of collected data (e.g. aggregation by a pipeline
/// stage or by the type of a transformation), it is often better to dump
/// as much data as possible into a file using e.g. `-sil-stats-dump-all
/// -sil-stats-modules -sil-stats-functions` and then e.g. use the helper scripts
/// to store the collected data into a database and then perform complex queries
/// on it. Many kinds of analysis can be then formulated pretty easily as SQL
/// queries.
///
/// The output format is a set of CSV (comma separated values) lines. Each lines
/// represents one counter or one counter change.
///
/// For counter updates it looks like this:
///  Kind, CounterName, StageName, TransformName, TransformPassNumber,
///  DeltaValue, OldCounterValue, NewCounterValue, Duration, Symbol
///
/// For counter updates it looks like this:
///  Kind, CounterName, StageName, TransformName, TransformPassNumber,
///  CounterValue, Duration, Symbol
///
/// where Kind is one of "function", "module", "function_history",
///       CounterName is one of "block", "inst", "function", "memory",
///       Symbol is e.g. the name of a function.
///       StageName and TransformName are the names of the current optimizer
///       pipeline stage and current transform.
///       Duration is the duration of the transformation.
//===----------------------------------------------------------------------===//

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Process.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/SourceManager.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/SILVisitor.h"
#include "swift/SILOptimizer/Analysis/Analysis.h"
#include "swift/SILOptimizer/PassManager/PassManager.h"
#include "swift/SILOptimizer/PassManager/Transforms.h"
#include "swift/SILOptimizer/Utils/OptimizerStatsUtils.h"
using namespace swift;

namespace {

/// The total number of different kinds of SILInstructions.
constexpr unsigned NumSILInstructions =
    unsigned(SILNodeKind::Last_SILInstruction)
  - unsigned(SILNodeKind::First_SILInstruction)
  + 1;

static unsigned getIndexForKind(SILInstructionKind kind) {
  return unsigned(kind) - unsigned(SILNodeKind::First_SILInstruction);
}

/// A set of counters, one per SILInstruction kind.
class InstructionCounts {
  unsigned Counts[NumSILInstructions] = {};

public:
  constexpr InstructionCounts() {}

  unsigned &operator[](SILInstructionKind kind) {
    return Counts[getIndexForKind(kind)];
  }

  void addAll(const InstructionCounts &other) {
    for (unsigned i = 0; i != NumSILInstructions; ++i) {
      Counts[i] += other.Counts[i];
    }
  }

  void subAll(const InstructionCounts &other) {
    for (unsigned i = 0; i != NumSILInstructions; ++i) {
      Counts[i] -= other.Counts[i];
    }
  }
};

/// A helper type to parse a comma separated list of SIL instruction names
/// provided as argument of the -sil-stats-only-instructions options.
class StatsOnlyInstructionsOpt {
  bool ShouldComputeInstCounts[NumSILInstructions] = {};

  /// The number of different kinds of SILInstructions to be tracked.
  unsigned NumInstCounts = 0;

public:
  constexpr StatsOnlyInstructionsOpt() {}

  void operator=(StringRef val) {
    if (val.empty())
      return;
    if (val == "all") {
      for (auto &inst : ShouldComputeInstCounts) {
        inst = true;
      }
      NumInstCounts = NumSILInstructions;
      return;
    }
    SmallVector<StringRef, 8> statsInstNames;
    val.split(statsInstNames, ',', -1, false);
    for (auto instName : statsInstNames) {
      // Check if it is a known instruction.
      auto kind = getSILInstructionKind(instName);
      unsigned index = getIndexForKind(kind);
      if (!ShouldComputeInstCounts[index]) {
        ShouldComputeInstCounts[index] = true;
        ++NumInstCounts;
      }
    }
  }

  bool shouldComputeInstCount(SILInstructionKind kind) const {
    return ShouldComputeInstCounts[getIndexForKind(kind)];
  }

  int getInstCountsNum() const {
    return NumInstCounts;
  }
};

StatsOnlyInstructionsOpt StatsOnlyInstructionsOptLoc;

/// Use this option like -Xllvm -sil-stats-only-instructions=strong_retain,alloc_stack
/// If you need to track all kinds of SILInstructions, you can use
/// -sil-stats-only-instructions=all
llvm::cl::opt<StatsOnlyInstructionsOpt, true, llvm::cl::parser<std::string>>
    StatsOnlyInstructions(
        "sil-stats-only-instructions",
        llvm::cl::desc(
            "Comma separated list of SIL instruction names, whose stats "
            "should be collected"),
        llvm::cl::Hidden, llvm::cl::ZeroOrMore,
        llvm::cl::value_desc("instruction name"),
        llvm::cl::location(StatsOnlyInstructionsOptLoc),
        llvm::cl::ValueRequired);

/// Dump as much statistics as possible, ignore any thresholds.
llvm::cl::opt<bool> SILStatsDumpAll(
    "sil-stats-dump-all", llvm::cl::init(false),
    llvm::cl::desc("Dump all SIL module and SIL function stats"));

/// Dump statistics about the SILModule.
llvm::cl::opt<bool> SILStatsModules(
    "sil-stats-modules", llvm::cl::init(false),
    llvm::cl::desc("Enable computation of statistics for SIL modules"));

/// Dump statistics about SILFunctions.
llvm::cl::opt<bool> SILStatsFunctions(
    "sil-stats-functions", llvm::cl::init(false),
    llvm::cl::desc("Enable computation of statistics for SIL functions"));

/// Dump statistics about lost debug variables.
llvm::cl::opt<bool> SILStatsLostVariables(
    "sil-stats-lost-variables", llvm::cl::init(false),
    llvm::cl::desc("Dump lost debug variables stats"));

/// The name of the output file for optimizer counters.
llvm::cl::opt<std::string> SILStatsOutputFile(
    "sil-stats-output-file", llvm::cl::init(""),
    llvm::cl::desc("The name of the output file for optimizer counters"));

/// A threshold in percents for the SIL basic block counters.
llvm::cl::opt<double> BlockCountDeltaThreshold(
    "sil-stats-block-count-delta-threshold", llvm::cl::init(1),
    llvm::cl::desc(
        "Threshold for reporting changed basic block count numbers"));

/// A threshold in percents for the SIL instructions counters.
llvm::cl::opt<double> InstCountDeltaThreshold(
    "sil-stats-inst-count-delta-threshold", llvm::cl::init(1),
    llvm::cl::desc(
        "Threshold for reporting changed instruction count numbers"));

/// A threshold in percents for the SIL functions counters.
llvm::cl::opt<double> FunctionCountDeltaThreshold(
    "sil-stats-function-count-delta-threshold", llvm::cl::init(1),
    llvm::cl::desc("Threshold for reporting changed function count numbers"));

/// A threshold in percents for the counters of memory used by the compiler.
llvm::cl::opt<double> UsedMemoryDeltaThreshold(
    "sil-stats-used-memory-delta-threshold", llvm::cl::init(5),
    llvm::cl::desc("Threshold for reporting changed memory usage numbers"));

llvm::cl::opt<double> UsedMemoryMinDeltaThreshold(
  "sil-stats-used-memory-min-threshold", llvm::cl::init(1),
    llvm::cl::desc("Min threshold for reporting changed memory usage numbers"));

/// A threshold in percents for the basic blocks counter inside a SILFunction.
/// Has effect only if it is used together with -sil-stats-functions.
llvm::cl::opt<double> FuncBlockCountDeltaThreshold(
    "sil-stats-func-block-count-delta-threshold", llvm::cl::init(200),
    llvm::cl::desc("Threshold for reporting changed basic block count numbers "
                   "for a function"));

/// A minimal threshold (in number of basic blocks) for the basic blocks counter
/// inside a SILFunction. Only functions with more basic blocks than this
/// threshold are reported. Has effect only if it is used together with
/// -sil-stats-functions.
llvm::cl::opt<int> FuncBlockCountMinThreshold(
    "sil-stats-func-block-count-min-threshold", llvm::cl::init(50),
    llvm::cl::desc(
        "Min threshold for reporting changed basic block count numbers "
        "for a function"));

/// A threshold in percents for the SIL instructions counter inside a
/// SILFunction. Has effect only if it is used together with
/// -sil-stats-functions.
llvm::cl::opt<double> FuncInstCountDeltaThreshold(
    "sil-stats-func-inst-count-delta-threshold", llvm::cl::init(200),
    llvm::cl::desc("Threshold for reporting changed instruction count numbers "
                   "for a function"));

/// A minimal threshold (in number of instructions) for the SIL instructions
/// counter inside a SILFunction. Only functions with more instructions than
/// this threshold are reported. Has effect only if it is used together with
/// -sil-stats-functions.
llvm::cl::opt<int> FuncInstCountMinThreshold(
    "sil-stats-func-inst-count-min-threshold", llvm::cl::init(300),
    llvm::cl::desc(
        "Min threshold for reporting changed instruction count numbers "
        "for a function"));

/// Track only statistics for a function with a given name.
/// Has effect only if it is used together with -sil-stats-functions.
llvm::cl::opt<std::string> StatsOnlyFunctionName(
    "sil-stats-only-function", llvm::cl::init(""),
    llvm::cl::desc("Function name, whose stats should be tracked"));

/// Track only statistics for a function whose name contains a given substring.
/// Has effect only if it is used together with -sil-stats-functions.
llvm::cl::opt<std::string> StatsOnlyFunctionsNamePattern(
    "sil-stats-only-functions", llvm::cl::init(""),
    llvm::cl::desc(
        "Pattern of a function name, whose stats should be tracked"));

/// Stats for a SIL function.
struct FunctionStat {
  int BlockCount = 0;
  int InstCount = 0;
  /// Instruction counts per SILInstruction kind.
  InstructionCounts InstCounts;

  using VarID = std::tuple<const SILDebugScope *, llvm::StringRef,
                           unsigned, unsigned>;
  llvm::StringSet<> VarNames;
  llvm::DenseSet<FunctionStat::VarID> DebugVariables;

  FunctionStat(SILFunction *F);
  FunctionStat() {}

  // The DebugVariables set contains pointers to VarNames. Disallow copy.
  FunctionStat(const FunctionStat &) = delete;
  FunctionStat(FunctionStat &&) = default;
  FunctionStat &operator=(const FunctionStat &) = delete;
  FunctionStat &operator=(FunctionStat &&) = default;

  void print(llvm::raw_ostream &stream) const {
    stream << "FunctionStat("
           << "blocks = " << BlockCount << ", Inst = " << InstCount << ")\n";
  }

  bool operator==(const FunctionStat &rhs) const {
    return BlockCount == rhs.BlockCount && InstCount == rhs.InstCount
      && DebugVariables == rhs.DebugVariables;
  }

  bool operator!=(const FunctionStat &rhs) const { return !(*this == rhs); }

  void dump() { print(llvm::errs()); }
};

/// Stats a single SIL module.
struct ModuleStat {
  /// Total number of SILFunctions in the current SILModule.
  int FunctionCount = 0;
  /// Total number of SILBasicBlocks in the current SILModule.
  int BlockCount = 0;
  /// Total number of SILInstructions in the current SILModule.
  int InstCount = 0;
  /// Total amount of memory used by the compiler.
  int UsedMemory = 0;
  /// Total number of SILInstructions created since the beginning of the current
  /// compilation.
  int CreatedInstCount = 0;
  /// Total number of SILInstructions deleted since the beginning of the current
  /// compilation.
  int DeletedInstCount = 0;
  /// Instruction counts per SILInstruction kind.
  InstructionCounts InstCounts;

  ModuleStat() {}

  /// Add the stats for a given function to the total module stats.
  void addFunctionStat(FunctionStat &Stat) {
    BlockCount += Stat.BlockCount;
    InstCount += Stat.InstCount;
    ++FunctionCount;
    if (!StatsOnlyInstructionsOptLoc.getInstCountsNum())
      return;
    InstCounts.addAll(Stat.InstCounts);
  }

  /// Subtract the stats for a given function from total module stats.
  void subFunctionStat(FunctionStat &Stat) {
    BlockCount -= Stat.BlockCount;
    InstCount -= Stat.InstCount;
    --FunctionCount;
    if (!StatsOnlyInstructionsOptLoc.getInstCountsNum())
      return;
    InstCounts.subAll(Stat.InstCounts);
  }

  /// Add the stats about current memory usage.
  void addMemoryStat() {
    UsedMemory = llvm::sys::Process::GetMallocUsage();
  }

  /// Add the stats about created and deleted instructions.
  void addCreatedAndDeletedInstructionsStat() {
    CreatedInstCount = SILInstruction::getNumCreatedInstructions();
    DeletedInstCount = SILInstruction::getNumDeletedInstructions();
  }

  void print(llvm::raw_ostream &stream) const {
    stream << "ModuleStat(functions = " << FunctionCount
           << ", blocks = " << BlockCount << ", Inst = " << InstCount
           << ", UsedMemory = " << UsedMemory / (1024 * 1024)
           << ", CreatedInst = " << CreatedInstCount
           << ", DeletedInst = " << DeletedInstCount
           << ")\n";
  }

  void dump() { print(llvm::errs()); }

  bool operator==(const ModuleStat &rhs) const {
    return FunctionCount == rhs.FunctionCount && BlockCount == rhs.BlockCount &&
           InstCount == rhs.InstCount && UsedMemory == rhs.UsedMemory;
  }

  bool operator!=(const ModuleStat &rhs) const { return !(*this == rhs); }
};

/// A helper type to collect the stats about a function (instructions, blocks,
/// debug variables).
struct InstCountVisitor : SILInstructionVisitor<InstCountVisitor> {
  int BlockCount = 0;
  int InstCount = 0;
  InstructionCounts &InstCounts;

  llvm::StringSet<> &VarNames;
  llvm::DenseSet<FunctionStat::VarID> &DebugVariables;

  InstCountVisitor(InstructionCounts &InstCounts,
                   llvm::StringSet<> &VarNames,
                   llvm::DenseSet<FunctionStat::VarID> &DebugVariables)
  : InstCounts(InstCounts), VarNames(VarNames), DebugVariables(DebugVariables) {}

  int getBlockCount() const {
    return BlockCount;
  }

  int getInstCount() const {
    return InstCount;
  }

  void visitSILBasicBlock(SILBasicBlock *BB) {
    ++BlockCount;
    SILInstructionVisitor<InstCountVisitor>::visitSILBasicBlock(BB);
  }

  void visit(SILInstruction *I) {
    ++InstCount;
    ++InstCounts[I->getKind()];

    if (!SILStatsLostVariables)
      return;
    // Check the debug variable.
    DebugVarCarryingInst inst(I);
    if (!inst)
      return;
    std::optional<SILDebugVariable> varInfo = inst.getVarInfo();
    if (!varInfo)
      return;

    llvm::StringRef UniqueName = VarNames.insert(varInfo->Name).first->getKey();
    unsigned line = 0, col = 0;
    if (varInfo->Loc && varInfo->Loc->getSourceLoc().isValid()) {
      std::tie(line, col) = inst->getModule().getSourceManager()
        .getPresumedLineAndColumnForLoc(varInfo->Loc->getSourceLoc(), 0);
    }
    FunctionStat::VarID key(
              varInfo->Scope ? varInfo->Scope : inst->getDebugScope(),
              UniqueName, line, col);
    DebugVariables.insert(key);
  }
};

/// A helper type to store different parameters related to the current transform.
class TransformationContext {
  /// SILModule being processed.
  SILModule &M;
  /// The pass manager being used.
  SILPassManager &PM;
  /// The transformation that was/will be performed.
  SILTransform *Transform;
  /// The time it took to perform the transformation.
  int Duration;
  /// The pass number in the optimizer pipeline.
  int PassNumber;

public:
  TransformationContext(SILModule &M, SILPassManager &PM,
                        SILTransform *Transform, int PassNumber, int Duration)
      : M(M), PM(PM), Transform(Transform), Duration(Duration),
        PassNumber(PassNumber) {}

  int getPassNumber() const {
    return PassNumber;
  }

  int getDuration() const {
    return Duration;
  }

  StringRef getTransformId() const {
    return Transform->getID();
  }

  StringRef getStageName() const {
    return PM.getStageName();
  }

  SILModule &getModule() {
    return M;
  }

  SILPassManager &getPassManager() {
    return PM;
  }
};

/// Aggregated statistics for the whole SILModule and all SILFunctions belonging
/// to it.
class AccumulatedOptimizerStats {
  using FunctionStats = llvm::DenseMap<const SILFunction *, FunctionStat>;

  /// Current stats for each function.
  FunctionStats FuncStats;
  /// Current stats for the module.
  ModuleStat ModStat;

public:
  FunctionStat &getFunctionStat(const SILFunction *F) {
    return FuncStats[F];
  }

  ModuleStat &getModuleStat() {
    return ModStat;
  }

  void deleteFunctionStat(SILFunction *F) {
    FuncStats.erase(F);
  }
};

/// A helper class to represent the module stats as an analysis,
/// so that it is preserved across multiple passes.
class OptimizerStatsAnalysis : public SILAnalysis {
  SILModule &M;
  /// The actual cache holding all the statistics.
  std::unique_ptr<AccumulatedOptimizerStats> Cache;

  /// Sets of functions changed, deleted or added since the last
  /// computation of statistics. These sets are used to avoid complete
  /// re-computation of the stats for the whole module. Instead only
  /// re-compute the stats for the changed functions, which is much
  /// faster.
  SmallVector<SILFunction *, 16> InvalidatedFuncs;
  SmallVector<SILFunction *, 16> DeletedFuncs;
  SmallVector<SILFunction *, 16> AddedFuncs;

public:
  OptimizerStatsAnalysis(SILModule *M)
      : SILAnalysis(SILAnalysisKind::OptimizerStats), M(*M), Cache(nullptr) {}

  static bool classof(const SILAnalysis *S) {
    return S->getKind() == SILAnalysisKind::OptimizerStats;
  }

  /// Invalidate all information in this analysis.
  virtual void invalidate() override {
    // This analysis is never invalidated, because it just used
    // to store the statistics.
  }

  /// Invalidate all of the information for a specific function.
  virtual void invalidate(SILFunction *F, InvalidationKind K) override {
    InvalidatedFuncs.push_back(F);
  }

  /// Notify the analysis about a newly created function.
  virtual void notifyAddedOrModifiedFunction(SILFunction *F) override {
    AddedFuncs.push_back(F);
  }

  /// Notify the analysis about a function which will be deleted from the
  /// module.
  virtual void notifyWillDeleteFunction(SILFunction *F) override {
    DeletedFuncs.push_back(F);
  };

  /// Notify the analysis about changed witness or vtables.
  virtual void invalidateFunctionTables() override {
  }


  SILModule &getModule() const {
    return M;
  }

  /// Get the collected statistics for a function.
  FunctionStat &getFunctionStat(const SILFunction *F) {
    if (!Cache)
      Cache = std::make_unique<AccumulatedOptimizerStats>();

    return Cache->getFunctionStat(F);
  }

  /// Get the collected statistics for a module.
  ModuleStat &getModuleStat() {
    if (!Cache)
      Cache = std::make_unique<AccumulatedOptimizerStats>();

    return Cache->getModuleStat();
  }

  /// Update module stats after running the Transform.
  void updateModuleStats(TransformationContext &Ctx);
};

class NewLineInserter {
  bool isNewline = true;

public:
  NewLineInserter() {}
  StringRef get() {
    StringRef result =  isNewline ? "\n" : "";
    isNewline = false;
    return result;
  }
};

/// The output stream to be used for writing the collected statistics.
/// Use the unique_ptr to ensure that the file is properly closed upon
/// exit.
std::unique_ptr<llvm::raw_ostream, void(*)(llvm::raw_ostream *)>
    stats_output_stream = {nullptr, nullptr};

/// Return the output stream to be used for logging the collected statistics.
llvm::raw_ostream &stats_os() {
  // Initialize the stream if it is not initialized yet.
  if (!stats_output_stream) {
    // If there is user-defined output file name, use it.
    if (!SILStatsOutputFile.empty()) {
      // Try to open the file.
      std::error_code EC;
      auto fd_stream = std::make_unique<llvm::raw_fd_ostream>(
          SILStatsOutputFile, EC, llvm::sys::fs::OpenFlags::OF_Text);
      if (!fd_stream->has_error() && !EC) {
        stats_output_stream = {fd_stream.release(),
                               [](llvm::raw_ostream *d) { delete d; }};
        return *stats_output_stream.get();
      }
      fd_stream->clear_error();
      llvm::errs() << SILStatsOutputFile << " : " << EC.message() << "\n";
    }
    // Otherwise use llvm::errs() as output. No need to destroy it at the end.
    stats_output_stream = {&llvm::errs(), [](llvm::raw_ostream *d) {}};
  }
  return *stats_output_stream.get();
}

/// A helper function to dump the counter value.
void printCounterValue(StringRef Kind, StringRef CounterName, int CounterValue,
                       StringRef Symbol, TransformationContext &Ctx) {
  stats_os() << Kind;
  stats_os() << ", ";

  stats_os() << CounterName;
  stats_os() << ", ";

  stats_os() << Ctx.getStageName();
  stats_os() << ", ";

  stats_os() << Ctx.getTransformId();
  stats_os() << ", ";

  stats_os() << Ctx.getPassNumber();
  stats_os() << ", ";

  stats_os() << CounterValue;
  stats_os() << ", ";

  stats_os() << Ctx.getDuration();
  stats_os() << ", ";

  stats_os() << Symbol;
  stats_os() << "\n";
}

/// A helper function to dump the change of the counter value.
void printCounterChange(StringRef Kind, StringRef CounterName, double Delta,
                        int OldValue, int NewValue, TransformationContext &Ctx,
                        StringRef Symbol = "") {
  stats_os() << Kind;
  stats_os() << ", ";

  stats_os() << CounterName;
  stats_os() << ", ";

  stats_os() << Ctx.getStageName();
  stats_os() << ", ";

  stats_os() << Ctx.getTransformId();
  stats_os() << ", ";

  stats_os() << Ctx.getPassNumber();
  stats_os() << ", ";

  llvm::format_provider<double>::format(Delta, stats_os(), "f8");
  stats_os() << ", ";

  stats_os() << OldValue;
  stats_os() << ", ";

  stats_os() << NewValue;
  stats_os() << ", ";

  stats_os() << Ctx.getDuration();
  stats_os() << ", ";

  stats_os() << Symbol;
  stats_os() << "\n";
}

/// Check if a function name matches the pattern provided by the user.
bool isMatchingFunction(SILFunction *F, bool shouldHaveNamePattern = false) {
  auto FuncName = F->getName();
  // Is it an exact string match?
  if (!StatsOnlyFunctionName.empty())
    return F->getName() == StatsOnlyFunctionName;

  // Does the name contain a user-defined substring?
  if (!StatsOnlyFunctionsNamePattern.empty()) {
    return FuncName.contains(StatsOnlyFunctionsNamePattern);
  }

  return shouldHaveNamePattern;
}

/// Compute the delta between the old and new values.
/// Return it as a percent value.
double computeDelta(int Old, int New) {
  return 100.0 * (Old ? ((New - Old) * 1.0 / Old) : 0.0);
}

/// Returns true if it is a first time we collect data for a given counter.
/// This is the case if the old value is 0 and the new one is something
/// different, i.e. we didn't have any statistics about it.
bool isFirstTimeData(int Old, int New) {
  return Old == 0 && New != Old;
}

int computeLostVariables(llvm::DenseSet<FunctionStat::VarID> &Old,
                         llvm::DenseSet<FunctionStat::VarID> &New) {
  unsigned count = 0;
  for (auto &var : Old) {
    if (New.contains(var))
      continue;
    // llvm::dbgs() << "Lost variable: " << std::get<1>(var) << "\n";
    count++;
  }
  return count;
}

/// Dump statistics for a SILFunction. It is only used if a user asked to
/// produce detailed stats about transformations of SILFunctions. This
/// information is dumped unconditionally, for each transformation that changed
/// the function in any form. No thresholds are taken into account.
///
/// \param Stat statistics computed now, after the run of the transformation
/// \param Ctx transformation context to be used
void processFuncStatHistory(SILFunction *F, FunctionStat &Stat,
                            TransformationContext &Ctx) {
  if (!SILStatsFunctions)
    return;

  if (!SILStatsDumpAll && !isMatchingFunction(F, /*shouldHaveNamePattern*/ true))
    return;

  printCounterValue("function_history", "block", Stat.BlockCount, F->getName(),
                    Ctx);
  printCounterValue("function_history", "inst", Stat.InstCount, F->getName(),
                    Ctx);

  /// Dump collected instruction counts.
  if (!StatsOnlyInstructionsOptLoc.getInstCountsNum())
    return;

  for (auto kind : allSILInstructionKinds()) {
    if (!Stat.InstCounts[kind])
      continue;
    if (!StatsOnlyInstructionsOptLoc.shouldComputeInstCount(kind))
      continue;
    std::string CounterName = "inst_";
    CounterName += getSILInstructionName(kind);
    printCounterValue("function_history", CounterName, Stat.InstCounts[kind],
                      F->getName(), Ctx);
  }
}

/// Process SILFunction's statistics changes.
///
/// \param OldStat statistics computed last time
/// \param NewStat statistics computed now, after the run of the transformation
/// \param Ctx transformation context to be used
void processFuncStatsChanges(SILFunction *F, FunctionStat &OldStat,
                             FunctionStat &NewStat,
                             TransformationContext &Ctx) {
  processFuncStatHistory(F, NewStat, Ctx);

  if (!SILStatsFunctions && !SILStatsLostVariables && !SILStatsDumpAll)
    return;

  if (OldStat == NewStat)
    return;

  if ((!StatsOnlyFunctionsNamePattern.empty() ||
       !StatsOnlyFunctionName.empty()) &&
      !isMatchingFunction(F))
    return;

  // Compute deltas.
  double DeltaBlockCount = computeDelta(OldStat.BlockCount, NewStat.BlockCount);
  double DeltaInstCount = computeDelta(OldStat.InstCount, NewStat.InstCount);
  int LostVariables = computeLostVariables(OldStat.DebugVariables,
                                           NewStat.DebugVariables);

  NewLineInserter nl;

  // TODO: handle cases where a function got smaller.
  if ((SILStatsDumpAll &&
       (DeltaBlockCount != 0.0 || OldStat.BlockCount == 0)) ||
      (std::abs(DeltaBlockCount) > FuncBlockCountDeltaThreshold &&
       OldStat.BlockCount > FuncBlockCountMinThreshold)) {
    stats_os() << nl.get();
    printCounterChange("function", "block", DeltaBlockCount, OldStat.BlockCount,
                       NewStat.BlockCount, Ctx, F->getName());
  }

  if ((SILStatsDumpAll && (DeltaInstCount != 0.0 || OldStat.InstCount == 0)) ||
      (std::abs(DeltaInstCount) > FuncInstCountDeltaThreshold &&
       OldStat.InstCount > FuncInstCountMinThreshold)) {
    stats_os() << nl.get();
    printCounterChange("function", "inst", DeltaInstCount, OldStat.InstCount,
                       NewStat.InstCount, Ctx, F->getName());
  }

  if ((SILStatsDumpAll || SILStatsLostVariables) && LostVariables) {
    stats_os() << nl.get();
    printCounterValue("function", "lostvars", LostVariables, F->getName(), Ctx);
  }
}

/// Process SILModule's statistics changes.
///
/// \param OldStat statistics computed last time
/// \param NewStat statistics computed now, after the run of the transformation
/// \param Ctx transformation context to be used
void processModuleStatsChanges(ModuleStat &OldStat, ModuleStat &NewStat,
                               TransformationContext &Ctx) {
  if (!SILStatsModules && !SILStatsDumpAll)
    return;

  // Bail if no changes.
  if (OldStat == NewStat)
    return;

  // Compute deltas.
  double DeltaBlockCount = computeDelta(OldStat.BlockCount, NewStat.BlockCount);
  double DeltaInstCount = computeDelta(OldStat.InstCount, NewStat.InstCount);
  double DeltaFunctionCount =
      computeDelta(OldStat.FunctionCount, NewStat.FunctionCount);
  double DeltaUsedMemory = computeDelta(OldStat.UsedMemory, NewStat.UsedMemory);

  NewLineInserter nl;

  // Print delta for blocks only if it is above a threshold or we are asked to
  // dump all changes.
  if ((SILStatsDumpAll &&
       (DeltaBlockCount != 0.0 ||
        isFirstTimeData(OldStat.BlockCount, NewStat.BlockCount))) ||
      (std::abs(DeltaBlockCount) > BlockCountDeltaThreshold)) {
    stats_os() << nl.get();
    printCounterChange("module", "block", DeltaBlockCount, OldStat.BlockCount,
                       NewStat.BlockCount, Ctx);
  }

  // Print delta for instructions only if it is above a threshold or we are
  // asked to dump all changes.
  if ((SILStatsDumpAll &&
       (DeltaInstCount != 0.0 ||
        isFirstTimeData(OldStat.InstCount, NewStat.InstCount))) ||
      (std::abs(DeltaInstCount) > InstCountDeltaThreshold)) {
    stats_os() << nl.get();
    printCounterChange("module", "inst", DeltaInstCount, OldStat.InstCount,
                       NewStat.InstCount, Ctx);
  }

  // Print delta for functions only if it is above a threshold or we are
  // asked to dump all changes.
  if ((SILStatsDumpAll &&
       (DeltaFunctionCount != 0.0 ||
        isFirstTimeData(OldStat.FunctionCount, NewStat.FunctionCount))) ||
      (std::abs(DeltaFunctionCount) > FunctionCountDeltaThreshold)) {
    stats_os() << nl.get();
    printCounterChange("module", "functions", DeltaFunctionCount,
                       OldStat.FunctionCount, NewStat.FunctionCount, Ctx);
  }

  // Print delta for the used memory only if it is above a threshold or we are
  // asked to dump all changes.
  if ((SILStatsDumpAll &&
       (std::abs(DeltaUsedMemory) > UsedMemoryMinDeltaThreshold ||
        isFirstTimeData(OldStat.UsedMemory, NewStat.UsedMemory))) ||
      (std::abs(DeltaUsedMemory) > UsedMemoryDeltaThreshold)) {
    stats_os() << nl.get();
    printCounterChange("module", "memory", DeltaUsedMemory, OldStat.UsedMemory,
                       NewStat.UsedMemory, Ctx);
  }


  if (SILStatsDumpAll) {
    // Dump stats about the number of created and deleted instructions.
    auto DeltaCreatedInstCount =
        computeDelta(OldStat.CreatedInstCount, NewStat.CreatedInstCount);
    auto DeltaDeletedInstCount =
        computeDelta(OldStat.DeletedInstCount, NewStat.DeletedInstCount);
    if (DeltaCreatedInstCount != 0.0 ||
        isFirstTimeData(OldStat.CreatedInstCount, NewStat.CreatedInstCount))
      printCounterChange("module", "created_inst", DeltaCreatedInstCount,
                         OldStat.CreatedInstCount, NewStat.CreatedInstCount,
                         Ctx);
    if (DeltaDeletedInstCount != 0.0 ||
        isFirstTimeData(OldStat.DeletedInstCount, NewStat.DeletedInstCount))
      printCounterChange("module", "deleted_inst", DeltaDeletedInstCount,
                         OldStat.DeletedInstCount, NewStat.DeletedInstCount,
                         Ctx);
  }

  /// Dump collected instruction counts.
  if (!StatsOnlyInstructionsOptLoc.getInstCountsNum())
    return;

  for (auto kind : allSILInstructionKinds()) {
    // Do not print anything, if there is no change.
    if (OldStat.InstCounts[kind] == NewStat.InstCounts[kind])
      continue;
    if (!StatsOnlyInstructionsOptLoc.shouldComputeInstCount(kind))
      continue;
    SmallString<64> CounterName("inst_");
    CounterName += getSILInstructionName(kind);
    auto DeltaCounterKindCount =
        computeDelta(OldStat.InstCounts[kind], NewStat.InstCounts[kind]);
    printCounterChange("module", CounterName, DeltaCounterKindCount,
                       OldStat.InstCounts[kind], NewStat.InstCounts[kind], Ctx);
  }
}

/// Update the stats for a module after a SIL transform has been performed.
void OptimizerStatsAnalysis::updateModuleStats(TransformationContext &Ctx) {
  assert(&getModule() == &Ctx.getModule());

  auto &ModStat = getModuleStat();
  auto OldModStat = ModStat;
  ModuleStat NewModStat;

  // First, collect statistics that require scanning SILFunctions.
  if (OldModStat.FunctionCount == 0) {
    // This is the first time the stats are computed for the module.
    // Iterate over all functions in the module and compute the stats.
    for (auto &F : M) {
      auto &FuncStat = getFunctionStat(&F);
      FunctionStat NewFuncStat(&F);
      processFuncStatHistory(&F, NewFuncStat, Ctx);
      // Update module stats.
      NewModStat.addFunctionStat(NewFuncStat);
      // Update function stats.
      FuncStat = std::move(NewFuncStat);
    }
  } else {
    // Go only over functions that were changed since the last computation.
    // These are the functions that got invalidated, removed or added.
    //
    // If no functions were changed by the last executed transformation, then
    // the sets of invalidated, deleted and added functions will be empty and
    // no FunctionStats will be updated.
    //
    // FIXME: As a result, the current implementation does not record the fact
    // about performing a given transformation on a given function, if the
    // function was not changed during the transformation. This reduced the
    // amount of recorded information, but makes the history of transformations
    // on a given function incomplete. If this ever becomes an issue, we can
    // record all transformations, even if they do not change anything.
    NewModStat = OldModStat;

    // Process modified functions.
    while (!InvalidatedFuncs.empty()) {
      auto *F = InvalidatedFuncs.back();
      InvalidatedFuncs.pop_back();
      auto &FuncStat = getFunctionStat(F);
      auto &OldFuncStat = FuncStat;
      FunctionStat NewFuncStat(F);
      processFuncStatsChanges(F, OldFuncStat, NewFuncStat, Ctx);
      NewModStat.subFunctionStat(OldFuncStat);
      NewModStat.addFunctionStat(NewFuncStat);
      FuncStat = std::move(NewFuncStat);
    }

    // Process deleted functions.
    while (!DeletedFuncs.empty()) {
      auto *F = DeletedFuncs.back();
      DeletedFuncs.pop_back();
      auto &FuncStat = getFunctionStat(F);
      auto &OldFuncStat = FuncStat;
      FunctionStat NewFuncStat;
      processFuncStatsChanges(F, OldFuncStat, NewFuncStat, Ctx);
      NewModStat.subFunctionStat(OldFuncStat);
      Cache->deleteFunctionStat(F);
    }

    // Process added functions.
    while (!AddedFuncs.empty()) {
      auto *F = AddedFuncs.back();
      AddedFuncs.pop_back();
      auto &FuncStat = getFunctionStat(F);
      FunctionStat OldFuncStat;
      FunctionStat NewFuncStat(F);
      processFuncStatsChanges(F, OldFuncStat, NewFuncStat, Ctx);
      NewModStat.addFunctionStat(NewFuncStat);
      FuncStat = std::move(NewFuncStat);
    }
  }

  // Then collect some more general statistics, which do not require
  // any scanning of SILFunctions or the like.
  NewModStat.addMemoryStat();
  NewModStat.addCreatedAndDeletedInstructionsStat();

  // Process updates.
  processModuleStatsChanges(OldModStat, NewModStat, Ctx);

  // Remember the new state of the collected stats.
  ModStat = NewModStat;
}

FunctionStat::FunctionStat(SILFunction *F) {
  InstCountVisitor V(InstCounts, VarNames, DebugVariables);
  V.visitSILFunction(F);
  BlockCount = V.getBlockCount();
  InstCount = V.getInstCount();
}

} // end anonymous namespace

/// Updates SILModule stats after finishing executing the
/// transform \p Transform.
///
/// \param M SILModule to be processed
/// \param Transform the SIL transformation that was just executed
/// \param PM the PassManager being used
void swift::updateSILModuleStatsAfterTransform(SILModule &M,
                                               SILTransform *Transform,
                                               SILPassManager &PM,
                                               int PassNumber, int Duration) {
  if (!SILStatsModules && !SILStatsFunctions && !SILStatsLostVariables
      && !SILStatsDumpAll)
    return;
  TransformationContext Ctx(M, PM, Transform, PassNumber, Duration);
  OptimizerStatsAnalysis *Stats = PM.getAnalysis<OptimizerStatsAnalysis>();
  Stats->updateModuleStats(Ctx);
}

// This is just a hook for possible extensions in the future.
// It could be used e.g. to detect sequences of consecutive executions
// of the same transform.
void swift::updateSILModuleStatsBeforeTransform(SILModule &M,
                                                SILTransform *Transform,
                                                SILPassManager &PM,
                                                int PassNumber) {
  if (!SILStatsModules && !SILStatsFunctions)
    return;
}

SILAnalysis *swift::createOptimizerStatsAnalysis(SILModule *M) {
  return new OptimizerStatsAnalysis(M);
}
