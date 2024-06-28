//===--- Migrator.cpp -----------------------------------------------------===//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "Diff.h"
#include "swift/Basic/Assertions.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Migrator/ASTMigratorPass.h"
#include "swift/Migrator/EditorAdapter.h"
#include "swift/Migrator/FixitApplyDiagnosticConsumer.h"
#include "swift/Migrator/Migrator.h"
#include "swift/Migrator/RewriteBufferEditsReceiver.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/FileManager.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Edit/EditedSource.h"
#include "clang/Rewrite/Core/RewriteBuffer.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"

using namespace swift;
using namespace swift::migrator;

bool migrator::updateCodeAndEmitRemapIfNeeded(CompilerInstance *Instance) {
  const auto &Invocation = Instance->getInvocation();
  if (!Invocation.getMigratorOptions().shouldRunMigrator())
    return false;

  // Delete the remap file, in case someone is re-running the Migrator. If the
  // file fails to compile and we don't get a chance to overwrite it, the old
  // changes may get picked up.
  llvm::sys::fs::remove(Invocation.getMigratorOptions().EmitRemapFilePath);

  Migrator M { Instance, Invocation }; // Provide inputs and configuration
  auto EffectiveVersion = Invocation.getLangOptions().EffectiveLanguageVersion;
  auto CurrentVersion = version::Version::getCurrentLanguageVersion();

  // Phase 1: Pre Fix-it passes
  // These uses the initial frontend invocation to apply any obvious fix-its
  // to see if we can get an error-free AST to get to Phase 2.
  std::unique_ptr<swift::CompilerInstance> PreFixItInstance;
  if (Instance->getASTContext().hadError()) {
    PreFixItInstance = M.repeatFixitMigrations(2, EffectiveVersion);

    // If we still couldn't fix all of the errors, give up.
    if (PreFixItInstance == nullptr ||
        !PreFixItInstance->hasASTContext() ||
        PreFixItInstance->getASTContext().hadError()) {
      return true;
    }
    M.StartInstance = PreFixItInstance.get();
  }

  // Phase 2: Syntactic Transformations
  // Don't run these passes if we're already in newest Swift version.
  if (EffectiveVersion != CurrentVersion) {
    SyntacticPassOptions Opts;

    // Type of optional try changes since Swift 5.
    Opts.RunOptionalTryMigration = !EffectiveVersion.isVersionAtLeast(5);
    auto FailedSyntacticPasses = M.performSyntacticPasses(Opts);
    if (FailedSyntacticPasses) {
      return true;
    }
  }

  // Phase 3: Post Fix-it Passes
  // Perform fix-it based migrations on the compiler, some number of times in
  // order to give the compiler an opportunity to
  // take its time reaching a fixed point.
  // This is the end of the pipeline, so we throw away the compiler instance(s)
  // we used in these fix-it runs.

  if (M.getMigratorOptions().EnableMigratorFixits) {
    M.repeatFixitMigrations(Migrator::MaxCompilerFixitPassIterations,
                            CurrentVersion);
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

Migrator::Migrator(CompilerInstance *StartInstance,
                   const CompilerInvocation &StartInvocation)
  : StartInstance(StartInstance), StartInvocation(StartInvocation) {

    auto ErrorOrStartBuffer = llvm::MemoryBuffer::getFile(getInputFilename());
    auto &StartBuffer = ErrorOrStartBuffer.get();
    auto StartBufferID = SrcMgr.addNewSourceBuffer(std::move(StartBuffer));
    States.push_back(MigrationState::start(SrcMgr, StartBufferID));
}

std::unique_ptr<swift::CompilerInstance>
Migrator::repeatFixitMigrations(const unsigned Iterations,
                                version::Version SwiftLanguageVersion) {
  for (unsigned i = 0; i < Iterations; ++i) {
    auto ThisInstance = performAFixItMigration(SwiftLanguageVersion);
    if (ThisInstance == nullptr) {
      break;
    } else {
      if (States.back()->noChangesOccurred()) {
        return ThisInstance;
      }
    }
  }
  return nullptr;
}

std::unique_ptr<swift::CompilerInstance>
Migrator::performAFixItMigration(version::Version SwiftLanguageVersion) {
  auto InputState = States.back();
  auto InputText = InputState->getOutputText();
  auto InputBuffer =
    llvm::MemoryBuffer::getMemBufferCopy(InputText, getInputFilename());

  CompilerInvocation Invocation { StartInvocation };
  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();
  Invocation.getLangOptions().EffectiveLanguageVersion = SwiftLanguageVersion;
  auto &LLVMArgs = Invocation.getFrontendOptions().LLVMArgs;
  auto aarch64_use_tbi = std::find(LLVMArgs.begin(), LLVMArgs.end(),
                                   "-aarch64-use-tbi");
  if (aarch64_use_tbi != LLVMArgs.end()) {
    LLVMArgs.erase(aarch64_use_tbi);
  }

  const auto &OrigFrontendOpts = StartInvocation.getFrontendOptions();

  assert(OrigFrontendOpts.InputsAndOutputs.hasPrimaryInputs() &&
         "Migration must have a primary");
  for (const auto &input : OrigFrontendOpts.InputsAndOutputs.getAllInputs()) {
    Invocation.getFrontendOptions().InputsAndOutputs.addInput(
        InputFile(input.getFileName(), input.isPrimary(),
                  input.isPrimary() ? InputBuffer.get() : input.getBuffer(),
                  input.getType()));
  }

  auto Instance = std::make_unique<swift::CompilerInstance>();
  // rdar://78576743 - Reset LLVM global state for command-line arguments set
  // by prior calls to setup.
  llvm::cl::ResetAllOptionOccurrences();
  std::string InstanceSetupError;
  if (Instance->setup(Invocation, InstanceSetupError)) {
    return nullptr;
  }

  FixitApplyDiagnosticConsumer FixitApplyConsumer {
    InputText,
    getInputFilename(),
  };
  Instance->addDiagnosticConsumer(&FixitApplyConsumer);

  Instance->performSema();

  StringRef ResultText = InputText;
  unsigned ResultBufferID = InputState->getOutputBufferID();

  if (FixitApplyConsumer.getNumFixitsApplied() > 0) {
    SmallString<4096> Scratch;
    llvm::raw_svector_ostream OS(Scratch);
    FixitApplyConsumer.printResult(OS);
    auto ResultBuffer = llvm::MemoryBuffer::getMemBufferCopy(OS.str());
    ResultText = ResultBuffer->getBuffer();
    ResultBufferID = SrcMgr.addNewSourceBuffer(std::move(ResultBuffer));
  }

  States.push_back(MigrationState::make(MigrationKind::CompilerFixits,
                                        SrcMgr, InputState->getOutputBufferID(),
                                        ResultBufferID));
  return Instance;
}

bool Migrator::performSyntacticPasses(SyntacticPassOptions Opts) {
  clang::FileSystemOptions ClangFileSystemOptions;
  clang::FileManager ClangFileManager { ClangFileSystemOptions };

  llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs> DummyClangDiagIDs {
    new clang::DiagnosticIDs()
  };
  auto ClangDiags =
    std::make_unique<clang::DiagnosticsEngine>(DummyClangDiagIDs,
                                                new clang::DiagnosticOptions,
                                                new clang::DiagnosticConsumer(),
                                                /*ShouldOwnClient=*/true);

  clang::SourceManager ClangSourceManager { *ClangDiags, ClangFileManager };
  clang::LangOptions ClangLangOpts;
  clang::edit::EditedSource Edits { ClangSourceManager, ClangLangOpts };

  auto InputState = States.back();
  auto InputText = InputState->getOutputText();

  EditorAdapter Editor { StartInstance->getSourceMgr(), ClangSourceManager };

  runAPIDiffMigratorPass(Editor, StartInstance->getPrimarySourceFile(),
                         getMigratorOptions());
  if (Opts.RunOptionalTryMigration) {
    runOptionalTryMigratorPass(Editor, StartInstance->getPrimarySourceFile(),
                               getMigratorOptions());
  }

  Edits.commit(Editor.getEdits());

  RewriteBufferEditsReceiver Rewriter {
    ClangSourceManager,
    Editor.getClangFileIDForSwiftBufferID(
      StartInstance->getPrimarySourceFile()->getBufferID().value()),
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

namespace {
/// Print a replacement from a diff edit scriptto the given output stream.
///
/// \param Filename The filename of the original file
/// \param Rep The Replacement to print
/// \param OS The output stream
void printReplacement(const StringRef Filename,
                      const Replacement &Rep,
                      llvm::raw_ostream &OS) {
  assert(!Filename.empty());
  if (Rep.Remove == 0 && Rep.Text.empty()) {
    return;
  }
  OS << "  {\n";

  OS << "    \"file\": \"";
  OS.write_escaped(Filename);
  OS << "\",\n";

  OS << "    \"offset\": " << Rep.Offset;
  if (Rep.Remove > 0) {
    OS << ",\n";
    OS << "    \"remove\": " << Rep.Remove;
  }
  if (!Rep.Text.empty()) {
    OS << ",\n";
    OS << "    \"text\": \"";
    OS.write_escaped(Rep.Text);
    OS << "\"\n";
  } else {
    OS << "\n";
  }
  OS << "  }";
}

/// Print a remap file to the given output stream.
///
/// \param OriginalFilename The filename of the file that was edited
/// not the output file for printing here.
/// \param InputText The input text without any changes.
/// \param OutputText The result text after any changes.
/// \param OS The output stream.
void printRemap(const StringRef OriginalFilename,
                const StringRef InputText,
                const StringRef OutputText,
                llvm::raw_ostream &OS) {
  assert(!OriginalFilename.empty());

  diff_match_patch<std::string> DMP;
  const auto Diffs =
      DMP.diff_main(InputText.str(), OutputText.str(), /*checkLines=*/false);

  OS << "[";

  size_t Offset = 0;

  llvm::SmallVector<Replacement, 32> Replacements;

  for (const auto &Diff : Diffs) {
    size_t OffsetIncrement = 0;
    switch (Diff.operation) {
      case decltype(DMP)::EQUAL:
        OffsetIncrement += Diff.text.size();
        break;
      case decltype(DMP)::INSERT:
        Replacements.push_back({ Offset, 0, Diff.text });
        break;
      case decltype(DMP)::DELETE:
        Replacements.push_back({ Offset, Diff.text.size(), "" });
        OffsetIncrement = Diff.text.size();
        break;
    }
    Offset += OffsetIncrement;
  }

  assert(Offset == InputText.size());

  // Combine removal edits with previous edits that are consecutive.
  for (unsigned i = 1; i < Replacements.size();) {
    auto &Previous = Replacements[i-1];
    auto &Current = Replacements[i];
    assert(Current.Offset >= Previous.Offset + Previous.Remove);
    unsigned Distance = Current.Offset-(Previous.Offset + Previous.Remove);
    if (Distance > 0) {
      ++i;
      continue;
    }
    if (!Current.Text.empty()) {
      ++i;
      continue;
    }
    Previous.Remove += Current.Remove;
    Replacements.erase(Replacements.begin() + i);
  }

  // Combine removal edits with next edits that are consecutive.
  for (unsigned i = 0; i + 1 < Replacements.size();) {
    auto &Current = Replacements[i];
    auto &nextRep = Replacements[i + 1];
    assert(nextRep.Offset >= Current.Offset + Current.Remove);
    unsigned Distance = nextRep.Offset - (Current.Offset + Current.Remove);
    if (Distance > 0) {
      ++i;
      continue;
    }
    if (!Current.Text.empty()) {
      ++i;
      continue;
    }
    nextRep.Offset -= Current.Remove;
    nextRep.Remove += Current.Remove;
    Replacements.erase(Replacements.begin() + i);
  }

  // For remaining removal diffs, include the byte adjacent to the range on the
  // left. libclang applies the diffs as byte diffs, so it doesn't matter if the
  // byte is part of a multi-byte UTF8 character.
  for (unsigned i = 0; i < Replacements.size(); ++i) {
    auto &Current = Replacements[i];
    if (!Current.Text.empty())
      continue;
    if (Current.Offset == 0)
      continue;
    Current.Offset -= 1;
    Current.Remove += 1;
    Current.Text = InputText.substr(Current.Offset, 1).str();
  }

  for (auto Rep = Replacements.begin(); Rep != Replacements.end(); ++Rep) {
    if (Rep != Replacements.begin()) {
      OS << ",\n";
    } else {
      OS << "\n";
    }
    printReplacement(OriginalFilename, *Rep, OS);
  }

  OS << "\n]";
}

} // end anonymous namespace

bool Migrator::emitRemap() const {
  const auto &RemapPath = getMigratorOptions().EmitRemapFilePath;
  if (RemapPath.empty()) {
    return false;
  }

  std::error_code Error;
  llvm::raw_fd_ostream FileOS(RemapPath,
                              Error, llvm::sys::fs::OF_Text);
  if (FileOS.has_error()) {
    return true;
  }

  auto InputText = States.front()->getOutputText();
  auto OutputText = States.back()->getOutputText();
  printRemap(getInputFilename(), InputText, OutputText, FileOS);

  FileOS.flush();
  return FileOS.has_error();
}

bool Migrator::emitMigratedFile() const {
  const auto &OutFilename = getMigratorOptions().EmitMigratedFilePath;
  if (OutFilename.empty()) {
    return false;
  }

  std::error_code Error;
  llvm::raw_fd_ostream FileOS(OutFilename,
                              Error, llvm::sys::fs::OF_Text);
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
  auto &PrimaryInput = StartInvocation.getFrontendOptions()
                           .InputsAndOutputs.getRequiredUniquePrimaryInput();
  return PrimaryInput.getFileName();
}
