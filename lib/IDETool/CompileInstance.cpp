//===--- CompileInstance.cpp ----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2021 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDETool/CompileInstance.h"

#include "DependencyChecking.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/Module.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Frontend/Frontend.h"
#include "swift/FrontendTool/FrontendTool.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "clang/AST/ASTContext.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;
using namespace swift::ide;

// Interface fingerprint check and modified function body collection.
namespace {

/// Information of modified function body.
struct ModInfo {
  /// Function decl in the *original* AST.
  AbstractFunctionDecl *FD;
  /// Range of the body in the *new* source file buffer.
  SourceRange NewSourceRange;

  ModInfo(AbstractFunctionDecl *FD, const SourceRange &NewSourceRange)
      : FD(FD), NewSourceRange(NewSourceRange) {}
};

static bool collectModifiedFunctions(ArrayRef<Decl *> r1, ArrayRef<Decl *> r2,
                                     llvm::SmallVectorImpl<ModInfo> &result) {
  assert(r1.size() == r2.size() &&
         "interface fingerprint matches but diffrent number of children");

  for (auto i1 = r1.begin(), i2 = r2.begin(), e1 = r1.end(), e2 = r2.end();
       i1 != e1 && i2 != e2; ++i1, ++i2) {
    auto &d1 = *i1, &d2 = *i2;

    assert(d1->getKind() == d2->getKind() &&
           "interface fingerprint matches but diffrent structure");

    /// FIXME: Nested types.
    ///   func foo() {
    ///     struct S {
    ///       func bar() { ... }
    ///     }
    ///   }
    /// * Could editing a local-type interface lead to the need for
    ///   retypechecking other functions?
    /// * Can we retypecheck only a function in local types? If only 'bar()'
    ///   body have changed, we want to only retypecheck 'bar()'.
    auto *f1 = dyn_cast<AbstractFunctionDecl>(d1);
    auto *f2 = dyn_cast<AbstractFunctionDecl>(d2);
    if (f1 && f2) {
      auto fp1 = f1->getBodyFingerprintIncludingLocalTypeMembers();
      auto fp2 = f2->getBodyFingerprintIncludingLocalTypeMembers();
      if (fp1 != fp2) {
        // The fingerprint of the body has changed. Record it.
        result.emplace_back(f1, f2->getBodySourceRange());
      }
      continue;
    }

    auto *idc1 = dyn_cast<IterableDeclContext>(d1);
    auto *idc2 = dyn_cast<IterableDeclContext>(d2);
    if (idc1 && idc2) {
      if (idc1->getBodyFingerprint() != idc2->getBodyFingerprint()) {
        // The fingerprint of the interface has changed. We can't reuse this.
        return true;
      }

      // Recurse into the child IDC members.
      if (collectModifiedFunctions(idc1->getParsedMembers(),
                                   idc2->getParsedMembers(), result)) {
        return true;
      }
    }
  }
  return false;
}

/// Collect functions in \p SF with modified bodies into \p result .
/// \p tmpSM is used for managing source buffers for new source files. Source
/// range for collected modified function body info is managed by \tmpSM.
/// \p tmpSM must be different from the source manager of \p SF .
static bool
getModifiedFunctionDeclList(const SourceFile &SF, SourceManager &tmpSM,
                            llvm::SmallVectorImpl<ModInfo> &result) {
  auto &ctx = SF.getASTContext();

  auto tmpBuffer = tmpSM.getFileSystem()->getBufferForFile(SF.getFilename());
  if (!tmpBuffer) {
    // The file is deleted?
    return true;
  }

  // Parse the new buffer into temporary SourceFile.

  LangOptions langOpts = ctx.LangOpts;
  TypeCheckerOptions typeckOpts = ctx.TypeCheckerOpts;
  SearchPathOptions searchPathOpts = ctx.SearchPathOpts;
  ClangImporterOptions clangOpts = ctx.ClangImporterOpts;
  SILOptions silOpts = ctx.SILOpts;
  symbolgraphgen::SymbolGraphOptions symbolOpts = ctx.SymbolGraphOpts;

  DiagnosticEngine tmpDiags(tmpSM);
  auto &tmpCtx = *ASTContext::get(langOpts, typeckOpts, silOpts, searchPathOpts,
                                  clangOpts, symbolOpts, tmpSM, tmpDiags);
  registerParseRequestFunctions(tmpCtx.evaluator);
  registerTypeCheckerRequestFunctions(tmpCtx.evaluator);

  ModuleDecl *tmpM = ModuleDecl::create(Identifier(), tmpCtx);
  auto tmpBufferID = tmpSM.addNewSourceBuffer(std::move(*tmpBuffer));
  SourceFile *tmpSF = new (tmpCtx)
      SourceFile(*tmpM, SF.Kind, tmpBufferID, SF.getParsingOptions());
  tmpM->addAuxiliaryFile(*tmpSF);

  // If the top-level code has been changed, we can't do anything.
  if (SF.getInterfaceHash() != tmpSF->getInterfaceHash())
    return true;

  return collectModifiedFunctions(SF.getTopLevelDecls(),
                                  tmpSF->getTopLevelDecls(), result);
}

/// Typecheck the body of \p func with the new source text specified with
/// \p newBodyRange managed by \p newSM .
///
/// This copies the source text of \p newBodyRange to the source manger
/// \p func originally parsed.
void retypeCheckFunctionBody(AbstractFunctionDecl *func,
                             SourceRange newBodyRange, SourceManager &newSM) {

  // To save the persistent memory in the source manager, add the sliced range
  // of the new function body to the source manager.
  // NOTE: Using 'getLocForStartOfLine' is to get the correct column value for
  //       diagnostics on the first line.
  auto tmpBufferID = newSM.findBufferContainingLoc(newBodyRange.Start);
  auto bufStartLoc = Lexer::getLocForStartOfLine(newSM, newBodyRange.Start);
  auto bufEndLoc = Lexer::getLocForEndOfToken(newSM, newBodyRange.End);

  auto bufStartOffset = newSM.getLocOffsetInBuffer(bufStartLoc, tmpBufferID);
  auto bufEndOffset = newSM.getLocOffsetInBuffer(bufEndLoc, tmpBufferID);

  auto slicedSourceText = newSM.getEntireTextForBuffer(tmpBufferID)
                              .slice(bufStartOffset, bufEndOffset);

  auto &origSM = func->getASTContext().SourceMgr;

  auto sliceBufferID = origSM.addMemBufferCopy(
      slicedSourceText, newSM.getIdentifierForBuffer(tmpBufferID));
  origSM.openVirtualFile(
      origSM.getLocForBufferStart(sliceBufferID),
      newSM.getDisplayNameForLoc(bufStartLoc),
      newSM.getPresumedLineAndColumnForLoc(bufStartLoc).first - 1);

  // Calculate the body range in the sliced source buffer.
  auto rangeStartOffset =
      newSM.getLocOffsetInBuffer(newBodyRange.Start, tmpBufferID);
  auto rangeEndOffset =
      newSM.getLocOffsetInBuffer(newBodyRange.End, tmpBufferID);
  auto rangeStartLoc =
      origSM.getLocForOffset(sliceBufferID, rangeStartOffset - bufStartOffset);
  auto rangeEndLoc =
      origSM.getLocForOffset(sliceBufferID, rangeEndOffset - bufStartOffset);
  SourceRange newRange{rangeStartLoc, rangeEndLoc};

  // Reset the body range of the function decl, and re-typecheck it.
  origSM.setGeneratedSourceInfo(
      sliceBufferID,
      GeneratedSourceInfo{
        GeneratedSourceInfo::ReplacedFunctionBody,
        Lexer::getCharSourceRangeFromSourceRange(
          origSM, func->getOriginalBodySourceRange()),
        Lexer::getCharSourceRangeFromSourceRange(origSM, newRange),
        func,
        nullptr
      }
  );
  func->setBodyToBeReparsed(newRange);
  (void)func->getTypecheckedBody();
}

} // namespace

bool CompileInstance::performCachedSemaIfPossible(DiagnosticConsumer *DiagC) {

  // Currently, only '-c' (aka. '-emit-object') action is supported.
  assert(CI->getInvocation().getFrontendOptions().RequestedAction ==
             FrontendOptions::ActionType::EmitObject &&
         "Unsupported action; only 'EmitObject' is supported");

  SourceManager &SM = CI->getSourceMgr();
  auto FS = SM.getFileSystem();

  if (shouldCheckDependencies()) {
    if (areAnyDependentFilesInvalidated(
            *CI, *FS, /*excludeBufferID=*/llvm::None,
            DependencyCheckedTimestamp, InMemoryDependencyHash)) {
      return true;
    }
    DependencyCheckedTimestamp = std::chrono::system_clock::now();
  }

  SourceManager tmpSM(FS);

  // Collect modified function body.
  SmallVector<ModInfo, 2> modifiedFuncDecls;
  bool isNotResuable = CI->forEachFileToTypeCheck([&](SourceFile &oldSF) {
    return getModifiedFunctionDeclList(oldSF, tmpSM, modifiedFuncDecls);
  });
  if (isNotResuable)
    return true;

  // OK, we can reuse the AST.

  CI->addDiagnosticConsumer(DiagC);
  SWIFT_DEFER { CI->removeDiagnosticConsumer(DiagC); };

  for (const auto &info : modifiedFuncDecls) {
    retypeCheckFunctionBody(info.FD, info.NewSourceRange, tmpSM);
  }

  return false;
}

bool CompileInstance::setupCI(
    llvm::ArrayRef<const char *> origArgs,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    DiagnosticConsumer *diagC) {
  auto &Diags = CI->getDiags();

  SmallVector<const char *, 16> args;
  // Put '-resource-dir' and '-diagnostic-documentation-path' at the top to
  // allow overriding them with the passed in arguments.
  args.append({"-resource-dir", RuntimeResourcePath.c_str()});
  args.append({"-Xfrontend", "-diagnostic-documentation-path", "-Xfrontend",
               DiagnosticDocumentationPath.c_str()});
  args.append(origArgs.begin(), origArgs.end());

  SmallString<256> driverPath(SwiftExecutablePath);
  llvm::sys::path::remove_filename(driverPath);
  llvm::sys::path::append(driverPath, "swiftc");

  CompilerInvocation invocation;
  bool invocationCreationFailed =
      driver::getSingleFrontendInvocationFromDriverArguments(
          driverPath, args, Diags,
          [&](ArrayRef<const char *> FrontendArgs) {
            return invocation.parseArgs(FrontendArgs, Diags);
          },
          /*ForceNoOutputs=*/false);
  if (invocationCreationFailed) {
    assert(Diags.hadAnyError());
    return false;
  }

  if (invocation.getFrontendOptions().RequestedAction !=
      FrontendOptions::ActionType::EmitObject) {
    Diags.diagnose(SourceLoc(), diag::not_implemented,
                   "only -c (aka. -emit-object) action is supported");
    return false;
  }

  // Since LLVM arguments are parsed into a global state, LLVM can't handle
  // multiple argument sets in a process simultaneously. So let's ignore them.
  // FIXME: Remove this if possible.
  invocation.getFrontendOptions().LLVMArgs.clear();

  /// Declare the frontend to be used for multiple compilations.
  invocation.getFrontendOptions().ReuseFrontendForMultipleCompilations = true;

  // Enable dependency trakcing (excluding system modules) to invalidate the
  // compiler instance if any dependent files are modified.
  invocation.getFrontendOptions().IntermoduleDependencyTracking =
      IntermoduleDepTrackingMode::ExcludeSystem;

  std::string InstanceSetupError;
  if (CI->setup(invocation, InstanceSetupError)) {
    assert(Diags.hadAnyError());
    return false;
  }
  CI->getASTContext().getPluginLoader().setRegistry(Plugins.get());

  return true;
}

bool CompileInstance::performSema(
    llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag) {

  // Compute the signature of the invocation.
  llvm::hash_code ArgsHash(0);
  for (auto arg : Args)
    ArgsHash = llvm::hash_combine(ArgsHash, StringRef(arg));

  if (CI && ArgsHash == CachedArgHash &&
      CachedReuseCount < Opts.MaxASTReuseCount) {
    CI->getASTContext().CancellationFlag = CancellationFlag;
    if (!performCachedSemaIfPossible(DiagC)) {
      // If we compileted cacehd Sema operation. We're done.
      ++CachedReuseCount;
      return CI->getDiags().hadAnyError();
    }
  }

  // Performing a new operation. Reset the compiler instance.
  CI = std::make_unique<CompilerInstance>();
  CI->addDiagnosticConsumer(DiagC);

  if (!setupCI(Args, fileSystem, DiagC)) {
    // Failed to setup the CI.
    CI.reset();
    return true;
  }

  // Remember cache related information.
  DependencyCheckedTimestamp = std::chrono::system_clock::now();
  CachedArgHash = ArgsHash;
  CachedReuseCount = 0;
  InMemoryDependencyHash.clear();
  cacheDependencyHashIfNeeded(*CI, /*excludeBufferID=*/llvm::None,
                              InMemoryDependencyHash);

  // Perform!
  CI->getASTContext().CancellationFlag = CancellationFlag;
  CI->performSema();
  CI->removeDiagnosticConsumer(DiagC);
  return CI->getDiags().hadAnyError();
}

bool CompileInstance::performCompile(
    llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag) {

  // Cancellation check. This gives a chance to cancel queued up requests before
  // processing anything.
  if (CancellationFlag && CancellationFlag->load(std::memory_order_relaxed))
    return true;

  if (performSema(Args, fileSystem, DiagC, CancellationFlag))
    return true;

  // Cancellation check after Sema.
  if (CI->isCancellationRequested())
    return true;

  CI->addDiagnosticConsumer(DiagC);
  SWIFT_DEFER { CI->removeDiagnosticConsumer(DiagC); };
  int ReturnValue = 0;
  return performCompileStepsPostSema(*CI, ReturnValue, /*observer=*/nullptr);
}

bool CompileInstance::shouldCheckDependencies() const {
  assert(CI);
  using namespace std::chrono;
  auto now = system_clock::now();
  auto threshold =
      DependencyCheckedTimestamp + seconds(Opts.DependencyCheckIntervalSecond);
  return threshold <= now;
}
