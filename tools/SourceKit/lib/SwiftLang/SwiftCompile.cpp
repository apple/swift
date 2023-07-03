//===--- SwiftCompile.cpp -------------------------------------------------===//
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

#include "SwiftEditorDiagConsumer.h"
#include "SwiftLangSupport.h"

#include "SourceKit/Support/FileSystemProvider.h"

#include "swift/IDETool/CompileInstance.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace SourceKit;
using namespace swift;

std::shared_ptr<compile::Session>
compile::SessionManager::getSession(StringRef name) {
  llvm::sys::ScopedLock lock(mtx);
  auto i = sessions.find(name);
  if (i != sessions.end()) {
    return i->second;
  }

  bool inserted = false;
  std::tie(i, inserted) =
      sessions.try_emplace(name, std::make_shared<compile::Session>(
                                     SwiftExecutablePath, RuntimeResourcePath,
                                     DiagnosticDocumentationPath, Plugins));
  assert(inserted);
  return i->second;
}

void compile::SessionManager::clearSession(StringRef name) {
  llvm::sys::ScopedLock lock(mtx);
  sessions.erase(name);
}

namespace {
class InvocationRequest final
    : public llvm::TrailingObjects<InvocationRequest, char *, char> {
  friend class llvm::TrailingObjects<InvocationRequest, char *, char>;
  size_t numArgs;

  size_t numTrailingObjects(OverloadToken<char *>) const { return numArgs; }

  MutableArrayRef<char *> getMutableArgs() {
    return {getTrailingObjects<char *>(), numArgs};
  }

  InvocationRequest(ArrayRef<const char *> Args) : numArgs(Args.size()) {
    // Copy the arguments to the buffer.
    char *ptr = getTrailingObjects<char>();
    auto thisArgs = getMutableArgs();
    size_t i = 0;
    for (const char *arg : Args) {
      thisArgs[i++] = ptr;
      auto size = strlen(arg) + 1;
      strncpy(ptr, arg, size);
      ptr += size;
    }
  }

public:
  static InvocationRequest *create(ArrayRef<const char *> Args) {
    size_t charBufSize = 0;
    for (auto arg : Args) {
      charBufSize += strlen(arg) + 1;
    }
    auto size = InvocationRequest::totalSizeToAlloc<char *, char>(Args.size(),
                                                                  charBufSize);
    auto *data = malloc(size);
    return new (data) InvocationRequest(Args);
  }

  ArrayRef<const char *> getArgs() const {
    return {getTrailingObjects<char *>(), numArgs};
  }
};
} // namespace

void compile::SessionManager::performCompileAsync(
    StringRef Name, ArrayRef<const char *> Args,
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fileSystem,
    std::shared_ptr<std::atomic<bool>> CancellationFlag,
    std::function<void(const RequestResult<CompilationResult> &)> Receiver) {
  auto session = getSession(Name);

  auto *request = InvocationRequest::create(Args);
  compileQueue.dispatch(
      [session, request, fileSystem, Receiver, CancellationFlag]() {
        SWIFT_DEFER {
          delete request;
        };

        // Cancelled during async dispatching?
        if (CancellationFlag->load(std::memory_order_relaxed)) {
          Receiver(RequestResult<CompilationResult>::cancelled());
          return;
        }

        EditorDiagConsumer diagC;
        auto stat =
            session->performCompile(request->getArgs(), fileSystem, &diagC,
                                    CancellationFlag);

        // Cancelled during the compilation?
        if (CancellationFlag->load(std::memory_order_relaxed)) {
          Receiver(RequestResult<CompilationResult>::cancelled());
          return;
        }

        SmallVector<DiagnosticEntryInfo, 0> diagEntryInfos;
        diagC.getAllDiagnostics(diagEntryInfos);
        Receiver(RequestResult<CompilationResult>::fromResult(
            {stat, diagEntryInfos}));
      },
      /*isStackDeep=*/true);
}

void SwiftLangSupport::performCompile(
    StringRef Name, ArrayRef<const char *> Args,
    llvm::Optional<VFSOptions> vfsOptions,
    SourceKitCancellationToken CancellationToken,
    std::function<void(const RequestResult<CompilationResult> &)> Receiver) {

  std::string error;
  auto fileSystem =
      getFileSystem(vfsOptions, /*primaryFile=*/llvm::None, error);
  if (!fileSystem) {
    Receiver(RequestResult<CompilationResult>::fromError(error));
    return;
  }

  std::shared_ptr<std::atomic<bool>> CancellationFlag = std::make_shared<std::atomic<bool>>(false);
  ReqTracker->setCancellationHandler(CancellationToken, [CancellationFlag]() {
    CancellationFlag->store(true, std::memory_order_relaxed);
  });

  CompileManager->performCompileAsync(Name, Args, std::move(fileSystem),
                                      CancellationFlag, Receiver);
}

void SwiftLangSupport::closeCompile(StringRef Name) {
  CompileManager->clearSession(Name);
}
