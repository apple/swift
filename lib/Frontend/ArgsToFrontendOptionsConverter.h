//===------ArgsToFrontendOptionsConverter.h-- --------------------*-C++ -*-===//
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

#ifndef SWIFT_FRONTEND_ARGSTOFRONTENDOPTIONSCONVERTER_H
#define SWIFT_FRONTEND_ARGSTOFRONTENDOPTIONSCONVERTER_H

#include "swift/AST/DiagnosticConsumer.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/Frontend/FrontendOptions.h"
#include "swift/Option/Options.h"
#include "llvm/Option/ArgList.h"

#include <vector>

namespace swift {

class ArgsToFrontendOptionsConverter {
private:
  DiagnosticEngine &Diags;
  const llvm::opt::ArgList &Args;
  FrontendOptions &Opts;

  llvm::Optional<std::vector<std::string>>
      cachedOutputFilenamesFromCommandLineOrFilelist;

  void handleDebugCrashGroupArguments();

  void computeDebugTimeOptions();
  bool computeFallbackModuleName();
  bool computeModuleName();
  bool computeModuleAliases();
  bool computeMainAndSupplementaryOutputFilenames();
  void computeDumpScopeMapLocations();
  void computeHelpOptions();
  void computeImplicitImportModuleNames(llvm::opt::OptSpecifier id,
                                        bool isTestable);
  void computeImportObjCHeaderOptions();
  void computeLLVMArgs();
  void computePlaygroundOptions();
  void computePrintStatsOptions();
  void computeTBDOptions();

  bool setUpImmediateArgs();

  bool checkUnusedSupplementaryOutputPaths() const;

  bool checkForUnusedOutputPaths() const;

  bool checkBuildFromInterfaceOnlyOptions() const;

public:
  ArgsToFrontendOptionsConverter(DiagnosticEngine &Diags,
                                 const llvm::opt::ArgList &Args,
                                 FrontendOptions &Opts)
      : Diags(Diags), Args(Args), Opts(Opts) {}

  /// Populates the FrontendOptions the converter was initialized with.
  ///
  /// \param buffers If present, buffers read in the processing of the frontend
  /// options will be saved here. These should only be used for debugging
  /// purposes.
  bool convert(
      SmallVectorImpl<std::unique_ptr<llvm::MemoryBuffer>> *buffers);

  static FrontendOptions::ActionType
  determineRequestedAction(const llvm::opt::ArgList &);
};

class ModuleAliasesConverter {
public:
  /// Sets the \c ModuleAliasMap in the \c FrontendOptions with args passed via `-module-alias`.
  ///
  /// \param args The arguments to `-module-alias`. If input has `-module-alias Foo=Bar
  ///             -module-alias Baz=Qux`, the args are ['Foo=Bar', 'Baz=Qux'].  The name
  ///             Foo is the name that appears in source files, while it maps to Bar, the name
  ///             of the binary on disk, /path/to/Bar.swiftmodule(interface), under the hood.
  /// \param options FrontendOptions containing the module alias map to set args to.
  /// \param diags Used to print diagnostics in case validation of the args fails.
  /// \return Whether the validation passed and successfully set the module alias map
  static bool computeModuleAliases(std::vector<std::string> args,
                                   FrontendOptions &options,
                                   DiagnosticEngine &diags);
};

} // namespace swift

#endif /* SWIFT_FRONTEND_ARGSTOFRONTENDOPTIONSCONVERTER_H */
