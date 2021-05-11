//===--- FrontendOptions.h --------------------------------------*- C++ -*-===//
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

#ifndef SWIFT_FRONTEND_FRONTENDOPTIONS_H
#define SWIFT_FRONTEND_FRONTENDOPTIONS_H

#include "swift/Basic/FileTypes.h"
#include "swift/Basic/Version.h"
#include "swift/Frontend/FrontendInputsAndOutputs.h"
#include "swift/Frontend/InputFile.h"
#include "llvm/ADT/Hashing.h"

#include <string>
#include <vector>

namespace llvm {
  class MemoryBuffer;
}

namespace swift {
enum class IntermoduleDepTrackingMode;

/// Options for controlling the behavior of the frontend.
class FrontendOptions {
  friend class ArgsToFrontendOptionsConverter;

  /// A list of arbitrary modules to import and make implicitly visible.
  std::vector<std::pair<std::string, bool /*testable*/>>
      ImplicitImportModuleNames;

public:
  FrontendInputsAndOutputs InputsAndOutputs;

  void forAllOutputPaths(const InputFile &input,
                         llvm::function_ref<void(StringRef)> fn) const;

  bool isOutputFileDirectory() const;

  /// An Objective-C header to import and make implicitly visible.
  std::string ImplicitObjCHeaderPath;

  /// The name of the module that the frontend is building.
  std::string ModuleName;

  /// The ABI name of the module that the frontend is building, to be used in
  /// mangling and metadata.
  std::string ModuleABIName;

  /// The name of the library to link against when using this module.
  std::string ModuleLinkName;

  /// Arguments which should be passed in immediate mode.
  std::vector<std::string> ImmediateArgv;

  /// A list of arguments to forward to LLVM's option processing; this
  /// should only be used for debugging and experimental features.
  std::vector<std::string> LLVMArgs;

  /// The path to output swift interface files for the compiled source files.
  std::string DumpAPIPath;

  /// The path to collect the group information for the compiled source files.
  std::string GroupInfoPath;

  /// The path to which we should store indexing data, if any.
  std::string IndexStorePath;

  /// The path to which we should emit combined module summary file
  std::string ModuleSummaryOutputPath;

  /// The path to load access notes from.
  std::string AccessNotesPath;

  /// The path to look in when loading a module interface file, to see if a
  /// binary module has already been built for use by the compiler.
  std::string PrebuiltModuleCachePath;

  /// For these modules, we should prefer using Swift interface when importing them.
  std::vector<std::string> PreferInterfaceForModules;

  /// User-defined module version number.
  llvm::VersionTuple UserModuleVersion;

  /// Emit index data for imported serialized swift system modules.
  bool IndexSystemModules = false;

  /// If indexing system modules, don't index the stdlib.
  bool IndexIgnoreStdlib = false;

  /// The module for which we should verify all of the generic signatures.
  std::string VerifyGenericSignaturesInModule;

  /// Number of retry opening an input file if the previous opening returns
  /// bad file descriptor error.
  unsigned BadFileDescriptorRetryCount = 0;
  enum class ActionType {
    NoneAction,        ///< No specific action
    Parse,             ///< Parse only
    ResolveImports,    ///< Parse and resolve imports only
    Typecheck,         ///< Parse and type-check only
    DumpParse,         ///< Parse only and dump AST
    DumpInterfaceHash, ///< Parse and dump the interface token hash.
    EmitSyntax,        ///< Parse and dump Syntax tree as JSON
    DumpAST,           ///< Parse, type-check, and dump AST
    PrintAST,          ///< Parse, type-check, and pretty-print AST

    /// Parse and dump scope map.
    DumpScopeMaps,

    /// Parse, type-check, and dump type refinement context hierarchy
    DumpTypeRefinementContexts,

    EmitImportedModules, ///< Emit the modules that this one imports
    EmitPCH,             ///< Emit PCH of imported bridging header

    EmitSILGen, ///< Emit raw SIL
    EmitSIL,    ///< Emit canonical SIL

    EmitModuleOnly, ///< Emit module only
    MergeModules,   ///< Merge modules only

    /// Build from a swiftinterface, as close to `import` as possible
    CompileModuleFromInterface,
    /// Same as CompileModuleFromInterface, but stopping after typechecking
    TypecheckModuleFromInterface,

    EmitSIBGen, ///< Emit serialized AST + raw SIL
    EmitSIB,    ///< Emit serialized AST + canonical SIL

    Immediate, ///< Immediate mode
    REPL,      ///< REPL mode

    EmitAssembly, ///< Emit assembly
    EmitIRGen,    ///< Emit LLVM IR before LLVM optimizations
    EmitIR,       ///< Emit LLVM IR after LLVM optimizations
    EmitBC,       ///< Emit LLVM BC
    EmitObject,   ///< Emit object file

    DumpTypeInfo, ///< Dump IRGen type info

    EmitPCM, ///< Emit precompiled Clang module from a module map
    DumpPCM, ///< Dump information about a precompiled Clang module

    ScanDependencies,        ///< Scan dependencies of Swift source files
    PrintVersion,       ///< Print version information.
    PrintFeature,       ///< Print supported feature of this compiler
  };

  /// Indicates the action the user requested that the frontend perform.
  ActionType RequestedAction = ActionType::NoneAction;

  enum class ParseInputMode {
    Swift,
    SwiftLibrary,
    SwiftModuleInterface,
    SIL,
  };
  ParseInputMode InputMode = ParseInputMode::Swift;

  /// Indicates that the input(s) should be parsed as the Swift stdlib.
  bool ParseStdlib = false;

  /// Ignore .swiftsourceinfo file when trying to get source locations from module imported decls.
  bool IgnoreSwiftSourceInfo = false;

  /// When true, emitted module files will always contain options for the
  /// debugger to use. When unset, the options will only be present if the
  /// module appears to not be a public module.
  Optional<bool> SerializeOptionsForDebugging;

  /// When true, check if all required SwiftOnoneSupport symbols are present in
  /// the module.
  bool CheckOnoneSupportCompleteness = false;

  /// If set, dumps wall time taken to check each function body to llvm::errs().
  bool DebugTimeFunctionBodies = false;

  /// If set, dumps wall time taken to check each expression.
  bool DebugTimeExpressionTypeChecking = false;

  /// The path to which we should output statistics files.
  std::string StatsOutputDir;

  /// Trace changes to stats to files in StatsOutputDir.
  bool TraceStats = false;

  /// Profile changes to stats to files in StatsOutputDir.
  bool ProfileEvents = false;

  /// Profile changes to stats to files in StatsOutputDir, grouped by source
  /// entity.
  bool ProfileEntities = false;

  /// Emit parseable-output directly from the frontend, instead of relying
  /// the driver to emit it. This is used in context where frontend jobs are executed by
  /// clients other than the driver.
  bool FrontendParseableOutput = false;

  /// Indicates whether or not an import statement can pick up a Swift source
  /// file (as opposed to a module file).
  bool EnableSourceImport = false;

  /// Indicates whether we are compiling for testing.
  ///
  /// \see ModuleDecl::isTestingEnabled
  bool EnableTesting = false;

  /// Indicates whether we are compiling for private imports.
  ///
  /// \see ModuleDecl::arePrivateImportsEnabled
  bool EnablePrivateImports = false;


  /// Indicates whether we add implicit dynamic.
  ///
  /// \see ModuleDecl::isImplicitDynamicEnabled
  bool EnableImplicitDynamic = false;

  /// Enables the "fully resilient" resilience strategy.
  ///
  /// \see ResilienceStrategy::Resilient
  bool EnableLibraryEvolution = false;

  /// If set, this module is part of a mixed Objective-C/Swift framework, and
  /// the Objective-C half should implicitly be visible to the Swift sources.
  bool ImportUnderlyingModule = false;

  /// If set, the header provided in ImplicitObjCHeaderPath will be rewritten
  /// by the Clang importer as part of semantic analysis.
  bool SerializeBridgingHeader = false;

  /// Indicates whether or not the frontend should print statistics upon
  /// termination.
  bool PrintStats = false;

  /// Indicates whether or not the Clang importer should print statistics upon
  /// termination.
  bool PrintClangStats = false;

  /// Indicates whether standard help should be shown.
  bool PrintHelp = false;

  /// Indicates whether full help (including "hidden" options) should be shown.
  bool PrintHelpHidden = false;

  /// Indicates that the frontend should print the target triple and then
  /// exit.
  bool PrintTargetInfo = false;

  /// See the \ref SILOptions.EmitVerboseSIL flag.
  bool EmitVerboseSIL = false;

  /// See the \ref SILOptions.EmitSortedSIL flag.
  bool EmitSortedSIL = false;

  /// Specifies the collection mode for the intermodule dependency tracker.
  /// Note that if set, the dependency tracker will be enabled even if no
  /// output path is configured.
  Optional<IntermoduleDepTrackingMode> IntermoduleDependencyTracking;

  /// Should we emit the cType when printing @convention(c) or no?
  bool PrintFullConvention = false;

  /// Should we serialize the hashes of dependencies (vs. the modification
  /// times) when compiling a module interface?
  bool SerializeModuleInterfaceDependencyHashes = false;

  /// Should we warn if an imported module needed to be rebuilt from a
  /// module interface file?
  bool RemarkOnRebuildFromModuleInterface = false;

  /// Should we lock .swiftinterface while generating .swiftmodule from it?
  bool DisableInterfaceFileLock = false;

  /// Should we enable the dependency verifier for all primary files known to this frontend?
  bool EnableIncrementalDependencyVerifier = false;

  /// The path of the swift-frontend executable.
  std::string MainExecutablePath;

  /// The directory path we should use when print #include for the bridging header.
  /// By default, we include ImplicitObjCHeaderPath directly.
  llvm::Optional<std::string> BridgingHeaderDirForPrint;

  /// Disable implicitly built Swift modules because they are explicitly
  /// built and given to the compiler invocation.
  bool DisableImplicitModules = false;

  /// Disable building Swift modules from textual interfaces. This should be
  /// for testing purposes only.
  bool DisableBuildingInterface = false;

  /// When performing a dependency scanning action, only identify and output all imports
  /// of the main Swift module's source files.
  bool ImportPrescan = false;

  /// When performing an incremental build, ensure that cross-module incremental
  /// build metadata is available in any swift modules emitted by this frontend
  /// job.
  ///
  /// This flag is currently only propagated from the driver to
  /// any merge-modules jobs.
  bool DisableCrossModuleIncrementalBuild = false;

  /// Best effort to output a .swiftmodule regardless of any compilation
  /// errors. SIL generation and serialization is skipped entirely when there
  /// are errors. The resulting serialized AST may include errors types and
  /// skip nodes entirely, depending on the errors involved.
  bool AllowModuleWithCompilerErrors = false;

  /// True if the "-static" option is set.
  bool Static = false;

  /// The different modes for validating TBD against the LLVM IR.
  enum class TBDValidationMode {
    Default,        ///< Do the default validation for the current platform.
    None,           ///< Do no validation.
    MissingFromTBD, ///< Only check for symbols that are in IR but not TBD.
    All, ///< Check for symbols that are in IR but not TBD and TBD but not IR.
  };

  /// Compare the symbols in the IR against the TBD file we would generate.
  TBDValidationMode ValidateTBDAgainstIR = TBDValidationMode::Default;

  /// An enum with different modes for automatically crashing at defined times.
  enum class DebugCrashMode {
    None, ///< Don't automatically crash.
    AssertAfterParse, ///< Automatically assert after parsing.
    CrashAfterParse, ///< Automatically crash after parsing.
  };

  /// Indicates a debug crash mode for the frontend.
  DebugCrashMode CrashMode = DebugCrashMode::None;

  /// Line and column for each of the locations to be probed by
  /// -dump-scope-maps.
  SmallVector<std::pair<unsigned, unsigned>, 2> DumpScopeMapLocations;

  /// Determines whether the static or shared resource folder is used.
  /// When set to `true`, the default resource folder will be set to
  /// '.../lib/swift', otherwise '.../lib/swift_static'.
  bool UseSharedResourceFolder = true;

  /// \return true if the given action only parses without doing other compilation steps.
  static bool shouldActionOnlyParse(ActionType);

  /// \return true if the given action requires the standard library to be
  /// loaded before it is run.
  static bool doesActionRequireSwiftStandardLibrary(ActionType);

  /// \return true if the given action requires input files to be provided.
  static bool doesActionRequireInputs(ActionType action);

  /// \return true if the given action requires input files to be provided.
  static bool doesActionPerformEndOfPipelineActions(ActionType action);

  /// Return a hash code of any components from these options that should
  /// contribute to a Swift Bridging PCH hash.
  llvm::hash_code getPCHHashComponents() const {
    return llvm::hash_value(0);
  }

  StringRef determineFallbackModuleName() const;

  bool isCompilingExactlyOneSwiftFile() const {
    return InputsAndOutputs.hasSingleInput() &&
           InputMode == ParseInputMode::Swift;
  }

  const PrimarySpecificPaths &
  getPrimarySpecificPathsForAtMostOnePrimary() const;
  const PrimarySpecificPaths &
      getPrimarySpecificPathsForPrimary(StringRef) const;

  /// Retrieves the list of arbitrary modules to import and make implicitly
  /// visible.
  ArrayRef<std::pair<std::string, bool /*testable*/>>
  getImplicitImportModuleNames() const {
    return ImplicitImportModuleNames;
  }

  /// Whether we're configured to track system intermodule dependencies.
  bool shouldTrackSystemDependencies() const;
  
  /// Whether to emit symbol graphs for the output module.
  bool EmitSymbolGraph = false;

  /// The directory to which we should emit a symbol graph JSON files.
  /// It is valid whenever there are any inputs.
  ///
  /// These are JSON file that describes the public interface of a module for
  /// curating documentation, separated into files for each module this module
  /// extends.
  ///
  /// \sa SymbolGraphASTWalker
  std::string SymbolGraphOutputDir;
  
  /// Whether to emit doc comment information in symbol graphs for symbols
  /// which are inherited through classes or default implementations.
  bool SkipInheritedDocs = false;

private:
  static bool canActionEmitDependencies(ActionType);
  static bool canActionEmitReferenceDependencies(ActionType);
  static bool canActionEmitObjCHeader(ActionType);
  static bool canActionEmitLoadedModuleTrace(ActionType);
  static bool canActionEmitModule(ActionType);
  static bool canActionEmitModuleDoc(ActionType);
  static bool canActionEmitModuleSummary(ActionType);
  static bool canActionEmitInterface(ActionType);

public:
  static bool doesActionGenerateSIL(ActionType);
  static bool doesActionGenerateIR(ActionType);
  static bool doesActionProduceOutput(ActionType);
  static bool doesActionProduceTextualOutput(ActionType);
  static bool needsProperModuleName(ActionType);
  static file_types::ID formatForPrincipalOutputFileForAction(ActionType);
};

}

#endif
