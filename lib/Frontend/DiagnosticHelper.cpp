//===--- DiagnosticHelper.cpp - Diagnostic Helper ---------------*- C++ -*-===//
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
//
//  This file implements the DiagnosticHelper class.
//
//===----------------------------------------------------------------------===//

#include "swift/Frontend/DiagnosticHelper.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/ParseableOutput.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Frontend/AccumulatingDiagnosticConsumer.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Frontend/SerializedDiagnosticConsumer.h"
#include "swift/Migrator/FixitFilter.h"
#include "llvm/Support/raw_ostream.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#endif

using namespace swift;
using namespace swift::parseable_output;

class LLVM_LIBRARY_VISIBILITY DiagnosticHelper::Implementation {
  friend class DiagnosticHelper;

public:
  Implementation(CompilerInstance &instance, llvm::raw_pwrite_stream &OS,
                 bool useQuasiPID);

  void initDiagConsumers(CompilerInvocation &invocation);

  void beginMessage(CompilerInvocation &invocation,
                    ArrayRef<const char *> args);
  void endMessage(int retCode);
  void setSuppressOutput(bool suppressOutput);

  void diagnoseFatalError(const char *reason, bool shouldCrash);

private:
  ~Implementation() {
    assert(!diagInProcess && "endMessage is not called after begin");
  }

  bool diagInProcess = false;
  const int64_t OSPid;
  const sys::TaskProcessInformation procInfo;

  CompilerInstance &instance;
  llvm::raw_pwrite_stream &errOS;

  // potentially created diagnostic consumers.
  PrintingDiagnosticConsumer PDC;
  llvm::StringMap<std::vector<std::string>> FileSpecificDiagnostics;
  std::unique_ptr<DiagnosticConsumer> FileSpecificAccumulatingConsumer;
  std::unique_ptr<DiagnosticConsumer> SerializedConsumerDispatcher;
  std::unique_ptr<DiagnosticConsumer> FixItsConsumer;
};

namespace {

/// If there is an error with fixits it writes the fixits as edits in json
/// format.
class JSONFixitWriter : public DiagnosticConsumer,
                        public migrator::FixitFilter {
  std::string FixitsOutputPath;
  std::unique_ptr<llvm::raw_ostream> OSPtr;
  bool FixitAll;
  SourceEdits AllEdits;

public:
  JSONFixitWriter(std::string fixitsOutputPath,
                  const DiagnosticOptions &DiagOpts)
      : FixitsOutputPath(std::move(fixitsOutputPath)),
        FixitAll(DiagOpts.FixitCodeForAllDiagnostics) {}

private:
  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    if (!(FixitAll || shouldTakeFixit(Info)))
      return;
    for (const auto &Fix : Info.FixIts)
      AllEdits.addEdit(SM, Fix.getRange(), Fix.getText());
  }

  bool finishProcessing() override {
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    OS.reset(
        new llvm::raw_fd_ostream(FixitsOutputPath, EC, llvm::sys::fs::OF_None));
    if (EC) {
      // Create a temporary diagnostics engine to print the error to stderr.
      SourceManager dummyMgr;
      DiagnosticEngine DE(dummyMgr);
      PrintingDiagnosticConsumer PDC;
      DE.addConsumer(PDC);
      DE.diagnose(SourceLoc(), diag::cannot_open_file, FixitsOutputPath,
                  EC.message());
      return true;
    }

    swift::writeEditsInJson(AllEdits, *OS);
    return false;
  }
};

} // anonymous namespace

/// Creates a diagnostic consumer that handles dispatching diagnostics to
/// multiple output files, based on the supplementary output paths specified by
/// \p inputsAndOutputs.
///
/// If no output files are needed, returns null.
static std::unique_ptr<DiagnosticConsumer>
createDispatchingDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs,
    llvm::function_ref<std::unique_ptr<DiagnosticConsumer>(const InputFile &)>
        maybeCreateConsumerForDiagnosticsFrom) {

  // The "4" here is somewhat arbitrary. In practice we're going to have one
  // sub-consumer for each diagnostic file we're trying to output, which (again
  // in practice) is going to be 1 in WMO mode and equal to the number of
  // primary inputs in batch mode. That in turn is going to be "the number of
  // files we need to recompile in this build, divided by the number of jobs".
  // So a value of "4" here means that there would be no heap allocation on a
  // clean build of a module with up to 32 files on an 8-core machine, if the
  // user doesn't customize anything.
  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 4> subconsumers;

  inputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        if (auto consumer = maybeCreateConsumerForDiagnosticsFrom(input))
          subconsumers.emplace_back(input.getFileName(), std::move(consumer));
        return false;
      });
  // For batch mode, the compiler must sometimes swallow diagnostics pertaining
  // to non-primary files in order to avoid Xcode showing the same diagnostic
  // multiple times. So, create a diagnostic "eater" for those non-primary
  // files.
  //
  // This routine gets called in cases where no primary subconsumers are
  // created. Don't bother to create non-primary subconsumers if there aren't
  // any primary ones.
  //
  // To avoid introducing bugs into WMO or single-file modes, test for multiple
  // primaries.
  if (!subconsumers.empty() && inputsAndOutputs.hasMultiplePrimaryInputs()) {
    inputsAndOutputs.forEachNonPrimaryInput(
        [&](const InputFile &input) -> bool {
          subconsumers.emplace_back(input.getFileName(), nullptr);
          return false;
        });
  }

  return FileSpecificDiagnosticConsumer::consolidateSubconsumers(subconsumers);
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified by \p inputsAndOutputs.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createSerializedDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs,
    bool emitMacroExpansionFiles) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      inputsAndOutputs,
      [emitMacroExpansionFiles](
          const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
        auto serializedDiagnosticsPath = input.getSerializedDiagnosticsPath();
        if (serializedDiagnosticsPath.empty())
          return nullptr;
        return serialized_diagnostics::createConsumer(serializedDiagnosticsPath,
                                                      emitMacroExpansionFiles);
      });
}

/// Creates a diagnostic consumer that accumulates all emitted diagnostics as
/// compilation proceeds. The accumulated diagnostics are then emitted in the
/// frontend's parseable-output.
static std::unique_ptr<DiagnosticConsumer> createAccumulatingDiagnosticConsumer(
    const FrontendInputsAndOutputs &InputsAndOutputs,
    llvm::StringMap<std::vector<std::string>> &FileSpecificDiagnostics) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      InputsAndOutputs,
      [&](const InputFile &Input) -> std::unique_ptr<DiagnosticConsumer> {
        FileSpecificDiagnostics.try_emplace(Input.getFileName(),
                                            std::vector<std::string>());
        auto &DiagBufferRef = FileSpecificDiagnostics[Input.getFileName()];
        return std::make_unique<AccumulatingFileDiagnosticConsumer>(
            DiagBufferRef);
      });
}

/// Creates a diagnostic consumer that handles JSONFixIt diagnostics, based on
/// the supplementary output paths specified in \p options.
///
/// If no json fixit diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createJSONFixItDiagnosticConsumerIfNeeded(
    const CompilerInvocation &invocation) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      invocation.getFrontendOptions().InputsAndOutputs,
      [&](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
        auto fixItsOutputPath = input.getFixItsOutputPath();
        if (fixItsOutputPath.empty())
          return nullptr;
        return std::make_unique<JSONFixitWriter>(
            fixItsOutputPath.str(), invocation.getDiagnosticOptions());
      });
}

DiagnosticHelper::Implementation::Implementation(CompilerInstance &instance,
                                                 llvm::raw_pwrite_stream &OS,
                                                 bool useQuasiPID)
    : OSPid(useQuasiPID ? QUASI_PID_START : getpid()), procInfo(OSPid),
      instance(instance), errOS(OS), PDC(OS) {
  instance.addDiagnosticConsumer(&PDC);
}

void DiagnosticHelper::Implementation::initDiagConsumers(
    CompilerInvocation &invocation) {
  if (invocation.getFrontendOptions().FrontendParseableOutput) {
    // We need a diagnostic consumer that will, per-file, collect all
    // diagnostics to be reported in parseable-output
    FileSpecificAccumulatingConsumer = createAccumulatingDiagnosticConsumer(
        invocation.getFrontendOptions().InputsAndOutputs,
        FileSpecificDiagnostics);
    instance.addDiagnosticConsumer(FileSpecificAccumulatingConsumer.get());

    // If we got this far, we need to suppress the output of the
    // PrintingDiagnosticConsumer to ensure that only the parseable-output
    // is emitted
    PDC.setSuppressOutput(true);
  }

  // Because the serialized diagnostics consumer is initialized here,
  // diagnostics emitted above, within CompilerInvocation::parseArgs, are never
  // serialized. This is a non-issue because, in nearly all cases, frontend
  // arguments are generated by the driver, not directly by a user. The driver
  // is responsible for emitting diagnostics for its own errors.
  // See https://github.com/apple/swift/issues/45288 for details.
  SerializedConsumerDispatcher = createSerializedDiagnosticConsumerIfNeeded(
      invocation.getFrontendOptions().InputsAndOutputs,
      invocation.getDiagnosticOptions().EmitMacroExpansionFiles);
  if (SerializedConsumerDispatcher)
    instance.addDiagnosticConsumer(SerializedConsumerDispatcher.get());

  FixItsConsumer = createJSONFixItDiagnosticConsumerIfNeeded(invocation);
  if (FixItsConsumer)
    instance.addDiagnosticConsumer(FixItsConsumer.get());

  if (invocation.getDiagnosticOptions().UseColor)
    PDC.forceColors();

  PDC.setPrintEducationalNotes(
      invocation.getDiagnosticOptions().PrintEducationalNotes);

  PDC.setFormattingStyle(
      invocation.getDiagnosticOptions().PrintedFormattingStyle);

  PDC.setEmitMacroExpansionFiles(
      invocation.getDiagnosticOptions().EmitMacroExpansionFiles);
}

static const char *
mapFrontendInvocationToAction(const CompilerInvocation &Invocation) {
  FrontendOptions::ActionType ActionType =
      Invocation.getFrontendOptions().RequestedAction;
  switch (ActionType) {
  case FrontendOptions::ActionType::REPL:
    return "repl";
  case FrontendOptions::ActionType::MergeModules:
    return "merge-module";
  case FrontendOptions::ActionType::Immediate:
    return "interpret";
  case FrontendOptions::ActionType::TypecheckModuleFromInterface:
    return "verify-module-interface";
  case FrontendOptions::ActionType::EmitPCH:
    return "generate-pch";
  case FrontendOptions::ActionType::EmitIR:
  case FrontendOptions::ActionType::EmitBC:
  case FrontendOptions::ActionType::EmitAssembly:
  case FrontendOptions::ActionType::EmitObject:
    // Whether or not these actions correspond to a "compile" job or a
    // "backend" job, depends on the input kind.
    if (Invocation.getFrontendOptions().InputsAndOutputs.shouldTreatAsLLVM())
      return "backend";
    else
      return "compile";
  case FrontendOptions::ActionType::EmitModuleOnly:
    return "emit-module";
  default:
    return "compile";
  }
  // The following Driver/Parseable-output actions do not correspond to
  // possible Frontend invocations:
  // ModuleWrapJob, AutolinkExtractJob, GenerateDSYMJob, VerifyDebugInfoJob,
  // StaticLinkJob, DynamicLinkJob
}

// TODO: Apply elsewhere in the compiler
static swift::file_types::ID computeFileTypeForPath(const StringRef Path) {
  if (!llvm::sys::path::has_extension(Path))
    return swift::file_types::ID::TY_INVALID;

  auto Extension = llvm::sys::path::extension(Path).str();
  auto FileType = file_types::lookupTypeForExtension(Extension);
  if (FileType == swift::file_types::ID::TY_INVALID) {
    auto PathStem = llvm::sys::path::stem(Path);
    // If this path has a multiple '.' extension (e.g. .abi.json),
    // then iterate over all preceeding possible extension variants.
    while (llvm::sys::path::has_extension(PathStem)) {
      auto NextExtension = llvm::sys::path::extension(PathStem);
      PathStem = llvm::sys::path::stem(PathStem);
      Extension = NextExtension.str() + Extension;
      FileType = file_types::lookupTypeForExtension(Extension);
      if (FileType != swift::file_types::ID::TY_INVALID)
        break;
    }
  }

  return FileType;
}

static DetailedTaskDescription constructDetailedTaskDescription(
    const CompilerInvocation &Invocation, ArrayRef<InputFile> PrimaryInputs,
    ArrayRef<const char *> Args, bool isEmitModuleOnly = false) {
  // Command line and arguments
  std::string Executable = Invocation.getFrontendOptions().MainExecutablePath;
  SmallVector<std::string, 16> Arguments;
  std::string CommandLine;
  SmallVector<CommandInput, 4> Inputs;
  SmallVector<OutputPair, 8> Outputs;
  CommandLine += Executable;
  for (const auto &A : Args) {
    Arguments.push_back(A);
    CommandLine += std::string(" ") + A;
  }

  // Primary Inputs
  for (const auto &input : PrimaryInputs) {
    Inputs.push_back(CommandInput(input.getFileName()));
  }

  for (const auto &input : PrimaryInputs) {
    if (!isEmitModuleOnly) {
      // Main per-input outputs
      auto OutputFile = input.outputFilename();
      if (!OutputFile.empty())
        Outputs.push_back(
            OutputPair(computeFileTypeForPath(OutputFile), OutputFile));
    }

    // Supplementary outputs
    const auto &primarySpecificFiles = input.getPrimarySpecificPaths();
    const auto &supplementaryOutputPaths =
        primarySpecificFiles.SupplementaryOutputs;
    supplementaryOutputPaths.forEachSetOutput([&](const std::string &output) {
      Outputs.push_back(OutputPair(computeFileTypeForPath(output), output));
    });
  }
  return DetailedTaskDescription{Executable, Arguments, CommandLine, Inputs,
                                 Outputs};
}

void DiagnosticHelper::Implementation::beginMessage(
    CompilerInvocation &invocation, ArrayRef<const char *> args) {
  if (!invocation.getFrontendOptions().FrontendParseableOutput)
    return;

  diagInProcess = true;
  const auto &IO = invocation.getFrontendOptions().InputsAndOutputs;
  // Parseable output clients may not understand the idea of a batch
  // compilation. We assign each primary in a batch job a quasi process id,
  // making sure it cannot collide with a real PID (always positive). Non-batch
  // compilation gets a real OS PID.
  int64_t pid = IO.hasUniquePrimaryInput() ? OSPid : QUASI_PID_START;

  if (IO.hasPrimaryInputs()) {
    IO.forEachPrimaryInputWithIndex(
        [&](const InputFile &Input, unsigned idx) -> bool {
          ArrayRef<InputFile> Inputs(Input);
          emitBeganMessage(
              errOS, mapFrontendInvocationToAction(invocation),
              constructDetailedTaskDescription(invocation, Inputs, args),
              pid - idx, procInfo);
          return false;
        });
  } else {
    // If no primary inputs are present, we are in WMO or EmitModule.
    bool isEmitModule = invocation.getFrontendOptions().RequestedAction ==
                        FrontendOptions::ActionType::EmitModuleOnly;
    emitBeganMessage(errOS, mapFrontendInvocationToAction(invocation),
                     constructDetailedTaskDescription(
                         invocation, IO.getAllInputs(), args, isEmitModule),
                     OSPid, procInfo);
  }
}

void DiagnosticHelper::Implementation::endMessage(int retCode) {
  auto &invocation = instance.getInvocation();
  if (!invocation.getFrontendOptions().FrontendParseableOutput)
    return;

  const auto &IO = invocation.getFrontendOptions().InputsAndOutputs;

  // Parseable output clients may not understand the idea of a batch
  // compilation. We assign each primary in a batch job a quasi process id,
  // making sure it cannot collide with a real PID (always positive). Non-batch
  // compilation gets a real OS PID.
  int64_t pid = IO.hasUniquePrimaryInput() ? OSPid : QUASI_PID_START;

  if (IO.hasPrimaryInputs()) {
    IO.forEachPrimaryInputWithIndex([&](const InputFile &Input,
                                        unsigned idx) -> bool {
      assert(FileSpecificDiagnostics.count(Input.getFileName()) != 0 &&
             "Expected diagnostic collection for input.");

      // Join all diagnostics produced for this file into a single output.
      auto PrimaryDiags = FileSpecificDiagnostics.lookup(Input.getFileName());
      const char *const Delim = "";
      std::ostringstream JoinedDiags;
      std::copy(PrimaryDiags.begin(), PrimaryDiags.end(),
                std::ostream_iterator<std::string>(JoinedDiags, Delim));

      emitFinishedMessage(errOS,
                          mapFrontendInvocationToAction(invocation),
                          JoinedDiags.str(), retCode, pid - idx, procInfo);
      return false;
    });
  } else {
    // If no primary inputs are present, we are in WMO.
    std::vector<std::string> AllDiagnostics;
    for (const auto &FileDiagnostics : FileSpecificDiagnostics) {
      AllDiagnostics.insert(AllDiagnostics.end(),
                            FileDiagnostics.getValue().begin(),
                            FileDiagnostics.getValue().end());
    }
    const char *const Delim = "";
    std::ostringstream JoinedDiags;
    std::copy(AllDiagnostics.begin(), AllDiagnostics.end(),
              std::ostream_iterator<std::string>(JoinedDiags, Delim));
    emitFinishedMessage(errOS, mapFrontendInvocationToAction(invocation),
                        JoinedDiags.str(), retCode, OSPid, procInfo);
  }

  diagInProcess = false;
}

void DiagnosticHelper::Implementation::setSuppressOutput(bool suppressOutput) {
  PDC.setSuppressOutput(suppressOutput);
}

void DiagnosticHelper::Implementation::diagnoseFatalError(const char *reason,
                                                          bool shouldCrash) {
  static const char *recursiveFatalError = nullptr;
  if (recursiveFatalError) {
    // Report the /original/ error through LLVM's default handler, not
    // whatever we encountered.
    llvm::remove_fatal_error_handler();
    llvm::report_fatal_error(recursiveFatalError, shouldCrash);
  }
  recursiveFatalError = reason;

  SourceManager dummyMgr;

  DiagnosticInfo errorInfo(
      DiagID(0), SourceLoc(), DiagnosticKind::Error,
      "fatal error encountered during compilation; " SWIFT_BUG_REPORT_MESSAGE,
      {}, StringRef(), SourceLoc(), {}, {}, {}, false);
  DiagnosticInfo noteInfo(DiagID(0), SourceLoc(), DiagnosticKind::Note, reason,
                          {}, StringRef(), SourceLoc(), {}, {}, {}, false);
  PDC.handleDiagnostic(dummyMgr, errorInfo);
  PDC.handleDiagnostic(dummyMgr, noteInfo);
  if (shouldCrash)
    abort();
}

DiagnosticHelper DiagnosticHelper::create(CompilerInstance &instance,
                                          llvm::raw_pwrite_stream &OS,
                                          bool useQuasiPID) {
  return DiagnosticHelper(instance, OS, useQuasiPID);
}

DiagnosticHelper::DiagnosticHelper(CompilerInstance &instance,
                                   llvm::raw_pwrite_stream &OS,
                                   bool useQuasiPID)
    : Impl(*new Implementation(instance, OS, useQuasiPID)) {}

DiagnosticHelper::~DiagnosticHelper() { delete &Impl; }

void DiagnosticHelper::initDiagConsumers(CompilerInvocation &invocation) {
  Impl.initDiagConsumers(invocation);
}

void DiagnosticHelper::beginMessage(CompilerInvocation &invocation,
                                    ArrayRef<const char *> args) {
  Impl.beginMessage(invocation, args);
}

void DiagnosticHelper::endMessage(int retCode) { Impl.endMessage(retCode); }

void DiagnosticHelper::setSuppressOutput(bool suppressOutput) {
  Impl.setSuppressOutput(suppressOutput);
}

void DiagnosticHelper::diagnoseFatalError(const char *reason,
                                          bool shouldCrash) {
  Impl.diagnoseFatalError(reason, shouldCrash);
}
