//===--- ClangModuleDependencyScanner.cpp - Dependency Scanning -----------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
// This file implements dependency scanning for Clang modules.
//
//===----------------------------------------------------------------------===//
#include "ImporterImpl.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningService.h"
#include "clang/Tooling/DependencyScanning/DependencyScanningTool.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Signals.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"

using namespace swift;

using namespace clang::tooling;
using namespace clang::tooling::dependencies;

static std::string
moduleCacheRelativeLookupModuleOutput(const ModuleID &MID, ModuleOutputKind MOK,
                                      const std::string &moduleCachePathStr) {
  llvm::SmallString<128> outputPath(moduleCachePathStr);
  llvm::sys::path::append(outputPath, MID.ModuleName + "-" + MID.ContextHash);
  switch (MOK) {
  case ModuleOutputKind::ModuleFile:
    llvm::sys::path::replace_extension(
        outputPath, getExtension(swift::file_types::TY_ClangModuleFile));
    break;
  case ModuleOutputKind::DependencyFile:
    llvm::sys::path::replace_extension(
        outputPath, getExtension(swift::file_types::TY_Dependencies));
    break;
  case ModuleOutputKind::DependencyTargets:
    return MID.ModuleName + "-" + MID.ContextHash;
  case ModuleOutputKind::DiagnosticSerializationFile:
    llvm::sys::path::replace_extension(
        outputPath, getExtension(swift::file_types::TY_SerializedDiagnostics));
    break;
  }
  return outputPath.str().str();
}

// Add search paths.
// Note: This is handled differently for the Clang importer itself, which
// adds search paths to Clang's data structures rather than to its
// command line.
static void addSearchPathInvocationArguments(
    std::vector<std::string> &invocationArgStrs, ASTContext &ctx) {
  SearchPathOptions &searchPathOpts = ctx.SearchPathOpts;
  for (const auto &framepath : searchPathOpts.getFrameworkSearchPaths()) {
    invocationArgStrs.push_back(framepath.IsSystem ? "-iframework" : "-F");
    invocationArgStrs.push_back(framepath.Path);
  }

  for (const auto &path : searchPathOpts.getImportSearchPaths()) {
    invocationArgStrs.push_back("-I");
    invocationArgStrs.push_back(path);
  }
}

/// Create the command line for Clang dependency scanning.
static std::vector<std::string> getClangDepScanningInvocationArguments(
    ASTContext &ctx, llvm::Optional<StringRef> sourceFileName = llvm::None) {
  std::vector<std::string> commandLineArgs =
      ClangImporter::getClangArguments(ctx);
  addSearchPathInvocationArguments(commandLineArgs, ctx);

  auto sourceFilePos = std::find(
      commandLineArgs.begin(), commandLineArgs.end(),
      "<swift-imported-modules>");
  assert(sourceFilePos != commandLineArgs.end());
  if (sourceFileName.has_value())
    *sourceFilePos = sourceFileName->str();
  else
    commandLineArgs.erase(sourceFilePos);

  // HACK! Drop the -fmodule-format= argument and the one that
  // precedes it.
  {
    auto moduleFormatPos = std::find_if(commandLineArgs.begin(),
                                        commandLineArgs.end(),
                                        [](StringRef arg) {
      return arg.startswith("-fmodule-format=");
    });
    assert(moduleFormatPos != commandLineArgs.end());
    assert(moduleFormatPos != commandLineArgs.begin());
    commandLineArgs.erase(moduleFormatPos-1, moduleFormatPos+1);
  }

  // HACK: No -fsyntax-only here?
  {
    auto syntaxOnlyPos = std::find(commandLineArgs.begin(),
                                   commandLineArgs.end(),
                                   "-fsyntax-only");
    assert(syntaxOnlyPos != commandLineArgs.end());
    *syntaxOnlyPos = "-c";
  }

  // The Clang modules produced by ClangImporter are always embedded in an
  // ObjectFilePCHContainer and contain -gmodules debug info.
  commandLineArgs.push_back("-gmodules");

  return commandLineArgs;
}

/// Record the module dependencies we found by scanning Clang modules into
/// the module dependencies cache.
void ClangImporter::recordModuleDependencies(
    ModuleDependenciesCache &cache,
    const ModuleDepsGraph &clangModuleDependencies) {
  auto &ctx = Impl.SwiftContext;

  // This scanner invocation's already-captured APINotes version
  std::vector<std::string> capturedPCMArgs = {
    "-Xcc",
    ("-fapinotes-swift-version=" +
     ctx.LangOpts.EffectiveLanguageVersion.asAPINotesVersionString())
  };

  for (const auto &clangModuleDep : clangModuleDependencies) {
    // If we've already cached this information, we're done.
    if (cache.hasDependency(
                    clangModuleDep.ID.ModuleName,
                    ModuleDependencyKind::Clang))
      continue;

    // Record this module as one we have now seen, to prevent future
    // scand unnecessarily return it as a result
    cache.addSeenClangModule(clang::tooling::dependencies::ModuleID{
        clangModuleDep.ID.ModuleName, clangModuleDep.ID.ContextHash});

    // File dependencies for this module.
    std::vector<std::string> fileDeps;
    for (const auto &fileDep : clangModuleDep.FileDeps) {
      fileDeps.push_back(fileDep.getKey().str());
    }

    std::vector<std::string> swiftArgs;
    auto addClangArg = [&](Twine arg) {
      swiftArgs.push_back("-Xcc");
      swiftArgs.push_back(arg.str());
    };

    // We are using Swift frontend mode.
    swiftArgs.push_back("-frontend");

    // Swift frontend action: -emit-pcm
    swiftArgs.push_back("-emit-pcm");
    swiftArgs.push_back("-module-name");
    swiftArgs.push_back(clangModuleDep.ID.ModuleName);

    // We pass the entire argument list via -Xcc, so the invocation should
    // use extra clang options alone.
    swiftArgs.push_back("-only-use-extra-clang-opts");

    auto pcmPath = moduleCacheRelativeLookupModuleOutput(
        clangModuleDep.ID, ModuleOutputKind::ModuleFile,
        cache.getModuleOutputPath());
    swiftArgs.push_back("-o");
    swiftArgs.push_back(pcmPath);

    // Ensure that the resulting PCM build invocation uses Clang frontend
    // directly
    swiftArgs.push_back("-direct-clang-cc1-module-build");

    // Swift frontend option for input file path (Foo.modulemap).
    swiftArgs.push_back(clangModuleDep.ClangModuleMapFile);

    // Handle VFSOverlay.
    if (!ctx.SearchPathOpts.VFSOverlayFiles.empty()) {
      for (auto &overlay : ctx.SearchPathOpts.VFSOverlayFiles) {
        swiftArgs.push_back("-vfsoverlay");
        swiftArgs.push_back(overlay);
      }
    }

    // Add args reported by the scanner.

    // Round-trip clang args to canonicalize and clear the options that swift
    // compiler doesn't need.
    clang::CompilerInvocation depsInvocation;
    clang::DiagnosticsEngine clangDiags(new clang::DiagnosticIDs(),
                                        new clang::DiagnosticOptions(),
                                        new clang::IgnoringDiagConsumer());

    llvm::SmallVector<const char*> clangArgs;
    llvm::for_each(clangModuleDep.BuildArguments, [&](const std::string &Arg) {
      clangArgs.push_back(Arg.c_str());
    });

    bool success = clang::CompilerInvocation::CreateFromArgs(
        depsInvocation, clangArgs, clangDiags);
    (void)success;
    assert(success && "clang option from dep scanner round trip failed");

    // Clear the cache key for module. The module key is computed from clang
    // invocation, not swift invocation.
    depsInvocation.getFrontendOpts().ModuleCacheKeys.clear();

    // FIXME: workaround for rdar://105684525: find the -ivfsoverlay option
    // from clang scanner and pass to swift.
    for (auto overlay : depsInvocation.getHeaderSearchOpts().VFSOverlayFiles) {
      if (llvm::is_contained(ctx.SearchPathOpts.VFSOverlayFiles, overlay))
        continue;
      swiftArgs.push_back("-vfsoverlay");
      swiftArgs.push_back(overlay);
    }

    llvm::BumpPtrAllocator allocator;
    llvm::StringSaver saver(allocator);
    clangArgs.clear();
    depsInvocation.generateCC1CommandLine(
        clangArgs,
        [&saver](const llvm::Twine &T) { return saver.save(T).data(); });

    llvm::for_each(clangArgs, addClangArg);

    // CASFileSystemRootID.
    std::string RootID = clangModuleDep.CASFileSystemRootID
                             ? clangModuleDep.CASFileSystemRootID->toString()
                             : "";

    std::string IncludeTree =
        clangModuleDep.IncludeTreeID ? *clangModuleDep.IncludeTreeID : "";

    if (ctx.ClangImporterOpts.CASOpts) {
      swiftArgs.push_back("-cache-compile-job");
      if (!ctx.ClangImporterOpts.CASOpts->CASPath.empty()) {
        swiftArgs.push_back("-cas-path");
        swiftArgs.push_back(ctx.ClangImporterOpts.CASOpts->CASPath);
      }
      if (!ctx.ClangImporterOpts.CASOpts->PluginPath.empty()) {
        swiftArgs.push_back("-cas-plugin-path");
        swiftArgs.push_back(ctx.ClangImporterOpts.CASOpts->PluginPath);
        for (auto Opt : ctx.ClangImporterOpts.CASOpts->PluginOptions) {
          swiftArgs.push_back("-cas-plugin-option");
          swiftArgs.push_back(
              (llvm::Twine(Opt.first) + "=" + Opt.second).str());
        }
      }
    }

    if (!RootID.empty()) {
      swiftArgs.push_back("-cas-fs");
      swiftArgs.push_back(RootID);
    }

    if (!IncludeTree.empty()) {
      swiftArgs.push_back("-clang-include-tree");
      swiftArgs.push_back("-clang-include-tree-root");
      swiftArgs.push_back(IncludeTree);
    }

    // Module-level dependencies.
    llvm::StringSet<> alreadyAddedModules;
    auto dependencies = ModuleDependencyInfo::forClangModule(
        pcmPath, clangModuleDep.ClangModuleMapFile,
        clangModuleDep.ID.ContextHash, swiftArgs, fileDeps, capturedPCMArgs,
        RootID, IncludeTree, /*module-cache-key*/ "");
    for (const auto &moduleName : clangModuleDep.ClangModuleDeps) {
      dependencies.addModuleImport(moduleName.ModuleName, &alreadyAddedModules);
      // It is safe to assume that all dependencies of a Clang module are Clang modules.
      // Doing this allows us to skip "resolving" Clang modules down the line.
      dependencies.addModuleDependency({moduleName.ModuleName, ModuleDependencyKind::Clang});
    }
    dependencies.setIsResolved(true);

    cache.recordDependency(clangModuleDep.ID.ModuleName,
                             std::move(dependencies));
  }
}

void ClangImporter::recordBridgingHeaderOptions(
    ModuleDependencyInfo &MDI,
    const clang::tooling::dependencies::TranslationUnitDeps &deps) {
  auto &ctx = Impl.SwiftContext;

  std::vector<std::string> swiftArgs;
  auto addClangArg = [&](Twine arg) {
    swiftArgs.push_back("-Xcc");
    swiftArgs.push_back(arg.str());
  };

  // We are using Swift frontend mode.
  swiftArgs.push_back("-frontend");

  // Swift frontend action: -emit-pcm
  swiftArgs.push_back("-emit-pch");

  // We pass the entire argument list via -Xcc, so the invocation should
  // use extra clang options alone.
  swiftArgs.push_back("-only-use-extra-clang-opts");

  // Ensure that the resulting PCM build invocation uses Clang frontend
  // directly
  swiftArgs.push_back("-direct-clang-cc1-module-build");

  // Add args reported by the scanner.

  // Round-trip clang args to canonicalize and clear the options that swift
  // compiler doesn't need.
  clang::CompilerInvocation depsInvocation;
  clang::DiagnosticsEngine clangDiags(new clang::DiagnosticIDs(),
                                      new clang::DiagnosticOptions(),
                                      new clang::IgnoringDiagConsumer());

  llvm::SmallVector<const char *> clangArgs;
  llvm::for_each(deps.Commands[0].Arguments, [&](const std::string &Arg) {
    clangArgs.push_back(Arg.c_str());
  });

  bool success = clang::CompilerInvocation::CreateFromArgs(
      depsInvocation, clangArgs, clangDiags);
  (void)success;
  assert(success && "clang option from dep scanner round trip failed");

  // Clear the cache key for module. The module key is computed from clang
  // invocation, not swift invocation.
  depsInvocation.getFrontendOpts().ProgramAction =
      clang::frontend::ActionKind::GeneratePCH;
  depsInvocation.getFrontendOpts().ModuleCacheKeys.clear();
  depsInvocation.getFrontendOpts().OutputFile = "";

  llvm::BumpPtrAllocator allocator;
  llvm::StringSaver saver(allocator);
  clangArgs.clear();
  depsInvocation.generateCC1CommandLine(
      clangArgs,
      [&saver](const llvm::Twine &T) { return saver.save(T).data(); });

  llvm::for_each(clangArgs, addClangArg);

  if (ctx.ClangImporterOpts.CASOpts) {
    swiftArgs.push_back("-cache-compile-job");
    if (!ctx.ClangImporterOpts.CASOpts->CASPath.empty()) {
      swiftArgs.push_back("-cas-path");
      swiftArgs.push_back(ctx.ClangImporterOpts.CASOpts->CASPath);
    }
    if (!ctx.ClangImporterOpts.CASOpts->PluginPath.empty()) {
      swiftArgs.push_back("-cas-plugin-path");
      swiftArgs.push_back(ctx.ClangImporterOpts.CASOpts->PluginPath);
      for (auto Opt : ctx.ClangImporterOpts.CASOpts->PluginOptions) {
        swiftArgs.push_back("-cas-plugin-option");
        swiftArgs.push_back((llvm::Twine(Opt.first) + "=" + Opt.second).str());
      }
    }
  }

  if (auto Tree = deps.IncludeTreeID) {
    swiftArgs.push_back("-clang-include-tree");
    swiftArgs.push_back("-clang-include-tree-root");
    swiftArgs.push_back(*Tree);
  }
  if (auto CASFS = deps.CASFileSystemRootID) {
    swiftArgs.push_back("-cas-fs");
    swiftArgs.push_back(*CASFS);
  }

  MDI.updateBridgingHeaderCommandLine(swiftArgs);
}

// The Swift compiler does not have a concept of a working directory.
// It is instead handled by the Swift driver by resolving relative paths
// according to the driver's notion of a working directory. On the other hand,
// Clang does have a concept working directory which may be specified on this
// Clang invocation with '-working-directory'. If so, it is crucial that we
// use this directory as an argument to the Clang scanner invocation below.
static llvm::Optional<std::string>
computeClangWorkingDirectory(const std::vector<std::string> &commandLineArgs,
                             const ASTContext &ctx) {
  std::string workingDir;
  auto clangWorkingDirPos = std::find(
      commandLineArgs.rbegin(), commandLineArgs.rend(), "-working-directory");
  if (clangWorkingDirPos == commandLineArgs.rend())
    workingDir =
        ctx.SourceMgr.getFileSystem()->getCurrentWorkingDirectory().get();
  else {
    if (clangWorkingDirPos - 1 == commandLineArgs.rend()) {
      ctx.Diags.diagnose(SourceLoc(), diag::clang_dependency_scan_error,
                         "Missing '-working-directory' argument");
      return llvm::None;
    }
    workingDir = *(clangWorkingDirPos - 1);
  }
  return workingDir;
}

llvm::Optional<const ModuleDependencyInfo *>
ClangImporter::getModuleDependencies(StringRef moduleName,
                                     ModuleDependenciesCache &cache,
                                     InterfaceSubContextDelegate &delegate,
                                     bool isTestableImport) {
  auto &ctx = Impl.SwiftContext;
  // Determine the command-line arguments for dependency scanning.
  std::vector<std::string> commandLineArgs =
      getClangDepScanningInvocationArguments(ctx);
  auto optionalWorkingDir = computeClangWorkingDirectory(commandLineArgs, ctx);
  if (!optionalWorkingDir) {
    ctx.Diags.diagnose(SourceLoc(), diag::clang_dependency_scan_error,
                       "Missing '-working-directory' argument");
    return llvm::None;
  }
  std::string workingDir = *optionalWorkingDir;

  auto moduleCachePath = cache.getModuleOutputPath();
  auto lookupModuleOutput =
      [moduleCachePath](const ModuleID &MID,
                        ModuleOutputKind MOK) -> std::string {
    return moduleCacheRelativeLookupModuleOutput(MID, MOK, moduleCachePath);
  };

  auto clangModuleDependencies =
      cache.getClangScannerTool().getModuleDependencies(
          moduleName, commandLineArgs, workingDir,
          cache.getAlreadySeenClangModules(), lookupModuleOutput);
  if (!clangModuleDependencies) {
    auto errorStr = toString(clangModuleDependencies.takeError());
    // We ignore the "module 'foo' not found" error, the Swift dependency
    // scanner will report such an error only if all of the module loaders
    // fail as well.
    if (errorStr.find("fatal error: module '" + moduleName.str() +
                      "' not found") == std::string::npos)
      ctx.Diags.diagnose(SourceLoc(), diag::clang_dependency_scan_error,
                         errorStr);
    return llvm::None;
  }

  // Record module dependencies for each module we found.
  recordModuleDependencies(cache, *clangModuleDependencies);
  return cache.findDependency(moduleName, ModuleDependencyKind::Clang);
}

bool ClangImporter::addBridgingHeaderDependencies(
    StringRef moduleName, ModuleDependencyKind moduleKind,
    ModuleDependenciesCache &cache) {
  auto &ctx = Impl.SwiftContext;
  auto optionalTargetModule = cache.findDependency(moduleName, moduleKind);
  assert(optionalTargetModule.has_value());
  auto targetModule = *(optionalTargetModule.value());

  // If we've already recorded bridging header dependencies, we're done.
  if (auto swiftInterfaceDeps = targetModule.getAsSwiftInterfaceModule()) {
    if (!swiftInterfaceDeps->textualModuleDetails.bridgingSourceFiles.empty() ||
        !swiftInterfaceDeps->textualModuleDetails.bridgingModuleDependencies
             .empty())
      return false;
  } else if (auto swiftSourceDeps = targetModule.getAsSwiftSourceModule()) {
    if (!swiftSourceDeps->textualModuleDetails.bridgingSourceFiles.empty() ||
        !swiftSourceDeps->textualModuleDetails.bridgingModuleDependencies
             .empty())
      return false;
  } else {
    llvm_unreachable("Unexpected module dependency kind");
  }

  // Retrieve the bridging header.
  std::string bridgingHeader = *(targetModule.getBridgingHeader());

  // Determine the command-line arguments for dependency scanning.
  std::vector<std::string> commandLineArgs =
      getClangDepScanningInvocationArguments(ctx, StringRef(bridgingHeader));
  auto optionalWorkingDir = computeClangWorkingDirectory(commandLineArgs, ctx);
  if (!optionalWorkingDir) {
    ctx.Diags.diagnose(SourceLoc(), diag::clang_dependency_scan_error,
                       "Missing '-working-directory' argument");
    return true;
  }
  std::string workingDir = *optionalWorkingDir;

  auto moduleCachePath = getModuleCachePathFromClang(getClangInstance());
  auto lookupModuleOutput =
      [moduleCachePath](const ModuleID &MID,
                        ModuleOutputKind MOK) -> std::string {
    return moduleCacheRelativeLookupModuleOutput(MID, MOK, moduleCachePath);
  };

  auto clangModuleDependencies =
      cache.getClangScannerTool().getTranslationUnitDependencies(
          commandLineArgs, workingDir, cache.getAlreadySeenClangModules(),
          lookupModuleOutput);
  if (!clangModuleDependencies) {
    // FIXME: Route this to a normal diagnostic.
    llvm::logAllUnhandledErrors(clangModuleDependencies.takeError(), llvm::errs());
    return true;
  }

  // Record module dependencies for each module we found.
  recordModuleDependencies(cache, clangModuleDependencies->ModuleGraph);

  // Record dependencies for the source files the bridging header includes.
  for (const auto &fileDep : clangModuleDependencies->FileDeps)
    targetModule.addBridgingSourceFile(fileDep);

  // ... and all module dependencies.
  llvm::StringSet<> alreadyAddedModules;
  for (const auto &moduleDep : clangModuleDependencies->ModuleGraph) {
    targetModule.addBridgingModuleDependency(moduleDep.ID.ModuleName,
                                             alreadyAddedModules);
  }

  if (auto TreeID = clangModuleDependencies->IncludeTreeID)
    targetModule.addBridgingHeaderIncludeTree(*TreeID);

  recordBridgingHeaderOptions(targetModule, *clangModuleDependencies);

  // Update the cache with the new information for the module.
  cache.updateDependency({moduleName.str(), moduleKind}, targetModule);

  return false;
}
