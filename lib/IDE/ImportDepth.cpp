//===--- ImportDepth.cpp --------------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDE/ImportDepth.h"
#include "swift/AST/Module.h"
#include "swift/Basic/Assertions.h"
#include "clang/Basic/Module.h"

using namespace swift;
using namespace swift::ide;

ImportDepth::ImportDepth(ASTContext &context,
                         const FrontendOptions &frontendOptions) {
  llvm::DenseSet<ModuleDecl *> seen;
  std::deque<std::pair<ModuleDecl *, uint8_t>> worklist;

  StringRef mainModule = frontendOptions.ModuleName;
  auto *main = context.getLoadedModule(context.getIdentifier(mainModule));
  assert(main && "missing main module");
  worklist.emplace_back(main, uint8_t(0));

  // Imports from -import-name such as Playground auxiliary sources are treated
  // specially by applying import depth 0.
  llvm::StringSet<> auxImports;
  for (const auto &pair : frontendOptions.getImplicitImportModuleNames())
    auxImports.insert(pair.first);

  // Private imports from this module.
  SmallVector<ImportedModule, 16> mainImports;
  main->getImportedModules(mainImports, ModuleDecl::getImportFilterLocal());
  for (auto &import : mainImports) {
    uint8_t depth = 1;
    if (auxImports.count(import.importedModule->getName().str()))
      depth = 0;
    worklist.emplace_back(import.importedModule, depth);
  }

  // Fill depths with BFS over module imports.
  while (!worklist.empty()) {
    ModuleDecl *module;
    uint8_t depth;
    std::tie(module, depth) = worklist.front();
    worklist.pop_front();

    if (!seen.insert(module).second)
      continue;

    // Insert new module:depth mapping.
    const clang::Module *CM = module->findUnderlyingClangModule();
    if (CM) {
      depths[CM->getFullModuleName()] = depth;
    } else {
      depths[module->getName().str()] = depth;
    }

    // Add imports to the worklist.
    SmallVector<ImportedModule, 16> imports;
    module->getImportedModules(imports);
    for (auto &import : imports) {
      uint8_t next = std::max(depth, uint8_t(depth + 1)); // unsigned wrap

      // Implicitly imported sub-modules get the same depth as their parent.
      if (const clang::Module *CMI =
              import.importedModule->findUnderlyingClangModule())
        if (CM && CMI->isSubModuleOf(CM))
          next = depth;
      worklist.emplace_back(import.importedModule, next);
    }
  }
}
