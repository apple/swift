//===--- ModuleInterfacePrinting.h - Routines to print module interface ---===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_IDE_MODULE_INTERFACE_PRINTING_H
#define SWIFT_IDE_MODULE_INTERFACE_PRINTING_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/OptionSet.h"

#include <string>
#include <vector>

namespace clang {
class Module;
}

namespace swift {
class ASTContext;
class ASTPrinter;
class ModuleDecl;
class SourceFile;
class Type;
struct PrintOptions;

namespace ide {

/// Flags used when traversing a module for printing.
enum class ModuleTraversal : unsigned {
  /// Visit modules even if their contents wouldn't be visible to name lookup.
  VisitHidden     = 0x01,
  /// Visit submodules.
  VisitSubmodules = 0x02,
  /// Skip the declarations in a Swift overlay module.
  SkipOverlay     = 0x04,
};

/// Options used to describe the traversal of a module for printing.
using ModuleTraversalOptions = OptionSet<ModuleTraversal>;

void collectModuleGroups(ModuleDecl *M, SmallVectorImpl<StringRef> &Into);

llvm::Optional<StringRef> findGroupNameForUSR(ModuleDecl *M, StringRef USR);

bool printTypeInterface(ModuleDecl *M, Type Ty, ASTPrinter &Printer,
                        std::string &TypeName, std::string &Error);

bool printTypeInterface(ModuleDecl *M, StringRef TypeUSR, ASTPrinter &Printer,
                        std::string &TypeName, std::string &Error);

void printModuleInterface(ModuleDecl *M, ArrayRef<StringRef> GroupNames,
                          ModuleTraversalOptions TraversalOptions,
                          ASTPrinter &Printer, const PrintOptions &Options,
                          const bool PrintSynthesizedExtensions);

/// Print the interface for a header that has been imported via the implicit
/// objc header importing feature.
void printHeaderInterface(StringRef Filename, ASTContext &Ctx,
                          ASTPrinter &Printer, const PrintOptions &Options);


/// Print the interface for a given swift source file.
void printSwiftSourceInterface(SourceFile &File, ASTPrinter &Printer,
                               const PrintOptions &Options);

/// Print the symbolic Swift interface for a given imported clang module.
void printSymbolicSwiftClangModuleInterface(ModuleDecl *M, ASTPrinter &Printer,
                                            const clang::Module *clangModule);

} // namespace ide

} // namespace swift

#endif // SWIFT_IDE_MODULE_INTERFACE_PRINTING_H
