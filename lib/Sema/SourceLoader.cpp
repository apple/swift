//===--- SourceLoader.cpp - Import .swift files as modules ----------------===//
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
/// A simple module loader that loads .swift source files.
///
//===----------------------------------------------------------------------===//

#include "swift/Sema/SourceLoader.h"
#include "swift/Subsystems.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/SourceFile.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/SaveAndRestore.h"
#include <system_error>

using namespace swift;

// FIXME: Basically the same as SerializedModuleLoader.
using FileOrError = llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>>;

static FileOrError findModule(ASTContext &ctx, Identifier moduleID,
                              SourceLoc importLoc) {
  llvm::SmallString<128> inputFilename;
  // Find a module with an actual, physical name on disk, in case
  // -module-alias is used (otherwise same).
  //
  // For example, if '-module-alias Foo=Bar' is passed in to the frontend,
  // and a source file has 'import Foo', a module called Bar (real name)
  // should be searched.
  StringRef moduleNameRef = ctx.getRealModuleName(moduleID).str();

  for (const auto &Path : ctx.SearchPathOpts.getImportSearchPaths()) {
    inputFilename = Path;
    llvm::sys::path::append(inputFilename, moduleNameRef);
    inputFilename.append(".swift");
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      ctx.SourceMgr.getFileSystem()->getBufferForFile(inputFilename.str());

    // Return if we loaded a file
    if (FileBufOrErr)
      return FileBufOrErr;
    // Or if we get any error other than the file not existing
    auto err = FileBufOrErr.getError();
    if (err != std::errc::no_such_file_or_directory)
      return FileBufOrErr;
  }

  return make_error_code(std::errc::no_such_file_or_directory);
}

void SourceLoader::collectVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  // TODO: Implement?
}

bool SourceLoader::canImportModule(ImportPath::Module path,
                                   ModuleVersionInfo *versionInfo,
                                   bool isTestableDependencyLookup) {
  // FIXME: Swift submodules?
  if (path.hasSubmodule())
    return false;

  auto ID = path[0];
  // Search the memory buffers to see if we can find this file on disk.
  FileOrError inputFileOrError = findModule(Ctx, ID.Item,
                                            ID.Loc);
  if (!inputFileOrError) {
    auto err = inputFileOrError.getError();
    if (err != std::errc::no_such_file_or_directory) {
      Ctx.Diags.diagnose(ID.Loc, diag::sema_opening_import,
                         ID.Item, err.message());
    }

    return false;
  }

  return true;
}

ModuleDecl *SourceLoader::loadModule(SourceLoc importLoc,
                                     ImportPath::Module path,
                                     bool AllowMemoryCache) {
  // FIXME: Swift submodules?
  if (path.size() > 1)
    return nullptr;

  auto moduleID = path[0];

  FileOrError inputFileOrError = findModule(Ctx, moduleID.Item,
                                            moduleID.Loc);
  if (!inputFileOrError) {
    auto err = inputFileOrError.getError();
    if (err != std::errc::no_such_file_or_directory) {
      Ctx.Diags.diagnose(moduleID.Loc, diag::sema_opening_import,
                         moduleID.Item, err.message());
    }

    return nullptr;
  }
  std::unique_ptr<llvm::MemoryBuffer> inputFile =
    std::move(inputFileOrError.get());

  if (dependencyTracker)
    dependencyTracker->addDependency(inputFile->getBufferIdentifier(),
                                     /*isSystem=*/false);

  unsigned bufferID;
  if (auto BufID =
       Ctx.SourceMgr.getIDForBufferIdentifier(inputFile->getBufferIdentifier()))
    bufferID = BufID.value();
  else
    bufferID = Ctx.SourceMgr.addNewSourceBuffer(std::move(inputFile));

  ImplicitImportInfo importInfo;
  importInfo.StdlibKind = Ctx.getStdlibModule() ? ImplicitStdlibKind::Stdlib
                                                : ImplicitStdlibKind::None;

  auto *importMod = ModuleDecl::create(moduleID.Item, Ctx, importInfo);
  if (EnableLibraryEvolution)
    importMod->setResilienceStrategy(ResilienceStrategy::Resilient);
  Ctx.addLoadedModule(importMod);

  auto *importFile =
      new (Ctx) SourceFile(*importMod, SourceFileKind::Library, bufferID,
                           SourceFile::getDefaultParsingOptions(Ctx.LangOpts));
  importMod->addFile(*importFile);
  performImportResolution(*importFile);
  importMod->setHasResolvedImports();
  bindExtensions(*importMod);
  return importMod;
}

void SourceLoader::loadExtensions(NominalTypeDecl *nominal,
                                  unsigned previousGeneration) {
  // Type-checking the source automatically loads all extensions; there's
  // nothing to do here.
}

llvm::Optional<const ModuleDependencyInfo *>
SourceLoader::getModuleDependencies(StringRef moduleName,
                                    ModuleDependenciesCache &cache,
                                    InterfaceSubContextDelegate &delegate,
                                    bool isTestableImport) {
  // FIXME: Implement?
  return llvm::None;
}
