//===--- Migrator.cpp -----------------------------------------------------===//
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

#include "swift/Frontend/Frontend.h"
#include "swift/Migrator/EditorAdapter.h"
#include "swift/Migrator/FixitApplyDiagnosticConsumer.h"
#include "swift/Migrator/Migrator.h"
#include "swift/Migrator/RewriteBufferEditsReceiver.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "llvm/Support/FileSystem.h"
#include "swift/IDE/APIDigesterData.h"

using namespace swift;
using namespace swift::migrator;
using namespace swift::ide::api;

bool migrator::updateCodeAndEmitRemap(const CompilerInvocation &Invocation) {

  Migrator M { Invocation }; // Provide inputs and configuration

  // Phase 1:
  // Perform any syntactic transformations if requested.

  // Prepare diff item store to use.
  APIDiffItemStore DiffStore;
  if (!M.getMigratorOptions().APIDigesterDataStorePath.empty()) {
    DiffStore.addStorePath(M.getMigratorOptions().APIDigesterDataStorePath);
  }

  auto FailedSyntacticPasses = M.performSyntacticPasses();
  if (FailedSyntacticPasses) {
    return true;
  }

  // Phase 2:
  // Perform fix-it based migrations on the compiler, some number of times in
  // order to give the compiler an opportunity to
  // take its time reaching a fixed point.

  if (M.getMigratorOptions().EnableMigratorFixits) {
    M.repeatFixitMigrations(Migrator::MaxCompilerFixitPassIterations);
  }

  // OK, we have a final resulting text. Now we compare against the input
  // to calculate a replacement map describing the changes to the input
  // necessary to get the output.
  // TODO: Document replacement map format.

  auto EmitRemapFailed = M.emitRemap();
  auto EmitMigratedFailed = M.emitMigratedFile();
  auto DumpMigrationStatesFailed = M.dumpStates();
  return EmitRemapFailed || EmitMigratedFailed || DumpMigrationStatesFailed;
}

Migrator::Migrator(const CompilerInvocation &StartInvocation)
  : StartInvocation(StartInvocation) {

    auto ErrorOrStartBuffer = llvm::MemoryBuffer::getFile(getInputFilename());
    auto &StartBuffer = ErrorOrStartBuffer.get();
    auto StartBufferID = SrcMgr.addNewSourceBuffer(std::move(StartBuffer));
    States.push_back(MigrationState::start(SrcMgr, StartBufferID));
}

void Migrator::
repeatFixitMigrations(const unsigned Iterations) {
  for (unsigned i = 0; i < Iterations; ++i) {
    auto ThisResult = performAFixItMigration();
    if (!ThisResult.hasValue()) {
      // Something went wrong? Track error in the state?
      break;
    } else {
      if (ThisResult.getValue()->outputDiffersFromInput()) {
        States.push_back(ThisResult.getValue());
      } else {
        break;
      }
    }
  }
}

llvm::Optional<RC<MigrationState>>
Migrator::performAFixItMigration() {

  auto InputState = States.back();
  auto InputBuffer =
    llvm::MemoryBuffer::getMemBufferCopy(InputState->getOutputText(),
                                     getInputFilename());

  CompilerInvocation Invocation { StartInvocation };
  Invocation.clearInputs();
  Invocation.addInputBuffer(InputBuffer.get());

  CompilerInstance Instance;
  if (Instance.setup(Invocation)) {
    // TODO: Return a state with an error attached?
    return None;
  }

  FixitApplyDiagnosticConsumer FixitApplyConsumer {
    getMigratorOptions(),
    InputState->getInputText(),
    getInputFilename(),
  };
  Instance.addDiagnosticConsumer(&FixitApplyConsumer);

  Instance.performSema();

  StringRef ResultText = InputState->getInputText();
  unsigned ResultBufferID = InputState->getInputBufferID();

  if (FixitApplyConsumer.getNumFixitsApplied() > 0) {
    SmallString<4096> Scratch;
    llvm::raw_svector_ostream OS(Scratch);
    FixitApplyConsumer.printResult(OS);
    auto ResultBuffer = llvm::MemoryBuffer::getMemBufferCopy(OS.str());
    ResultText = ResultBuffer->getBuffer();
    ResultBufferID = SrcMgr.addNewSourceBuffer(std::move(ResultBuffer));
  }

  return MigrationState::make(MigrationKind::CompilerFixits,
                              SrcMgr, InputState->getInputBufferID(),
                              ResultBufferID);
}

bool Migrator::performSyntacticPasses() {
  clang::FileSystemOptions ClangFileSystemOptions;
  clang::FileManager ClangFileManager { ClangFileSystemOptions };

  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DummyClangDiagIDs {
    new clang::DiagnosticIDs()
  };
  auto ClangDiags =
    llvm::make_unique<clang::DiagnosticsEngine>(DummyClangDiagIDs,
                                                new clang::DiagnosticOptions,
                                                new clang::DiagnosticConsumer(),
                                                /*ShouldOwnClient=*/true);

  clang::SourceManager ClangSourceManager { *ClangDiags, ClangFileManager };
  clang::LangOptions ClangLangOpts;
  clang::edit::EditedSource Edits { ClangSourceManager, ClangLangOpts };

  // Make a CompilerInstance
  // Perform Sema
  // auto SF = CI.getPrimarySourceFile() ?

  CompilerInvocation Invocation { StartInvocation };

  auto InputState = States.back();
  auto TempInputBuffer =
    llvm::MemoryBuffer::getMemBufferCopy(InputState->getOutputText(),
                                         getInputFilename());
  Invocation.clearInputs();
  Invocation.addInputBuffer(TempInputBuffer.get());

  CompilerInstance Instance;
  if (Instance.setup(StartInvocation)) {
    return true;
  }

  Instance.performSema();
  if (Instance.getDiags().hasFatalErrorOccurred()) {
    return true;
  }

  EditorAdapter Editor { Instance.getSourceMgr(), ClangSourceManager };

  // const auto SF = Instance.getPrimarySourceFile();

  // From here, create the syntactic pass:
  //
  // SyntacticMigratorPass MyPass {
  //   Editor, Sema's SourceMgr, ClangSrcManager, SF
  // };
  // MyPass.run();
  //
  // Once it has run, push the edits into Edits above:
  // Edits.commit(YourPass.getEdits());

  // Now, we'll take all of the changes we've accumulated, get a resulting text,
  // and push a MigrationState.
  auto InputText = States.back()->getOutputText();

  RewriteBufferEditsReceiver Rewriter {
    ClangSourceManager,
    Editor.getClangFileIDForSwiftBufferID(
      Instance.getPrimarySourceFile()->getBufferID().getValue()),
    InputState->getOutputText()
  };
    
  Edits.applyRewrites(Rewriter);

  SmallString<1024> Scratch;
  llvm::raw_svector_ostream OS(Scratch);
  Rewriter.printResult(OS);
  auto ResultBuffer = this->SrcMgr.addMemBufferCopy(OS.str());

  States.push_back(
    MigrationState::make(MigrationKind::Syntactic,
                         this->SrcMgr,
                         States.back()->getInputBufferID(),
                         ResultBuffer));
  return false;
}

bool Migrator::emitRemap() const {
  // TODO: Need to integrate diffing library to diff start and end state's
  // output text.
  return false;
}

bool Migrator::emitMigratedFile() const {
  const auto &OutFilename = getMigratorOptions().EmitMigratedFilePath;
  if (OutFilename.empty()) {
    return false;
  }

  std::error_code Error;
  llvm::raw_fd_ostream FileOS(OutFilename,
                              Error, llvm::sys::fs::F_Text);
  if (FileOS.has_error()) {
    return true;
  }

  FileOS << States.back()->getOutputText();

  FileOS.flush();

  return FileOS.has_error();
}

bool Migrator::dumpStates() const {
  const auto &OutDir = getMigratorOptions().DumpMigrationStatesDir;
  if (OutDir.empty()) {
    return false;
  }

  auto Failed = false;
  for (size_t i = 0; i < States.size(); ++i) {
    Failed |= States[i]->print(i, OutDir);
  }

  return Failed;
}

const MigratorOptions &Migrator::getMigratorOptions() const {
  return StartInvocation.getMigratorOptions();
}

const StringRef Migrator::getInputFilename() const {
  auto PrimaryInput =
    StartInvocation.getFrontendOptions().PrimaryInput.getValue();
  return StartInvocation.getInputFilenames()[PrimaryInput.Index];
}
