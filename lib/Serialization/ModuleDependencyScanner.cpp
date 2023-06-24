//===--- ModuleDependencyScanner.cpp - Compute module dependencies --------===//
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

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Serialization/ModuleDependencyScanner.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include "ModuleFileSharedCore.h"

#include <algorithm>
using namespace swift;
using llvm::ErrorOr;

std::error_code ModuleDependencyScanner::findModuleFilesInDirectory(
    ImportPath::Element ModuleID, const SerializedModuleBaseName &BaseName,
    SmallVectorImpl<char> *ModuleInterfacePath,
    SmallVectorImpl<char> *ModuleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *ModuleSourceInfoBuffer,
    bool skipBuildingInterface, bool IsFramework,
    bool isTestableDependencyLookup) {
  using namespace llvm::sys;

  auto &fs = *Ctx.SourceMgr.getFileSystem();

  auto ModPath = BaseName.getName(file_types::TY_SwiftModuleFile);
  auto InPath = BaseName.getName(file_types::TY_SwiftModuleInterfaceFile);

  if (LoadMode == ModuleLoadingMode::OnlySerialized || !fs.exists(InPath)) {
    if (fs.exists(ModPath)) {
      // The module file will be loaded directly.
      auto dependencies = scanModuleFile(ModPath, IsFramework);
      if (dependencies) {
        this->dependencies = std::move(dependencies.get());
        return std::error_code();
      }
      return dependencies.getError();
    } else {
      return std::make_error_code(std::errc::no_such_file_or_directory);
    }
  }
  assert(fs.exists(InPath));

  // Use the private interface file if exits.
  auto PrivateInPath =
  BaseName.getName(file_types::TY_PrivateSwiftModuleInterfaceFile);
  if (fs.exists(PrivateInPath)) {
    InPath = PrivateInPath;
  }
  auto dependencies = scanInterfaceFile(InPath, IsFramework,
                                        isTestableDependencyLookup);
  if (dependencies) {
    this->dependencies = std::move(dependencies.get());
    return std::error_code();
  }

  return dependencies.getError();
}

bool PlaceholderSwiftModuleScanner::findModule(
    ImportPath::Element moduleID, SmallVectorImpl<char> *moduleInterfacePath,
    SmallVectorImpl<char> *moduleInterfaceSourcePath,
    std::unique_ptr<llvm::MemoryBuffer> *moduleBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *moduleDocBuffer,
    std::unique_ptr<llvm::MemoryBuffer> *moduleSourceInfoBuffer,
    bool skipBuildingInterface, bool isTestableDependencyLookup,
    bool &isFramework, bool &isSystemModule) {
  StringRef moduleName = Ctx.getRealModuleName(moduleID.Item).str();
  auto it = PlaceholderDependencyModuleMap.find(moduleName);
  if (it == PlaceholderDependencyModuleMap.end()) {
    return false;
  }
  auto &moduleInfo = it->getValue();
  auto dependencies = ModuleDependencyInfo::forPlaceholderSwiftModuleStub(
      moduleInfo.modulePath,
      moduleInfo.moduleDocPath.has_value() ?
            moduleInfo.moduleDocPath.value() : "",
      moduleInfo.moduleSourceInfoPath.has_value() ?
            moduleInfo.moduleSourceInfoPath.value() : "");
  this->dependencies = std::move(dependencies);
  return true;
}

static std::vector<std::string> getCompiledCandidates(ASTContext &ctx,
                                                      StringRef moduleName,
                                                      StringRef interfacePath) {
  return ctx.getModuleInterfaceChecker()->getCompiledModuleCandidatesForInterface(
      moduleName.str(), interfacePath);
}

ErrorOr<ModuleDependencyInfo> ModuleDependencyScanner::scanInterfaceFile(
    Twine moduleInterfacePath, bool isFramework, bool isTestableImport) {
  // Create a module filename.
  // FIXME: Query the module interface loader to determine an appropriate
  // name for the module, which includes an appropriate hash.
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  auto realModuleName = Ctx.getRealModuleName(moduleName);
  llvm::SmallString<32> modulePath = realModuleName.str();
  llvm::sys::path::replace_extension(modulePath, newExt);
  Optional<ModuleDependencyInfo> Result;

  // FIXME: Consider not spawning a sub-instance for this
  std::error_code code =
    astDelegate.runInSubContext(realModuleName.str(),
                                              moduleInterfacePath.str(),
                                              StringRef(),
                                              SourceLoc(),
                [&](ASTContext &Ctx, ModuleDecl *mainMod,
                    ArrayRef<StringRef> BaseArgs,
                    ArrayRef<StringRef> PCMArgs, StringRef Hash) {
    assert(mainMod);
    std::string InPath = moduleInterfacePath.str();
    auto compiledCandidates = getCompiledCandidates(Ctx, realModuleName.str(),
                                                    InPath);
    std::vector<std::string> Args(BaseArgs.begin(), BaseArgs.end());

    // Add explicit Swift dependency compilation flags
    Args.push_back("-explicit-interface-module-build");
    Args.push_back("-disable-implicit-swift-modules");
    Args.push_back("-Xcc"); Args.push_back("-fno-implicit-modules");
    Args.push_back("-Xcc"); Args.push_back("-fno-implicit-module-maps");
    for (const auto &candidate : compiledCandidates) {
      Args.push_back("-candidate-module-file");
      Args.push_back(candidate);
    }

    // Compute the output path and add it to the command line
    SmallString<128> outputPathBase(moduleCachePath);
    llvm::sys::path::append(
        outputPathBase,
        moduleName.str() + "-" + Hash + "." +
            file_types::getExtension(file_types::TY_SwiftModuleFile));
    Args.push_back("-o");
    Args.push_back(outputPathBase.str().str());

    // Open the interface file.
    auto &fs = *Ctx.SourceMgr.getFileSystem();
    auto interfaceBuf = fs.getBufferForFile(moduleInterfacePath);
    if (!interfaceBuf) {
      return interfaceBuf.getError();
    }

    // Create a source file.
    unsigned bufferID = Ctx.SourceMgr.addNewSourceBuffer(std::move(interfaceBuf.get()));
    auto moduleDecl = ModuleDecl::create(realModuleName, Ctx);

    SourceFile::ParsingOptions parsingOpts;
    auto sourceFile = new (Ctx) SourceFile(
        *moduleDecl, SourceFileKind::Interface, bufferID, parsingOpts);
    moduleDecl->addAuxiliaryFile(*sourceFile);

    std::string RootID;
    if (dependencyTracker) {
      dependencyTracker->startTracking();
      dependencyTracker->trackFile(moduleInterfacePath);
      auto RootOrError = dependencyTracker->createTreeFromDependencies();
      if (!RootOrError)
        return llvm::errorToErrorCode(RootOrError.takeError());
      RootID = RootOrError->getID().toString();
    }

    std::vector<StringRef> ArgsRefs(Args.begin(), Args.end());
    Result = ModuleDependencyInfo::forSwiftInterfaceModule(
        outputPathBase.str().str(), InPath, compiledCandidates, ArgsRefs,
        PCMArgs, Hash, isFramework, RootID, /*module-cache-key*/ "");

    // Walk the source file to find the import declarations.
    llvm::StringSet<> alreadyAddedModules;
    Result->addModuleImport(*sourceFile, alreadyAddedModules);

    // Collect implicitly imported modules in case they are not explicitly
    // printed in the interface file, e.g. SwiftOnoneSupport.
    auto &imInfo = mainMod->getImplicitImportInfo();
    for (auto import: imInfo.AdditionalUnloadedImports) {
      Result->addModuleImport(import.module.getModulePath(), &alreadyAddedModules);
    }

    // For a `@testable` direct dependency, read in the dependencies
    // from an adjacent binary module, for completeness.
    if (isTestableImport) {
      auto adjacentBinaryModule = std::find_if(
          compiledCandidates.begin(), compiledCandidates.end(),
          [moduleInterfacePath](const std::string &candidate) {
            return llvm::sys::path::parent_path(candidate) ==
                   llvm::sys::path::parent_path(moduleInterfacePath.str());
          });
      if (adjacentBinaryModule != compiledCandidates.end()) {
        // Required modules.
        auto adjacentBinaryModuleRequiredImports = getImportsOfModule(
            *adjacentBinaryModule, ModuleLoadingBehavior::Required, isFramework,
            isRequiredOSSAModules(), Ctx.LangOpts.SDKName,
            Ctx.LangOpts.PackageName, Ctx.SourceMgr.getFileSystem().get(),
            Ctx.SearchPathOpts.DeserializedPathRecoverer);
        if (!adjacentBinaryModuleRequiredImports)
          return adjacentBinaryModuleRequiredImports.getError();
        auto adjacentBinaryModuleRequiredModuleImports =
          (*adjacentBinaryModuleRequiredImports).moduleImports;
#ifndef NDEBUG
        //  Verify that the set of required modules read out from the binary
        //  module is a super-set of module imports identified in the
        //  textual interface.
        for (const auto &requiredImport : Result->getModuleImports()) {
          assert(adjacentBinaryModuleRequiredModuleImports.contains(requiredImport) &&
                 "Expected adjacent binary module's import set to contain all "
                 "textual interface imports.");
        }
#endif

        for (const auto &requiredImport : adjacentBinaryModuleRequiredModuleImports)
          Result->addModuleImport(requiredImport.getKey(),
                                  &alreadyAddedModules);

        // Optional modules. Will be looked-up on a best-effort basis
        auto adjacentBinaryModuleOptionalImports = getImportsOfModule(
            *adjacentBinaryModule, ModuleLoadingBehavior::Optional, isFramework,
            isRequiredOSSAModules(), Ctx.LangOpts.SDKName,
            Ctx.LangOpts.PackageName, Ctx.SourceMgr.getFileSystem().get(),
            Ctx.SearchPathOpts.DeserializedPathRecoverer);
        if (!adjacentBinaryModuleOptionalImports)
          return adjacentBinaryModuleOptionalImports.getError();
        auto adjacentBinaryModuleOptionalModuleImports =
          (*adjacentBinaryModuleOptionalImports).moduleImports;
        for (const auto &optionalImport : adjacentBinaryModuleOptionalModuleImports)
          Result->addOptionalModuleImport(optionalImport.getKey(),
                                          &alreadyAddedModules);
      }
    }

    return std::error_code();
  });

  if (code) {
    return code;
  }
  return *Result;
}

Optional<const ModuleDependencyInfo*> SerializedModuleLoaderBase::getModuleDependencies(
    StringRef moduleName, ModuleDependenciesCache &cache,
    InterfaceSubContextDelegate &delegate, bool isTestableDependencyLookup) {
  ImportPath::Module::Builder builder(Ctx, moduleName, /*separator=*/'.');
  auto modulePath = builder.get();
  auto moduleId = modulePath.front().Item;
  // Instantiate dependency scanning "loaders".
  SmallVector<std::unique_ptr<ModuleDependencyScanner>, 2> scanners;
  // Placeholder dependencies must be resolved first, to prevent the
  // ModuleDependencyScanner from first discovering artifacts of a previous
  // build. Such artifacts are captured as compiledModuleCandidates in the
  // dependency graph of the placeholder dependency module itself.
  // FIXME: submodules?
  scanners.push_back(std::make_unique<PlaceholderSwiftModuleScanner>(
      Ctx, LoadMode, moduleId, Ctx.SearchPathOpts.PlaceholderDependencyModuleMap,
      delegate, cache.getScanService().createSwiftDependencyTracker()));
  scanners.push_back(std::make_unique<ModuleDependencyScanner>(
      Ctx, LoadMode, moduleId, delegate, ModuleDependencyScanner::MDS_plain,
      cache.getScanService().createSwiftDependencyTracker()));

  // Check whether there is a module with this name that we can import.
  assert(isa<PlaceholderSwiftModuleScanner>(scanners[0].get()) &&
         "Expected PlaceholderSwiftModuleScanner as the first dependency scanner loader.");
  for (auto &scanner : scanners) {
    if (scanner->canImportModule(modulePath, nullptr, isTestableDependencyLookup)) {
      // Record the dependencies.
      cache.recordDependency(moduleName, *(scanner->dependencies));
      return cache.findDependency(moduleName, scanner->dependencies->getKind());
    }
  }

  return None;
}
