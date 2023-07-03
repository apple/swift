//===--- IDEInspectionInstance.cpp ----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/IDETool/IDEInspectionInstance.h"

#include "DependencyChecking.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/Module.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Frontend/Frontend.h"
#include "swift/IDE/CodeCompletion.h"
#include "swift/IDE/CodeCompletionConsumer.h"
#include "swift/Parse/Lexer.h"
#include "swift/Parse/PersistentParserState.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "clang/AST/ASTContext.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace swift;
using namespace ide;

std::unique_ptr<llvm::MemoryBuffer>
swift::ide::makeCodeCompletionMemoryBuffer(const llvm::MemoryBuffer *origBuf,
                                           unsigned &Offset,
                                           StringRef bufferIdentifier) {

  auto origBuffSize = origBuf->getBufferSize();
  if (Offset > origBuffSize)
    Offset = origBuffSize;

  auto newBuffer = llvm::WritableMemoryBuffer::getNewUninitMemBuffer(
      origBuffSize + 1, bufferIdentifier);
  auto *pos = origBuf->getBufferStart() + Offset;
  auto *newPos =
      std::copy(origBuf->getBufferStart(), pos, newBuffer->getBufferStart());
  *newPos = '\0';
  std::copy(pos, origBuf->getBufferEnd(), newPos + 1);

  return std::unique_ptr<llvm::MemoryBuffer>(newBuffer.release());
}

namespace {
/// Returns index number of \p D in \p Decls . If it's not found, returns ~0.
template <typename Range>
unsigned findIndexInRange(Decl *D, const Range &Decls) {
  unsigned N = 0;
  for (auto I = Decls.begin(), E = Decls.end(); I != E; ++I) {
    if ((*I)->isImplicit())
      continue;
    if (*I == D)
      return N;
    ++N;
  }
  return ~0U;
}

/// Return the element at \p N in \p Decls .
template <typename Range> Decl *getElementAt(const Range &Decls, unsigned N) {
  for (auto I = Decls.begin(), E = Decls.end(); I != E; ++I) {
    if ((*I)->isImplicit())
      continue;
    if (N == 0)
      return *I;
    --N;
  }
  return nullptr;
}

/// Find the equivalent \c DeclContext with \p DC from \p SF AST.
/// This assumes the AST which contains \p DC has exact the same structure with
/// \p SF.
static DeclContext *getEquivalentDeclContextFromSourceFile(DeclContext *DC,
                                                           SourceFile *SF) {
  PrettyStackTraceDeclContext trace("getting equivalent decl context for", DC);
  auto *newDC = DC;
  // NOTE: Shortcut for DC->getParentSourceFile() == SF case is not needed
  // because they should be always different.

  // Get the index path in the current AST.
  SmallVector<unsigned, 4> IndexStack;
  do {
    auto *D = newDC->getAsDecl();
    if (!D)
      return nullptr;
    auto *parentDC = newDC->getParent();
    unsigned N = ~0U;

    if (auto accessor = dyn_cast<AccessorDecl>(D)) {
      // The AST for accessors is like:
      //   DeclContext -> AbstractStorageDecl -> AccessorDecl
      // We need to push the index of the accessor within the accessor list
      // of the storage.
      auto *storage = accessor->getStorage();
      if (!storage)
        return nullptr;
      auto accessorN = findIndexInRange(accessor, storage->getAllAccessors());
      IndexStack.push_back(accessorN);
      D = storage;
    }

    if (auto parentSF = dyn_cast<SourceFile>(parentDC)) {
      N = findIndexInRange(D, parentSF->getTopLevelDecls());
    } else if (auto parentIDC = dyn_cast_or_null<IterableDeclContext>(
                   parentDC->getAsDecl())) {
      N = findIndexInRange(D, parentIDC->getMembers());
    } else {
#ifndef NDEBUG
      llvm_unreachable("invalid DC kind for finding equivalent DC (indexpath)");
#endif
      return nullptr;
    }

    // Not found in the decl context tree.
    if (N == ~0U) {
      return nullptr;
    }

    IndexStack.push_back(N);
    newDC = parentDC;
  } while (!newDC->isModuleScopeContext());

  assert(isa<SourceFile>(newDC) && "DC should be in a SourceFile");

  // Query the equivalent decl context from the base SourceFile using the index
  // path.
  newDC = SF;
  do {
    auto N = IndexStack.pop_back_val();
    Decl *D = nullptr;
    if (auto parentSF = dyn_cast<SourceFile>(newDC))
      D = getElementAt(parentSF->getTopLevelDecls(), N);
    else if (auto parentIDC = dyn_cast<IterableDeclContext>(newDC->getAsDecl()))
      D = getElementAt(parentIDC->getMembers(), N);
    else
      llvm_unreachable("invalid DC kind for finding equivalent DC (query)");

    if (auto storage = dyn_cast_or_null<AbstractStorageDecl>(D)) {
      if (IndexStack.empty())
        return nullptr;
      auto accessorN = IndexStack.pop_back_val();
      D = getElementAt(storage->getAllAccessors(), accessorN);
    }

    newDC = dyn_cast_or_null<DeclContext>(D);
    if (!newDC)
      return nullptr;
  } while (!IndexStack.empty());

  assert(newDC->getContextKind() == DC->getContextKind());

  return newDC;
}

} // namespace

bool IDEInspectionInstance::performCachedOperationIfPossible(
    llvm::hash_code ArgsHash,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    const SearchPathOptions &SearchPathOpts,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<IDEInspectionInstanceResult>)>
        Callback) {
  llvm::PrettyStackTraceString trace(
      "While performing cached IDE inspection if possible");

  // Check the invalidation first. Otherwise, in case no 'CacheCI' exists yet,
  // the flag will remain 'true' even after 'CachedCI' is populated.
  if (CachedCIShouldBeInvalidated.exchange(false))
    return false;
  if (!CachedCI)
    return false;
  if (CachedReuseCount >= Opts.MaxASTReuseCount)
    return false;
  if (CachedArgHash != ArgsHash)
    return false;

  auto *oldSF = CachedCI->getIDEInspectionFile();
  assert(oldSF->getBufferID());

  auto *oldState = oldSF->getDelayedParserState();
  assert(oldState->hasIDEInspectionDelayedDeclState());
  auto &oldInfo = oldState->getIDEInspectionDelayedDeclState();

  auto &SM = CachedCI->getSourceMgr();
  auto bufferName = ideInspectionTargetBuffer->getBufferIdentifier();
  if (SM.getIdentifierForBuffer(*oldSF->getBufferID()) != bufferName)
    return false;

  if (shouldCheckDependencies()) {
    // The passed in FileSystem does not have any overlays resolved. Make sure
    // to do so before checking dependencies (otherwise we might decide we need
    // to run the slow path due to a missing/different file).
    auto ExpectedOverlay = SearchPathOpts.makeOverlayFileSystem(FileSystem);
    if (ExpectedOverlay) {
      FileSystem = std::move(ExpectedOverlay.get());
    } else {
      llvm::consumeError(ExpectedOverlay.takeError());
    }

    if (areAnyDependentFilesInvalidated(
            *CachedCI, *FileSystem, *oldSF->getBufferID(),
            DependencyCheckedTimestamp, InMemoryDependencyHash))
      return false;
    DependencyCheckedTimestamp = std::chrono::system_clock::now();
  }

  // Parse the new buffer into temporary SourceFile.
  SourceManager tmpSM;
  auto tmpBufferID = tmpSM.addMemBufferCopy(ideInspectionTargetBuffer);
  tmpSM.setIDEInspectionTarget(tmpBufferID, Offset);

  LangOptions langOpts = CachedCI->getASTContext().LangOpts;
  TypeCheckerOptions typeckOpts = CachedCI->getASTContext().TypeCheckerOpts;
  SILOptions silOpts = CachedCI->getASTContext().SILOpts;
  SearchPathOptions searchPathOpts = CachedCI->getASTContext().SearchPathOpts;
  DiagnosticEngine tmpDiags(tmpSM);
  ClangImporterOptions clangOpts;
  symbolgraphgen::SymbolGraphOptions symbolOpts;
  std::unique_ptr<ASTContext> tmpCtx(
      ASTContext::get(langOpts, typeckOpts, silOpts, searchPathOpts, clangOpts,
                      symbolOpts, tmpSM, tmpDiags));
  tmpCtx->CancellationFlag = CancellationFlag;
  registerParseRequestFunctions(tmpCtx->evaluator);
  registerIDERequestFunctions(tmpCtx->evaluator);
  registerTypeCheckerRequestFunctions(tmpCtx->evaluator);
  registerClangImporterRequestFunctions(tmpCtx->evaluator);
  registerConstExtractRequestFunctions(tmpCtx->evaluator);
  registerSILGenRequestFunctions(tmpCtx->evaluator);
  ModuleDecl *tmpM = ModuleDecl::create(Identifier(), *tmpCtx);
  SourceFile *tmpSF = new (*tmpCtx)
      SourceFile(*tmpM, oldSF->Kind, tmpBufferID, oldSF->getParsingOptions());
  tmpM->addAuxiliaryFile(*tmpSF);

  // FIXME: Since we don't setup module loaders on the temporary AST context,
  // 'canImport()' conditional compilation directive always fails. That causes
  // interface hash change and prevents reuse of AST context.

  // Parse and get the IDE inspection context.
  auto *newState = tmpSF->getDelayedParserState();
  if (!newState->hasIDEInspectionDelayedDeclState())
    return false;

  auto &newInfo = newState->getIDEInspectionDelayedDeclState();
  unsigned newBufferID;
  DeclContext *traceDC = nullptr;
  switch (newInfo.Kind) {
  case IDEInspectionDelayedDeclKind::FunctionBody: {
    // If the interface has changed, AST must be refreshed.
    // See if the inteface of the function and types visible from a function
    // body has changed since the last completion. If they haven't changed,
    // completion can reuse the existing AST of the source file.
    // \c getInterfaceHash() is not enough because it doesn't take the interface
    // of the type members into account. For example:
    //
    //   struct S {
    //     func foo() {}
    //   }
    //   func main(val: S) {
    //     val.<HERE>
    //   }
    //
    // In this case, we need to ensure that the interface of \c S hasn't
    // changed. Note that we don't care about local types (i.e. type
    // declarations inside function bodies, closures, or top level statement
    // bodies) because they are not visible from other functions where the
    // completion is happening.
    const auto oldInterfaceHash = oldSF->getInterfaceHashIncludingTypeMembers();
    const auto newInterfaceHash = tmpSF->getInterfaceHashIncludingTypeMembers();
    if (oldInterfaceHash != newInterfaceHash)
      return false;

    DeclContext *DC =
        getEquivalentDeclContextFromSourceFile(newInfo.ParentContext, oldSF);
    if (!DC || !isa<AbstractFunctionDecl>(DC))
      return false;

    // OK, we can perform fast completion for this. Update the orignal delayed
    // decl state.

    // Fast completion keeps the buffer in memory for multiple completions.
    // To reduce the consumption, slice the source buffer so it only holds
    // the portion that is needed for the second pass.
    auto startOffset = newInfo.StartOffset;
    if (newInfo.PrevOffset != ~0u)
      startOffset = newInfo.PrevOffset;
    auto startLoc = tmpSM.getLocForOffset(tmpBufferID, startOffset);
    startLoc = Lexer::getLocForStartOfLine(tmpSM, startLoc);
    startOffset = tmpSM.getLocOffsetInBuffer(startLoc, tmpBufferID);

    auto endOffset = newInfo.EndOffset;
    auto endLoc = tmpSM.getLocForOffset(tmpBufferID, endOffset);
    endLoc = Lexer::getLocForEndOfToken(tmpSM, endLoc);
    endOffset = tmpSM.getLocOffsetInBuffer(endLoc, tmpBufferID);

    newInfo.StartOffset -= startOffset;
    newInfo.EndOffset -= startOffset;
    if (newInfo.PrevOffset != ~0u)
      newInfo.PrevOffset -= startOffset;

    auto sourceText =
        ideInspectionTargetBuffer->getBuffer().slice(startOffset, endOffset);
    auto newOffset = Offset - startOffset;

    newBufferID = SM.addMemBufferCopy(sourceText, bufferName);
    SM.openVirtualFile(SM.getLocForBufferStart(newBufferID),
                       tmpSM.getDisplayNameForLoc(startLoc),
                       tmpSM.getPresumedLineAndColumnForLoc(startLoc).first -
                           1);
    SM.setIDEInspectionTarget(newBufferID, newOffset);

    oldInfo.ParentContext = DC;
    oldInfo.StartOffset = newInfo.StartOffset;
    oldInfo.EndOffset = newInfo.EndOffset;
    oldInfo.PrevOffset = newInfo.PrevOffset;
    oldState->restoreIDEInspectionDelayedDeclState(oldInfo);

    auto newBufferStart = SM.getRangeForBuffer(newBufferID).getStart();
    SourceRange newBodyRange(newBufferStart.getAdvancedLoc(newInfo.StartOffset),
                             newBufferStart.getAdvancedLoc(newInfo.EndOffset));

    auto *AFD = cast<AbstractFunctionDecl>(DC);
    SM.setGeneratedSourceInfo(
        newBufferID,
        GeneratedSourceInfo{
          GeneratedSourceInfo::ReplacedFunctionBody,
          Lexer::getCharSourceRangeFromSourceRange(
              SM, AFD->getOriginalBodySourceRange()),
          Lexer::getCharSourceRangeFromSourceRange(SM, newBodyRange),
          AFD,
          nullptr
        }
    );

    AFD->setBodyToBeReparsed(newBodyRange);
    oldSF->clearScope();

    traceDC = AFD;
    break;
  }
  case IDEInspectionDelayedDeclKind::Decl:
  case IDEInspectionDelayedDeclKind::TopLevelCodeDecl: {
    // Support decl/top-level code only if the completion happens in a single
    // file 'main' script (e.g. playground).
    auto *oldM = oldInfo.ParentContext->getParentModule();
    if (oldM->getFiles().size() != 1 || oldSF->Kind != SourceFileKind::Main)
      return false;

    // Perform fast completion.

    // Prepare the new buffer in the source manager.
    auto sourceText = ideInspectionTargetBuffer->getBuffer();
    if (newInfo.Kind == IDEInspectionDelayedDeclKind::TopLevelCodeDecl) {
      // We don't need the source text after the top-level code.
      auto endOffset = newInfo.EndOffset;
      auto endLoc = tmpSM.getLocForOffset(tmpBufferID, endOffset);
      endLoc = Lexer::getLocForEndOfToken(tmpSM, endLoc);
      endOffset = tmpSM.getLocOffsetInBuffer(endLoc, tmpBufferID);
      sourceText = sourceText.slice(0, endOffset);
    }
    newBufferID = SM.addMemBufferCopy(sourceText, bufferName);
    SM.setIDEInspectionTarget(newBufferID, Offset);

    // Create a new module and a source file using the current AST context.
    auto &Ctx = oldM->getASTContext();
    auto *newM = ModuleDecl::createMainModule(Ctx, oldM->getName(),
                                              oldM->getImplicitImportInfo());
    newM->setABIName(oldM->getABIName());
    auto *newSF = new (Ctx) SourceFile(*newM, SourceFileKind::Main, newBufferID,
                                       oldSF->getParsingOptions());
    newM->addFile(*newSF);

    // Tell the compiler instance we've replaced the main module.
    CachedCI->setMainModule(newM);

    // Re-process the whole file (parsing will be lazily triggered). Still
    // re-use imported modules.
    performImportResolution(*newSF);
    bindExtensions(*newM);

    traceDC = newM;
#ifndef NDEBUG
    const auto *reparsedState = newSF->getDelayedParserState();
    assert(reparsedState->hasIDEInspectionDelayedDeclState() &&
           "Didn't find IDE inspection point?");

    auto &reparsedInfo = reparsedState->getIDEInspectionDelayedDeclState();
    assert(reparsedInfo.Kind == newInfo.Kind);
#endif
    break;
  }
  }

  {
    PrettyStackTraceDeclContext trace("performing cached IDE inspection",
                                      traceDC);

    // The diagnostic engine is keeping track of state which might modify
    // parsing and type checking behaviour. Clear the flags.
    CachedCI->getDiags().resetHadAnyError();
    CachedCI->getASTContext().CancellationFlag = CancellationFlag;

    if (DiagC)
      CachedCI->addDiagnosticConsumer(DiagC);

    if (CancellationFlag && CancellationFlag->load(std::memory_order_relaxed)) {
      Callback(CancellableResult<IDEInspectionInstanceResult>::cancelled());
    } else {
      Callback(CancellableResult<IDEInspectionInstanceResult>::success(
          {CachedCI, /*reusingASTContext=*/true,
           /*DidFindIDEInspectionTarget=*/true}));
    }

    if (DiagC)
      CachedCI->removeDiagnosticConsumer(DiagC);
  }

  CachedReuseCount += 1;

  return true;
}

void IDEInspectionInstance::performNewOperation(
    llvm::Optional<llvm::hash_code> ArgsHash,
    swift::CompilerInvocation &Invocation,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<IDEInspectionInstanceResult>)>
        Callback) {
  llvm::PrettyStackTraceString trace("While performing new IDE inspection");

  // If ArgsHash is None we shouldn't cache the compiler instance.
  bool ShouldCacheCompilerInstance = ArgsHash.has_value();

  auto CI = std::make_shared<CompilerInstance>();

  // Track non-system dependencies in fast-completion mode to invalidate the
  // compiler instance if any dependent files are modified.
  Invocation.getFrontendOptions().IntermoduleDependencyTracking =
      IntermoduleDepTrackingMode::ExcludeSystem;

  {
    if (DiagC)
      CI->addDiagnosticConsumer(DiagC);

    SWIFT_DEFER {
      if (DiagC)
        CI->removeDiagnosticConsumer(DiagC);
    };

    if (FileSystem != llvm::vfs::getRealFileSystem())
      CI->getSourceMgr().setFileSystem(FileSystem);

    Invocation.setIDEInspectionTarget(ideInspectionTargetBuffer, Offset);

    std::string InstanceSetupError;
    if (CI->setup(Invocation, InstanceSetupError)) {
      Callback(CancellableResult<IDEInspectionInstanceResult>::failure(
          InstanceSetupError));
      return;
    }
    CI->getASTContext().getPluginLoader().setRegistry(Plugins.get());
    CI->getASTContext().CancellationFlag = CancellationFlag;
    registerIDERequestFunctions(CI->getASTContext().evaluator);

    CI->performParseAndResolveImportsOnly();

    bool DidFindIDEInspectionTarget = CI->getIDEInspectionFile()
                                        ->getDelayedParserState()
                                        ->hasIDEInspectionDelayedDeclState();
    ShouldCacheCompilerInstance &= DidFindIDEInspectionTarget;

    auto CancellationFlag = CI->getASTContext().CancellationFlag;
    if (CancellationFlag && CancellationFlag->load(std::memory_order_relaxed)) {
      Callback(CancellableResult<IDEInspectionInstanceResult>::cancelled());
      // The completion instance may be in an invalid state when it's been
      // cancelled. Don't cache it.
      ShouldCacheCompilerInstance = false;
    } else {
      Callback(CancellableResult<IDEInspectionInstanceResult>::success(
          {CI, /*ReuisingASTContext=*/false, DidFindIDEInspectionTarget}));
      if (CancellationFlag &&
          CancellationFlag->load(std::memory_order_relaxed)) {
        ShouldCacheCompilerInstance = false;
      }
    }
  }

  // Cache the compiler instance if fast completion is enabled.
  // If we didn't find a code compleiton token, we can't cache the instance
  // because performCachedOperationIfPossible wouldn't have an old code
  // completion state to compare the new one to.
  if (ShouldCacheCompilerInstance)
    cacheCompilerInstance(std::move(CI), *ArgsHash);
}

void IDEInspectionInstance::cacheCompilerInstance(
    std::shared_ptr<CompilerInstance> CI, llvm::hash_code ArgsHash) {
  CachedCI = std::move(CI);
  CachedArgHash = ArgsHash;
  auto now = std::chrono::system_clock::now();
  DependencyCheckedTimestamp = now;
  CachedReuseCount = 0;
  InMemoryDependencyHash.clear();
  cacheDependencyHashIfNeeded(
      *CachedCI,
      CachedCI->getASTContext().SourceMgr.getIDEInspectionTargetBufferID(),
      InMemoryDependencyHash);
}

bool IDEInspectionInstance::shouldCheckDependencies() const {
  assert(CachedCI);
  using namespace std::chrono;
  auto now = system_clock::now();
  auto threshold = DependencyCheckedTimestamp +
                   seconds(Opts.DependencyCheckIntervalSecond);
  return threshold <= now;
}

void IDEInspectionInstance::markCachedCompilerInstanceShouldBeInvalidated() {
  CachedCIShouldBeInvalidated = true;
}

void IDEInspectionInstance::setOptions(IDEInspectionInstance::Options NewOpts) {
  std::lock_guard<std::mutex> lock(mtx);
  Opts = NewOpts;
}

void swift::ide::IDEInspectionInstance::performOperation(
    swift::CompilerInvocation &Invocation, llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<IDEInspectionInstanceResult>)>
        Callback) {
  // Compute the signature of the invocation.
  llvm::hash_code ArgsHash(0);
  for (auto arg : Args)
    ArgsHash = llvm::hash_combine(ArgsHash, StringRef(arg));

  // Concurrent completions will block so that they have higher chance to use
  // the cached completion instance.
  std::lock_guard<std::mutex> lock(mtx);

  if (performCachedOperationIfPossible(ArgsHash, FileSystem,
                                       Invocation.getSearchPathOptions(),
                                       ideInspectionTargetBuffer, Offset, DiagC,
                                       CancellationFlag, Callback)) {
    // We were able to reuse a cached AST. Callback has already been invoked
    // and we don't need to build a new AST. We are done.
    return;
  }

  // We don't need token list.
  Invocation.getLangOptions().CollectParsedToken = false;

  performNewOperation(ArgsHash, Invocation, FileSystem,
                      ideInspectionTargetBuffer, Offset, DiagC,
                      CancellationFlag, Callback);
}

void swift::ide::IDEInspectionInstance::codeComplete(
    swift::CompilerInvocation &Invocation, llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC, ide::CodeCompletionContext &CompletionContext,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<CodeCompleteResult>)> Callback) {
  using ResultType = CancellableResult<CodeCompleteResult>;

  struct ConsumerToCallbackAdapter
      : public SimpleCachingCodeCompletionConsumer {
    SwiftCompletionInfo SwiftContext;
    ImportDepth ImportDep;
    std::shared_ptr<std::atomic<bool>> CancellationFlag;
    llvm::function_ref<void(ResultType)> Callback;
    bool HandleResultsCalled = false;

    ConsumerToCallbackAdapter(
        ImportDepth ImportDep,
        std::shared_ptr<std::atomic<bool>> CancellationFlag,
        llvm::function_ref<void(ResultType)> Callback)
        : ImportDep(ImportDep), CancellationFlag(CancellationFlag),
          Callback(Callback) {}

    void setContext(std::shared_ptr<CompilerInstance> compilerInstance,
                    swift::ide::CodeCompletionContext *completionContext) {
      SwiftContext.compilerInstance = std::move(compilerInstance);
      SwiftContext.completionContext = completionContext;
    }
    void clearContext() { SwiftContext = SwiftCompletionInfo(); }

    void handleResults(CodeCompletionContext &context) override {
      HandleResultsCalled = true;
      if (CancellationFlag &&
          CancellationFlag->load(std::memory_order_relaxed)) {
        Callback(ResultType::cancelled());
      } else {
        assert(SwiftContext.compilerInstance);
        Callback(ResultType::success({context.getResultSink(), SwiftContext, ImportDep}));
      }
    }
  };

  // Disable source location resolutions from .swiftsourceinfo file because
  // they're somewhat heavy operations and aren't needed for completion.
  performOperation(
      Invocation, Args, FileSystem, ideInspectionTargetBuffer, Offset, DiagC,
      CancellationFlag,
      [&](CancellableResult<IDEInspectionInstanceResult> CIResult) {
        CIResult.mapAsync<CodeCompleteResult>(
            [&CompletionContext, &CancellationFlag](auto &Result,
                                                    auto DeliverTransformed) {
              CompletionContext.ReusingASTContext = Result.DidReuseAST;
              std::shared_ptr<CompilerInstance> CI = Result.CI;
              ImportDepth ImportDep{CI->getASTContext(),
                                    CI->getInvocation().getFrontendOptions()};
              ConsumerToCallbackAdapter Consumer(ImportDep, CancellationFlag,
                                                 DeliverTransformed);

              std::unique_ptr<IDEInspectionCallbacksFactory> callbacksFactory(
                  ide::makeCodeCompletionCallbacksFactory(CompletionContext,
                                                          Consumer));

              if (!Result.DidFindIDEInspectionTarget) {
                SwiftCompletionInfo Info{CI, &CompletionContext};
                CodeCompletionResultSink ResultSink;
                DeliverTransformed(ResultType::success({ResultSink, Info, ImportDep}));
                return;
              }

              Consumer.setContext(CI, &CompletionContext);
              performIDEInspectionSecondPass(*CI->getIDEInspectionFile(),
                                             *callbacksFactory);
              Consumer.clearContext();
              if (!Consumer.HandleResultsCalled) {
                // If we didn't receive a handleResult call from the second
                // pass, we didn't receive any results. To make sure Callback
                // gets called exactly once, call it manually with no results
                // here.
                SwiftCompletionInfo Info{CI, &CompletionContext};
                CodeCompletionResultSink ResultSink;
                DeliverTransformed(ResultType::success({ResultSink, Info, ImportDep}));
              }
            },
            Callback);
      });
}

void swift::ide::IDEInspectionInstance::typeContextInfo(
    swift::CompilerInvocation &Invocation, llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<TypeContextInfoResult>)>
        Callback) {
  using ResultType = CancellableResult<TypeContextInfoResult>;

  struct ConsumerToCallbackAdapter : public ide::TypeContextInfoConsumer {
    bool ReusingASTContext;
    std::shared_ptr<std::atomic<bool>> CancellationFlag;
    llvm::function_ref<void(ResultType)> Callback;
    bool HandleResultsCalled = false;

    ConsumerToCallbackAdapter(
        bool ReusingASTContext,
        std::shared_ptr<std::atomic<bool>> CancellationFlag,
        llvm::function_ref<void(ResultType)> Callback)
        : ReusingASTContext(ReusingASTContext),
          CancellationFlag(CancellationFlag), Callback(Callback) {}

    void handleResults(ArrayRef<ide::TypeContextInfoItem> Results) override {
      HandleResultsCalled = true;
      if (CancellationFlag &&
          CancellationFlag->load(std::memory_order_relaxed)) {
        Callback(ResultType::cancelled());
      } else {
        Callback(ResultType::success({Results, ReusingASTContext}));
      }
    }
  };

  performOperation(
      Invocation, Args, FileSystem, ideInspectionTargetBuffer, Offset, DiagC,
      CancellationFlag,
      [&](CancellableResult<IDEInspectionInstanceResult> CIResult) {
        CIResult.mapAsync<TypeContextInfoResult>(
            [&CancellationFlag](auto &Result, auto DeliverTransformed) {
              ConsumerToCallbackAdapter Consumer(
                  Result.DidReuseAST, CancellationFlag, DeliverTransformed);
              std::unique_ptr<IDEInspectionCallbacksFactory> callbacksFactory(
                  ide::makeTypeContextInfoCallbacksFactory(Consumer));

              if (!Result.DidFindIDEInspectionTarget) {
                // Deliver empty results if we didn't find a code completion
                // token.
                DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }

              performIDEInspectionSecondPass(
                  *Result.CI->getIDEInspectionFile(), *callbacksFactory);
              if (!Consumer.HandleResultsCalled) {
                // If we didn't receive a handleResult call from the second
                // pass, we didn't receive any results. To make sure Callback
                // gets called exactly once, call it manually with no results
                // here.
                DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }
            },
            Callback);
      });
}

void swift::ide::IDEInspectionInstance::conformingMethodList(
    swift::CompilerInvocation &Invocation, llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC, ArrayRef<const char *> ExpectedTypeNames,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<ConformingMethodListResults>)>
        Callback) {
  using ResultType = CancellableResult<ConformingMethodListResults>;

  struct ConsumerToCallbackAdapter
      : public swift::ide::ConformingMethodListConsumer {
    bool ReusingASTContext;
    std::shared_ptr<std::atomic<bool>> CancellationFlag;
    llvm::function_ref<void(ResultType)> Callback;
    bool HandleResultsCalled = false;

    ConsumerToCallbackAdapter(
        bool ReusingASTContext,
        std::shared_ptr<std::atomic<bool>> CancellationFlag,
        llvm::function_ref<void(ResultType)> Callback)
        : ReusingASTContext(ReusingASTContext),
          CancellationFlag(CancellationFlag), Callback(Callback) {}

    void handleResult(const ide::ConformingMethodListResult &result) override {
      HandleResultsCalled = true;
      if (CancellationFlag &&
          CancellationFlag->load(std::memory_order_relaxed)) {
        Callback(ResultType::cancelled());
      } else {
        Callback(ResultType::success({&result, ReusingASTContext}));
      }
    }
  };

  performOperation(
      Invocation, Args, FileSystem, ideInspectionTargetBuffer, Offset, DiagC,
      CancellationFlag,
      [&](CancellableResult<IDEInspectionInstanceResult> CIResult) {
        CIResult.mapAsync<ConformingMethodListResults>(
            [&ExpectedTypeNames, &CancellationFlag](auto &Result,
                                                    auto DeliverTransformed) {
              ConsumerToCallbackAdapter Consumer(
                  Result.DidReuseAST, CancellationFlag, DeliverTransformed);
              std::unique_ptr<IDEInspectionCallbacksFactory> callbacksFactory(
                  ide::makeConformingMethodListCallbacksFactory(
                      ExpectedTypeNames, Consumer));

              if (!Result.DidFindIDEInspectionTarget) {
                DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }

              performIDEInspectionSecondPass(
                  *Result.CI->getIDEInspectionFile(), *callbacksFactory);
              if (!Consumer.HandleResultsCalled) {
                // If we didn't receive a handleResult call from the second
                // pass, we didn't receive any results. To make sure Callback
                // gets called exactly once, call it manually with no results
                // here.
                DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }
            },
            Callback);
      });
}

void swift::ide::IDEInspectionInstance::cursorInfo(
    swift::CompilerInvocation &Invocation, llvm::ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    llvm::MemoryBuffer *ideInspectionTargetBuffer, unsigned int Offset,
    DiagnosticConsumer *DiagC,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    llvm::function_ref<void(CancellableResult<CursorInfoResults>)> Callback) {
  using ResultType = CancellableResult<CursorInfoResults>;

  struct ConsumerToCallbackAdapter : public swift::ide::CursorInfoConsumer {
    bool ReusingASTContext;
    std::shared_ptr<std::atomic<bool>> CancellationFlag;
    llvm::function_ref<void(ResultType)> Callback;
    bool HandleResultsCalled = false;

    ConsumerToCallbackAdapter(
        bool ReusingASTContext,
        std::shared_ptr<std::atomic<bool>> CancellationFlag,
        llvm::function_ref<void(ResultType)> Callback)
        : ReusingASTContext(ReusingASTContext),
          CancellationFlag(CancellationFlag), Callback(Callback) {}

    void handleResults(std::vector<ResolvedCursorInfoPtr> result) override {
      HandleResultsCalled = true;
      if (CancellationFlag &&
          CancellationFlag->load(std::memory_order_relaxed)) {
        Callback(ResultType::cancelled());
      } else {
        Callback(ResultType::success({result, ReusingASTContext}));
      }
    }
  };

  performOperation(
      Invocation, Args, FileSystem, ideInspectionTargetBuffer, Offset, DiagC,
      CancellationFlag,
      [&](CancellableResult<IDEInspectionInstanceResult> CIResult) {
        CIResult.mapAsync<CursorInfoResults>(
            [&CancellationFlag](auto &Result, auto DeliverTransformed) {
              auto &Mgr = Result.CI->getSourceMgr();
              auto RequestedLoc = Mgr.getIDEInspectionTargetLoc();
              ConsumerToCallbackAdapter Consumer(
                  Result.DidReuseAST, CancellationFlag, DeliverTransformed);
              std::unique_ptr<IDEInspectionCallbacksFactory> callbacksFactory(
                  ide::makeCursorInfoCallbacksFactory(Consumer, RequestedLoc));

              if (!Result.DidFindIDEInspectionTarget) {
                return DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }

              performIDEInspectionSecondPass(
                  *Result.CI->getIDEInspectionFile(), *callbacksFactory);
              if (!Consumer.HandleResultsCalled) {
                // If we didn't receive a handleResult call from the second
                // pass, we didn't receive any results. To make sure Callback
                // gets called exactly once, call it manually with no results
                // here.
                DeliverTransformed(
                    ResultType::success({/*Results=*/{}, Result.DidReuseAST}));
              }
            },
            Callback);
      });
}
