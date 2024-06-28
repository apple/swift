//===---------- ScanningLoaders.cpp - Compute module dependencies ---------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticSuppression.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Serialization/ScanningLoaders.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Subsystems.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/CAS/CachingOnDiskFileSystem.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/PrefixMapper.h"
#include "llvm/Support/Threading.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <algorithm>
#include <system_error>

using namespace swift;

std::error_code SwiftModuleScanner::findModuleFilesInDirectory(
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
  auto InPath = BaseName.findInterfacePath(fs, Ctx);

  // Lookup binary module if it is a testable lookup, or only binary module
  // lookup, or interface file does not exist.
  if (LoadMode == ModuleLoadingMode::OnlySerialized ||
      isTestableDependencyLookup || !InPath) {
    if (fs.exists(ModPath)) {
      // The module file will be loaded directly.
      auto dependencies =
          scanModuleFile(ModPath, IsFramework, isTestableDependencyLookup);
      if (dependencies) {
        this->dependencies = std::move(dependencies.get());
        return std::error_code();
      }
      return dependencies.getError();
    }
    return std::make_error_code(std::errc::no_such_file_or_directory);
  }
  assert(InPath);

  auto dependencies =
      scanInterfaceFile(*InPath, IsFramework, isTestableDependencyLookup);
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
      moduleInfo.moduleDocPath.has_value() ? moduleInfo.moduleDocPath.value()
                                           : "",
      moduleInfo.moduleSourceInfoPath.has_value()
          ? moduleInfo.moduleSourceInfoPath.value()
          : "");
  this->dependencies = std::move(dependencies);
  return true;
}

void PlaceholderSwiftModuleScanner::parsePlaceholderModuleMap(
    StringRef fileName) {
  ExplicitModuleMapParser parser(Allocator);
  llvm::StringMap<ExplicitClangModuleInputInfo> ClangDependencyModuleMap;
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> fileBufOrErr =
      llvm::MemoryBuffer::getFile(fileName);
  if (!fileBufOrErr) {
    Ctx.Diags.diagnose(SourceLoc(), diag::explicit_swift_module_map_missing,
                       fileName);
    return;
  }
  auto result = parser.parseSwiftExplicitModuleMap(
      (*fileBufOrErr)->getMemBufferRef(), PlaceholderDependencyModuleMap,
      ClangDependencyModuleMap);
  if (result == std::errc::invalid_argument) {
    Ctx.Diags.diagnose(SourceLoc(),
                       diag::placeholder_dependency_module_map_corrupted,
                       fileName);
  } else if (result == std::errc::no_such_file_or_directory) {
    Ctx.Diags.diagnose(
        SourceLoc(), diag::placeholder_dependency_module_map_missing, fileName);
  }
}

static std::vector<std::string> getCompiledCandidates(ASTContext &ctx,
                                                      StringRef moduleName,
                                                      StringRef interfacePath) {
  return ctx.getModuleInterfaceChecker()
      ->getCompiledModuleCandidatesForInterface(moduleName.str(),
                                                interfacePath);
}

llvm::ErrorOr<ModuleDependencyInfo>
SwiftModuleScanner::scanInterfaceFile(Twine moduleInterfacePath,
                                      bool isFramework, bool isTestableImport) {
  // Create a module filename.
  // FIXME: Query the module interface loader to determine an appropriate
  // name for the module, which includes an appropriate hash.
  auto newExt = file_types::getExtension(file_types::TY_SwiftModuleFile);
  auto realModuleName = Ctx.getRealModuleName(moduleName);
  StringRef sdkPath = Ctx.SearchPathOpts.getSDKPath();
  llvm::SmallString<32> modulePath = realModuleName.str();
  llvm::sys::path::replace_extension(modulePath, newExt);
  std::optional<ModuleDependencyInfo> Result;
  std::error_code code = astDelegate.runInSubContext(
      realModuleName.str(), moduleInterfacePath.str(), sdkPath,
      StringRef(), SourceLoc(),
      [&](ASTContext &Ctx, ModuleDecl *mainMod, ArrayRef<StringRef> BaseArgs,
          ArrayRef<StringRef> PCMArgs, StringRef Hash) {
        assert(mainMod);
        std::string InPath = moduleInterfacePath.str();
        auto compiledCandidates =
            getCompiledCandidates(Ctx, realModuleName.str(), InPath);
        if (!compiledCandidates.empty() &&
            !Ctx.SearchPathOpts.NoScannerModuleValidation) {
          assert(compiledCandidates.size() == 1 &&
                 "Should only have 1 candidate module");
          auto BinaryDep = scanModuleFile(compiledCandidates[0], isFramework,
                                          isTestableImport);
          if (BinaryDep) {
            Result = *BinaryDep;
            return std::error_code();
          }

          // If return no such file, just fallback to use interface.
          if (BinaryDep.getError() != std::errc::no_such_file_or_directory)
            return BinaryDep.getError();
        }

        std::vector<std::string> Args(BaseArgs.begin(), BaseArgs.end());
        // Add explicit Swift dependency compilation flags
        Args.push_back("-explicit-interface-module-build");
        Args.push_back("-disable-implicit-swift-modules");

        // Handle clang arguments. For caching build, all arguments are passed
        // with `-direct-clang-cc1-module-build`.
        if (Ctx.ClangImporterOpts.ClangImporterDirectCC1Scan) {
          Args.push_back("-direct-clang-cc1-module-build");
          auto *importer =
              static_cast<ClangImporter *>(Ctx.getClangModuleLoader());
          for (auto &Arg : importer->getSwiftExplicitModuleDirectCC1Args()) {
            Args.push_back("-Xcc");
            Args.push_back(Arg);
          }
        } else {
          Args.push_back("-Xcc");
          Args.push_back("-fno-implicit-modules");
          Args.push_back("-Xcc");
          Args.push_back("-fno-implicit-module-maps");
        }
        for (const auto &candidate : compiledCandidates) {
          Args.push_back("-candidate-module-file");
          Args.push_back(candidate);
        }

        // Compute the output path and add it to the command line
        SmallString<128> outputPathBase(moduleOutputPath);
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
        unsigned bufferID =
            Ctx.SourceMgr.addNewSourceBuffer(std::move(interfaceBuf.get()));
        auto moduleDecl = ModuleDecl::create(realModuleName, Ctx);

        SourceFile::ParsingOptions parsingOpts;
        auto sourceFile = new (Ctx) SourceFile(
            *moduleDecl, SourceFileKind::Interface, bufferID, parsingOpts);
        moduleDecl->addAuxiliaryFile(*sourceFile);
        std::vector<StringRef> ArgsRefs(Args.begin(), Args.end());
        std::vector<StringRef> compiledCandidatesRefs(compiledCandidates.begin(),
                                                      compiledCandidates.end());

        // If this interface specified '-autolink-force-load', add it to the
        // set of linked libraries for this module.
        std::vector<LinkLibrary> linkLibraries;
        if (llvm::find(ArgsRefs, "-autolink-force-load") != ArgsRefs.end()) {
          std::string linkName = realModuleName.str().str();
          auto linkNameArgIt = llvm::find(ArgsRefs, "-module-link-name");
          if (linkNameArgIt != ArgsRefs.end())
            linkName = *(linkNameArgIt+1);
          linkLibraries.push_back({linkName,
                                   isFramework ? LibraryKind::Framework : LibraryKind::Library,
                                   true});
        }
        bool isStatic = llvm::find(ArgsRefs, "-static") != ArgsRefs.end();

        Result = ModuleDependencyInfo::forSwiftInterfaceModule(
            outputPathBase.str().str(), InPath, compiledCandidatesRefs,
            ArgsRefs, linkLibraries, PCMArgs, Hash, isFramework, isStatic, {},
            /*module-cache-key*/ "");

        if (Ctx.CASOpts.EnableCaching) {
          std::vector<std::string> clangDependencyFiles;
          auto clangImporter =
              static_cast<ClangImporter *>(Ctx.getClangModuleLoader());
          clangImporter->addClangInvovcationDependencies(clangDependencyFiles);
          llvm::for_each(clangDependencyFiles, [&](std::string &file) {
            Result->addAuxiliaryFile(file);
          });
        }

        // Walk the source file to find the import declarations.
        llvm::StringSet<> alreadyAddedModules;
        Result->addModuleImports(*sourceFile, alreadyAddedModules,
                                 &Ctx.SourceMgr);

        // Collect implicitly imported modules in case they are not explicitly
        // printed in the interface file, e.g. SwiftOnoneSupport.
        auto &imInfo = mainMod->getImplicitImportInfo();
        for (auto import : imInfo.AdditionalUnloadedImports) {
          Result->addModuleImport(import.module.getModulePath(),
                                  &alreadyAddedModules, &Ctx.SourceMgr);
        }

        return std::error_code();
      });

  if (code) {
    return code;
  }
  return *Result;
}

ModuleDependencyVector SerializedModuleLoaderBase::getModuleDependencies(
    Identifier moduleName, StringRef moduleOutputPath,
    llvm::IntrusiveRefCntPtr<llvm::cas::CachingOnDiskFileSystem> CacheFS,
    const llvm::DenseSet<clang::tooling::dependencies::ModuleID>
        &alreadySeenClangModules,
    clang::tooling::dependencies::DependencyScanningTool &clangScanningTool,
    InterfaceSubContextDelegate &delegate, llvm::TreePathPrefixMapper *mapper,
    bool isTestableDependencyLookup) {
  ImportPath::Module::Builder builder(moduleName);
  auto modulePath = builder.get();
  auto moduleId = modulePath.front().Item;

  // Instantiate dependency scanning "loaders".
  SmallVector<std::unique_ptr<SwiftModuleScanner>, 2> scanners;
  // Placeholder dependencies must be resolved first, to prevent the
  // ModuleDependencyScanner from first discovering artifacts of a previous
  // build. Such artifacts are captured as compiledModuleCandidates in the
  // dependency graph of the placeholder dependency module itself.
  // FIXME: submodules?
  scanners.push_back(std::make_unique<PlaceholderSwiftModuleScanner>(
      Ctx, LoadMode, moduleId, Ctx.SearchPathOpts.PlaceholderDependencyModuleMap,
      delegate, moduleOutputPath));
  scanners.push_back(std::make_unique<SwiftModuleScanner>(
      Ctx, LoadMode, moduleId, delegate, moduleOutputPath,
      SwiftModuleScanner::MDS_plain));

  // Check whether there is a module with this name that we can import.
  assert(isa<PlaceholderSwiftModuleScanner>(scanners[0].get()) &&
         "Expected PlaceholderSwiftModuleScanner as the first dependency "
         "scanner loader.");
  for (auto &scanner : scanners) {
    if (scanner->canImportModule(modulePath, SourceLoc(), nullptr,
                                 isTestableDependencyLookup)) {

      ModuleDependencyVector moduleDependnecies;
      moduleDependnecies.push_back(
          std::make_pair(ModuleDependencyID{moduleName.str().str(),
                                            scanner->dependencies->getKind()},
                         *(scanner->dependencies)));
      return moduleDependnecies;
    }
  }

  return {};
}
