//===--- SwiftASTManager.h - ------------------------------------*- C++ -*-===//
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
// The SwiftASTManager stack consists of four essential classes:
//  - SwiftASTManager: Global object that is the entry point into AST building
//  - ASTProducer: Produces ASTs for a single compiler invocation. Keeps track
//      of old ASTs and determines if the olds ASTs are suitable to serve a
//      SwiftASTConsumer (through SwiftASTConsumer::canUseASTWithSnapshots).
//  - ASTBuildOperation: If ASTProducer decides that an AST needs to be built,
//      it creates an ASTBuildOperation. It keeps track of all consumers that
//      depend on it and supports AST build cancellation if all interested
//      consumers disappear.
//  - SwiftASTConsumer: Correlates to a SourceKit request. It just wants an AST
//      and doesn't care if/how the AST is built or whether it's served from a
//      cache.
//
// There is a strict memory ownership hierarchy. Classes higher up hold strong
// references to once further down. Callbacks that call upwards only hold weak
// references.
// Exception: Everything can hold a strong reference to the SwiftASTManager,
// because it's expected to stay alive anyway. The SwiftASTManager is
// responsible to occasionally clean up all strong references to classes lower
// down.
//
// Adding a new consumer:
// ----------------------
// To add a new consumer, SwiftASTManager::processASTAsync is called. It finds
// a suitable ASTProducer, which in turn looks up an ASTBuildOperation that
// either contains the same snapshot state that the consumer requested or one
// that is close enough to the consumer's expectation that it can serve it.
// If there already exists one, the consumer is added to it. Should the build
// operation already be finished, the consumer is directly called with the
// result. Otherwise, a new ASTBuildOperation is created, the consumer is added
// to it and the ASTBuildOperation is scheduled on
// SwiftASTManager::Implementation::ASTBuildQueue. This ensures that only one
// AST is built at a time.
// The SwiftASTManager keeps a weak reference to the consumer, so that the
// consumer can be cancelled if new requests come in (see implementation of
// processASTAsync).
//
// Building an AST:
// ----------------
// If it's a ASTBuildOperation's turn to build an AST (guarded by
// ASTBuildQueue), it checks whether any consumers are attached to it. If none
// are, there is no work to do and it finishes.
// Otherwise, it builds the AST, informs the consumers about the result and
// saves the result in a member variable so that it can serve future
// SwiftASTConsumers from it.
//
// Cancellation:
// -------------
// Only SwiftASTConsumers can be cancelled. The cancellation of the AST build
// only happens if no more consumers are interested in it.
// If requestCancellation is called on a SwiftASTConsumer, it informs the
// ASTBuildOperation, it is scheduled on, that it's no longer interested in the
// result. In theory, the ASTBuildOperation might or might not honour that
// request. In practice, it will always call SwiftASTConsumer::cancelled and
// remove the consumer from the list of consumers interested in its result. If
// no more consumers are interested in the ASTBuildOperation's result, it
// cancels the AST build by setting a flag that causes the type checker to fail
// with a "expression is too complex" error at the next suitable opportunity.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTASTMANAGER_H
#define LLVM_SOURCEKIT_LIB_SWIFTLANG_SWIFTASTMANAGER_H

#include "SourceKit/Core/Context.h"
#include "SourceKit/Core/LLVM.h"
#include "SourceKit/Support/CancellationToken.h"
#include "SwiftInvocation.h"
#include "swift/Frontend/FrontendOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Mutex.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <functional>
#include <string>

namespace llvm {
  class MemoryBuffer;
}

namespace swift {
  class CompilerInstance;
  class CompilerInvocation;
  class DiagnosticEngine;
  class PluginRegistry;
  class SourceFile;
  class SourceManager;
}

namespace SourceKit {
  class Context;
  struct DiagnosticEntryInfo;
  class ImmutableTextSnapshot;
  typedef RefPtr<ImmutableTextSnapshot> ImmutableTextSnapshotRef;
  class SwiftEditorDocumentFileMap;
  class SwiftLangSupport;
  class SwiftInvocation;
  struct SwiftStatistics;
  class GlobalConfig;
  typedef RefPtr<SwiftInvocation> SwiftInvocationRef;
  class EditorDiagConsumer;

class ASTUnit : public SourceKit::ThreadSafeRefCountedBase<ASTUnit> {
public:
  struct Implementation;
  Implementation &Impl;

  explicit ASTUnit(uint64_t Generation, std::shared_ptr<SwiftStatistics> Stats);
  ~ASTUnit();

  swift::CompilerInstance &getCompilerInstance() const;
  uint64_t getGeneration() const;
  ArrayRef<ImmutableTextSnapshotRef> getSnapshots() const;
  EditorDiagConsumer &getEditorDiagConsumer() const;
  swift::SourceFile &getPrimarySourceFile() const;

  /// Perform \p Fn asynchronously while preventing concurrent access to the
  /// AST.
  void performAsync(std::function<void()> Fn);
};

typedef IntrusiveRefCntPtr<ASTUnit> ASTUnitRef;

class SwiftASTConsumer : public std::enable_shared_from_this<SwiftASTConsumer> {
  /// Mutex guarding all accesses to \c CancellationRequestCallback and \c
  /// IsCancelled.
  llvm::sys::Mutex CancellationRequestCallbackAndIsCancelledMtx;

  /// A callback that informs the \c ASTBuildOperation, which is producing the
  /// AST for this consumer, that the consumer is no longer of interest. Calling
  /// this callback will eventually call \c cancelled on this consumer.
  /// If the consumer isn't associated with any \c ASTBuildOperation at the
  /// moment (e.g. if it hasn't been scheduled on one yet or if the build
  /// operation has already informed the ASTConsumer), the callback is \c None.
  llvm::Optional<std::function<void(std::shared_ptr<SwiftASTConsumer>)>>
      CancellationRequestCallback;

  bool IsCancelled = false;

public:
  virtual ~SwiftASTConsumer() { }

  // MARK: Cancellation

  /// The result of this consumer is no longer of interest to the SourceKit
  /// client.
  /// This will cause \c cancelled to be called on this \c SwiftASTConsumer and
  /// cause the \c ASTBuildOperation to be cancelled if no other consumer is
  /// depending on it.
  void requestCancellation() {
    llvm::Optional<std::function<void(std::shared_ptr<SwiftASTConsumer>)>>
        CallbackToCall;
    {
      llvm::sys::ScopedLock L(CancellationRequestCallbackAndIsCancelledMtx);
      IsCancelled = true;
      CallbackToCall = CancellationRequestCallback;
      CancellationRequestCallback = llvm::None;
    }
    if (CallbackToCall.has_value()) {
      (*CallbackToCall)(shared_from_this());
    }
  }

  /// Set a cancellation request callback that informs a \c ASTBuildOperation
  /// when this \c SwiftASTConsumer is cancelled. Asserts that there is
  /// currently no callback set.
  /// The cancellation request callback will automatically be removed when the
  /// SwiftASTManager is cancelled.
  /// If this \c SwiftASTConsumer has already been cancelled when this method is
  /// called, \c NewCallback will be called immediately.
  void setCancellationRequestCallback(
      std::function<void(std::shared_ptr<SwiftASTConsumer>)> NewCallback) {
    bool ShouldCallCallback = false;
    {
      llvm::sys::ScopedLock L(CancellationRequestCallbackAndIsCancelledMtx);
      assert(!CancellationRequestCallback.has_value() &&
             "Can't set two cancellation callbacks on a SwiftASTConsumer");
      if (IsCancelled) {
        ShouldCallCallback = true;
      } else {
        CancellationRequestCallback = NewCallback;
      }
    }
    if (ShouldCallCallback) {
      NewCallback(shared_from_this());
    }
  }

  /// Removes the cancellation request callback previously set by \c
  /// setCancellationRequestCallback.
  void removeCancellationRequestCallback() {
    llvm::sys::ScopedLock L(CancellationRequestCallbackAndIsCancelledMtx);
    CancellationRequestCallback = llvm::None;
  }

  // MARK: Result methods
  // Exactly one of these will be called for every consumer.

  /// An AST was produced that the consumer should handle.
  virtual void handlePrimaryAST(ASTUnitRef AstUnit) = 0;

  /// Creation of the AST failed due to \p Error. The request corresponding to
  /// this consumer should fail.
  virtual void failed(StringRef Error);

  /// The consumer was cancelled by the \c requestCancellation method and the \c
  /// ASTBuildOperation creating the AST for this consumer honored the request.
  virtual void cancelled() {}

  // MARK: Use ASTs from older snapshots

  /// Typically a \c SwiftASTConsumer expects an AST for a specific source
  /// state, namely the one on disk when the request was created.
  /// If an AST for a similar source state has already been built, it might be
  /// sufficient to serve this consumer.
  ///
  /// An \c ASTProducer might ask a consumer if it can also handle an AST
  /// constructed from the source state described by \p Snapshots.
  /// If the consumer returns \c true from this method, it should expect to
  /// receive an AST constructed from the source state described by \p
  /// Snapshots. It might need to adjust its internal state (e.g. requested
  /// offset for this new source state).
  virtual bool canUseASTWithSnapshots(
      ArrayRef<ImmutableTextSnapshotRef> Snapshots) {
    return false;
  }
};

typedef std::shared_ptr<SwiftASTConsumer> SwiftASTConsumerRef;
typedef std::weak_ptr<SwiftASTConsumer> SwiftASTConsumerWeakRef;

class SwiftASTManager : public std::enable_shared_from_this<SwiftASTManager> {
public:
  explicit SwiftASTManager(std::shared_ptr<SwiftEditorDocumentFileMap>,
                           std::shared_ptr<GlobalConfig> Config,
                           std::shared_ptr<SwiftStatistics> Stats,
                           std::shared_ptr<RequestTracker> ReqTracker,
                           std::shared_ptr<swift::PluginRegistry> Plugins,
                           StringRef SwiftExecutablePath,
                           StringRef RuntimeResourcePath,
                           StringRef DiagnosticDocumentationPath);
  ~SwiftASTManager();

  SwiftInvocationRef getTypecheckInvocation(ArrayRef<const char *> Args,
                                            StringRef PrimaryFile,
                                            std::string &Error);

  /// Same as the previous `getInvocation`, but allows the caller to specify a
  /// custom `FileSystem` to be used throughout the invocation.
  SwiftInvocationRef getTypecheckInvocation(
      ArrayRef<const char *> Args, StringRef PrimaryFile,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
      std::string &Error);

  /// Provides the AST associated with an invocation to the AST consumer,
  /// asynchronously.
  /// \param OncePerASTToken if non-null, a previous query with the same value
  /// token, that is enqueued waiting to be executed on the same AST, will be
  /// cancelled.
  void
  processASTAsync(SwiftInvocationRef Invok, SwiftASTConsumerRef ASTConsumer,
                  const void *OncePerASTToken,
                  SourceKitCancellationToken CancellationToken,
                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem);

  std::unique_ptr<llvm::MemoryBuffer>
  getMemoryBuffer(StringRef Filename,
                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                  std::string &Error);

  bool initCompilerInvocation(swift::CompilerInvocation &Invocation,
                              ArrayRef<const char *> Args,
                              swift::FrontendOptions::ActionType Action,
                              swift::DiagnosticEngine &Diags,
                              StringRef PrimaryFile, std::string &Error);

  /// Same as the previous `initCompilerInvocation`, but allows the caller to
  /// specify a custom `FileSystem` to be used throughout the invocation.
  bool initCompilerInvocation(
      swift::CompilerInvocation &Invocation, ArrayRef<const char *> Args,
      swift::FrontendOptions::ActionType Action, swift::DiagnosticEngine &Diags,
      StringRef PrimaryFile,
      llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
      std::string &Error);

  bool initCompilerInvocation(swift::CompilerInvocation &CompInvok,
                              ArrayRef<const char *> OrigArgs,
                              swift::FrontendOptions::ActionType Action,
                              StringRef PrimaryFile, std::string &Error);

  /// Initializes \p Invocation as if for typechecking, but with no inputs.
  ///
  /// If \p AllowInputs is false, it is an error for \p OrigArgs to contain any
  /// input files.
  bool initCompilerInvocationNoInputs(swift::CompilerInvocation &Invocation,
                                      ArrayRef<const char *> OrigArgs,
                                      swift::FrontendOptions::ActionType Action,
                                      swift::DiagnosticEngine &Diags,
                                      std::string &Error,
                                      bool AllowInputs = true);

  void removeCachedAST(SwiftInvocationRef Invok);

  struct Implementation;
  Implementation &Impl;
};

typedef std::shared_ptr<SwiftASTManager> SwiftASTManagerRef;

} // namespace SourceKit

#endif
