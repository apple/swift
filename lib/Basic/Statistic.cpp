//===--- Statistic.cpp - Swift unified stats reporting --------------------===//
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

#include "clang/AST/Decl.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/Timer.h"
#include "swift/AST/Decl.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/SILFunction.h"
#include "swift/Driver/DependencyGraph.h"
#include "llvm/Config/config.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/raw_ostream.h"
#include <chrono>
#include <limits>

#ifdef HAVE_SYS_TIME_H
#include <sys/time.h>
#endif
#ifdef HAVE_SYS_RESOURCE_H
#include <sys/resource.h>
#endif

namespace swift {
using namespace llvm;
using namespace llvm::sys;

static int64_t
getChildrenMaxResidentSetSize() {
#if defined(HAVE_GETRUSAGE) && !defined(__HAIKU__)
  struct rusage RU;
  ::getrusage(RUSAGE_CHILDREN, &RU);
  int64_t M = static_cast<int64_t>(RU.ru_maxrss);
  if (M < 0)
    M = std::numeric_limits<int64_t>::max();
  return M;
#else
  return 0;
#endif
}

static std::string
makeFileName(StringRef Prefix,
             StringRef ProgramName,
             StringRef AuxName,
             StringRef Suffix) {
  std::string tmp;
  raw_string_ostream stream(tmp);
  auto now = std::chrono::system_clock::now();
  auto dur = now.time_since_epoch();
  auto usec = std::chrono::duration_cast<std::chrono::microseconds>(dur);
  stream << Prefix
         << "-" << usec.count()
         << "-" << ProgramName
         << "-" << AuxName
         << "-" << Process::GetRandomNumber()
         << "." << Suffix;
  return stream.str();
}

static std::string
makeStatsFileName(StringRef ProgramName,
                  StringRef AuxName) {
  return makeFileName("stats", ProgramName, AuxName, "json");
}

static std::string
makeTraceFileName(StringRef ProgramName,
                  StringRef AuxName) {
  return makeFileName("trace", ProgramName, AuxName, "csv");
}

// LLVM's statistics-reporting machinery is sensitive to filenames containing
// YAML-quote-requiring characters, which occur surprisingly often in the wild;
// we only need a recognizable and likely-unique name for a target here, not an
// exact filename, so we go with a crude approximation. Furthermore, to avoid
// parse ambiguities when "demangling" counters and filenames we exclude hyphens
// and slashes.
static std::string
cleanName(StringRef n) {
  std::string tmp;
  for (auto c : n) {
    if (('a' <= c && c <= 'z') ||
        ('A' <= c && c <= 'Z') ||
        ('0' <= c && c <= '9') ||
        (c == '.'))
      tmp += c;
    else
      tmp += '_';
  }
  return tmp;
}

static std::string
auxName(StringRef ModuleName,
        StringRef InputName,
        StringRef TripleName,
        StringRef OutputType,
        StringRef OptType) {
  if (InputName.empty()) {
    InputName = "all";
  }
  // Dispose of path prefix, which might make composite name too long.
  InputName = path::filename(InputName);
  if (OptType.empty()) {
    OptType = "Onone";
  }
  if (!OutputType.empty() && OutputType.front() == '.') {
    OutputType = OutputType.substr(1);
  }
  if (!OptType.empty() && OptType.front() == '-') {
    OptType = OptType.substr(1);
  }
  return (cleanName(ModuleName)
          + "-" + cleanName(InputName)
          + "-" + cleanName(TripleName)
          + "-" + cleanName(OutputType)
          + "-" + cleanName(OptType));
}

class UnifiedStatsReporter::RecursionSafeTimers {

  struct RecursionSafeTimer {
    llvm::Optional<SharedTimer> Timer;
    size_t RecursionDepth;
  };

  StringMap<RecursionSafeTimer> Timers;

public:

  void BeginTimer(StringRef Name) {
    RecursionSafeTimer &T = Timers[Name];
    if (T.RecursionDepth == 0) {
      T.Timer.emplace(Name);
    }
    T.RecursionDepth++;
  }

  void EndTimer(StringRef Name) {
    auto I = Timers.find(Name);
    assert(I != Timers.end());
    RecursionSafeTimer &T = I->getValue();
    assert(T.RecursionDepth != 0);
    T.RecursionDepth--;
    if (T.RecursionDepth == 0) {
      T.Timer.reset();
    }
  }
};

UnifiedStatsReporter::UnifiedStatsReporter(StringRef ProgramName,
                                           StringRef ModuleName,
                                           StringRef InputName,
                                           StringRef TripleName,
                                           StringRef OutputType,
                                           StringRef OptType,
                                           StringRef Directory,
                                           SourceManager *SM,
                                           clang::SourceManager *CSM,
                                           bool TraceEvents)
  : UnifiedStatsReporter(ProgramName,
                         auxName(ModuleName,
                                 InputName,
                                 TripleName,
                                 OutputType,
                                 OptType),
                         Directory,
                         SM, CSM, TraceEvents)
{
}

UnifiedStatsReporter::UnifiedStatsReporter(StringRef ProgramName,
                                           StringRef AuxName,
                                           StringRef Directory,
                                           SourceManager *SM,
                                           clang::SourceManager *CSM,
                                           bool TraceEvents)
  : currentProcessExitStatusSet(false),
    currentProcessExitStatus(EXIT_FAILURE),
    StatsFilename(Directory),
    TraceFilename(Directory),
    StartedTime(llvm::TimeRecord::getCurrentTime()),
    Timer(make_unique<NamedRegionTimer>(AuxName,
                                        "Building Target",
                                        ProgramName, "Running Program")),
    SourceMgr(SM),
    ClangSourceMgr(CSM),
    RecursiveTimers(llvm::make_unique<RecursionSafeTimers>())
{
  path::append(StatsFilename, makeStatsFileName(ProgramName, AuxName));
  path::append(TraceFilename, makeTraceFileName(ProgramName, AuxName));
  EnableStatistics(/*PrintOnExit=*/false);
  SharedTimer::enableCompilationTimers();
  if (TraceEvents)
    LastTracedFrontendCounters = make_unique<AlwaysOnFrontendCounters>();
}

UnifiedStatsReporter::AlwaysOnDriverCounters &
UnifiedStatsReporter::getDriverCounters()
{
  if (!DriverCounters)
    DriverCounters = make_unique<AlwaysOnDriverCounters>();
  return *DriverCounters;
}

UnifiedStatsReporter::AlwaysOnFrontendCounters &
UnifiedStatsReporter::getFrontendCounters()
{
  if (!FrontendCounters)
    FrontendCounters = make_unique<AlwaysOnFrontendCounters>();
  return *FrontendCounters;
}

void
UnifiedStatsReporter::noteCurrentProcessExitStatus(int status) {
  assert(!currentProcessExitStatusSet);
  currentProcessExitStatusSet = true;
  currentProcessExitStatus = status;
}

void
UnifiedStatsReporter::publishAlwaysOnStatsToLLVM() {
  if (FrontendCounters) {
    auto &C = getFrontendCounters();
#define FRONTEND_STATISTIC(TY, NAME)                            \
    do {                                                        \
      static Statistic Stat = {#TY, #NAME, #NAME, {0}, false};  \
      Stat += (C).NAME;                                         \
    } while (0);
#include "swift/Basic/Statistics.def"
#undef FRONTEND_STATISTIC
  }
  if (DriverCounters) {
    auto &C = getDriverCounters();
#define DRIVER_STATISTIC(NAME)                                       \
    do {                                                             \
      static Statistic Stat = {"Driver", #NAME, #NAME, {0}, false};  \
      Stat += (C).NAME;                                              \
    } while (0);
#include "swift/Basic/Statistics.def"
#undef DRIVER_STATISTIC
  }
}

void
UnifiedStatsReporter::printAlwaysOnStatsAndTimers(raw_ostream &OS) {
  // Adapted from llvm::PrintStatisticsJSON
  OS << "{\n";
  const char *delim = "";
  if (FrontendCounters) {
    auto &C = getFrontendCounters();
#define FRONTEND_STATISTIC(TY, NAME)                        \
    do {                                                    \
      OS << delim << "\t\"" #TY "." #NAME "\": " << C.NAME; \
      delim = ",\n";                                        \
    } while (0);
#include "swift/Basic/Statistics.def"
#undef FRONTEND_STATISTIC
  }
  if (DriverCounters) {
    auto &C = getDriverCounters();
#define DRIVER_STATISTIC(NAME)                              \
    do {                                                    \
      OS << delim << "\t\"Driver." #NAME "\": " << C.NAME;  \
      delim = ",\n";                                        \
    } while (0);
#include "swift/Basic/Statistics.def"
#undef DRIVER_STATISTIC
  }
  // Print timers.
  TimerGroup::printAllJSONValues(OS, delim);
  OS << "\n}\n";
  OS.flush();
}

FrontendStatsTracer::FrontendStatsTracer(
    UnifiedStatsReporter *Reporter, StringRef EventName, const void *Entity,
    const UnifiedStatsReporter::TraceFormatter *Formatter)
    : Reporter(Reporter), SavedTime(), EventName(EventName), Entity(Entity),
      Formatter(Formatter) {
  if (Reporter) {
    SavedTime = llvm::TimeRecord::getCurrentTime();
    Reporter->saveAnyFrontendStatsEvents(*this, true);
  }
}

FrontendStatsTracer::FrontendStatsTracer() = default;

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S)
    : FrontendStatsTracer(R, S, nullptr, nullptr)
{}

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S,
                                         const Decl *D)
    : FrontendStatsTracer(R, S, D, getTraceFormatter<const Decl *>())
{}

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S,
                                         const ProtocolConformance *P)
    : FrontendStatsTracer(R, S, P,
                          getTraceFormatter<const ProtocolConformance *>()) {}

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S,
                                         const Expr *E)
    : FrontendStatsTracer(R, S, E, getTraceFormatter<const Expr *>())
{}

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S,
                                         const clang::Decl *D)
    : FrontendStatsTracer(R, S, D, getTraceFormatter<const clang::Decl *>())
{}

FrontendStatsTracer::FrontendStatsTracer(UnifiedStatsReporter *R, StringRef S,
                                         const SILFunction *F)
    : FrontendStatsTracer(R, S, F, getTraceFormatter<const SILFunction *>())
{}

FrontendStatsTracer&
FrontendStatsTracer::operator=(FrontendStatsTracer&& other)
{
  Reporter = other.Reporter;
  SavedTime = other.SavedTime;
  EventName = other.EventName;
  Entity = other.Entity;
  Formatter = other.Formatter;
  other.Reporter = nullptr;
  return *this;
}

FrontendStatsTracer::FrontendStatsTracer(FrontendStatsTracer&& other)
  : Reporter(other.Reporter),
    SavedTime(other.SavedTime),
    EventName(other.EventName),
    Entity(other.Entity),
    Formatter(other.Formatter)
{
  other.Reporter = nullptr;
}

FrontendStatsTracer::~FrontendStatsTracer()
{
  if (Reporter)
    Reporter->saveAnyFrontendStatsEvents(*this, false);
}

void
UnifiedStatsReporter::saveAnyFrontendStatsEvents(
    FrontendStatsTracer const& T,
    bool IsEntry)
{
  // First make a note in the recursion-safe timers; these
  // are active anytime UnifiedStatsReporter is active.
  if (IsEntry) {
    RecursiveTimers->BeginTimer(T.EventName);
  } else {
    RecursiveTimers->EndTimer(T.EventName);
  }

  // If we don't have a saved entry to form deltas against in
  // the trace buffer, we're not tracing: return early.
  if (!LastTracedFrontendCounters)
    return;
  auto Now = llvm::TimeRecord::getCurrentTime();
  auto StartUS = uint64_t(1000000.0 * T.SavedTime.getProcessTime());
  auto NowUS = uint64_t(1000000.0 * Now.getProcessTime());
  auto LiveUS = IsEntry ? 0 : NowUS - StartUS;
  auto &C = getFrontendCounters();
#define FRONTEND_STATISTIC(TY, NAME)                          \
  do {                                                        \
    int64_t total = C.NAME;                                    \
    int64_t delta = C.NAME - LastTracedFrontendCounters->NAME; \
    static char const *name = #TY "." #NAME;                  \
    if (delta != 0) {                                         \
      LastTracedFrontendCounters->NAME = C.NAME;              \
      FrontendStatsEvents.emplace_back(FrontendStatsEvent {   \
          NowUS, LiveUS, IsEntry, T.EventName, name,          \
            delta, total, T.Entity, T.Formatter});            \
    }                                                         \
  } while (0);
#include "swift/Basic/Statistics.def"
#undef FRONTEND_STATISTIC
}

UnifiedStatsReporter::TraceFormatter::~TraceFormatter() {}

UnifiedStatsReporter::~UnifiedStatsReporter()
{
  // If nobody's marked this process as successful yet,
  // mark it as failing.
  if (currentProcessExitStatus != EXIT_SUCCESS) {
    if (FrontendCounters) {
      auto &C = getFrontendCounters();
      C.NumProcessFailures++;
    } else {
      auto &C = getDriverCounters();
      C.NumProcessFailures++;
    }
  }

  // NB: Timer needs to be Optional<> because it needs to be destructed early;
  // LLVM will complain about double-stopping a timer if you tear down a
  // NamedRegionTimer after printing all timers. The printing routines were
  // designed with more of a global-scope, run-at-process-exit in mind, which
  // we're repurposing a bit here.
  Timer.reset();

  // We currently do this by manual TimeRecord keeping because LLVM has decided
  // not to allow access to the Timers inside NamedRegionTimers.
  auto ElapsedTime = llvm::TimeRecord::getCurrentTime();
  ElapsedTime -= StartedTime;

  if (DriverCounters) {
    auto &C = getDriverCounters();
    C.ChildrenMaxRSS = getChildrenMaxResidentSetSize();
  }

  if (FrontendCounters) {
    auto &C = getFrontendCounters();
    // Convenience calculation for crude top-level "absolute speed".
    if (C.NumSourceLines != 0 && ElapsedTime.getProcessTime() != 0.0)
      C.NumSourceLinesPerSecond = (int64_t) (((double)C.NumSourceLines) /
                                             ElapsedTime.getProcessTime());
  }

  std::error_code EC;
  raw_fd_ostream ostream(StatsFilename, EC, fs::F_Append | fs::F_Text);
  if (EC) {
    llvm::errs() << "Error opening -stats-output-dir file '"
                 << TraceFilename << "' for writing\n";
    return;
  }

  // We change behavior here depending on whether -DLLVM_ENABLE_STATS and/or
  // assertions were on in this build; this is somewhat subtle, but turning on
  // all stats for all of LLVM and clang is a bit more expensive and intrusive
  // than we want to be in release builds.
  //
  //  - If enabled: we copy all of our "always-on" local stats into LLVM's
  //    global statistics list, and ask LLVM to manage the printing of them.
  //
  //  - If disabled: we still have our "always-on" local stats to write, and
  //    LLVM's global _timers_ were still enabled (they're runtime-enabled, not
  //    compile-time) so we sequence printing our own stats and LLVM's timers
  //    manually.

#if !defined(NDEBUG) || defined(LLVM_ENABLE_STATS)
  publishAlwaysOnStatsToLLVM();
  PrintStatisticsJSON(ostream);
#else
  printAlwaysOnStatsAndTimers(ostream);
#endif

  if (LastTracedFrontendCounters && SourceMgr) {
    std::error_code EC;
    raw_fd_ostream tstream(TraceFilename, EC, fs::F_Append | fs::F_Text);
    if (EC) {
      llvm::errs() << "Error opening -trace-stats-events file '"
                   << TraceFilename << "' for writing\n";
      return;
    }
    tstream << "Time,Live,IsEntry,EventName,CounterName,"
            << "CounterDelta,CounterValue,EntityName,EntityRange\n";
    for (auto const &E : FrontendStatsEvents) {
      tstream << E.TimeUSec << ','
              << E.LiveUSec << ','
              << (E.IsEntry ? "\"entry\"," : "\"exit\",")
              << '"' << E.EventName << '"' << ','
              << '"' << E.CounterName << '"' << ','
              << E.CounterDelta << ','
              << E.CounterValue << ',';
      tstream << '"';
      if (E.Formatter)
        E.Formatter->traceName(E.Entity, tstream);
      tstream << '"' << ',';
      tstream << '"';
      if (E.Formatter)
        E.Formatter->traceLoc(E.Entity, SourceMgr, ClangSourceMgr, tstream);
      tstream << '"' << '\n';
    }
  }
}

} // namespace swift
