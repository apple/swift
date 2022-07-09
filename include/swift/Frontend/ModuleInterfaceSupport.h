//===------ ModuleInterfaceSupport.h - swiftinterface files -----*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2019 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_FRONTEND_MODULEINTERFACESUPPORT_H
#define SWIFT_FRONTEND_MODULEINTERFACESUPPORT_H

#include "swift/Basic/LLVM.h"
#include "swift/Basic/Version.h"
#include "llvm/Support/Regex.h"

#define SWIFT_INTERFACE_FORMAT_VERSION_KEY "swift-interface-format-version"
#define SWIFT_COMPILER_VERSION_KEY "swift-compiler-version"
#define SWIFT_MODULE_FLAGS_KEY "swift-module-flags"
#define SWIFT_MODULE_FLAGS_IGNORABLE_KEY "swift-module-flags-ignorable"

namespace swift {

class ASTContext;
class ModuleDecl;

/// Options for controlling the generation of the .swiftinterface output.
struct ModuleInterfaceOptions {
  /// Should we prefer printing TypeReprs when writing out types in a module
  /// interface, or should we fully-qualify them?
  bool PreserveTypesAsWritten = false;

  /// See \ref FrontendOptions.PrintFullConvention.
  /// [TODO: Clang-type-plumbing] This check should go away.
  bool PrintFullConvention = false;

  /// Copy of all the command-line flags passed at .swiftinterface
  /// generation time, re-applied to CompilerInvocation when reading
  /// back .swiftinterface and reconstructing .swiftmodule.
  std::string Flags;

  /// Flags that should be emitted to the .swiftinterface file but are OK to be
  /// ignored by the earlier version of the compiler.
  std::string IgnorableFlags;

  /// Print SPI decls and attributes.
  bool PrintSPIs = false;

  /// Print imports with both @_implementationOnly and @_spi, only applies
  /// when PrintSPIs is true.
  bool ExperimentalSPIImports = false;

  /// Intentionally print invalid syntax into the file.
  bool DebugPrintInvalidSyntax = false;

  /// A list of modules we shouldn't import in the public interfaces.
  std::vector<std::string> ModulesToSkipInPublicInterface;
};

extern version::Version InterfaceFormatVersion;
std::string getSwiftInterfaceCompilerVersionForCurrentCompiler(ASTContext &ctx);

llvm::Regex getSwiftInterfaceFormatVersionRegex();
llvm::Regex getSwiftInterfaceCompilerVersionRegex();

/// Emit a stable module interface for \p M, which can be used by a client
/// source file to import this module, subject to options given by \p Opts.
///
/// Unlike a serialized module, the textual format generated by
/// emitSwiftInterface is intended to be stable across compiler versions while
/// still describing the full ABI of the module in question.
///
/// The initial plan for this format can be found at
/// https://forums.swift.org/t/plan-for-module-stability/14551/
///
/// \return true if an error occurred
///
/// \sa swift::serialize
bool emitSwiftInterface(raw_ostream &out,
                        ModuleInterfaceOptions const &Opts,
                        ModuleDecl *M);

} // end namespace swift

#endif
