//===--- PluginLoader.h -----------------------------------------*- C++ -*-===//
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
#ifndef SWIFT_AST_PLUGIN_LOADER_H
#define SWIFT_AST_PLUGIN_LOADER_H

#include "swift/AST/ModuleLoader.h"
#include "swift/AST/PluginRegistry.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringRef.h"

namespace swift {

class ASTContext;

/// Compiler plugin loader tied to an ASTContext. This is responsible for:
///  * Find plugins based on the module name
///  * Load plugins resolving VFS paths
///  * Track plugin dependencies
class PluginLoader {
  /// Plugin registry. Lazily populated by get/setRegistry().
  /// NOTE: Do not reference this directly. Use getRegistry().
  PluginRegistry *Registry = nullptr;

  /// `Registry` storage if this owns it.
  std::unique_ptr<PluginRegistry> OwnedRegistry = nullptr;

  ASTContext &Ctx;
  DependencyTracker *DepTracker;

  /// Map a module name to an executable plugin path that provides the module.
  llvm::DenseMap<swift::Identifier, llvm::StringRef> ExecutablePluginPaths;

  void createModuleToExecutablePluginMap();

public:
  PluginLoader(ASTContext &Ctx, DependencyTracker *DepTracker)
      : Ctx(Ctx), DepTracker(DepTracker) {
    createModuleToExecutablePluginMap();
  }

  void setRegistry(PluginRegistry *newValue);
  PluginRegistry *getRegistry();

  /// Lookup a library plugin that can handle \p moduleName and return the path
  /// to it from `-load-plugin-library`.
  /// The path returned can be loaded by 'loadLibraryPlugin' method.
  llvm::Optional<std::string>
  lookupExplicitLibraryPluginByModuleName(Identifier moduleName);

  /// Lookup a library plugin that can handle \p moduleName and return the path
  /// to it from `-plugin-path`.
  /// The path returned can be loaded by 'loadLibraryPlugin' method.
  llvm::Optional<std::string>
  lookupLibraryPluginInSearchPathByModuleName(Identifier moduleName);

  /// Lookup an executable plugin that is declared to handle \p moduleName
  /// module by '-load-plugin-executable'.
  /// The path returned can be loaded by 'loadExecutablePlugin' method.
  llvm::Optional<StringRef>
  lookupExecutablePluginByModuleName(Identifier moduleName);

  /// Look for dynamic libraries in paths from `-external-plugin-path` and
  /// return a pair of `(library path, plugin server executable)` if found.
  /// These paths are valid within the VFS, use `FS.getRealPath()` for their
  /// underlying path.
  llvm::Optional<std::pair<std::string, std::string>>
  lookupExternalLibraryPluginByModuleName(Identifier moduleName);

  /// Load the specified dylib plugin path resolving the path with the
  /// current VFS. If it fails to load the plugin, a diagnostic is emitted, and
  /// returns a nullptr.
  /// NOTE: This method is idempotent. If the plugin is already loaded, the same
  /// instance is simply returned.
  LoadedLibraryPlugin *loadLibraryPlugin(llvm::StringRef path);

  /// Launch the specified executable plugin path resolving the path with the
  /// current VFS. If it fails to load the plugin, a diagnostic is emitted, and
  /// returns a nullptr.
  /// NOTE: This method is idempotent. If the plugin is already loaded, the same
  /// instance is simply returned.
  LoadedExecutablePlugin *loadExecutablePlugin(llvm::StringRef path);
};

} // namespace swift

#endif // SWIFT_AST_PLUGIN_LOADER_H
