//===--- PluginRegistry.h ---------------------------------------*- C++ -*-===//
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
#ifndef SWIFT_PLUGIN_REGISTRY_H
#define SWIFT_PLUGIN_REGISTRY_H

#include "swift/Basic/StringExtras.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/Chrono.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <mutex>
#include <vector>

namespace swift {

/// Represent a 'dlopen'ed plugin library.
class LoadedLibraryPlugin {
  // Opaque handle used to interface with OS-specific dynamic library.
  void *handle;

  /// Cache of loaded symbols.
  llvm::StringMap<void *> resolvedSymbols;

public:
  LoadedLibraryPlugin(void *handle) : handle(handle) {}

  /// Finds the address of the given symbol within the library.
  void *getAddressOfSymbol(const char *symbolName);
};

/// Represent a "resolved" exectuable plugin.
///
/// Plugin clients usually deal with this object to communicate with the actual
/// plugin implementation.
/// This object has a file path of the plugin executable, and is responsible to
/// launch it and manages the process. When the plugin process crashes, this
/// should automatically relaunch the process so the clients can keep using this
/// object as the interface.
class LoadedExecutablePlugin {

  /// Represents the current process of the executable plugin.
  struct PluginProcess {
    const llvm::sys::procid_t pid;
    const int inputFileDescriptor;
    const int outputFileDescriptor;
    bool isStale = false;

    PluginProcess(llvm::sys::procid_t pid, int inputFileDescriptor,
                  int outputFileDescriptor);

    ~PluginProcess();

    ssize_t write(const void *buf, size_t nbyte) const;
    ssize_t read(void *buf, size_t nbyte) const;
  };

  /// Launched current process.
  std::unique_ptr<PluginProcess> Process;

  /// Path to the plugin executable.
  const std::string ExecutablePath;

  /// Last modification time of the `ExecutablePath` when this is initialized.
  const llvm::sys::TimePoint<> LastModificationTime;

  /// Opaque value of the protocol capability of the pluugin. This is a
  /// value from ASTGen.
  const void *capability = nullptr;

  /// Callbacks to be called when the connection is restored.
  llvm::SmallVector<std::function<void(void)> *, 0> onReconnect;

  /// Flag to dump plugin messagings.
  bool dumpMessaging = false;

  /// Cleanup function to call ASTGen.
  std::function<void(void)> cleanup;

  std::mutex mtx;

public:
  LoadedExecutablePlugin(llvm::StringRef ExecutablePath,
                         llvm::sys::TimePoint<> LastModificationTime)
      : ExecutablePath(ExecutablePath),
        LastModificationTime(LastModificationTime){};
  ~LoadedExecutablePlugin();

  /// The last modification time of 'ExecutablePath' when this object is
  /// created.
  llvm::sys::TimePoint<> getLastModificationTime() const {
    return LastModificationTime;
  }

  /// Indicates that the current process is usable.
  bool isAlive() const { return Process != nullptr && !Process->isStale; }

  /// Mark the current process "stale".
  void setStale() const { Process->isStale = true; }

  void lock() { mtx.lock(); }
  void unlock() { mtx.unlock(); }

  // Launch the plugin if it's not already running, or it's stale. Return an
  // error if it's fails to execute it.
  llvm::Error spawnIfNeeded();

  /// Send a message to the plugin.
  llvm::Error sendMessage(llvm::StringRef message) const;

  /// Wait for a message from plugin and returns it.
  llvm::Expected<std::string> waitForNextMessage() const;

  bool isInitialized() const { return bool(cleanup); }
  void setCleanup(std::function<void(void)> cleanup) {
    this->cleanup = cleanup;
  }

  /// Add "on reconnect" callback.
  /// These callbacks are called when `spawnIfNeeded()` relaunched the plugin.
  void addOnReconnect(std::function<void(void)> *fn) {
    onReconnect.push_back(fn);
  }

  /// Remove "on reconnect" callback.
  void removeOnReconnect(std::function<void(void)> *fn) {
    llvm::erase_value(onReconnect, fn);
  }

  llvm::sys::procid_t getPid() { return Process->pid; }

  NullTerminatedStringRef getExecutablePath() {
    return {ExecutablePath.c_str(), ExecutablePath.size()};
  }

  const void *getCapability() { return capability; };
  void setCapability(const void *newValue) { capability = newValue; };

  void setDumpMessaging(bool flag) { dumpMessaging = flag; }
};

class PluginRegistry {
  /// Record of loaded plugin library modules.
  llvm::StringMap<std::unique_ptr<LoadedLibraryPlugin>> LoadedPluginLibraries;

  /// Record of loaded plugin executables.
  llvm::StringMap<std::unique_ptr<LoadedExecutablePlugin>>
      LoadedPluginExecutables;

  /// Flag to dump plugin messagings.
  bool dumpMessaging = false;

  std::mutex mtx;

public:
  PluginRegistry();

  /// Load a dynamic link library specified by \p path.
  /// If \p path plugin is already loaded, this returns the cached object.
  llvm::Expected<LoadedLibraryPlugin *> loadLibraryPlugin(llvm::StringRef path);

  /// Load an executable plugin specified by \p path .
  /// If \p path plugin is already loaded, this returns the cached object.
  llvm::Expected<LoadedExecutablePlugin *>
  loadExecutablePlugin(llvm::StringRef path);
};

} // namespace swift

#endif // SWIFT_PLUGIN_REGISTRY_H
