//===--- FrontendTool.cpp - Swift Compiler Frontend -----------------------===//
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
///
/// \file
/// This is the entry point to the swift -frontend functionality, which
/// implements the core compiler functionality along with a number of additional
/// tools for demonstration and testing purposes.
///
/// This is separate from the rest of libFrontend to reduce the dependencies
/// required by that library.
///
//===----------------------------------------------------------------------===//

#include "swift/FrontendTool/FrontendTool.h"
#include "swift/DependencyScan/ScanDependencies.h"
#include "Dependencies.h"
#include "TBD.h"
#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/ParseableOutput.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/UUID.h"
#include "swift/Option/Options.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/AccumulatingDiagnosticConsumer.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Frontend/SerializedDiagnosticConsumer.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Immediate/Immediate.h"
#include "swift/Index/IndexRecord.h"
#include "swift/Option/Options.h"
#include "swift/Migrator/FixitFilter.h"
#include "swift/Migrator/Migrator.h"
#include "swift/PrintAsObjC/PrintAsObjC.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Syntax/Serialization/SyntaxSerialization.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "swift/TBDGen/TBDGen.h"
#include "swift/SIL/ModuleSummary.h"
#include "swift/Serialization/ModuleSummary.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Option/Option.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"

#if __has_include(<unistd.h>)
#include <unistd.h>
#elif defined(_WIN32)
#include <process.h>
#endif
#include <algorithm>
#include <memory>
#include <unordered_set>
#include <utility>

using namespace swift;
using namespace swift::parseable_output;

static std::string displayName(StringRef MainExecutablePath) {
  std::string Name = llvm::sys::path::stem(MainExecutablePath).str();
  Name += " -frontend";
  return Name;
}

static void emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts) {
  opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &f) -> bool {
        return swift::emitMakeDependenciesIfNeeded(diags, depTracker, opts, f);
      });
}

static void
emitLoadedModuleTraceForAllPrimariesIfNeeded(ModuleDecl *mainModule,
                                             DependencyTracker *depTracker,
                                             const FrontendOptions &opts) {
  opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        return swift::emitLoadedModuleTraceIfNeeded(mainModule, depTracker,
                                                    opts, input);
      });
}

/// Gets an output stream for the provided output filename, or diagnoses to the
/// provided AST Context and returns null if there was an error getting the
/// stream.
static std::unique_ptr<llvm::raw_fd_ostream>
getFileOutputStream(StringRef OutputFilename, ASTContext &Ctx) {
  std::error_code errorCode;
  auto os = std::make_unique<llvm::raw_fd_ostream>(
              OutputFilename, errorCode, llvm::sys::fs::F_None);
  if (errorCode) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_output,
                       OutputFilename, errorCode.message());
    return nullptr;
  }
  return os;
}

/// Writes the Syntax tree to the given file
static bool emitSyntax(const SourceFile &SF, StringRef OutputFilename) {
  auto os = getFileOutputStream(OutputFilename, SF.getASTContext());
  if (!os) return true;

  json::Output jsonOut(*os, /*UserInfo=*/{}, /*PrettyPrint=*/false);
  auto Root = SF.getSyntaxRoot().getRaw();
  jsonOut << *Root;
  *os << "\n";
  return false;
}

/// Writes SIL out to the given file.
static bool writeSIL(SILModule &SM, ModuleDecl *M, const SILOptions &Opts,
                     StringRef OutputFilename) {
  auto OS = getFileOutputStream(OutputFilename, M->getASTContext());
  if (!OS) return true;
  SM.print(*OS, M, Opts);

  return M->getASTContext().hadError();
}

static bool writeSIL(SILModule &SM, const PrimarySpecificPaths &PSPs,
                     const CompilerInstance &Instance,
                     const SILOptions &Opts) {
  return writeSIL(SM, Instance.getMainModule(), Opts,
                  PSPs.OutputFilename);
}

/// Prints the Objective-C "generated header" interface for \p M to \p
/// outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::printAsObjC
static bool printAsObjCIfNeeded(StringRef outputPath, ModuleDecl *M,
                                StringRef bridgingHeader, bool moduleIsPublic) {
  if (outputPath.empty())
    return false;
  return withOutputFile(M->getDiags(), outputPath,
                        [&](raw_ostream &out) -> bool {
    auto requiredAccess = moduleIsPublic ? AccessLevel::Public
                                         : AccessLevel::Internal;
    return printAsObjC(out, M, bridgingHeader, requiredAccess);
  });
}

/// Prints the stable module interface for \p M to \p outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::emitSwiftInterface
static bool
printModuleInterfaceIfNeeded(StringRef outputPath,
                             ModuleInterfaceOptions const &Opts,
                             LangOptions const &LangOpts,
                             ModuleDecl *M) {
  if (outputPath.empty())
    return false;

  DiagnosticEngine &diags = M->getDiags();
  if (!LangOpts.isSwiftVersionAtLeast(5)) {
    assert(LangOpts.isSwiftVersionAtLeast(4));
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_swift_version,
                   LangOpts.isSwiftVersionAtLeast(4, 2) ? "4.2" : "4");
  }
  if (M->getResilienceStrategy() != ResilienceStrategy::Resilient) {
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_library_evolution);
  }
  return withOutputFile(diags, outputPath,
                        [M, Opts](raw_ostream &out) -> bool {
    return swift::emitSwiftInterface(out, Opts, M);
  });
}

namespace {

/// If there is an error with fixits it writes the fixits as edits in json
/// format.
class JSONFixitWriter
  : public DiagnosticConsumer, public migrator::FixitFilter {
  std::string FixitsOutputPath;
  std::unique_ptr<llvm::raw_ostream> OSPtr;
  bool FixitAll;
  std::vector<SingleEdit> AllEdits;

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
    for (const auto &Fix : Info.FixIts) {
      AllEdits.push_back({SM, Fix.getRange(), Fix.getText().str()});
    }
  }

  bool finishProcessing() override {
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    OS.reset(new llvm::raw_fd_ostream(FixitsOutputPath,
                                      EC,
                                      llvm::sys::fs::F_None));
    if (EC) {
      // Create a temporary diagnostics engine to print the error to stderr.
      SourceManager dummyMgr;
      DiagnosticEngine DE(dummyMgr);
      PrintingDiagnosticConsumer PDC;
      DE.addConsumer(PDC);
      DE.diagnose(SourceLoc(), diag::cannot_open_file,
                  FixitsOutputPath, EC.message());
      return true;
    }

    swift::writeEditsInJson(llvm::makeArrayRef(AllEdits), *OS);
    return false;
  }
};

} // anonymous namespace

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithAssertion() {
  // Per the user's request, this assertion should always fail in
  // builds with assertions enabled.

  // This should not be converted to llvm_unreachable, as those are
  // treated as optimization hints in builds where they turn into
  // __builtin_unreachable().
  assert((0) && "This is an assertion!");
}

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithCrash() {
  LLVM_BUILTIN_TRAP;
}

static void countStatsOfSourceFile(UnifiedStatsReporter &Stats,
                                   const CompilerInstance &Instance,
                                   SourceFile *SF) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumDecls += SF->getTopLevelDecls().size();
  C.NumLocalTypeDecls += SF->LocalTypeDecls.size();
  C.NumObjCMethods += SF->ObjCMethods.size();

  SmallVector<OperatorDecl *, 2> operators;
  SF->getOperatorDecls(operators);
  C.NumOperators += operators.size();

  SmallVector<PrecedenceGroupDecl *, 2> groups;
  SF->getPrecedenceGroups(groups);
  C.NumPrecedenceGroups += groups.size();

  auto bufID = SF->getBufferID();
  if (bufID.hasValue()) {
    C.NumSourceLines +=
      SM.getEntireTextForBuffer(bufID.getValue()).count('\n');
  }
}

static void countASTStats(UnifiedStatsReporter &Stats,
                          CompilerInstance& Instance) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumSourceBuffers = SM.getLLVMSourceMgr().getNumBuffers();
  C.NumLinkLibraries = Instance.getLinkLibraries().size();

  auto const &AST = Instance.getASTContext();
  C.NumLoadedModules = AST.getNumLoadedModules();

  if (auto *D = Instance.getDependencyTracker()) {
    C.NumDependencies = D->getDependencies().size();
    C.NumIncrementalDependencies = D->getIncrementalDependencies().size();
  }

  for (auto SF : Instance.getPrimarySourceFiles()) {
    auto &Ctx = SF->getASTContext();
    Ctx.evaluator.enumerateReferencesInFile(SF, [&C](const auto &ref) {
    using NodeKind = evaluator::DependencyCollector::Reference::Kind;
      switch (ref.kind) {
      case NodeKind::Empty:
      case NodeKind::Tombstone:
        llvm_unreachable("Cannot enumerate dead dependency!");
      case NodeKind::TopLevel:
        C.NumReferencedTopLevelNames += 1;
        return;
      case NodeKind::Dynamic:
        C.NumReferencedDynamicNames += 1;
        return;
      case NodeKind::PotentialMember:
      case NodeKind::UsedMember:
        C.NumReferencedMemberNames += 1;
        return;
      }
    });
  }

  if (!Instance.getPrimarySourceFiles().empty()) {
    for (auto SF : Instance.getPrimarySourceFiles())
      countStatsOfSourceFile(Stats, Instance, SF);
  } else if (auto *M = Instance.getMainModule()) {
    // No primary source file, but a main module; this is WMO-mode
    for (auto *F : M->getFiles()) {
      if (auto *SF = dyn_cast<SourceFile>(F)) {
        countStatsOfSourceFile(Stats, Instance, SF);
      }
    }
  }
}

static void countStatsPostSILGen(UnifiedStatsReporter &Stats,
                                 const SILModule& Module) {
  auto &C = Stats.getFrontendCounters();
  // FIXME: calculate these in constant time, via the dense maps.
  C.NumSILGenFunctions += Module.getFunctionList().size();
  C.NumSILGenVtables += Module.getVTables().size();
  C.NumSILGenWitnessTables += Module.getWitnessTableList().size();
  C.NumSILGenDefaultWitnessTables += Module.getDefaultWitnessTableList().size();
  C.NumSILGenGlobalVariables += Module.getSILGlobalList().size();
}

static bool precompileBridgingHeader(const CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  auto &ImporterOpts = Invocation.getClangImporterOptions();
  auto &PCHOutDir = ImporterOpts.PrecompiledHeaderOutputDir;
  if (!PCHOutDir.empty()) {
    // Create or validate a persistent PCH.
    auto SwiftPCHHash = Invocation.getPCHHash();
    auto PCH = clangImporter->getOrCreatePCH(ImporterOpts, SwiftPCHHash);
    return !PCH.hasValue();
  }
  return clangImporter->emitBridgingPCH(
      opts.InputsAndOutputs.getFilenameOfFirstInput(),
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool precompileClangModule(const CompilerInstance &Instance) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->emitPrecompiledModule(
      opts.InputsAndOutputs.getFilenameOfFirstInput(), opts.ModuleName,
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool dumpPrecompiledClangModule(const CompilerInstance &Instance) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->dumpPrecompiledModule(
      opts.InputsAndOutputs.getFilenameOfFirstInput(),
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool buildModuleFromInterface(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const FrontendOptions &FEOpts = Invocation.getFrontendOptions();
  assert(FEOpts.InputsAndOutputs.hasSingleInput());
  StringRef InputPath = FEOpts.InputsAndOutputs.getFilenameOfFirstInput();
  StringRef PrebuiltCachePath = FEOpts.PrebuiltModuleCachePath;
  ModuleInterfaceLoaderOptions LoaderOpts(FEOpts);
  return ModuleInterfaceLoader::buildSwiftModuleFromSwiftInterface(
      Instance.getSourceMgr(), Instance.getDiags(),
      Invocation.getSearchPathOptions(), Invocation.getLangOptions(),
      Invocation.getClangImporterOptions(),
      Invocation.getClangModuleCachePath(), PrebuiltCachePath,
      Invocation.getModuleName(), InputPath, Invocation.getOutputFilename(),
      FEOpts.SerializeModuleInterfaceDependencyHashes,
      FEOpts.shouldTrackSystemDependencies(), LoaderOpts,
      RequireOSSAModules_t(Invocation.getSILOptions()));
}

static bool compileLLVMIR(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &inputsAndOutputs =
      Invocation.getFrontendOptions().InputsAndOutputs;
  // Load in bitcode file.
  assert(inputsAndOutputs.hasSingleInput() &&
         "We expect a single input for bitcode input!");
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      swift::vfs::getFileOrSTDIN(Instance.getFileSystem(),
                                 inputsAndOutputs.getFilenameOfFirstInput());

  if (!FileBufOrErr) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                 inputsAndOutputs.getFilenameOfFirstInput(),
                                 FileBufOrErr.getError().message());
    return true;
  }
  llvm::MemoryBuffer *MainFile = FileBufOrErr.get().get();

  llvm::SMDiagnostic Err;
  auto LLVMContext = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> Module =
      llvm::parseIR(MainFile->getMemBufferRef(), Err, *LLVMContext.get());
  if (!Module) {
    // TODO: Translate from the diagnostic info to the SourceManager location
    // if available.
    Instance.getDiags().diagnose(SourceLoc(), diag::error_parse_input_file,
                                 inputsAndOutputs.getFilenameOfFirstInput(),
                                 Err.getMessage());
    return true;
  }
  return performLLVM(Invocation.getIRGenOptions(), Instance.getASTContext(),
                     Module.get(), inputsAndOutputs.getSingleOutputFilename());
}

static void verifyGenericSignaturesIfNeeded(const FrontendOptions &opts,
                                            ASTContext &Context) {
  auto verifyGenericSignaturesInModule = opts.VerifyGenericSignaturesInModule;
  if (verifyGenericSignaturesInModule.empty())
    return;
  if (auto module = Context.getModuleByName(verifyGenericSignaturesInModule))
    GenericSignatureBuilder::verifyGenericSignaturesInModule(module);
}

static bool dumpAndPrintScopeMap(const CompilerInstance &Instance,
                                 SourceFile &SF) {
  // Not const because may require reexpansion
  ASTScope &scope = SF.getScope();

  const auto &opts = Instance.getInvocation().getFrontendOptions();
  if (opts.DumpScopeMapLocations.empty()) {
    llvm::errs() << "***Complete scope map***\n";
    scope.buildFullyExpandedTree();
    scope.print(llvm::errs());
    return Instance.getASTContext().hadError();
  }
  // Probe each of the locations, and dump what we find.
  for (auto lineColumn : opts.DumpScopeMapLocations) {
    scope.buildFullyExpandedTree();
    scope.dumpOneScopeMapLocation(lineColumn);
  }
  return Instance.getASTContext().hadError();
}

static SourceFile &
getPrimaryOrMainSourceFile(const CompilerInstance &Instance) {
  if (SourceFile *SF = Instance.getPrimarySourceFile()) {
    return *SF;
  }
  return Instance.getMainModule()->getMainSourceFile();
}

/// Dumps the AST of all available primary source files. If corresponding output
/// files were specified, use them; otherwise, dump the AST to stdout.
static bool dumpAST(CompilerInstance &Instance) {
  auto primaryFiles = Instance.getPrimarySourceFiles();
  if (!primaryFiles.empty()) {
    for (SourceFile *sourceFile: primaryFiles) {
      auto PSPs = Instance.getPrimarySpecificPathsForSourceFile(*sourceFile);
      auto OutputFilename = PSPs.OutputFilename;
      auto OS = getFileOutputStream(OutputFilename, Instance.getASTContext());
      sourceFile->dump(*OS, /*parseIfNeeded*/ true);
    }
  } else {
    // Some invocations don't have primary files. In that case, we default to
    // looking for the main file and dumping it to `stdout`.
    auto &SF = getPrimaryOrMainSourceFile(Instance);
    SF.dump(llvm::outs(), /*parseIfNeeded*/ true);
  }
  return Instance.getASTContext().hadError();
}

static bool emitReferenceDependencies(CompilerInstance &Instance,
                                      SourceFile *const SF,
                                      StringRef outputPath) {
  const auto alsoEmitDotFile = Instance.getInvocation()
                                   .getLangOptions()
                                   .EmitFineGrainedDependencySourcefileDotFiles;

  // Before writing to the dependencies file path, preserve any previous file
  // that may have been there. No error handling -- this is just a nicety, it
  // doesn't matter if it fails.
  llvm::sys::fs::rename(outputPath, outputPath + "~");

  using SourceFileDepGraph = fine_grained_dependencies::SourceFileDepGraph;
  return fine_grained_dependencies::withReferenceDependencies(
      SF, *Instance.getDependencyTracker(), outputPath, alsoEmitDotFile,
      [&](SourceFileDepGraph &&g) -> bool {
        const bool hadError =
            fine_grained_dependencies::writeFineGrainedDependencyGraphToPath(
                Instance.getDiags(), outputPath, g);

        // If path is stdout, cannot read it back, so check for "-"
        assert(outputPath == "-" || g.verifyReadsWhatIsWritten(outputPath));

        if (alsoEmitDotFile)
          g.emitDotFile(outputPath, Instance.getDiags());
        return hadError;
      });
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
    default:
      return "compile";
  }
  // The following Driver/Parseable-output actions do not correspond to
  // possible Frontend invocations:
  // ModuleWrapJob, AutolinkExtractJob, GenerateDSYMJob, VerifyDebugInfoJob,
  // StaticLinkJob, DynamicLinkJob
}

static DetailedTaskDescription
constructDetailedTaskDescription(const CompilerInvocation &Invocation,
                                 const InputFile &PrimaryInput,
                                 ArrayRef<const char *> Args) {
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

  // Primary Input only
  Inputs.push_back(CommandInput(PrimaryInput.getFileName()));

  // Output for this Primary
  auto OutputFile = PrimaryInput.outputFilename();
  Outputs.push_back(OutputPair(file_types::lookupTypeForExtension(
                                   llvm::sys::path::extension(OutputFile)),
                               OutputFile));

  // Supplementary outputs
  const auto &primarySpecificFiles = PrimaryInput.getPrimarySpecificPaths();
  const auto &supplementaryOutputPaths =
      primarySpecificFiles.SupplementaryOutputs;
  supplementaryOutputPaths.forEachSetOutput([&](const std::string &output) {
    Outputs.push_back(OutputPair(
        file_types::lookupTypeForExtension(llvm::sys::path::extension(output)),
        output));
  });
  return DetailedTaskDescription{Executable, Arguments, CommandLine, Inputs,
                                 Outputs};
}

static void emitSwiftdepsForAllPrimaryInputsIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasReferenceDependenciesPath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::emit_reference_dependencies_without_primary_file);
    return;
  }

  // Do not write out swiftdeps for any primaries if we've encountered an
  // error. Without this, the driver will attempt to integrate swiftdeps
  // from broken swift files. One way this could go wrong is if a primary that
  // fails to build in an early wave has dependents in a later wave. The
  // driver will not schedule those later dependents after this batch exits,
  // so they will have no opportunity to bring their swiftdeps files up to
  // date. With this early exit, the driver sees the same priors in the
  // swiftdeps files from before errors were introduced into the batch, and
  // integration therefore always hops from "known good" to "known good" states.
  //
  // FIXME: It seems more appropriate for the driver to notice the early-exit
  // and react by always enqueuing the jobs it dropped in the other waves.
  //
  // We will output a module if allowing errors, so ignore that case.
  if (Instance.getDiags().hadAnyError() &&
      !Invocation.getFrontendOptions().AllowModuleWithCompilerErrors)
    return;

  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &referenceDependenciesFilePath =
        Invocation.getReferenceDependenciesFilePathForPrimary(
            SF->getFilename());
    if (referenceDependenciesFilePath.empty()) {
      continue;
    }

    emitReferenceDependencies(Instance, SF, referenceDependenciesFilePath);
  }
}

static bool writeTBDIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  const auto &tbdOpts = Invocation.getTBDGenOptions();
  if (!frontendOpts.InputsAndOutputs.hasTBDPath())
    return false;

  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_only_supported_in_whole_module);
    return false;
  }

  if (Invocation.getSILOptions().CrossModuleOptimization) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_not_supported_with_cmo);
    return false;
  }

  const std::string &TBDPath = Invocation.getTBDPathForWholeModule();

  return writeTBD(Instance.getMainModule(), TBDPath, tbdOpts);
}

static std::string changeToLdAdd(StringRef ldHide) {
  SmallString<64> SymbolBuffer;
  llvm::raw_svector_ostream OS(SymbolBuffer);
  auto Parts = ldHide.split("$hide$");
  assert(!Parts.first.empty());
  assert(!Parts.second.empty());
  OS << Parts.first << "$add$" << Parts.second;
  return OS.str().str();
}

static bool writeLdAddCFileIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  if (!frontendOpts.InputsAndOutputs.isWholeModule())
    return false;
  auto Path = Invocation.getLdAddCFileOutputPathForWholeModule();
  if (Path.empty())
    return false;
  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_only_supported_in_whole_module);
    return true;
  }
  if (!Invocation.getTBDGenOptions().ModuleInstallNameMapPath.empty()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::linker_directives_choice_confusion);
    return true;
  }
  auto tbdOpts = Invocation.getTBDGenOptions();
  tbdOpts.LinkerDirectivesOnly = true;
  auto *module = Instance.getMainModule();
  auto ldSymbols =
      getPublicSymbols(TBDGenDescriptor::forModule(module, tbdOpts));
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::F_None);
  if (EC) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_opening_output, Path,
                                 EC.message());
    return true;
  }
  OS << "// Automatically generated C source file from the Swift compiler \n"
     << "// to add removed symbols back to the high-level framework for deployment\n"
     << "// targets prior to the OS version when these symbols were moved to\n"
     << "// a low-level framework " << module->getName().str() << ".\n\n";
  unsigned Idx = 0;
  for (auto &S: ldSymbols) {
    SmallString<32> NameBuffer;
    llvm::raw_svector_ostream NameOS(NameBuffer);
    NameOS << "ldAdd_" << Idx;
    OS << "extern const char " << NameOS.str() << " __asm(\"" <<
      changeToLdAdd(S) << "\");\n";
    OS << "const char " << NameOS.str() << " = 0;\n";
    ++ Idx;
  }
  return false;
}

static bool performCompileStepsPostSILGen(CompilerInstance &Instance,
                                          std::unique_ptr<SILModule> SM,
                                          ModuleOrSourceFile MSF,
                                          const PrimarySpecificPaths &PSPs,
                                          int &ReturnValue,
                                          FrontendObserver *observer);

static bool performCompileStepsPostSema(CompilerInstance &Instance,
                                        int &ReturnValue,
                                        FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const SILOptions &SILOpts = Invocation.getSILOptions();
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  auto *mod = Instance.getMainModule();
  if (!opts.InputsAndOutputs.hasPrimaryInputs()) {
    // If there are no primary inputs the compiler is in WMO mode and builds one
    // SILModule for the entire module.
    auto SM = performASTLowering(mod, Instance.getSILTypes(), SILOpts);
    const PrimarySpecificPaths PSPs =
        Instance.getPrimarySpecificPathsForWholeModuleOptimizationMode();
    return performCompileStepsPostSILGen(Instance, std::move(SM), mod, PSPs,
                                         ReturnValue, observer);
  }
  // If there are primary source files, build a separate SILModule for
  // each source file, and run the remaining SILOpt-Serialize-IRGen-LLVM
  // once for each such input.
  if (!Instance.getPrimarySourceFiles().empty()) {
    bool result = false;
    for (auto *PrimaryFile : Instance.getPrimarySourceFiles()) {
      auto SM = performASTLowering(*PrimaryFile, Instance.getSILTypes(),
                                   SILOpts);
      const PrimarySpecificPaths PSPs =
          Instance.getPrimarySpecificPathsForSourceFile(*PrimaryFile);
      result |= performCompileStepsPostSILGen(Instance, std::move(SM),
                                              PrimaryFile, PSPs, ReturnValue,
                                              observer);
    }

    return result;
  }

  // If there are primary inputs but no primary _source files_, there might be
  // a primary serialized input.
  bool result = false;
  for (FileUnit *fileUnit : mod->getFiles()) {
    if (auto SASTF = dyn_cast<SerializedASTFile>(fileUnit))
      if (opts.InputsAndOutputs.isInputPrimary(SASTF->getFilename())) {
        auto SM = performASTLowering(*SASTF, Instance.getSILTypes(), SILOpts);
        const PrimarySpecificPaths &PSPs =
            Instance.getPrimarySpecificPathsForPrimary(SASTF->getFilename());
        result |= performCompileStepsPostSILGen(Instance, std::move(SM), mod,
                                                PSPs, ReturnValue, observer);
      }
  }

  return result;
}

static void emitIndexDataForSourceFile(SourceFile *PrimarySourceFile,
                                       const CompilerInstance &Instance);

/// Emits index data for all primary inputs, or the main module.
static void emitIndexData(const CompilerInstance &Instance) {
  if (Instance.getPrimarySourceFiles().empty()) {
    emitIndexDataForSourceFile(nullptr, Instance);
  } else {
    for (SourceFile *SF : Instance.getPrimarySourceFiles())
      emitIndexDataForSourceFile(SF, Instance);
  }
}

/// Emits all "one-per-module" supplementary outputs that don't depend on
/// anything past type-checking.
static bool emitAnyWholeModulePostTypeCheckSupplementaryOutputs(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  // FIXME: Whole-module outputs with a non-whole-module action ought to
  // be disallowed, but the driver implements -index-file mode by generating a
  // regular whole-module frontend command line and modifying it to index just
  // one file (by making it a primary) instead of all of them. If that
  // invocation also has flags to emit whole-module supplementary outputs, the
  // compiler can crash trying to access information for non-type-checked
  // declarations in the non-primary files. For now, prevent those crashes by
  // guarding the emission of whole-module supplementary outputs.
  if (!opts.InputsAndOutputs.isWholeModule())
    return false;

  // Record whether we failed to emit any of these outputs, but keep going; one
  // failure does not mean skipping the rest.
  bool hadAnyError = false;

  if (opts.InputsAndOutputs.hasObjCHeaderOutputPath()) {
    std::string BridgingHeaderPathForPrint;
    if (!opts.ImplicitObjCHeaderPath.empty()) {
      if (opts.BridgingHeaderDirForPrint.hasValue()) {
        // User specified preferred directory for including, use that dir.
        llvm::SmallString<32> Buffer(*opts.BridgingHeaderDirForPrint);
        llvm::sys::path::append(Buffer,
          llvm::sys::path::filename(opts.ImplicitObjCHeaderPath));
        BridgingHeaderPathForPrint = (std::string)Buffer;
      } else {
        // By default, include the given bridging header path directly.
        BridgingHeaderPathForPrint = opts.ImplicitObjCHeaderPath;
      }
    }
    hadAnyError |= printAsObjCIfNeeded(
        Invocation.getObjCHeaderOutputPathForAtMostOnePrimary(),
        Instance.getMainModule(), BridgingHeaderPathForPrint,
        Invocation.isModuleExternallyConsumed(Instance.getMainModule()));
  }

  if (opts.InputsAndOutputs.hasModuleInterfaceOutputPath()) {
    hadAnyError |= printModuleInterfaceIfNeeded(
        Invocation.getModuleInterfaceOutputPathForWholeModule(),
        Invocation.getModuleInterfaceOptions(),
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  if (opts.InputsAndOutputs.hasPrivateModuleInterfaceOutputPath()) {
    // Copy the settings from the module interface to add SPI printing.
    ModuleInterfaceOptions privOpts = Invocation.getModuleInterfaceOptions();
    privOpts.PrintSPIs = true;

    hadAnyError |= printModuleInterfaceIfNeeded(
        Invocation.getPrivateModuleInterfaceOutputPathForWholeModule(),
        privOpts,
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  {
    hadAnyError |= writeTBDIfNeeded(Instance);
  }
  {
    hadAnyError |= writeLdAddCFileIfNeeded(Instance);
  }

  return hadAnyError;
}

static void dumpAPIIfNeeded(const CompilerInstance &Instance) {
  using namespace llvm::sys;
  const auto &Invocation = Instance.getInvocation();
  StringRef OutDir = Invocation.getFrontendOptions().DumpAPIPath;
  if (OutDir.empty())
    return;

  auto getOutPath = [&](SourceFile *SF) -> std::string {
    SmallString<256> Path = OutDir;
    StringRef Filename = SF->getFilename();
    path::append(Path, path::filename(Filename));
    return std::string(Path.str());
  };

  std::unordered_set<std::string> Filenames;

  auto dumpFile = [&](SourceFile *SF) -> bool {
    SmallString<512> TempBuf;
    llvm::raw_svector_ostream TempOS(TempBuf);

    PrintOptions PO = PrintOptions::printInterface(
        Invocation.getFrontendOptions().PrintFullConvention);
    PO.PrintOriginalSourceText = true;
    PO.Indent = 2;
    PO.PrintAccess = false;
    PO.SkipUnderscoredStdlibProtocols = true;
    SF->print(TempOS, PO);
    if (TempOS.str().trim().empty())
      return false; // nothing to show.

    std::string OutPath = getOutPath(SF);
    bool WasInserted = Filenames.insert(OutPath).second;
    if (!WasInserted) {
      llvm::errs() << "multiple source files ended up with the same dump API "
                      "filename to write to: " << OutPath << '\n';
      return true;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(OutPath, EC, fs::FA_Read | fs::FA_Write);
    if (EC) {
      llvm::errs() << "error opening file '" << OutPath << "': "
                   << EC.message() << '\n';
      return true;
    }

    OS << TempOS.str();
    return false;
  };

  std::error_code EC = fs::create_directories(OutDir);
  if (EC) {
    llvm::errs() << "error creating directory '" << OutDir << "': "
                 << EC.message() << '\n';
    return;
  }

  for (auto *FU : Instance.getMainModule()->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(FU))
      if (dumpFile(SF))
        return;
  }
}

/// Perform any actions that must have access to the ASTContext, and need to be
/// delayed until the Swift compile pipeline has finished. This may be called
/// before or after LLVM depending on when the ASTContext gets freed.
static void performEndOfPipelineActions(CompilerInstance &Instance) {
  assert(Instance.hasASTContext());
  auto &ctx = Instance.getASTContext();
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();

  // If we were asked to print Clang stats, do so.
  if (opts.PrintClangStats && ctx.getClangModuleLoader())
    ctx.getClangModuleLoader()->printStatistics();

  // Report AST stats if needed.
  if (auto *stats = ctx.Stats)
    countASTStats(*stats, Instance);

  // Report mangling stats if there was no error.
  if (!ctx.hadError())
    Mangle::printManglingStats();

  // Make sure we didn't load a module during a parse-only invocation, unless
  // it's -emit-imported-modules, which can load modules.
  auto action = opts.RequestedAction;
  if (FrontendOptions::shouldActionOnlyParse(action) &&
      !ctx.getLoadedModules().empty() &&
      action != FrontendOptions::ActionType::EmitImportedModules) {
    assert(ctx.getNumLoadedModules() == 1 &&
           "Loaded a module during parse-only");
    assert(ctx.getLoadedModules().begin()->second == Instance.getMainModule());
  }

  if (!opts.AllowModuleWithCompilerErrors) {
    // Verify the AST for all the modules we've loaded.
    ctx.verifyAllLoadedModules();

    // Verify generic signatures if we've been asked to.
    verifyGenericSignaturesIfNeeded(Invocation.getFrontendOptions(), ctx);
  }

  // Emit any additional outputs that we only need for a successful compilation.
  // We don't want to unnecessarily delay getting any errors back to the user.
  if (!ctx.hadError()) {
    emitLoadedModuleTraceForAllPrimariesIfNeeded(
        Instance.getMainModule(), Instance.getDependencyTracker(), opts);

    dumpAPIIfNeeded(Instance);
  }
  if (!ctx.hadError() || opts.AllowModuleWithCompilerErrors) {
    emitAnyWholeModulePostTypeCheckSupplementaryOutputs(Instance);
  }

  // Verify reference dependencies of the current compilation job. Note this
  // must be run *before* verifying diagnostics so that the former can be tested
  // via the latter.
  if (opts.EnableIncrementalDependencyVerifier) {
    if (!Instance.getPrimarySourceFiles().empty()) {
      swift::verifyDependencies(Instance.getSourceMgr(),
                                Instance.getPrimarySourceFiles());
    } else {
      swift::verifyDependencies(Instance.getSourceMgr(),
                                Instance.getMainModule()->getFiles());
    }
  }

  // FIXME: This predicate matches the status quo, but there's no reason
  // indexing cannot run for actions that do not require stdlib e.g. to better
  // facilitate tests.
  if (FrontendOptions::doesActionRequireSwiftStandardLibrary(action)) {
    emitIndexData(Instance);
  }

  // Emit Swiftdeps for every file in the batch.
  emitSwiftdepsForAllPrimaryInputsIfNeeded(Instance);

  // Emit Make-style dependencies.
  emitMakeDependenciesIfNeeded(Instance.getDiags(),
                               Instance.getDependencyTracker(), opts);
}

static bool printSwiftVersion(const CompilerInvocation &Invocation) {
  llvm::outs() << version::getSwiftFullVersion(
                      version::Version::getCurrentLanguageVersion())
               << '\n';
  llvm::outs() << "Target: " << Invocation.getLangOptions().Target.str()
               << '\n';
  return false;
}

static void printSingleFrontendOpt(llvm::opt::OptTable &table, options::ID id,
                                   llvm::raw_ostream &OS) {
  if (table.getOption(id).hasFlag(options::FrontendOption)) {
    auto name = StringRef(table.getOptionName(id));
    if (!name.empty()) {
      OS << "    \"" << name << "\",\n";
    }
  }
}

static bool printSwiftFeature(CompilerInstance &instance) {
  ASTContext &context = instance.getASTContext();
  const CompilerInvocation &invocation = instance.getInvocation();
  const FrontendOptions &opts = invocation.getFrontendOptions();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  std::error_code EC;
  llvm::raw_fd_ostream out(path, EC, llvm::sys::fs::F_None);

  if (out.has_error() || EC) {
    context.Diags.diagnose(SourceLoc(), diag::error_opening_output, path,
                           EC.message());
    out.clear_error();
    return true;
  }
  std::unique_ptr<llvm::opt::OptTable> table = createSwiftOptTable();

  out << "{\n";
  SWIFT_DEFER {
    out << "}\n";
  };
  out << "  \"SupportedArguments\": [\n";
#define OPTION(PREFIX, NAME, ID, KIND, GROUP, ALIAS, ALIASARGS, FLAGS, PARAM,  \
               HELPTEXT, METAVAR, VALUES)                                      \
  printSingleFrontendOpt(*table, swift::options::OPT_##ID, out);
#include "swift/Option/Options.inc"
#undef OPTION
  out << "    \"LastOption\"\n";
  out << "  ],\n";
  out << "  \"SupportedFeatures\": [\n";
  // Print supported featur names here.
  out << "    \"LastFeature\"\n";
  out << "  ]\n";
  return false;
}

static bool
withSemanticAnalysis(CompilerInstance &Instance, FrontendObserver *observer,
                     llvm::function_ref<bool(CompilerInstance &)> cont) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  assert(!FrontendOptions::shouldActionOnlyParse(opts.RequestedAction) &&
         "Action may only parse, but has requested semantic analysis!");

  Instance.performSema();
  if (observer)
    observer->performedSemanticAnalysis(Instance);

  switch (opts.CrashMode) {
  case FrontendOptions::DebugCrashMode::AssertAfterParse:
    debugFailWithAssertion();
    return true;
  case FrontendOptions::DebugCrashMode::CrashAfterParse:
    debugFailWithCrash();
    return true;
  case FrontendOptions::DebugCrashMode::None:
    break;
  }

  (void)migrator::updateCodeAndEmitRemapIfNeeded(&Instance);

  if (Instance.getASTContext().hadError() &&
      !opts.AllowModuleWithCompilerErrors)
    return true;

  return cont(Instance);
}

static bool performScanDependencies(CompilerInstance &Instance) {
  auto batchScanInput =
      Instance.getASTContext().SearchPathOpts.BatchScanInputFilePath;
  ModuleDependenciesCache SingleUseCache;
  if (batchScanInput.empty()) {
    if (Instance.getInvocation().getFrontendOptions().ImportPrescan)
      return dependencies::prescanDependencies(Instance);
    else
      return dependencies::scanDependencies(Instance);
  } else {
    if (Instance.getInvocation().getFrontendOptions().ImportPrescan)
      return dependencies::batchPrescanDependencies(Instance, batchScanInput);
    else
      return dependencies::batchScanDependencies(Instance, batchScanInput);
  }
}

static bool performParseOnly(ModuleDecl &MainModule) {
  // A -parse invocation only cares about the side effects of parsing, so
  // force the parsing of all the source files.
  for (auto *file : MainModule.getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(file))
      (void)SF->getTopLevelDecls();
  }
  return MainModule.getASTContext().hadError();
}

static bool performAction(CompilerInstance &Instance,
                          int &ReturnValue,
                          FrontendObserver *observer) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  auto &Context = Instance.getASTContext();
  switch (Instance.getInvocation().getFrontendOptions().RequestedAction) {
  // MARK: Trivial Actions
  case FrontendOptions::ActionType::NoneAction:
    return Context.hadError();
  case FrontendOptions::ActionType::PrintVersion:
    return printSwiftVersion(Instance.getInvocation());
  case FrontendOptions::ActionType::PrintFeature:
    return printSwiftFeature(Instance);
  case FrontendOptions::ActionType::REPL:
    llvm::report_fatal_error("Compiler-internal integrated REPL has been "
                             "removed; use the LLDB-enhanced REPL instead.");

  // MARK: Actions for Clang and Clang Modules
  case FrontendOptions::ActionType::EmitPCH:
    return precompileBridgingHeader(Instance);
  case FrontendOptions::ActionType::EmitPCM:
    return precompileClangModule(Instance);
  case FrontendOptions::ActionType::DumpPCM:
    return dumpPrecompiledClangModule(Instance);

  // MARK: Module Interface Actions
  case FrontendOptions::ActionType::CompileModuleFromInterface:
  case FrontendOptions::ActionType::TypecheckModuleFromInterface:
    return buildModuleFromInterface(Instance);

  // MARK: Actions that Dump
  case FrontendOptions::ActionType::DumpParse:
    return dumpAST(Instance);
  case FrontendOptions::ActionType::DumpAST: {
    // FIXME: -dump-ast expects to be able to write output even if type checking
    // fails which does not cleanly fit the model \c withSemanticAnalysis is
    // trying to impose. Once there is a request for the "semantic AST", this
    // point is moot.
    Instance.performSema();
    return dumpAST(Instance);
  }
  case FrontendOptions::ActionType::PrintAST:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          getPrimaryOrMainSourceFile(Instance).print(
              llvm::outs(), PrintOptions::printEverything());
          return Instance.getASTContext().hadError();
        });
  case FrontendOptions::ActionType::DumpScopeMaps:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          return dumpAndPrintScopeMap(Instance,
                                      getPrimaryOrMainSourceFile(Instance));
        });
  case FrontendOptions::ActionType::DumpTypeRefinementContexts:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          getPrimaryOrMainSourceFile(Instance).getTypeRefinementContext()->dump(
              llvm::errs(), Instance.getASTContext().SourceMgr);
          return Instance.getASTContext().hadError();
        });
  case FrontendOptions::ActionType::DumpInterfaceHash:
    getPrimaryOrMainSourceFile(Instance).dumpInterfaceHash(llvm::errs());
    return Context.hadError();
  case FrontendOptions::ActionType::EmitSyntax:
    return emitSyntax(getPrimaryOrMainSourceFile(Instance),
                      opts.InputsAndOutputs.getSingleOutputFilename());
  case FrontendOptions::ActionType::EmitImportedModules:
    return emitImportedModules(Instance.getMainModule(), opts);

  // MARK: Dependency Scanning Actions
  case FrontendOptions::ActionType::ScanDependencies:
    return performScanDependencies(Instance);

  // MARK: General Compilation Actions
  case FrontendOptions::ActionType::Parse:
    return performParseOnly(*Instance.getMainModule());
  case FrontendOptions::ActionType::ResolveImports:
    return Instance.performParseAndResolveImportsOnly();
  case FrontendOptions::ActionType::Typecheck:
    return withSemanticAnalysis(Instance, observer,
                                [](CompilerInstance &Instance) {
                                  return Instance.getASTContext().hadError();
                                });
  case FrontendOptions::ActionType::EmitSILGen:
  case FrontendOptions::ActionType::EmitSIBGen:
  case FrontendOptions::ActionType::EmitSIL:
  case FrontendOptions::ActionType::EmitSIB:
  case FrontendOptions::ActionType::EmitModuleOnly:
  case FrontendOptions::ActionType::MergeModules:
  case FrontendOptions::ActionType::Immediate:
  case FrontendOptions::ActionType::EmitAssembly:
  case FrontendOptions::ActionType::EmitIRGen:
  case FrontendOptions::ActionType::EmitIR:
  case FrontendOptions::ActionType::EmitBC:
  case FrontendOptions::ActionType::EmitObject:
  case FrontendOptions::ActionType::DumpTypeInfo:
    return withSemanticAnalysis(
        Instance, observer, [&](CompilerInstance &Instance) {
          assert(FrontendOptions::doesActionGenerateSIL(opts.RequestedAction) &&
                 "All actions not requiring SILGen must have been handled!");
          return performCompileStepsPostSema(Instance, ReturnValue, observer);
        });
  }

  assert(false && "Unhandled case in performCompile!");
  return Context.hadError();
}

/// Performs the compile requested by the user.
/// \param Instance Will be reset after performIRGeneration when the verifier
///                 mode is NoVerify and there were no errors.
/// \returns true on error
static bool performCompile(CompilerInstance &Instance,
                           int &ReturnValue,
                           FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  const FrontendOptions::ActionType Action = opts.RequestedAction;

  // To compile LLVM IR, just pass it off unmodified.
  if (opts.InputsAndOutputs.shouldTreatAsLLVM())
    return compileLLVMIR(Instance);

  // If we aren't in a parse-only context and expect an implicit stdlib import,
  // load in the standard library. If we either fail to find it or encounter an
  // error while loading it, bail early. Continuing the compilation will at best
  // trigger a bunch of other errors due to the stdlib being missing, or at
  // worst crash downstream as many call sites don't currently handle a missing
  // stdlib.
  if (FrontendOptions::doesActionRequireSwiftStandardLibrary(Action)) {
    if (Instance.loadStdlibIfNeeded())
      return true;
  }

  assert([&]() -> bool {
    if (FrontendOptions::shouldActionOnlyParse(Action)) {
      // Parsing gets triggered lazily, but let's make sure we have the right
      // input kind.
      return llvm::all_of(
          opts.InputsAndOutputs.getAllInputs(), [](const InputFile &IF) {
            const auto kind = IF.getType();
            return kind == file_types::TY_Swift ||
                   kind == file_types::TY_SwiftModuleInterfaceFile;
          });
    }
    return true;
  }() && "Only supports parsing .swift files");

  bool hadError = performAction(Instance, ReturnValue, observer);

  // We might have freed the ASTContext already, but in that case we would
  // have already performed these actions.
  if (Instance.hasASTContext() &&
      FrontendOptions::doesActionPerformEndOfPipelineActions(Action)) {
    performEndOfPipelineActions(Instance);
    if (!opts.AllowModuleWithCompilerErrors)
      hadError |= Instance.getASTContext().hadError();
  }
  return hadError;
}

static bool serializeSIB(SILModule *SM, const PrimarySpecificPaths &PSPs,
                         const ASTContext &Context, ModuleOrSourceFile MSF) {
  const std::string &moduleOutputPath =
      PSPs.SupplementaryOutputs.ModuleOutputPath;
  assert(!moduleOutputPath.empty() && "must have an output path");

  SerializationOptions serializationOpts;
  serializationOpts.OutputPath = moduleOutputPath.c_str();
  serializationOpts.SerializeAllSIL = true;
  serializationOpts.IsSIB = true;

  serialize(MSF, serializationOpts, SM);
  return Context.hadError();
}

static bool serializeModuleSummary(SILModule *SM,
                                   const PrimarySpecificPaths &PSPs,
                                   const ASTContext &Context) {
  auto summaryOutputPath = PSPs.SupplementaryOutputs.ModuleSummaryOutputPath;
  auto Summary = modulesummary::buildModuleSummaryIndex(*SM);
  return modulesummary::writeModuleSummaryIndex(*Summary.get(), Context.Diags,
                                                summaryOutputPath);
}

static GeneratedModule
generateIR(const IRGenOptions &IRGenOpts, const TBDGenOptions &TBDOpts,
           std::unique_ptr<SILModule> SM,
           const PrimarySpecificPaths &PSPs,
           StringRef OutputFilename, ModuleOrSourceFile MSF,
           llvm::GlobalVariable *&HashGlobal,
           ArrayRef<std::string> parallelOutputFilenames) {
  if (auto *SF = MSF.dyn_cast<SourceFile *>()) {
    return performIRGeneration(SF, IRGenOpts, TBDOpts,
                               std::move(SM), OutputFilename, PSPs,
                               SF->getPrivateDiscriminator().str(),
                               &HashGlobal);
  } else {
    return performIRGeneration(MSF.get<ModuleDecl *>(), IRGenOpts, TBDOpts,
                               std::move(SM), OutputFilename, PSPs,
                               parallelOutputFilenames, &HashGlobal);
  }
}

static bool processCommandLineAndRunImmediately(CompilerInstance &Instance,
                                                std::unique_ptr<SILModule> &&SM,
                                                ModuleOrSourceFile MSF,
                                                FrontendObserver *observer,
                                                int &ReturnValue) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  assert(!MSF.is<SourceFile *>() && "-i doesn't work in -primary-file mode");
  const IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  const ProcessCmdLine &CmdLine =
      ProcessCmdLine(opts.ImmediateArgv.begin(), opts.ImmediateArgv.end());

  PrettyStackTraceStringAction trace(
      "running user code",
      MSF.is<SourceFile *>() ? MSF.get<SourceFile *>()->getFilename()
                     : MSF.get<ModuleDecl *>()->getModuleFilename());

  ReturnValue =
      RunImmediately(Instance, CmdLine, IRGenOpts, Invocation.getSILOptions(),
                     std::move(SM));
  return Instance.getASTContext().hadError();
}

static bool validateTBDIfNeeded(const CompilerInvocation &Invocation,
                                ModuleOrSourceFile MSF,
                                const llvm::Module &IRModule) {
  const auto mode = Invocation.getFrontendOptions().ValidateTBDAgainstIR;
  const bool canPerformTBDValidation = [&]() {
    // If the user has requested we skip validation, honor it.
    if (mode == FrontendOptions::TBDValidationMode::None) {
      return false;
    }

    // Cross-module optimization does not support TBD.
    if (Invocation.getSILOptions().CrossModuleOptimization) {
      return false;
    }

    // If we can't validate the given input file, bail early. This covers cases
    // like passing raw SIL as a primary file.
    const auto &IO = Invocation.getFrontendOptions().InputsAndOutputs;
    // FIXME: This would be a good test of the interface format.
    if (IO.shouldTreatAsModuleInterface() || IO.shouldTreatAsSIL() ||
        IO.shouldTreatAsLLVM() || IO.shouldTreatAsObjCHeader()) {
      return false;
    }

    // Modules with SIB files cannot be validated. This is because SIB files
    // may have serialized hand-crafted SIL definitions that are invisible to
    // TBDGen as it is an AST-only traversal.
    if (auto *mod = MSF.dyn_cast<ModuleDecl *>()) {
      return llvm::none_of(mod->getFiles(), [](const FileUnit *File) -> bool {
        auto SASTF = dyn_cast<SerializedASTFile>(File);
        return SASTF && SASTF->isSIB();
      });
    }

    // "Default" mode's behavior varies if using a debug compiler.
    if (mode == FrontendOptions::TBDValidationMode::Default) {
#ifndef NDEBUG
      // With a debug compiler, we do some validation by default.
      return true;
#else
      // Otherwise, the default is to do nothing.
      return false;
#endif
    }

    
    return true;
  }();

  if (!canPerformTBDValidation) {
    return false;
  }

  const bool diagnoseExtraSymbolsInTBD = [mode]() {
    switch (mode) {
    case FrontendOptions::TBDValidationMode::None:
      llvm_unreachable("Handled Above!");
    case FrontendOptions::TBDValidationMode::Default:
    case FrontendOptions::TBDValidationMode::MissingFromTBD:
      return false;
    case FrontendOptions::TBDValidationMode::All:
      return true;
    }
    llvm_unreachable("invalid mode");
  }();

  TBDGenOptions Opts = Invocation.getTBDGenOptions();
  // Ignore embedded symbols from external modules for validation to remove
  // noise from e.g. statically-linked libraries.
  Opts.embedSymbolsFromModules.clear();
  if (auto *SF = MSF.dyn_cast<SourceFile *>()) {
    return validateTBD(SF, IRModule, Opts, diagnoseExtraSymbolsInTBD);
  } else {
    return validateTBD(MSF.get<ModuleDecl *>(), IRModule, Opts,
                       diagnoseExtraSymbolsInTBD);
  }
}

static void freeASTContextIfPossible(CompilerInstance &Instance) {
  // If the stats reporter is installed, we need the ASTContext to live through
  // the entire compilation process.
  if (Instance.getASTContext().Stats) {
    return;
  }

  const auto &opts = Instance.getInvocation().getFrontendOptions();

  // If there are multiple primary inputs it is too soon to free
  // the ASTContext, etc.. OTOH, if this compilation generates code for > 1
  // primary input, then freeing it after processing the last primary is
  // unlikely to reduce the peak heap size. So, only optimize the
  // single-primary-case (or WMO).
  if (opts.InputsAndOutputs.hasMultiplePrimaryInputs()) {
    return;
  }

  // Make sure to perform the end of pipeline actions now, because they need
  // access to the ASTContext.
  performEndOfPipelineActions(Instance);

  Instance.freeASTContext();
}

static bool generateCode(CompilerInstance &Instance, StringRef OutputFilename,
                         llvm::Module *IRModule,
                         llvm::GlobalVariable *HashGlobal) {
  const auto &opts = Instance.getInvocation().getIRGenOptions();
  std::unique_ptr<llvm::TargetMachine> TargetMachine =
      createTargetMachine(opts, Instance.getASTContext());

  // Free up some compiler resources now that we have an IRModule.
  freeASTContextIfPossible(Instance);

  // If we emitted any errors while perfoming the end-of-pipeline actions, bail.
  if (Instance.getDiags().hadAnyError())
    return true;

  // Now that we have a single IR Module, hand it over to performLLVM.
  return performLLVM(opts, Instance.getDiags(), nullptr, HashGlobal, IRModule,
                     TargetMachine.get(), OutputFilename,
                     Instance.getStatsReporter());
}

static bool performCompileStepsPostSILGen(CompilerInstance &Instance,
                                          std::unique_ptr<SILModule> SM,
                                          ModuleOrSourceFile MSF,
                                          const PrimarySpecificPaths &PSPs,
                                          int &ReturnValue,
                                          FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  FrontendOptions::ActionType Action = opts.RequestedAction;
  const ASTContext &Context = Instance.getASTContext();
  const IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();

  Optional<BufferIndirectlyCausingDiagnosticRAII> ricd;
  if (auto *SF = MSF.dyn_cast<SourceFile *>())
    ricd.emplace(*SF);

  if (observer)
    observer->performedSILGeneration(*SM);

  auto *Stats = Instance.getASTContext().Stats;
  if (Stats)
    countStatsPostSILGen(*Stats, *SM);

  // We've been told to emit SIL after SILGen, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSILGen) {
    return writeSIL(*SM, PSPs, Instance, Invocation.getSILOptions());
  }

  if (Action == FrontendOptions::ActionType::EmitSIBGen) {
    serializeSIB(SM.get(), PSPs, Context, MSF);
    return Context.hadError();
  }

  SM->installSILRemarkStreamer();

  // This is the action to be used to serialize SILModule.
  // It may be invoked multiple times, but it will perform
  // serialization only once. The serialization may either happen
  // after high-level optimizations or after all optimizations are
  // done, depending on the compiler setting.

  auto SerializeSILModuleAction = [&]() {
    const SupplementaryOutputPaths &outs = PSPs.SupplementaryOutputs;
    if (outs.ModuleOutputPath.empty())
      return;

    SerializationOptions serializationOpts =
        Invocation.computeSerializationOptions(outs, Instance.getMainModule());

    const bool canEmitIncrementalInfoIntoModule =
        !serializationOpts.DisableCrossModuleIncrementalInfo &&
        (Action == FrontendOptions::ActionType::MergeModules);
    if (canEmitIncrementalInfoIntoModule) {
      const auto alsoEmitDotFile =
          Instance.getInvocation()
              .getLangOptions()
              .EmitFineGrainedDependencySourcefileDotFiles;

      using SourceFileDepGraph = fine_grained_dependencies::SourceFileDepGraph;
      auto *Mod = MSF.get<ModuleDecl *>();
      fine_grained_dependencies::withReferenceDependencies(
          Mod, *Instance.getDependencyTracker(), Mod->getModuleFilename(),
          alsoEmitDotFile, [&](SourceFileDepGraph &&g) {
            serialize(MSF, serializationOpts, SM.get(), &g);
            return false;
          });
    } else {
      serialize(MSF, serializationOpts, SM.get());
    }
  };

  // Set the serialization action, so that the SIL module
  // can be serialized at any moment, e.g. during the optimization pipeline.
  SM->setSerializeSILAction(SerializeSILModuleAction);

  // Perform optimizations and mandatory/diagnostic passes.
  if (Instance.performSILProcessing(SM.get()))
    return true;

  if (observer)
    observer->performedSILProcessing(*SM);

  if (PSPs.haveModuleSummaryOutputPath()) {
    if (serializeModuleSummary(SM.get(), PSPs, Context)) {
      return true;
    }
  }

  if (Action == FrontendOptions::ActionType::EmitSIB)
    return serializeSIB(SM.get(), PSPs, Context, MSF);

  if (PSPs.haveModuleOrModuleDocOutputPaths()) {
    if (Action == FrontendOptions::ActionType::MergeModules ||
        Action == FrontendOptions::ActionType::EmitModuleOnly) {
      return Context.hadError() && !opts.AllowModuleWithCompilerErrors;
    }
  }

  assert(Action >= FrontendOptions::ActionType::EmitSIL &&
         "All actions not requiring SILPasses must have been handled!");

  // We've been told to write canonical SIL, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSIL)
    return writeSIL(*SM, PSPs, Instance, Invocation.getSILOptions());

  assert(Action >= FrontendOptions::ActionType::Immediate &&
         "All actions not requiring IRGen must have been handled!");
  assert(Action != FrontendOptions::ActionType::REPL &&
         "REPL mode must be handled immediately after Instance->performSema()");

  // Check if we had any errors; if we did, don't proceed to IRGen.
  if (Context.hadError())
    return !opts.AllowModuleWithCompilerErrors;

  runSILLoweringPasses(*SM);

  // TODO: at this point we need to flush any the _tracing and profiling_
  // in the UnifiedStatsReporter, because the those subsystems of the USR
  // retain _pointers into_ the SILModule, and the SILModule's lifecycle is
  // not presently such that it will outlive the USR (indeed, as it's
  // destroyed on a separate thread, this fact isn't even _deterministic_
  // after this point). If future plans require the USR tracing or
  // profiling entities after this point, more rearranging will be required.
  if (Stats)
    Stats->flushTracesAndProfiles();

  if (Action == FrontendOptions::ActionType::DumpTypeInfo)
    return performDumpTypeInfo(IRGenOpts, *SM);

  if (Action == FrontendOptions::ActionType::Immediate)
    return processCommandLineAndRunImmediately(
        Instance, std::move(SM), MSF, observer, ReturnValue);

  StringRef OutputFilename = PSPs.OutputFilename;
  std::vector<std::string> ParallelOutputFilenames =
      opts.InputsAndOutputs.copyOutputFilenames();
  llvm::GlobalVariable *HashGlobal;
  auto IRModule = generateIR(
      IRGenOpts, Invocation.getTBDGenOptions(), std::move(SM), PSPs,
      OutputFilename, MSF, HashGlobal, ParallelOutputFilenames);

  // If no IRModule is available, bail. This can either happen if IR generation
  // fails, or if parallelIRGen happened correctly (in which case it would have
  // already performed LLVM).
  if (!IRModule)
    return Instance.getDiags().hadAnyError();

  if (validateTBDIfNeeded(Invocation, MSF, *IRModule.getModule()))
    return true;

  return generateCode(Instance, OutputFilename, IRModule.getModule(),
                      HashGlobal);
}

static void emitIndexDataForSourceFile(SourceFile *PrimarySourceFile,
                                       const CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();

  if (opts.IndexStorePath.empty())
    return;

  // FIXME: provide index unit token(s) explicitly and only use output file
  // paths as a fallback.

  bool isDebugCompilation;
  switch (Invocation.getSILOptions().OptMode) {
    case OptimizationMode::NotSet:
    case OptimizationMode::NoOptimization:
      isDebugCompilation = true;
      break;
    case OptimizationMode::ForSpeed:
    case OptimizationMode::ForSize:
      isDebugCompilation = false;
      break;
  }

  if (PrimarySourceFile) {
    const PrimarySpecificPaths &PSPs =
        opts.InputsAndOutputs.getPrimarySpecificPathsForPrimary(
            PrimarySourceFile->getFilename());
    StringRef OutputFile = PSPs.IndexUnitOutputFilename;
    if (OutputFile.empty())
      OutputFile = PSPs.OutputFilename;
    (void) index::indexAndRecord(PrimarySourceFile, OutputFile,
                                 opts.IndexStorePath, opts.IndexSystemModules,
                                 opts.IndexIgnoreStdlib, isDebugCompilation,
                                 Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker());
  } else {
    std::string moduleToken =
        Invocation.getModuleOutputPathForAtMostOnePrimary();
    if (moduleToken.empty())
      moduleToken = opts.InputsAndOutputs.getSingleIndexUnitOutputFilename();

    (void) index::indexAndRecord(Instance.getMainModule(),
                                 opts.InputsAndOutputs
                                   .copyIndexUnitOutputFilenames(),
                                 moduleToken, opts.IndexStorePath,
                                 opts.IndexSystemModules,
                                 opts.IndexIgnoreStdlib,
                                 isDebugCompilation,
                                 Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker());
  }
}

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
  // This routine gets called in cases where no primary subconsumers are created.
  // Don't bother to create non-primary subconsumers if there aren't any primary
  // ones.
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
    const FrontendInputsAndOutputs &inputsAndOutputs) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      inputsAndOutputs,
      [](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
        auto serializedDiagnosticsPath = input.getSerializedDiagnosticsPath();
        if (serializedDiagnosticsPath.empty())
          return nullptr;
        return serialized_diagnostics::createConsumer(
            serializedDiagnosticsPath);
      });
}

/// Creates a diagnostic consumer that accumulates all emitted diagnostics as compilation
/// proceeds. The accumulated diagnostics are then emitted in the frontend's parseable-output.
static std::unique_ptr<DiagnosticConsumer>
createAccumulatingDiagnosticConsumer(
    const FrontendInputsAndOutputs &InputsAndOutputs,
    llvm::StringMap<std::vector<std::string>> &FileSpecificDiagnostics) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      InputsAndOutputs,
      [&](const InputFile &Input) -> std::unique_ptr<DiagnosticConsumer> {
    FileSpecificDiagnostics.try_emplace(Input.getFileName(),
                                        std::vector<std::string>());
    auto &DiagBufferRef = FileSpecificDiagnostics[Input.getFileName()];
    return std::make_unique<AccumulatingFileDiagnosticConsumer>(DiagBufferRef);
  });
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified in \p options.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
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

/// Print information about a
static void printCompatibilityLibrary(
    llvm::VersionTuple runtimeVersion, llvm::VersionTuple maxVersion,
    StringRef filter, StringRef libraryName, bool &printedAny,
    llvm::raw_ostream &out) {
  if (runtimeVersion > maxVersion)
    return;

  if (printedAny) {
    out << ",";
  }

  out << "\n";
  out << "      {\n";

  out << "        \"libraryName\": \"";
  out.write_escaped(libraryName);
  out << "\",\n";

  out << "        \"filter\": \"";
  out.write_escaped(filter);
  out << "\"\n";
  out << "      }";

  printedAny = true;
}

/// Print information about the target triple in JSON.
static void printTripleInfo(const llvm::Triple &triple,
                            llvm::Optional<llvm::VersionTuple> runtimeVersion,
                            llvm::raw_ostream &out) {
  out << "{\n";

  out << "    \"triple\": \"";
  out.write_escaped(triple.getTriple());
  out << "\",\n";

  out << "    \"unversionedTriple\": \"";
  out.write_escaped(getUnversionedTriple(triple).getTriple());
  out << "\",\n";

  out << "    \"moduleTriple\": \"";
  out.write_escaped(getTargetSpecificModuleTriple(triple).getTriple());
  out << "\",\n";

  if (runtimeVersion) {
    out << "    \"swiftRuntimeCompatibilityVersion\": \"";
    out.write_escaped(runtimeVersion->getAsString());
    out << "\",\n";

    // Compatibility libraries that need to be linked.
    out << "    \"compatibilityLibraries\": [";
    bool printedAnyCompatibilityLibrary = false;
    #define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName)           \
      printCompatibilityLibrary(                                        \
        *runtimeVersion, llvm::VersionTuple Version, #Filter, LibraryName, \
        printedAnyCompatibilityLibrary, out);
    #include "swift/Frontend/BackDeploymentLibs.def"

    if (printedAnyCompatibilityLibrary) {
      out << "\n   ";
    }
    out << " ],\n";
  } else {
    out << "    \"compatibilityLibraries\": [ ],\n";
  }

  out << "    \"librariesRequireRPath\": "
      << (tripleRequiresRPathForSwiftInOS(triple) ? "true" : "false")
      << "\n";

  out << "  }";

}

/// Print information about the selected target in JSON.
static void printTargetInfo(const CompilerInvocation &invocation,
                            llvm::raw_ostream &out) {
  out << "{\n";

  // Compiler version, as produced by --version.
  out << "  \"compilerVersion\": \"";
  out.write_escaped(version::getSwiftFullVersion(
                                                 version::Version::getCurrentLanguageVersion()));
  out << "\",\n";

  // Target triple and target variant triple.
  auto runtimeVersion =
    invocation.getIRGenOptions().AutolinkRuntimeCompatibilityLibraryVersion;
  auto &langOpts = invocation.getLangOptions();
  out << "  \"target\": ";
  printTripleInfo(langOpts.Target, runtimeVersion, out);
  out << ",\n";

  if (auto &variant = langOpts.TargetVariant) {
    out << "  \"targetVariant\": ";
    printTripleInfo(*variant, runtimeVersion, out);
    out << ",\n";
  }

  // Various paths.
  auto &searchOpts = invocation.getSearchPathOptions();
  out << "  \"paths\": {\n";

  if (!searchOpts.SDKPath.empty()) {
    out << "    \"sdkPath\": \"";
    out.write_escaped(searchOpts.SDKPath);
    out << "\",\n";
  }

  auto outputPaths = [&](StringRef name, const std::vector<std::string> &paths){
    out << "    \"" << name << "\": [\n";
    llvm::interleave(paths, [&out](const std::string &path) {
      out << "      \"";
      out.write_escaped(path);
      out << "\"";
    }, [&out] {
      out << ",\n";
    });
    out << "\n    ],\n";
  };

  outputPaths("runtimeLibraryPaths", searchOpts.RuntimeLibraryPaths);
  outputPaths("runtimeLibraryImportPaths",
              searchOpts.RuntimeLibraryImportPaths);

  out << "    \"runtimeResourcePath\": \"";
  out.write_escaped(searchOpts.RuntimeResourcePath);
  out << "\"\n";

  out << "  }\n";

  out << "}\n";
}

int swift::performFrontend(ArrayRef<const char *> Args,
                           const char *Argv0, void *MainAddr,
                           FrontendObserver *observer) {
  INITIALIZE_LLVM();
  llvm::setBugReportMsg(SWIFT_CRASH_BUG_REPORT_MESSAGE "\n");
  llvm::EnablePrettyStackTraceOnSigInfoForThisThread();

  PrintingDiagnosticConsumer PDC;

  // Hopefully we won't trigger any LLVM-level fatal errors, but if we do try
  // to route them through our usual textual diagnostics before crashing.
  //
  // Unfortunately it's not really safe to do anything else, since very
  // low-level operations in LLVM can trigger fatal errors.
  auto diagnoseFatalError = [&PDC](const std::string &reason, bool shouldCrash){
    static const std::string *recursiveFatalError = nullptr;
    if (recursiveFatalError) {
      // Report the /original/ error through LLVM's default handler, not
      // whatever we encountered.
      llvm::remove_fatal_error_handler();
      llvm::report_fatal_error(*recursiveFatalError, shouldCrash);
    }
    recursiveFatalError = &reason;

    SourceManager dummyMgr;

    DiagnosticInfo errorInfo(
        DiagID(0), SourceLoc(), DiagnosticKind::Error,
        "fatal error encountered during compilation; " SWIFT_BUG_REPORT_MESSAGE,
        {}, StringRef(), SourceLoc(), {}, {}, {}, false);
    DiagnosticInfo noteInfo(DiagID(0), SourceLoc(), DiagnosticKind::Note,
                            reason, {}, StringRef(), SourceLoc(), {}, {}, {},
                            false);
    PDC.handleDiagnostic(dummyMgr, errorInfo);
    PDC.handleDiagnostic(dummyMgr, noteInfo);
    if (shouldCrash)
      abort();
  };
  llvm::ScopedFatalErrorHandler handler([](void *rawCallback,
                                           const std::string &reason,
                                           bool shouldCrash) {
    auto *callback = static_cast<decltype(&diagnoseFatalError)>(rawCallback);
    (*callback)(reason, shouldCrash);
  }, &diagnoseFatalError);

  std::unique_ptr<CompilerInstance> Instance =
    std::make_unique<CompilerInstance>();

  // In parseable output, avoid printing diagnostics
  Instance->addDiagnosticConsumer(&PDC);

  struct FinishDiagProcessingCheckRAII {
    bool CalledFinishDiagProcessing = false;
    ~FinishDiagProcessingCheckRAII() {
      assert(CalledFinishDiagProcessing && "returned from the function "
        "without calling finishDiagProcessing");
    }
  } FinishDiagProcessingCheckRAII;

  auto finishDiagProcessing = [&](int retValue, bool verifierEnabled) -> int {
    FinishDiagProcessingCheckRAII.CalledFinishDiagProcessing = true;
    PDC.setSuppressOutput(false);
    bool diagnosticsError = Instance->getDiags().finishProcessing();
    // If the verifier is enabled and did not encounter any verification errors,
    // return 0 even if the compile failed. This behavior isn't ideal, but large
    // parts of the test suite are reliant on it.
    if (verifierEnabled && !diagnosticsError) {
      return 0;
    }
    return retValue ? retValue : diagnosticsError;
  };

  if (Args.empty()) {
    Instance->getDiags().diagnose(SourceLoc(), diag::error_no_frontend_args);
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  CompilerInvocation Invocation;

  SmallString<128> workingDirectory;
  llvm::sys::fs::current_path(workingDirectory);

  std::string MainExecutablePath =
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr);

  // Parse arguments.
  SmallVector<std::unique_ptr<llvm::MemoryBuffer>, 4> configurationFileBuffers;
  if (Invocation.parseArgs(Args, Instance->getDiags(),
                           &configurationFileBuffers, workingDirectory,
                           MainExecutablePath)) {
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  // Make an array of PrettyStackTrace objects to dump the configuration files
  // we used to parse the arguments. These are RAII objects, so they and the
  // buffers they refer to must be kept alive in order to be useful. (That is,
  // we want them to be alive for the entire rest of performFrontend.)
  //
  // This can't be a SmallVector or similar because PrettyStackTraces can't be
  // moved (or copied)...and it can't be an array of non-optionals because
  // PrettyStackTraces can't be default-constructed. So we end up with a
  // dynamically-sized array of optional PrettyStackTraces, which get
  // initialized by iterating over the buffers we collected above.
  auto configurationFileStackTraces =
      std::make_unique<Optional<PrettyStackTraceFileContents>[]>(
        configurationFileBuffers.size());
  for_each(configurationFileBuffers.begin(), configurationFileBuffers.end(),
           &configurationFileStackTraces[0],
           [](const std::unique_ptr<llvm::MemoryBuffer> &buffer,
              Optional<PrettyStackTraceFileContents> &trace) {
    trace.emplace(*buffer);
  });

  // Setting DWARF Version depend on platform
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  IRGenOpts.DWARFVersion = swift::DWARFVersion;

  // The compiler invocation is now fully configured; notify our observer.
  if (observer) {
    observer->parsedArgs(Invocation);
  }

  if (Invocation.getFrontendOptions().PrintHelp ||
      Invocation.getFrontendOptions().PrintHelpHidden) {
    unsigned IncludedFlagsBitmask = options::FrontendOption;
    unsigned ExcludedFlagsBitmask =
      Invocation.getFrontendOptions().PrintHelpHidden ? 0 :
                                                        llvm::opt::HelpHidden;
    std::unique_ptr<llvm::opt::OptTable> Options(createSwiftOptTable());
    Options->PrintHelp(llvm::outs(), displayName(MainExecutablePath).c_str(),
                       "Swift frontend", IncludedFlagsBitmask,
                       ExcludedFlagsBitmask, /*ShowAllAliases*/false);
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().PrintTargetInfo) {
    printTargetInfo(Invocation, llvm::outs());
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::NoneAction) {
    Instance->getDiags().diagnose(SourceLoc(),
                                  diag::error_missing_frontend_action);
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  llvm::StringMap<std::vector<std::string>> FileSpecificDiagnostics;
  std::unique_ptr<DiagnosticConsumer> FileSpecificAccumulatingConsumer;
  if (Invocation.getFrontendOptions().FrontendParseableOutput) {
    // We need a diagnostic consumer that will, per-file, collect all
    // diagnostics to be reported in parseable-output
    FileSpecificAccumulatingConsumer = createAccumulatingDiagnosticConsumer(
        Invocation.getFrontendOptions().InputsAndOutputs,
        FileSpecificDiagnostics);
    Instance->addDiagnosticConsumer(FileSpecificAccumulatingConsumer.get());

    // If we got this far, we need to suppress the output of the
    // PrintingDiagnosticConsumer to ensure that only the parseable-output
    // is emitted
    PDC.setSuppressOutput(true);
  }

  // Because the serialized diagnostics consumer is initialized here,
  // diagnostics emitted above, within CompilerInvocation::parseArgs, are never
  // serialized. This is a non-issue because, in nearly all cases, frontend
  // arguments are generated by the driver, not directly by a user. The driver
  // is responsible for emitting diagnostics for its own errors. See SR-2683
  // for details.
  std::unique_ptr<DiagnosticConsumer> SerializedConsumerDispatcher =
      createSerializedDiagnosticConsumerIfNeeded(
        Invocation.getFrontendOptions().InputsAndOutputs);
  if (SerializedConsumerDispatcher)
    Instance->addDiagnosticConsumer(SerializedConsumerDispatcher.get());

  std::unique_ptr<DiagnosticConsumer> FixItsConsumer =
      createJSONFixItDiagnosticConsumerIfNeeded(Invocation);
  if (FixItsConsumer)
    Instance->addDiagnosticConsumer(FixItsConsumer.get());

  if (Invocation.getDiagnosticOptions().UseColor)
    PDC.forceColors();

  PDC.setPrintEducationalNotes(
      Invocation.getDiagnosticOptions().PrintEducationalNotes);

  PDC.setFormattingStyle(
      Invocation.getDiagnosticOptions().PrintedFormattingStyle);

  if (Invocation.getFrontendOptions().PrintStats) {
    llvm::EnableStatistics();
  }

  const DiagnosticOptions &diagOpts = Invocation.getDiagnosticOptions();
  bool verifierEnabled = diagOpts.VerifyMode != DiagnosticOptions::NoVerify;

  if (Instance->setup(Invocation)) {
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  // The compiler instance has been configured; notify our observer.
  if (observer) {
    observer->configuredCompiler(*Instance);
  }

  if (verifierEnabled) {
    // Suppress printed diagnostic output during the compile if the verifier is
    // enabled.
    PDC.setSuppressOutput(true);
  }

  if (Invocation.getFrontendOptions().FrontendParseableOutput) {
   const auto &IO = Invocation.getFrontendOptions().InputsAndOutputs;
    const auto OSPid = getpid();
    const auto ProcInfo = sys::TaskProcessInformation(OSPid);

    // Parseable output clients may not understand the idea of a batch
    // compilation. We assign each primary in a batch job a quasi process id,
    // making sure it cannot collide with a real PID (always positive). Non-batch
    // compilation gets a real OS PID.
    int64_t Pid = IO.hasUniquePrimaryInput() ? OSPid : QUASI_PID_START;
    IO.forEachPrimaryInputWithIndex([&](const InputFile &Input,
                                        unsigned idx) -> bool {
      emitBeganMessage(
          llvm::errs(),
          mapFrontendInvocationToAction(Invocation),
          constructDetailedTaskDescription(Invocation, Input, Args), Pid - idx,
          ProcInfo);
      return false;
    });
  }

  int ReturnValue = 0;
  bool HadError = performCompile(*Instance, ReturnValue, observer);

  if (verifierEnabled) {
    DiagnosticEngine &diags = Instance->getDiags();
    if (diags.hasFatalErrorOccurred() &&
        !Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
      diags.resetHadAnyError();
      PDC.setSuppressOutput(false);
      diags.diagnose(SourceLoc(), diag::verify_encountered_fatal);
      HadError = true;
    }
  }

  auto r = finishDiagProcessing(HadError ? 1 : ReturnValue, verifierEnabled);
  if (auto *StatsReporter = Instance->getStatsReporter())
    StatsReporter->noteCurrentProcessExitStatus(r);

  if (Invocation.getFrontendOptions().FrontendParseableOutput) {
    const auto &IO = Invocation.getFrontendOptions().InputsAndOutputs;
    const auto OSPid = getpid();
    const auto ProcInfo = sys::TaskProcessInformation(OSPid);

    // Parseable output clients may not understand the idea of a batch
    // compilation. We assign each primary in a batch job a quasi process id,
    // making sure it cannot collide with a real PID (always positive). Non-batch
    // compilation gets a real OS PID.
    int64_t Pid = IO.hasUniquePrimaryInput() ? OSPid : QUASI_PID_START;
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

      emitFinishedMessage(llvm::errs(),
                          mapFrontendInvocationToAction(Invocation),
                          JoinedDiags.str(), r, Pid - idx, ProcInfo);
      return false;
    });
  }

  return r;
}

void FrontendObserver::parsedArgs(CompilerInvocation &invocation) {}
void FrontendObserver::configuredCompiler(CompilerInstance &instance) {}
void FrontendObserver::performedSemanticAnalysis(CompilerInstance &instance) {}
void FrontendObserver::performedSILGeneration(SILModule &module) {}
void FrontendObserver::performedSILProcessing(SILModule &module) {}
