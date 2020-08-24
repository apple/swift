//===--- LTO.cpp - Swift LTO ----------------------------------------------===//
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

#ifndef SWIFT_LTO_H
#define SWIFT_LTO_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/MemoryBuffer.h"
#include <functional>
#include <memory>

#include "swift/AST/ASTContext.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/ClangImporter/ClangImporterOptions.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Serialization/Validation.h"

namespace swift {

class ASTContext;

namespace lto {

using GetStreamFn = std::function<std::unique_ptr<llvm::raw_ostream>(
    llvm::StringRef ModuleName)>;

class LTOPipeline {
  llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryPaths;
  llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryImportPaths;
  llvm::StringRef RuntimeResourcePath;
  llvm::SmallVector<Identifier, 2> ModuleNames;
  LangOptions LangOpts;
  ClangImporterOptions ClangOpts;
  TypeCheckerOptions TCOpts;
  SearchPathOptions SearchPathOpts;
  SourceManager SM;
  DiagnosticEngine Diags;
  PrintingDiagnosticConsumer PrintDiags;
  std::unique_ptr<ASTContext> Ctx;
  MemoryBufferSerializedModuleLoader *MBL;

public:
  LTOPipeline(llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryPaths,
              llvm::SmallVector<llvm::StringRef, 4> RuntimeLibraryImportPaths,
              llvm::StringRef RuntimeResourcePath)
      : RuntimeLibraryPaths(RuntimeLibraryPaths),
        RuntimeLibraryImportPaths(RuntimeLibraryImportPaths),
        RuntimeResourcePath(RuntimeResourcePath), Diags(SM) {}
  bool addModule(std::unique_ptr<llvm::MemoryBuffer> Buffer);
  bool emitLLVMModules(GetStreamFn GetStream);

private:
  ASTContext *createASTContext(serialization::ValidationInfo info,
                               serialization::ExtendedValidationInfo extInfo);
};

} // namespace lto
} // namespace swift

#endif // SWIFT_LTO_H
