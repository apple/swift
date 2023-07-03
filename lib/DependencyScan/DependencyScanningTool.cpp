//===------------ DependencyScanningTool.cpp - Swift Compiler -------------===//
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

#include "swift/DependencyScan/DependencyScanningTool.h"
#include "swift/DependencyScan/SerializedModuleDependencyCacheFormat.h"
#include "swift/DependencyScan/StringUtils.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/TargetInfo.h"
#include "swift/DependencyScan/DependencyScanImpl.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/VirtualOutputBackends.h"

#include <sstream>

namespace swift {
namespace dependencies {

// Global mutex for target info queries since they are executed separately .
llvm::sys::SmartMutex<true> TargetInfoMutex;

llvm::ErrorOr<swiftscan_string_ref_t> getTargetInfo(ArrayRef<const char *> Command,
                                                    const char *main_executable_path) {
  llvm::sys::SmartScopedLock<true> Lock(TargetInfoMutex);

  // We must reset option occurrences because we are handling an unrelated
  // command-line to those possibly parsed before using the same tool.
  // We must do so because LLVM options parsing is done using a managed
  // static `GlobalParser`.
  llvm::cl::ResetAllOptionOccurrences();
  // Parse arguments.
  std::string CommandString;
  for (const auto *c : Command) {
    CommandString.append(c);
    CommandString.append(" ");
  }
  SmallVector<const char *, 4> Args;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  // Ensure that we use the Windows command line parsing on Windows as we need
  // to ensure that we properly handle paths.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSWindows()) 
    llvm::cl::TokenizeWindowsCommandLine(CommandString, Saver, Args);
  else
    llvm::cl::TokenizeGNUCommandLine(CommandString, Saver, Args);
  SourceManager dummySM;
  DiagnosticEngine DE(dummySM);
  CompilerInvocation Invocation;
  if (Invocation.parseArgs(Args, DE, nullptr, {}, main_executable_path)) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  // Store the result to a string.
  std::string ResultStr;
  llvm::raw_string_ostream StrOS(ResultStr);
  swift::targetinfo::printTargetInfo(Invocation, StrOS);
  return c_string_utils::create_clone(ResultStr.c_str());
}

void DependencyScannerDiagnosticCollectingConsumer::handleDiagnostic(SourceManager &SM,
                      const DiagnosticInfo &Info) {
  addDiagnostic(SM, Info);
  for (auto ChildInfo : Info.ChildDiagnosticInfo) {
    addDiagnostic(SM, *ChildInfo);
  }
}

void DependencyScannerDiagnosticCollectingConsumer::addDiagnostic(SourceManager &SM, const DiagnosticInfo &Info) {
  // Determine what kind of diagnostic we're emitting.
  llvm::SourceMgr::DiagKind SMKind;
  switch (Info.Kind) {
  case DiagnosticKind::Error:
    SMKind = llvm::SourceMgr::DK_Error;
    break;
  case DiagnosticKind::Warning:
    SMKind = llvm::SourceMgr::DK_Warning;
    break;
  case DiagnosticKind::Note:
    SMKind = llvm::SourceMgr::DK_Note;
    break;
  case DiagnosticKind::Remark:
    SMKind = llvm::SourceMgr::DK_Remark;
    break;
  }
  // Translate ranges.
  SmallVector<llvm::SMRange, 2> Ranges;
  for (auto R : Info.Ranges)
    Ranges.push_back(getRawRange(SM, R));
  // Translate fix-its.
  SmallVector<llvm::SMFixIt, 2> FixIts;
  for (DiagnosticInfo::FixIt F : Info.FixIts)
    FixIts.push_back(getRawFixIt(SM, F));

  std::string ResultingMessage;
  llvm::raw_string_ostream Stream(ResultingMessage);

  // Actually substitute the diagnostic arguments into the diagnostic text.
  llvm::SmallString<256> Text;
  llvm::raw_svector_ostream Out(Text);
  DiagnosticEngine::formatDiagnosticText(Out, Info.FormatString,
                                         Info.FormatArgs);
  auto Msg = SM.GetMessage(Info.Loc, SMKind, Text, Ranges, FixIts);
  Diagnostics.push_back(ScannerDiagnosticInfo{Msg.getMessage().str(), SMKind});
}

DependencyScanningTool::DependencyScanningTool()
    : ScanningService(std::make_unique<SwiftDependencyScanningService>()),
      VersionedPCMInstanceCacheCache(
          std::make_unique<CompilerArgInstanceCacheMap>()),
      CDC(), Alloc(), Saver(Alloc) {}

llvm::ErrorOr<swiftscan_dependency_graph_t>
DependencyScanningTool::getDependencies(
    ArrayRef<const char *> Command,
    const llvm::StringSet<> &PlaceholderModules) {
  // The primary instance used to scan the query Swift source-code
  auto InstanceOrErr = initScannerForAction(Command);
  if (std::error_code EC = InstanceOrErr.getError())
    return EC;
  auto Instance = std::move(*InstanceOrErr);

  // Local scan cache instance, wrapping the shared global cache.
  ModuleDependenciesCache cache(*ScanningService,
                                Instance->getMainModule()->getNameStr().str(),
                                Instance->getInvocation().getModuleScanningHash());
  // Execute the scanning action, retrieving the in-memory result
  auto DependenciesOrErr = performModuleScan(*Instance.get(), cache);
  if (DependenciesOrErr.getError())
    return std::make_error_code(std::errc::not_supported);
  auto Dependencies = std::move(*DependenciesOrErr);

  return Dependencies;
}

llvm::ErrorOr<swiftscan_import_set_t>
DependencyScanningTool::getImports(ArrayRef<const char *> Command) {
  // The primary instance used to scan the query Swift source-code
  auto InstanceOrErr = initScannerForAction(Command);
  if (std::error_code EC = InstanceOrErr.getError())
    return EC;
  auto Instance = std::move(*InstanceOrErr);

  // Execute the scanning action, retrieving the in-memory result
  auto DependenciesOrErr = performModulePrescan(*Instance.get());
  if (DependenciesOrErr.getError())
    return std::make_error_code(std::errc::not_supported);
  auto Dependencies = std::move(*DependenciesOrErr);

  return Dependencies;
}

std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>>
DependencyScanningTool::getDependencies(
    ArrayRef<const char *> Command,
    const std::vector<BatchScanInput> &BatchInput,
    const llvm::StringSet<> &PlaceholderModules) {
  // The primary instance used to scan Swift modules
  auto InstanceOrErr = initScannerForAction(Command);
  if (std::error_code EC = InstanceOrErr.getError())
    return std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>>(
        BatchInput.size(), std::make_error_code(std::errc::invalid_argument));
  auto Instance = std::move(*InstanceOrErr);

  // Local scan cache instance, wrapping the shared global cache.
  ModuleDependenciesCache cache(*ScanningService,
                                Instance->getMainModule()->getNameStr().str(),
                                Instance->getInvocation().getModuleScanningHash());
  auto BatchScanResults = performBatchModuleScan(
      *Instance.get(), cache, VersionedPCMInstanceCacheCache.get(),
      Saver, BatchInput);

  return BatchScanResults;
}

void DependencyScanningTool::serializeCache(llvm::StringRef path) {
  llvm::sys::SmartScopedLock<true> Lock(DependencyScanningToolStateLock);
  SourceManager SM;
  DiagnosticEngine Diags(SM);
  Diags.addConsumer(CDC);
  llvm::vfs::OnDiskOutputBackend Backend;
  module_dependency_cache_serialization::writeInterModuleDependenciesCache(
      Diags, Backend, path, *ScanningService);
}

bool DependencyScanningTool::loadCache(llvm::StringRef path) {
  llvm::sys::SmartScopedLock<true> Lock(DependencyScanningToolStateLock);
  SourceManager SM;
  DiagnosticEngine Diags(SM);
  Diags.addConsumer(CDC);
  ScanningService = std::make_unique<SwiftDependencyScanningService>();
  bool readFailed =
      module_dependency_cache_serialization::readInterModuleDependenciesCache(
          path, *ScanningService);
  if (readFailed) {
    Diags.diagnose(SourceLoc(), diag::warn_scanner_deserialize_failed, path);
  }
  return readFailed;
}

void DependencyScanningTool::resetCache() {
  ScanningService.reset(new SwiftDependencyScanningService());
}

void DependencyScanningTool::resetDiagnostics() {
  CDC.reset();
}

llvm::ErrorOr<std::unique_ptr<CompilerInstance>>
DependencyScanningTool::initScannerForAction(
    ArrayRef<const char *> Command) {
  // The remainder of this method operates on shared state in the
  // scanning service and global LLVM state with:
  // llvm::cl::ResetAllOptionOccurrences
  llvm::sys::SmartScopedLock<true> Lock(DependencyScanningToolStateLock);
  auto instanceOrErr = initCompilerInstanceForScan(Command);
  if (instanceOrErr.getError())
    return instanceOrErr;
  return instanceOrErr;
}

llvm::ErrorOr<std::unique_ptr<CompilerInstance>>
DependencyScanningTool::initCompilerInstanceForScan(
    ArrayRef<const char *> CommandArgs) {
  // State unique to an individual scan
  auto Instance = std::make_unique<CompilerInstance>();
  Instance->addDiagnosticConsumer(&CDC);

  // Basic error checking on the arguments
  if (CommandArgs.empty()) {
    Instance->getDiags().diagnose(SourceLoc(), diag::error_no_frontend_args);
    return std::make_error_code(std::errc::invalid_argument);
  }

  CompilerInvocation Invocation;
  SmallString<128> WorkingDirectory;
  llvm::sys::fs::current_path(WorkingDirectory);

  // We must reset option occurrences because we are handling an unrelated
  // command-line to those possibly parsed before using the same tool.
  // We must do so because LLVM options parsing is done using a managed
  // static `GlobalParser`.
  llvm::cl::ResetAllOptionOccurrences();
  // Parse/tokenize arguments.
  std::string CommandString;
  for (const auto *c : CommandArgs) {
    CommandString.append(c);
    CommandString.append(" ");
  }
  SmallVector<const char *, 4> Args;
  llvm::BumpPtrAllocator Alloc;
  llvm::StringSaver Saver(Alloc);
  // Ensure that we use the Windows command line parsing on Windows as we need
  // to ensure that we properly handle paths.
  if (llvm::Triple(llvm::sys::getProcessTriple()).isOSWindows())
    llvm::cl::TokenizeWindowsCommandLine(CommandString, Saver, Args);
  else
    llvm::cl::TokenizeGNUCommandLine(CommandString, Saver, Args);

  if (Invocation.parseArgs(Args, Instance->getDiags(),
                           nullptr, WorkingDirectory, "/tmp/foo")) {
    return std::make_error_code(std::errc::invalid_argument);
  }

  // Setup the instance
  std::string InstanceSetupError;
  if (Instance->setup(Invocation, InstanceSetupError)) {
    return std::make_error_code(std::errc::not_supported);
  }

  // Setup the caching service after the instance finishes setup.
  if (ScanningService->setupCachingDependencyScanningService(*Instance))
    return std::make_error_code(std::errc::invalid_argument);

  (void)Instance->getMainModule();

  return Instance;
}

} // namespace dependencies
} // namespace swift
