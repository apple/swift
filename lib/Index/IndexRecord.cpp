//===--- IndexRecord.cpp --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2016 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See http://swift.org/LICENSE.txt for license information
// See http://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/Index/IndexRecord.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTPrinter.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Expr.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Pattern.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/Stmt.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/PathRemapper.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/IDE/ModuleInterfacePrinting.h"
#include "swift/Index/Index.h"
#include "clang/Basic/FileManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Index/IndexRecordWriter.h"
#include "clang/Index/IndexUnitWriter.h"
#include "clang/Index/IndexingAction.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Serialization/ASTReader.h"
#include "llvm/Support/Path.h"

using namespace swift;
using namespace swift::index;
using clang::index::IndexUnitWriter;
using clang::index::IndexRecordWriter;
using clang::index::SymbolRole;
using clang::index::SymbolRoleSet;

//===----------------------------------------------------------------------===//
// Index data collection and record writing
//===----------------------------------------------------------------------===//

namespace {
class SymbolTracker {
public:
  struct SymbolRelation {
    size_t symbolIndex;
    SymbolRoleSet roles;

    llvm::hash_code hash() const { return llvm::hash_combine(symbolIndex, roles); }
  };
  struct SymbolOccurrence {
    size_t symbolIndex;
    SymbolRoleSet roles;
    unsigned line;
    unsigned column;
    SmallVector<SymbolRelation, 3> related;

    llvm::hash_code hash() const {
      auto hash = llvm::hash_combine(symbolIndex, roles, line, column);
      for (auto &relation : related) {
        hash = llvm::hash_combine(hash, relation.hash());
      }
      return hash;
    }
  };
  struct Symbol {
    StringRef name;
    StringRef USR;
    StringRef group;

    SymbolInfo symInfo;
    unsigned isTestCandidate : 1;

    llvm::hash_code hash() const {
      return llvm::hash_combine(
          name, USR, group,
          static_cast<unsigned>(symInfo.Kind),
          static_cast<unsigned>(symInfo.SubKind),
          symInfo.Properties, isTestCandidate);
    }
  };

  Symbol *getSymbol(size_t index) {
    assert(index < symbols.size());
    return &symbols[index];
  }

  ArrayRef<SymbolOccurrence> getOccurrences() {
    if (!sorted) {
      std::stable_sort(occurrences.begin(), occurrences.end(),
          [](const SymbolOccurrence &a, const SymbolOccurrence& b) {
        if (a.line < b.line)
          return true;
        if (b.line < a.line)
          return false;
        return a.column < b.column;
      });
      sorted = true;
    }
    return occurrences;
  }

  size_t addSymbol(const IndexRelation &indexSym) {
    auto pair = USRToSymbol.insert(std::make_pair(indexSym.USR.data(),
                                                  symbols.size()));
    if (pair.second) {
      Symbol symbol{indexSym.name,
                    indexSym.USR,
                    indexSym.group,
                    indexSym.symInfo,
                    0};
      recordHash = llvm::hash_combine(recordHash, symbol.hash());
      symbols.push_back(std::move(symbol));
    }

    return pair.first->second;
  }

  void addOccurrence(const IndexSymbol &indexOccur) {
    sorted = false;

    SmallVector<SymbolRelation, 3> relations;
    for(IndexRelation indexRel: indexOccur.Relations) {
      relations.push_back({addSymbol(indexRel), indexRel.roles});
    }

    occurrences.push_back({/*symbolIndex=*/addSymbol(indexOccur),
                           indexOccur.roles,
                           indexOccur.line,
                           indexOccur.column,
                           std::move(relations)});

    recordHash = llvm::hash_combine(recordHash, occurrences.back().hash());
  }

  llvm::hash_code hashRecord() const { return recordHash; }

private:
  llvm::DenseMap<const char *, size_t> USRToSymbol;
  std::vector<Symbol> symbols;
  std::vector<SymbolOccurrence> occurrences;
  bool sorted = false;
  llvm::hash_code recordHash = 0;
};

class IndexRecordingConsumer : public IndexDataConsumer {
  SymbolTracker record;
  // Keep a USR map to uniquely identify Decls.
  // FIXME: if we just passed the original Decl * through we could use that,
  // which would also let us avoid producing the USR/Name/etc. for decls unless
  // we actually need it (once per Decl instead of once per occurrence).
  std::vector<IndexSymbol> symbolStack;

  bool includeLocals;

  std::function<void(SymbolTracker &)> onFinish;

public:
  IndexRecordingConsumer(bool includeLocals,
                         std::function<void(SymbolTracker &)> onFinish)
      : includeLocals(includeLocals), onFinish(std::move(onFinish)) {}

  void failed(StringRef error) override {
    // FIXME: expose errors?
  }

  bool startDependency(StringRef name, StringRef path, bool isClangModule, bool isSystem) override {
    return true;
  }
  bool finishDependency(bool isClangModule) override { return true; }
  Action startSourceEntity(const IndexSymbol &symbol) override {
    symbolStack.push_back(symbol);
    return Action::Continue;
  }
  bool finishSourceEntity(SymbolInfo sym, SymbolRoleSet roles) override {
    IndexSymbol symbol = std::move(symbolStack.back());
    symbolStack.pop_back();
    assert(!symbol.USR.empty());
    record.addOccurrence(symbol);
    return true;
  }

  void finish() override { onFinish(record); }

  bool indexLocals() override { return includeLocals; }
};

class StdlibGroupsIndexRecordingConsumer : public IndexDataConsumer {
  llvm::StringMap<std::unique_ptr<SymbolTracker>> TrackerByGroup;
  // Keep a USR map to uniquely identify Decls.
  // FIXME: if we just passed the original Decl * through we could use that,
  // which would also let us avoid producing the USR/Name/etc. for decls unless
  // we actually need it (once per Decl instead of once per occurrence).
  std::vector<IndexSymbol> symbolStack;

  std::function<bool(StringRef groupName, SymbolTracker &)> onFinish;

public:
  StdlibGroupsIndexRecordingConsumer(std::function<bool(StringRef groupName, SymbolTracker &)> onFinish)
      : onFinish(std::move(onFinish)) {}

  void failed(StringRef error) override {
    // FIXME: expose errors?
  }

  bool startDependency(StringRef name, StringRef path, bool isClangModule, bool isSystem) override {
    return true;
  }
  bool finishDependency(bool isClangModule) override { return true; }
  Action startSourceEntity(const IndexSymbol &symbol) override {
    symbolStack.push_back(symbol);
    return Action::Continue;
  }
  bool finishSourceEntity(SymbolInfo sym, SymbolRoleSet roles) override {
    IndexSymbol symbol = std::move(symbolStack.back());
    symbolStack.pop_back();
    assert(!symbol.USR.empty());
    StringRef groupName = findGroupForSymbol(symbol);
    auto &tracker = TrackerByGroup[groupName];
    if (!tracker) {
      tracker = std::make_unique<SymbolTracker>();
    }
    tracker->addOccurrence(symbol);
    return true;
  }

  void finish() override {
    SmallVector<std::pair<StringRef, SymbolTracker *>, 0> SortedGroups;
    for (auto &entry : TrackerByGroup) {
      SortedGroups.emplace_back(entry.first(), entry.second.get());
    }
    llvm::sort(SortedGroups, llvm::less_first());

    for (auto &pair : SortedGroups) {
      StringRef groupName = pair.first;
      SymbolTracker *tracker = pair.second;
      bool cont = onFinish(groupName, *tracker);
      if (!cont)
        break;
    }
  }

private:
  StringRef findGroupForSymbol(const IndexSymbol &sym);

};
} // end anonymous namespace

static StringRef findGroupNameForDecl(const Decl *D) {
  if (!D || isa<ModuleDecl>(D) || isa<TopLevelCodeDecl>(D))
    return StringRef();

  auto groupNameOpt = D->getGroupName();
  if (groupNameOpt)
    return *groupNameOpt;

  return findGroupNameForDecl(D->getDeclContext()->getInnermostDeclarationDeclContext());
}

StringRef StdlibGroupsIndexRecordingConsumer::findGroupForSymbol(const IndexSymbol &sym) {
  bool isDeclOrDef = sym.roles & ((SymbolRoleSet)SymbolRole::Declaration | (SymbolRoleSet)SymbolRole::Definition);
  if (isDeclOrDef) {
    if (!sym.group.empty())
      return sym.group;
    return findGroupNameForDecl(sym.decl);
  }

  for (auto &rel : sym.Relations) {
    if (!rel.group.empty())
      return rel.group;
    if (rel.decl)
      return findGroupNameForDecl(rel.decl);
  }
  llvm_unreachable("did not find group name for reference");
}

static bool writeRecord(SymbolTracker &record, std::string Filename,
                        std::string indexStorePath, DiagnosticEngine *diags,
                        std::string &outRecordFile) {
  IndexRecordWriter recordWriter(indexStorePath);
  std::string error;
  auto result = recordWriter.beginRecord(
      Filename, record.hashRecord(), error, &outRecordFile);
  switch (result) {
  case IndexRecordWriter::Result::Failure:
    diags->diagnose(SourceLoc(), diag::error_write_index_record, error);
    return true;
  case IndexRecordWriter::Result::AlreadyExists:
    return false;
  case IndexRecordWriter::Result::Success:
    break;
  }

  for (auto &occurrence : record.getOccurrences()) {
    SmallVector<clang::index::writer::SymbolRelation, 3> relations;
    for(SymbolTracker::SymbolRelation symbolRelation: occurrence.related) {
      relations.push_back({record.getSymbol(symbolRelation.symbolIndex), symbolRelation.roles});
    }

    recordWriter.addOccurrence(
        record.getSymbol(occurrence.symbolIndex), occurrence.roles,
        occurrence.line, occurrence.column, relations);
  }

  result = recordWriter.endRecord(error,
      [&](clang::index::writer::OpaqueDecl opaqueSymbol,
          SmallVectorImpl<char> &scratch) {
    auto *symbol = static_cast<const SymbolTracker::Symbol *>(opaqueSymbol);
    clang::index::writer::Symbol result;
    result.SymInfo = symbol->symInfo;
    result.Name = symbol->name;
    result.USR = symbol->USR;
    result.CodeGenName = ""; // FIXME
    return result;
  });

  if (result == IndexRecordWriter::Result::Failure) {
    diags->diagnose(SourceLoc(), diag::error_write_index_record, error);
    return true;
  }

  return false;
}

static std::unique_ptr<IndexRecordingConsumer>
makeRecordingConsumer(std::string Filename, std::string indexStorePath,
                      bool includeLocals, DiagnosticEngine *diags,
                      std::string *outRecordFile,
                      bool *outFailed) {
  return std::make_unique<IndexRecordingConsumer>(includeLocals,
                                                  [=](SymbolTracker &record) {
    *outFailed = writeRecord(record, Filename, indexStorePath, diags,
                             *outRecordFile);
  });
}

static bool
recordSourceFile(SourceFile *SF, StringRef indexStorePath,
                 bool includeLocals, DiagnosticEngine &diags,
                 llvm::function_ref<void(StringRef, StringRef)> callback) {
  std::string recordFile;
  bool failed = false;
  auto consumer =
      makeRecordingConsumer(SF->getFilename().str(), indexStorePath.str(),
                            includeLocals, &diags, &recordFile, &failed);
  indexSourceFile(SF, *consumer);

  if (!failed && !recordFile.empty())
    callback(recordFile, SF->getFilename());
  return failed;
}

//===----------------------------------------------------------------------===//
// Index unit file writing
//===----------------------------------------------------------------------===//

// Used to get std::string pointers to pass as writer::OpaqueModule.
namespace {
class StringScratchSpace {
  std::vector<std::unique_ptr<std::string>> StrsCreated;

public:
  const std::string *createString(StringRef str) {
    StrsCreated.emplace_back(std::make_unique<std::string>(str));
    return StrsCreated.back().get();
  }
};
}

static clang::index::writer::ModuleInfo
getModuleInfoFromOpaqueModule(clang::index::writer::OpaqueModule mod,
                              SmallVectorImpl<char> &Scratch) {
  clang::index::writer::ModuleInfo info;
  info.Name = *static_cast<const std::string*>(mod);
  return info;
}

static bool
emitDataForSwiftSerializedModule(ModuleDecl *module,
                                 StringRef indexStorePath,
                                 bool indexClangModules,
                                 bool indexSystemModules,
                                 bool skipStdlib,
                                 bool includeLocals,
                                 bool explicitModulebuild,
                                 StringRef targetTriple,
                                 const clang::CompilerInstance &clangCI,
                                 DiagnosticEngine &diags,
                                 IndexUnitWriter &parentUnitWriter,
                                 const PathRemapper &pathRemapper,
                                 SourceFile *initialFile);

static void
appendSymbolicInterfaceToIndexStorePath(SmallVectorImpl<char> &resultingPath) {
  llvm::sys::path::append(resultingPath, "interfaces");
}

static llvm::Error initSymbolicInterfaceStorePath(StringRef storePath) {
  SmallString<128> subPath = storePath;
  appendSymbolicInterfaceToIndexStorePath(subPath);
  std::error_code ec = llvm::sys::fs::create_directories(subPath);
  if (ec)
    return llvm::errorCodeToError(ec);
  return llvm::Error::success();
}

static void appendSymbolicInterfaceClangModuleFilename(
    StringRef filePath, SmallVectorImpl<char> &resultingPath) {
  llvm::sys::path::append(resultingPath, llvm::sys::path::filename(filePath));
  StringRef extension = ".symbolicswiftinterface";
  resultingPath.append(extension.begin(), extension.end());
}

// FIXME (Alex): Share code with IndexUnitWriter in LLVM after refactoring it.
static llvm::Expected<bool>
isFileUpToDateForOutputFile(StringRef filePath, StringRef timeCompareFilePath) {
  auto makeError = [](StringRef path, std::error_code ec) -> llvm::Error {
    std::string error;
    llvm::raw_string_ostream(error)
        << "could not access path '" << path << "': " << ec.message();
    return llvm::createStringError(ec, error.c_str());
  };
  llvm::sys::fs::file_status unitStat;
  if (std::error_code ec = llvm::sys::fs::status(filePath, unitStat)) {
    if (ec != std::errc::no_such_file_or_directory)
      return makeError(filePath, ec);
    return false;
  }

  if (timeCompareFilePath.empty())
    return true;

  llvm::sys::fs::file_status compareStat;
  if (std::error_code ec =
          llvm::sys::fs::status(timeCompareFilePath, compareStat)) {
    if (ec != std::errc::no_such_file_or_directory)
      return makeError(timeCompareFilePath, ec);
    return true;
  }

  // Return true (unit is up-to-date) if the file to compare is older than the
  // unit file.
  return compareStat.getLastModificationTime() <=
         unitStat.getLastModificationTime();
}

/// Emit the symbolic swift interface file for an imported Clang module into the
/// index store directory.
///
/// The swift interface file is emitted only when it doesn't exist yet, or when
/// the PCM for the Clang module has been updated.
///
/// System modules without the 'cplusplus' requirement are not emitted.
static void emitSymbolicInterfaceForClangModule(
    ClangModuleUnit *clangModUnit, ModuleDecl *M,
    const clang::Module *clangModule, StringRef indexStorePath,
    const clang::CompilerInstance &clangCI, DiagnosticEngine &diags) {
  if (!M->getASTContext().LangOpts.EnableCXXInterop)
    return;
  // Skip system modules without an explicit 'cplusplus' requirement.
  bool isSystem = clangModUnit->isSystemModule();
  if (isSystem && !importer::requiresCPlusPlus(clangModule))
    return;

  // Make sure the `interfaces` directory is created.
  if (auto err = initSymbolicInterfaceStorePath(indexStorePath)) {
    llvm::handleAllErrors(std::move(err), [&](const llvm::ECError &ec) {
      diags.diagnose(SourceLoc(), diag::error_create_symbolic_interfaces_dir,
                     ec.convertToErrorCode().message());
    });
    return;
  }

  // Determine the output name for the symbolic interface file.
  clang::serialization::ModuleFile *ModFile =
      clangCI.getASTReader()->getModuleManager().lookup(
          clangModule->getASTFile());
  assert(ModFile && "no module file loaded for module ?");
  SmallString<128> interfaceOutputPath = indexStorePath;
  appendSymbolicInterfaceToIndexStorePath(interfaceOutputPath);
  appendSymbolicInterfaceClangModuleFilename(ModFile->FileName,
                                             interfaceOutputPath);

  // Check if the symbolic interface file is already up to date.
  std::string error;
  auto upToDate =
      isFileUpToDateForOutputFile(interfaceOutputPath, ModFile->FileName);
  if (!upToDate) {
    llvm::handleAllErrors(
        upToDate.takeError(), [&](const llvm::StringError &ec) {
          diags.diagnose(SourceLoc(),
                         diag::error_symbolic_interfaces_failed_status_check,
                         ec.getMessage());
        });
    return;
  }
  if (M->getASTContext().LangOpts.EnableIndexingSystemModuleRemarks) {
    diags.diagnose(SourceLoc(), diag::remark_emitting_symbolic_interface_module,
                   interfaceOutputPath, *upToDate);
  }
  if (*upToDate)
    return;

  // Output the interface to a temporary file first.
  SmallString<128> tempOutputPath = interfaceOutputPath;
  tempOutputPath += "-%%%%%%%%";
  int tempFD;
  if (llvm::sys::fs::createUniqueFile(tempOutputPath.str(), tempFD,
                                      tempOutputPath)) {
    llvm::raw_string_ostream errOS(error);
    errOS << "failed to create temporary file: " << tempOutputPath;
    diags.diagnose(SourceLoc(), diag::error_write_symbolic_interface,
                   errOS.str());
    return;
  }

  llvm::raw_fd_ostream os(tempFD, /*shouldClose=*/true);
  StreamPrinter printer(os);
  ide::printSymbolicSwiftClangModuleInterface(M, printer, clangModule);
  os.close();

  if (os.has_error()) {
    llvm::raw_string_ostream errOS(error);
    errOS << "failed to write '" << tempOutputPath
          << "': " << os.error().message();
    diags.diagnose(SourceLoc(), diag::error_write_symbolic_interface,
                   errOS.str());
    os.clear_error();
    llvm::sys::fs::remove(tempOutputPath);
    return;
  }

  // Move the resulting output to the destination symbolic interface file.
  std::error_code ec = llvm::sys::fs::rename(
      /*from=*/tempOutputPath, /*to=*/interfaceOutputPath);
  if (ec) {
    llvm::raw_string_ostream errOS(error);
    errOS << "failed to rename '" << tempOutputPath << "' to '"
          << interfaceOutputPath << "': " << ec.message();
    diags.diagnose(SourceLoc(), diag::error_write_symbolic_interface,
                   errOS.str());
    llvm::sys::fs::remove(tempOutputPath);
    return;
  }
}

static void emitTransitiveClangSymbolicInterfacesForSwiftModuleImports(
    ArrayRef<ImportedModule> imports, StringRef indexStorePath,
    const clang::CompilerInstance &clangCI, DiagnosticEngine &diags) {
  auto &fileMgr = clangCI.getFileManager();
  for (auto &import : imports) {
    ModuleDecl *mod = import.importedModule;
    if (mod->isOnoneSupportModule())
      continue; // ignore the Onone support library.
    if (mod->isSwiftShimsModule())
      continue;

    for (auto *FU : mod->getFiles()) {
      switch (FU->getKind()) {
      default:
        break;
      case FileUnitKind::SerializedAST:
      case FileUnitKind::DWARFModule:
      case FileUnitKind::ClangModule: {
        auto *LFU = cast<LoadedFile>(FU);
        if (auto F = fileMgr.getFile(LFU->getFilename())) {
          if (FU->getKind() == FileUnitKind::ClangModule) {
            auto clangModUnit = cast<ClangModuleUnit>(LFU);
            if (auto clangMod = clangModUnit->getUnderlyingClangModule()) {
              // Emit the symbolic interface file in addition to index data.
              emitSymbolicInterfaceForClangModule(
                  clangModUnit, mod, clangMod, indexStorePath, clangCI, diags);
            }
          }
          // FIXME: We should keep recursing here into other Swift modules.
        }
      }
      }
    }
  }
}

static void addModuleDependencies(ArrayRef<ImportedModule> imports,
                                  StringRef indexStorePath,
                                  bool indexClangModules,
                                  bool indexSystemModules,
                                  bool skipStdlib,
                                  bool includeLocals,
                                  bool explicitModuleBuild,
                                  StringRef targetTriple,
                                  const clang::CompilerInstance &clangCI,
                                  DiagnosticEngine &diags,
                                  IndexUnitWriter &unitWriter,
                                  StringScratchSpace &moduleNameScratch,
                                  const PathRemapper &pathRemapper,
                                  SourceFile *initialFile = nullptr) {
  auto &fileMgr = clangCI.getFileManager();

  for (auto &import : imports) {
    ModuleDecl *mod = import.importedModule;
    if (mod->isOnoneSupportModule())
      continue; // ignore the Onone support library.
    if (mod->isSwiftShimsModule())
      continue;

    for (auto *FU : mod->getFiles()) {
      switch (FU->getKind()) {
      case FileUnitKind::Source:
      case FileUnitKind::Builtin:
      case FileUnitKind::Synthesized:
        break;
      case FileUnitKind::SerializedAST:
      case FileUnitKind::DWARFModule:
      case FileUnitKind::ClangModule: {
        auto *LFU = cast<LoadedFile>(FU);

        // This should really be returned from emitting index data, rather
        // than guessing which is used for the output file here.
        StringRef modulePath;
        if (FU->getKind() == FileUnitKind::ClangModule) {
          modulePath = LFU->getFilename();
        } else {
          modulePath = LFU->getSourceFilename();
        }

        auto F = fileMgr.getFile(modulePath);
        if (!F)
          break;

        // Use module real name for unit writer in case module aliasing
        // is used. For example, if a file being indexed has `import Foo`
        // and `-module-alias Foo=Bar` is passed, treat Foo as an alias
        // and Bar as the real module name as its dependency.
        StringRef moduleName = mod->getRealName().str();
        bool withoutUnitName = true;
        if (FU->getKind() == FileUnitKind::ClangModule) {
          auto clangModUnit = cast<ClangModuleUnit>(LFU);
          bool shouldIndexModule =
              indexClangModules &&
              (!mod->isNonUserModule() || indexSystemModules);
          withoutUnitName = !shouldIndexModule;
          if (auto clangMod = clangModUnit->getUnderlyingClangModule()) {
            moduleName = clangMod->getTopLevelModuleName();
            // FIXME: clang's -Rremarks do not seem to go through Swift's
            // diagnostic emitter.
            if (shouldIndexModule)
              clang::index::emitIndexDataForModuleFile(clangMod,
                                                       clangCI, unitWriter);
            // Emit the symbolic interface file in addition to index data.
            if (indexClangModules)
              emitSymbolicInterfaceForClangModule(clangModUnit, mod, clangMod,
                                                  indexStorePath, clangCI,
                                                  diags);
          }
        } else {
          // Serialized AST file.
          // Only index distributed system modules, and the stdlib.
          // We don't officially support binary swift modules, so normally
          // the index data for user modules would get generated while
          // building them.
          if (mod->isNonUserModule() && indexSystemModules &&
              (!skipStdlib || !mod->isStdlibModule())) {
            emitDataForSwiftSerializedModule(mod, indexStorePath,
                                             indexClangModules,
                                             indexSystemModules, skipStdlib,
                                             includeLocals,
                                             explicitModuleBuild,
                                             targetTriple,
                                             clangCI, diags,
                                             unitWriter,
                                             pathRemapper,
                                             initialFile);
            withoutUnitName = false;
          }

          // If this is a cross-import overlay, make sure we use the name of
          // the underlying module instead.
          if (auto *declaring = mod->getDeclaringModuleIfCrossImportOverlay())
            moduleName = declaring->getNameStr();


          // Emit symbolic interface files for any re-exported Clang modules
          // from this Swift module.
          if (mod->getASTContext().LangOpts.EnableCXXInterop) {
            SmallVector<ImportedModule, 4> imports;
            mod->getImportedModules(imports,
                                    ModuleDecl::ImportFilterKind::Exported);
            if (indexClangModules)
              emitTransitiveClangSymbolicInterfacesForSwiftModuleImports(
                  imports, indexStorePath, clangCI, diags);
          }
        }
        clang::index::writer::OpaqueModule opaqMod =
            moduleNameScratch.createString(moduleName);
        unitWriter.addASTFileDependency(*F, mod->isNonUserModule(), opaqMod,
                                        withoutUnitName);

        break;
      }
      }
    }
  }
}

/// \returns true if an error occurred.
static bool
emitDataForSwiftSerializedModule(ModuleDecl *module,
                                 StringRef indexStorePath,
                                 bool indexClangModules,
                                 bool indexSystemModules,
                                 bool skipStdlib,
                                 bool includeLocals,
                                 bool explicitModuleBuild,
                                 StringRef targetTriple,
                                 const clang::CompilerInstance &clangCI,
                                 DiagnosticEngine &diags,
                                 IndexUnitWriter &parentUnitWriter,
                                 const PathRemapper &pathRemapper,
                                 SourceFile *initialFile) {
  StringRef filename = module->getModuleSourceFilename();
  std::string moduleName = module->getNameStr().str();

  // If this is a cross-import overlay, make sure we use the name of the
  // underlying module instead.
  if (ModuleDecl *declaring = module->getDeclaringModuleIfCrossImportOverlay())
    moduleName = declaring->getNameStr().str();

  std::string error;
  auto isUptodateOpt = parentUnitWriter.isUnitUpToDateForOutputFile(
      /*FilePath=*/filename,
      /*TimeCompareFilePath=*/filename, error);
  if (!isUptodateOpt.has_value()) {
    diags.diagnose(SourceLoc(), diag::error_index_failed_status_check, error);
    return true;
  }
  if (*isUptodateOpt)
    return false;

  // Reload resilient modules from swiftinterface to avoid indexing
  // internal details.
  bool skipIndexingModule = false;
  // Note, we are unable to reload from interface on an explicit module build
  if (!explicitModuleBuild &&
      module->getResilienceStrategy() == ResilienceStrategy::Resilient &&
      !module->isBuiltFromInterface() &&
      !module->isStdlibModule()) {
    auto &ctx = module->getASTContext();
    llvm::SaveAndRestore<bool> S(ctx.IgnoreAdjacentModules, true);

    ImportPath::Module::Builder builder(module->getName());
    auto reloadedModule = ctx.getModule(builder.get(),
                                        /*AllowMemoryCached=*/false);

    if (reloadedModule) {
      module = reloadedModule;
    } else {
      // If we can't rebuild from the swiftinterface, don't index this module.
      skipIndexingModule = true;
    }
  }

  if (module->getASTContext().LangOpts.EnableIndexingSystemModuleRemarks) {
    diags.diagnose(SourceLoc(),
                   diag::remark_indexing_system_module,
                   filename, skipIndexingModule);
  }

  // Pairs of (recordFile, groupName).
  std::vector<std::pair<std::string, std::string>> records;

  if (skipIndexingModule) {
    // Don't add anything to records but keep going so we still mark the module
    // as indexed to avoid rebuilds of broken swiftinterfaces.
  } else if (!module->isStdlibModule()) {
    std::string recordFile;
    bool failed = false;
    auto consumer = makeRecordingConsumer(filename.str(), indexStorePath.str(),
                                          includeLocals, &diags, &recordFile, &failed);
    indexModule(module, *consumer);

    if (failed)
      return true;

    records.emplace_back(recordFile, moduleName);
  } else {
    // Record stdlib groups as if they were submodules.

    auto makeSubmoduleNameFromGroupName = [](StringRef groupName, SmallString<128> &buf) {
      buf += "Swift";
      if (groupName.empty())
        return;
      buf += '.';
      for (char ch : groupName) {
        if (ch == '/')
          buf += '.';
        else
          buf += ch;
      }
    };
    auto appendGroupNameForFilename = [](StringRef groupName, SmallString<256> &buf) {
      if (groupName.empty())
        return;
      buf += '_';
      for (char ch : groupName) {
        if (ch == '/' || ch ==' ')
          buf += '_';
        else
          buf += ch;
      }
    };

    bool failed = false;
    StdlibGroupsIndexRecordingConsumer groupIndexConsumer([&](StringRef groupName, SymbolTracker &tracker) -> bool {
      SmallString<128> moduleName;
      makeSubmoduleNameFromGroupName(groupName, moduleName);
      SmallString<256> fileNameWithGroup = filename;
      appendGroupNameForFilename(groupName, fileNameWithGroup);

      std::string outRecordFile;
      failed =
          failed || writeRecord(tracker, std::string(fileNameWithGroup.str()),
                                indexStorePath.str(), &diags, outRecordFile);
      if (failed)
        return false;
      records.emplace_back(outRecordFile, moduleName.str().str());
      return true;
    });
    indexModule(module, groupIndexConsumer);
    if (failed)
      return true;
  }

  auto &fileMgr = clangCI.getFileManager();
  bool isSystem = module->isNonUserModule();
  // FIXME: Get real values for the following.
  StringRef swiftVersion;
  StringRef sysrootPath = clangCI.getHeaderSearchOpts().Sysroot;
  // For indexing serialized modules 'debug compilation' is irrelevant, so
  // set it to true by default.
  bool isDebugCompilation = true;
  auto clangRemapper = pathRemapper.asClangPathRemapper();

  IndexUnitWriter unitWriter(
      fileMgr, indexStorePath, "swift", swiftVersion, filename, moduleName,
      /*MainFile=*/nullptr, isSystem, /*IsModuleUnit=*/true, isDebugCompilation,
      targetTriple, sysrootPath, clangRemapper, getModuleInfoFromOpaqueModule);

  auto FE = fileMgr.getFile(filename);
  for (auto &pair : records) {
    std::string &recordFile = pair.first;
    std::string &groupName = pair.second;
    if (recordFile.empty())
      continue;
    clang::index::writer::OpaqueModule mod = &groupName;
    unitWriter.addRecordFile(recordFile, *FE, isSystem, mod);
  }

  SmallVector<ImportedModule, 8> imports;
  module->getImportedModules(imports, {ModuleDecl::ImportFilterKind::Exported,
                                       ModuleDecl::ImportFilterKind::Default});
  StringScratchSpace moduleNameScratch;
  addModuleDependencies(imports, indexStorePath, indexClangModules,
                        indexSystemModules, skipStdlib, includeLocals,
                        explicitModuleBuild,
                        targetTriple, clangCI, diags, unitWriter,
                        moduleNameScratch, pathRemapper, initialFile);

  if (unitWriter.write(error)) {
    diags.diagnose(SourceLoc(), diag::error_write_index_unit, error);
    return true;
  }

  return false;
}

static bool
recordSourceFileUnit(SourceFile *primarySourceFile, StringRef indexUnitToken,
                     StringRef indexStorePath, bool indexClangModules,
                     bool indexSystemModules, bool skipStdlib,
                     bool includeLocals, bool isDebugCompilation,
                     bool isExplicitModuleBuild, StringRef targetTriple,
                     ArrayRef<const clang::FileEntry *> fileDependencies,
                     const clang::CompilerInstance &clangCI,
                     const PathRemapper &pathRemapper,
                     DiagnosticEngine &diags) {
  auto &fileMgr = clangCI.getFileManager();
  auto *module = primarySourceFile->getParentModule();
  bool isSystem = module->isNonUserModule();
  auto mainFile = fileMgr.getFile(primarySourceFile->getFilename());
  auto clangRemapper = pathRemapper.asClangPathRemapper();
  // FIXME: Get real values for the following.
  StringRef swiftVersion;
  StringRef sysrootPath = clangCI.getHeaderSearchOpts().Sysroot;
  IndexUnitWriter unitWriter(
      fileMgr, indexStorePath, "swift", swiftVersion, indexUnitToken,
      module->getNameStr(), mainFile ? *mainFile : nullptr, isSystem,
      /*isModuleUnit=*/false, isDebugCompilation, targetTriple, sysrootPath,
      clangRemapper, getModuleInfoFromOpaqueModule);

  // Module dependencies.
  SmallVector<ImportedModule, 8> imports;
  primarySourceFile->getImportedModules(imports,
                                        ModuleDecl::getImportFilterLocal());
  StringScratchSpace moduleNameScratch;
  addModuleDependencies(imports, indexStorePath, indexClangModules,
                        indexSystemModules, skipStdlib, includeLocals,
                        isExplicitModuleBuild, targetTriple, clangCI, diags,
                        unitWriter, moduleNameScratch, pathRemapper,
                        primarySourceFile);

  // File dependencies.
  for (auto *F : fileDependencies)
    unitWriter.addFileDependency(F, /*FIXME:isSystem=*/false, /*Module=*/nullptr);

  recordSourceFile(primarySourceFile, indexStorePath, includeLocals, diags,
                   [&](StringRef recordFile, StringRef filename) {
                     auto file = fileMgr.getFile(filename);
                     unitWriter.addRecordFile(
                         recordFile, file ? *file : nullptr,
                         module->isNonUserModule(), /*Module=*/nullptr);
                   });

  std::string error;
  if (unitWriter.write(error)) {
    diags.diagnose(SourceLoc(), diag::error_write_index_unit, error);
    return true;
  }

  return false;
}

// Not currently used, see related comments in the call sites.
#if 0
static void
collectFileDependencies(llvm::SetVector<const clang::FileEntry *> &result,
                        const DependencyTracker &dependencyTracker,
                        ModuleDecl *module, clang::FileManager &fileMgr) {
  for (auto *F : module->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(F)) {
      if (auto *dep = fileMgr.getFile(SF->getFilename())) {
        result.insert(dep);
      }
    }
  }
  for (StringRef filename : dependencyTracker.getDependencies()) {
    if (auto *F = fileMgr.getFile(filename))
      result.insert(F);
  }
}
#endif

//===----------------------------------------------------------------------===//
// Indexing entry points
//===----------------------------------------------------------------------===//

bool index::indexAndRecord(SourceFile *primarySourceFile,
                           StringRef indexUnitToken,
                           StringRef indexStorePath,
                           bool indexClangModules,
                           bool indexSystemModules,
                           bool skipStdlib,
                           bool includeLocals,
                           bool isDebugCompilation,
                           bool isExplicitModuleBuild,
                           StringRef targetTriple,
                           const DependencyTracker &dependencyTracker,
                           const PathRemapper &pathRemapper) {
  auto &astContext = primarySourceFile->getASTContext();
  auto &clangCI = astContext.getClangModuleLoader()->getClangInstance();
  auto &diags = astContext.Diags;

  std::string error;
  if (IndexUnitWriter::initIndexDirectory(indexStorePath, error)) {
    diags.diagnose(SourceLoc(), diag::error_create_index_dir, error);
    return true;
  }

  llvm::SetVector<const clang::FileEntry *> fileDependencies;
  // FIXME: This is not desirable because:
  // 1. It picks shim header files as file dependencies
  // 2. Having all the other swift files of the module as file dependencies ends
  //   up making all of them associated with all the other files as main files.
  //   It's better to associate each swift file with the unit that recorded it
  //   as the main one.
  // Keeping the code in case we want to revisit.
#if 0
  auto *module = primarySourceFile->getParentModule();
  collectFileDependencies(fileDependencies, dependencyTracker, module, fileMgr);
#endif

  return recordSourceFileUnit(primarySourceFile, indexUnitToken,
                              indexStorePath, indexClangModules,
                              indexSystemModules, skipStdlib, includeLocals,
                              isDebugCompilation, isExplicitModuleBuild,
                              targetTriple, fileDependencies.getArrayRef(),
                              clangCI, pathRemapper, diags);
}

bool index::indexAndRecord(ModuleDecl *module,
                           ArrayRef<std::string> indexUnitTokens,
                           StringRef moduleUnitToken,
                           StringRef indexStorePath,
                           bool indexClangModules,
                           bool indexSystemModules,
                           bool skipStdlib,
                           bool includeLocals,
                           bool isDebugCompilation,
                           bool isExplicitModuleBuild,
                           StringRef targetTriple,
                           const DependencyTracker &dependencyTracker,
                           const PathRemapper &pathRemapper) {
  auto &astContext = module->getASTContext();
  auto &clangCI = astContext.getClangModuleLoader()->getClangInstance();
  auto &diags = astContext.Diags;

  std::string error;
  if (IndexUnitWriter::initIndexDirectory(indexStorePath, error)) {
    diags.diagnose(SourceLoc(), diag::error_create_index_dir, error);
    return true;
  }

  // Add the current module's source files to the dependencies.
  llvm::SetVector<const clang::FileEntry *> fileDependencies;
  // FIXME: This is not desirable because:
  // 1. It picks shim header files as file dependencies
  // 2. Having all the other swift files of the module as file dependencies ends
  //   up making all of them associated with all the other files as main files.
  //   It's better to associate each swift file with the unit that recorded it
  //   as the main one.
  // Keeping the code in case we want to revisit.
#if 0
  collectFileDependencies(fileDependencies, dependencyTracker, module, fileMgr);
#endif

  // Write a unit for each source file.
  unsigned unitIndex = 0;
  for (auto *F : module->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(F)) {
      if (unitIndex == indexUnitTokens.size()) {
        diags.diagnose(SourceLoc(), diag::error_index_inputs_more_than_outputs);
        return true;
      }
      if (recordSourceFileUnit(SF, indexUnitTokens[unitIndex],
                               indexStorePath, indexClangModules,
                               indexSystemModules, skipStdlib, includeLocals,
                               isDebugCompilation, isExplicitModuleBuild,
                               targetTriple, fileDependencies.getArrayRef(),
                               clangCI, pathRemapper, diags))
        return true;
      unitIndex += 1;
    }
  }

  // In the case where inputs are swift modules, like in the merge-module step,
  // ignore the inputs; associated unit files for the modules' source inputs
  // should have been generated at swift module creation time.

  return false;
}
