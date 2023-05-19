//===--- PluginLoader.cpp -------------------------------------------------===//
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

#include "swift/AST/PluginLoader.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/SourceManager.h"
#include "llvm/Config/config.h"

using namespace swift;

void PluginLoader::createModuleToExecutablePluginMap() {
  for (auto &arg : Ctx.SearchPathOpts.getCompilerPluginExecutablePaths()) {
    // Create a moduleName -> pluginPath mapping.
    assert(!arg.ExecutablePath.empty() && "empty plugin path");
    StringRef pathStr = Ctx.AllocateCopy(arg.ExecutablePath);
    for (auto moduleName : arg.ModuleNames) {
      ExecutablePluginPaths[Ctx.getIdentifier(moduleName)] = pathStr;
    }
  }
}

void PluginLoader::setRegistry(PluginRegistry *newValue) {
  assert(Registry == nullptr && "Too late to set a new plugin registry");
  Registry = newValue;
}

PluginRegistry *PluginLoader::getRegistry() {
  // Create a new one if it hasn't been set.
  if (!Registry) {
    Registry = new PluginRegistry();
    OwnedRegistry.reset(Registry);
  }

  assert(Registry != nullptr);
  return Registry;
}

llvm::Optional<std::string>
PluginLoader::lookupExplicitLibraryPluginByModuleName(Identifier moduleName) {
  // Look for 'lib${module name}(.dylib|.so)'.
  SmallString<64> expectedBasename;
  expectedBasename.append("lib");
  expectedBasename.append(moduleName.str());
  expectedBasename.append(LTDL_SHLIB_EXT);

  // Try '-load-plugin-library'.
  for (const auto &libPath :
       Ctx.SearchPathOpts.getCompilerPluginLibraryPaths()) {
    if (llvm::sys::path::filename(libPath) == expectedBasename) {
      return libPath;
    }
  }
  return None;
}

llvm::Optional<std::string>
PluginLoader::lookupLibraryPluginInSearchPathByModuleName(
    Identifier moduleName) {
  // Look for 'lib${module name}(.dylib|.so)'.
  SmallString<64> expectedBasename;
  expectedBasename.append("lib");
  expectedBasename.append(moduleName.str());
  expectedBasename.append(LTDL_SHLIB_EXT);

  // Try '-plugin-path'.
  auto fs = Ctx.SourceMgr.getFileSystem();
  for (const auto &searchPath : Ctx.SearchPathOpts.PluginSearchPaths) {
    SmallString<128> fullPath(searchPath);
    llvm::sys::path::append(fullPath, expectedBasename);
    if (fs->exists(fullPath)) {
      return std::string(fullPath);
    }
  }

  return None;
}

Optional<std::pair<std::string, std::string>>
PluginLoader::lookupExternalLibraryPluginByModuleName(Identifier moduleName) {
  auto fs = Ctx.SourceMgr.getFileSystem();

  for (auto &pair : Ctx.SearchPathOpts.ExternalPluginSearchPaths) {
    SmallString<128> fullPath(pair.SearchPath);
    llvm::sys::path::append(fullPath,
                            "lib" + moduleName.str() + LTDL_SHLIB_EXT);

    if (fs->exists(fullPath)) {
      return {{std::string(fullPath), pair.ServerPath}};
    }
  }
  return None;
}

Optional<StringRef>
PluginLoader::lookupExecutablePluginByModuleName(Identifier moduleName) {
  auto &execPluginPaths = ExecutablePluginPaths;
  auto found = execPluginPaths.find(moduleName);
  if (found == execPluginPaths.end())
    return None;
  return found->second;
}

LoadedLibraryPlugin *PluginLoader::loadLibraryPlugin(StringRef path) {
  auto fs = Ctx.SourceMgr.getFileSystem();
  SmallString<128> resolvedPath;
  if (auto err = fs->getRealPath(path, resolvedPath)) {
    Ctx.Diags.diagnose(SourceLoc(), diag::compiler_plugin_not_loaded, path,
                       err.message());
    return nullptr;
  }

  // Track the dependency.
  if (DepTracker)
    DepTracker->addDependency(resolvedPath, /*IsSystem=*/false);

  // Load the plugin.
  auto plugin = getRegistry()->loadLibraryPlugin(resolvedPath);
  if (!plugin) {
    Ctx.Diags.diagnose(SourceLoc(), diag::compiler_plugin_not_loaded, path,
                       llvm::toString(plugin.takeError()));
    return nullptr;
  }

  return plugin.get();
}

LoadedExecutablePlugin *PluginLoader::loadExecutablePlugin(StringRef path) {
  auto fs = Ctx.SourceMgr.getFileSystem();
  SmallString<128> resolvedPath;
  if (auto err = fs->getRealPath(path, resolvedPath)) {
    Ctx.Diags.diagnose(SourceLoc(), diag::compiler_plugin_not_loaded, path,
                       err.message());
    return nullptr;
  }

  // Track the dependency.
  if (DepTracker)
    DepTracker->addDependency(resolvedPath, /*IsSystem=*/false);

  // Load the plugin.
  auto plugin = getRegistry()->loadExecutablePlugin(resolvedPath);
  if (!plugin) {
    Ctx.Diags.diagnose(SourceLoc(), diag::compiler_plugin_not_loaded, path,
                       llvm::toString(plugin.takeError()));
    return nullptr;
  }

  return plugin.get();
}
