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
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Program.h"

#include <mutex>
#include <vector>

namespace swift {

/// Base class for compiler plugins, or plugin servers.
class CompilerPlugin {
  const std::string path;

  std::mutex mtx;

  /// Opaque value of the protocol capability of the plugin. This is a
  /// value from ASTGen.
  const void *capability = nullptr;

  /// Cleanup function to call ASTGen.
  std::function<void(void)> cleanup;

  /// Callbacks to be called when the connection is restored.
  llvm::SmallVector<std::function<void(void)> *, 0> onReconnect;

  /// Flag to enable dumping of plugin messages.
  bool dumpMessaging = false;

protected:
  CompilerPlugin(llvm::StringRef path) : path(path) {}

  bool shouldDumpMessaging() const { return dumpMessaging; }

public:
  NullTerminatedStringRef getPath() const {
    return {path.c_str(), path.size()};
  }

  void lock() { mtx.lock(); }
  void unlock() { mtx.unlock(); }

  bool isInitialized() const { return bool(cleanup); }
  void setCleanup(std::function<void(void)> cleanup) {
    this->cleanup = cleanup;
  }

  const void *getCapability() { return capability; };
  void setCapability(const void *newValue) { capability = newValue; };

  void setDumpMessaging(bool flag) { dumpMessaging = flag; }

  virtual ~CompilerPlugin();

  /// Launch the plugin if it's not already running, or it's stale. Return an
  /// error if it's fails to execute it.
  virtual llvm::Error spawnIfNeeded() = 0;

  /// Send a message to the plugin.
  virtual llvm::Error sendMessage(llvm::StringRef message) = 0;

  /// Wait for a message from plugin and returns it.
  virtual llvm::Expected<std::string> waitForNextMessage() = 0;

  /// Add "on reconnect" callback.
  /// These callbacks are called when `spawnIfNeeded()` relaunched the plugin.
  void addOnReconnect(std::function<void(void)> *fn) {
    onReconnect.push_back(fn);
  }

  /// Remove "on reconnect" callback.
  void removeOnReconnect(std::function<void(void)> *fn) {
    llvm::erase_value(onReconnect, fn);
  }

  ArrayRef<std::function<void(void)> *> getOnReconnectCallbacks() {
    return onReconnect;
  }
};

/// Represents a in-process plugin server.
class InProcessPlugins : public CompilerPlugin {
  /// PluginServer
  llvm::sys::DynamicLibrary server;

  /// Entry point in the in-process plugin server. It receives the request
  /// string and populate the response string. The return value indicates there
  /// was an error. If true the returned string contains the error message.
  /// 'free'ing the populated `responseDataPtr` is caller's responsibility.
  using HandleMessageFunction = bool (*)(const char *requestData,
                                         size_t requestLength,
                                         char **responseDataPtr,
                                         size_t *responseDataLengthPtr);
  HandleMessageFunction handleMessageFn;

  /// Temporary storage for the response data from 'handleMessageFn'.
  std::string receivedResponse;

  InProcessPlugins(llvm::StringRef serverPath, llvm::sys::DynamicLibrary server,
                   HandleMessageFunction handleMessageFn)
      : CompilerPlugin(serverPath), server(server),
        handleMessageFn(handleMessageFn) {}

public:
  /// Create an instance by loading the in-process plugin server at 'serverPath'
  /// and return it.
  static llvm::Expected<std::unique_ptr<InProcessPlugins>>
  create(const char *serverPath);

  /// Send a message to the plugin.
  llvm::Error sendMessage(llvm::StringRef message) override;

  /// Wait for a message from plugin and returns it.
  llvm::Expected<std::string> waitForNextMessage() override;

  llvm::Error spawnIfNeeded() override {
    // NOOP. It's always loaded.
    return llvm::Error::success();
  }
};

/// Represent a "resolved" executable plugin.
///
/// Plugin clients usually deal with this object to communicate with the actual
/// plugin implementation.
/// This object has a file path of the plugin executable, and is responsible to
/// launch it and manages the process. When the plugin process crashes, this
/// should automatically relaunch the process so the clients can keep using this
/// object as the interface.
class LoadedExecutablePlugin : public CompilerPlugin {

  /// Represents the current process of the executable plugin.
  struct PluginProcess {
    const llvm::sys::ProcessInfo process;
    const int input;
    const int output;
    bool isStale = false;

    PluginProcess(llvm::sys::ProcessInfo process, int input, int output)
        : process(process), input(input), output(output) {}
    ~PluginProcess();

    ssize_t write(const void *buf, size_t nbyte) const;
    ssize_t read(void *buf, size_t nbyte) const;
  };

  /// Launched current process.
  std::unique_ptr<PluginProcess> Process;

  /// Last modification time of the `ExecutablePath` when this is initialized.
  const llvm::sys::TimePoint<> LastModificationTime;

  /// Disable sandbox.
  bool disableSandbox = false;

  /// Mark the current process "stale" (not usable anymore for some reason,
  /// probably crashed).
  void setStale() { Process->isStale = true; }

public:
  LoadedExecutablePlugin(llvm::StringRef ExecutablePath,
                         llvm::sys::TimePoint<> LastModificationTime,
                         bool disableSandbox)
      : CompilerPlugin(ExecutablePath),
        LastModificationTime(LastModificationTime),
        disableSandbox(disableSandbox){};

  /// The last modification time of 'ExecutablePath' when this object is
  /// created.
  llvm::sys::TimePoint<> getLastModificationTime() const {
    return LastModificationTime;
  }

  /// Indicates that the current process is usable.
  bool isAlive() const { return Process != nullptr && !Process->isStale; }

  // Launch the plugin if it's not already running, or it's stale. Return an
  // error if it's fails to execute it.
  llvm::Error spawnIfNeeded() override;

  /// Send a message to the plugin.
  llvm::Error sendMessage(llvm::StringRef message) override;

  /// Wait for a message from plugin and returns it.
  llvm::Expected<std::string> waitForNextMessage() override;

  llvm::sys::procid_t getPid() { return Process->process.Pid; }
  llvm::sys::process_t getProcess() { return Process->process.Process; }
};

class PluginRegistry {
  /// The in-process plugin server.
  std::unique_ptr<InProcessPlugins> inProcessPlugins;

  /// Record of loaded plugin executables.
  llvm::StringMap<std::unique_ptr<LoadedExecutablePlugin>>
      LoadedPluginExecutables;

  /// Flag to dump plugin messagings.
  bool dumpMessaging = false;

  std::mutex mtx;

public:
  PluginRegistry();

  /// Get the in-process plugin server.
  /// If it's loaded, returned the cached object. If the loaded instance is
  /// from a different 'serverPath', returns an error as we don't support
  /// multiple in-process plugin server in a host process.
  llvm::Expected<CompilerPlugin *>
  getInProcessPlugins(llvm::StringRef serverPath);

  /// Load an executable plugin specified by \p path .
  /// If \p path plugin is already loaded, this returns the cached object.
  llvm::Expected<CompilerPlugin *> loadExecutablePlugin(llvm::StringRef path,
                                                        bool disableSandbox);
};

} // namespace swift

#endif // SWIFT_PLUGIN_REGISTRY_H
