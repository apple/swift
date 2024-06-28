//===--- ImportedModules.cpp -- generates the list of imported modules ----===//
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

#include "Dependencies.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Module.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Frontend/FrontendOptions.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/VirtualOutputBackend.h"

using namespace swift;

static StringRef getTopLevelName(const clang::Module *module) {
  return module->getTopLevelModule()->Name;
}

static void findAllClangImports(const clang::Module *module,
                                llvm::SetVector<StringRef> &modules) {
  for (auto imported : module->Imports) {
    modules.insert(getTopLevelName(imported));
  }

  for (auto sub : module->submodules()) {
    findAllClangImports(sub, modules);
  }
}

bool swift::emitImportedModules(ModuleDecl *mainModule,
                                const FrontendOptions &opts,
                                llvm::vfs::OutputBackend &backend) {
  auto &Context = mainModule->getASTContext();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  auto &diags = Context.Diags;
  auto out = backend.createFile(path);
  if (!out) {
    diags.diagnose(SourceLoc(), diag::error_opening_output,
                   path, toString(out.takeError()));
    return true;
  }

  llvm::SetVector<StringRef> Modules;

  // Find the imports in the main Swift code.
  // We don't need `getTopLevelDeclsForDisplay()` here because we only care
  // about `ImportDecl`s.
  llvm::SmallVector<Decl *, 32> Decls;
  mainModule->getDisplayDecls(Decls);
  for (auto D : Decls) {
    auto ID = dyn_cast<ImportDecl>(D);
    if (!ID)
      continue;

    ImportPath::Builder scratch;
    auto modulePath = ID->getRealModulePath(scratch);
    // only the top-level name is needed (i.e. A in A.B.C)
    Modules.insert(modulePath[0].Item.str());
  }

  // And now look in the C code we're possibly using.
  auto clangImporter =
      static_cast<ClangImporter *>(Context.getClangModuleLoader());

  StringRef implicitHeaderPath = opts.ImplicitObjCHeaderPath;
  if (!implicitHeaderPath.empty()) {
    if (!clangImporter->importBridgingHeader(implicitHeaderPath, mainModule)) {
      SmallVector<ImportedModule, 16> imported;
      clangImporter->getImportedHeaderModule()->getImportedModules(
          imported, ModuleDecl::getImportFilterLocal());

      for (auto IM : imported) {
        if (auto clangModule = IM.importedModule->findUnderlyingClangModule())
          Modules.insert(getTopLevelName(clangModule));
        else
          assert(IM.importedModule->isStdlibModule() &&
                 "unexpected non-stdlib swift module");
      }
    }
  }

  if (opts.ImportUnderlyingModule) {
    auto underlyingModule = clangImporter->loadModule(SourceLoc(),
      ImportPath::Module::Builder(mainModule->getName()).get());
    if (!underlyingModule) {
      Context.Diags.diagnose(SourceLoc(),
                             diag::error_underlying_module_not_found,
                             mainModule->getName());
      return true;
    }
    auto clangModule = underlyingModule->findUnderlyingClangModule();

    findAllClangImports(clangModule, Modules);
  }

  for (auto name : Modules) {
    *out << name << "\n";
  }

  if (auto error = out->keep()) {
    diags.diagnose(SourceLoc(), diag::error_closing_output,
                   path, toString(std::move(error)));
    return true;
  }

  return false;
}
