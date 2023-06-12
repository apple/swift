//===--- SyntacticMacroExpansion.h ----------------------------------------===//
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

#ifndef SWIFT_IDE_SYNTACTICMACROEXPANSION_H
#define SWIFT_IDE_SYNTACTICMACROEXPANSION_H

#include "swift/AST/Decl.h"
#include "swift/AST/MacroDefinition.h"
#include "swift/AST/PluginRegistry.h"
#include "swift/Basic/Fingerprint.h"
#include "swift/Frontend/Frontend.h"
#include "llvm/Support/MemoryBuffer.h"

namespace swift {

class ASTContext;
class SourceFile;

namespace ide {
class SourceEditConsumer;

/// Simple object to specify a syntactic macro expansion.
struct MacroExpansionSpecifier {
  unsigned offset;
  swift::MacroRoles macroRoles;
  swift::MacroDefinition macroDefinition;
};

/// Instance of a syntactic macro expansion context. This is created for each
/// list of compiler arguments (i.e. 'argHash'), and reused as long as the
/// compiler arguments are not changed.
class SyntacticMacroExpansionInstance {
  CompilerInvocation invocation;

  SourceManager SourceMgr;
  DiagnosticEngine Diags{SourceMgr};
  std::unique_ptr<ASTContext> Ctx;
  ModuleDecl *TheModule = nullptr;
  llvm::StringMap<MacroDecl *> MacroDecls;

  /// Create 'SourceFile' using the buffer.
  swift::SourceFile *getSourceFile(llvm::MemoryBuffer *inputBuf);

  /// Synthesize 'MacroDecl' AST object to use the expansion.
  swift::MacroDecl *
  getSynthesizedMacroDecl(swift::Identifier name,
                          const MacroExpansionSpecifier &expansion);

  /// Expand single 'expansion' in SF.
  void expand(swift::SourceFile *SF,
                    const MacroExpansionSpecifier &expansion,
                    SourceEditConsumer &consumer);

public:
  SyntacticMacroExpansionInstance() {}

  /// Setup the instance with \p args .
  bool setup(StringRef SwiftExecutablePath, ArrayRef<const char *> args,
             std::shared_ptr<PluginRegistry> plugins, std::string &error);

  ASTContext &getASTContext() { return *Ctx; }

  /// Expand all macros in \p inputBuf and send the edit results to \p consumer.
  /// Expansions are specified by \p expansions .
  void expandAll(llvm::MemoryBuffer *inputBuf,
                     ArrayRef<MacroExpansionSpecifier> expansions,
                     SourceEditConsumer &consumer);
};

/// Manager object to vend 'SyntacticMacroExpansionInstance'.
class SyntacticMacroExpansion {
  StringRef SwiftExecutablePath;
  std::shared_ptr<PluginRegistry> Plugins;

public:
  SyntacticMacroExpansion(StringRef SwiftExecutablePath,
                          std::shared_ptr<PluginRegistry> Plugins)
      : SwiftExecutablePath(SwiftExecutablePath), Plugins(Plugins) {}

  /// Get instance configured with the specified compiler arguments.
  std::shared_ptr<SyntacticMacroExpansionInstance>
  getInstance(ArrayRef<const char *> args, std::string &error);
};

} // namespace ide
} // namespace swift

#endif // SWIFT_IDE_SYNTACTICMACROEXPANSION_H
