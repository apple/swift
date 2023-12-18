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
#include "swift/Parse/Lexer.h"
#include "llvm/Config/config.h"
#include "llvm/Support/VirtualFileSystem.h"

using namespace swift;

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

/// Get plugin module name from \p path if the path looks like a shared library
/// path. Otherwise, returns an empty string.
static StringRef pluginModuleNameStringFromPath(StringRef path) {
  // Plugin library must be named 'lib${module name}(.dylib|.so|.dll)'.
  // FIXME: Shared library prefix might be different between platforms.
#if defined(_WIN32)
  constexpr StringRef libPrefix{};
  constexpr StringRef libSuffix = ".dll";
#else
  constexpr StringRef libPrefix = "lib";
  constexpr StringRef libSuffix = LTDL_SHLIB_EXT;
#endif

  StringRef filename = llvm::sys::path::filename(path);
  if (filename.starts_with(libPrefix) && filename.ends_with(libSuffix)) {
    // We don't check if the result it a valid identifier. Even if we put
    // invalid name in the lookup table, clients wound not be able to lookup
    // that name, thus harmless.
    return filename.drop_front(libPrefix.size()).drop_back(libSuffix.size());
  }
  return "";
}

static llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
getPluginLoadingFS(ASTContext &Ctx) {
  // If there is a clang include tree FS, using real file system to load plugin
  // as the FS in SourceMgr doesn't support directory iterator.
  if (Ctx.ClangImporterOpts.HasClangIncludeTreeRoot)
    return llvm::vfs::getRealFileSystem();
  return Ctx.SourceMgr.getFileSystem();
}

llvm::DenseMap<Identifier, PluginLoader::PluginEntry> &
PluginLoader::getPluginMap() {
  if (PluginMap.has_value()) {
    return PluginMap.value();
  }

  // Create and populate the map.
  PluginMap.emplace();
  auto &map = PluginMap.value();

  // Helper function to try inserting an entry if there's no existing entry
  // associated with the module name.
  auto try_emplace = [&](StringRef moduleName, StringRef libPath,
                         StringRef execPath) {
    auto moduleNameIdentifier = Ctx.getIdentifier(moduleName);
    if (map.find(moduleNameIdentifier) != map.end()) {
      // Specified module name is already in the map.
      return;
    }

    libPath = libPath.empty() ? "" : Ctx.AllocateCopy(libPath);
    execPath = execPath.empty() ? "" : Ctx.AllocateCopy(execPath);
    auto result = map.insert({moduleNameIdentifier, {libPath, execPath}});
    assert(result.second);
    (void)result;
  };

  auto fs = getPluginLoadingFS(Ctx);
  std::error_code ec;

  for (auto &entry : Ctx.SearchPathOpts.PluginSearchOpts) {
    switch (entry.getKind()) {

    // '-load-plugin-library <library path>'.
    case PluginSearchOption::Kind::LoadPluginLibrary: {
      auto &val = entry.get<PluginSearchOption::LoadPluginLibrary>();
      auto moduleName = pluginModuleNameStringFromPath(val.LibraryPath);
      if (!moduleName.empty()) {
        try_emplace(moduleName, val.LibraryPath, /*executablePath=*/"");
      }
      continue;
    }

    // '-load-plugin-executable <executable path>#<module name>, ...'.
    case PluginSearchOption::Kind::LoadPluginExecutable: {
      auto &val = entry.get<PluginSearchOption::LoadPluginExecutable>();
      assert(!val.ExecutablePath.empty() && "empty plugin path");
      for (auto &moduleName : val.ModuleNames) {
        try_emplace(moduleName, /*libraryPath=*/"", val.ExecutablePath);
      }
      continue;
    }

    // '-plugin-path <library search path>'.
    case PluginSearchOption::Kind::PluginPath: {
      auto &val = entry.get<PluginSearchOption::PluginPath>();
      for (auto i = fs->dir_begin(val.SearchPath, ec);
           i != llvm::vfs::directory_iterator(); i = i.increment(ec)) {
        auto libPath = i->path();
        auto moduleName = pluginModuleNameStringFromPath(libPath);
        if (!moduleName.empty()) {
          try_emplace(moduleName, libPath, /*executablePath=*/"");
        }
      }
      continue;
    }

    // '-external-plugin-path <library search path>#<server path>'.
    case PluginSearchOption::Kind::ExternalPluginPath: {
      auto &val = entry.get<PluginSearchOption::ExternalPluginPath>();
      for (auto i = fs->dir_begin(val.SearchPath, ec);
           i != llvm::vfs::directory_iterator(); i = i.increment(ec)) {
        auto libPath = i->path();
        auto moduleName = pluginModuleNameStringFromPath(libPath);
        if (!moduleName.empty()) {
          try_emplace(moduleName, libPath, val.ServerPath);
        }
      }
      continue;
    }
    }
    llvm_unreachable("unhandled PluginSearchOption::Kind");
  }

  return map;
}

const PluginLoader::PluginEntry &
PluginLoader::lookupPluginByModuleName(Identifier moduleName) {
  auto &map = getPluginMap();
  auto found = map.find(moduleName);
  if (found == map.end()) {
    static PluginEntry notFound{"", ""};
    return notFound;
  }

  // Track the dependency.
  recordDependency(found->second, moduleName);
  return found->second;
}

llvm::Expected<LoadedLibraryPlugin *>
PluginLoader::loadLibraryPlugin(StringRef path) {
  auto fs = getPluginLoadingFS(Ctx);
  SmallString<128> resolvedPath;
  if (auto err = fs->getRealPath(path, resolvedPath)) {
    return llvm::createStringError(err, err.message());
  }

  // Load the plugin.
  auto plugin = getRegistry()->loadLibraryPlugin(resolvedPath);
  if (!plugin) {
    resolvedPath.push_back(0);
    return llvm::handleErrors(
        plugin.takeError(), [&](const llvm::ErrorInfoBase &err) {
          return llvm::createStringError(
              err.convertToErrorCode(),
              "compiler plugin '%s' could not be loaded;  %s",
              resolvedPath.data(), err.message().data());
        });
  }

  return plugin;
}

llvm::Expected<LoadedExecutablePlugin *>
PluginLoader::loadExecutablePlugin(StringRef path) {
  auto fs = getPluginLoadingFS(Ctx);
  SmallString<128> resolvedPath;
  if (auto err = fs->getRealPath(path, resolvedPath)) {
    return llvm::createStringError(err, err.message());
  }

  // Load the plugin.
  auto plugin =
      getRegistry()->loadExecutablePlugin(resolvedPath, disableSandbox);
  if (!plugin) {
    resolvedPath.push_back(0);
    return llvm::handleErrors(
        plugin.takeError(), [&](const llvm::ErrorInfoBase &err) {
          return llvm::createStringError(
              err.convertToErrorCode(),
              "compiler plugin '%s' could not be loaded: %s",
              resolvedPath.data(), err.message().data());
        });
  }

  return plugin;
}

void PluginLoader::recordDependency(const PluginEntry &plugin,
                                    Identifier moduleName) {
  if (!DepTracker)
    return;

  // libraryPath: non-nil, executablePath: nil: in-process library plugin.
  // libraryPath: non-nil, executablePath: non-nil: external library plugin.
  // libraryPath: nil, executablePath: non-nil: executable plugin.
  StringRef path =
      !plugin.libraryPath.empty() ? plugin.libraryPath : plugin.executablePath;

  // NOTE: We don't track plugin-server path as a dependency because it doesn't
  // provide much value.

  assert(!path.empty());
  SmallString<128> resolvedPath;
  auto fs = Ctx.SourceMgr.getFileSystem();
  if (auto err = fs->getRealPath(path, resolvedPath)) {
    return;
  }

  DepTracker->addMacroPluginDependency(resolvedPath, moduleName);
}
