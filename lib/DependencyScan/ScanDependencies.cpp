//===--- ScanDependencies.cpp -- Scans the dependencies of a module -------===//
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

#include "swift/Basic/PrettyStackTrace.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/Decl.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsDriver.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/Module.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/SourceFile.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/FileTypes.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/STLExtras.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/DependencyScan/DependencyScanImpl.h"
#include "swift/DependencyScan/DependencyScanningTool.h"
#include "swift/DependencyScan/ModuleDependencyScanner.h"
#include "swift/DependencyScan/ScanDependencies.h"
#include "swift/DependencyScan/SerializedModuleDependencyCacheFormat.h"
#include "swift/DependencyScan/StringUtils.h"
#include "swift/Frontend/CachingUtils.h"
#include "swift/Frontend/CompileJobCacheKey.h"
#include "swift/Frontend/CompileJobCacheResult.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Strings.h"
#include "clang/Basic/Module.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SetOperations.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/CAS/ActionCache.h"
#include "llvm/CAS/CASReference.h"
#include "llvm/CAS/ObjectStore.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/StringSaver.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/YAMLParser.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Support/raw_ostream.h"
#include <algorithm>
#include <set>
#include <sstream>
#include <string>
#include <algorithm>

using namespace swift;
using namespace swift::dependencies;
using namespace swift::c_string_utils;
using namespace llvm::yaml;

namespace {

static std::string getScalaNodeText(Node *N) {
  SmallString<32> Buffer;
  return cast<ScalarNode>(N)->getValue(Buffer).str();
}

/// Parse an entry like this, where the "platforms" key-value pair is optional:
///  {
///     "swiftModuleName": "Foo",
///     "arguments": "-target 10.15",
///     "output": "../Foo.json"
///  },
static bool parseBatchInputEntries(ASTContext &Ctx, llvm::StringSaver &saver,
                                   Node *Node,
                                   std::vector<BatchScanInput> &result) {
  auto *SN = cast<SequenceNode>(Node);
  if (!SN)
    return true;
  for (auto It = SN->begin(); It != SN->end(); ++It) {
    auto *MN = cast<MappingNode>(&*It);
    BatchScanInput entry;
    std::optional<std::set<int8_t>> Platforms;
    for (auto &Pair : *MN) {
      auto Key = getScalaNodeText(Pair.getKey());
      auto *Value = Pair.getValue();
      if (Key == "clangModuleName") {
        entry.moduleName = saver.save(getScalaNodeText(Value));
        entry.isSwift = false;
      } else if (Key == "swiftModuleName") {
        entry.moduleName = saver.save(getScalaNodeText(Value));
        entry.isSwift = true;
      } else if (Key == "arguments") {
        entry.arguments = saver.save(getScalaNodeText(Value));
      } else if (Key == "output") {
        entry.outputPath = saver.save(getScalaNodeText(Value));
      } else {
        // Future proof.
        continue;
      }
    }
    if (entry.moduleName.empty())
      return true;
    if (entry.outputPath.empty())
      return true;
    result.emplace_back(std::move(entry));
  }
  return false;
}

static std::optional<std::vector<BatchScanInput>>
parseBatchScanInputFile(ASTContext &ctx, StringRef batchInputPath,
                        llvm::StringSaver &saver) {
  assert(!batchInputPath.empty());
  namespace yaml = llvm::yaml;
  std::vector<BatchScanInput> result;

  // Load the input file.
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      llvm::MemoryBuffer::getFile(batchInputPath);
  if (!FileBufOrErr) {
    ctx.Diags.diagnose(SourceLoc(), diag::batch_scan_input_file_missing,
                       batchInputPath);
    return std::nullopt;
  }
  StringRef Buffer = FileBufOrErr->get()->getBuffer();

  // Use a new source manager instead of the one from ASTContext because we
  // don't want the Json file to be persistent.
  SourceManager SM;
  yaml::Stream Stream(llvm::MemoryBufferRef(Buffer, batchInputPath),
                      SM.getLLVMSourceMgr());
  for (auto DI = Stream.begin(); DI != Stream.end(); ++DI) {
    assert(DI != Stream.end() && "Failed to read a document");
    yaml::Node *N = DI->getRoot();
    assert(N && "Failed to find a root");
    if (parseBatchInputEntries(ctx, saver, N, result)) {
      ctx.Diags.diagnose(SourceLoc(), diag::batch_scan_input_file_corrupted,
                         batchInputPath);
      return std::nullopt;
    }
  }
  return result;
}

static llvm::Expected<llvm::cas::ObjectRef>
updateModuleCacheKey(ModuleDependencyInfo &depInfo,
                     ModuleDependenciesCache &cache,
                     llvm::cas::ObjectStore &CAS) {
  auto commandLine = depInfo.getCommandline();
  std::vector<const char *> Args;
  if (commandLine.size() > 1)
    for (auto &c : ArrayRef<std::string>(commandLine).drop_front(1))
      Args.push_back(c.c_str());

  auto base = createCompileJobBaseCacheKey(CAS, Args);
  if (!base)
    return base.takeError();

  std::string InputPath;
  if (auto *dep = depInfo.getAsClangModule())
    InputPath = dep->moduleMapFile;
  else if (auto *dep = depInfo.getAsSwiftInterfaceModule())
    InputPath = dep->swiftInterfaceFile;
  else
    llvm_unreachable("Unhandled dependency kind");

  if (cache.getScanService().hasPathMapping())
    InputPath = cache.getScanService().remapPath(InputPath);

  // Module compilation commands always have only one input and the input
  // index is always 0.
  auto key = createCompileJobCacheKeyForOutput(CAS, *base, /*InputIndex=*/0);
  if (!key)
    return key.takeError();

  depInfo.updateModuleCacheKey(CAS.getID(*key).toString());
  return *key;
}

static llvm::Error resolveExplicitModuleInputs(
    ModuleDependencyID moduleID, const ModuleDependencyInfo &resolvingDepInfo,
    const std::set<ModuleDependencyID> &dependencies,
    ModuleDependenciesCache &cache, CompilerInstance &instance,
    std::optional<std::set<ModuleDependencyID>> bridgingHeaderDeps) {
  // Only need to resolve dependency for following dependencies.
  if (moduleID.Kind == ModuleDependencyKind::SwiftPlaceholder)
    return llvm::Error::success();

  // If the dependency is already finalized, nothing needs to be done.
  if (resolvingDepInfo.isFinalized())
    return llvm::Error::success();

  auto &service = cache.getScanService();
  auto remapPath = [&](StringRef path) { return service.remapPath(path); };
  std::vector<std::string> rootIDs;
  std::vector<std::string> includeTrees;
  if (auto ID = resolvingDepInfo.getClangIncludeTree())
    includeTrees.push_back(*ID);

  auto tracker = cache.getScanService().createSwiftDependencyTracker();
  auto addBridgingHeaderDeps =
      [&](const ModuleDependencyInfo &depInfo) -> llvm::Error {
    auto sourceDepDetails = depInfo.getAsSwiftSourceModule();
    if (!sourceDepDetails)
      return llvm::Error::success();

    if (sourceDepDetails->textualModuleDetails
            .CASBridgingHeaderIncludeTreeRootID.empty()) {
      if (!sourceDepDetails->textualModuleDetails.bridgingSourceFiles.empty()) {
        if (tracker) {
          tracker->startTracking();
          for (auto &file :
               sourceDepDetails->textualModuleDetails.bridgingSourceFiles)
            tracker->trackFile(file);
          auto bridgeRoot = tracker->createTreeFromDependencies();
          if (!bridgeRoot)
            return bridgeRoot.takeError();
          rootIDs.push_back(bridgeRoot->getID().toString());
        }
      }
    } else
      includeTrees.push_back(sourceDepDetails->textualModuleDetails
                                 .CASBridgingHeaderIncludeTreeRootID);
    return llvm::Error::success();
  };
  if (auto E = addBridgingHeaderDeps(resolvingDepInfo))
    return E;

  std::vector<std::string> commandLine = resolvingDepInfo.getCommandline();
  for (const auto &depModuleID : dependencies) {
    const auto optionalDepInfo =
        cache.findDependency(depModuleID);
    assert(optionalDepInfo.has_value());
    const auto depInfo = optionalDepInfo.value();
    switch (depModuleID.Kind) {
    case swift::ModuleDependencyKind::SwiftInterface: {
      auto interfaceDepDetails = depInfo->getAsSwiftInterfaceModule();
      assert(interfaceDepDetails && "Expected Swift Interface dependency.");
      auto &path = interfaceDepDetails->moduleCacheKey.empty()
                       ? interfaceDepDetails->moduleOutputPath
                       : interfaceDepDetails->moduleCacheKey;
      commandLine.push_back("-swift-module-file=" + depModuleID.ModuleName + "=" +
                            path);
    } break;
    case swift::ModuleDependencyKind::SwiftBinary: {
      auto binaryDepDetails = depInfo->getAsSwiftBinaryModule();
      assert(binaryDepDetails && "Expected Swift Binary Module dependency.");
      auto &path = binaryDepDetails->moduleCacheKey.empty()
                       ? binaryDepDetails->compiledModulePath
                       : binaryDepDetails->moduleCacheKey;
      commandLine.push_back("-swift-module-file=" + depModuleID.ModuleName + "=" +
                            path);
    } break;
    case swift::ModuleDependencyKind::SwiftPlaceholder: {
      auto placeholderDetails = depInfo->getAsPlaceholderDependencyModule();
      assert(placeholderDetails && "Expected Swift Placeholder dependency.");
      commandLine.push_back("-swift-module-file=" + depModuleID.ModuleName + "=" +
                            placeholderDetails->compiledModulePath);
    } break;
    case swift::ModuleDependencyKind::Clang: {
      auto clangDepDetails = depInfo->getAsClangModule();
      assert(clangDepDetails && "Expected Clang Module dependency.");
      if (!resolvingDepInfo.isClangModule()) {
        commandLine.push_back("-Xcc");
        commandLine.push_back("-fmodule-file=" + depModuleID.ModuleName + "=" +
                              clangDepDetails->mappedPCMPath);
        if (!instance.getInvocation()
                 .getClangImporterOptions()
                 .UseClangIncludeTree) {
          commandLine.push_back("-Xcc");
          commandLine.push_back("-fmodule-map-file=" +
                                remapPath(clangDepDetails->moduleMapFile));
        }
      }
      if (!clangDepDetails->moduleCacheKey.empty()) {
        commandLine.push_back("-Xcc");
        commandLine.push_back("-fmodule-file-cache-key");
        commandLine.push_back("-Xcc");
        commandLine.push_back(clangDepDetails->mappedPCMPath);
        commandLine.push_back("-Xcc");
        commandLine.push_back(clangDepDetails->moduleCacheKey);
      }

      // Only need to merge the CASFS from clang importer.
      if (auto ID = depInfo->getCASFSRootID())
        rootIDs.push_back(*ID);
      if (auto ID = depInfo->getClangIncludeTree())
        includeTrees.push_back(*ID);
    } break;
    case swift::ModuleDependencyKind::SwiftSource: {
      if (auto E = addBridgingHeaderDeps(*depInfo))
        return E;
      break;
    }
    default:
      llvm_unreachable("Unhandled dependency kind.");
    }
  }

  // Update the dependency in the cache with the modified command-line.
  auto dependencyInfoCopy = resolvingDepInfo;
  if (resolvingDepInfo.isSwiftInterfaceModule() ||
      resolvingDepInfo.isClangModule()) {
    if (service.hasPathMapping())
      commandLine = remapPathsFromCommandLine(commandLine, remapPath);
    dependencyInfoCopy.updateCommandLine(commandLine);
  }

  // Handle CAS options.
  if (instance.getInvocation().getCASOptions().EnableCaching) {
    // Merge CASFS from clang dependency.
    auto &CASFS = cache.getScanService().getSharedCachingFS();
    auto &CAS = CASFS.getCAS();

    assert(tracker && "no caching tracker is available");
    // Compute the CASFS root ID for the resolving dependency.
    if (auto *sourceDep = resolvingDepInfo.getAsSwiftSourceModule()) {
      tracker->startTracking();
      tracker->addCommonSearchPathDeps(
          instance.getInvocation().getSearchPathOptions());
      llvm::for_each(
          sourceDep->sourceFiles,
          [&tracker](const std::string &file) { tracker->trackFile(file); });
      llvm::for_each(
          sourceDep->auxiliaryFiles,
          [&tracker](const std::string &file) { tracker->trackFile(file); });
      auto root = tracker->createTreeFromDependencies();
      if (!root)
        return root.takeError();
      auto rootID = root->getID().toString();
      dependencyInfoCopy.updateCASFileSystemRootID(rootID);
      rootIDs.push_back(rootID);
    } else if (auto *textualDep =
                   resolvingDepInfo.getAsSwiftInterfaceModule()) {
      tracker->startTracking();
      tracker->addCommonSearchPathDeps(
          instance.getInvocation().getSearchPathOptions());
      tracker->trackFile(textualDep->swiftInterfaceFile);
      llvm::for_each(
          textualDep->auxiliaryFiles,
          [&tracker](const std::string &file) { tracker->trackFile(file); });
      auto root = tracker->createTreeFromDependencies();
      if (!root)
        return root.takeError();
      auto rootID = root->getID().toString();
      dependencyInfoCopy.updateCASFileSystemRootID(rootID);
      rootIDs.push_back(rootID);
    }

    // Update build command line.
    if (resolvingDepInfo.isSwiftInterfaceModule() ||
        resolvingDepInfo.isSwiftSourceModule()) {
      // Update with casfs option.
      std::vector<std::string> newCommandLine =
          dependencyInfoCopy.getCommandline();
      for (auto rootID : rootIDs) {
        newCommandLine.push_back("-cas-fs");
        newCommandLine.push_back(rootID);
      }

      for (auto tree : includeTrees) {
        newCommandLine.push_back("-clang-include-tree-root");
        newCommandLine.push_back(tree);
      }
      dependencyInfoCopy.updateCommandLine(newCommandLine);
    }

    if (bridgingHeaderDeps) {
      std::vector<std::string> newCommandLine =
          dependencyInfoCopy.getBridgingHeaderCommandline();
      for (auto bridgingDep : *bridgingHeaderDeps) {
        auto dep = cache.findDependency(bridgingDep);
        assert(dep && "unknown clang dependency");
        auto *clangDep = (*dep)->getAsClangModule();
        assert(clangDep && "wrong module dependency kind");
        if (!clangDep->moduleCacheKey.empty()) {
          newCommandLine.push_back("-Xcc");
          newCommandLine.push_back("-fmodule-file-cache-key");
          newCommandLine.push_back("-Xcc");
          newCommandLine.push_back(clangDep->mappedPCMPath);
          newCommandLine.push_back("-Xcc");
          newCommandLine.push_back(clangDep->moduleCacheKey);
        }
      }
      dependencyInfoCopy.updateBridgingHeaderCommandLine(newCommandLine);
    }

    if (resolvingDepInfo.isClangModule() ||
        resolvingDepInfo.isSwiftInterfaceModule()) {
      // Compute and update module cache key.
      auto Key = updateModuleCacheKey(dependencyInfoCopy, cache, CAS);
      if (!Key)
        return Key.takeError();
    }

    // For binary module, we need to make sure the lookup key is setup here in
    // action cache. We just use the CASID of the binary module itself as key.
    if (auto *binaryDep = dependencyInfoCopy.getAsSwiftBinaryModule()) {
      auto Ref =
          CASFS.getObjectRefForFileContent(binaryDep->compiledModulePath);
      if (!Ref)
        return llvm::errorCodeToError(Ref.getError());
      assert(*Ref && "Binary module should be loaded into CASFS already");
      dependencyInfoCopy.updateModuleCacheKey(CAS.getID(**Ref).toString());

      swift::cas::CompileJobCacheResult::Builder Builder;
      Builder.addOutput(file_types::ID::TY_SwiftModuleFile, **Ref);
      auto Result = Builder.build(CAS);
      if (!Result)
        return Result.takeError();
      if (auto E = instance.getActionCache().put(CAS.getID(**Ref),
                                                 CAS.getID(*Result)))
        return E;
    }
  }
  dependencyInfoCopy.setIsFinalized(true);
  cache.updateDependency(moduleID, dependencyInfoCopy);

  return llvm::Error::success();
}

static llvm::Error pruneUnusedVFSOverlays(
    ModuleDependencyID moduleID, const ModuleDependencyInfo &resolvingDepInfo,
    const std::set<ModuleDependencyID> &dependencies,
    ModuleDependenciesCache &cache, CompilerInstance &instance) {
  auto isVFSOverlayFlag = [](StringRef arg) {
    return arg == "-ivfsoverlay" || arg == "-vfsoverlay";
  };
  auto isXCCArg = [](StringRef arg) {
    return arg == "-Xcc";
  };

  // Pruning of unused VFS overlay options for Clang dependencies
  // is performed by the Clang dependency scanner.
  if (!resolvingDepInfo.isSwiftModule())
    return llvm::Error::success();

  // If this Swift dependency contains any VFS overlay paths,
  // then attempt to prune the ones not used by any of the Clang dependencies.
  if (!llvm::any_of(resolvingDepInfo.getCommandline(),
                    [&isVFSOverlayFlag](const std::string &arg) {
                      return isVFSOverlayFlag(arg);
                    }))
    return llvm::Error::success();

  // 1. For each Clang dependency, gather its ivfsoverlay path arguments
  // to keep track of which overlays are actually used and were not
  // pruned by the Clang dependency scanner.
  llvm::StringSet<> usedVFSOverlayPaths;
  for (const auto &depModuleID : dependencies) {
    const auto optionalDepInfo = cache.findDependency(depModuleID);
    assert(optionalDepInfo.has_value());
    const auto depInfo = optionalDepInfo.value();
    if (auto clangDepDetails = depInfo->getAsClangModule()) {
      const auto &depCommandLine = clangDepDetails->buildCommandLine;
      // true if the previous argument was the dash-option of an option pair
      bool getNext = false;
      for (const auto &A : depCommandLine) {
        StringRef arg(A);
        if (isXCCArg(arg))
          continue;
        if (getNext) {
          getNext = false;
          usedVFSOverlayPaths.insert(arg);
        } else if (isVFSOverlayFlag(arg))
          getNext = true;
      }
    }
  }

  // 2. Each -Xcc VFS overlay path on the resolving command-line which is not used by
  // any of the Clang dependencies can be removed from the command-line.
  const std::vector<std::string> &currentCommandLine =
      resolvingDepInfo.getCommandline();
  std::vector<std::string> resolvedCommandLine;
  size_t skip = 0;
  for (auto it = currentCommandLine.begin(), end = currentCommandLine.end();
       it != end; it++) {
    if (skip) {
      skip--;
      continue;
    }
    // If this VFS overlay was not used across any of the dependencies, skip it.
    if ((it+1) != end && isXCCArg(*it) && isVFSOverlayFlag(*(it + 1))) {
      assert(it + 2 != end); // Extra -Xcc
      assert(it + 3 != end); // Actual VFS overlay path argument
      if (!usedVFSOverlayPaths.contains(*(it + 3))) {
        skip = 3;
        continue;
      }
    }
    resolvedCommandLine.push_back(*it);
  }

  // 3. Update the dependency in the cache if the command-line has been modified.
  if (currentCommandLine.size() != resolvedCommandLine.size()) {
    auto dependencyInfoCopy = resolvingDepInfo;
    dependencyInfoCopy.updateCommandLine(resolvedCommandLine);

    // Update the CAS cache key for the new command-line
    if (instance.getInvocation().getCASOptions().EnableCaching) {
      auto &CAS = cache.getScanService().getSharedCachingFS().getCAS();
      auto Key = updateModuleCacheKey(dependencyInfoCopy, cache, CAS);
      if (!Key)
        return Key.takeError();
    }
    cache.updateDependency(moduleID, dependencyInfoCopy);
  }

  return llvm::Error::success();
}

namespace {
std::string quote(StringRef unquoted) {
  llvm::SmallString<128> buffer;
  llvm::raw_svector_ostream os(buffer);
  for (const auto ch : unquoted) {
    if (ch == '\\')
      os << '\\';
    if (ch == '"')
      os << '\\';
    os << ch;
  }
  return buffer.str().str();
}
}

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested = false);

/// Write a string value as JSON.
void writeJSONValue(llvm::raw_ostream &out, StringRef value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(value);
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_ref_t value,
                    unsigned indentLevel) {
  out << "\"";
  out << quote(get_C_string(value));
  out << "\"";
}

void writeJSONValue(llvm::raw_ostream &out, swiftscan_string_set_t *value_set,
                    unsigned indentLevel) {
  out << "[\n";

  for (size_t i = 0; i < value_set->count; ++i) {
    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value_set->strings[i], indentLevel + 1);

    if (i != value_set->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

void writeEncodedModuleIdJSONValue(llvm::raw_ostream &out,
                                   swiftscan_string_ref_t value,
                                   unsigned indentLevel) {
  out << "{\n";
  static const std::string textualPrefix("swiftTextual");
  static const std::string binaryPrefix("swiftBinary");
  static const std::string placeholderPrefix("swiftPlaceholder");
  static const std::string clangPrefix("clang");
  std::string valueStr = get_C_string(value);
  std::string moduleKind;
  std::string moduleName;
  if (!valueStr.compare(0, textualPrefix.size(), textualPrefix)) {
    moduleKind = "swift";
    moduleName = valueStr.substr(textualPrefix.size() + 1);
  } else if (!valueStr.compare(0, binaryPrefix.size(), binaryPrefix)) {
    // FIXME: rename to be consistent in the clients (swift-driver)
    moduleKind = "swiftPrebuiltExternal";
    moduleName = valueStr.substr(binaryPrefix.size() + 1);
  } else if (!valueStr.compare(0, placeholderPrefix.size(),
                               placeholderPrefix)) {
    moduleKind = "swiftPlaceholder";
    moduleName = valueStr.substr(placeholderPrefix.size() + 1);
  } else {
    moduleKind = "clang";
    moduleName = valueStr.substr(clangPrefix.size() + 1);
  }
  writeJSONSingleField(out, moduleKind, moduleName, indentLevel + 1,
                       /*trailingComma=*/false);
  out.indent(indentLevel * 2);
  out << "}";
}

/// Write a boolean value as JSON.
void writeJSONValue(llvm::raw_ostream &out, bool value, unsigned indentLevel) {
  out.write_escaped(value ? "true" : "false");
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, ArrayRef<T> values,
                    unsigned indentLevel) {
  out << "[\n";

  for (const auto &value : values) {

    out.indent((indentLevel + 1) * 2);

    writeJSONValue(out, value, indentLevel + 1);

    if (&value != &values.back()) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";
}

/// Write a JSON array.
template <typename T>
void writeJSONValue(llvm::raw_ostream &out, const std::vector<T> &values,
                    unsigned indentLevel) {
  writeJSONValue(out, llvm::ArrayRef(values), indentLevel);
}

/// Write a single JSON field.
template <typename T>
void writeJSONSingleField(llvm::raw_ostream &out, StringRef fieldName,
                          const T &value, unsigned indentLevel,
                          bool trailingComma, bool nested) {
  out.indent(indentLevel * 2);
  writeJSONValue(out, fieldName, indentLevel);
  out << ": ";
  auto updatedIndentLevel = indentLevel;
  
  if (nested) {
    // This is a hack to "fix" a format for a value that should be a nested
    // set of strings. Currently only capturedPCMArgs (clang) is expected to
    // in the nested format, which supposedly only contains one set of strings.
    // Adjust the indentation to account for the nested brackets.
    updatedIndentLevel += 1;
    out << "[\n";
    out.indent(updatedIndentLevel * 2);
  }

  writeJSONValue(out, value, updatedIndentLevel);

  if (nested) {
    // If nested, add an extra closing brack with a correct indentation.
    out << "\n";
    out.indent(indentLevel * 2);
    out << "]";
  }

  if (trailingComma)
    out << ",";
  out << "\n";
}

void writeDependencies(llvm::raw_ostream &out,
                       const swiftscan_string_set_t *dependencies,
                       std::string dependenciesKind,
                       unsigned indentLevel, bool trailingComma) {
  out.indent(indentLevel * 2);
  out << "\"" + dependenciesKind + "\": ";
  out << "[\n";

  for (size_t i = 0; i < dependencies->count; ++i) {
    out.indent((indentLevel + 1) * 2);
    writeEncodedModuleIdJSONValue(out, dependencies->strings[i],
                                  indentLevel + 1);
    if (i != dependencies->count - 1) {
      out << ",";
    }
    out << "\n";
  }

  out.indent(indentLevel * 2);
  out << "]";

  if (trailingComma)
    out << ",";
  out << "\n";
}

static const swiftscan_swift_textual_details_t *
getAsTextualDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL)
    return &details->swift_textual_details;
  return nullptr;
}

static const swiftscan_swift_placeholder_details_t *
getAsPlaceholderDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER)
    return &details->swift_placeholder_details;
  return nullptr;
}

static const swiftscan_swift_binary_details_t *
getAsBinaryDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY)
    return &details->swift_binary_details;
  return nullptr;
}

static const swiftscan_clang_details_t *
getAsClangDependencyModule(swiftscan_module_details_t details) {
  if (details->kind == SWIFTSCAN_DEPENDENCY_INFO_CLANG)
    return &details->clang_details;
  return nullptr;
}

static void writePrescanJSON(llvm::raw_ostream &out,
                             const swiftscan_import_set_t importSet) {
  // Write out a JSON containing all main module imports.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };

  writeJSONSingleField(out, "imports", importSet->imports, 0, false);
}

static void writeJSON(llvm::raw_ostream &out,
                      const swiftscan_dependency_graph_t fullDependencies) {
  // Write out a JSON description of all of the dependencies.
  out << "{\n";
  SWIFT_DEFER { out << "}\n"; };
  // Name of the main module.
  writeJSONSingleField(out, "mainModuleName",
                       fullDependencies->main_module_name,
                       /*indentLevel=*/1, /*trailingComma=*/true);
  // Write out all of the modules.
  out << "  \"modules\": [\n";
  SWIFT_DEFER { out << "  ]\n"; };
  const auto module_set = fullDependencies->dependencies;
  for (size_t mi = 0; mi < module_set->count; ++mi) {
    const auto &moduleInfo = *module_set->modules[mi];
    auto &directDependencies = moduleInfo.direct_dependencies;
    // The module we are describing.
    out.indent(2 * 2);
    writeEncodedModuleIdJSONValue(out, moduleInfo.module_name, 2);
    out << ",\n";
    out.indent(2 * 2);
    out << "{\n";
    auto swiftPlaceholderDeps =
        getAsPlaceholderDependencyModule(moduleInfo.details);
    auto swiftTextualDeps = getAsTextualDependencyModule(moduleInfo.details);
    auto swiftBinaryDeps = getAsBinaryDependencyModule(moduleInfo.details);
    auto clangDeps = getAsClangDependencyModule(moduleInfo.details);

    // Module path.
    const char *modulePathSuffix = clangDeps ? ".pcm" : ".swiftmodule";

    std::string modulePath;
    std::string moduleKindAndName =
        std::string(get_C_string(moduleInfo.module_name));
    std::string moduleName =
        moduleKindAndName.substr(moduleKindAndName.find(":") + 1);
    if (swiftPlaceholderDeps)
      modulePath = get_C_string(swiftPlaceholderDeps->compiled_module_path);
    else if (swiftBinaryDeps)
      modulePath = get_C_string(swiftBinaryDeps->compiled_module_path);
    else if (clangDeps || swiftTextualDeps)
      modulePath = get_C_string(moduleInfo.module_path);
    else
      modulePath = moduleName + modulePathSuffix;

    writeJSONSingleField(out, "modulePath", modulePath, /*indentLevel=*/3,
                         /*trailingComma=*/true);

    // Source files.
    if (swiftTextualDeps || clangDeps) {
      writeJSONSingleField(out, "sourceFiles", moduleInfo.source_files, 3,
                           /*trailingComma=*/true);
    }

    // Direct dependencies.
    if (swiftTextualDeps || swiftBinaryDeps || clangDeps)
      writeDependencies(out, directDependencies,
                        "directDependencies", 3,
                        /*trailingComma=*/true);
    // Swift and Clang-specific details.
    out.indent(3 * 2);
    out << "\"details\": {\n";
    out.indent(4 * 2);
    if (swiftTextualDeps) {
      out << "\"swift\": {\n";
      /// Swift interface file, if there is one. The main module, for
      /// example, will not have an interface file.
      std::string moduleInterfacePath =
          swiftTextualDeps->module_interface_path.data
              ? get_C_string(swiftTextualDeps->module_interface_path)
              : "";
      if (!moduleInterfacePath.empty()) {
        writeJSONSingleField(out, "moduleInterfacePath", moduleInterfacePath, 5,
                             /*trailingComma=*/true);
        out.indent(5 * 2);
        out << "\"compiledModuleCandidates\": [\n";
        for (int i = 0,
                 count = swiftTextualDeps->compiled_module_candidates->count;
             i < count; ++i) {
          const auto &candidate = get_C_string(
              swiftTextualDeps->compiled_module_candidates->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(candidate) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << "],\n";
      }
      out.indent(5 * 2);
      out << "\"commandLine\": [\n";
      for (int i = 0, count = swiftTextualDeps->command_line->count; i < count;
           ++i) {
        const auto &arg =
            get_C_string(swiftTextualDeps->command_line->strings[i]);
        out.indent(6 * 2);
        out << "\"" << quote(arg) << "\"";
        if (i != count - 1)
          out << ",";
        out << "\n";
      }
      out.indent(5 * 2);
      out << "],\n";
      writeJSONSingleField(out, "contextHash", swiftTextualDeps->context_hash,
                           5,
                           /*trailingComma=*/true);
      bool hasBridgingHeaderPath =
          swiftTextualDeps->bridging_header_path.data &&
          get_C_string(swiftTextualDeps->bridging_header_path)[0] != '\0';
      bool hasOverlayDependencies =
          swiftTextualDeps->swift_overlay_module_dependencies &&
          swiftTextualDeps->swift_overlay_module_dependencies->count > 0;
      bool commaAfterBridgingHeaderPath = hasOverlayDependencies;
      bool commaAfterExtraPcmArgs =
          hasBridgingHeaderPath || commaAfterBridgingHeaderPath;
      bool commaAfterFramework =
          swiftTextualDeps->extra_pcm_args->count != 0 || commaAfterExtraPcmArgs;

      if (swiftTextualDeps->cas_fs_root_id.length != 0) {
        writeJSONSingleField(out, "casFSRootID",
                             swiftTextualDeps->cas_fs_root_id, 5,
                             /*trailingComma=*/true);
      }
      if (swiftTextualDeps->module_cache_key.length != 0) {
        writeJSONSingleField(out, "moduleCacheKey",
                             swiftTextualDeps->module_cache_key, 5,
                             /*trailingComma=*/true);
      }
      writeJSONSingleField(out, "isFramework", swiftTextualDeps->is_framework,
                           5, commaAfterFramework);
      if (swiftTextualDeps->extra_pcm_args->count != 0) {
        out.indent(5 * 2);
        out << "\"extraPcmArgs\": [\n";
        for (int i = 0, count = swiftTextualDeps->extra_pcm_args->count;
             i < count; ++i) {
          const auto &arg =
              get_C_string(swiftTextualDeps->extra_pcm_args->strings[i]);
          out.indent(6 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(5 * 2);
        out << (commaAfterExtraPcmArgs ? "],\n" : "]\n");
      }
      /// Bridging header and its source file dependencies, if any.
      if (hasBridgingHeaderPath) {
        out.indent(5 * 2);
        out << "\"bridgingHeader\": {\n";
        writeJSONSingleField(out, "path",
                             swiftTextualDeps->bridging_header_path, 6,
                             /*trailingComma=*/true);
        writeJSONSingleField(out, "sourceFiles",
                             swiftTextualDeps->bridging_source_files, 6,
                             /*trailingComma=*/true);
        if (swiftTextualDeps->bridging_header_include_tree.length != 0) {
          writeJSONSingleField(out, "includeTree",
                               swiftTextualDeps->bridging_header_include_tree,
                               6, /*trailingComma=*/true);
        }
        writeJSONSingleField(out, "moduleDependencies",
                             swiftTextualDeps->bridging_module_dependencies, 6,
                             /*trailingComma=*/true);
        out.indent(6 * 2);
        out << "\"commandLine\": [\n";
        for (int i = 0,
                 count = swiftTextualDeps->bridging_pch_command_line->count;
             i < count; ++i) {
          const auto &arg = get_C_string(
              swiftTextualDeps->bridging_pch_command_line->strings[i]);
          out.indent(7 * 2);
          out << "\"" << quote(arg) << "\"";
          if (i != count - 1)
            out << ",";
          out << "\n";
        }
        out.indent(6 * 2);
        out << "]\n";
        out.indent(5 * 2);
        out << (commaAfterBridgingHeaderPath ? "},\n" : "}\n");
      }
      if (hasOverlayDependencies) {
        writeDependencies(out, swiftTextualDeps->swift_overlay_module_dependencies,
                          "swiftOverlayDependencies", 5,
                          /*trailingComma=*/false);
      }
    } else if (swiftPlaceholderDeps) {
      out << "\"swiftPlaceholder\": {\n";

      // Module doc file
      if (swiftPlaceholderDeps->module_doc_path.data &&
          get_C_string(swiftPlaceholderDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftPlaceholderDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);

      // Module Source Info file
      if (swiftPlaceholderDeps->module_source_info_path.data &&
          get_C_string(swiftPlaceholderDeps->module_source_info_path)[0] !=
              '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftPlaceholderDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/false);
    } else if (swiftBinaryDeps) {
      bool hasOverlayDependencies =
        swiftBinaryDeps->swift_overlay_module_dependencies &&
        swiftBinaryDeps->swift_overlay_module_dependencies->count > 0;

      out << "\"swiftPrebuiltExternal\": {\n";
      assert(swiftBinaryDeps->compiled_module_path.data &&
             get_C_string(swiftBinaryDeps->compiled_module_path)[0] != '\0' &&
             "Expected .swiftmodule for a Binary Swift Module Dependency.");

      writeJSONSingleField(out, "compiledModulePath",
                           swiftBinaryDeps->compiled_module_path,
                           /*indentLevel=*/5,
                           /*trailingComma=*/true);
      // Module doc file
      if (swiftBinaryDeps->module_doc_path.data &&
          get_C_string(swiftBinaryDeps->module_doc_path)[0] != '\0')
        writeJSONSingleField(out, "moduleDocPath",
                             swiftBinaryDeps->module_doc_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      // Module Source Info file
      if (swiftBinaryDeps->module_source_info_path.data &&
          get_C_string(swiftBinaryDeps->module_source_info_path)[0] != '\0')
        writeJSONSingleField(out, "moduleSourceInfoPath",
                             swiftBinaryDeps->module_source_info_path,
                             /*indentLevel=*/5,
                             /*trailingComma=*/true);
      if (swiftBinaryDeps->module_cache_key.length != 0) {
        writeJSONSingleField(out, "moduleCacheKey",
                             swiftBinaryDeps->module_cache_key, 5,
                             /*trailingComma=*/true);
      }

      // Module Header Dependencies
      if (swiftBinaryDeps->header_dependency.length != 0)
        writeJSONSingleField(out, "headerDependency",
                             swiftBinaryDeps->header_dependency, 5,
                             /*trailingComma=*/true);

      // Module Header Module Dependencies
      if (swiftBinaryDeps->header_dependencies_module_dependnecies->count != 0)
        writeJSONSingleField(out, "headerModuleDependencies",
                             swiftBinaryDeps->header_dependencies_module_dependnecies, 5,
                             /*trailingComma=*/true);

      // Module Header Source Files
      if (swiftBinaryDeps->header_dependencies_source_files->count != 0)
        writeJSONSingleField(out, "headerDependenciesSourceFiles",
                             swiftBinaryDeps->header_dependencies_source_files, 5,
                             /*trailingComma=*/true);

      if (hasOverlayDependencies) {
        writeDependencies(out, swiftBinaryDeps->swift_overlay_module_dependencies,
                          "swiftOverlayDependencies", 5,
                          /*trailingComma=*/true);
      }

      writeJSONSingleField(out, "isFramework", swiftBinaryDeps->is_framework,
                           5, /*trailingComma=*/false);
    } else {
      out << "\"clang\": {\n";

      // Module map file.
      writeJSONSingleField(out, "moduleMapPath", clangDeps->module_map_path, 5,
                           /*trailingComma=*/true);

      // Context hash.
      writeJSONSingleField(out, "contextHash", clangDeps->context_hash, 5,
                           /*trailingComma=*/true);

      // Command line.
      writeJSONSingleField(out, "commandLine", clangDeps->command_line, 5,
                           /*trailingComma=*/true);

      if (clangDeps->cas_fs_root_id.length != 0)
        writeJSONSingleField(out, "casFSRootID", clangDeps->cas_fs_root_id, 5,
                             /*trailingComma=*/true);
      if (clangDeps->clang_include_tree.length != 0)
        writeJSONSingleField(out, "clangIncludeTree",
                             clangDeps->clang_include_tree, 5,
                             /*trailingComma=*/true);
      if (clangDeps->module_cache_key.length != 0)
        writeJSONSingleField(out, "moduleCacheKey", clangDeps->module_cache_key,
                             5,
                             /*trailingComma=*/true);

      // Captured PCM arguments.
      writeJSONSingleField(out, "capturedPCMArgs", clangDeps->captured_pcm_args, 5,
                           /*trailingComma=*/false, /*nested=*/true);

    }

    out.indent(4 * 2);
    out << "}\n";
    out.indent(3 * 2);
    out << "}\n";
    out.indent(2 * 2);
    out << "}";

    if (mi != module_set->count - 1)
      out << ",";
    out << "\n";
  }
}

static bool writePrescanJSONToOutput(DiagnosticEngine &diags,
                                     llvm::vfs::OutputBackend &backend,
                                     StringRef path,
                                     const swiftscan_import_set_t importSet) {
  return withOutputPath(diags, backend, path, [&](llvm::raw_pwrite_stream &os) {
    writePrescanJSON(os, importSet);
    return false;
  });
}

static bool writeJSONToOutput(DiagnosticEngine &diags,
                              llvm::vfs::OutputBackend &backend, StringRef path,
                              const swiftscan_dependency_graph_t dependencies) {
  return withOutputPath(diags, backend, path, [&](llvm::raw_pwrite_stream &os) {
    writeJSON(os, dependencies);
    return false;
  });
}

static void bridgeDependencyIDs(const ArrayRef<ModuleDependencyID> dependencies,
                                std::vector<std::string> &bridgedDependencyNames) {
  for (const auto &dep : dependencies) {
    std::string dependencyKindAndName;
    switch (dep.Kind) {
    case ModuleDependencyKind::SwiftInterface:
    case ModuleDependencyKind::SwiftSource:
      dependencyKindAndName = "swiftTextual";
      break;
    case ModuleDependencyKind::SwiftBinary:
      dependencyKindAndName = "swiftBinary";
      break;
    case ModuleDependencyKind::SwiftPlaceholder:
      dependencyKindAndName = "swiftPlaceholder";
      break;
    case ModuleDependencyKind::Clang:
      dependencyKindAndName = "clang";
      break;
    default:
      llvm_unreachable("Unhandled dependency kind.");
    }
    dependencyKindAndName += ":";
    dependencyKindAndName += dep.ModuleName;
    bridgedDependencyNames.push_back(dependencyKindAndName);
  }
}

static swiftscan_diagnostic_set_t *mapCollectedDiagnosticsForOutput(
    const DependencyScanDiagnosticCollector *diagnosticCollector) {
  auto collectedDiagnostics = diagnosticCollector->getDiagnostics();
  auto numDiagnostics = collectedDiagnostics.size();
  swiftscan_diagnostic_set_t *diagnosticOutput = new swiftscan_diagnostic_set_t;
  diagnosticOutput->count = numDiagnostics;
  diagnosticOutput->diagnostics =
      new swiftscan_diagnostic_info_t[numDiagnostics];
  for (size_t i = 0; i < numDiagnostics; ++i) {
    const auto &Diagnostic = collectedDiagnostics[i];
    swiftscan_diagnostic_info_s *diagnosticInfo =
        new swiftscan_diagnostic_info_s;
    diagnosticInfo->message =
        swift::c_string_utils::create_clone(Diagnostic.Message.c_str());
    switch (Diagnostic.Severity) {
    case llvm::SourceMgr::DK_Error:
      diagnosticInfo->severity = SWIFTSCAN_DIAGNOSTIC_SEVERITY_ERROR;
      break;
    case llvm::SourceMgr::DK_Warning:
      diagnosticInfo->severity = SWIFTSCAN_DIAGNOSTIC_SEVERITY_WARNING;
      break;
    case llvm::SourceMgr::DK_Note:
      diagnosticInfo->severity = SWIFTSCAN_DIAGNOSTIC_SEVERITY_NOTE;
      break;
    case llvm::SourceMgr::DK_Remark:
      diagnosticInfo->severity = SWIFTSCAN_DIAGNOSTIC_SEVERITY_REMARK;
      break;
    }
    diagnosticOutput->diagnostics[i] = diagnosticInfo;
  }
  return diagnosticOutput;
}

static swiftscan_dependency_graph_t
generateFullDependencyGraph(const CompilerInstance &instance,
                            const DependencyScanDiagnosticCollector *diagnosticCollector,
                            const ModuleDependenciesCache &cache,
                            const ArrayRef<ModuleDependencyID> allModules) {
  if (allModules.empty()) {
    return nullptr;
  }

  std::string mainModuleName = allModules.front().ModuleName;
  swiftscan_dependency_set_t *dependencySet = new swiftscan_dependency_set_t;
  dependencySet->count = allModules.size();
  dependencySet->modules =
      new swiftscan_dependency_info_t[dependencySet->count];

  for (size_t i = 0; i < allModules.size(); ++i) {
    const auto &module = allModules[i];
    // Grab the completed module dependencies.
    auto moduleDepsQuery = cache.findDependency(module);
    if (!moduleDepsQuery) {
      llvm::report_fatal_error(Twine("Module Dependency Cache missing module") +
                               module.ModuleName);
    }

    auto moduleDeps = *moduleDepsQuery;
    // Collect all the required pieces to build a ModuleInfo
    auto swiftPlaceholderDeps = moduleDeps->getAsPlaceholderDependencyModule();
    auto swiftTextualDeps = moduleDeps->getAsSwiftInterfaceModule();
    auto swiftSourceDeps = moduleDeps->getAsSwiftSourceModule();
    auto swiftBinaryDeps = moduleDeps->getAsSwiftBinaryModule();
    auto clangDeps = moduleDeps->getAsClangModule();

    // ModulePath
    const char *modulePathSuffix =
        moduleDeps->isSwiftModule() ? ".swiftmodule" : ".pcm";
    std::string modulePath;
    if (swiftTextualDeps)
      modulePath = swiftTextualDeps->moduleOutputPath;
    else if (swiftPlaceholderDeps)
      modulePath = swiftPlaceholderDeps->compiledModulePath;
    else if (swiftBinaryDeps)
      modulePath = swiftBinaryDeps->compiledModulePath;
    else if (clangDeps)
      modulePath = clangDeps->pcmOutputPath;
    else
      modulePath = module.ModuleName + modulePathSuffix;

    // SourceFiles
    std::vector<std::string> sourceFiles;
    if (swiftSourceDeps) {
      sourceFiles = swiftSourceDeps->sourceFiles;
    } else if (clangDeps) {
      sourceFiles = clangDeps->fileDependencies;
    }

    auto optionalDepInfo = cache.findDependency(module);
    assert(optionalDepInfo.has_value() && "Missing dependency info during graph generation diagnosis.");
    auto depInfo = optionalDepInfo.value();
    auto directDependencies = depInfo->getDirectModuleDependencies();

    // Generate a swiftscan_clang_details_t object based on the dependency kind
    auto getModuleDetails = [&]() -> swiftscan_module_details_t {
      swiftscan_module_details_s *details = new swiftscan_module_details_s;
      if (swiftTextualDeps) {
        swiftscan_string_ref_t moduleInterfacePath =
            create_clone(swiftTextualDeps->swiftInterfaceFile.c_str());
        swiftscan_string_ref_t bridgingHeaderPath =
            swiftTextualDeps->textualModuleDetails.bridgingHeaderFile.has_value()
                ? create_clone(
                      swiftTextualDeps->textualModuleDetails.bridgingHeaderFile.value().c_str())
                : create_null();
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL;
        // Create an overlay dependencies set according to the output format
        std::vector<std::string> bridgedOverlayDependencyNames;
        bridgeDependencyIDs(swiftTextualDeps->swiftOverlayDependencies,
                            bridgedOverlayDependencyNames);

        details->swift_textual_details = {
            moduleInterfacePath,
            create_set(swiftTextualDeps->compiledModuleCandidates),
            bridgingHeaderPath,
            create_set(
                swiftTextualDeps->textualModuleDetails.bridgingSourceFiles),
            create_set(swiftTextualDeps->textualModuleDetails
                           .bridgingModuleDependencies),
            create_set(bridgedOverlayDependencyNames),
            create_set(swiftTextualDeps->textualModuleDetails.buildCommandLine),
            /*bridgingHeaderBuildCommand*/ create_set({}),
            create_set(swiftTextualDeps->textualModuleDetails.extraPCMArgs),
            create_clone(swiftTextualDeps->contextHash.c_str()),
            swiftTextualDeps->isFramework,
            create_clone(swiftTextualDeps->textualModuleDetails
                             .CASFileSystemRootID.c_str()),
            create_clone(swiftTextualDeps->textualModuleDetails
                             .CASBridgingHeaderIncludeTreeRootID.c_str()),
            create_clone(swiftTextualDeps->moduleCacheKey.c_str())};
      } else if (swiftSourceDeps) {
        swiftscan_string_ref_t moduleInterfacePath = create_null();
        swiftscan_string_ref_t bridgingHeaderPath =
          swiftSourceDeps->textualModuleDetails.bridgingHeaderFile.has_value()
                ? create_clone(
                           swiftSourceDeps->textualModuleDetails.bridgingHeaderFile.value().c_str())
                : create_null();
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_TEXTUAL;
        // Create an overlay dependencies set according to the output format
        std::vector<std::string> bridgedOverlayDependencyNames;
        bridgeDependencyIDs(swiftSourceDeps->swiftOverlayDependencies,
                            bridgedOverlayDependencyNames);

        details->swift_textual_details = {
            moduleInterfacePath, create_empty_set(), bridgingHeaderPath,
            create_set(
                swiftSourceDeps->textualModuleDetails.bridgingSourceFiles),
            create_set(swiftSourceDeps->textualModuleDetails
                           .bridgingModuleDependencies),
            create_set(bridgedOverlayDependencyNames),
            create_set(swiftSourceDeps->textualModuleDetails.buildCommandLine),
            create_set(swiftSourceDeps->bridgingHeaderBuildCommandLine),
            create_set(swiftSourceDeps->textualModuleDetails.extraPCMArgs),
            /*contextHash*/
            create_clone(
                instance.getInvocation().getModuleScanningHash().c_str()),
            /*isFramework*/ false,
            /*CASFS*/
            create_clone(swiftSourceDeps->textualModuleDetails
                             .CASFileSystemRootID.c_str()),
            /*IncludeTree*/
            create_clone(swiftSourceDeps->textualModuleDetails
                             .CASBridgingHeaderIncludeTreeRootID.c_str()),
            /*CacheKey*/ create_clone("")};
      } else if (swiftPlaceholderDeps) {
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_PLACEHOLDER;
        details->swift_placeholder_details = {
            create_clone(swiftPlaceholderDeps->compiledModulePath.c_str()),
            create_clone(swiftPlaceholderDeps->moduleDocPath.c_str()),
            create_clone(swiftPlaceholderDeps->sourceInfoPath.c_str())};
      } else if (swiftBinaryDeps) {
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_SWIFT_BINARY;
        // Create an overlay dependencies set according to the output format
        std::vector<std::string> bridgedOverlayDependencyNames;
        bridgeDependencyIDs(swiftBinaryDeps->swiftOverlayDependencies,
                            bridgedOverlayDependencyNames);
        details->swift_binary_details = {
            create_clone(swiftBinaryDeps->compiledModulePath.c_str()),
            create_clone(swiftBinaryDeps->moduleDocPath.c_str()),
            create_clone(swiftBinaryDeps->sourceInfoPath.c_str()),
            create_set(bridgedOverlayDependencyNames),
            create_clone(swiftBinaryDeps->headerImport.c_str()),
            create_set(swiftBinaryDeps->headerModuleDependencies),
            create_set(swiftBinaryDeps->headerSourceFiles),
            swiftBinaryDeps->isFramework,
            create_clone(swiftBinaryDeps->moduleCacheKey.c_str())};
      } else {
        // Clang module details
        details->kind = SWIFTSCAN_DEPENDENCY_INFO_CLANG;
        details->clang_details = {
            create_clone(clangDeps->moduleMapFile.c_str()),
            create_clone(clangDeps->contextHash.c_str()),
            create_set(clangDeps->buildCommandLine),
            create_set(clangDeps->capturedPCMArgs),
            create_clone(clangDeps->CASFileSystemRootID.c_str()),
            create_clone(clangDeps->CASClangIncludeTreeRootID.c_str()),
            create_clone(clangDeps->moduleCacheKey.c_str())};
      }
      return details;
    };

    swiftscan_dependency_info_s *moduleInfo = new swiftscan_dependency_info_s;
    dependencySet->modules[i] = moduleInfo;

    std::string encodedModuleName = createEncodedModuleKindAndName(module);
    auto ttt = create_clone(encodedModuleName.c_str());
    moduleInfo->module_name = ttt;
    moduleInfo->module_path = create_clone(modulePath.c_str());
    moduleInfo->source_files = create_set(sourceFiles);

    // Create a direct dependencies set according to the output format
    std::vector<std::string> bridgedDependencyNames;
    bridgeDependencyIDs(directDependencies, bridgedDependencyNames);
    moduleInfo->direct_dependencies = create_set(bridgedDependencyNames);
    moduleInfo->details = getModuleDetails();
  }

  swiftscan_dependency_graph_t result = new swiftscan_dependency_graph_s;
  result->main_module_name = create_clone(mainModuleName.c_str());
  result->dependencies = dependencySet;
  result->diagnostics =
      diagnosticCollector
          ? mapCollectedDiagnosticsForOutput(diagnosticCollector)
          : nullptr;
  return result;
}

/// Implements a topological sort via recursion and reverse postorder DFS.
/// Does not bother handling cycles, relying on a DAG guarantee by the client.
static std::vector<ModuleDependencyID>
computeTopologicalSortOfExplicitDependencies(
    const std::vector<ModuleDependencyID> &allModules,
    const ModuleDependenciesCache &cache) {
  std::unordered_set<ModuleDependencyID> visited;
  std::vector<ModuleDependencyID> result;
  std::stack<ModuleDependencyID> stack;

  // Must be explicitly-typed to allow recursion
  std::function<void(const ModuleDependencyID &)> visit;
  visit = [&visit, &cache, &visited, &result,
           &stack](const ModuleDependencyID &moduleID) {
    // Mark this node as visited -- we are done if it already was.
    if (!visited.insert(moduleID).second)
      return;

    // Otherwise, visit each adjacent node.
    for (const auto &succID : cache.getAllDependencies(moduleID)) {
      // We don't worry if successor is already in this current stack,
      // since that would mean we have found a cycle, which should not
      // be possible because we checked for cycles earlier.
      stack.push(succID);
      visit(succID);
      auto top = stack.top();
      stack.pop();
      assert(top == succID);
    }

    // Add to the result.
    result.push_back(moduleID);
  };

  for (const auto &modID : allModules) {
    assert(stack.empty());
    stack.push(modID);
    visit(modID);
    auto top = stack.top();
    stack.pop();
    assert(top == modID);
  }

  std::reverse(result.begin(), result.end());
  return result;
}

/// For each module in the graph, compute a set of all its dependencies,
/// direct *and* transitive.
static std::unordered_map<ModuleDependencyID, std::set<ModuleDependencyID>>
computeTransitiveClosureOfExplicitDependencies(
    const std::vector<ModuleDependencyID> &topologicallySortedModuleList,
    const ModuleDependenciesCache &cache) {
  // The usage of an ordered ::set is important to ensure the
  // dependencies are listed in a deterministic order.
  std::unordered_map<ModuleDependencyID, std::set<ModuleDependencyID>> result;
  for (const auto &modID : topologicallySortedModuleList)
    result[modID] = {modID};

  // Traverse the set of modules in reverse topological order, assimilating
  // transitive closures
  for (auto it = topologicallySortedModuleList.rbegin(),
            end = topologicallySortedModuleList.rend();
       it != end; ++it) {
    const auto &modID = *it;
    auto &modReachableSet = result[modID];
    for (const auto &succID : cache.getAllDependencies(modID)) {
      const auto &succReachableSet = result[succID];
      llvm::set_union(modReachableSet, succReachableSet);
    }
  }
  // For ease of use down-the-line, remove the node's self from its set of
  // reachable nodes
  for (const auto &modID : topologicallySortedModuleList)
    result[modID].erase(modID);

  return result;
}

static std::set<ModuleDependencyID> computeBridgingHeaderTransitiveDependencies(
    const ModuleDependencyInfo *dep,
    const std::unordered_map<ModuleDependencyID, std::set<ModuleDependencyID>>
        &transitiveClosures,
    const ModuleDependenciesCache &cache) {
  std::set<ModuleDependencyID> result;
  auto *sourceDep = dep->getAsSwiftSourceModule();
  if (!sourceDep)
    return result;

  for (auto &dep : sourceDep->textualModuleDetails.bridgingModuleDependencies) {
    ModuleDependencyID modID{dep, ModuleDependencyKind::Clang};
    result.insert(modID);
    auto succDeps = transitiveClosures.find(modID);
    assert(succDeps != transitiveClosures.end() && "unknown dependency");
    llvm::set_union(result, succDeps->second);
  }

  return result;
}

static std::vector<ModuleDependencyID>
findClangDepPath(const ModuleDependencyID &from, const ModuleDependencyID &to,
                 const ModuleDependenciesCache &cache) {
  std::unordered_set<ModuleDependencyID> visited;
  std::vector<ModuleDependencyID> result;
  std::stack<ModuleDependencyID, std::vector<ModuleDependencyID>> stack;

  // Must be explicitly-typed to allow recursion
  std::function<void(const ModuleDependencyID &)> visit;

  visit = [&visit, &cache, &visited, &result, &stack,
           to](const ModuleDependencyID &moduleID) {
    if (!visited.insert(moduleID).second)
      return;

    if (moduleID == to) {
      // Copy stack contents to the result
      auto end = &stack.top() + 1;
      auto begin = end - stack.size();
      result.assign(begin, end);
      return;
    }

    // Otherwise, visit each child node.
    for (const auto &succID : cache.getAllDependencies(moduleID)) {
      stack.push(succID);
      visit(succID);
      stack.pop();
    }
  };

  stack.push(from);
  visit(from);
  return result;
}

static bool diagnoseCycle(const CompilerInstance &instance,
                          const ModuleDependenciesCache &cache,
                          ModuleDependencyID mainId) {
  ModuleDependencyIDSetVector openSet;
  ModuleDependencyIDSetVector closeSet;

  auto kindIsSwiftDependency = [&](const ModuleDependencyID &ID) {
    return ID.Kind == swift::ModuleDependencyKind::SwiftInterface ||
           ID.Kind == swift::ModuleDependencyKind::SwiftBinary;
  };

  auto emitModulePath = [&](const std::vector<ModuleDependencyID> path,
                            llvm::SmallString<64> &buffer) {
    llvm::interleave(
        path,
        [&buffer](const ModuleDependencyID &id) {
          buffer.append(id.ModuleName);
          switch (id.Kind) {
          case swift::ModuleDependencyKind::SwiftSource:
            buffer.append(" (Source Target)");
            break;
          case swift::ModuleDependencyKind::SwiftInterface:
            buffer.append(".swiftinterface");
            break;
          case swift::ModuleDependencyKind::SwiftBinary:
            buffer.append(".swiftmodule");
            break;
          case swift::ModuleDependencyKind::Clang:
            buffer.append(".pcm");
            break;
          default:
            llvm::report_fatal_error(
                Twine("Invalid Module Dependency Kind in cycle: ") +
                id.ModuleName);
            break;
          }
        },
        [&buffer] { buffer.append(" -> "); });
  };

  auto emitCycleDiagnostic = [&](const ModuleDependencyID &sourceId,
				 const ModuleDependencyID &sinkId) {
    auto startIt = std::find(openSet.begin(), openSet.end(), sourceId);
    assert(startIt != openSet.end());
    std::vector<ModuleDependencyID> cycleNodes(startIt, openSet.end());
    cycleNodes.push_back(sinkId);
    llvm::SmallString<64> errorBuffer;
    emitModulePath(cycleNodes, errorBuffer);
    instance.getASTContext().Diags.diagnose(
        SourceLoc(), diag::scanner_find_cycle, errorBuffer.str());

    // TODO: for (std::tuple<const ModuleDependencyID&, const
    // ModuleDependencyID&> v : cycleNodes | std::views::adjacent<2>)
    for (auto it = cycleNodes.begin(), end = cycleNodes.end(); it != end;
         it++) {
      if (it + 1 == cycleNodes.end())
        continue;

      const auto &thisID = *it;
      const auto &nextID = *(it + 1);
      if (kindIsSwiftDependency(thisID) && kindIsSwiftDependency(nextID) &&
          llvm::any_of(
              cache.getOnlyOverlayDependencies(thisID),
              [&](const ModuleDependencyID id) { return id == nextID; })) {
        llvm::SmallString<64> noteBuffer;
        auto clangDepPath = findClangDepPath(
            thisID,
            ModuleDependencyID{nextID.ModuleName, ModuleDependencyKind::Clang},
            cache);
        emitModulePath(clangDepPath, noteBuffer);
        instance.getASTContext().Diags.diagnose(
            SourceLoc(), diag::scanner_find_cycle_swift_overlay_path,
            thisID.ModuleName, nextID.ModuleName, noteBuffer.str());
      }
    }
  };

  // Start from the main module and check direct and overlay dependencies
  openSet.insert(mainId);
  while (!openSet.empty()) {
    auto lastOpen = openSet.back();
    auto beforeSize = openSet.size();
    assert(cache.findDependency(lastOpen).has_value() &&
           "Missing dependency info during cycle diagnosis.");
    for (const auto &depId : cache.getAllDependencies(lastOpen)) {
      if (closeSet.count(depId))
        continue;
      // Ensure we detect dependency of the Source target
      // on an existing Swift module with the same name
      if (kindIsSwiftDependency(depId) &&
          depId.ModuleName == mainId.ModuleName && openSet.contains(mainId)) {
        emitCycleDiagnostic(mainId, depId);
        return true;
      }
      if (openSet.insert(depId)) {
        break;
      } else {
        emitCycleDiagnostic(depId, depId);
        return true;
      }
    }
    // No new node added. We can close this node
    if (openSet.size() == beforeSize) {
      closeSet.insert(openSet.back());
      openSet.pop_back();
    } else {
      assert(openSet.size() == beforeSize + 1);
    }
  }
  assert(openSet.empty());
  closeSet.clear();
  return false;
}

static void updateCachedInstanceOpts(CompilerInstance &cachedInstance,
                                     const CompilerInstance &invocationInstance,
                                     llvm::StringRef entryArguments) {
  cachedInstance.getASTContext().SearchPathOpts =
      invocationInstance.getASTContext().SearchPathOpts;

  // The Clang Importer arguments must consist of a combination of
  // Clang Importer arguments of the current invocation to inherit its Clang-specific
  // search path options, followed by the options specific to the given batch-entry,
  // which may overload some of the invocation's options (e.g. target)
  cachedInstance.getASTContext().ClangImporterOpts =
      invocationInstance.getASTContext().ClangImporterOpts;
  std::istringstream iss(entryArguments.str());
  std::vector<std::string> splitArguments(
      std::istream_iterator<std::string>{iss},
      std::istream_iterator<std::string>());
  for (auto it = splitArguments.begin(), end = splitArguments.end(); it != end;
       ++it) {
    if ((*it) == "-Xcc") {
      assert((it + 1 != end) && "Expected option following '-Xcc'");
      cachedInstance.getASTContext().ClangImporterOpts.ExtraArgs.push_back(
          *(it + 1));
    }
  }
}

static bool
forEachBatchEntry(CompilerInstance &invocationInstance,
                  ModuleDependenciesCache &invocationCache,
                  CompilerArgInstanceCacheMap *versionedPCMInstanceCache,
                  llvm::StringSaver &saver,
                  const std::vector<BatchScanInput> &batchInput,
                  llvm::function_ref<void(BatchScanInput, CompilerInstance &,
                                          ModuleDependenciesCache &)>
                      scanningAction) {
  const CompilerInvocation &invoke = invocationInstance.getInvocation();
  bool localSubInstanceMap = false;
  CompilerArgInstanceCacheMap *subInstanceMap;
  if (versionedPCMInstanceCache)
    subInstanceMap = versionedPCMInstanceCache;
  else {
    subInstanceMap = new CompilerArgInstanceCacheMap;
    localSubInstanceMap = true;
  }
  SWIFT_DEFER {
    if (localSubInstanceMap)
      delete subInstanceMap;
  };

  auto &diags = invocationInstance.getDiags();
  ForwardingDiagnosticConsumer FDC(invocationInstance.getDiags());

  for (auto &entry : batchInput) {
    CompilerInstance *pInstance = nullptr;
    ModuleDependenciesCache *pCache = nullptr;
    if (entry.arguments.empty()) {
      // Use the compiler's instance if no arguments are specified.
      pInstance = &invocationInstance;
      pCache = &invocationCache;
    } else if (subInstanceMap->count(entry.arguments)) {
      // Use the previously created instance if we've seen the arguments
      // before.
      pInstance = std::get<0>((*subInstanceMap)[entry.arguments]).get();
      pCache = std::get<2>((*subInstanceMap)[entry.arguments]).get();
      // We must update the search paths of this instance to instead reflect
      // those of the current scanner invocation.
      updateCachedInstanceOpts(*pInstance, invocationInstance, entry.arguments);
    } else {
      // We must reset option occurrences because we are handling an unrelated command-line
      // to those parsed before. We must do so because LLVM options parsing is done
      // using a managed static `GlobalParser`.
      llvm::cl::ResetAllOptionOccurrences();

      // Create a new instance by the arguments and save it in the map.
      auto newService = std::make_unique<SwiftDependencyScanningService>();
      auto newInstance = std::make_unique<CompilerInstance>();

      SmallVector<const char *, 4> args;
      llvm::cl::TokenizeGNUCommandLine(entry.arguments, saver, args);
      CompilerInvocation subInvoke = invoke;
      newInstance->addDiagnosticConsumer(&FDC);
      if (subInvoke.parseArgs(args, diags)) {
        invocationInstance.getDiags().diagnose(
            SourceLoc(), diag::scanner_arguments_invalid, entry.arguments);
        return true;
      }
      std::string InstanceSetupError;
      if (newInstance->setup(subInvoke, InstanceSetupError)) {
        invocationInstance.getDiags().diagnose(
            SourceLoc(), diag::scanner_arguments_invalid, entry.arguments);
        return true;
      }
      auto mainModuleName = newInstance->getMainModule()->getNameStr();
      auto scanContextHash =
          newInstance->getInvocation().getModuleScanningHash();
      auto moduleOutputPath = newInstance->getInvocation()
                                  .getFrontendOptions()
                                  .ExplicitModulesOutputPath;
      auto newLocalCache = std::make_unique<ModuleDependenciesCache>(
          *newService, mainModuleName.str(), moduleOutputPath, scanContextHash);
      pInstance = newInstance.get();
      pCache = newLocalCache.get();
      subInstanceMap->insert(
          {entry.arguments,
           std::make_tuple(std::move(newInstance), std::move(newService),
                           std::move(newLocalCache))});
    }
    assert(pInstance);
    assert(pCache);
    scanningAction(entry, *pInstance, *pCache);
  }

  return false;
}
} // namespace

static void serializeDependencyCache(CompilerInstance &instance,
                                     const SwiftDependencyScanningService &service) {
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  ASTContext &Context = instance.getASTContext();
  auto savePath = opts.SerializedDependencyScannerCachePath;
  module_dependency_cache_serialization::writeInterModuleDependenciesCache(
      Context.Diags, instance.getOutputBackend(), savePath, service);
  if (opts.EmitDependencyScannerCacheRemarks) {
    Context.Diags.diagnose(SourceLoc(), diag::remark_save_cache, savePath);
  }
}

static void deserializeDependencyCache(CompilerInstance &instance,
                                       SwiftDependencyScanningService &service) {
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  ASTContext &Context = instance.getASTContext();
  auto loadPath = opts.SerializedDependencyScannerCachePath;
  if (module_dependency_cache_serialization::readInterModuleDependenciesCache(
          loadPath, service)) {
    Context.Diags.diagnose(SourceLoc(), diag::warn_scanner_deserialize_failed,
                           loadPath);
  } else if (opts.EmitDependencyScannerCacheRemarks) {
    Context.Diags.diagnose(SourceLoc(), diag::remark_reuse_cache, loadPath);
  }
}

bool swift::dependencies::scanDependencies(CompilerInstance &instance) {
  ASTContext &Context = instance.getASTContext();
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  // `-scan-dependencies` invocations use a single new instance
  // of a module cache
  SwiftDependencyScanningService service;
  if (opts.ReuseDependencyScannerCache)
    deserializeDependencyCache(instance, service);

  if (service.setupCachingDependencyScanningService(instance))
    return true;

  ModuleDependenciesCache cache(
      service, instance.getMainModule()->getNameStr().str(),
      instance.getInvocation().getFrontendOptions().ExplicitModulesOutputPath,
      instance.getInvocation().getModuleScanningHash());

  // Execute scan
  llvm::ErrorOr<swiftscan_dependency_graph_t> dependenciesOrErr =
      performModuleScan(instance, nullptr, cache);

  // Serialize the dependency cache if -serialize-dependency-scan-cache
  // is specified
  if (opts.SerializeDependencyScannerCache)
    serializeDependencyCache(instance, service);

  if (dependenciesOrErr.getError())
    return true;
  auto dependencies = std::move(*dependenciesOrErr);

  if (writeJSONToOutput(Context.Diags, instance.getOutputBackend(), path,
                        dependencies))
    return true;

  // This process succeeds regardless of whether any errors occurred.
  // FIXME: We shouldn't need this, but it's masking bugs in our scanning
  // logic where we don't create a fresh context when scanning Swift interfaces
  // that includes their own command-line flags.
  Context.Diags.resetHadAnyError();
  return false;
}

bool swift::dependencies::prescanDependencies(CompilerInstance &instance) {
  ASTContext &Context = instance.getASTContext();
  const FrontendOptions &opts = instance.getInvocation().getFrontendOptions();
  std::string path = opts.InputsAndOutputs.getSingleOutputFilename();
  // `-scan-dependencies` invocations use a single new instance
  // of a module cache
  SwiftDependencyScanningService singleUseService;
  ModuleDependenciesCache cache(
      singleUseService, instance.getMainModule()->getNameStr().str(),
      instance.getInvocation().getFrontendOptions().ExplicitModulesOutputPath,
      instance.getInvocation().getModuleScanningHash());

  // Execute import prescan, and write JSON output to the output stream
  auto importSetOrErr = performModulePrescan(instance, nullptr, cache);
  if (importSetOrErr.getError())
    return true;
  auto importSet = std::move(*importSetOrErr);

  // Serialize and output main module dependencies only and exit.
  if (writePrescanJSONToOutput(Context.Diags, instance.getOutputBackend(), path,
                               importSet))
    return true;

  // This process succeeds regardless of whether any errors occurred.
  // FIXME: We shouldn't need this, but it's masking bugs in our scanning
  // logic where we don't create a fresh context when scanning Swift interfaces
  // that includes their own command-line flags.
  Context.Diags.resetHadAnyError();
  return false;
}

bool swift::dependencies::batchScanDependencies(
    CompilerInstance &instance, llvm::StringRef batchInputFile) {
  // The primary cache used for scans carried out with the compiler instance
  // we have created

  SwiftDependencyScanningService singleUseService;
  if (singleUseService.setupCachingDependencyScanningService(instance))
    return true;

  ModuleDependenciesCache cache(
      singleUseService, instance.getMainModule()->getNameStr().str(),
      instance.getInvocation().getFrontendOptions().ExplicitModulesOutputPath,
      instance.getInvocation().getModuleScanningHash());
  (void)instance.getMainModule();
  llvm::BumpPtrAllocator alloc;
  llvm::StringSaver saver(alloc);
  auto batchInput =
      parseBatchScanInputFile(instance.getASTContext(), batchInputFile, saver);
  if (!batchInput.has_value())
    return true;

  auto batchScanResults = performBatchModuleScan(
      instance, /*DependencyScanDiagnosticCollector*/ nullptr, 
      cache, /*versionedPCMInstanceCache*/ nullptr, saver,
      *batchInput);

  // Write the result JSON to the specified output path, for each entry
  auto ientries = batchInput->cbegin();
  auto iresults = batchScanResults.cbegin();
  for (; ientries != batchInput->end() and iresults != batchScanResults.end();
       ++ientries, ++iresults) {
    if ((*iresults).getError())
      return true;

    if (writeJSONToOutput(instance.getASTContext().Diags,
                          instance.getOutputBackend(), (*ientries).outputPath,
                          **iresults))
      return true;
  }
  return false;
}

std::string
swift::dependencies::createEncodedModuleKindAndName(ModuleDependencyID id) {
  switch (id.Kind) {
  case ModuleDependencyKind::SwiftInterface:
  case ModuleDependencyKind::SwiftSource:
    return "swiftTextual:" + id.ModuleName;
  case ModuleDependencyKind::SwiftBinary:
    return "swiftBinary:" + id.ModuleName;
  case ModuleDependencyKind::SwiftPlaceholder:
    return "swiftPlaceholder:" + id.ModuleName;
  case ModuleDependencyKind::Clang:
    return "clang:" + id.ModuleName;
  default:
    llvm_unreachable("Unhandled dependency kind.");
  }
}

static void resolveDependencyCommandLineArguments(
    CompilerInstance &instance, ModuleDependenciesCache &cache,
    const std::vector<ModuleDependencyID> &topoSortedModuleList) {
  auto moduleTransitiveClosures =
      computeTransitiveClosureOfExplicitDependencies(topoSortedModuleList,
                                                     cache);
  for (const auto &modID : llvm::reverse(topoSortedModuleList)) {
    auto dependencyClosure = moduleTransitiveClosures[modID];
    // For main module or binary modules, no command-line to resolve.
    // For Clang modules, their dependencies are resolved by the clang Scanner
    // itself for us.
    auto optionalDeps = cache.findDependency(modID);
    assert(optionalDeps.has_value());
    auto deps = optionalDeps.value();
    std::optional<std::set<ModuleDependencyID>> bridgingHeaderDeps;
    if (modID.Kind == ModuleDependencyKind::SwiftSource)
      bridgingHeaderDeps = computeBridgingHeaderTransitiveDependencies(
          deps, moduleTransitiveClosures, cache);

    if (auto E =
            resolveExplicitModuleInputs(modID, *deps, dependencyClosure, cache,
                                        instance, bridgingHeaderDeps))
      instance.getDiags().diagnose(SourceLoc(), diag::error_cas,
                                   toString(std::move(E)));

    if (auto E = pruneUnusedVFSOverlays(modID, *deps, dependencyClosure,
                                        cache, instance))
      instance.getDiags().diagnose(SourceLoc(), diag::error_cas,
                                   toString(std::move(E)));
  }
}

static void
updateDependencyTracker(CompilerInstance &instance,
                        ModuleDependenciesCache &cache,
                        const std::vector<ModuleDependencyID> &allModules) {
  if (auto depTracker = instance.getDependencyTracker()) {
    for (auto module : allModules) {
      auto optionalDeps = cache.findDependency(module);
      if (!optionalDeps.has_value())
        continue;
      auto deps = optionalDeps.value();

      if (auto swiftDeps = deps->getAsSwiftInterfaceModule()) {
        depTracker->addDependency(swiftDeps->swiftInterfaceFile,
                                  /*IsSystem=*/false);
        for (const auto &bridgingSourceFile :
             swiftDeps->textualModuleDetails.bridgingSourceFiles)
          depTracker->addDependency(bridgingSourceFile, /*IsSystem=*/false);
      } else if (auto swiftSourceDeps = deps->getAsSwiftSourceModule()) {
        for (const auto &sourceFile : swiftSourceDeps->sourceFiles)
          depTracker->addDependency(sourceFile, /*IsSystem=*/false);
        for (const auto &bridgingSourceFile :
             swiftSourceDeps->textualModuleDetails.bridgingSourceFiles)
          depTracker->addDependency(bridgingSourceFile, /*IsSystem=*/false);
      } else if (auto clangDeps = deps->getAsClangModule()) {
        if (!clangDeps->moduleMapFile.empty())
          depTracker->addDependency(clangDeps->moduleMapFile,
                                    /*IsSystem=*/false);
        for (const auto &sourceFile : clangDeps->fileDependencies)
          depTracker->addDependency(sourceFile, /*IsSystem=*/false);
      }
    }
  }
}

llvm::ErrorOr<swiftscan_dependency_graph_t>
swift::dependencies::performModuleScan(CompilerInstance &instance,
                                       DependencyScanDiagnosticCollector *diagnosticCollector,
                                       ModuleDependenciesCache &cache) {
  auto scanner = ModuleDependencyScanner(
      cache.getScanService(), instance.getInvocation(),
      instance.getSILOptions(), instance.getASTContext(),
      *instance.getDependencyTracker(), instance.getDiags(),
      instance.getInvocation().getFrontendOptions().ParallelDependencyScan);

  // Identify imports of the main module and add an entry for it
  // to the dependency graph.
  auto mainModuleDepInfo =
      scanner.getMainModuleDependencyInfo(instance.getMainModule());
  auto mainModuleName = instance.getMainModule()->getNameStr();
  auto mainModuleID = ModuleDependencyID{mainModuleName.str(),
                                         ModuleDependencyKind::SwiftSource};
  // We may be re-using an instance of the cache which already contains
  // an entry for this module.
  if (cache.findDependency(mainModuleName, ModuleDependencyKind::SwiftSource))
    cache.updateDependency(
        ModuleDependencyID{mainModuleName.str(), ModuleDependencyKind::SwiftSource},
        std::move(*mainModuleDepInfo));
  else
    cache.recordDependency(mainModuleName, std::move(*mainModuleDepInfo));

  // Perform the full module scan starting at the main module.
  auto allModules = scanner.getModuleDependencies(mainModuleID, cache);

#ifndef NDEBUG
  // Verify that all collected dependencies have had their
  // imports resolved to module IDs.
  for (const auto &moduleID : allModules)
    assert(cache.findDependency(moduleID)
               .value()
               ->isResolved());
#endif

  if (diagnoseCycle(instance, cache, mainModuleID))
    return std::make_error_code(std::errc::not_supported);

  auto topologicallySortedModuleList =
      computeTopologicalSortOfExplicitDependencies(allModules, cache);
  resolveDependencyCommandLineArguments(instance, cache,
                                        topologicallySortedModuleList);

  updateDependencyTracker(instance, cache, allModules);
  return generateFullDependencyGraph(instance, diagnosticCollector, cache,
                                     topologicallySortedModuleList);
}

llvm::ErrorOr<swiftscan_import_set_t>
swift::dependencies::performModulePrescan(CompilerInstance &instance,
                                          DependencyScanDiagnosticCollector *diagnosticCollector,
                                          ModuleDependenciesCache &cache) {
  // Setup the scanner
  auto scanner = ModuleDependencyScanner(
      cache.getScanService(), instance.getInvocation(),
      instance.getSILOptions(), instance.getASTContext(),
      *instance.getDependencyTracker(), instance.getDiags(),
      instance.getInvocation().getFrontendOptions().ParallelDependencyScan);
  // Execute import prescan, and write JSON output to the output stream
  auto mainDependencies =
      scanner.getMainModuleDependencyInfo(instance.getMainModule());
  if (!mainDependencies)
    return mainDependencies.getError();
  auto *importSet = new swiftscan_import_set_s;
  importSet->imports = create_set(mainDependencies->getModuleImports());
  importSet->diagnostics =
      diagnosticCollector
          ? mapCollectedDiagnosticsForOutput(diagnosticCollector)
          : nullptr;
  return importSet;
}

std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>>
swift::dependencies::performBatchModuleScan(
    CompilerInstance &invocationInstance,
    DependencyScanDiagnosticCollector *diagnosticCollector,
    ModuleDependenciesCache &invocationCache,
    CompilerArgInstanceCacheMap *versionedPCMInstanceCache,
    llvm::StringSaver &saver, const std::vector<BatchScanInput> &batchInput) {
  std::vector<llvm::ErrorOr<swiftscan_dependency_graph_t>> batchScanResult;
  batchScanResult.reserve(batchInput.size());

  // Perform a full dependency scan for each batch entry module
  forEachBatchEntry(
      invocationInstance, invocationCache, versionedPCMInstanceCache, saver,
      batchInput,
      [&batchScanResult, &diagnosticCollector](BatchScanInput entry,
                         CompilerInstance &instance,
                         ModuleDependenciesCache &cache) {
        auto scanner = ModuleDependencyScanner(
            cache.getScanService(), instance.getInvocation(),
            instance.getSILOptions(), instance.getASTContext(),
            *instance.getDependencyTracker(), instance.getDiags(),
            instance.getInvocation().getFrontendOptions().ParallelDependencyScan);

        StringRef moduleName = entry.moduleName;
        bool isClang = !entry.isSwift;
        std::optional<const ModuleDependencyInfo *> rootDeps;
        if (isClang) {
          // Loading the clang module using Clang importer.
          // This action will populate the cache with the main module's
          // dependencies.
          rootDeps = scanner.getNamedClangModuleDependencyInfo(moduleName, cache);
        } else {
          rootDeps = scanner.getNamedSwiftModuleDependencyInfo(moduleName, cache);
        }
        if (!rootDeps.has_value()) {
          // We cannot find the clang module, abort.
          batchScanResult.push_back(
              std::make_error_code(std::errc::invalid_argument));
          return;
        }

        ModuleDependencyIDSetVector allModules;
        ModuleDependencyID moduleID{
            moduleName.str(), isClang ? ModuleDependencyKind::Clang
                                      : ModuleDependencyKind::SwiftInterface};
        auto allDependencies = scanner.getModuleDependencies(moduleID, cache);
        batchScanResult.push_back(
            generateFullDependencyGraph(instance, diagnosticCollector,
                                        cache, allDependencies));
        if (diagnosticCollector)
          diagnosticCollector->reset();
      });

  return batchScanResult;
}
