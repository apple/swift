//===--- FrontendTool.cpp - Swift Compiler Frontend -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This is the entry point to the swift -frontend functionality, which
/// implements the core compiler functionality along with a number of additional
/// tools for demonstration and testing purposes.
///
/// This is separate from the rest of libFrontend to reduce the dependencies
/// required by that library.
///
//===----------------------------------------------------------------------===//

#include "swift/FrontendTool/FrontendTool.h"
#include "ImportedModules.h"
#include "ScanDependencies.h"
#include "TBD.h"

#include "swift/Subsystems.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/FileSystem.h"
#include "swift/AST/FineGrainedDependencies.h"
#include "swift/AST/GenericSignatureBuilder.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/IRGenRequests.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/ASTMangler.h"
#include "swift/AST/TBDGenRequests.h"
#include "swift/AST/TypeRefinementContext.h"
#include "swift/Basic/Dwarf.h"
#include "swift/Basic/Edit.h"
#include "swift/Basic/FileSystem.h"
#include "swift/Basic/JSONSerialization.h"
#include "swift/Basic/LLVMInitialize.h"
#include "swift/Basic/Platform.h"
#include "swift/Basic/PrettyStackTrace.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/UUID.h"
#include "swift/Frontend/DiagnosticVerifier.h"
#include "swift/Frontend/Frontend.h"
#include "swift/Frontend/PrintingDiagnosticConsumer.h"
#include "swift/Frontend/SerializedDiagnosticConsumer.h"
#include "swift/Frontend/ModuleInterfaceLoader.h"
#include "swift/Frontend/ModuleInterfaceSupport.h"
#include "swift/Immediate/Immediate.h"
#include "swift/Index/IndexRecord.h"
#include "swift/Option/Options.h"
#include "swift/Migrator/FixitFilter.h"
#include "swift/Migrator/Migrator.h"
#include "swift/PrintAsObjC/PrintAsObjC.h"
#include "swift/Serialization/SerializationOptions.h"
#include "swift/Serialization/SerializedModuleLoader.h"
#include "swift/SILOptimizer/PassManager/Passes.h"
#include "swift/SIL/SILRemarkStreamer.h"
#include "swift/Syntax/Serialization/SyntaxSerialization.h"
#include "swift/Syntax/SyntaxNodes.h"
#include "swift/TBDGen/TBDGen.h"

#include "clang/AST/ASTContext.h"

#include "llvm/ADT/Statistic.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/Option/Option.h"
#include "llvm/Option/OptTable.h"
#include "llvm/Remarks/RemarkSerializer.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/YAMLTraits.h"
#include "llvm/Target/TargetMachine.h"

#include <memory>
#include <unordered_set>
#include <utility>

#if !defined(_MSC_VER) && !defined(__MINGW32__)
#include <unistd.h>
#else
#include <io.h>
#endif

using namespace swift;

static std::string displayName(StringRef MainExecutablePath) {
  std::string Name = llvm::sys::path::stem(MainExecutablePath).str();
  Name += " -frontend";
  return Name;
}

StringRef
swift::frontend::utils::escapeForMake(StringRef raw,
                                      llvm::SmallVectorImpl<char> &buffer) {
  buffer.clear();

  // The escaping rules for GNU make are complicated due to the various
  // subsitutions and use of the tab in the leading position for recipes.
  // Various symbols have significance in different contexts.  It is not
  // possible to correctly quote all characters in Make (as of 3.7).  Match
  // gcc and clang's behaviour for the escaping which covers only a subset of
  // characters.
  for (unsigned I = 0, E = raw.size(); I != E; ++I) {
    switch (raw[I]) {
    case '#': // Handle '#' the broken GCC way
      buffer.push_back('\\');
      break;

    case ' ':
      for (unsigned J = I; J && raw[J - 1] == '\\'; --J)
        buffer.push_back('\\');
      buffer.push_back('\\');
      break;

    case '$': // $ is escaped by $
      buffer.push_back('$');
      break;
    }
    buffer.push_back(raw[I]);
  }
  buffer.push_back('\0');

  return buffer.data();
}

static std::vector<std::string>
reversePathSortedFilenames(const ArrayRef<std::string> elts) {
  std::vector<std::string> tmp(elts.begin(), elts.end());
  std::sort(tmp.begin(), tmp.end(), [](const std::string &a,
                                       const std::string &b) -> bool {
              return std::lexicographical_compare(a.rbegin(), a.rend(),
                                                  b.rbegin(), b.rend());
            });
  return tmp;
}

/// Emits a Make-style dependencies file.
static bool emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts,
                                         const InputFile &input) {
  const std::string &dependenciesFilePath = input.dependenciesFilePath();
  if (dependenciesFilePath.empty())
    return false;

  std::error_code EC;
  llvm::raw_fd_ostream out(dependenciesFilePath, EC, llvm::sys::fs::F_None);

  if (out.has_error() || EC) {
    diags.diagnose(SourceLoc(), diag::error_opening_output,
                   dependenciesFilePath, EC.message());
    out.clear_error();
    return true;
  }

  llvm::SmallString<256> buffer;

  // collect everything in memory to avoid redundant work
  // when there are multiple targets
  std::string dependencyString;
  
  // First include all other files in the module. Make-style dependencies
  // need to be conservative!
  auto inputPaths =
    reversePathSortedFilenames(opts.InputsAndOutputs.getInputFilenames());
  for (auto const &path : inputPaths) {
    dependencyString.push_back(' ');
    dependencyString.append(frontend::utils::escapeForMake(path, buffer));
  }
  // Then print dependencies we've picked up during compilation.
  auto dependencyPaths =
    reversePathSortedFilenames(depTracker->getDependencies());
  for (auto const &path : dependencyPaths) {
    dependencyString.push_back(' ');
    dependencyString.append(frontend::utils::escapeForMake(path, buffer));
  }
  
  // FIXME: Xcode can't currently handle multiple targets in a single
  // dependency line.
  opts.forAllOutputPaths(input, [&](const StringRef targetName) {
    auto targetNameEscaped = frontend::utils::escapeForMake(targetName, buffer);
    out << targetNameEscaped << " :" << dependencyString << '\n';
  });

  return false;
}

static void emitMakeDependenciesIfNeeded(DiagnosticEngine &diags,
                                         DependencyTracker *depTracker,
                                         const FrontendOptions &opts) {
  opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &f) -> bool {
        return emitMakeDependenciesIfNeeded(diags, depTracker, opts, f);
      });
}

// MARK: - Module Trace

namespace {
struct SwiftModuleTraceInfo {
  Identifier Name;
  std::string Path;
  bool IsImportedDirectly;
  bool SupportsLibraryEvolution;
};

struct LoadedModuleTraceFormat {
  static const unsigned CurrentVersion = 2;
  unsigned Version;
  Identifier Name;
  std::string Arch;
  std::vector<SwiftModuleTraceInfo> SwiftModules;
};
}

namespace swift {
namespace json {
template <> struct ObjectTraits<SwiftModuleTraceInfo> {
  static void mapping(Output &out, SwiftModuleTraceInfo &contents) {
    StringRef name = contents.Name.str();
    out.mapRequired("name", name);
    out.mapRequired("path", contents.Path);
    out.mapRequired("isImportedDirectly", contents.IsImportedDirectly);
    out.mapRequired("supportsLibraryEvolution",
                    contents.SupportsLibraryEvolution);
  }
};

// Version notes:
// 1. Keys: name, arch, swiftmodules
// 2. New keys: version, swiftmodulesDetailedInfo
template <> struct ObjectTraits<LoadedModuleTraceFormat> {
  static void mapping(Output &out, LoadedModuleTraceFormat &contents) {
    out.mapRequired("version", contents.Version);

    StringRef name = contents.Name.str();
    out.mapRequired("name", name);

    out.mapRequired("arch", contents.Arch);

    // The 'swiftmodules' key is kept for backwards compatibility.
    std::vector<std::string> moduleNames;
    for (auto &m : contents.SwiftModules)
      moduleNames.push_back(m.Path);
    out.mapRequired("swiftmodules", moduleNames);

    out.mapRequired("swiftmodulesDetailedInfo", contents.SwiftModules);
  }
};
}
}

static bool isClangOverlayOf(ModuleDecl *potentialOverlay,
                             ModuleDecl *potentialUnderlying) {
  return !potentialOverlay->isNonSwiftModule()
      && potentialUnderlying->isNonSwiftModule()
      && potentialOverlay->getName() == potentialUnderlying->getName();
}

// TODO: Delete this once changes from https://reviews.llvm.org/D83449 land on
// apple/llvm-project's swift/master branch.
template <typename SetLike, typename Item>
static bool contains(const SetLike &setLike, Item item) {
  return setLike.find(item) != setLike.end();
}

/// Get a set of modules imported by \p module.
///
/// By default, all imports are included.
static void getImmediateImports(
    ModuleDecl *module,
    SmallPtrSetImpl<ModuleDecl *> &imports,
    ModuleDecl::ImportFilter importFilter = {
      ModuleDecl::ImportFilterKind::Public,
      ModuleDecl::ImportFilterKind::Private,
      ModuleDecl::ImportFilterKind::ImplementationOnly,
      ModuleDecl::ImportFilterKind::SPIAccessControl,
      ModuleDecl::ImportFilterKind::ShadowedBySeparateOverlay
    }) {
  SmallVector<ModuleDecl::ImportedModule, 8> importList;
  module->getImportedModules(importList, importFilter);

  for (ModuleDecl::ImportedModule &import : importList)
    imports.insert(import.importedModule);
}

namespace {
/// Helper type for computing (approximate) information about ABI-dependencies.
///
/// This misses out on details such as typealiases and more.
/// See the "isImportedDirectly" field above for more details.
class ABIDependencyEvaluator {
  /// Map of ABIs exported by a particular module, excluding itself.
  ///
  /// For example, consider (primed letters represent Clang modules):
  /// \code
  /// - A is @_exported-imported by B
  /// - B is #imported by C' (via a compiler-generated umbrella header)
  /// - C' is @_exported-imported by C (Swift overlay)
  /// - D' is #imported by E'
  /// - D' is @_exported-imported by D (Swift overlay)
  /// - E' is @_exported-imported by E (Swift overlay)
  /// \endcode
  ///
  /// Then the \c abiExportMap will be
  /// \code
  /// { A: {}, B: {A}, C: {B}, C': {B}, D: {}, D': {}, E: {D}, E': {D'} }
  /// \endcode
  ///
  /// \b WARNING: Use \c reexposeImportedABI instead of inserting directly.
  llvm::DenseMap<ModuleDecl *, llvm::DenseSet<ModuleDecl *>> abiExportMap;

  /// Stack for depth-first traversal.
  SmallVector<ModuleDecl *, 32> searchStack;

  llvm::DenseSet<ModuleDecl *> visited;

  /// Helper function to handle invariant violations as crashes in debug mode.
  void crashOnInvariantViolation(
    llvm::function_ref<void (llvm::raw_string_ostream &)> f) const;

  /// Computes the ABI exports for \p importedModule and adds them to
  /// \p module's ABI exports.
  ///
  /// If \p includeImportedModule is true, also adds \p importedModule to
  /// \p module's ABI exports.
  ///
  /// Correct way to add entries to \c abiExportMap.
  void reexposeImportedABI(ModuleDecl *module, ModuleDecl *importedModule,
                           bool includeImportedModule = true);

  /// Check if a Swift module is an overlay for some Clang module.
  ///
  /// FIXME: Delete this hack once SR-13363 is fixed and ModuleDecl has the
  /// right API which we can use directly.
  bool isOverlayOfClangModule(ModuleDecl *swiftModule);

  /// Check for cases where we have a fake cycle through an overlay.
  ///
  /// \code
  /// Actual stack:
  ///    sandwichedModule -> Overlay (Swift) -> ... -> sandwichedModule
  ///                     ^^--- wrong!
  /// Ideal stack:
  ///    sandwichedModule -> Underlying (Clang)
  /// \endcode
  ///
  /// This happens when we have a dependency like:
  /// \code
  ///     Overlay (Swift) -> sandwichedModule -> Underlying (Clang)
  /// \endcode
  ///
  /// We check this lazily because eagerly detecting if the dependency on an
  /// overlay is correct or not is difficult.
  bool isFakeCycleThroughOverlay(ModuleDecl **sandwichedModuleIter);

  /// Recursive step in computing ABI dependencies.
  ///
  /// Use this method instead of using the \c forClangModule/\c forSwiftModule
  /// methods.
  void computeABIDependenciesForModule(ModuleDecl *module);
  void computeABIDependenciesForSwiftModule(ModuleDecl *module);
  void computeABIDependenciesForClangModule(ModuleDecl *module);

  static void printModule(const ModuleDecl *module, llvm::raw_ostream &os);

  template<typename SetLike>
  static void printModuleSet(const SetLike &set, llvm::raw_ostream &os);

public:
  ABIDependencyEvaluator() = default;
  ABIDependencyEvaluator(const ABIDependencyEvaluator &) = delete;
  ABIDependencyEvaluator(ABIDependencyEvaluator &&) = default;

  void getABIDependenciesForSwiftModule(
    ModuleDecl *module, SmallPtrSetImpl<ModuleDecl *> &abiDependencies);

  void printABIExportMap(llvm::raw_ostream &os) const;
};
} // end anonymous namespace

// See [NOTE: Bailing-vs-crashing-in-trace-emission].
// TODO: Use PrettyStackTrace instead?
void ABIDependencyEvaluator::crashOnInvariantViolation(
  llvm::function_ref<void (llvm::raw_string_ostream &)> f) const {
#ifndef NDEBUG
  std::string msg;
  llvm::raw_string_ostream os(msg);
  os << "error: invariant violation: ";
  f(os);
  llvm::report_fatal_error(os.str());
#endif
}

// [NOTE: Trace-Clang-submodule-complexity]
//
// A Clang module may have zero or more submodules. In practice, when traversing
// the imports of a module, we observe that different submodules of the same
// top-level module (almost) freely import each other. Despite this, we still
// need to conceptually traverse the tree formed by the submodule relationship
// (with the top-level module being the root).
//
// This needs to be taken care of in two ways:
// 1. We need to make sure we only go towards the leaves. It's okay if we "jump"
//    branches, so long as we don't try to visit an ancestor when one of its
//    descendants is still on the traversal stack, so that we don't end up with
//    arbitrarily complex intra-module cycles.
//    See also: [NOTE: Intra-module-leafwards-traversal].
// 2. When adding entries to the ABI export map, we need to avoid marking
//    dependencies within the same top-level module. This step is needed in
//    addition to step 1 to avoid creating cycles like
//    Overlay -> Underlying -> Submodule -> Overlay.

void ABIDependencyEvaluator::reexposeImportedABI(
    ModuleDecl *module, ModuleDecl *importedModule,
    bool includeImportedModule) {
  if (module == importedModule) {
    crashOnInvariantViolation([&](llvm::raw_string_ostream &os) {
      os << "module "; printModule(module, os); os << " imports itself!\n";
    });
    return;
  }

  auto addToABIExportMap = [this](ModuleDecl *module, ModuleDecl *reexport) {
    if (module == reexport) {
      crashOnInvariantViolation([&](llvm::raw_string_ostream &os){
        os << "expected module "; printModule(reexport, os);
        os << "  to not re-export itself\n";
      });
      return;
    }
    if (reexport->isNonSwiftModule()
        && module->isNonSwiftModule()
        && module->getTopLevelModule() == reexport->getTopLevelModule()) {
      // Dependencies within the same top-level Clang module are not useful.
      // See also: [NOTE: Trace-Clang-submodule-complexity].
      return;
    }

    // We only care about dependencies across top-level modules and we want to
    // avoid exploding abiExportMap with submodules. So we only insert entries
    // after calling getTopLevelModule().

    if (::isClangOverlayOf(module, reexport)) {
      // For overlays, we need to have a dependency on the underlying module.
      // Otherwise, we might accidentally create a Swift -> Swift cycle.
      abiExportMap[module].insert(
        reexport->getTopLevelModule(/*preferOverlay*/false));
      return;
    }
    abiExportMap[module].insert(
        reexport->getTopLevelModule(/*preferOverlay*/true));
  };

  computeABIDependenciesForModule(importedModule);
  if (includeImportedModule) {
    addToABIExportMap(module, importedModule);
  }
  // Force creation of default value if missing. This prevents abiExportMap from
  // growing (and moving) when calling addToABIExportMap. If abiExportMap gets
  // moved, then abiExportMap[importedModule] will be moved, forcing us to
  // create a defensive copy to avoid iterator invalidation on move.
  (void)abiExportMap[module];
  for (auto reexportedModule: abiExportMap[importedModule])
    addToABIExportMap(module, reexportedModule);
}

bool ABIDependencyEvaluator::isOverlayOfClangModule(ModuleDecl *swiftModule) {
  assert(!swiftModule->isNonSwiftModule());

  llvm::SmallPtrSet<ModuleDecl *, 8> importList;
  ::getImmediateImports(swiftModule, importList,
                        {ModuleDecl::ImportFilterKind::Public});
  bool isOverlay =
      llvm::any_of(importList, [&](ModuleDecl *importedModule) -> bool {
        return isClangOverlayOf(swiftModule, importedModule);
      });
  return isOverlay;
}

bool ABIDependencyEvaluator::isFakeCycleThroughOverlay(
    ModuleDecl **sandwichModuleIter) {
  assert(sandwichModuleIter >= searchStack.begin()
         && sandwichModuleIter < searchStack.end()
         && "sandwichModuleIter points to an element in searchStack");
  // The sandwichedModule must be a Clang module.
  if (!(*sandwichModuleIter)->isNonSwiftModule())
    return false;
  auto nextModuleIter = sandwichModuleIter + 1;
  if (nextModuleIter == searchStack.end())
    return false;
  // The next module must be a Swift overlay for a Clang module
  if ((*nextModuleIter)->isNonSwiftModule())
    return false;
  return isOverlayOfClangModule(*nextModuleIter);
}

void ABIDependencyEvaluator::computeABIDependenciesForModule(
    ModuleDecl *module) {
  auto moduleIter = llvm::find(searchStack, module);
  if (moduleIter != searchStack.end()) {
    if (isFakeCycleThroughOverlay(moduleIter))
      return;
    crashOnInvariantViolation([&](llvm::raw_string_ostream &os) {
      os << "unexpected cycle in import graph!\n";
      for (auto m: searchStack) {
        printModule(m, os); os << "\ndepends on ";
      }
      printModule(module, os); os << '\n';
    });
    return;
  }
  if (::contains(visited, module))
    return;
  searchStack.push_back(module);
  if (module->isNonSwiftModule())
    computeABIDependenciesForClangModule(module);
  else
    computeABIDependenciesForSwiftModule(module);
  searchStack.pop_back();
  visited.insert(module);
}

void ABIDependencyEvaluator::computeABIDependenciesForSwiftModule(
    ModuleDecl *module) {
  SmallPtrSet<ModuleDecl *, 32> allImports;
  ::getImmediateImports(module, allImports);
  for (auto import: allImports) {
    computeABIDependenciesForModule(import);
    if (::isClangOverlayOf(module, import)) {
      reexposeImportedABI(module, import,
                          /*includeImportedModule=*/false);
    }
  }

  SmallPtrSet<ModuleDecl *, 32> reexportedImports;
  ::getImmediateImports(module, reexportedImports,
                        {ModuleDecl::ImportFilterKind::Public});
  for (auto reexportedImport: reexportedImports) {
    reexposeImportedABI(module, reexportedImport);
  }
}

void ABIDependencyEvaluator::computeABIDependenciesForClangModule(
    ModuleDecl *module) {
  SmallPtrSet<ModuleDecl *, 32> imports;
  ::getImmediateImports(module, imports);
  for (auto import: imports) {
    // There are three cases here which can potentially create cycles:
    //
    // 1. Clang modules importing the stdlib.
    //    See [NOTE: Pure-Clang-modules-privately-import-stdlib].
    // 2. Overlay S @_exported-imports underlying module S' and another Clang
    //    module C'. C' (transitively) #imports S' but it gets treated as if
    //    C' imports S. This creates a cycle: S -> C' -> ... -> S.
    //    In practice, this case is hit for
    //      Darwin (Swift) -> SwiftOverlayShims (Clang) -> Darwin (Swift).
    //    We may also hit this in a slightly different direction, in case
    //    the module directly imports SwiftOverlayShims:
    //      SwiftOverlayShims -> Darwin (Swift) -> SwiftOverlayShims
    //    The latter is handled later by isFakeCycleThroughOverlay.
    // 3. [NOTE: Intra-module-leafwards-traversal]
    //    Cycles within the same top-level module.
    //    These don't matter for us, since we only care about the dependency
    //    graph at the granularity of top-level modules. So we ignore these
    //    by only considering parent -> submodule dependencies.
    //    See also [NOTE: Trace-Clang-submodule-complexity].
    if (import->isStdlibModule()) {
      continue;
    }
    if (!import->isNonSwiftModule() && isOverlayOfClangModule(import) &&
        llvm::find(searchStack, import) != searchStack.end()) {
      continue;
    }
    if (import->isNonSwiftModule()
        && module->getTopLevelModule() == import->getTopLevelModule()
        && (module == import
            || !import->findUnderlyingClangModule()
                      ->isSubModuleOf(module->findUnderlyingClangModule()))) {
      continue;
    }
    computeABIDependenciesForModule(import);
    reexposeImportedABI(module, import);
  }
}

void ABIDependencyEvaluator::getABIDependenciesForSwiftModule(
    ModuleDecl *module, SmallPtrSetImpl<ModuleDecl *> &abiDependencies) {
  computeABIDependenciesForModule(module);
  SmallPtrSet<ModuleDecl *, 32> allImports;
  ::getImmediateImports(module, allImports);
  for (auto directDependency: allImports) {
    abiDependencies.insert(directDependency);
    for (auto exposedDependency: abiExportMap[directDependency]) {
      abiDependencies.insert(exposedDependency);
    }
  }
}

void ABIDependencyEvaluator::printModule(
    const ModuleDecl *module, llvm::raw_ostream &os) {
  module->getReverseFullModuleName().printForward(os);
  os << (module->isNonSwiftModule() ? " (Clang)" : " (Swift)");
  os << " @ " << llvm::format("0x%llx", reinterpret_cast<uintptr_t>(module));
}

template<typename SetLike>
void ABIDependencyEvaluator::printModuleSet(
    const SetLike &set, llvm::raw_ostream &os) {
  os << "{ ";
  for (auto module: set) {
    printModule(module, os); os << ", ";
  }
  os << "}";
}

void ABIDependencyEvaluator::printABIExportMap(llvm::raw_ostream &os) const {
  os << "ABI Export Map {{\n";
  for (auto &entry: abiExportMap) {
    printModule(entry.first, os); os << " : ";
    printModuleSet(entry.second, os);
    os << "\n";
  }
  os << "}}\n";
}

/// Compute the per-module information to be recorded in the trace file.
//
// The most interesting/tricky thing here is _which_ paths get recorded in
// the trace file as dependencies. It depends on how the module was synthesized.
// The key points are:
//
// 1. Paths to swiftmodules in the module cache or in the prebuilt cache are not
//    recorded - Precondition: the corresponding path to the swiftinterface must
//    already be present as a key in pathToModuleDecl.
// 2. swiftmodules next to a swiftinterface are saved if they are up-to-date.
//
// FIXME: Use the VFS instead of handling paths directly. We are particularly
// sloppy about handling relative paths in the dependency tracker.
static void computeSwiftModuleTraceInfo(
    const SmallPtrSetImpl<ModuleDecl *> &abiDependencies,
    const llvm::DenseMap<StringRef, ModuleDecl *> &pathToModuleDecl,
    const DependencyTracker &depTracker,
    StringRef prebuiltCachePath,
    std::vector<SwiftModuleTraceInfo> &traceInfo) {

  SmallString<256> buffer;

  std::string errMsg;
  llvm::raw_string_ostream err(errMsg);

  // FIXME: Use PrettyStackTrace instead.
  auto errorUnexpectedPath =
      [&pathToModuleDecl](llvm::raw_string_ostream &errStream) {
    errStream << "The module <-> path mapping we have is:\n";
    for (auto &m: pathToModuleDecl)
      errStream << m.second->getName() << " <-> " << m.first << '\n';
    llvm::report_fatal_error(errStream.str());
  };

  using namespace llvm::sys;

  auto computeAdjacentInterfacePath = [](SmallVectorImpl<char> &modPath) {
    auto swiftInterfaceExt =
      file_types::getExtension(file_types::TY_SwiftModuleInterfaceFile);
    path::replace_extension(modPath, swiftInterfaceExt);
  };

  for (auto &depPath : depTracker.getDependencies()) {

    // Decide if this is a swiftmodule based on the extension of the raw
    // dependency path, as the true file may have a different one.
    // For example, this might happen when the canonicalized path points to
    // a Content Addressed Storage (CAS) location.
    auto moduleFileType =
      file_types::lookupTypeForExtension(path::extension(depPath));
    auto isSwiftmodule =
      moduleFileType == file_types::TY_SwiftModuleFile;
    auto isSwiftinterface =
      moduleFileType == file_types::TY_SwiftModuleInterfaceFile;

    if (!(isSwiftmodule || isSwiftinterface))
      continue;

    auto dep = pathToModuleDecl.find(depPath);
    if (dep != pathToModuleDecl.end()) {
      // Great, we recognize the path! Check if the file is still around.

      ModuleDecl *depMod = dep->second;
      if(depMod->isResilient() && !isSwiftinterface) {
        // FIXME: Ideally, we would check that the swiftmodule has a
        // swiftinterface next to it. Tracked by rdar://problem/56351399.
      }

      // FIXME: Better error handling
      StringRef realDepPath
        = fs::real_path(depPath, buffer, /*expand_tile*/true)
        ? StringRef(depPath) // Couldn't find the canonical path, assume
                             // this is good enough.
        : buffer.str();

      bool isImportedDirectly = ::contains(abiDependencies, depMod);

      traceInfo.push_back(
          {/*Name=*/
           depMod->getName(),
           /*Path=*/
           realDepPath.str(),
           // TODO: There is an edge case which is not handled here.
           // When we build a framework using -import-underlying-module, or an
           // app/test using -import-objc-header, we should look at the direct
           // imports of the bridging modules, and mark those as our direct
           // imports.
           // TODO: Add negative test cases for the comment above.
           // TODO: Describe precise semantics of "isImportedDirectly".
           /*IsImportedDirectly=*/
           isImportedDirectly,
           /*SupportsLibraryEvolution=*/
           depMod->isResilient()});
      buffer.clear();

      continue;
    }

    // If the depTracker had an interface, that means that we must've
    // built a swiftmodule from that interface, so we should have that
    // filename available.
    if (isSwiftinterface) {
      err << "Unexpected path for swiftinterface file:\n" << depPath << "\n";
      errorUnexpectedPath(err);
    }

    // Skip cached modules in the prebuilt cache. We will add the corresponding
    // swiftinterface from the SDK directly, but this isn't checked. :-/
    //
    // FIXME: This is incorrect if both paths are not relative w.r.t. to the
    // same root.
    if (StringRef(depPath).startswith(prebuiltCachePath))
      continue;

    // If we have a swiftmodule next to an interface, that interface path will
    // be saved (not checked), so don't save the path to this swiftmodule.
    SmallString<256> moduleAdjacentInterfacePath(depPath);
    computeAdjacentInterfacePath(moduleAdjacentInterfacePath);
    if (::contains(pathToModuleDecl, moduleAdjacentInterfacePath))
      continue;

    // FIXME: The behavior of fs::exists for relative paths is undocumented.
    // Use something else instead?
    if (fs::exists(moduleAdjacentInterfacePath)) {
      // This should be an error but it is not because of funkiness around
      // compatible modules such as us having both armv7s.swiftinterface
      // and armv7.swiftinterface in the dependency tracker.
      continue;
    }
    buffer.clear();

    // We might land here when we have a arm.swiftmodule in the cache path
    // which added a dependency on a arm.swiftinterface (which was not loaded).
  }

  // Almost a re-implementation of reversePathSortedFilenames :(.
  std::sort(
    traceInfo.begin(), traceInfo.end(),
    [](const SwiftModuleTraceInfo &m1, const SwiftModuleTraceInfo &m2) -> bool {
      return std::lexicographical_compare(
        m1.Path.rbegin(), m1.Path.rend(),
        m2.Path.rbegin(), m2.Path.rend());
  });
}

// [NOTE: Bailing-vs-crashing-in-trace-emission] There are certain edge cases
// in trace emission where an invariant that you think should hold does not hold
// in practice. For example, sometimes we have seen modules without any
// corresponding filename.
//
// Since the trace is a supplementary output for build system consumption, it
// it better to emit it on a best-effort basis instead of crashing and failing
// the build.
//
// Moreover, going forward, it would be nice if trace emission were more robust
// so we could emit the trace on a best-effort basis even if the dependency
// graph is ill-formed, so that the trace can be used as a debugging aid.
static bool emitLoadedModuleTraceIfNeeded(ModuleDecl *mainModule,
                                          DependencyTracker *depTracker,
                                          StringRef prebuiltCachePath,
                                          StringRef loadedModuleTracePath) {
  ASTContext &ctxt = mainModule->getASTContext();
  assert(!ctxt.hadError()
         && "We should've already exited earlier if there was an error.");

  if (loadedModuleTracePath.empty())
    return false;
  std::error_code EC;
  llvm::raw_fd_ostream out(loadedModuleTracePath, EC, llvm::sys::fs::F_Append);

  if (out.has_error() || EC) {
    ctxt.Diags.diagnose(SourceLoc(), diag::error_opening_output,
                        loadedModuleTracePath, EC.message());
    out.clear_error();
    return true;
  }

  SmallPtrSet<ModuleDecl *, 32> abiDependencies;
  {
    ABIDependencyEvaluator evaluator{};
    evaluator.getABIDependenciesForSwiftModule(mainModule,
                                               abiDependencies);
  }

  llvm::DenseMap<StringRef, ModuleDecl *> pathToModuleDecl;
  for (const auto &module : ctxt.getLoadedModules()) {
    ModuleDecl *loadedDecl = module.second;
    if (!loadedDecl)
      llvm::report_fatal_error("Expected loaded modules to be non-null.");
    if (loadedDecl == mainModule)
      continue;
    if (loadedDecl->getModuleFilename().empty()) {
      // FIXME: rdar://problem/59853077
      // Ideally, this shouldn't happen. As a temporary workaround, avoid
      // crashing with a message while we investigate the problem.
      llvm::errs() << "WARNING: Module '" << loadedDecl->getName().str()
                   << "' has an empty filename. This is probably an "
                   << "invariant violation.\n"
                   << "Please report it as a compiler bug.\n";
      continue;
    }
    pathToModuleDecl.insert(
      std::make_pair(loadedDecl->getModuleFilename(), loadedDecl));
  }

  std::vector<SwiftModuleTraceInfo> swiftModules;
  computeSwiftModuleTraceInfo(abiDependencies,
                              pathToModuleDecl, *depTracker,
                              prebuiltCachePath, swiftModules);

  LoadedModuleTraceFormat trace = {
      /*version=*/LoadedModuleTraceFormat::CurrentVersion,
      /*name=*/mainModule->getName(),
      /*arch=*/ctxt.LangOpts.Target.getArchName().str(), swiftModules};

  // raw_fd_ostream is unbuffered, and we may have multiple processes writing,
  // so first write to memory and then dump the buffer to the trace file.
  std::string stringBuffer;
  {
    llvm::raw_string_ostream memoryBuffer(stringBuffer);
    json::Output jsonOutput(memoryBuffer, /*UserInfo=*/{},
                            /*PrettyPrint=*/false);
    json::jsonize(jsonOutput, trace, /*Required=*/true);
  }
  stringBuffer += "\n";
  out << stringBuffer;

  return true;
}

static void
emitLoadedModuleTraceForAllPrimariesIfNeeded(ModuleDecl *mainModule,
                                             DependencyTracker *depTracker,
                                             const FrontendOptions &opts) {
  opts.InputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        return emitLoadedModuleTraceIfNeeded(
          mainModule, depTracker, opts.PrebuiltModuleCachePath,
          input.loadedModuleTracePath());
      });
}

/// Gets an output stream for the provided output filename, or diagnoses to the
/// provided AST Context and returns null if there was an error getting the
/// stream.
static std::unique_ptr<llvm::raw_fd_ostream>
getFileOutputStream(StringRef OutputFilename, ASTContext &Ctx) {
  std::error_code errorCode;
  auto os = std::make_unique<llvm::raw_fd_ostream>(
              OutputFilename, errorCode, llvm::sys::fs::F_None);
  if (errorCode) {
    Ctx.Diags.diagnose(SourceLoc(), diag::error_opening_output,
                       OutputFilename, errorCode.message());
    return nullptr;
  }
  return os;
}

/// Writes the Syntax tree to the given file
static bool emitSyntax(SourceFile *SF, StringRef OutputFilename) {
  auto os = getFileOutputStream(OutputFilename, SF->getASTContext());
  if (!os) return true;

  json::Output jsonOut(*os, /*UserInfo=*/{}, /*PrettyPrint=*/false);
  auto Root = SF->getSyntaxRoot().getRaw();
  jsonOut << *Root;
  *os << "\n";
  return false;
}

/// Writes SIL out to the given file.
static bool writeSIL(SILModule &SM, ModuleDecl *M, const SILOptions &Opts,
                     StringRef OutputFilename) {
  auto OS = getFileOutputStream(OutputFilename, M->getASTContext());
  if (!OS) return true;
  SM.print(*OS, M, Opts);

  return M->getASTContext().hadError();
}

static bool writeSIL(SILModule &SM, const PrimarySpecificPaths &PSPs,
                     const CompilerInstance &Instance,
                     const SILOptions &Opts) {
  return writeSIL(SM, Instance.getMainModule(), Opts,
                  PSPs.OutputFilename);
}

/// Prints the Objective-C "generated header" interface for \p M to \p
/// outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::printAsObjC
static bool printAsObjCIfNeeded(StringRef outputPath, ModuleDecl *M,
                                StringRef bridgingHeader, bool moduleIsPublic) {
  if (outputPath.empty())
    return false;
  return withOutputFile(M->getDiags(), outputPath,
                        [&](raw_ostream &out) -> bool {
    auto requiredAccess = moduleIsPublic ? AccessLevel::Public
                                         : AccessLevel::Internal;
    return printAsObjC(out, M, bridgingHeader, requiredAccess);
  });
}

/// Prints the stable module interface for \p M to \p outputPath.
///
/// ...unless \p outputPath is empty, in which case it does nothing.
///
/// \returns true if there were any errors
///
/// \see swift::emitSwiftInterface
static bool
printModuleInterfaceIfNeeded(StringRef outputPath,
                             ModuleInterfaceOptions const &Opts,
                             LangOptions const &LangOpts,
                             ModuleDecl *M) {
  if (outputPath.empty())
    return false;

  DiagnosticEngine &diags = M->getDiags();
  if (!LangOpts.isSwiftVersionAtLeast(5)) {
    assert(LangOpts.isSwiftVersionAtLeast(4));
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_swift_version,
                   LangOpts.isSwiftVersionAtLeast(4, 2) ? "4.2" : "4");
  }
  if (M->getResilienceStrategy() != ResilienceStrategy::Resilient) {
    diags.diagnose(SourceLoc(),
                   diag::warn_unsupported_module_interface_library_evolution);
  }
  return withOutputFile(diags, outputPath,
                        [M, Opts](raw_ostream &out) -> bool {
    return swift::emitSwiftInterface(out, Opts, M);
  });
}

namespace {

/// If there is an error with fixits it writes the fixits as edits in json
/// format.
class JSONFixitWriter
  : public DiagnosticConsumer, public migrator::FixitFilter {
  std::string FixitsOutputPath;
  std::unique_ptr<llvm::raw_ostream> OSPtr;
  bool FixitAll;
  std::vector<SingleEdit> AllEdits;

public:
  JSONFixitWriter(std::string fixitsOutputPath,
                  const DiagnosticOptions &DiagOpts)
    : FixitsOutputPath(fixitsOutputPath),
      FixitAll(DiagOpts.FixitCodeForAllDiagnostics) {}

private:
  void handleDiagnostic(SourceManager &SM,
                        const DiagnosticInfo &Info) override {
    if (!(FixitAll || shouldTakeFixit(Info)))
      return;
    for (const auto &Fix : Info.FixIts) {
      AllEdits.push_back({SM, Fix.getRange(), Fix.getText().str()});
    }
  }

  bool finishProcessing() override {
    std::error_code EC;
    std::unique_ptr<llvm::raw_fd_ostream> OS;
    OS.reset(new llvm::raw_fd_ostream(FixitsOutputPath,
                                      EC,
                                      llvm::sys::fs::F_None));
    if (EC) {
      // Create a temporary diagnostics engine to print the error to stderr.
      SourceManager dummyMgr;
      DiagnosticEngine DE(dummyMgr);
      PrintingDiagnosticConsumer PDC;
      DE.addConsumer(PDC);
      DE.diagnose(SourceLoc(), diag::cannot_open_file,
                  FixitsOutputPath, EC.message());
      return true;
    }

    swift::writeEditsInJson(llvm::makeArrayRef(AllEdits), *OS);
    return false;
  }
};

} // anonymous namespace

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithAssertion() {
  // Per the user's request, this assertion should always fail in
  // builds with assertions enabled.

  // This should not be converted to llvm_unreachable, as those are
  // treated as optimization hints in builds where they turn into
  // __builtin_unreachable().
  assert((0) && "This is an assertion!");
}

// This is a separate function so that it shows up in stack traces.
LLVM_ATTRIBUTE_NOINLINE
static void debugFailWithCrash() {
  LLVM_BUILTIN_TRAP;
}

static void countStatsOfSourceFile(UnifiedStatsReporter &Stats,
                                   const CompilerInstance &Instance,
                                   SourceFile *SF) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumDecls += SF->getTopLevelDecls().size();
  C.NumLocalTypeDecls += SF->LocalTypeDecls.size();
  C.NumObjCMethods += SF->ObjCMethods.size();

  SmallVector<OperatorDecl *, 2> operators;
  SF->getOperatorDecls(operators);
  C.NumOperators += operators.size();

  SmallVector<PrecedenceGroupDecl *, 2> groups;
  SF->getPrecedenceGroups(groups);
  C.NumPrecedenceGroups += groups.size();

  auto bufID = SF->getBufferID();
  if (bufID.hasValue()) {
    C.NumSourceLines +=
      SM.getEntireTextForBuffer(bufID.getValue()).count('\n');
  }
}

static void countASTStats(UnifiedStatsReporter &Stats,
                          CompilerInstance& Instance) {
  auto &C = Stats.getFrontendCounters();
  auto &SM = Instance.getSourceMgr();
  C.NumSourceBuffers = SM.getLLVMSourceMgr().getNumBuffers();
  C.NumLinkLibraries = Instance.getLinkLibraries().size();

  auto const &AST = Instance.getASTContext();
  C.NumLoadedModules = AST.getNumLoadedModules();

  if (auto *D = Instance.getDependencyTracker()) {
    C.NumDependencies = D->getDependencies().size();
  }

  for (auto SF : Instance.getPrimarySourceFiles()) {
    auto &Ctx = SF->getASTContext();
    Ctx.evaluator.enumerateReferencesInFile(SF, [&C](const auto &ref) {
    using NodeKind = evaluator::DependencyCollector::Reference::Kind;
      switch (ref.kind) {
      case NodeKind::Empty:
      case NodeKind::Tombstone:
        llvm_unreachable("Cannot enumerate dead dependency!");
      case NodeKind::TopLevel:
        C.NumReferencedTopLevelNames += 1;
        return;
      case NodeKind::Dynamic:
        C.NumReferencedDynamicNames += 1;
        return;
      case NodeKind::PotentialMember:
      case NodeKind::UsedMember:
        C.NumReferencedMemberNames += 1;
        return;
      }
    });
  }

  if (!Instance.getPrimarySourceFiles().empty()) {
    for (auto SF : Instance.getPrimarySourceFiles())
      countStatsOfSourceFile(Stats, Instance, SF);
  } else if (auto *M = Instance.getMainModule()) {
    // No primary source file, but a main module; this is WMO-mode
    for (auto *F : M->getFiles()) {
      if (auto *SF = dyn_cast<SourceFile>(F)) {
        countStatsOfSourceFile(Stats, Instance, SF);
      }
    }
  }
}

static void countStatsPostSILGen(UnifiedStatsReporter &Stats,
                                 const SILModule& Module) {
  auto &C = Stats.getFrontendCounters();
  // FIXME: calculate these in constant time, via the dense maps.
  C.NumSILGenFunctions += Module.getFunctionList().size();
  C.NumSILGenVtables += Module.getVTables().size();
  C.NumSILGenWitnessTables += Module.getWitnessTableList().size();
  C.NumSILGenDefaultWitnessTables += Module.getDefaultWitnessTableList().size();
  C.NumSILGenGlobalVariables += Module.getSILGlobalList().size();
}

static bool precompileBridgingHeader(const CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  auto &ImporterOpts = Invocation.getClangImporterOptions();
  auto &PCHOutDir = ImporterOpts.PrecompiledHeaderOutputDir;
  if (!PCHOutDir.empty()) {
    // Create or validate a persistent PCH.
    auto SwiftPCHHash = Invocation.getPCHHash();
    auto PCH = clangImporter->getOrCreatePCH(ImporterOpts, SwiftPCHHash);
    return !PCH.hasValue();
  }
  return clangImporter->emitBridgingPCH(
      opts.InputsAndOutputs.getFilenameOfFirstInput(),
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool precompileClangModule(const CompilerInstance &Instance) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->emitPrecompiledModule(
      opts.InputsAndOutputs.getFilenameOfFirstInput(), opts.ModuleName,
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool dumpPrecompiledClangModule(const CompilerInstance &Instance) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  auto clangImporter = static_cast<ClangImporter *>(
      Instance.getASTContext().getClangModuleLoader());
  return clangImporter->dumpPrecompiledModule(
      opts.InputsAndOutputs.getFilenameOfFirstInput(),
      opts.InputsAndOutputs.getSingleOutputFilename());
}

static bool buildModuleFromInterface(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const FrontendOptions &FEOpts = Invocation.getFrontendOptions();
  assert(FEOpts.InputsAndOutputs.hasSingleInput());
  StringRef InputPath = FEOpts.InputsAndOutputs.getFilenameOfFirstInput();
  StringRef PrebuiltCachePath = FEOpts.PrebuiltModuleCachePath;
  ModuleInterfaceLoaderOptions LoaderOpts(FEOpts);
  return ModuleInterfaceLoader::buildSwiftModuleFromSwiftInterface(
      Instance.getSourceMgr(), Instance.getDiags(),
      Invocation.getSearchPathOptions(), Invocation.getLangOptions(),
      Invocation.getClangImporterOptions(),
      Invocation.getClangModuleCachePath(),
      PrebuiltCachePath, Invocation.getModuleName(), InputPath,
      Invocation.getOutputFilename(),
      FEOpts.SerializeModuleInterfaceDependencyHashes,
      FEOpts.shouldTrackSystemDependencies(), LoaderOpts);
}

static bool compileLLVMIR(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &inputsAndOutputs =
      Invocation.getFrontendOptions().InputsAndOutputs;
  // Load in bitcode file.
  assert(inputsAndOutputs.hasSingleInput() &&
         "We expect a single input for bitcode input!");
  llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> FileBufOrErr =
      swift::vfs::getFileOrSTDIN(Instance.getFileSystem(),
                                 inputsAndOutputs.getFilenameOfFirstInput());

  if (!FileBufOrErr) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_open_input_file,
                                 inputsAndOutputs.getFilenameOfFirstInput(),
                                 FileBufOrErr.getError().message());
    return true;
  }
  llvm::MemoryBuffer *MainFile = FileBufOrErr.get().get();

  llvm::SMDiagnostic Err;
  auto LLVMContext = std::make_unique<llvm::LLVMContext>();
  std::unique_ptr<llvm::Module> Module =
      llvm::parseIR(MainFile->getMemBufferRef(), Err, *LLVMContext.get());
  if (!Module) {
    // TODO: Translate from the diagnostic info to the SourceManager location
    // if available.
    Instance.getDiags().diagnose(SourceLoc(), diag::error_parse_input_file,
                                 inputsAndOutputs.getFilenameOfFirstInput(),
                                 Err.getMessage());
    return true;
  }
  return performLLVM(Invocation.getIRGenOptions(), Instance.getASTContext(),
                     Module.get(), inputsAndOutputs.getSingleOutputFilename());
}

static void verifyGenericSignaturesIfNeeded(const CompilerInvocation &Invocation,
                                            ASTContext &Context) {
  auto verifyGenericSignaturesInModule =
      Invocation.getFrontendOptions().VerifyGenericSignaturesInModule;
  if (verifyGenericSignaturesInModule.empty())
    return;
  if (auto module = Context.getModuleByName(verifyGenericSignaturesInModule))
    GenericSignatureBuilder::verifyGenericSignaturesInModule(module);
}

static void dumpAndPrintScopeMap(const CompilerInstance &Instance,
                                 SourceFile *SF) {
  // Not const because may require reexpansion
  ASTScope &scope = SF->getScope();

  const auto &opts = Instance.getInvocation().getFrontendOptions();
  if (opts.DumpScopeMapLocations.empty()) {
    llvm::errs() << "***Complete scope map***\n";
    scope.buildFullyExpandedTree();
    scope.print(llvm::errs());
    return;
  }
  // Probe each of the locations, and dump what we find.
  for (auto lineColumn : opts.DumpScopeMapLocations) {
    scope.buildFullyExpandedTree();
    scope.dumpOneScopeMapLocation(lineColumn);
  }
}

static SourceFile *
getPrimaryOrMainSourceFile(const CompilerInstance &Instance) {
  SourceFile *SF = Instance.getPrimarySourceFile();
  if (!SF) {
    SourceFileKind Kind = Instance.getInvocation().getSourceFileKind();
    SF = &Instance.getMainModule()->getMainSourceFile(Kind);
  }
  return SF;
}

/// Dumps the AST of all available primary source files. If corresponding output
/// files were specified, use them; otherwise, dump the AST to stdout.
static void dumpAST(CompilerInstance &Instance) {
  auto primaryFiles = Instance.getPrimarySourceFiles();
  if (!primaryFiles.empty()) {
    for (SourceFile *sourceFile: primaryFiles) {
      auto PSPs = Instance.getPrimarySpecificPathsForSourceFile(*sourceFile);
      auto OutputFilename = PSPs.OutputFilename;
      auto OS = getFileOutputStream(OutputFilename, Instance.getASTContext());
      sourceFile->dump(*OS, /*parseIfNeeded*/ true);
    }
  } else {
    // Some invocations don't have primary files. In that case, we default to
    // looking for the main file and dumping it to `stdout`.
    auto *SF = getPrimaryOrMainSourceFile(Instance);
    SF->dump(llvm::outs(), /*parseIfNeeded*/ true);
  }
}

/// We may have been told to dump the AST (either after parsing or
/// type-checking, which is already differentiated in
/// CompilerInstance::performSema()), so dump or print the main source file and
/// return.

static Optional<bool> dumpASTIfNeeded(CompilerInstance &Instance) {
  const auto &opts = Instance.getInvocation().getFrontendOptions();
  const FrontendOptions::ActionType Action = opts.RequestedAction;
  ASTContext &Context = Instance.getASTContext();
  switch (Action) {
  default:
    return None;

  case FrontendOptions::ActionType::PrintAST:
    getPrimaryOrMainSourceFile(Instance)
        ->print(llvm::outs(), PrintOptions::printEverything());
    break;

  case FrontendOptions::ActionType::DumpScopeMaps:
    dumpAndPrintScopeMap(Instance, getPrimaryOrMainSourceFile(Instance));
    break;

  case FrontendOptions::ActionType::DumpTypeRefinementContexts:
    getPrimaryOrMainSourceFile(Instance)
        ->getTypeRefinementContext()
        ->dump(llvm::errs(), Context.SourceMgr);
    break;

  case FrontendOptions::ActionType::DumpInterfaceHash:
    getPrimaryOrMainSourceFile(Instance)->dumpInterfaceHash(llvm::errs());
    break;

  case FrontendOptions::ActionType::EmitSyntax:
    emitSyntax(getPrimaryOrMainSourceFile(Instance),
               opts.InputsAndOutputs.getSingleOutputFilename());
    break;

  case FrontendOptions::ActionType::DumpParse:
  case FrontendOptions::ActionType::DumpAST:
    dumpAST(Instance);
    break;

  case FrontendOptions::ActionType::EmitImportedModules:
    emitImportedModules(Context, Instance.getMainModule(), opts);
    break;
  }
  return Context.hadError();
}

static void emitReferenceDependenciesForAllPrimaryInputsIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasReferenceDependenciesPath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::emit_reference_dependencies_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &referenceDependenciesFilePath =
        Invocation.getReferenceDependenciesFilePathForPrimary(
            SF->getFilename());
    if (!referenceDependenciesFilePath.empty()) {
      const auto LangOpts = Invocation.getLangOptions();
      (void)fine_grained_dependencies::emitReferenceDependencies(
          Instance.getDiags(), SF, *Instance.getDependencyTracker(),
          referenceDependenciesFilePath,
          LangOpts.EmitFineGrainedDependencySourcefileDotFiles);
    }
  }
}
static void
emitSwiftRangesForAllPrimaryInputsIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  if (Invocation.getFrontendOptions().InputsAndOutputs.hasSwiftRangesPath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::emit_swift_ranges_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &swiftRangesFilePath =
        Invocation.getSwiftRangesFilePathForPrimary(SF->getFilename());
    if (!swiftRangesFilePath.empty()) {
      (void)Instance.emitSwiftRanges(Instance.getDiags(), SF,
                                     swiftRangesFilePath);
    }
  }
}
static void emitCompiledSourceForAllPrimaryInputsIfNeeded(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  if (Invocation.getFrontendOptions()
          .InputsAndOutputs.hasCompiledSourcePath() &&
      Instance.getPrimarySourceFiles().empty()) {
    Instance.getDiags().diagnose(
        SourceLoc(), diag::emit_compiled_source_without_primary_file);
    return;
  }
  for (auto *SF : Instance.getPrimarySourceFiles()) {
    const std::string &compiledSourceFilePath =
        Invocation.getCompiledSourceFilePathForPrimary(SF->getFilename());
    if (!compiledSourceFilePath.empty()) {
      (void)Instance.emitCompiledSource(Instance.getDiags(), SF,
                                        compiledSourceFilePath);
    }
  }
}

static bool writeTBDIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  const auto &tbdOpts = Invocation.getTBDGenOptions();
  if (!frontendOpts.InputsAndOutputs.hasTBDPath())
    return false;

  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_only_supported_in_whole_module);
    return false;
  }

  if (Invocation.getSILOptions().CrossModuleOptimization) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_not_supported_with_cmo);
    return false;
  }

  const std::string &TBDPath = Invocation.getTBDPathForWholeModule();

  return writeTBD(Instance.getMainModule(), TBDPath, tbdOpts);
}

static std::string changeToLdAdd(StringRef ldHide) {
  SmallString<64> SymbolBuffer;
  llvm::raw_svector_ostream OS(SymbolBuffer);
  auto Parts = ldHide.split("$hide$");
  assert(!Parts.first.empty());
  assert(!Parts.second.empty());
  OS << Parts.first << "$add$" << Parts.second;
  return OS.str().str();
}

static bool writeLdAddCFileIfNeeded(CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &frontendOpts = Invocation.getFrontendOptions();
  if (!frontendOpts.InputsAndOutputs.isWholeModule())
    return false;
  auto Path = Invocation.getLdAddCFileOutputPathForWholeModule();
  if (Path.empty())
    return false;
  if (!frontendOpts.InputsAndOutputs.isWholeModule()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::tbd_only_supported_in_whole_module);
    return true;
  }
  if (!Invocation.getTBDGenOptions().ModuleInstallNameMapPath.empty()) {
    Instance.getDiags().diagnose(SourceLoc(),
                                 diag::linker_directives_choice_confusion);
    return true;
  }
  auto tbdOpts = Invocation.getTBDGenOptions();
  tbdOpts.LinkerDirectivesOnly = true;
  auto *module = Instance.getMainModule();
  auto ldSymbols =
      getPublicSymbols(TBDGenDescriptor::forModule(module, tbdOpts));
  std::error_code EC;
  llvm::raw_fd_ostream OS(Path, EC, llvm::sys::fs::F_None);
  if (EC) {
    Instance.getDiags().diagnose(SourceLoc(), diag::error_opening_output, Path,
                                 EC.message());
    return true;
  }
  OS << "// Automatically generated C source file from the Swift compiler \n"
     << "// to add removed symbols back to the high-level framework for deployment\n"
     << "// targets prior to the OS version when these symbols were moved to\n"
     << "// a low-level framework " << module->getName().str() << ".\n\n";
  unsigned Idx = 0;
  for (auto &S: ldSymbols) {
    SmallString<32> NameBuffer;
    llvm::raw_svector_ostream NameOS(NameBuffer);
    NameOS << "ldAdd_" << Idx;
    OS << "extern const char " << NameOS.str() << " __asm(\"" <<
      changeToLdAdd(S) << "\");\n";
    OS << "const char " << NameOS.str() << " = 0;\n";
    ++ Idx;
  }
  return false;
}

static bool performCompileStepsPostSILGen(CompilerInstance &Instance,
                                          std::unique_ptr<SILModule> SM,
                                          ModuleOrSourceFile MSF,
                                          const PrimarySpecificPaths &PSPs,
                                          int &ReturnValue,
                                          FrontendObserver *observer);

static bool performCompileStepsPostSema(CompilerInstance &Instance,
                                        int &ReturnValue,
                                        FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const SILOptions &SILOpts = Invocation.getSILOptions();
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  auto *mod = Instance.getMainModule();
  if (!opts.InputsAndOutputs.hasPrimaryInputs()) {
    // If there are no primary inputs the compiler is in WMO mode and builds one
    // SILModule for the entire module.
    auto SM = performASTLowering(mod, Instance.getSILTypes(), SILOpts);
    const PrimarySpecificPaths PSPs =
        Instance.getPrimarySpecificPathsForWholeModuleOptimizationMode();
    return performCompileStepsPostSILGen(Instance, std::move(SM), mod, PSPs,
                                         ReturnValue, observer);
  }
  // If there are primary source files, build a separate SILModule for
  // each source file, and run the remaining SILOpt-Serialize-IRGen-LLVM
  // once for each such input.
  if (!Instance.getPrimarySourceFiles().empty()) {
    bool result = false;
    for (auto *PrimaryFile : Instance.getPrimarySourceFiles()) {
      auto SM = performASTLowering(*PrimaryFile, Instance.getSILTypes(),
                                   SILOpts);
      const PrimarySpecificPaths PSPs =
          Instance.getPrimarySpecificPathsForSourceFile(*PrimaryFile);
      result |= performCompileStepsPostSILGen(Instance, std::move(SM),
                                              PrimaryFile, PSPs, ReturnValue,
                                              observer);
    }

    return result;
  }

  // If there are primary inputs but no primary _source files_, there might be
  // a primary serialized input.
  bool result = false;
  for (FileUnit *fileUnit : mod->getFiles()) {
    if (auto SASTF = dyn_cast<SerializedASTFile>(fileUnit))
      if (opts.InputsAndOutputs.isInputPrimary(SASTF->getFilename())) {
        auto SM = performASTLowering(*SASTF, Instance.getSILTypes(), SILOpts);
        const PrimarySpecificPaths &PSPs =
            Instance.getPrimarySpecificPathsForPrimary(SASTF->getFilename());
        result |= performCompileStepsPostSILGen(Instance, std::move(SM), mod,
                                                PSPs, ReturnValue, observer);
      }
  }

  return result;
}

static void emitIndexDataForSourceFile(SourceFile *PrimarySourceFile,
                                       const CompilerInstance &Instance);

/// Emits index data for all primary inputs, or the main module.
static void emitIndexData(const CompilerInstance &Instance) {
  if (Instance.getPrimarySourceFiles().empty()) {
    emitIndexDataForSourceFile(nullptr, Instance);
  } else {
    for (SourceFile *SF : Instance.getPrimarySourceFiles())
      emitIndexDataForSourceFile(SF, Instance);
  }
}

/// Emits all "one-per-module" supplementary outputs that don't depend on
/// anything past type-checking.
static bool emitAnyWholeModulePostTypeCheckSupplementaryOutputs(
    CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const FrontendOptions &opts = Invocation.getFrontendOptions();

  // FIXME: Whole-module outputs with a non-whole-module action ought to
  // be disallowed, but the driver implements -index-file mode by generating a
  // regular whole-module frontend command line and modifying it to index just
  // one file (by making it a primary) instead of all of them. If that
  // invocation also has flags to emit whole-module supplementary outputs, the
  // compiler can crash trying to access information for non-type-checked
  // declarations in the non-primary files. For now, prevent those crashes by
  // guarding the emission of whole-module supplementary outputs.
  if (!opts.InputsAndOutputs.isWholeModule())
    return false;

  // Record whether we failed to emit any of these outputs, but keep going; one
  // failure does not mean skipping the rest.
  bool hadAnyError = false;

  if (opts.InputsAndOutputs.hasObjCHeaderOutputPath()) {
    std::string BridgingHeaderPathForPrint;
    if (!opts.ImplicitObjCHeaderPath.empty()) {
      if (opts.BridgingHeaderDirForPrint.hasValue()) {
        // User specified preferred directory for including, use that dir.
        llvm::SmallString<32> Buffer(*opts.BridgingHeaderDirForPrint);
        llvm::sys::path::append(Buffer,
          llvm::sys::path::filename(opts.ImplicitObjCHeaderPath));
        BridgingHeaderPathForPrint = Buffer.str();
      } else {
        // By default, include the given bridging header path directly.
        BridgingHeaderPathForPrint = opts.ImplicitObjCHeaderPath;
      }
    }
    hadAnyError |= printAsObjCIfNeeded(
        Invocation.getObjCHeaderOutputPathForAtMostOnePrimary(),
        Instance.getMainModule(), BridgingHeaderPathForPrint,
        Invocation.isModuleExternallyConsumed(Instance.getMainModule()));
  }

  if (opts.InputsAndOutputs.hasModuleInterfaceOutputPath()) {
    hadAnyError |= printModuleInterfaceIfNeeded(
        Invocation.getModuleInterfaceOutputPathForWholeModule(),
        Invocation.getModuleInterfaceOptions(),
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  if (opts.InputsAndOutputs.hasPrivateModuleInterfaceOutputPath()) {
    // Copy the settings from the module interface
    ModuleInterfaceOptions privOpts = Invocation.getModuleInterfaceOptions();
    privOpts.PrintSPIs = true;

    hadAnyError |= printModuleInterfaceIfNeeded(
        Invocation.getPrivateModuleInterfaceOutputPathForWholeModule(),
        privOpts,
        Invocation.getLangOptions(),
        Instance.getMainModule());
  }

  {
    hadAnyError |= writeTBDIfNeeded(Instance);
  }
  {
    hadAnyError |= writeLdAddCFileIfNeeded(Instance);
  }

  return hadAnyError;
}

static void dumpAPIIfNeeded(const CompilerInstance &Instance) {
  using namespace llvm::sys;
  const auto &Invocation = Instance.getInvocation();
  StringRef OutDir = Invocation.getFrontendOptions().DumpAPIPath;
  if (OutDir.empty())
    return;

  auto getOutPath = [&](SourceFile *SF) -> std::string {
    SmallString<256> Path = OutDir;
    StringRef Filename = SF->getFilename();
    path::append(Path, path::filename(Filename));
    return std::string(Path.str());
  };

  std::unordered_set<std::string> Filenames;

  auto dumpFile = [&](SourceFile *SF) -> bool {
    SmallString<512> TempBuf;
    llvm::raw_svector_ostream TempOS(TempBuf);

    PrintOptions PO = PrintOptions::printInterface();
    PO.PrintOriginalSourceText = true;
    PO.Indent = 2;
    PO.PrintAccess = false;
    PO.SkipUnderscoredStdlibProtocols = true;
    SF->print(TempOS, PO);
    if (TempOS.str().trim().empty())
      return false; // nothing to show.

    std::string OutPath = getOutPath(SF);
    bool WasInserted = Filenames.insert(OutPath).second;
    if (!WasInserted) {
      llvm::errs() << "multiple source files ended up with the same dump API "
                      "filename to write to: " << OutPath << '\n';
      return true;
    }

    std::error_code EC;
    llvm::raw_fd_ostream OS(OutPath, EC, fs::FA_Read | fs::FA_Write);
    if (EC) {
      llvm::errs() << "error opening file '" << OutPath << "': "
                   << EC.message() << '\n';
      return true;
    }

    OS << TempOS.str();
    return false;
  };

  std::error_code EC = fs::create_directories(OutDir);
  if (EC) {
    llvm::errs() << "error creating directory '" << OutDir << "': "
                 << EC.message() << '\n';
    return;
  }

  for (auto *FU : Instance.getMainModule()->getFiles()) {
    if (auto *SF = dyn_cast<SourceFile>(FU))
      if (dumpFile(SF))
        return;
  }
}

/// Perform any actions that must have access to the ASTContext, and need to be
/// delayed until the Swift compile pipeline has finished. This may be called
/// before or after LLVM depending on when the ASTContext gets freed.
static void performEndOfPipelineActions(CompilerInstance &Instance) {
  assert(Instance.hasASTContext());
  auto &ctx = Instance.getASTContext();
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();

  // If we were asked to print Clang stats, do so.
  if (opts.PrintClangStats && ctx.getClangModuleLoader())
    ctx.getClangModuleLoader()->printStatistics();

  // Report AST stats if needed.
  if (auto *stats = ctx.Stats)
    countASTStats(*stats, Instance);

  // Report mangling stats if there was no error.
  if (!ctx.hadError())
    Mangle::printManglingStats();

  // Make sure we didn't load a module during a parse-only invocation, unless
  // it's -emit-imported-modules, which can load modules.
  auto action = opts.RequestedAction;
  if (FrontendOptions::shouldActionOnlyParse(action) &&
      action != FrontendOptions::ActionType::EmitImportedModules) {
    assert(ctx.getNumLoadedModules() == 1 &&
           "Loaded a module during parse-only");
    assert(ctx.getLoadedModules().begin()->second == Instance.getMainModule());
  }

  // Verify the AST for all the modules we've loaded.
  ctx.verifyAllLoadedModules();

  // Verify generic signatures if we've been asked to.
  verifyGenericSignaturesIfNeeded(Invocation, ctx);

  // Emit any additional outputs that we only need for a successful compilation.
  // We don't want to unnecessarily delay getting any errors back to the user.
  if (!ctx.hadError()) {
    emitLoadedModuleTraceForAllPrimariesIfNeeded(
        Instance.getMainModule(), Instance.getDependencyTracker(), opts);
    
    emitAnyWholeModulePostTypeCheckSupplementaryOutputs(Instance);

    dumpAPIIfNeeded(Instance);
  }

  // Verify reference dependencies of the current compilation job. Note this
  // must be run *before* verifying diagnostics so that the former can be tested
  // via the latter.
  if (opts.EnableIncrementalDependencyVerifier) {
    if (!Instance.getPrimarySourceFiles().empty()) {
      swift::verifyDependencies(Instance.getSourceMgr(),
                                Instance.getPrimarySourceFiles());
    } else {
      swift::verifyDependencies(Instance.getSourceMgr(),
                                Instance.getMainModule()->getFiles());
    }
  }

  // Emit dependencies and index data.
  emitReferenceDependenciesForAllPrimaryInputsIfNeeded(Instance);
  emitIndexData(Instance);
  emitMakeDependenciesIfNeeded(Instance.getDiags(),
                               Instance.getDependencyTracker(), opts);

  // Emit information about the parsed primaries.
  emitSwiftRangesForAllPrimaryInputsIfNeeded(Instance);
  emitCompiledSourceForAllPrimaryInputsIfNeeded(Instance);
}

/// Performs the compile requested by the user.
/// \param Instance Will be reset after performIRGeneration when the verifier
///                 mode is NoVerify and there were no errors.
/// \returns true on error
static bool performCompile(CompilerInvocation &Invok,
                           CompilerInstance &Instance,
                           ArrayRef<const char *> Args,
                           int &ReturnValue,
                           FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  const FrontendOptions::ActionType Action = opts.RequestedAction;

  // We've been asked to precompile a bridging header or module; we want to
  // avoid touching any other inputs and just parse, emit and exit.
  if (Action == FrontendOptions::ActionType::EmitPCH)
    return precompileBridgingHeader(Instance);
  if (Action == FrontendOptions::ActionType::EmitPCM)
    return precompileClangModule(Instance);
  if (Action == FrontendOptions::ActionType::DumpPCM)
    return dumpPrecompiledClangModule(Instance);
  if (Action == FrontendOptions::ActionType::PrintVersion) {
    llvm::outs() << version::getSwiftFullVersion(
      version::Version::getCurrentLanguageVersion()) << '\n';
    llvm::outs() << "Target: "
      << Invocation.getLangOptions().Target.str() << '\n';
    return false;
  }
  if (Action == FrontendOptions::ActionType::CompileModuleFromInterface)
    return buildModuleFromInterface(Instance);

  if (Invocation.getInputKind() == InputFileKind::LLVM)
    return compileLLVMIR(Instance);

  // If we aren't in a parse-only context and expect an implicit stdlib import,
  // load in the standard library. If we either fail to find it or encounter an
  // error while loading it, bail early. Continuing the compilation will at best
  // trigger a bunch of other errors due to the stdlib being missing, or at
  // worst crash downstream as many call sites don't currently handle a missing
  // stdlib.
  if (!FrontendOptions::shouldActionOnlyParse(Action)) {
    if (Instance.loadStdlibIfNeeded())
      return true;
  }

  bool didFinishPipeline = false;
  SWIFT_DEFER {
    assert(didFinishPipeline && "Returned without calling finishPipeline");
  };

  auto finishPipeline = [&](bool hadError) -> bool {
    // We might have freed the ASTContext already, but in that case we would
    // have already performed these actions.
    if (Instance.hasASTContext()) {
      performEndOfPipelineActions(Instance);
      hadError |= Instance.getASTContext().hadError();
    }
    didFinishPipeline = true;
    return hadError;
  };

  auto &Context = Instance.getASTContext();
  if (FrontendOptions::shouldActionOnlyParse(Action)) {
    // Parsing gets triggered lazily, but let's make sure we have the right
    // input kind.
    auto kind = Invocation.getInputKind();
    assert((kind == InputFileKind::Swift ||
            kind == InputFileKind::SwiftLibrary ||
            kind == InputFileKind::SwiftModuleInterface) &&
           "Only supports parsing .swift files");
    (void)kind;
  } else if (Action == FrontendOptions::ActionType::ResolveImports) {
    Instance.performParseAndResolveImportsOnly();
    return finishPipeline(Context.hadError());
  } else {
    Instance.performSema();
  }

  if (Action == FrontendOptions::ActionType::Parse) {
    // A -parse invocation only cares about the side effects of parsing, so
    // force the parsing of all the source files.
    for (auto *file : Instance.getMainModule()->getFiles()) {
      if (auto *SF = dyn_cast<SourceFile>(file))
        (void)SF->getTopLevelDecls();
    }
    return finishPipeline(Context.hadError());
  }

  if (Action == FrontendOptions::ActionType::ScanDependencies) {
    auto batchScanInput = Instance.getASTContext().SearchPathOpts.BatchScanInputFilePath;
    if (batchScanInput.empty())
      return finishPipeline(scanDependencies(Instance));
    else
      return finishPipeline(batchScanModuleDependencies(Invok,
                                                        Instance,
                                                        batchScanInput));
  }

  if (Action == FrontendOptions::ActionType::ScanClangDependencies)
    return finishPipeline(scanClangDependencies(Instance));

  if (observer)
    observer->performedSemanticAnalysis(Instance);

  {
    FrontendOptions::DebugCrashMode CrashMode = opts.CrashMode;
    if (CrashMode == FrontendOptions::DebugCrashMode::AssertAfterParse)
      debugFailWithAssertion();
    else if (CrashMode == FrontendOptions::DebugCrashMode::CrashAfterParse)
      debugFailWithCrash();
  }

  (void)migrator::updateCodeAndEmitRemapIfNeeded(&Instance);

  if (Action == FrontendOptions::ActionType::REPL) {
    llvm::report_fatal_error("Compiler-internal integrated REPL has been "
                             "removed; use the LLDB-enhanced REPL instead.");
  }

  if (auto r = dumpASTIfNeeded(Instance))
    return finishPipeline(*r);

  if (Context.hadError())
    return finishPipeline(/*hadError*/ true);

  // We've just been told to perform a typecheck, so we can return now.
  if (Action == FrontendOptions::ActionType::Typecheck)
    return finishPipeline(/*hadError*/ false);

  assert(FrontendOptions::doesActionGenerateSIL(Action) &&
         "All actions not requiring SILGen must have been handled!");

  return finishPipeline(
      performCompileStepsPostSema(Instance, ReturnValue, observer));
}

static bool serializeSIB(SILModule *SM, const PrimarySpecificPaths &PSPs,
                         const ASTContext &Context, ModuleOrSourceFile MSF) {
  const std::string &moduleOutputPath =
      PSPs.SupplementaryOutputs.ModuleOutputPath;
  assert(!moduleOutputPath.empty() && "must have an output path");

  SerializationOptions serializationOpts;
  serializationOpts.OutputPath = moduleOutputPath.c_str();
  serializationOpts.SerializeAllSIL = true;
  serializationOpts.IsSIB = true;
  serializationOpts.SerializeOptionsForDebugging = true;

  serialize(MSF, serializationOpts, SM);
  return Context.hadError();
}

static GeneratedModule
generateIR(const IRGenOptions &IRGenOpts, const TBDGenOptions &TBDOpts,
           std::unique_ptr<SILModule> SM,
           const PrimarySpecificPaths &PSPs,
           StringRef OutputFilename, ModuleOrSourceFile MSF,
           llvm::GlobalVariable *&HashGlobal,
           ArrayRef<std::string> parallelOutputFilenames) {
  if (auto *SF = MSF.dyn_cast<SourceFile *>()) {
    return performIRGeneration(SF, IRGenOpts, TBDOpts,
                               std::move(SM), OutputFilename, PSPs,
                               SF->getPrivateDiscriminator().str(),
                               &HashGlobal);
  } else {
    return performIRGeneration(MSF.get<ModuleDecl *>(), IRGenOpts, TBDOpts,
                               std::move(SM), OutputFilename, PSPs,
                               parallelOutputFilenames, &HashGlobal);
  }
}

static bool processCommandLineAndRunImmediately(CompilerInstance &Instance,
                                                std::unique_ptr<SILModule> &&SM,
                                                ModuleOrSourceFile MSF,
                                                FrontendObserver *observer,
                                                int &ReturnValue) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  assert(!MSF.is<SourceFile *>() && "-i doesn't work in -primary-file mode");
  const IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  const ProcessCmdLine &CmdLine =
      ProcessCmdLine(opts.ImmediateArgv.begin(), opts.ImmediateArgv.end());

  PrettyStackTraceStringAction trace(
      "running user code",
      MSF.is<SourceFile *>() ? MSF.get<SourceFile *>()->getFilename()
                     : MSF.get<ModuleDecl *>()->getModuleFilename());

  ReturnValue =
      RunImmediately(Instance, CmdLine, IRGenOpts, Invocation.getSILOptions(),
                     std::move(SM));
  return Instance.getASTContext().hadError();
}

static bool validateTBDIfNeeded(const CompilerInvocation &Invocation,
                                ModuleOrSourceFile MSF,
                                const llvm::Module &IRModule) {
  const auto mode = Invocation.getFrontendOptions().ValidateTBDAgainstIR;
  const bool canPerformTBDValidation = [&]() {
    // If the user has requested we skip validation, honor it.
    if (mode == FrontendOptions::TBDValidationMode::None) {
      return false;
    }

    // Cross-module optimization does not support TBD.
    if (Invocation.getSILOptions().CrossModuleOptimization) {
      return false;
    }

    // If we can't validate the given input file, bail early. This covers cases
    // like passing raw SIL as a primary file.
    if (!inputFileKindCanHaveTBDValidated(Invocation.getInputKind())) {
      return false;
    }

    // Modules with SIB files cannot be validated. This is because SIB files
    // may have serialized hand-crafted SIL definitions that are invisible to
    // TBDGen as it is an AST-only traversal.
    if (auto *mod = MSF.dyn_cast<ModuleDecl *>()) {
      return llvm::none_of(mod->getFiles(), [](const FileUnit *File) -> bool {
        auto SASTF = dyn_cast<SerializedASTFile>(File);
        return SASTF && SASTF->isSIB();
      });
    }

    // "Default" mode's behavior varies if using a debug compiler.
    if (mode == FrontendOptions::TBDValidationMode::Default) {
#ifndef NDEBUG
      // With a debug compiler, we do some validation by default.
      return true;
#else
      // Otherwise, the default is to do nothing.
      return false;
#endif
    }

    
    return true;
  }();

  if (!canPerformTBDValidation) {
    return false;
  }

  const bool diagnoseExtraSymbolsInTBD = [mode]() {
    switch (mode) {
    case FrontendOptions::TBDValidationMode::None:
      llvm_unreachable("Handled Above!");
    case FrontendOptions::TBDValidationMode::Default:
    case FrontendOptions::TBDValidationMode::MissingFromTBD:
      return false;
    case FrontendOptions::TBDValidationMode::All:
      return true;
    }
    llvm_unreachable("invalid mode");
  }();

  TBDGenOptions Opts = Invocation.getTBDGenOptions();
  // Ignore embedded symbols from external modules for validation to remove
  // noise from e.g. statically-linked libraries.
  Opts.embedSymbolsFromModules.clear();
  if (auto *SF = MSF.dyn_cast<SourceFile *>()) {
    return validateTBD(SF, IRModule, Opts, diagnoseExtraSymbolsInTBD);
  } else {
    return validateTBD(MSF.get<ModuleDecl *>(), IRModule, Opts,
                       diagnoseExtraSymbolsInTBD);
  }
}

static void freeASTContextIfPossible(CompilerInstance &Instance) {
  // If the stats reporter is installed, we need the ASTContext to live through
  // the entire compilation process.
  if (Instance.getASTContext().Stats) {
    return;
  }

  const auto &opts = Instance.getInvocation().getFrontendOptions();

  // If there are multiple primary inputs it is too soon to free
  // the ASTContext, etc.. OTOH, if this compilation generates code for > 1
  // primary input, then freeing it after processing the last primary is
  // unlikely to reduce the peak heap size. So, only optimize the
  // single-primary-case (or WMO).
  if (opts.InputsAndOutputs.hasMultiplePrimaryInputs()) {
    return;
  }

  // Make sure to perform the end of pipeline actions now, because they need
  // access to the ASTContext.
  performEndOfPipelineActions(Instance);

  Instance.freeASTContext();
}

static bool generateCode(CompilerInstance &Instance, StringRef OutputFilename,
                         llvm::Module *IRModule,
                         llvm::GlobalVariable *HashGlobal) {
  const auto &opts = Instance.getInvocation().getIRGenOptions();
  std::unique_ptr<llvm::TargetMachine> TargetMachine =
      createTargetMachine(opts, Instance.getASTContext());

  // Free up some compiler resources now that we have an IRModule.
  freeASTContextIfPossible(Instance);

  // If we emitted any errors while perfoming the end-of-pipeline actions, bail.
  if (Instance.getDiags().hadAnyError())
    return true;

  // Now that we have a single IR Module, hand it over to performLLVM.
  return performLLVM(opts, Instance.getDiags(), nullptr, HashGlobal, IRModule,
                     TargetMachine.get(), OutputFilename,
                     Instance.getStatsReporter());
}

static bool performCompileStepsPostSILGen(CompilerInstance &Instance,
                                          std::unique_ptr<SILModule> SM,
                                          ModuleOrSourceFile MSF,
                                          const PrimarySpecificPaths &PSPs,
                                          int &ReturnValue,
                                          FrontendObserver *observer) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();
  FrontendOptions::ActionType Action = opts.RequestedAction;
  const ASTContext &Context = Instance.getASTContext();
  const IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();

  Optional<BufferIndirectlyCausingDiagnosticRAII> ricd;
  if (auto *SF = MSF.dyn_cast<SourceFile *>())
    ricd.emplace(*SF);

  if (observer)
    observer->performedSILGeneration(*SM);

  auto *Stats = Instance.getASTContext().Stats;
  if (Stats)
    countStatsPostSILGen(*Stats, *SM);

  // We've been told to emit SIL after SILGen, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSILGen) {
    return writeSIL(*SM, PSPs, Instance, Invocation.getSILOptions());
  }

  if (Action == FrontendOptions::ActionType::EmitSIBGen) {
    serializeSIB(SM.get(), PSPs, Context, MSF);
    return Context.hadError();
  }

  SM->installSILRemarkStreamer();

  // This is the action to be used to serialize SILModule.
  // It may be invoked multiple times, but it will perform
  // serialization only once. The serialization may either happen
  // after high-level optimizations or after all optimizations are
  // done, depending on the compiler setting.

  auto SerializeSILModuleAction = [&]() {
    const SupplementaryOutputPaths &outs = PSPs.SupplementaryOutputs;
    if (outs.ModuleOutputPath.empty())
      return;

    SerializationOptions serializationOpts =
        Invocation.computeSerializationOptions(outs, Instance.getMainModule());
    serialize(MSF, serializationOpts, SM.get());
  };

  // Set the serialization action, so that the SIL module
  // can be serialized at any moment, e.g. during the optimization pipeline.
  SM->setSerializeSILAction(SerializeSILModuleAction);

  // Perform optimizations and mandatory/diagnostic passes.
  if (Instance.performSILProcessing(SM.get()))
    return true;

  if (observer)
    observer->performedSILProcessing(*SM);

  if (Action == FrontendOptions::ActionType::EmitSIB)
    return serializeSIB(SM.get(), PSPs, Context, MSF);

  if (PSPs.haveModuleOrModuleDocOutputPaths()) {
    if (Action == FrontendOptions::ActionType::MergeModules ||
        Action == FrontendOptions::ActionType::EmitModuleOnly) {
      return Context.hadError();
    }
  }

  assert(Action >= FrontendOptions::ActionType::EmitSIL &&
         "All actions not requiring SILPasses must have been handled!");

  // We've been told to write canonical SIL, so write it now.
  if (Action == FrontendOptions::ActionType::EmitSIL)
    return writeSIL(*SM, PSPs, Instance, Invocation.getSILOptions());

  assert(Action >= FrontendOptions::ActionType::Immediate &&
         "All actions not requiring IRGen must have been handled!");
  assert(Action != FrontendOptions::ActionType::REPL &&
         "REPL mode must be handled immediately after Instance->performSema()");

  // Check if we had any errors; if we did, don't proceed to IRGen.
  if (Context.hadError())
    return true;

  runSILLoweringPasses(*SM);

  // TODO: at this point we need to flush any the _tracing and profiling_
  // in the UnifiedStatsReporter, because the those subsystems of the USR
  // retain _pointers into_ the SILModule, and the SILModule's lifecycle is
  // not presently such that it will outlive the USR (indeed, as it's
  // destroyed on a separate thread, this fact isn't even _deterministic_
  // after this point). If future plans require the USR tracing or
  // profiling entities after this point, more rearranging will be required.
  if (Stats)
    Stats->flushTracesAndProfiles();

  if (Action == FrontendOptions::ActionType::DumpTypeInfo)
    return performDumpTypeInfo(IRGenOpts, *SM);

  if (Action == FrontendOptions::ActionType::Immediate)
    return processCommandLineAndRunImmediately(
        Instance, std::move(SM), MSF, observer, ReturnValue);

  StringRef OutputFilename = PSPs.OutputFilename;
  std::vector<std::string> ParallelOutputFilenames =
      opts.InputsAndOutputs.copyOutputFilenames();
  llvm::GlobalVariable *HashGlobal;
  auto IRModule = generateIR(
      IRGenOpts, Invocation.getTBDGenOptions(), std::move(SM), PSPs,
      OutputFilename, MSF, HashGlobal, ParallelOutputFilenames);

  // If no IRModule is available, bail. This can either happen if IR generation
  // fails, or if parallelIRGen happened correctly (in which case it would have
  // already performed LLVM).
  if (!IRModule)
    return Instance.getDiags().hadAnyError();

  if (validateTBDIfNeeded(Invocation, MSF, *IRModule.getModule()))
    return true;

  return generateCode(Instance, OutputFilename, IRModule.getModule(),
                      HashGlobal);
}

static void emitIndexDataForSourceFile(SourceFile *PrimarySourceFile,
                                       const CompilerInstance &Instance) {
  const auto &Invocation = Instance.getInvocation();
  const auto &opts = Invocation.getFrontendOptions();

  if (opts.IndexStorePath.empty())
    return;

  // FIXME: provide index unit token(s) explicitly and only use output file
  // paths as a fallback.

  bool isDebugCompilation;
  switch (Invocation.getSILOptions().OptMode) {
    case OptimizationMode::NotSet:
    case OptimizationMode::NoOptimization:
      isDebugCompilation = true;
      break;
    case OptimizationMode::ForSpeed:
    case OptimizationMode::ForSize:
      isDebugCompilation = false;
      break;
  }

  if (PrimarySourceFile) {
    const PrimarySpecificPaths &PSPs =
        opts.InputsAndOutputs.getPrimarySpecificPathsForPrimary(
            PrimarySourceFile->getFilename());
    (void) index::indexAndRecord(PrimarySourceFile, PSPs.OutputFilename,
                                 opts.IndexStorePath, opts.IndexSystemModules,
                                 opts.IndexIgnoreStdlib, isDebugCompilation,
                                 Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker());
  } else {
    std::string moduleToken =
        Invocation.getModuleOutputPathForAtMostOnePrimary();
    if (moduleToken.empty())
      moduleToken = opts.InputsAndOutputs.getSingleOutputFilename();

    (void) index::indexAndRecord(Instance.getMainModule(), opts.InputsAndOutputs.copyOutputFilenames(),
                                 moduleToken, opts.IndexStorePath,
                                 opts.IndexSystemModules, opts.IndexIgnoreStdlib,
                                 isDebugCompilation, Invocation.getTargetTriple(),
                                 *Instance.getDependencyTracker());
  }
}

/// Creates a diagnostic consumer that handles dispatching diagnostics to
/// multiple output files, based on the supplementary output paths specified by
/// \p inputsAndOutputs.
///
/// If no output files are needed, returns null.
static std::unique_ptr<DiagnosticConsumer>
createDispatchingDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs,
    llvm::function_ref<std::unique_ptr<DiagnosticConsumer>(const InputFile &)>
        maybeCreateConsumerForDiagnosticsFrom) {

  // The "4" here is somewhat arbitrary. In practice we're going to have one
  // sub-consumer for each diagnostic file we're trying to output, which (again
  // in practice) is going to be 1 in WMO mode and equal to the number of
  // primary inputs in batch mode. That in turn is going to be "the number of
  // files we need to recompile in this build, divided by the number of jobs".
  // So a value of "4" here means that there would be no heap allocation on a
  // clean build of a module with up to 32 files on an 8-core machine, if the
  // user doesn't customize anything.
  SmallVector<FileSpecificDiagnosticConsumer::Subconsumer, 4> subconsumers;

  inputsAndOutputs.forEachInputProducingSupplementaryOutput(
      [&](const InputFile &input) -> bool {
        if (auto consumer = maybeCreateConsumerForDiagnosticsFrom(input))
          subconsumers.emplace_back(input.file(), std::move(consumer));
        return false;
      });
  // For batch mode, the compiler must sometimes swallow diagnostics pertaining
  // to non-primary files in order to avoid Xcode showing the same diagnostic
  // multiple times. So, create a diagnostic "eater" for those non-primary
  // files.
  //
  // This routine gets called in cases where no primary subconsumers are created.
  // Don't bother to create non-primary subconsumers if there aren't any primary
  // ones.
  //
  // To avoid introducing bugs into WMO or single-file modes, test for multiple
  // primaries.
  if (!subconsumers.empty() && inputsAndOutputs.hasMultiplePrimaryInputs()) {
    inputsAndOutputs.forEachNonPrimaryInput(
        [&](const InputFile &input) -> bool {
          subconsumers.emplace_back(input.file(), nullptr);
          return false;
        });
  }

  return FileSpecificDiagnosticConsumer::consolidateSubconsumers(subconsumers);
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified by \p inputsAndOutputs.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createSerializedDiagnosticConsumerIfNeeded(
    const FrontendInputsAndOutputs &inputsAndOutputs) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      inputsAndOutputs,
      [](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
    std::string serializedDiagnosticsPath = input.serializedDiagnosticsPath();
    if (serializedDiagnosticsPath.empty())
      return nullptr;
    return serialized_diagnostics::createConsumer(serializedDiagnosticsPath);
  });
}

/// Creates a diagnostic consumer that handles serializing diagnostics, based on
/// the supplementary output paths specified in \p options.
///
/// The returned consumer will handle producing multiple serialized diagnostics
/// files if necessary, by using sub-consumers for each file and dispatching to
/// the right one.
///
/// If no serialized diagnostics are being produced, returns null.
static std::unique_ptr<DiagnosticConsumer>
createJSONFixItDiagnosticConsumerIfNeeded(
    const CompilerInvocation &invocation) {
  return createDispatchingDiagnosticConsumerIfNeeded(
      invocation.getFrontendOptions().InputsAndOutputs,
      [&](const InputFile &input) -> std::unique_ptr<DiagnosticConsumer> {
    std::string fixItsOutputPath = input.fixItsOutputPath();
    if (fixItsOutputPath.empty())
      return nullptr;
    return std::make_unique<JSONFixitWriter>(
        fixItsOutputPath, invocation.getDiagnosticOptions());
  });
}

/// Print information about a
static void printCompatibilityLibrary(
    llvm::VersionTuple runtimeVersion, llvm::VersionTuple maxVersion,
    StringRef filter, StringRef libraryName, bool &printedAny,
    llvm::raw_ostream &out) {
  if (runtimeVersion > maxVersion)
    return;

  if (printedAny) {
    out << ",";
  }

  out << "\n";
  out << "      {\n";

  out << "        \"libraryName\": \"";
  out.write_escaped(libraryName);
  out << "\",\n";

  out << "        \"filter\": \"";
  out.write_escaped(filter);
  out << "\"\n";
  out << "      }";

  printedAny = true;
}

/// Print information about the target triple in JSON.
static void printTripleInfo(const llvm::Triple &triple,
                            llvm::Optional<llvm::VersionTuple> runtimeVersion,
                            llvm::raw_ostream &out) {
  out << "{\n";

  out << "    \"triple\": \"";
  out.write_escaped(triple.getTriple());
  out << "\",\n";

  out << "    \"unversionedTriple\": \"";
  out.write_escaped(getUnversionedTriple(triple).getTriple());
  out << "\",\n";

  out << "    \"moduleTriple\": \"";
  out.write_escaped(getTargetSpecificModuleTriple(triple).getTriple());
  out << "\",\n";

  if (runtimeVersion) {
    out << "    \"swiftRuntimeCompatibilityVersion\": \"";
    out.write_escaped(runtimeVersion->getAsString());
    out << "\",\n";

    // Compatibility libraries that need to be linked.
    out << "    \"compatibilityLibraries\": [";
    bool printedAnyCompatibilityLibrary = false;
    #define BACK_DEPLOYMENT_LIB(Version, Filter, LibraryName)           \
      printCompatibilityLibrary(                                        \
        *runtimeVersion, llvm::VersionTuple Version, #Filter, LibraryName, \
        printedAnyCompatibilityLibrary, out);
    #include "swift/Frontend/BackDeploymentLibs.def"

    if (printedAnyCompatibilityLibrary) {
      out << "\n   ";
    }
    out << " ],\n";
  } else {
    out << "    \"compatibilityLibraries\": [ ],\n";
  }

  out << "    \"librariesRequireRPath\": "
      << (tripleRequiresRPathForSwiftInOS(triple) ? "true" : "false")
      << "\n";

  out << "  }";

}

/// Print information about the selected target in JSON.
static void printTargetInfo(const CompilerInvocation &invocation,
                            llvm::raw_ostream &out) {
  out << "{\n";

  // Compiler version, as produced by --version.
  out << "  \"compilerVersion\": \"";
  out.write_escaped(version::getSwiftFullVersion(
                                                 version::Version::getCurrentLanguageVersion()));
  out << "\",\n";

  // Target triple and target variant triple.
  auto runtimeVersion =
    invocation.getIRGenOptions().AutolinkRuntimeCompatibilityLibraryVersion;
  auto &langOpts = invocation.getLangOptions();
  out << "  \"target\": ";
  printTripleInfo(langOpts.Target, runtimeVersion, out);
  out << ",\n";

  if (auto &variant = langOpts.TargetVariant) {
    out << "  \"targetVariant\": ";
    printTripleInfo(*variant, runtimeVersion, out);
    out << ",\n";
  }

  // Various paths.
  auto &searchOpts = invocation.getSearchPathOptions();
  out << "  \"paths\": {\n";

  if (!searchOpts.SDKPath.empty()) {
    out << "    \"sdkPath\": \"";
    out.write_escaped(searchOpts.SDKPath);
    out << "\",\n";
  }

  auto outputPaths = [&](StringRef name, const std::vector<std::string> &paths){
    out << "    \"" << name << "\": [\n";
    llvm::interleave(paths, [&out](const std::string &path) {
      out << "      \"";
      out.write_escaped(path);
      out << "\"";
    }, [&out] {
      out << ",\n";
    });
    out << "\n    ],\n";
  };

  outputPaths("runtimeLibraryPaths", searchOpts.RuntimeLibraryPaths);
  outputPaths("runtimeLibraryImportPaths",
              searchOpts.RuntimeLibraryImportPaths);

  out << "    \"runtimeResourcePath\": \"";
  out.write_escaped(searchOpts.RuntimeResourcePath);
  out << "\"\n";

  out << "  }\n";

  out << "}\n";
}

int swift::performFrontend(ArrayRef<const char *> Args,
                           const char *Argv0, void *MainAddr,
                           FrontendObserver *observer) {
  INITIALIZE_LLVM();
  llvm::EnablePrettyStackTraceOnSigInfoForThisThread();

  PrintingDiagnosticConsumer PDC;

  // Hopefully we won't trigger any LLVM-level fatal errors, but if we do try
  // to route them through our usual textual diagnostics before crashing.
  //
  // Unfortunately it's not really safe to do anything else, since very
  // low-level operations in LLVM can trigger fatal errors.
  auto diagnoseFatalError = [&PDC](const std::string &reason, bool shouldCrash){
    static const std::string *recursiveFatalError = nullptr;
    if (recursiveFatalError) {
      // Report the /original/ error through LLVM's default handler, not
      // whatever we encountered.
      llvm::remove_fatal_error_handler();
      llvm::report_fatal_error(*recursiveFatalError, shouldCrash);
    }
    recursiveFatalError = &reason;

    SourceManager dummyMgr;

    DiagnosticInfo errorInfo(
        DiagID(0), SourceLoc(), DiagnosticKind::Error,
        "fatal error encountered during compilation; please file a bug report "
        "with your project and the crash log",
        {}, SourceLoc(), {}, {}, {}, false);
    DiagnosticInfo noteInfo(DiagID(0), SourceLoc(), DiagnosticKind::Note,
                            reason, {}, SourceLoc(), {}, {}, {}, false);
    PDC.handleDiagnostic(dummyMgr, errorInfo);
    PDC.handleDiagnostic(dummyMgr, noteInfo);
    if (shouldCrash)
      abort();
  };
  llvm::ScopedFatalErrorHandler handler([](void *rawCallback,
                                           const std::string &reason,
                                           bool shouldCrash) {
    auto *callback = static_cast<decltype(&diagnoseFatalError)>(rawCallback);
    (*callback)(reason, shouldCrash);
  }, &diagnoseFatalError);

  std::unique_ptr<CompilerInstance> Instance =
    std::make_unique<CompilerInstance>();
  Instance->addDiagnosticConsumer(&PDC);

  struct FinishDiagProcessingCheckRAII {
    bool CalledFinishDiagProcessing = false;
    ~FinishDiagProcessingCheckRAII() {
      assert(CalledFinishDiagProcessing && "returned from the function "
        "without calling finishDiagProcessing");
    }
  } FinishDiagProcessingCheckRAII;

  auto finishDiagProcessing = [&](int retValue, bool verifierEnabled) -> int {
    FinishDiagProcessingCheckRAII.CalledFinishDiagProcessing = true;
    PDC.setSuppressOutput(false);
    bool diagnosticsError = Instance->getDiags().finishProcessing();
    // If the verifier is enabled and did not encounter any verification errors,
    // return 0 even if the compile failed. This behavior isn't ideal, but large
    // parts of the test suite are reliant on it.
    if (verifierEnabled && !diagnosticsError) {
      return 0;
    }
    return retValue ? retValue : diagnosticsError;
  };

  if (Args.empty()) {
    Instance->getDiags().diagnose(SourceLoc(), diag::error_no_frontend_args);
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  CompilerInvocation Invocation;

  SmallString<128> workingDirectory;
  llvm::sys::fs::current_path(workingDirectory);

  std::string MainExecutablePath =
      llvm::sys::fs::getMainExecutable(Argv0, MainAddr);

  // Parse arguments.
  SmallVector<std::unique_ptr<llvm::MemoryBuffer>, 4> configurationFileBuffers;
  if (Invocation.parseArgs(Args, Instance->getDiags(),
                           &configurationFileBuffers, workingDirectory,
                           MainExecutablePath)) {
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  // Make an array of PrettyStackTrace objects to dump the configuration files
  // we used to parse the arguments. These are RAII objects, so they and the
  // buffers they refer to must be kept alive in order to be useful. (That is,
  // we want them to be alive for the entire rest of performFrontend.)
  //
  // This can't be a SmallVector or similar because PrettyStackTraces can't be
  // moved (or copied)...and it can't be an array of non-optionals because
  // PrettyStackTraces can't be default-constructed. So we end up with a
  // dynamically-sized array of optional PrettyStackTraces, which get
  // initialized by iterating over the buffers we collected above.
  auto configurationFileStackTraces =
      std::make_unique<Optional<PrettyStackTraceFileContents>[]>(
        configurationFileBuffers.size());
  for_each(configurationFileBuffers.begin(), configurationFileBuffers.end(),
           &configurationFileStackTraces[0],
           [](const std::unique_ptr<llvm::MemoryBuffer> &buffer,
              Optional<PrettyStackTraceFileContents> &trace) {
    trace.emplace(*buffer);
  });

  // Setting DWARF Version depend on platform
  IRGenOptions &IRGenOpts = Invocation.getIRGenOptions();
  IRGenOpts.DWARFVersion = swift::DWARFVersion;

  // The compiler invocation is now fully configured; notify our observer.
  if (observer) {
    observer->parsedArgs(Invocation);
  }

  if (Invocation.getFrontendOptions().PrintHelp ||
      Invocation.getFrontendOptions().PrintHelpHidden) {
    unsigned IncludedFlagsBitmask = options::FrontendOption;
    unsigned ExcludedFlagsBitmask =
      Invocation.getFrontendOptions().PrintHelpHidden ? 0 :
                                                        llvm::opt::HelpHidden;
    std::unique_ptr<llvm::opt::OptTable> Options(createSwiftOptTable());
    Options->PrintHelp(llvm::outs(), displayName(MainExecutablePath).c_str(),
                       "Swift frontend", IncludedFlagsBitmask,
                       ExcludedFlagsBitmask, /*ShowAllAliases*/false);
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().PrintTargetInfo) {
    printTargetInfo(Invocation, llvm::outs());
    return finishDiagProcessing(0, /*verifierEnabled*/ false);
  }

  if (Invocation.getFrontendOptions().RequestedAction ==
      FrontendOptions::ActionType::NoneAction) {
    Instance->getDiags().diagnose(SourceLoc(),
                                  diag::error_missing_frontend_action);
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  // Because the serialized diagnostics consumer is initialized here,
  // diagnostics emitted above, within CompilerInvocation::parseArgs, are never
  // serialized. This is a non-issue because, in nearly all cases, frontend
  // arguments are generated by the driver, not directly by a user. The driver
  // is responsible for emitting diagnostics for its own errors. See SR-2683
  // for details.
  std::unique_ptr<DiagnosticConsumer> SerializedConsumerDispatcher =
      createSerializedDiagnosticConsumerIfNeeded(
        Invocation.getFrontendOptions().InputsAndOutputs);
  if (SerializedConsumerDispatcher)
    Instance->addDiagnosticConsumer(SerializedConsumerDispatcher.get());

  std::unique_ptr<DiagnosticConsumer> FixItsConsumer =
      createJSONFixItDiagnosticConsumerIfNeeded(Invocation);
  if (FixItsConsumer)
    Instance->addDiagnosticConsumer(FixItsConsumer.get());

  if (Invocation.getDiagnosticOptions().UseColor)
    PDC.forceColors();

  PDC.setPrintEducationalNotes(
      Invocation.getDiagnosticOptions().PrintEducationalNotes);

  PDC.setFormattingStyle(
      Invocation.getDiagnosticOptions().PrintedFormattingStyle);

  if (Invocation.getFrontendOptions().PrintStats) {
    llvm::EnableStatistics();
  }

  const DiagnosticOptions &diagOpts = Invocation.getDiagnosticOptions();
  bool verifierEnabled = diagOpts.VerifyMode != DiagnosticOptions::NoVerify;

  if (Instance->setup(Invocation)) {
    return finishDiagProcessing(1, /*verifierEnabled*/ false);
  }

  // The compiler instance has been configured; notify our observer.
  if (observer) {
    observer->configuredCompiler(*Instance);
  }

  if (verifierEnabled) {
    // Suppress printed diagnostic output during the compile if the verifier is
    // enabled.
    PDC.setSuppressOutput(true);
  }

  int ReturnValue = 0;
  bool HadError = performCompile(Invocation, *Instance, Args, ReturnValue, observer);

  if (verifierEnabled) {
    DiagnosticEngine &diags = Instance->getDiags();
    if (diags.hasFatalErrorOccurred() &&
        !Invocation.getDiagnosticOptions().ShowDiagnosticsAfterFatalError) {
      diags.resetHadAnyError();
      PDC.setSuppressOutput(false);
      diags.diagnose(SourceLoc(), diag::verify_encountered_fatal);
      HadError = true;
    }
  }

  auto r = finishDiagProcessing(HadError ? 1 : ReturnValue, verifierEnabled);
  if (auto *StatsReporter = Instance->getStatsReporter())
    StatsReporter->noteCurrentProcessExitStatus(r);
  return r;
}

void FrontendObserver::parsedArgs(CompilerInvocation &invocation) {}
void FrontendObserver::configuredCompiler(CompilerInstance &instance) {}
void FrontendObserver::performedSemanticAnalysis(CompilerInstance &instance) {}
void FrontendObserver::performedSILGeneration(SILModule &module) {}
void FrontendObserver::performedSILProcessing(SILModule &module) {}
