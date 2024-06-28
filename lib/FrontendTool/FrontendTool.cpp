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
#include "Dependencies.h"
#include "TBD.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/FineGrainedDependencyFormat.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/TargetInfo.h"
#include "swift/Basic/UUID.h"
#include "swift/Basic/Version.h"
#include "swift/ConstExtract/ConstExtract.h"
#include "swift/DependencyScan/ScanDependencies.h"
#include "swift/Frontend/CachedDiagnostics.h"
#include "swift/Frontend/CachingUtils.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "swift/Frontend/DiagnosticHelper.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/IRGen/TBDGen.h"
#include "swift/Immediate/Immediate.h"
#include "swift/Index/IndexRecord.h"
#include "swift/Migrator/FixitFilter.h"
#include "swift/Migrator/Migrator.h"
#include "swift/Option/Options.h"
#include "swift/PrintAsClang/PrintAsClang.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"

#include "clang/Lex/Preprocessor.h"

#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Option/Option.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputBackends.h"
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

static std::string displayName(StringRef MainExecutablePath) {
  std::string Name = llvm::sys::path::stem(MainExecutablePath).str();
  Name += " -frontend";
  return Name;
}

static void emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts,
                                         llvm::vfs::OutputBackend &backend) {
  opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &f) -> bool {
        return swift::emitMakeDependenciesIfNeeded(diags, depTracker, opts, f,
                                                   backend);
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

/// Writes SIL out to the given file.
static bool writeSIL(SILModule &SM, ModuleDecl *M, const SILOptions &Opts,
                     StringRef OutputFilename,
                     llvm::vfs::OutputBackend &Backend) {
  return withOutputPath(M->getDiags(), Backend, OutputFilename,
                        [&](raw_ostream &out) -> bool {
                          SM.print(out, M, Opts);
                          return M->getASTContext().hadError();
                        });
}

static bool writeSIL(SILModule &SM, const PrimarySpecificPaths &PSPs,
                     CompilerInstance &Instance,
                     const SILOptions &Opts) {
  return writeSIL(SM, Instance.getMainModule(), Opts,
                  PSPs.OutputFilename, Instance.getOutputBackend());
}

/// Prints the Objective-C "generated header" interface for \p M to \p
/// outputPath.
/// Print the exposed "generated header" interface for \p M to \p
/// outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::printAsClangHeader
static bool printAsClangHeaderIfNeeded(llvm::vfs::OutputBackend &outputBackend,
    StringRef outputPath, ModuleDecl *M, StringRef bridgingHeader,
    const FrontendOptions &frontendOpts, const IRGenOptions &irGenOpts,
    clang::HeaderSearch &clangHeaderSearchInfo) {
  if (outputPath.empty())
    return false;
  return withOutputPath(
      M->getDiags(), outputBackend, outputPath, [&](raw_ostream &out) -> bool {
        return printAsClangHeader(out, M, bridgingHeader, frontendOpts,
                                  irGenOpts, clangHeaderSearchInfo);
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
printModuleInterfaceIfNeeded(llvm::vfs::OutputBackend &outputBackend,
                             StringRef outputPath,
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
  return withOutputPath(diags, outputBackend, outputPath,
                        [M, Opts](raw_ostream &out) -> bool {
                          return swift::emitSwiftInterface(out, Opts, M);
                        });
}

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
  C.NumLocalTypeDecls += SF->getLocalTypeDecls().size();
  C.NumObjCMethods += SF->ObjCMethods.size();

  SmallVector<OperatorDecl *, 2> operators;
  SF->getOperatorDecls(operators);
  C.NumOperators += operators.size();

  SmallVector<PrecedenceGroupDecl *, 2> groups;
  SF->getPrecedenceGroups(groups);
  C.NumPrecedenceGroups += groups.size();

  auto bufID = SF->getBufferID();
  if (bufID.has_value()) {
    C.NumSourceLines +=
      SM.getEntireTextForBuffer(bufID.value()).count('\n');
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
    C.NumMacroPluginDependencies = D->getMacroPluginDependencies().size();
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
  auto OutputBackend = Instance.getOutputBackend().clone();
  if (!PCHOutDir.empty()) {
    // Create or validate a persistent PCH.
    auto SwiftPCHHash = Invocation.getPCHHash();
    auto PCH = clangImporter->getOrCreatePCH(ImporterOpts, SwiftPCHHash,
                                             /*cached=*/false);
    return !PCH.has_value();
  }
  return clangImporter->emitBridgingPCH(
      opts.InputsAndOutputs.getFilenameOfFirstInput(),
      opts.InputsAndOutputs.getSingleOutputFilename(), /*cached=*/false);
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
  StringRef ABIPath = Instance.getPrimarySpecificPathsForAtMostOnePrimary()
                          .SupplementaryOutputs.ABIDescriptorOutputPath;
  bool IgnoreAdjacentModules = Instance.hasASTContext() &&
                               Instance.getASTContext().IgnoreAdjacentModules;

  // When building explicit module dependencies, they are
  // discovered by dependency scanner and the swiftmodule is already rebuilt
  // ignoring candidate module. There is no need to serialized dependencies for
  // validation purpose because the build system (swift-driver) is then
  // responsible for checking whether inputs are up-to-date.
  bool ShouldSerializeDeps = !FEOpts.ExplicitInterfaceBuild;

  // If an explicit interface build was requested, bypass the creation of a new
  // sub-instance from the interface which will build it in a separate thread,
  // and isntead directly use the current \c Instance for compilation.
  //
  // FIXME: -typecheck-module-from-interface is the exception here because
  // currently we need to ensure it still reads the flags written out
  // in the .swiftinterface file itself. Instead, creation of that
  // job should incorporate those flags.
  if (FEOpts.ExplicitInterfaceBuild && !(FEOpts.isTypeCheckAction()))
    return ModuleInterfaceLoader::buildExplicitSwiftModuleFromSwiftInterface(
        Instance, Invocation.getClangModuleCachePath(),
        FEOpts.BackupModuleInterfaceDir, PrebuiltCachePath, ABIPath, InputPath,
        Invocation.getOutputFilename(), ShouldSerializeDeps,
        Invocation.getSearchPathOptions().CandidateCompiledModules);

  return ModuleInterfaceLoader::buildSwiftModuleFromSwiftInterface(
      Instance.getSourceMgr(), Instance.getDiags(),
      Invocation.getSearchPathOptions(), Invocation.getLangOptions(),
      Invocation.getClangImporterOptions(), Invocation.getCASOptions(),
      Invocation.getClangModuleCachePath(), PrebuiltCachePath,
      FEOpts.BackupModuleInterfaceDir, Invocation.getModuleName(), InputPath,
      Invocation.getOutputFilename(), ABIPath,
      FEOpts.SerializeModuleInterfaceDependencyHashes,
      FEOpts.shouldTrackSystemDependencies(), LoaderOpts,
      RequireOSSAModules_t(Invocation.getSILOptions()),
      IgnoreAdjacentModules);
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
    swift::validateGenericSignaturesInModule(module);
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
      if (withOutputPath(Instance.getASTContext().Diags,
                         Instance.getOutputBackend(), OutputFilename,
                         [&](raw_ostream &out) -> bool {
                           sourceFile->dump(out, /*parseIfNeeded*/ true);
                           return false;
                         }))
        return true;
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
      SF, *Instance.getDependencyTracker(), Instance.getOutputBackend(),
      outputPath, alsoEmitDotFile, [&](SourceFileDepGraph &&g) -> bool {
        const bool hadError =
            fine_grained_dependencies::writeFineGrainedDependencyGraphToPath(
                Instance.getDiags(), Instance.getOutputBackend(), outputPath,
                g);

        // If path is stdout, cannot read it back, so check for "-"
        assert(outputPath == "-" || g.verifyReadsWhatIsWritten(outputPath));

        if (alsoEmitDotFile)
          g.emitDotFile(Instance.getOutputBackend(), outputPath,
                        Instance.getDiags());
        return hadError;
      });
}

static void emitSwiftdepsForAllPrimaryInputsIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasReferenceDependenciesFilePath() &&
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

static bool emitConstValuesForWholeModuleIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  auto constExtractProtocolListPath =
      Instance.getASTContext().SearchPathOpts.ConstGatherProtocolListFilePath;
  if (constExtractProtocolListPath.empty())
    return false;
  if (!frontendOpts.InputsAndOutputs.hasConstValuesOutputPath())
    return false;
  assert(frontendOpts.InputsAndOutputs.isWholeModule() &&
         "'emitConstValuesForWholeModule' only makes sense when the whole module can be seen");
  auto ConstValuesFilePath = frontendOpts.InputsAndOutputs
    .getPrimarySpecificPathsForAtMostOnePrimary().SupplementaryOutputs
    .ConstValuesOutputPath;

  // List of protocols whose conforming nominal types
  // we should extract compile-time-known values from
  std::unordered_set<std::string> Protocols;
  bool inputParseSuccess = parseProtocolListFromFile(constExtractProtocolListPath,
                                                     Instance.getDiags(), Protocols);
  if (!inputParseSuccess)
    return true;
  auto ConstValues = gatherConstValuesForModule(Protocols,
                                                Instance.getMainModule());

  return withOutputPath(Instance.getDiags(), Instance.getOutputBackend(),
                        ConstValuesFilePath, [&](llvm::raw_ostream &OS) {
                          writeAsJSONToFile(ConstValues, OS);
                          return false;
                        });
}

static void emitConstValuesForAllPrimaryInputsIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  auto constExtractProtocolListPath =
      Instance.getASTContext().SearchPathOpts.ConstGatherProtocolListFilePath;
  if (constExtractProtocolListPath.empty())
    return;

  // List of protocols whose conforming nominal types
  // we should extract compile-time-known values from
  std::unordered_set<std::string> Protocols;
  bool inputParseSuccess = parseProtocolListFromFile(constExtractProtocolListPath,
                                                     Instance.getDiags(), Protocols);
  if (!inputParseSuccess)
    return;
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &ConstValuesFilePath =
        Invocation.getConstValuesFilePathForPrimary(
            SF->getFilename());
    if (ConstValuesFilePath.empty())
      continue;

    auto ConstValues = gatherConstValuesForPrimary(Protocols, SF);
    withOutputPath(Instance.getDiags(), Instance.getOutputBackend(),
                   ConstValuesFilePath, [&](llvm::raw_ostream &OS) {
                     writeAsJSONToFile(ConstValues, OS);
                     return false;
                   });
  }
}

static bool writeModuleSemanticInfoIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  if (!frontendOpts.InputsAndOutputs.hasModuleSemanticInfoOutputPath())
    return false;
  std::error_code EC;
  assert(frontendOpts.InputsAndOutputs.isWholeModule() &&
         "TBDPath only makes sense when the whole module can be seen");
  auto ModuleSemanticPath = frontendOpts.InputsAndOutputs
    .getPrimarySpecificPathsForAtMostOnePrimary().SupplementaryOutputs
    .ModuleSemanticInfoOutputPath;

  return withOutputPath(Instance.getDiags(), Instance.getOutputBackend(),
                        ModuleSemanticPath, [&](llvm::raw_ostream &OS) {
                          OS << "{}\n";
                          return false;
                        });
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

  if (Invocation.getSILOptions().CMOMode ==
      CrossModuleOptimizationMode::Aggressive) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_not_supported_with_cmo);
    return false;
  }

  const std::string &TBDPath = Invocation.getTBDPathForWholeModule();

  return writeTBD(Instance.getMainModule(), TBDPath,
                  Instance.getOutputBackend(), tbdOpts);
}

static bool writeAPIDescriptor(ModuleDecl *M, StringRef OutputPath,
                               llvm::vfs::OutputBackend &Backend) {
  return withOutputPath(M->getDiags(), Backend, OutputPath,
                        [&](raw_ostream &OS) -> bool {
                          writeAPIJSONFile(M, OS, /*PrettyPrinted=*/false);
                          return false;
                        });
}

static bool writeAPIDescriptorIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  if (!frontendOpts.InputsAndOutputs.hasAPIDescriptorOutputPath())
    return false;

  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::api_descriptor_only_supported_in_whole_module);
    return false;
  }

  const std::string &APIDescriptorPath =
      Invocation.getAPIDescriptorPathForWholeModule();

  return writeAPIDescriptor(Instance.getMainModule(), APIDescriptorPath,
                            Instance.getOutputBackend());
}

static bool performCompileStepsPostSILGen(CompilerInstance &Instance,
                                          std::unique_ptr<SILModule> SM,
                                          ModuleOrSourceFile MSF,
                                          const PrimarySpecificPaths &PSPs,
                                          int &ReturnValue,
                                          FrontendObserver *observer);

bool swift::performCompileStepsPostSema(CompilerInstance &Instance,
                                        int &ReturnValue,
                                        FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  auto getSILOptions = [&](const PrimarySpecificPaths &PSPs) -> SILOptions {
    SILOptions SILOpts = Invocation.getSILOptions();
    if (SILOpts.OptRecordFile.empty()) {
      // Check if the record file path was passed via supplemental outputs.
      SILOpts.OptRecordFile = SILOpts.OptRecordFormat ==
        llvm::remarks::Format::YAML ?
          PSPs.SupplementaryOutputs.YAMLOptRecordPath :
          PSPs.SupplementaryOutputs.BitstreamOptRecordPath;
    }
    return SILOpts;
  };

  auto *mod = Instance.getMainModule();
  if (!opts.InputsAndOutputs.hasPrimaryInputs()) {
    // If there are no primary inputs the compiler is in WMO mode and builds one
    // SILModule for the entire module.
    const PrimarySpecificPaths PSPs =
        Instance.getPrimarySpecificPathsForWholeModuleOptimizationMode();
    SILOptions SILOpts = getSILOptions(PSPs);
    IRGenOptions irgenOpts = Invocation.getIRGenOptions();
    auto SM = performASTLowering(mod, Instance.getSILTypes(), SILOpts,
                                 &irgenOpts);
    return performCompileStepsPostSILGen(Instance, std::move(SM), mod, PSPs,
                                         ReturnValue, observer);
  }
  // If there are primary source files, build a separate SILModule for
  // each source file, and run the remaining SILOpt-Serialize-IRGen-LLVM
  // once for each such input.
  if (!Instance.getPrimarySourceFiles().empty()) {
    bool result = false;
    for (auto *PrimaryFile : Instance.getPrimarySourceFiles()) {
      const PrimarySpecificPaths PSPs =
          Instance.getPrimarySpecificPathsForSourceFile(*PrimaryFile);
      SILOptions SILOpts = getSILOptions(PSPs);
    IRGenOptions irgenOpts = Invocation.getIRGenOptions();
      auto SM = performASTLowering(*PrimaryFile, Instance.getSILTypes(),
                                   SILOpts, &irgenOpts);
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
        const PrimarySpecificPaths &PSPs =
            Instance.getPrimarySpecificPathsForPrimary(SASTF->getFilename());
        SILOptions SILOpts = getSILOptions(PSPs);
        auto SM = performASTLowering(*SASTF, Instance.getSILTypes(), SILOpts);
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
  const auto &Context = Instance.getASTContext();
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

  if ((!Context.hadError() || opts.AllowModuleWithCompilerErrors) &&
      opts.InputsAndOutputs.hasClangHeaderOutputPath()) {
    std::string BridgingHeaderPathForPrint = Instance.getBridgingHeaderPath();
    if (!BridgingHeaderPathForPrint.empty()) {
      if (opts.BridgingHeaderDirForPrint.has_value()) {
        // User specified preferred directory for including, use that dir.
        llvm::SmallString<32> Buffer(*opts.BridgingHeaderDirForPrint);
        llvm::sys::path::append(Buffer,
          llvm::sys::path::filename(BridgingHeaderPathForPrint));
        BridgingHeaderPathForPrint = (std::string)Buffer;
      }
    }
    hadAnyError |= printAsClangHeaderIfNeeded(
        Instance.getOutputBackend(),
        Invocation.getClangHeaderOutputPathForAtMostOnePrimary(),
        Instance.getMainModule(), BridgingHeaderPathForPrint, opts,
        Invocation.getIRGenOptions(),
        Context.getClangModuleLoader()
            ->getClangPreprocessor()
            .getHeaderSearchInfo());
  }

  // Only want the header if there's been any errors, ie. there's not much
  // point outputting a swiftinterface for an invalid module
  if (Context.hadError())
    return hadAnyError;

  if (opts.InputsAndOutputs.hasModuleInterfaceOutputPath()) {
    hadAnyError |= printModuleInterfaceIfNeeded(
        Instance.getOutputBackend(),
        Invocation.getModuleInterfaceOutputPathForWholeModule(),
        Invocation.getModuleInterfaceOptions(),
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  if (opts.InputsAndOutputs.hasPrivateModuleInterfaceOutputPath()) {
    // Copy the settings from the module interface to add SPI printing.
    ModuleInterfaceOptions privOpts = Invocation.getModuleInterfaceOptions();
    privOpts.setInterfaceMode(PrintOptions::InterfaceMode::Private);
    privOpts.ModulesToSkipInPublicInterface.clear();

    hadAnyError |= printModuleInterfaceIfNeeded(
        Instance.getOutputBackend(),
        Invocation.getPrivateModuleInterfaceOutputPathForWholeModule(),
        privOpts,
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }
  if (opts.InputsAndOutputs.hasPackageModuleInterfaceOutputPath()) {
    // Copy the settings from the module interface to add package decl printing.
    ModuleInterfaceOptions pkgOpts = Invocation.getModuleInterfaceOptions();
    pkgOpts.setInterfaceMode(PrintOptions::InterfaceMode::Package);
    pkgOpts.ModulesToSkipInPublicInterface.clear();

    hadAnyError |= printModuleInterfaceIfNeeded(
        Instance.getOutputBackend(),
        Invocation.getPackageModuleInterfaceOutputPathForWholeModule(),
        pkgOpts,
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  {
    hadAnyError |= writeTBDIfNeeded(Instance);
  }

  {
    hadAnyError |= writeAPIDescriptorIfNeeded(Instance);
  }

  {
    hadAnyError |= writeModuleSemanticInfoIfNeeded(Instance);
  }

  {
    hadAnyError |= emitConstValuesForWholeModuleIfNeeded(Instance);
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

  if (opts.DumpClangLookupTables && ctx.getClangModuleLoader())
    ctx.getClangModuleLoader()->dumpSwiftLookupTables();

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

  // Contains the hadError checks internally, we still want to output the
  // Objective-C header when there's errors and currently allowing them
  emitAnyWholeModulePostTypeCheckSupplementaryOutputs(Instance);

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

  if (Invocation.getLangOptions()
          .EnableExperimentalEagerClangModuleDiagnostics) {

    // A consumer meant to import all visible declarations.
    class EagerConsumer : public VisibleDeclConsumer {
    public:
      virtual void
      foundDecl(ValueDecl *VD, DeclVisibilityKind Reason,
                DynamicLookupInfo dynamicLookupInfo = {}) override {
        if (auto *IDC = dyn_cast<IterableDeclContext>(VD)) {
          (void)IDC->getMembers();
        }
      }
    };

    EagerConsumer consumer;
    for (auto module : ctx.getLoadedModules()) {
      // None of the passed parameter have an effect, we just need to trigger
      // imports.
      module.second->lookupVisibleDecls(/*Access Path*/ {}, consumer,
                                        NLKind::QualifiedLookup);
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
                               Instance.getDependencyTracker(), opts,
                               Instance.getOutputBackend());

  // Emit extracted constant values for every file in the batch
  emitConstValuesForAllPrimaryInputsIfNeeded(Instance);
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
  if (table.getOption(id).hasFlag(options::FrontendOption) ||
      table.getOption(id).hasFlag(options::AutolinkExtractOption) ||
      table.getOption(id).hasFlag(options::ModuleWrapOption) ||
      table.getOption(id).hasFlag(options::SwiftIndentOption) ||
      table.getOption(id).hasFlag(options::SwiftSymbolGraphExtractOption) ||
      table.getOption(id).hasFlag(options::SwiftAPIDigesterOption)) {
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
  llvm::raw_fd_ostream out(path, EC, llvm::sys::fs::OF_None);

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
  // Print supported feature names here.
  out << "    \"LastFeature\"\n";
  out << "  ]\n";
  return false;
}

static bool
withSemanticAnalysis(CompilerInstance &Instance, FrontendObserver *observer,
                     llvm::function_ref<bool(CompilerInstance &)> cont,
                     bool runDespiteErrors = false) {
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

  bool hadError = Instance.getASTContext().hadError()
                      && !opts.AllowModuleWithCompilerErrors;
  if (hadError && !runDespiteErrors)
    return true;

  return cont(Instance) || hadError;
}

static bool performScanDependencies(CompilerInstance &Instance) {
  auto batchScanInput =
      Instance.getASTContext().SearchPathOpts.BatchScanInputFilePath;
  if (batchScanInput.empty()) {
    if (Instance.getInvocation().getFrontendOptions().ImportPrescan)
      return dependencies::prescanDependencies(Instance);
    else
      return dependencies::scanDependencies(Instance);
  } else {
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
  switch (Instance.getInvocation().getFrontendOptions().RequestedAction) {
  // MARK: Trivial Actions
  case FrontendOptions::ActionType::NoneAction:
    return Instance.getASTContext().hadError();
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
  case FrontendOptions::ActionType::DumpAST:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          return dumpAST(Instance);
        }, /*runDespiteErrors=*/true);
  case FrontendOptions::ActionType::PrintAST:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          getPrimaryOrMainSourceFile(Instance).print(
              llvm::outs(), PrintOptions::printEverything());
          return Instance.getASTContext().hadError();
        });
  case FrontendOptions::ActionType::PrintASTDecl:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          getPrimaryOrMainSourceFile(Instance).print(
              llvm::outs(), PrintOptions::printDeclarations());
          return Instance.getASTContext().hadError();
        });
  case FrontendOptions::ActionType::DumpScopeMaps:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          return dumpAndPrintScopeMap(Instance,
                                      getPrimaryOrMainSourceFile(Instance));
        }, /*runDespiteErrors=*/true);
  case FrontendOptions::ActionType::DumpTypeRefinementContexts:
    return withSemanticAnalysis(
        Instance, observer, [](CompilerInstance &Instance) {
          getPrimaryOrMainSourceFile(Instance).getTypeRefinementContext()->dump(
              llvm::errs(), Instance.getASTContext().SourceMgr);
          return Instance.getASTContext().hadError();
        }, /*runDespiteErrors=*/true);
  case FrontendOptions::ActionType::DumpInterfaceHash:
    getPrimaryOrMainSourceFile(Instance).dumpInterfaceHash(llvm::errs());
    return Instance.getASTContext().hadError();
  case FrontendOptions::ActionType::EmitImportedModules:
    return emitImportedModules(Instance.getMainModule(), opts,
                               Instance.getOutputBackend());

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
  case FrontendOptions::ActionType::Immediate: {
    const auto &Ctx = Instance.getASTContext();
    if (Ctx.LangOpts.hasFeature(Feature::LazyImmediate)) {
      ReturnValue = RunImmediatelyFromAST(Instance);
      return Ctx.hadError();
    }
    return withSemanticAnalysis(
        Instance, observer, [&](CompilerInstance &Instance) {
          assert(FrontendOptions::doesActionGenerateSIL(opts.RequestedAction) &&
                 "All actions not requiring SILGen must have been handled!");
          return performCompileStepsPostSema(Instance, ReturnValue, observer);
        });
  }
  case FrontendOptions::ActionType::EmitSILGen:
  case FrontendOptions::ActionType::EmitSIBGen:
  case FrontendOptions::ActionType::EmitSIL:
  case FrontendOptions::ActionType::EmitSIB:
  case FrontendOptions::ActionType::EmitModuleOnly:
  case FrontendOptions::ActionType::MergeModules:
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
  return Instance.getASTContext().hadError();
}

/// Try replay the compiler result from cache.
///
/// Return true if all the outputs are fetched from cache. Otherwise, return
/// false and will not replay any output.
static bool tryReplayCompilerResults(CompilerInstance &Instance) {
  if (!Instance.supportCaching() ||
      Instance.getInvocation().getCASOptions().CacheSkipReplay)
    return false;

  assert(Instance.getCompilerBaseKey() &&
         "Instance is not setup correctly for replay");

  auto *CDP = Instance.getCachingDiagnosticsProcessor();
  assert(CDP && "CachingDiagnosticsProcessor needs to be setup for replay");

  // Don't capture diagnostics from replay.
  CDP->endDiagnosticCapture();

  bool replayed = replayCachedCompilerOutputs(
      Instance.getObjectStore(), Instance.getActionCache(),
      *Instance.getCompilerBaseKey(), Instance.getDiags(),
      Instance.getInvocation().getFrontendOptions().InputsAndOutputs, *CDP,
      Instance.getInvocation().getCASOptions().EnableCachingRemarks);

  // If we didn't replay successfully, re-start capture.
  if (!replayed)
    CDP->startDiagnosticCapture();

  return replayed;
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

  if (tryReplayCompilerResults(Instance))
    return false;

  // To compile LLVM IR, just pass it off unmodified.
  if (opts.InputsAndOutputs.shouldTreatAsLLVM())
    return compileLLVMIR(Instance);

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
  auto canIgnoreErrorForExit = [&Instance, &opts]() {
    return opts.AllowModuleWithCompilerErrors ||
      (opts.isTypeCheckAction() && Instance.downgradeInterfaceVerificationErrors());
  };

  // We might have freed the ASTContext already, but in that case we would
  // have already performed these actions.
  if (Instance.hasASTContext() &&
      FrontendOptions::doesActionPerformEndOfPipelineActions(Action)) {
    performEndOfPipelineActions(Instance);
    if (!canIgnoreErrorForExit())
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
  serializationOpts.OutputPath = moduleOutputPath;
  serializationOpts.SerializeAllSIL = true;
  serializationOpts.IsSIB = true;

  symbolgraphgen::SymbolGraphOptions symbolGraphOptions;

  serialize(MSF, serializationOpts, symbolGraphOptions, SM);
  return Context.hadError();
}

static bool serializeModuleSummary(SILModule *SM,
                                   const PrimarySpecificPaths &PSPs,
                                   const ASTContext &Context) {
  auto summaryOutputPath = PSPs.SupplementaryOutputs.ModuleSummaryOutputPath;
  return withOutputPath(Context.Diags, Context.getOutputBackend(),
                           summaryOutputPath, [&](llvm::raw_ostream &out) {
                             out << "Some stuff";
                             return false;
                           });
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

    // Embedded Swift does not support TBD.
    if (Invocation.getLangOptions().hasFeature(Feature::Embedded)) {
      return false;
    }

    // Cross-module optimization does not support TBD.
    if (Invocation.getSILOptions().CMOMode == CrossModuleOptimizationMode::Aggressive ||
        Invocation.getSILOptions().CMOMode == CrossModuleOptimizationMode::Everything) {
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
      bool hasSIB = llvm::any_of(mod->getFiles(), [](const FileUnit *File) -> bool {
        auto SASTF = dyn_cast<SerializedASTFile>(File);
        return SASTF && SASTF->isSIB();
      });

      if (hasSIB) {
        return false;
      }
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

  // If this instance is used for multiple compilations, we need the ASTContext
  // to live.
  if (Instance.getInvocation()
          .getFrontendOptions()
          .ReuseFrontendForMultipleCompilations) {
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

  TargetMachine->Options.MCOptions.CAS = Instance.getSharedCASInstance();
  // Free up some compiler resources now that we have an IRModule.
  freeASTContextIfPossible(Instance);

  // If we emitted any errors while performing the end-of-pipeline actions, bail.
  if (Instance.getDiags().hadAnyError())
    return true;

  // Now that we have a single IR Module, hand it over to performLLVM.
  return performLLVM(opts, Instance.getDiags(), nullptr, HashGlobal, IRModule,
                     TargetMachine.get(), OutputFilename,
                     Instance.getOutputBackend(),
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

  std::optional<BufferIndirectlyCausingDiagnosticRAII> ricd;
  if (auto *SF = MSF.dyn_cast<SourceFile *>())
    ricd.emplace(*SF);

  if (observer)
    observer->performedSILGeneration(*SM);

  // Cancellation check after SILGen.
  if (Instance.isCancellationRequested())
    return true;

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

    // Infer if this is an emit-module job part of an incremental build,
    // vs a partial emit-module job (with primary files) or other kinds.
    // We may want to rely on a flag instead to differentiate them.
    const bool isEmitModuleSeparately =
        Action == FrontendOptions::ActionType::EmitModuleOnly &&
        MSF.is<ModuleDecl *>() &&
        Instance.getInvocation()
            .getTypeCheckerOptions()
            .SkipFunctionBodies == FunctionBodySkipping::NonInlinableWithoutTypes;
    const bool canEmitIncrementalInfoIntoModule =
        !serializationOpts.DisableCrossModuleIncrementalInfo &&
        (Action == FrontendOptions::ActionType::MergeModules ||
         isEmitModuleSeparately);
    if (canEmitIncrementalInfoIntoModule) {
      const auto alsoEmitDotFile =
          Instance.getInvocation()
              .getLangOptions()
              .EmitFineGrainedDependencySourcefileDotFiles;

      using SourceFileDepGraph = fine_grained_dependencies::SourceFileDepGraph;
      auto *Mod = MSF.get<ModuleDecl *>();
      fine_grained_dependencies::withReferenceDependencies(
          Mod, *Instance.getDependencyTracker(),
          Instance.getOutputBackend(), Mod->getModuleFilename(),
          alsoEmitDotFile, [&](SourceFileDepGraph &&g) {
            serialize(MSF, serializationOpts, Invocation.getSymbolGraphOptions(), SM.get(), &g);
            return false;
          });
    } else {
      serialize(MSF, serializationOpts, Invocation.getSymbolGraphOptions(), SM.get());
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

  // Cancellation check after SILOptimization.
  if (Instance.isCancellationRequested())
    return true;

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

  // Cancellation check after SILLowering.
  if (Instance.isCancellationRequested())
    return true;

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
  auto IRModule =
      generateIR(IRGenOpts, Invocation.getTBDGenOptions(), std::move(SM), PSPs,
                 OutputFilename, MSF, HashGlobal, ParallelOutputFilenames);

  // Cancellation check after IRGen.
  if (Instance.isCancellationRequested())
    return true;

  // If no IRModule is available, bail. This can either happen if IR generation
  // fails, or if parallelIRGen happened correctly (in which case it would have
  // already performed LLVM).
  if (!IRModule)
    return Instance.getDiags().hadAnyError();

  if (validateTBDIfNeeded(Invocation, MSF, *IRModule.getModule()))
    return true;

  if (IRGenOpts.UseSingleModuleLLVMEmission) {
    // Pretend the other files that drivers/build systems expect exist by
    // creating empty files.
    if (writeEmptyOutputFilesFor(Context, ParallelOutputFilenames, IRGenOpts))
      return true;
  }

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
                                 opts.IndexStorePath,
                                 !opts.IndexIgnoreClangModules,
                                 opts.IndexSystemModules,
                                 opts.IndexIgnoreStdlib,
                                 opts.IndexIncludeLocals,
                                 isDebugCompilation,
                                 opts.DisableImplicitModules,
                                 Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker(),
                                 Invocation.getIRGenOptions().FilePrefixMap);
  } else {
    std::string moduleToken =
        Invocation.getModuleOutputPathForAtMostOnePrimary();
    if (moduleToken.empty())
      moduleToken = opts.InputsAndOutputs.getSingleIndexUnitOutputFilename();

    (void) index::indexAndRecord(Instance.getMainModule(),
                                 opts.InputsAndOutputs
                                   .copyIndexUnitOutputFilenames(),
                                 moduleToken, opts.IndexStorePath,
                                 !opts.IndexIgnoreClangModules,
                                 opts.IndexSystemModules,
                                 opts.IndexIgnoreStdlib,
                                 opts.IndexIncludeLocals,
                                 isDebugCompilation,
                                 opts.DisableImplicitModules,
                                 Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker(),
                                 Invocation.getIRGenOptions().FilePrefixMap);
  }
}

/// A PrettyStackTraceEntry to print frontend information useful for debugging.
class PrettyStackTraceFrontend : public llvm::PrettyStackTraceEntry {
  const CompilerInvocation &Invocation;

public:
  PrettyStackTraceFrontend(const CompilerInvocation &invocation)
      : Invocation(invocation) {}

  void print(llvm::raw_ostream &os) const override {
    auto effective = Invocation.getLangOptions().EffectiveLanguageVersion;
    if (effective != version::Version::getCurrentLanguageVersion()) {
      os << "Compiling with effective version " << effective;
    } else {
      os << "Compiling with the current language version";
    }
    if (Invocation.getFrontendOptions().AllowModuleWithCompilerErrors) {
      os << " while allowing modules with compiler errors";
    }
    os << "\n";
  };
};

int swift::performFrontend(ArrayRef<const char *> Args,
                           const char *Argv0, void *MainAddr,
                           FrontendObserver *observer) {
  INITIALIZE_LLVM();
  llvm::setBugReportMsg(SWIFT_CRASH_BUG_REPORT_MESSAGE "\n");
  llvm::EnablePrettyStackTraceOnSigInfoForThisThread();

  std::unique_ptr<CompilerInstance> Instance =
    std::make_unique<CompilerInstance>();

  DiagnosticHelper DH = DiagnosticHelper::create(*Instance);

  // Hopefully we won't trigger any LLVM-level fatal errors, but if we do try
  // to route them through our usual textual diagnostics before crashing.
  //
  // Unfortunately it's not really safe to do anything else, since very
  // low-level operations in LLVM can trigger fatal errors.
  llvm::ScopedFatalErrorHandler handler(
      [](void *rawCallback, const char *reason, bool shouldCrash) {
        auto *helper = static_cast<DiagnosticHelper *>(rawCallback);
        helper->diagnoseFatalError(reason, shouldCrash);
      },
      &DH);

  struct FinishDiagProcessingCheckRAII {
    bool CalledFinishDiagProcessing = false;
    ~FinishDiagProcessingCheckRAII() {
      assert(CalledFinishDiagProcessing && "returned from the function "
        "without calling finishDiagProcessing");
    }
  } FinishDiagProcessingCheckRAII;

  auto finishDiagProcessing = [&](int retValue, bool verifierEnabled) -> int {
    FinishDiagProcessingCheckRAII.CalledFinishDiagProcessing = true;
    DH.setSuppressOutput(false);
    if (auto *CDP = Instance->getCachingDiagnosticsProcessor()) {
      // Don't cache if build failed.
      if (retValue)
        CDP->endDiagnosticCapture();
    }
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

  // Don't ask clients to report bugs when running a script in immediate mode.
  // When a script asserts the compiler reports the error with the same
  // stacktrace as a compiler crash. From here we can't tell which is which,
  // for now let's not explicitly ask for bug reports.
  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::Immediate) {
    llvm::setBugReportMsg(nullptr);
  }
  
  PrettyStackTraceFrontend frontendTrace(Invocation);

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
      std::make_unique<std::optional<PrettyStackTraceFileContents>[]>(
          configurationFileBuffers.size());
  for_each(configurationFileBuffers.begin(), configurationFileBuffers.end(),
           &configurationFileStackTraces[0],
           [](const std::unique_ptr<llvm::MemoryBuffer> &buffer,
              std::optional<PrettyStackTraceFileContents> &trace) {
             trace.emplace(*buffer);
           });

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
    Options->printHelp(llvm::outs(), displayName(MainExecutablePath).c_str(),
                       "Swift frontend", IncludedFlagsBitmask,
                       ExcludedFlagsBitmask, /*ShowAllAliases*/false);
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().PrintTargetInfo) {
    swift::targetinfo::printTargetInfo(Invocation, llvm::outs());
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::NoneAction) {
    Instance->getDiags().diagnose(SourceLoc(),
                                  diag::error_missing_frontend_action);
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  DH.initDiagConsumers(Invocation);

  if (Invocation.getFrontendOptions().PrintStats) {
    llvm::EnableStatistics();
  }

  DH.beginMessage(Invocation, Args);

  const DiagnosticOptions &diagOpts = Invocation.getDiagnosticOptions();
  bool verifierEnabled = diagOpts.VerifyMode != DiagnosticOptions::NoVerify;

  std::string InstanceSetupError;
  if (Instance->setup(Invocation, InstanceSetupError, Args)) {
    int ReturnCode = 1;
    DH.endMessage(ReturnCode);

    return finishDiagProcessing(ReturnCode, /*verifierEnabled*/ false);
  }

  // The compiler instance has been configured; notify our observer.
  if (observer) {
    observer->configuredCompiler(*Instance);
  }

  if (verifierEnabled) {
    // Suppress printed diagnostic output during the compile if the verifier is
    // enabled.
    DH.setSuppressOutput(true);
  }

  CompilerInstance::HashingBackendPtrTy HashBackend = nullptr;
  if (Invocation.getFrontendOptions().DeterministicCheck) {
    // Setup a verfication instance to run.
    std::unique_ptr<CompilerInstance> VerifyInstance =
        std::make_unique<CompilerInstance>();
    std::string InstanceSetupError;
    // This should not fail because it passed already.
    (void)VerifyInstance->setup(Invocation, InstanceSetupError, Args);

    // Run the first time without observer and discard return value;
    int ReturnValueTest = 0;
    (void)performCompile(*VerifyInstance, ReturnValueTest,
                         /*observer*/ nullptr);
    // Get the hashing output backend and free the compiler instance.
    HashBackend = VerifyInstance->getHashingBackend();
  }

  int ReturnValue = 0;
  bool HadError = performCompile(*Instance, ReturnValue, observer);

  if (verifierEnabled) {
    DiagnosticEngine &diags = Instance->getDiags();
    if (diags.hasFatalErrorOccurred() &&
        !Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
      diags.resetHadAnyError();
      DH.setSuppressOutput(false);
      diags.diagnose(SourceLoc(), diag::verify_encountered_fatal);
      HadError = true;
    }
  }

  if (Invocation.getFrontendOptions().DeterministicCheck) {
    // Collect all output files.
    auto ReHashBackend = Instance->getHashingBackend();
    std::set<std::string> AllOutputs;
    llvm::for_each(HashBackend->outputFiles(), [&](StringRef F) {
      AllOutputs.insert(F.str());
    });
    llvm::for_each(ReHashBackend->outputFiles(), [&](StringRef F) {
      AllOutputs.insert(F.str());
    });

    DiagnosticEngine &diags = Instance->getDiags();
    for (auto &Filename : AllOutputs) {
      auto O1 = HashBackend->getHashValueForFile(Filename);
      if (!O1) {
        diags.diagnose(SourceLoc(), diag::error_output_missing, Filename,
                       /*SecondRun=*/false);
        HadError = true;
        continue;
      }
      auto O2 = ReHashBackend->getHashValueForFile(Filename);
      if (!O2) {
        diags.diagnose(SourceLoc(), diag::error_output_missing, Filename,
                       /*SecondRun=*/true);
        HadError = true;
        continue;
      }
      if (*O1 != *O2) {
        diags.diagnose(SourceLoc(), diag::error_nondeterministic_output,
                       Filename, *O1, *O2);
        HadError = true;
        continue;
      }
      diags.diagnose(SourceLoc(), diag::matching_output_produced, Filename,
                     *O1);
    }
  }

  auto r = finishDiagProcessing(HadError ? 1 : ReturnValue, verifierEnabled);
  if (auto *StatsReporter = Instance->getStatsReporter())
    StatsReporter->noteCurrentProcessExitStatus(r);

  DH.endMessage(r);
  return r;
}

void FrontendObserver::parsedArgs(CompilerInvocation &invocation) {}
void FrontendObserver::configuredCompiler(CompilerInstance &instance) {}
void FrontendObserver::performedSemanticAnalysis(CompilerInstance &instance) {}
void FrontendObserver::performedSILGeneration(SILModule &module) {}
void FrontendObserver::performedSILProcessing(SILModule &module) {}
