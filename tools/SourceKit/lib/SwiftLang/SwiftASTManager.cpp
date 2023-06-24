//===--- SwiftASTManager.cpp ----------------------------------------------===//
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

#include "SwiftASTManager.h"
#include "SwiftEditorDiagConsumer.h"
#include "SwiftInvocation.h"
#include "SwiftLangSupport.h"
#include "SourceKit/Core/Context.h"
#include "SourceKit/Support/Concurrency.h"
#include "SourceKit/Support/ImmutableTextBuffer.h"
#include "SourceKit/Support/Logging.h"
#include "SourceKit/Support/Tracing.h"

#include "swift/AST/PluginLoader.h"
#include "swift/Basic/Cache.h"
#include "swift/Driver/FrontendUtil.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/IDETool/CompilerInvocation.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
// This is included only for createLazyResolver(). Move to different header ?
#include "swift/Sema/IDETypeChecking.h"

#include "llvm/ADT/FoldingSet.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"

using namespace SourceKit;
using namespace swift;
using namespace swift::sys;

void SwiftASTConsumer::failed(StringRef Error) { }

//===----------------------------------------------------------------------===//
// SwiftInvocation
//===----------------------------------------------------------------------===//

namespace {

struct InvocationOptions {
  const std::vector<std::string> Args;
  const std::string PrimaryFile;
  const CompilerInvocation Invok;

  InvocationOptions(ArrayRef<const char *> CArgs, StringRef PrimaryFile,
                    CompilerInvocation Invok)
    : Args(_convertArgs(CArgs)),
      PrimaryFile(PrimaryFile),
      Invok(std::move(Invok)) {
    // Assert invocation with a primary file. We want to avoid full typechecking
    // for all files.
    assert(!this->PrimaryFile.empty());
    assert(this->Invok.getFrontendOptions()
               .InputsAndOutputs.hasUniquePrimaryInput() &&
           "Must have exactly one primary input for code completion, etc.");
  }

  void applyTo(CompilerInvocation &CompInvok) const;
  void
  applyToSubstitutingInputs(CompilerInvocation &CompInvok,
                            FrontendInputsAndOutputs &&InputsAndOutputs) const;
  void profile(llvm::FoldingSetNodeID &ID) const;
  void raw(std::vector<std::string> &Args, std::string &PrimaryFile) const;

private:
  static std::vector<std::string> _convertArgs(ArrayRef<const char *> CArgs) {
    std::vector<std::string> Args;
    Args.reserve(CArgs.size());
    for (auto Arg : CArgs)
      Args.push_back(Arg);
    return Args;
  }
};

struct ASTKey {
  llvm::FoldingSetNodeID FSID;
};

template <typename T>
size_t getVectorMemoryCost(const std::vector<T> &Vec) {
  return Vec.capacity() * sizeof(T);
}

} // end anonymous namespace

struct SwiftInvocation::Implementation {
  InvocationOptions Opts;
  ASTKey Key;

  explicit Implementation(InvocationOptions opts) : Opts(std::move(opts)) {
    Opts.profile(Key.FSID);
  }
};

SwiftInvocation::~SwiftInvocation() {
  delete &Impl;
}

ArrayRef<std::string> SwiftInvocation::getArgs() const {
  return ArrayRef(Impl.Opts.Args);
}

void SwiftInvocation::applyTo(swift::CompilerInvocation &CompInvok) const {
  return Impl.Opts.applyTo(CompInvok);
}

void SwiftInvocation::raw(std::vector<std::string> &Args,
                          std::string &PrimaryFile) const {
  return Impl.Opts.raw(Args, PrimaryFile);
}

void InvocationOptions::applyTo(CompilerInvocation &CompInvok) const {
  CompInvok = this->Invok;
}
void InvocationOptions::applyToSubstitutingInputs(
    CompilerInvocation &CompInvok,
    FrontendInputsAndOutputs &&inputsAndOutputs) const {
  CompInvok = this->Invok;
  CompInvok.getFrontendOptions().InputsAndOutputs = inputsAndOutputs;
}

void InvocationOptions::raw(std::vector<std::string> &Args,
                            std::string &PrimaryFile) const {
  Args.assign(this->Args.begin(), this->Args.end());
  PrimaryFile = this->PrimaryFile;
}

void InvocationOptions::profile(llvm::FoldingSetNodeID &ID) const {
  // FIXME: This ties ASTs to every argument and the exact order that they were
  // provided, preventing much sharing of ASTs.
  // Note though that previously we tried targeting specific options considered
  // semantically relevant but it proved too fragile (very easy to miss some new
  // compiler invocation option).
  // Possibly have all compiler invocation options auto-generated from a
  // tablegen definition file, thus forcing a decision for each option if it is
  // ok to share ASTs with the option differing.
  for (auto &Arg : Args)
    ID.AddString(Arg);
  ID.AddString(PrimaryFile);
}

//===----------------------------------------------------------------------===//
// SwiftASTManager
//===----------------------------------------------------------------------===//

namespace SourceKit {
  struct ASTUnit::Implementation {
    const uint64_t Generation;
    std::shared_ptr<SwiftStatistics> Stats;
    SmallVector<ImmutableTextSnapshotRef, 4> Snapshots;
    EditorDiagConsumer CollectDiagConsumer;
    CompilerInstance CompInst;
    WorkQueue Queue{ WorkQueue::Dequeuing::Serial, "sourcekit.swift.ConsumeAST" };

    Implementation(uint64_t Generation, std::shared_ptr<SwiftStatistics> Stats)
        : Generation(Generation), Stats(Stats) {}

    void consumeAsync(SwiftASTConsumerRef ASTConsumer, ASTUnitRef ASTRef);
  };

  void ASTUnit::Implementation::consumeAsync(SwiftASTConsumerRef ConsumerRef,
                                             ASTUnitRef ASTRef) {
#if defined(_WIN32)
	// Windows uses more up for stack space (why?) than macOS/Linux which
	// causes stack overflows in a dispatch thread with 64k stack. Passing
	// useDeepStack=true means it's given a _beginthreadex thread with an 8MB
	// stack.
	bool useDeepStack = true;
#else
	bool useDeepStack = false;
#endif
    Queue.dispatch([ASTRef, ConsumerRef]{
      SwiftASTConsumer &ASTConsumer = *ConsumerRef;

      CompilerInstance &CI = ASTRef->getCompilerInstance();

      if (CI.getPrimarySourceFile()) {
        ASTConsumer.handlePrimaryAST(ASTRef);
      } else {
        LOG_WARN_FUNC("did not find primary SourceFile");
        ConsumerRef->failed("did not find primary SourceFile");
      }
    }, useDeepStack);
  }

  ASTUnit::ASTUnit(uint64_t Generation, std::shared_ptr<SwiftStatistics> Stats)
      : Impl(*new Implementation(Generation, Stats)) {
    auto numASTs = ++Stats->numASTsInMem;
    Stats->maxASTsInMem.updateMax(numASTs);
  }

  ASTUnit::~ASTUnit() {
    --Impl.Stats->numASTsInMem;
    delete &Impl;
  }

  swift::CompilerInstance &ASTUnit::getCompilerInstance() const {
    return Impl.CompInst;
  }

   uint64_t ASTUnit::getGeneration() const {
    return Impl.Generation;
  }

  ArrayRef<ImmutableTextSnapshotRef> ASTUnit::getSnapshots() const {
    return Impl.Snapshots;
  }

  SourceFile &ASTUnit::getPrimarySourceFile() const {
    return *Impl.CompInst.getPrimarySourceFile();
  }

  EditorDiagConsumer &ASTUnit::getEditorDiagConsumer() const {
    return Impl.CollectDiagConsumer;
  }

  void ASTUnit::performAsync(std::function<void()> Fn) {
    Impl.Queue.dispatch(std::move(Fn));
  }
} // namespace SourceKit

namespace {

typedef uint64_t BufferStamp;

struct FileContent {
  ImmutableTextSnapshotRef Snapshot;
  std::string Filename;
  std::unique_ptr<llvm::MemoryBuffer> Buffer;
  bool IsPrimary;
  BufferStamp Stamp;

  FileContent(ImmutableTextSnapshotRef Snapshot, std::string Filename,
              std::unique_ptr<llvm::MemoryBuffer> Buffer, bool IsPrimary,
              BufferStamp Stamp)
      : Snapshot(std::move(Snapshot)), Filename(Filename),
        Buffer(std::move(Buffer)), IsPrimary(IsPrimary), Stamp(Stamp) {}

  explicit operator InputFile() const {
    return InputFile(Filename, IsPrimary, Buffer.get());
  }

  size_t getMemoryCost() const {
    return sizeof(*this) + Filename.size() + Buffer->getBufferSize();
  }
};

/// An \c ASTBuildOperations builds an AST. Once the AST is built, it informs
/// a list of \c SwiftASTConsumers about the built AST.
/// It also supports cancellation with the following paradigm: If an \c
/// SwiftASTConsumer is no longer needed, it can be cancelled, which will remove
/// it from the \c ASTBuildOperation. If the \c ASTBuildOperation has no more
/// consumers attached to it, it will cancel the AST build at the next
/// opportunity.
class ASTBuildOperation
    : public std::enable_shared_from_this<ASTBuildOperation> {
  /// After the AST has been built, the corresponding result.
  struct ASTBuildResult {
    /// The AST that was created by the build operation.
    ASTUnitRef AST;
    /// An error message emitted by the creation of the AST. There might still
    /// be an AST if an error occurred, but it's usefulness depends on the
    /// severity of the error.
    std::string Error;
    /// Whether the build operation was cancelled. There might be an AST and
    /// error but their usefulness depends on when the operation was cancelled.
    bool Cancelled;
    /// Whether the result contains any values, i.e. whether the operation has
    /// produced a result yet.
    bool HasValue;

    ASTBuildResult() : HasValue(false) {}

    void emplace(ASTUnitRef AST, std::string Error, bool Cancelled) {
      assert(!HasValue && "Should only emplace a result once");
      this->HasValue = true;
      this->AST = AST;
      this->Error = Error;
      this->Cancelled = Cancelled;
    }

    operator bool() const { return HasValue; }

    size_t getMemoryCost() {
      size_t Cost = sizeof(*this) + Error.size();
      if (AST) {
        Cost += sizeof(*AST);
        if (AST->getCompilerInstance().hasASTContext()) {
          Cost += AST->Impl.CompInst.getASTContext().getTotalMemory();
        }
      }
      return Cost;
    }
  };

  /// Parameters necessary to build the AST.
  const SwiftInvocationRef InvokRef;
  const IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem;

  /// The contents of all explicit input files of the compiler innovation, which
  /// can be determined at construction time of the \c ASTBuildOperation.
  const std::vector<FileContent> FileContents;

  /// Guards \c DependencyStamps. This prevents reading from \c DependencyStamps
  /// while it is being modified. It does not provide any ordering gurantees
  /// that \c DependencyStamps have been computed in \c buildASTUnit before they
  /// are accessed in \c matchesSourceState but that's fine (see comment on
  /// \c DependencyStamps).
  llvm::sys::Mutex DependencyStampsMtx;

  /// \c DependencyStamps contains the stamps of all module dependencies needed
  /// for the AST build. These stamps are only known after the AST is built.
  /// Before the AST has been built, we thus assume that all dependency stamps
  /// match. This seems to be a reasonable assumption since the dependencies
  /// shouldn't change (much) in the time between an \c ASTBuildOperation is
  /// created and until it produced an AST.
  /// Must only be accessed if \c DependencyStampsMtx has been claimed.
  SmallVector<std::pair<std::string, BufferStamp>, 8> DependencyStamps = {};

  /// The ASTManager from which this operation got scheduled. Used to update
  /// global stats and access the file system.
  SwiftASTManagerRef ASTManager;

  /// A flag to cancel the AST build. If this flag is set to \c true, the type
  /// checker will cancel type checking at the next possible opportunity.
  const std::shared_ptr<std::atomic<bool>> CancellationFlag =
      std::make_shared<std::atomic<bool>>(false);

  /// A callback that's called when the operation finishes. Used to remove it
  /// from the \c ASTProducer that scheduled it.
  const std::function<void(void)> DidFinishCallback;

  /// The consumers and result are guarded by the same mutex to avoid
  /// simultaneously adding a consumer and setting the result, which might cause
  /// the consumer's callback to neither be called when it gets added to this
  /// operation, nor when the operation finishes.
  llvm::sys::Mutex ConsumersAndResultMtx;

  /// The consumers that should be informed about this AST once it finishes
  /// building. When this vector is empty, the AST build can be cancelled.
  SmallVector<SwiftASTConsumerRef, 4> Consumers = {};

  /// Once the build operation has finished, its result, which can be an AST, an
  /// error or the fact that it has been cancelled.
  ASTBuildResult Result;

  enum class State { Created, Queued, Running, Finished };

  /// The state the operation is in. Only used in assertions to verify no state
  /// is skipped or executed twice.
  State OperationState = State::Created;

  /// Inform a consumer that the AST has been built or that the build failed
  /// with an error.
  void informConsumer(SwiftASTConsumerRef Consumer);

  /// Actually build the AST unit, synchronously on the current thread. If an
  /// error occurred during the build, \p Error will contain the message. In
  /// case of an error, a non-null AST may still be returned. Its usefulness
  /// depends on the severity of the error.
  ASTUnitRef buildASTUnit(std::string &Error);

  /// Transition the build operation to \p NewState, asserting that the current
  /// state is \p ExpectedOldState.
  void transitionToState(State NewState, State ExpectedOldState) {
    assert(OperationState == ExpectedOldState);
    OperationState = NewState;
  }

  /// Create a vector of \c FileContents containing all files explicitly
  /// referenced by the compiler invocation.
  std::vector<FileContent> fileContentsForFilesInCompilerInvocation();

public:
  ASTBuildOperation(IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                    SwiftInvocationRef InvokRef, SwiftASTManagerRef ASTManager,
                    std::function<void(void)> DidFinishCallback)
      : InvokRef(InvokRef), FileSystem(FileSystem), ASTManager(ASTManager),
        DidFinishCallback(DidFinishCallback) {
    // const_cast is fine here. We just want to guard against modifying these
    // fields later on. It's fine to set them in the constructor.
    const_cast<std::vector<FileContent> &>(this->FileContents) =
        fileContentsForFilesInCompilerInvocation();
  }

  ~ASTBuildOperation() {
    assert(OperationState == State::Finished &&
           "ASTBuildOperations should only be destructed once they have "
           "produced an AST or are finished. Otherwise, some consumers might "
           "not receive their callback.");
  }

  ArrayRef<FileContent> getFileContents() const { return FileContents; }

  /// Returns true if the build operation has finished.
  bool isFinished() {
    llvm::sys::ScopedLock L(ConsumersAndResultMtx);
    return Result.HasValue;
  }

  bool isCancelled() {
    llvm::sys::ScopedLock L(ConsumersAndResultMtx);
    return (Result.HasValue && Result.Cancelled) ||
           CancellationFlag->load(std::memory_order_relaxed);
  }

  size_t getMemoryCost() {
    size_t Cost = sizeof(*this) + getVectorMemoryCost(FileContents) +
                  Result.getMemoryCost();
    for (const FileContent &File : FileContents) {
      Cost += File.getMemoryCost();
    }
    return Cost;
  }

  /// Schedule building this AST on the given \p Queue.
  void schedule(WorkQueue Queue);

  /// Inform the given \p Consumer when the AST has been built. If the build
  /// operation has already built the AST, the consumer is directly informed.
  /// Returns \c true if the \p Consumer was added. Returns \c false if the
  /// operation has already been cancelled, in which case the consumer should be
  /// scheduled on a different build operation. This ensures that we don't hit
  /// a race condition when a  build operation gets cancelled in between when it
  /// gets selected as a viable candidate but before the consumer gets added to
  /// it.
  bool addConsumer(SwiftASTConsumerRef Consumer);

  /// Determines whether the AST built from this build operation can be used for
  /// the given source state. Note that before the AST is built, this does not
  /// consider dependencies needed for the AST build that are not explicitly
  /// listed in the input files. As such, this might be \c true before the AST
  /// build and \c false after the AST has been built. See documentation on \c
  /// DependencyStamps for more info.
  bool matchesSourceState(IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem);

  /// Called when a consumer is cancelled. This calls \c cancelled on the
  /// consumer, removes it from the \c Consumers severed by this build operation
  /// and, if no consumers are left, cancels the AST build of this operation.
  void requestConsumerCancellation(SwiftASTConsumerRef Consumer);
};

using ASTBuildOperationRef = std::shared_ptr<ASTBuildOperation>;

/// An \c ASTProducer produces ASTs for a given compiler invocation through
/// multiple \c ASTBuildOperations.
/// While \c ASTBuildOperations only build ASTs for a single snapshot, \c
/// ASTProducer also keeps track of ASTs built from different (older) snapshots.
/// It is thus able to serve an \c SwiftASTConsumer with an AST from an older
/// snapshot, should it accept it by returning \c true in \c
/// canUseASTWithSnapshots.
class ASTProducer : public std::enable_shared_from_this<ASTProducer> {
  SwiftInvocationRef InvokRef;

  /// The build operations that have been scheduled by this producer. Some of
  /// these operations might already have finished, effectively caching an old
  /// AST, one might currently be building an AST and some might be waiting to
  /// execute. Operations are guaranteed to be in FIFO order, that is the first
  /// one in the vector is the oldest build operation.
  SmallVector<ASTBuildOperationRef, 4> BuildOperations = {};
  WorkQueue BuildOperationsQueue = WorkQueue(
      WorkQueue::Dequeuing::Serial, "ASTProducer.BuildOperationsQueue");

  /// Erase all finished build operations with a result except for the latest
  /// one which contains a successful results.
  /// This cleans up all stale build operations (probably containing old ASTs),
  /// but keeps the latest AST around, so that new consumers can be served from
  /// it, if possible.
  ///
  /// Must be executed on \c BuildOperationsQueue.
  void cleanBuildOperations() {
    auto ReverseOperations = llvm::reverse(BuildOperations);
    auto LastOperationWithResultIt =
        llvm::find_if(ReverseOperations, [](ASTBuildOperationRef BuildOp) {
          return BuildOp->isFinished() && !BuildOp->isCancelled();
        });
    ASTBuildOperationRef LastOperationWithResult = nullptr;
    if (LastOperationWithResultIt != ReverseOperations.end()) {
      LastOperationWithResult = *LastOperationWithResultIt;
    }
    llvm::erase_if(BuildOperations, [LastOperationWithResult](
                                        ASTBuildOperationRef BuildOp) {
      return BuildOp->isFinished() && BuildOp != LastOperationWithResult;
    });
  }

  /// Returns the latest build operation which can serve the \p Consumer or
  /// \c nullptr if no such build operation exists.
  ///
  /// Must be executed on \c BuildOperationsQueue.
  ASTBuildOperationRef getBuildOperationForConsumer(
      SwiftASTConsumerRef Consumer,
      IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
      SwiftASTManagerRef Mgr);

public:
  explicit ASTProducer(SwiftInvocationRef InvokRef)
      : InvokRef(std::move(InvokRef)) {}

  /// Schedules the given \p Consumer to the latest suitable build operation.
  /// Independently of what happens, the consumer will receive either a \c
  /// cancelled, \c failed or \c handlePrimaryAST callback.
  void enqueueConsumer(SwiftASTConsumerRef Consumer,
                       IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                       SwiftASTManagerRef Mgr);

  size_t getMemoryCost() const {
    size_t Cost = sizeof(*this);
    for (auto &BuildOp : BuildOperations) {
      Cost += BuildOp->getMemoryCost();
    }
    return Cost;
  }
};

typedef std::shared_ptr<ASTProducer> ASTProducerRef;

} // end anonymous namespace

namespace swift {
namespace sys {

template <>
struct CacheValueCostInfo<ASTProducer> {
  static size_t getCost(const ASTProducer &Unit) {
    return Unit.getMemoryCost();
  }
};

template <>
struct CacheKeyHashInfo<ASTKey> {
  static uintptr_t getHashValue(const ASTKey &Key) {
    return Key.FSID.ComputeHash();
  }
  static bool isEqual(void *LHS, void *RHS) {
    return static_cast<ASTKey*>(LHS)->FSID == static_cast<ASTKey*>(RHS)->FSID;
  }
};

} // namespace sys
} // namespace swift

struct SwiftASTManager::Implementation {
  explicit Implementation(
      std::shared_ptr<SwiftEditorDocumentFileMap> EditorDocs,
      std::shared_ptr<GlobalConfig> Config,
      std::shared_ptr<SwiftStatistics> Stats,
      std::shared_ptr<RequestTracker> ReqTracker,
      std::shared_ptr<PluginRegistry> Plugins, StringRef SwiftExecutablePath,
      StringRef RuntimeResourcePath, StringRef DiagnosticDocumentationPath)
      : EditorDocs(EditorDocs), Config(Config), Stats(Stats),
        ReqTracker(ReqTracker), Plugins(Plugins),
        SwiftExecutablePath(SwiftExecutablePath),
        RuntimeResourcePath(RuntimeResourcePath),
        DiagnosticDocumentationPath(DiagnosticDocumentationPath),
        SessionTimestamp(llvm::sys::toTimeT(std::chrono::system_clock::now())) {
  }

  std::shared_ptr<SwiftEditorDocumentFileMap> EditorDocs;
  std::shared_ptr<GlobalConfig> Config;
  std::shared_ptr<SwiftStatistics> Stats;
  std::shared_ptr<RequestTracker> ReqTracker;
  std::shared_ptr<PluginRegistry> Plugins;
  /// The path of the swift-frontend executable.
  /// Used to find clang relative to it.
  std::string SwiftExecutablePath;
  std::string RuntimeResourcePath;
  std::string DiagnosticDocumentationPath;
  SourceManager SourceMgr;
  Cache<ASTKey, ASTProducerRef> ASTCache{ "sourcekit.swift.ASTCache" };
  llvm::sys::Mutex CacheMtx;
  std::time_t SessionTimestamp;

  /// A consumer that has been scheduled using \c processASTAsync.
  /// The \c OncePerASTToken allows us to cancel previously scheduled consumers
  /// if a new request/consumer with the same \c OncePerASTToken comes in.
  /// Since we only keep a reference to the consumers to cancel them, the
  /// reference to the consumer itself is weak - if it's already deallocated,
  /// there is no need to cancel it anymore.
  /// The \c CancellationToken that allows cancellation of this consumer.
  /// Multiple consumers might share the same \c CancellationToken if they were
  /// created from the same SourceKit request. E.g. a \c CursorInfoConsumer
  /// might schedule a second \c CursorInfoConsumer if it discovers that the AST
  /// that was used to serve the first request is not up-to-date enough.
  /// If \c CancellationToken is \c nullptr, the consumer can't be cancelled
  /// using a cancellation token.
  struct ScheduledConsumer {
    SwiftASTConsumerWeakRef Consumer;
    const void *OncePerASTToken;
  };

  /// FIXME: Once we no longer support implicit cancellation using
  /// OncePerASTToken, we can stop keeping track of ScheduledConsumers and
  /// completely rely on RequestTracker for cancellation.
  llvm::sys::Mutex ScheduledConsumersMtx;
  std::vector<ScheduledConsumer> ScheduledConsumers;

  /// Queue guaranteeing that only one \c ASTBuildOperation builds an AST at a
  /// time.
  WorkQueue ASTBuildQueue{ WorkQueue::Dequeuing::Serial,
                           "sourcekit.swift.ASTBuilding" };

  /// Queue on which consumers may be notified about results and cancellation.
  /// This is essentially just a background queue to which we can jump to inform
  /// consumers while making sure that no locks are currently claimed.
  WorkQueue ConsumerNotificationQueue{
      WorkQueue::Dequeuing::Concurrent,
      "SwiftASTManager::Implementation::ConsumerNotificationQueue"};

  /// Remove all scheduled consumers that don't exist anymore. This is just a
  /// garbage-collection operation to make sure the \c ScheduledConsumers vector
  /// doesn't explode. One should never make assumptions that all consumers in
  /// \c ScheduledConsumers are alive.
  void cleanDeletedConsumers() {
    llvm::sys::ScopedLock L(ScheduledConsumersMtx);
    llvm::erase_if(ScheduledConsumers, [](ScheduledConsumer Consumer) {
      return Consumer.Consumer.expired();
    });
  }

  ASTProducerRef getASTProducer(SwiftInvocationRef InvokRef);

  FileContent
  getFileContent(StringRef FilePath, bool IsPrimary,
                 IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                 std::string &Error) const;

  BufferStamp
  getBufferStamp(StringRef FilePath,
                 IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                 bool CheckEditorDocs = true) const;

  std::unique_ptr<llvm::MemoryBuffer>
  getMemoryBuffer(StringRef Filename,
                  IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
                  std::string &Error) const;
};

SwiftASTManager::SwiftASTManager(
    std::shared_ptr<SwiftEditorDocumentFileMap> EditorDocs,
    std::shared_ptr<GlobalConfig> Config,
    std::shared_ptr<SwiftStatistics> Stats,
    std::shared_ptr<RequestTracker> ReqTracker,
    std::shared_ptr<PluginRegistry> Plugins, StringRef SwiftExecutablePath,
    StringRef RuntimeResourcePath, StringRef DiagnosticDocumentationPath)
    : Impl(*new Implementation(EditorDocs, Config, Stats, ReqTracker, Plugins,
                               SwiftExecutablePath, RuntimeResourcePath,
                               DiagnosticDocumentationPath)) {}

SwiftASTManager::~SwiftASTManager() {
  delete &Impl;
}

std::unique_ptr<llvm::MemoryBuffer> SwiftASTManager::getMemoryBuffer(
    StringRef Filename,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) {
  return Impl.getFileContent(Filename, /*IsPrimary=*/false, FileSystem, Error)
      .Buffer;
}

static FrontendInputsAndOutputs
convertFileContentsToInputs(ArrayRef<FileContent> contents) {
  FrontendInputsAndOutputs inputsAndOutputs;
  for (const FileContent &content : contents)
    inputsAndOutputs.addInput(InputFile(content));
  return inputsAndOutputs;
}

bool SwiftASTManager::initCompilerInvocation(
    CompilerInvocation &Invocation, ArrayRef<const char *> OrigArgs,
    swift::FrontendOptions::ActionType Action, DiagnosticEngine &Diags,
    StringRef UnresolvedPrimaryFile, std::string &Error) {
  return initCompilerInvocation(Invocation, OrigArgs, Action, Diags,
                                UnresolvedPrimaryFile,
                                llvm::vfs::getRealFileSystem(), Error);
}

bool SwiftASTManager::initCompilerInvocation(
    CompilerInvocation &Invocation, ArrayRef<const char *> OrigArgs,
    FrontendOptions::ActionType Action, DiagnosticEngine &Diags,
    StringRef UnresolvedPrimaryFile,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) {
  return ide::initCompilerInvocation(
      Invocation, OrigArgs, Action, Diags, UnresolvedPrimaryFile, FileSystem,
      Impl.SwiftExecutablePath, Impl.RuntimeResourcePath,
      Impl.DiagnosticDocumentationPath, Impl.SessionTimestamp, Error);
}

bool SwiftASTManager::initCompilerInvocation(
    CompilerInvocation &CompInvok, ArrayRef<const char *> OrigArgs,
    swift::FrontendOptions::ActionType Action, StringRef PrimaryFile,
    std::string &Error) {
  DiagnosticEngine Diagnostics(Impl.SourceMgr);
  return initCompilerInvocation(CompInvok, OrigArgs, Action, Diagnostics,
                                PrimaryFile, Error);
}

bool SwiftASTManager::initCompilerInvocationNoInputs(
    swift::CompilerInvocation &Invocation, ArrayRef<const char *> OrigArgs,
    swift::FrontendOptions::ActionType Action, swift::DiagnosticEngine &Diags,
    std::string &Error, bool AllowInputs) {

  SmallVector<const char *, 16> Args(OrigArgs.begin(), OrigArgs.end());
  // Use stdin as a .swift input to satisfy the driver.
  Args.push_back("-");
  if (initCompilerInvocation(Invocation, Args, Action, Diags, "", Error))
    return true;

  if (!AllowInputs &&
      Invocation.getFrontendOptions().InputsAndOutputs.inputCount() > 1) {
    Error = "unexpected input in compiler arguments";
    return true;
  }

  // Clear the inputs.
  Invocation.getFrontendOptions().InputsAndOutputs.clearInputs();
  return false;
}

SwiftInvocationRef
SwiftASTManager::getTypecheckInvocation(ArrayRef<const char *> OrigArgs,
                                        StringRef PrimaryFile,
                                        std::string &Error) {
  return getTypecheckInvocation(OrigArgs, PrimaryFile,
                                llvm::vfs::getRealFileSystem(), Error);
}

SwiftInvocationRef SwiftASTManager::getTypecheckInvocation(
    ArrayRef<const char *> OrigArgs, StringRef PrimaryFile,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) {
  assert(FileSystem);

  DiagnosticEngine Diags(Impl.SourceMgr);
  EditorDiagConsumer CollectDiagConsumer;
  Diags.addConsumer(CollectDiagConsumer);

  CompilerInvocation CompInvok;
  if (initCompilerInvocation(CompInvok, OrigArgs,
                             FrontendOptions::ActionType::Typecheck, Diags,
                             PrimaryFile, FileSystem, Error)) {
    // We create a traced operation here to represent the failure to parse
    // arguments since we cannot reach `createAST` where that would normally
    // happen.
    trace::TracedOperation TracedOp(trace::OperationKind::PerformSema);
    if (TracedOp.enabled()) {
      trace::SwiftInvocation TraceInfo;
      trace::initTraceInfo(TraceInfo, PrimaryFile, OrigArgs);
      TracedOp.setDiagnosticProvider(
        [&CollectDiagConsumer](SmallVectorImpl<DiagnosticEntryInfo> &diags) {
          CollectDiagConsumer.getAllDiagnostics(diags);
        });
      TracedOp.start(TraceInfo);
    }
    return nullptr;
  }

  InvocationOptions Opts(OrigArgs, PrimaryFile, CompInvok);
  return new SwiftInvocation(
      *new SwiftInvocation::Implementation(std::move(Opts)));
}

void SwiftASTManager::processASTAsync(
    SwiftInvocationRef InvokRef, SwiftASTConsumerRef ASTConsumer,
    const void *OncePerASTToken, SourceKitCancellationToken CancellationToken,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem) {
  assert(fileSystem);
  ASTProducerRef Producer = Impl.getASTProducer(InvokRef);

  Impl.cleanDeletedConsumers();
  {
    llvm::sys::ScopedLock L(Impl.ScheduledConsumersMtx);
    if (OncePerASTToken) {
      // Cancel any consumers with the same OncePerASTToken.
      for (auto ScheduledConsumer : Impl.ScheduledConsumers) {
        if (ScheduledConsumer.OncePerASTToken == OncePerASTToken) {
          Impl.ConsumerNotificationQueue.dispatch([ScheduledConsumer]() {
            if (auto Consumer = ScheduledConsumer.Consumer.lock()) {
              Consumer->requestCancellation();
            }
          });
        }
      }
    }
    Impl.ScheduledConsumers.push_back({ASTConsumer, OncePerASTToken});
  }

  Producer->enqueueConsumer(ASTConsumer, fileSystem, shared_from_this());

  auto WeakConsumer = SwiftASTConsumerWeakRef(ASTConsumer);
  auto WeakThis = std::weak_ptr<SwiftASTManager>(shared_from_this());
  Impl.ReqTracker->setCancellationHandler(
      CancellationToken, [WeakConsumer, WeakThis] {
        if (auto This = WeakThis.lock()) {
          This->Impl.ConsumerNotificationQueue.dispatch([WeakConsumer]() {
            if (auto Consumer = WeakConsumer.lock()) {
              Consumer->requestCancellation();
            }
          });
        }
      });
}

void SwiftASTManager::removeCachedAST(SwiftInvocationRef Invok) {
  Impl.ASTCache.remove(Invok->Impl.Key);
}

ASTProducerRef
SwiftASTManager::Implementation::getASTProducer(SwiftInvocationRef InvokRef) {
  llvm::sys::ScopedLock L(CacheMtx);
  llvm::Optional<ASTProducerRef> OptProducer = ASTCache.get(InvokRef->Impl.Key);
  if (OptProducer.has_value())
    return OptProducer.value();
  ASTProducerRef Producer = std::make_shared<ASTProducer>(InvokRef);
  ASTCache.set(InvokRef->Impl.Key, Producer);
  return Producer;
}

static FileContent getFileContentFromSnap(ImmutableTextSnapshotRef Snap,
                                          bool IsPrimary, StringRef FilePath) {
  auto Buf = llvm::MemoryBuffer::getMemBufferCopy(
      Snap->getBuffer()->getText(), FilePath);
  return FileContent(Snap, FilePath.str(), std::move(Buf), IsPrimary,
                     Snap->getStamp());
}

FileContent SwiftASTManager::Implementation::getFileContent(
    StringRef UnresolvedPath, bool IsPrimary,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) const {
  std::string FilePath = SwiftLangSupport::resolvePathSymlinks(UnresolvedPath);
  if (auto EditorDoc = EditorDocs->findByPath(FilePath, /*IsRealpath=*/true))
    return getFileContentFromSnap(EditorDoc->getLatestSnapshot(), IsPrimary,
                                  FilePath);

  // FIXME: Is there a way to get timestamp and buffer for a file atomically ?
  // No need to check EditorDocs again. We did so above.
  auto Stamp = getBufferStamp(FilePath, FileSystem, /*CheckEditorDocs=*/false);
  auto Buffer = getMemoryBuffer(FilePath, FileSystem, Error);
  return FileContent(nullptr, UnresolvedPath.str(), std::move(Buffer),
                     IsPrimary, Stamp);
}

BufferStamp SwiftASTManager::Implementation::getBufferStamp(
    StringRef FilePath,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    bool CheckEditorDocs) const {
  assert(FileSystem);

  if (CheckEditorDocs) {
    if (auto EditorDoc = EditorDocs->findByPath(FilePath)) {
      return EditorDoc->getLatestSnapshot()->getStamp();
    }
  }

  auto StatusOrErr = FileSystem->status(FilePath);
  if (std::error_code Err = StatusOrErr.getError()) {
    // Failure to read the file.
    LOG_WARN_FUNC("failed to stat file: " << FilePath << " (" << Err.message()
                                          << ')');
    return -1;
  }
  return StatusOrErr.get().getLastModificationTime().time_since_epoch().count();
}

std::unique_ptr<llvm::MemoryBuffer>
SwiftASTManager::Implementation::getMemoryBuffer(
    StringRef Filename,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    std::string &Error) const {
  assert(FileSystem);
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      FileSystem->getBufferForFile(Filename);
  if (FileBufOrErr)
    return std::move(FileBufOrErr.get());

  llvm::raw_string_ostream OSErr(Error);
  OSErr << "error opening input file '" << Filename << "' ("
        << FileBufOrErr.getError().message() << ')';
  return nullptr;
}

std::vector<FileContent>
ASTBuildOperation::fileContentsForFilesInCompilerInvocation() {
  const InvocationOptions &Opts = InvokRef->Impl.Opts;
  std::string Error; // is ignored

  std::vector<FileContent> FileContents;
  FileContents.reserve(
      Opts.Invok.getFrontendOptions().InputsAndOutputs.inputCount());

  // IMPORTANT: The computation of stamps must match the one in
  // matchesSourceState.
  for (const auto &input :
       Opts.Invok.getFrontendOptions().InputsAndOutputs.getAllInputs()) {
    const std::string &Filename = input.getFileName();
    bool IsPrimary = input.isPrimary();
    auto Content =
        ASTManager->Impl.getFileContent(Filename, IsPrimary, FileSystem, Error);
    if (!Content.Buffer) {
      LOG_WARN_FUNC("failed getting file contents for " << Filename << ": "
                                                        << Error);
      // File may not exist, continue and recover as if it was empty.
      Content.Buffer = llvm::WritableMemoryBuffer::getNewMemBuffer(0, Filename);
    }
    FileContents.push_back(std::move(Content));
  }
  assert(FileContents.size() ==
         Opts.Invok.getFrontendOptions().InputsAndOutputs.inputCount());
  return FileContents;
}

bool ASTBuildOperation::matchesSourceState(
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> OtherFileSystem) {
  const InvocationOptions &Opts = InvokRef->Impl.Opts;

  auto Inputs = Opts.Invok.getFrontendOptions().InputsAndOutputs.getAllInputs();
  for (size_t I = 0; I < Inputs.size(); I++) {
    if (getFileContents()[I].Stamp !=
        ASTManager->Impl.getBufferStamp(Inputs[I].getFileName(),
                                        OtherFileSystem)) {
      return false;
    }
  }

  llvm::sys::ScopedLock L(DependencyStampsMtx);

  for (auto &Dependency : DependencyStamps) {
    if (Dependency.second !=
        ASTManager->Impl.getBufferStamp(Dependency.first, OtherFileSystem))
      return false;
  }

  return true;
}

void ASTBuildOperation::requestConsumerCancellation(
    SwiftASTConsumerRef Consumer) {
  llvm::sys::ScopedLock L(ConsumersAndResultMtx);
  // No need to check if we have already called the consumer here, because it
  // is removed from `Consumers` if it's informed about a result from
  // `schedule()`.
  auto ConsumerIndex = llvm::find_if(
      Consumers, [&Consumer](SwiftASTConsumerRef ConsumerInQueue) {
        return ConsumerInQueue == Consumer;
      });
  if (ConsumerIndex == Consumers.end()) {
    // Consumer no longer tracked by this build operation. Did it finish
    // already?
    return;
  }
  Consumers.erase(ConsumerIndex);
  if (Consumers.empty()) {
    // If there are no more consumers waiting for this result, cancel the AST
    // build.
    CancellationFlag->store(true, std::memory_order_relaxed);
  }
  ASTManager->Impl.ConsumerNotificationQueue.dispatch([Consumer] {
    Consumer->cancelled();
  });
}

static void collectModuleDependencies(ModuleDecl *TopMod,
    llvm::SmallPtrSetImpl<ModuleDecl *> &Visited,
    SmallVectorImpl<std::string> &Filenames) {

  if (!TopMod)
    return;

  auto ClangModuleLoader = TopMod->getASTContext().getClangModuleLoader();

  ModuleDecl::ImportFilter ImportFilter = {
      ModuleDecl::ImportFilterKind::Exported,
      ModuleDecl::ImportFilterKind::Default};
  if (Visited.empty()) {
    // Only collect implementation-only dependencies from the main module.
    ImportFilter |= ModuleDecl::ImportFilterKind::ImplementationOnly;
  }
  // FIXME: ImportFilterKind::ShadowedByCrossImportOverlay?
  SmallVector<ImportedModule, 8> Imports;
  TopMod->getImportedModules(Imports, ImportFilter);

  for (auto Import : Imports) {
    ModuleDecl *Mod = Import.importedModule;
    if (Mod->isSystemModule())
      continue;
    // FIXME: Setup dependencies on the included headers.
    if (ClangModuleLoader &&
        Mod == ClangModuleLoader->getImportedHeaderModule())
      continue;
    bool NewVisit = Visited.insert(Mod).second;
    if (!NewVisit)
      continue;

    // FIXME: Handle modules with multiple source files; these will fail on
    // getModuleFilename() (by returning an empty path). Note that such modules
    // may be heterogeneous.
    {
      std::string Path = Mod->getModuleFilename().str();
      if (Path.empty() || Path == TopMod->getModuleFilename())
        continue; // this is a submodule.
      Filenames.push_back(std::move(Path));
    }

    bool IsClangModule = false;
    for (auto File : Mod->getFiles()) {
      if (File->getKind() == FileUnitKind::ClangModule) {
        IsClangModule = true;
        break;
      }
    }
    if (IsClangModule) {
      // No need to keep track of the clang module dependencies.
      continue;
    }

    collectModuleDependencies(Mod, Visited, Filenames);
  }
}

static std::atomic<uint64_t> ASTUnitGeneration{ 0 };

void ASTBuildOperation::informConsumer(SwiftASTConsumerRef Consumer) {
  assert(Result &&
         "Can't inform consumer about result if we don't have a result yet");
  Consumer->removeCancellationRequestCallback();
  if (Result.Cancelled) {
    assert(false && "We should only cancel the build operation if there are no "
                    "more consumers attached to it and should not accept any "
                    "new consumers if the build operation was cancelled. Thus "
                    "this case should never happen.");
    ASTManager->Impl.ConsumerNotificationQueue.dispatch([Consumer] {
      Consumer->cancelled();
    });
  } else if (Result.AST) {
    Result.AST->Impl.consumeAsync(Consumer, Result.AST);
  } else {
    ASTManager->Impl.ConsumerNotificationQueue.dispatch([Consumer, Error = Result.Error] {
      Consumer->failed(Error);
    });
  }
}

ASTUnitRef ASTBuildOperation::buildASTUnit(std::string &Error) {
  ++ASTManager->Impl.Stats->numASTBuilds;

  const InvocationOptions &Opts = InvokRef->Impl.Opts;

  LOG_FUNC_SECTION(InfoHighPrio) {
    Log->getOS() << "AST build: ";
    Log->getOS() << Opts.Invok.getModuleName() << '/' << Opts.PrimaryFile;
  }

  ASTUnitRef ASTRef = new ASTUnit(++ASTUnitGeneration, ASTManager->Impl.Stats);
  for (auto &Content : getFileContents()) {
    if (Content.Snapshot)
      ASTRef->Impl.Snapshots.push_back(Content.Snapshot);
  }
  auto &CompIns = ASTRef->Impl.CompInst;
  auto &Consumer = ASTRef->Impl.CollectDiagConsumer;
  // Display diagnostics to stderr.
  CompIns.addDiagnosticConsumer(&Consumer);
  trace::TracedOperation TracedOp(trace::OperationKind::PerformSema);
  trace::SwiftInvocation TraceInfo;
  if (TracedOp.enabled()) {
    trace::initTraceInfo(TraceInfo, InvokRef->Impl.Opts.PrimaryFile,
                         InvokRef->Impl.Opts.Args);
    TracedOp.setDiagnosticProvider(
        [&Consumer](SmallVectorImpl<DiagnosticEntryInfo> &diags) {
          Consumer.getAllDiagnostics(diags);
        });
  }

  CompilerInvocation Invocation;
  InvokRef->Impl.Opts.applyToSubstitutingInputs(
      Invocation, convertFileContentsToInputs(getFileContents()));

  Invocation.getLangOptions().CollectParsedToken = true;

  if (FileSystem != llvm::vfs::getRealFileSystem()) {
    CompIns.getSourceMgr().setFileSystem(FileSystem);
  }

  if (CompIns.setup(Invocation, Error)) {
    LOG_WARN_FUNC("Compilation setup failed!!!");
    if (Error.empty()) {
      Error = "compilation setup failed";
    }
    return nullptr;
  }
  CompIns.getASTContext().getPluginLoader().setRegistry(
      ASTManager->Impl.Plugins.get());
  CompIns.getASTContext().CancellationFlag = CancellationFlag;
  registerIDERequestFunctions(CompIns.getASTContext().evaluator);
  if (TracedOp.enabled()) {
    TracedOp.start(TraceInfo);
  }

  CloseClangModuleFiles scopedCloseFiles(
      *CompIns.getASTContext().getClangModuleLoader());
  CompIns.performSema();

  llvm::SmallPtrSet<ModuleDecl *, 16> Visited;
  SmallVector<std::string, 8> Filenames;
  collectModuleDependencies(CompIns.getMainModule(), Visited, Filenames);
  // FIXME: There exists a small window where the module file may have been
  // modified after compilation finished and before we get its stamp.
  {
    llvm::sys::ScopedLock L(DependencyStampsMtx);
    for (auto &Filename : Filenames) {
      DependencyStamps.push_back(std::make_pair(
          Filename, ASTManager->Impl.getBufferStamp(Filename, FileSystem)));
    }
  }

  // Since we only typecheck the primary file (plus referenced constructs
  // from other files), any error is likely to break SIL generation.
  if (!Consumer.hadAnyError()) {
    // FIXME: Any error anywhere in the SourceFile will switch off SIL
    // diagnostics. This means that this can happen:
    //   - The user sees a SIL diagnostic in one function
    //   - The user edits another function in the same file and introduces a
    //     typechecking error.
    //   - The SIL diagnostic in the first function will be gone.
    //
    // Could we maybe selectively SILGen functions from the SourceFile, so
    // that we avoid SILGen'ing the second function with the typecheck error
    // but still allow SILGen'ing the first function ?
    // Or try to keep track of SIL diagnostics emitted previously ?

    // FIXME: We should run SIL diagnostics asynchronously after typechecking
    // so that they don't delay reporting of typechecking diagnostics and they
    // don't block any other AST processing for the same SwiftInvocation.

    if (auto SF = CompIns.getPrimarySourceFile()) {
      if (CancellationFlag->load(std::memory_order_relaxed)) {
        return nullptr;
      }
      // Disable cancellation while performing SILGen. If the cancellation flag
      // is set, type checking performed during SILGen checks the cancellation
      // flag and might thus fail, which SILGen cannot handle.
      llvm::SaveAndRestore<std::shared_ptr<std::atomic<bool>>> DisableCancellationDuringSILGen(CompIns.getASTContext().CancellationFlag, nullptr);
      SILOptions SILOpts = Invocation.getSILOptions();
      auto &TC = CompIns.getSILTypes();
      std::unique_ptr<SILModule> SILMod = performASTLowering(*SF, TC, SILOpts);
      if (CancellationFlag->load(std::memory_order_relaxed)) {
        return nullptr;
      }
      runSILDiagnosticPasses(*SILMod);
    }
  }

  return ASTRef;
}

void ASTBuildOperation::schedule(WorkQueue Queue) {
  transitionToState(State::Queued, /*ExpectedOldState=*/State::Created);
  auto SharedThis = shared_from_this();
  // Capture `SharedThis` in the dispatched lambda to keep `this` alive.
  // Capture `this` for a more convenient access of members.
  Queue.dispatch(
      [this, SharedThis] {
        transitionToState(State::Running, /*ExpectedOldState=*/State::Queued);

        SWIFT_DEFER {
          transitionToState(State::Finished,
                            /*ExpectedOldState=*/State::Running);
        };

        {
          llvm::sys::ScopedLock L(ConsumersAndResultMtx);
          if (Consumers.empty()) {
            // There are no consumers - no point creating the AST anymore.
            Result.emplace(/*AST=*/nullptr, /*Error=*/"", /*Cancelled=*/true);
            return;
          }
          if (CancellationFlag->load(std::memory_order_relaxed)) {
            assert(false && "We should only set the cancellation flag if there "
                            "are no more consumers");
            for (auto &Consumer : Consumers) {
              Consumer->cancelled();
            }
          }
        }

        std::string Error;
        assert(!Result && "We should only be producing a result once");
        ASTUnitRef AST = buildASTUnit(Error);
        SmallVector<SwiftASTConsumerRef, 4> LocalConsumers;
        {
          llvm::sys::ScopedLock L(ConsumersAndResultMtx);
          bool WasCancelled = CancellationFlag->load(std::memory_order_relaxed);
          Result.emplace(AST, Error, WasCancelled);
          LocalConsumers = Consumers;
          Consumers = {};
        }
        for (auto &Consumer : LocalConsumers) {
          informConsumer(Consumer);
        }
        DidFinishCallback();
      },
      /*isStackDeep=*/true);
}

bool ASTBuildOperation::addConsumer(SwiftASTConsumerRef Consumer) {
  {
    llvm::sys::ScopedLock L(ConsumersAndResultMtx);
    if (isCancelled()) {
      return false;
    }
    if (Result) {
      informConsumer(Consumer);
      return true;
    }
    assert(OperationState != State::Finished);
    Consumers.push_back(Consumer);
  }
  auto WeakThis = std::weak_ptr<ASTBuildOperation>(shared_from_this());
  Consumer->setCancellationRequestCallback(
      [WeakThis](SwiftASTConsumerRef Consumer) {
        if (auto This = WeakThis.lock()) {
          This->requestConsumerCancellation(Consumer);
        }
      });
  return true;
}

/// Returns a build operation that `Consumer` can use, in order of the
/// following:
///   1. The latest finished build operation that either exactly matches, or
///      can be used with snapshots
///   2. If none, the latest in-progress build operation with the same
///      conditions
///   3. `nullptr` otherwise
ASTBuildOperationRef ASTProducer::getBuildOperationForConsumer(
    SwiftASTConsumerRef Consumer,
    IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    SwiftASTManagerRef Mgr) {
  ASTBuildOperationRef LatestUsableOp;
  Statistic *StatCount = nullptr;
  for (auto &BuildOp : llvm::reverse(BuildOperations)) {
    if (BuildOp->isCancelled())
      continue;

    // No point checking for a match, we already have one - we're just looking
    // for a finished operation that can be used with the file contents of
    // `BuildOp` at this point (which we will prefer over an incomplete
    // operation, whether that exactly matches or not).
    if (LatestUsableOp && !BuildOp->isFinished())
      continue;

    // Check for an exact match
    if (BuildOp->matchesSourceState(FileSystem)) {
      LatestUsableOp = BuildOp;
      StatCount = &Mgr->Impl.Stats->numASTCacheHits;
      if (BuildOp->isFinished())
        break;
      continue;
    }

    // Check for whether the operation can be used taking into account
    // snapshots
    std::vector<ImmutableTextSnapshotRef> Snapshots;
    Snapshots.reserve(BuildOp->getFileContents().size());
    for (auto &FileContent : BuildOp->getFileContents()) {
      if (FileContent.Snapshot) {
        Snapshots.push_back(FileContent.Snapshot);
      }
    }

    if (Consumer->canUseASTWithSnapshots(Snapshots)) {
      LatestUsableOp = BuildOp;
      StatCount = &Mgr->Impl.Stats->numASTsUsedWithSnapshots;
      if (BuildOp->isFinished())
        break;
    }
  }

  if (StatCount) {
    ++(*StatCount);
  }
  return LatestUsableOp;
}

void ASTProducer::enqueueConsumer(
    SwiftASTConsumerRef Consumer,
    IntrusiveRefCntPtr<llvm::vfs::FileSystem> FileSystem,
    SwiftASTManagerRef Mgr) {
  // Enqueue the consumer in the background because getBuildOperationForConsumer
  // consults the file system and might be slow. Also, there's no need to do
  // this synchronously since all results will be delivered async anyway.
  auto This = shared_from_this();
  BuildOperationsQueue.dispatch([Consumer, FileSystem, Mgr, This]() {
    // The passed in filesystem does not have overlays resolved. Make sure to
    // do so before performing any file operations.
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> FS = FileSystem;
    const InvocationOptions &InvocOpts = This->InvokRef->Impl.Opts;
    const CompilerInvocation &ActualInvoc = InvocOpts.Invok;
    auto ExpectedOverlay =
        ActualInvoc.getSearchPathOptions().makeOverlayFileSystem(FileSystem);
    if (ExpectedOverlay) {
      FS = std::move(ExpectedOverlay.get());
    } else {
      llvm::consumeError(ExpectedOverlay.takeError());
    }

    if (auto BuildOp =
            This->getBuildOperationForConsumer(Consumer, FS, Mgr)) {
      bool WasAdded = BuildOp->addConsumer(Consumer);
      if (!WasAdded) {
        // The build operation was cancelled after the call to
        // getBuildOperationForConsumer but before the consumer could be
        // added. This should be an absolute edge case. Let's just try
        // again.
        This->enqueueConsumer(Consumer, FS, Mgr);
      }
    } else {
      auto WeakThis = std::weak_ptr<ASTProducer>(This);
      auto DidFinishCallback = [WeakThis, Mgr]() {
        if (auto This = WeakThis.lock()) {
          This->BuildOperationsQueue.dispatchSync(
              [This]() { This->cleanBuildOperations(); });
          // Re-register the object with the cache to update its memory
          // cost.
          Mgr->Impl.ASTCache.set(This->InvokRef->Impl.Key, This);
        }
      };

      ASTBuildOperationRef NewBuildOp = std::make_shared<ASTBuildOperation>(
          FS, This->InvokRef, Mgr, DidFinishCallback);
      This->BuildOperations.push_back(NewBuildOp);
      bool WasAdded = NewBuildOp->addConsumer(Consumer);
      assert(WasAdded && "Consumer wasn't added to a new build operation "
                         "that can't have been cancelled yet?");
      (void)WasAdded;
      NewBuildOp->schedule(Mgr->Impl.ASTBuildQueue);
    }
  });
}
