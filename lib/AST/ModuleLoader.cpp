//===--- ModuleLoader.cpp - Swift Language Module Implementation ----------===//
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
//
//  This file implements the ModuleLoader class and/or any helpers.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsCommon.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/SourceManager.h"
#include "clang/Frontend/Utils.h"
#include "swift/ClangImporter/ClangImporter.h"

namespace llvm {
class FileCollectorBase;
}

namespace swift {

DependencyTracker::DependencyTracker(
    IntermoduleDepTrackingMode Mode,
    std::shared_ptr<llvm::FileCollectorBase> FileCollector)
    // NB: The ClangImporter believes it's responsible for the construction of
    // this instance, and it static_cast<>s the instance pointer to its own
    // subclass based on that belief. If you change this to be some other
    // instance, you will need to change ClangImporter's code to handle the
    // difference.
    : clangCollector(
          ClangImporter::createDependencyCollector(Mode, FileCollector)) {}

void
DependencyTracker::addDependency(StringRef File, bool IsSystem) {
  // DependencyTracker exposes an interface that (intentionally) does not talk
  // about clang at all, nor about missing deps. It does expose an IsSystem
  // dimension, which we accept and pass along to the clang DependencyCollector.
  clangCollector->maybeAddDependency(File, /*FromModule=*/false,
                                     IsSystem, /*IsModuleFile=*/false,
                                     /*IsMissing=*/false);
}

void DependencyTracker::addIncrementalDependency(StringRef File,
                                                 Fingerprint FP) {
  if (incrementalDepsUniquer.insert(File).second) {
    incrementalDeps.emplace_back(File.str(), FP);
  }
}

ArrayRef<std::string>
DependencyTracker::getDependencies() const {
  return clangCollector->getDependencies();
}

ArrayRef<DependencyTracker::IncrementalDependency>
DependencyTracker::getIncrementalDependencies() const {
  return incrementalDeps;
}

std::shared_ptr<clang::DependencyCollector>
DependencyTracker::getClangCollector() {
  return clangCollector;
}

static bool findOverlayFilesInDirectory(ASTContext &ctx, StringRef path,
                                        StringRef moduleName,
                                        SourceLoc diagLoc,
                                        llvm::function_ref<void(StringRef)> callback) {
  using namespace llvm::sys;
  using namespace file_types;

  auto fs = ctx.SourceMgr.getFileSystem();

  std::error_code error;
  for (auto dir = fs->dir_begin(path, error);
       !error && dir != llvm::vfs::directory_iterator();
       dir.increment(error)) {
    StringRef file = dir->path();
    if (lookupTypeForExtension(path::extension(file)) != TY_SwiftOverlayFile)
      continue;

    callback(file);
  }

  // A CAS file list returns operation not permitted on directory iterations.
  if (error && error != std::errc::no_such_file_or_directory &&
      error != std::errc::operation_not_permitted) {
    ctx.Diags.diagnose(diagLoc, diag::cannot_list_swiftcrossimport_dir,
                       moduleName, error.message(), path);
  }
  return !error;
}

static void findOverlayFilesInternal(ASTContext &ctx, StringRef moduleDefiningPath,
                             StringRef moduleName,
                             SourceLoc diagLoc,
                             llvm::function_ref<void(StringRef)> callback) {
  using namespace llvm::sys;
  using namespace file_types;
  auto &langOpts = ctx.LangOpts;
  // This method constructs several paths to directories near the module and
  // scans them for .swiftoverlay files. These paths can be in various
  // directories and have a few different filenames at the end, but I'll
  // illustrate the path transformations by showing examples for a module
  // defined by a swiftinterface at:
  //
  // /usr/lib/swift/FooKit.swiftmodule/x86_64-apple-macos.swiftinterface

  // dirPath = /usr/lib/swift/FooKit.swiftmodule
  SmallString<64> dirPath{moduleDefiningPath};

  // dirPath = /usr/lib/swift/
  path::remove_filename(dirPath);

  // dirPath = /usr/lib/swift/FooKit.swiftcrossimport
  path::append(dirPath, moduleName);
  path::replace_extension(dirPath, getExtension(TY_SwiftCrossImportDir));

  // Search for swiftoverlays that apply to all platforms.
  if (!findOverlayFilesInDirectory(ctx, dirPath, moduleName, diagLoc, callback))
    // If we diagnosed an error, or we didn't find the directory at all, don't
    // bother trying the target-specific directories.
    return;

  // dirPath = /usr/lib/swift/FooKit.swiftcrossimport/x86_64-apple-macos
  auto moduleTriple = getTargetSpecificModuleTriple(langOpts.Target);
  path::append(dirPath, moduleTriple.str());

  // Search for swiftoverlays specific to the target triple's platform.
  findOverlayFilesInDirectory(ctx, dirPath, moduleName, diagLoc, callback);

  // The rest of this handles target variant triples, which are only used for
  // certain MacCatalyst builds.
  if (!langOpts.TargetVariant)
    return;

  // dirPath = /usr/lib/swift/FooKit.swiftcrossimport/x86_64-apple-ios-macabi
  path::remove_filename(dirPath);
  auto moduleVariantTriple =
      getTargetSpecificModuleTriple(*langOpts.TargetVariant);
  path::append(dirPath, moduleVariantTriple.str());

  // Search for swiftoverlays specific to the target variant's platform.
  findOverlayFilesInDirectory(ctx, dirPath, moduleName, diagLoc, callback);
}

void ModuleLoader::findOverlayFiles(SourceLoc diagLoc, ModuleDecl *module,
                                    FileUnit *file) {
  using namespace llvm::sys;
  using namespace file_types;

  if (file->getModuleDefiningPath().empty())
    return;
  findOverlayFilesInternal(module->getASTContext(),
                           file->getModuleDefiningPath(),
                           module->getName().str(),
                           diagLoc,
                           [&](StringRef file) {
    module->addCrossImportOverlayFile(file);
    // FIXME: Better to add it only if we load it.
    if (dependencyTracker)
      dependencyTracker->addDependency(file, module->isSystemModule());
  });
}

llvm::StringMap<llvm::SmallSetVector<Identifier, 4>>
ModuleDependencyInfo::collectCrossImportOverlayNames(ASTContext &ctx,
                                                     StringRef moduleName) const {
  using namespace llvm::sys;
  using namespace file_types;
  llvm::Optional<std::string> modulePath;
  // A map from secondary module name to a vector of overlay names.
  llvm::StringMap<llvm::SmallSetVector<Identifier, 4>> result;

  switch (getKind()) {
    case swift::ModuleDependencyKind::SwiftInterface: {
      auto *swiftDep = getAsSwiftInterfaceModule();
      // Prefer interface path to binary module path if we have it.
      modulePath = swiftDep->swiftInterfaceFile;
      assert(modulePath.has_value());
      StringRef parentDir = llvm::sys::path::parent_path(*modulePath);
      if (llvm::sys::path::extension(parentDir) == ".swiftmodule") {
        modulePath = parentDir.str();
      }
      break;
    }
    case swift::ModuleDependencyKind::SwiftBinary: {
      auto *swiftBinaryDep = getAsSwiftBinaryModule();
      modulePath = swiftBinaryDep->compiledModulePath;
      assert(modulePath.has_value());
      StringRef parentDir = llvm::sys::path::parent_path(*modulePath);
      if (llvm::sys::path::extension(parentDir) == ".swiftmodule") {
        modulePath = parentDir.str();
      }
      break;
    }
    case swift::ModuleDependencyKind::Clang: {
      auto *clangDep = getAsClangModule();
      modulePath = clangDep->moduleMapFile;
      assert(modulePath.has_value());
      break;
    }
    case swift::ModuleDependencyKind::SwiftSource: {
      return result;
    }
    case swift::ModuleDependencyKind::SwiftPlaceholder: {
      return result;
    }
    case swift::ModuleDependencyKind::LastKind:
      llvm_unreachable("Unhandled dependency kind.");
  }
  // Mimic getModuleDefiningPath() for Swift and Clang module.
  findOverlayFilesInternal(ctx, *modulePath, moduleName, SourceLoc(),
                           [&](StringRef file) {
    StringRef bystandingModule;
    auto overlayNames =
      ModuleDecl::collectCrossImportOverlay(ctx, file, moduleName,
                                            bystandingModule);
    result[bystandingModule] = std::move(overlayNames);
  });
  return result;
}
} // namespace swift
