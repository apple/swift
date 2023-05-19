//===--- PluginRegistry.cpp -----------------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2023 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "swift/AST/PluginRegistry.h"

#include "swift/Basic/Defer.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Program.h"
#include "swift/Basic/Sandbox.h"
#include "swift/Basic/StringExtras.h"
#include "llvm/Support/Endian.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Config/config.h"

#include <signal.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#if HAVE_UNISTD_H
#include <unistd.h>
#elif defined(_WIN32)
#include <io.h>
#endif

using namespace swift;

PluginRegistry::PluginRegistry() {
  dumpMessaging = ::getenv("SWIFT_DUMP_PLUGIN_MESSAGING") != nullptr;
}

llvm::Expected<LoadedLibraryPlugin *>
PluginRegistry::loadLibraryPlugin(StringRef path) {
  std::lock_guard<std::mutex> lock(mtx);
  auto &storage = LoadedPluginLibraries[path];
  if (storage) {
    // Already loaded.
    return storage.get();
  }

  void *lib = nullptr;
#if defined(_WIN32)
  lib = LoadLibraryA(path.str().c_str());
  if (!lib) {
    std::error_code ec(GetLastError(), std::system_category());
    return llvm::errorCodeToError(ec);
  }
#else
  lib = dlopen(path.str().c_str(), RTLD_LAZY | RTLD_LOCAL);
  if (!lib) {
    return llvm::createStringError(llvm::inconvertibleErrorCode(), dlerror());
  }
#endif

  storage = std::make_unique<LoadedLibraryPlugin>(lib);
  return storage.get();
}

void *LoadedLibraryPlugin::getAddressOfSymbol(const char *symbolName) {
  auto &cached = resolvedSymbols[symbolName];
  if (cached)
    return cached;
#if !defined(_WIN32)
  cached = dlsym(handle, symbolName);
#endif
  return cached;
}

llvm::Expected<LoadedExecutablePlugin *>
PluginRegistry::loadExecutablePlugin(StringRef path) {
  llvm::sys::fs::file_status stat;
  if (auto err = llvm::sys::fs::status(path, stat)) {
    return llvm::errorCodeToError(err);
  }

  std::lock_guard<std::mutex> lock(mtx);

  // See if the plugin is already loaded.
  auto &storage = LoadedPluginExecutables[path];
  if (storage) {
    // See if the loaded one is still usable.
    if (storage->getLastModificationTime() == stat.getLastModificationTime())
      return storage.get();

    // The plugin is updated. Close the previously opened plugin.
    storage = nullptr;
  }

  if (!llvm::sys::fs::exists(stat)) {
    return llvm::createStringError(std::errc::no_such_file_or_directory,
                                   "not found");
  }

  if (!llvm::sys::fs::can_execute(path)) {
    return llvm::createStringError(std::errc::permission_denied,
                                   "not executable");
  }

  auto plugin = std::make_unique<LoadedExecutablePlugin>(
      path, stat.getLastModificationTime());

  plugin->setDumpMessaging(dumpMessaging);

  // Launch here to see if it's actually executable, and diagnose (by returning
  // an error) if necessary.
  if (auto error = plugin->spawnIfNeeded()) {
    return std::move(error);
  }

  storage = std::move(plugin);
  return storage.get();
}

llvm::Error LoadedExecutablePlugin::spawnIfNeeded() {
  if (Process) {
    // See if the loaded one is still usable.
    if (!Process->isStale)
      return llvm::Error::success();

    // NOTE: We don't check the mtime here because 'stat(2)' call is too heavy.
    // PluginRegistry::loadExecutablePlugin() checks it and replace this object
    // itself if the plugin is updated.

    // The plugin is stale. Discard the previously opened process.
    Process.reset();
  }

  // Create command line arguments.
  SmallVector<StringRef, 4> command{ExecutablePath};

  // Apply sandboxing.
  llvm::BumpPtrAllocator Allocator;
  Sandbox::apply(command, Allocator);

  // Launch.
  auto childInfo = ExecuteWithPipe(command[0], command);
  if (!childInfo) {
    return llvm::errorCodeToError(childInfo.getError());
  }

  Process = std::make_unique<PluginProcess>(childInfo->Pid,
                                            childInfo->ReadFileDescriptor,
                                            childInfo->WriteFileDescriptor);

  // Call "on reconnect" callbacks.
  for (auto *callback : onReconnect) {
    (*callback)();
  }

  return llvm::Error::success();
}

LoadedExecutablePlugin::PluginProcess::PluginProcess(llvm::sys::procid_t pid,
                                                     int inputFileDescriptor,
                                                     int outputFileDescriptor)
    : pid(pid), inputFileDescriptor(inputFileDescriptor),
      outputFileDescriptor(outputFileDescriptor) {}

LoadedExecutablePlugin::PluginProcess::~PluginProcess() {
  close(inputFileDescriptor);
  close(outputFileDescriptor);
}

LoadedExecutablePlugin::~LoadedExecutablePlugin() {
  // Let ASTGen to cleanup things.
  if (this->cleanup)
    this->cleanup();
}

ssize_t LoadedExecutablePlugin::PluginProcess::read(void *buf,
                                                    size_t nbyte) const {
  ssize_t bytesToRead = nbyte;
  void *ptr = buf;

#if defined(SIGPIPE)
  /// Ignore SIGPIPE while reading.
  auto *old_handler = signal(SIGPIPE, SIG_IGN);
  SWIFT_DEFER { signal(SIGPIPE, old_handler); };
#endif

  while (bytesToRead > 0) {
    ssize_t readingSize = std::min(ssize_t(INT32_MAX), bytesToRead);
    ssize_t readSize = ::read(inputFileDescriptor, ptr, readingSize);
    if (readSize <= 0) {
      // 0: EOF (the plugin exited?), -1: error (e.g. broken pipe.)
      // FIXME: Mark the plugin 'stale' and relaunch later.
      break;
    }
    ptr = static_cast<char *>(ptr) + readSize;
    bytesToRead -= readSize;
  }

  return nbyte - bytesToRead;
}

ssize_t LoadedExecutablePlugin::PluginProcess::write(const void *buf,
                                                     size_t nbyte) const {
  ssize_t bytesToWrite = nbyte;
  const void *ptr = buf;

#if defined(SIGPIPE)
  /// Ignore SIGPIPE while writing.
  auto *old_handler = signal(SIGPIPE, SIG_IGN);
  SWIFT_DEFER { signal(SIGPIPE, old_handler); };
#endif

  while (bytesToWrite > 0) {
    ssize_t writingSize = std::min(ssize_t(INT32_MAX), bytesToWrite);
    ssize_t writtenSize = ::write(outputFileDescriptor, ptr, writingSize);
    if (writtenSize <= 0) {
      // -1: error (e.g. broken pipe,)
      // FIXME: Mark the plugin 'stale' and relaunch later.
      break;
    }
    ptr = static_cast<const char *>(ptr) + writtenSize;
    bytesToWrite -= writtenSize;
  }
  return nbyte - bytesToWrite;
}

llvm::Error LoadedExecutablePlugin::sendMessage(llvm::StringRef message) const {
  ssize_t writtenSize = 0;

  if (dumpMessaging) {
    llvm::dbgs() << "->(plugin:" << Process->pid << ") " << message << "\n";
  }

  const char *data = message.data();
  size_t size = message.size();

  // Write header (message size).
  uint64_t header = llvm::support::endian::byte_swap(
      uint64_t(size), llvm::support::endianness::little);
  writtenSize = Process->write(&header, sizeof(header));
  if (writtenSize != sizeof(header)) {
    setStale();
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "failed to write plugin message header");
  }

  // Write message.
  writtenSize = Process->write(data, size);
  if (writtenSize != ssize_t(size)) {
    setStale();
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "failed to write plugin message data");
  }

  return llvm::Error::success();
}

llvm::Expected<std::string> LoadedExecutablePlugin::waitForNextMessage() const {
  ssize_t readSize = 0;

  // Read header (message size).
  uint64_t header;
  readSize = Process->read(&header, sizeof(header));

  if (readSize != sizeof(header)) {
    setStale();
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "failed to read plugin message header");
  }

  size_t size = llvm::support::endian::read<uint64_t>(
      &header, llvm::support::endianness::little);

  // Read message.
  std::string message;
  message.reserve(size);
  auto sizeToRead = size;
  while (sizeToRead > 0) {
    char buffer[4096];
    readSize = Process->read(buffer, std::min(sizeof(buffer), sizeToRead));
    if (readSize == 0) {
      setStale();
      return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                     "failed to read plugin message data");
    }
    sizeToRead -= readSize;
    message.append(buffer, readSize);
  }

  if (dumpMessaging) {
    llvm::dbgs() << "<-(plugin:" << Process->pid << ") " << message << "\n";
  }

  return message;
}
