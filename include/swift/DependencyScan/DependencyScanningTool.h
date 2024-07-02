//===-------------- DependencyScanningTool.h - Swift Compiler -------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_DEPENDENCY_SCANNING_TOOL_H
#define SWIFT_DEPENDENCY_SCANNING_TOOL_H

#include "swift-c/DependencyScan/DependencyScan.h"
#include "swift/Frontend/Frontend.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/DependencyScan/ScanDependencies.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/StringSaver.h"

namespace swift {
namespace dependencies {
class DependencyScanningTool;
class DependencyScanDiagnosticCollector;

struct ScanQueryInstance {
  std::unique_ptr<CompilerInstance> ScanInstance;
  std::shared_ptr<DependencyScanDiagnosticCollector> ScanDiagnostics;
};

/// Diagnostic consumer that simply collects the diagnostics emitted so-far
class DependencyScanDiagnosticCollector : public DiagnosticConsumer {
private:
  struct ScannerDiagnosticInfo {
    std::string Message;
    llvm::SourceMgr::DiagKind Severity;
    std::optional<ScannerImportStatementInfo::ImportDiagnosticLocationInfo> ImportLocation;
  };
  std::vector<ScannerDiagnosticInfo> Diagnostics;

  void handleDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;

protected:
  virtual void addDiagnostic(SourceManager &SM, const DiagnosticInfo &Info);

public:
  friend DependencyScanningTool;
  DependencyScanDiagnosticCollector() {}
  void reset() { Diagnostics.clear(); }
  const std::vector<ScannerDiagnosticInfo> &getDiagnostics() const {
    return Diagnostics;
  }
};

/// Locking variant of the above diagnostic collector that guards accesses to
/// its state with a lock.
class LockingDependencyScanDiagnosticCollector
    : public DependencyScanDiagnosticCollector {
private:
  void addDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) override;
  llvm::sys::SmartMutex<true> ScanningDiagnosticConsumerStateLock;

public:
  friend DependencyScanningTool;
  LockingDependencyScanDiagnosticCollector() {}
};

/// Given a set of arguments to a print-target-info frontend tool query, produce the
/// JSON target info.
llvm::ErrorOr<swiftscan_string_ref_t> getTargetInfo(ArrayRef<const char *> Command,
                                                    const char *main_executable_path);

/// The high-level implementation of the dependency scanner that runs on
/// an individual worker thread.
class DependencyScanningTool {
public:
  /// Construct a dependency scanning tool.
  DependencyScanningTool();

  /// Collect the full module dependency graph for the input, ignoring any
  /// placeholder modules.
  ///
  /// \returns a \c StringError with the diagnostic output if errors
  /// occurred, \c swiftscan_dependency_result_t otherwise.
  llvm::ErrorOr<swiftscan_dependency_graph_t>
  getDependencies(ArrayRef<const char *> Command,
                  const llvm::StringSet<> &PlaceholderModules,
                  StringRef WorkingDirectory);

  /// Collect the set of imports for the input module
  ///
  /// \returns a \c StringError with the diagnostic output if errors
  /// occurred, \c swiftscan_prescan_result_t otherwise.
  llvm::ErrorOr<swiftscan_import_set_t>
  getImports(ArrayRef<const char *> Command, StringRef WorkingDirectory);

  /// Collect the full module dependency graph for the input collection of
  /// module names (batch inputs) and output them to the
  /// BatchScanInput-specified output locations.
  ///
  /// \returns a \c std::error_code if errors occurred during scan.
  std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>>
  getDependencies(ArrayRef<const char *> Command,
                  const std::vector<BatchScanInput> &BatchInput,
                  const llvm::StringSet<> &PlaceholderModules,
                  StringRef WorkingDirectory);

  /// Writes the current `SharedCache` instance to a specified FileSystem path.
  void serializeCache(llvm::StringRef path);
  /// Loads an instance of a `SwiftDependencyScanningService` to serve as the `SharedCache`
  /// from a specified FileSystem path.
  bool loadCache(llvm::StringRef path);
  /// Discard the tool's current `SharedCache` and start anew.
  void resetCache();
  /// Query diagnostics consumed so far.
  std::vector<DependencyScanDiagnosticCollector::ScannerDiagnosticInfo> getDiagnostics();
  /// Discared the collection of diagnostics encountered so far.
  void resetDiagnostics();

  /// Using the specified invocation command, instantiate a CompilerInstance
  /// that will be used for this scan.
  llvm::ErrorOr<ScanQueryInstance>
  initCompilerInstanceForScan(ArrayRef<const char *> Command,
                              StringRef WorkingDirectory,
                              std::shared_ptr<DependencyScanDiagnosticCollector> scannerDiagnosticsCollector);

private:
  /// Shared cache of module dependencies, re-used by individual full-scan queries
  /// during the lifetime of this Tool.
  std::unique_ptr<SwiftDependencyScanningService> ScanningService;

  /// Shared cache of compiler instances created during batch scanning, corresponding to
  /// command-line options specified in the batch scan input entry.
  std::unique_ptr<CompilerArgInstanceCacheMap> VersionedPCMInstanceCacheCache;

  /// Shared state mutual-exclusivity lock
  llvm::sys::SmartMutex<true> DependencyScanningToolStateLock;

  /// A shared consumer that accumulates encountered diagnostics.
  LockingDependencyScanDiagnosticCollector CDC;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver;
};

swiftscan_diagnostic_set_t *mapCollectedDiagnosticsForOutput(const DependencyScanDiagnosticCollector *diagnosticCollector);

} // end namespace dependencies
} // end namespace swift

#endif // SWIFT_DEPENDENCY_SCANNING_TOOL_H
