//===--- LangOptions.h - Language & configuration options -------*- C++ -*-===//
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
//
//  This file defines the LangOptions class, which provides various
//  language and configuration flags.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_BASIC_LANGOPTIONS_H
#define SWIFT_BASIC_LANGOPTIONS_H

#include "swift/Config.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/Version.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/VersionTuple.h"
#include <string>
#include <vector>

namespace swift {

  /// Kind of implicit platform conditions.
  enum class PlatformConditionKind {
#define PLATFORM_CONDITION(LABEL, IDENTIFIER) LABEL,
#include "swift/AST/PlatformConditionKinds.def"
  };

  /// Describes which Swift 3 Objective-C inference warnings should be
  /// emitted.
  enum class Swift3ObjCInferenceWarnings {
    /// No warnings; this is the default.
    None,
    /// "Minimal" warnings driven by uses of declarations that make use of
    /// the Objective-C entry point directly.
    Minimal,
    /// "Complete" warnings that add "@objc" for every entry point that
    /// Swift 3 would have inferred as "@objc" but Swift 4 will not.
    Complete,
  };

  /// A collection of options that affect the language dialect and
  /// provide compiler debugging facilities.
  class LangOptions final {
  public:

    /// The target we are building for.
    ///
    /// This represents the minimum deployment target.
    llvm::Triple Target;

    /// \brief The second target for a zippered build
    ///
    /// This represents the target and minimum deployment version for the
    /// second ('variant') target when performing a zippered build.
    /// For example, if the target is x86_64-apple-macosx10.14 then
    /// a target-variant of x86_64-apple-ios12.0-macabi will produce
    /// a zippered binary that can be loaded into both macCatalyst and
    /// macOS processes. A value of 'None' means no zippering will be
    /// performed.
    llvm::Optional<llvm::Triple> TargetVariant;

    /// The SDK version, if known.
    Optional<llvm::VersionTuple> SDKVersion;

    /// The target variant SDK version, if known.
    Optional<llvm::VersionTuple> VariantSDKVersion;

    ///
    /// Language features
    ///

    /// User-overridable language version to compile for.
    version::Version EffectiveLanguageVersion = version::Version::getCurrentLanguageVersion();

    /// PackageDescription version to compile for.
    version::Version PackageDescriptionVersion;

    /// Disable API availability checking.
    bool DisableAvailabilityChecking = false;

    /// Maximum number of typo corrections we are allowed to perform.
    /// This is disabled by default until we can get typo-correction working within acceptable performance bounds.
    unsigned TypoCorrectionLimit = 0;
    
    /// Should access control be respected?
    bool EnableAccessControl = true;

    /// Enable 'availability' restrictions for App Extensions.
    bool EnableAppExtensionRestrictions = false;

    /// Require public declarations to declare an introduction OS version.
    bool RequireExplicitAvailability = false;

    /// Introduction platform and version to suggest as fix-it
    /// when using RequireExplicitAvailability.
    std::string RequireExplicitAvailabilityTarget;

    // Availability macros definitions to be expanded at parsing.
    SmallVector<StringRef, 4> AvailabilityMacros;

    /// If false, '#file' evaluates to the full path rather than a
    /// human-readable string.
    bool EnableConcisePoundFile = false;

    /// Detect and automatically import modules' cross-import overlays.
    bool EnableCrossImportOverlays = false;

    /// Emit a remark when import resolution implicitly adds a cross-import
    /// overlay.
    bool EnableCrossImportRemarks = false;

    ///
    /// Support for alternate usage modes
    ///

    /// Enable features useful for running in the debugger.
    bool DebuggerSupport = false;

    /// Enable the MemoryBufferSerializedModuleImporter.
    /// Only used by lldb-moduleimport-test.
    bool EnableMemoryBufferImporter = false;

    /// Allows using identifiers with a leading dollar.
    bool EnableDollarIdentifiers = false;

    /// Allow throwing call expressions without annotation with 'try'.
    bool EnableThrowWithoutTry = false;

    /// If set, inserts instrumentation useful for testing the debugger.
    bool DebuggerTestingTransform = false;

    /// Indicates whether the AST should be instrumented to simulate a
    /// debugger's program counter. Similar to the PlaygroundTransform, this
    /// will instrument the AST with function calls that get called when you
    /// would see a program counter move in a debugger. To adopt this implement
    /// the __builtin_pc_before and __builtin_pc_after functions.
    bool PCMacro = false;

    /// Enable features useful for running playgrounds.
    // FIXME: This should probably be limited to the particular SourceFile.
    bool Playground = false;

    /// Indicates whether the playground transformation should be applied.
    bool PlaygroundTransform = false;

    /// Indicates whether the playground transformation should omit
    /// instrumentation that has a high runtime performance impact.
    bool PlaygroundHighPerformance = false;

    /// Keep comments during lexing and attach them to declarations.
    bool AttachCommentsToDecls = false;

    /// Whether to include initializers when code-completing a postfix
    /// expression.
    bool CodeCompleteInitsInPostfixExpr = false;

    /// Whether to use heuristics to decide whether to show call-pattern
    /// completions.
    bool CodeCompleteCallPatternHeuristics = false;

    ///
    /// Flags for use by tests
    ///

    /// Enable Objective-C Runtime interop code generation and build
    /// configuration options.
    bool EnableObjCInterop = true;

    /// Enable C++ interop code generation and build configuration
    /// options. Disabled by default because there is no way to control the
    /// language mode of clang on a per-header or even per-module basis. Also
    /// disabled because it is not complete.
    bool EnableCXXInterop = false;

    /// On Darwin platforms, use the pre-stable ABI's mark bit for Swift
    /// classes instead of the stable ABI's bit. This is needed when
    /// targeting OSes prior to macOS 10.14.4 and iOS 12.2, where
    /// libobjc does not support the stable ABI's marker bit.
    bool UseDarwinPreStableABIBit = false;

    /// Enables checking that uses of @objc require importing
    /// the Foundation module.
    /// This is enabled by default because SILGen can crash in such a case, but
    /// it gets disabled when compiling the Swift core stdlib.
    bool EnableObjCAttrRequiresFoundation = true;

    /// If true, <code>@testable import Foo</code> produces an error if \c Foo
    /// was not compiled with -enable-testing.
    bool EnableTestableAttrRequiresTestableModule = true;

    ///
    /// Flags for developers
    ///

    /// Enable named lazy member loading.
    bool NamedLazyMemberLoading = true;
    
    /// The path to which we should emit GraphViz output for the complete
    /// request-evaluator graph.
    std::string RequestEvaluatorGraphVizPath;
    
    /// Whether to dump debug info for request evaluator cycles.
    bool DebugDumpCycles = false;

    /// Whether to build a request dependency graph for debugging.
    bool BuildRequestDependencyGraph = false;

    /// Enable SIL type lowering
    bool EnableSubstSILFunctionTypesForFunctionValues = true;

    /// Whether to diagnose an ephemeral to non-ephemeral conversion as an
    /// error.
    bool DiagnoseInvalidEphemeralnessAsError = false;

    /// The maximum depth to which to test decl circularity.
    unsigned MaxCircularityDepth = 500;

    /// Perform all dynamic allocations using malloc/free instead of
    /// optimized custom allocator, so that memory debugging tools can be used.
    bool UseMalloc = false;

    /// Enable experimental #assert feature.
    bool EnableExperimentalStaticAssert = false;

    /// Enable experimental concurrency model.
    bool EnableExperimentalConcurrency = false;

    /// Should we check the target OSs of serialized modules to see that they're
    /// new enough?
    bool EnableTargetOSChecking = true;

    /// Whether to attempt to recover from missing cross-references and other
    /// errors when deserializing from a Swift module.
    ///
    /// This is a staging flag; eventually it will be removed.
    bool EnableDeserializationRecovery = true;

    /// Someday, ASTScopeLookup will supplant lookup in the parser
    bool DisableParserLookup = false;

    /// Whether to enable the new operator decl and precedencegroup lookup
    /// behavior. This is a staging flag, and will be removed in the future.
    bool EnableNewOperatorLookup = false;

    /// Whether to enable the "fuzzy" forward-scanning behavior for trailing
    /// closure matching, which skips over defaulted closure parameters
    /// to match later (non-defaulted) closure parameters
    ///
    /// This is a backward-compatibility hack for unlabeled trailing closures,
    /// to be disabled in Swift 6+.
    bool EnableFuzzyForwardScanTrailingClosureMatching = true;

    /// Use Clang function types for computing canonical types.
    /// If this option is false, the clang function types will still be computed
    /// but will not be used for checking type equality.
    /// [TODO: Clang-type-plumbing] Turn on for feature rollout.
    bool UseClangFunctionTypes = false;

    /// Whether to use the import as member inference system
    ///
    /// When importing a global, try to infer whether we can import it as a
    /// member of some type instead. This includes inits, computed properties,
    /// and methods.
    bool InferImportAsMember = false;

    /// If set to true, compile with the SIL Opaque Values enabled.
    /// This is for bootstrapping. It can't be in SILOptions because the
    /// TypeChecker uses it to set resolve the ParameterConvention.
    bool EnableSILOpaqueValues = false;

    /// If set to true, the diagnosis engine can assume the emitted diagnostics
    /// will be used in editor. This usually leads to more aggressive fixit.
    bool DiagnosticsEditorMode = false;

    /// Whether to enable Swift 3 @objc inference, e.g., for members of
    /// Objective-C-derived classes and 'dynamic' members.
    bool EnableSwift3ObjCInference = false;

    /// Warn about cases where Swift 3 would infer @objc but later versions
    /// of Swift do not.
    Swift3ObjCInferenceWarnings WarnSwift3ObjCInference =
      Swift3ObjCInferenceWarnings::None;

    /// Diagnose implicit 'override'.
    bool WarnImplicitOverrides = false;

    /// Diagnose uses of NSCoding with classes that have unstable mangled names.
    bool EnableNSKeyedArchiverDiagnostics = true;

    /// Diagnose switches over non-frozen enums that do not have catch-all
    /// cases.
    bool EnableNonFrozenEnumExhaustivityDiagnostics = false;

    /// Regex for the passes that should report passed and missed optimizations.
    ///
    /// These are shared_ptrs so that this class remains copyable.
    std::shared_ptr<llvm::Regex> OptimizationRemarkPassedPattern;
    std::shared_ptr<llvm::Regex> OptimizationRemarkMissedPattern;

    /// Whether collect tokens during parsing for syntax coloring.
    bool CollectParsedToken = false;

    /// Whether to parse syntax tree. If the syntax tree is built, the generated
    /// AST may not be correct when syntax nodes are reused as part of
    /// incrementals parsing.
    bool BuildSyntaxTree = false;

    /// Whether parsing is occurring for creation of syntax tree only, and no typechecking will occur after
    /// parsing e.g. when parsing for SwiftSyntax. This is intended to affect parsing, e.g. disable
    /// unnecessary name lookups that are not useful for pure syntactic parsing.
    bool ParseForSyntaxTreeOnly = false;

    /// Whether to verify the parsed syntax tree and emit related diagnostics.
    bool VerifySyntaxTree = false;

    /// Whether to disable the evaluation of '#if' decls, such that the bodies
    /// of active clauses aren't hoisted into the enclosing scope.
    bool DisablePoundIfEvaluation = false;

    /// When using fine-grained dependencies, emit dot files for every swiftdeps
    /// file.
    bool EmitFineGrainedDependencySourcefileDotFiles = false;

    /// Whether to enable experimental differentiable programming features:
    /// `@differentiable` declaration attribute, etc.
    bool EnableExperimentalDifferentiableProgramming = false;

    /// Whether to enable forward mode differentiation.
    bool EnableExperimentalForwardModeDifferentiation = false;

    /// Whether to enable experimental `AdditiveArithmetic` derived
    /// conformances.
    bool EnableExperimentalAdditiveArithmeticDerivedConformances = false;

    /// Enable verification when every SubstitutionMap is constructed.
    bool VerifyAllSubstitutionMaps = false;

    /// Load swiftmodule files in memory as volatile and avoid mmap.
    bool EnableVolatileModules = false;

    /// Allow deserializing implementation only dependencies. This should only
    /// be set true by lldb and other tooling, so that deserilization
    /// recovery issues won't bring down the debugger.
    /// TODO: remove this when @_implementationOnly modules are robust enough.
    bool AllowDeserializingImplementationOnly = false;
    
    /// Allows to diagnose @reflectable declarations
    /// when full reflection metadata is enabled.
    bool FullReflectionMetadataIsEnabled = false;
    
    /// Allows to diagnose @reflectable declarations
    /// when reflection metadata is disabled.
    bool ReflectionMetadataIsDisabled = false;

    /// Sets the target we are building for and updates platform conditions
    /// to match.
    ///
    /// \returns A pair - the first element is true if the OS was invalid.
    /// The second element is true if the Arch was invalid.
    std::pair<bool, bool> setTarget(llvm::Triple triple);

    /// Returns the minimum platform version to which code will be deployed.
    ///
    /// This is only implemented on certain OSs. If no target has been
    /// configured, returns v0.0.0.
    llvm::VersionTuple getMinPlatformVersion() const {
      unsigned major = 0, minor = 0, revision = 0;
      if (Target.isMacOSX()) {
        Target.getMacOSXVersion(major, minor, revision);
      } else if (Target.isiOS()) {
        Target.getiOSVersion(major, minor, revision);
      } else if (Target.isWatchOS()) {
        Target.getOSVersion(major, minor, revision);
      }
      return llvm::VersionTuple(major, minor, revision);
    }

    /// Sets an implicit platform condition.
    void addPlatformConditionValue(PlatformConditionKind Kind, StringRef Value) {
      assert(!Value.empty());
      PlatformConditionValues.emplace_back(Kind, Value.str());
    }

    /// Removes all values added with addPlatformConditionValue.
    void clearAllPlatformConditionValues() {
      PlatformConditionValues.clear();
    }
    
    /// Returns the value for the given platform condition or an empty string.
    StringRef getPlatformConditionValue(PlatformConditionKind Kind) const;

    /// Check whether the given platform condition matches the given value.
    bool checkPlatformCondition(PlatformConditionKind Kind, StringRef Value) const;

    /// Explicit conditional compilation flags, initialized via the '-D'
    /// compiler flag.
    void addCustomConditionalCompilationFlag(StringRef Name) {
      assert(!Name.empty());
      CustomConditionalCompilationFlags.push_back(Name.str());
    }

    /// Determines if a given conditional compilation flag has been set.
    bool isCustomConditionalCompilationFlagSet(StringRef Name) const;

    ArrayRef<std::pair<PlatformConditionKind, std::string>>
    getPlatformConditionValues() const {
      return PlatformConditionValues;
    }

    ArrayRef<std::string> getCustomConditionalCompilationFlags() const {
      return CustomConditionalCompilationFlags;
    }

    /// Whether our effective Swift version is at least 'major'.
    ///
    /// This is usually the check you want; for example, when introducing
    /// a new language feature which is only visible in Swift 5, you would
    /// check for isSwiftVersionAtLeast(5).
    bool isSwiftVersionAtLeast(unsigned major, unsigned minor = 0) const {
      return EffectiveLanguageVersion.isVersionAtLeast(major, minor);
    }

    /// Returns true if the given platform condition argument represents
    /// a supported target operating system.
    ///
    /// \param suggestedKind Populated with suggested replacement platform condition
    /// \param suggestedValues Populated with suggested replacement values
    /// if a match is not found, or if the value has been deprecated
    /// in favor of a newer one.
    static bool checkPlatformConditionSupported(
      PlatformConditionKind Kind, StringRef Value,
      PlatformConditionKind &suggestedKind,
      std::vector<StringRef> &suggestedValues);

    /// Return a hash code of any components from these options that should
    /// contribute to a Swift Bridging PCH hash.
    llvm::hash_code getPCHHashComponents() const {
      SmallString<16> Scratch;
      llvm::raw_svector_ostream OS(Scratch);
      OS << EffectiveLanguageVersion;
      return llvm::hash_combine(Target.str(), OS.str());
    }

  private:
    llvm::SmallVector<std::pair<PlatformConditionKind, std::string>, 6>
        PlatformConditionValues;
    llvm::SmallVector<std::string, 2> CustomConditionalCompilationFlags;
  };

  class TypeCheckerOptions final {
  public:
    /// If non-zero, warn when a function body takes longer than this many
    /// milliseconds to type-check.
    ///
    /// Intended for debugging purposes only.
    unsigned WarnLongFunctionBodies = 0;

    /// If non-zero, warn when type-checking an expression takes longer
    /// than this many milliseconds.
    ///
    /// Intended for debugging purposes only.
    unsigned WarnLongExpressionTypeChecking = 0;

    /// If non-zero, abort the expression type checker if it takes more
    /// than this many seconds.
    unsigned ExpressionTimeoutThreshold = 600;

    /// If non-zero, abort the switch statement exhaustiveness checker if
    /// the Space::minus function is called more than this many times.
    ///
    /// Why this number? Times out in about a second on a 2017 iMac, Retina 5K,
    /// 4.2 GHz Intel Core i7.
    /// (It's arbitrary, but will keep the compiler from taking too much time.)
    unsigned SwitchCheckingInvocationThreshold = 200000;
    
    /// If true, the time it takes to type-check each function will be dumped
    /// to llvm::errs().
    bool DebugTimeFunctionBodies = false;

    /// If true, the time it takes to type-check each expression will be
    /// dumped to llvm::errs().
    bool DebugTimeExpressions = false;

    /// Indicate that the type checker should skip type-checking non-inlinable
    /// function bodies.
    bool SkipNonInlinableFunctionBodies = false;

    ///
    /// Flags for developers
    ///

    /// Whether we are debugging the constraint solver.
    ///
    /// This option enables verbose debugging output from the constraint
    /// solver.
    bool DebugConstraintSolver = false;

    /// Specific solution attempt for which the constraint
    /// solver should be debugged.
    unsigned DebugConstraintSolverAttempt = 0;

    /// Line numbers to activate the constraint solver debugger.
    /// Should be stored sorted.
    llvm::SmallVector<unsigned, 4> DebugConstraintSolverOnLines;

    /// Debug the generic signatures computed by the generic signature builder.
    bool DebugGenericSignatures = false;

    /// Triggers llvm fatal_error if typechecker tries to typecheck a decl or an
    /// identifier reference with the provided prefix name.
    /// This is for testing purposes.
    std::string DebugForbidTypecheckPrefix;

    /// The upper bound, in bytes, of temporary data that can be
    /// allocated by the constraint solver.
    unsigned SolverMemoryThreshold = 512 * 1024 * 1024;

    unsigned SolverBindingThreshold = 1024 * 1024;

    /// The upper bound to number of sub-expressions unsolved
    /// before termination of the shrink phrase of the constraint solver.
    unsigned SolverShrinkUnsolvedThreshold = 10;

    /// Disable the shrink phase of the expression type checker.
    bool SolverDisableShrink = false;

    /// Enable experimental operator designated types feature.
    bool EnableOperatorDesignatedTypes = false;
    
    /// Disable constraint system performance hacks.
    bool DisableConstraintSolverPerformanceHacks = false;

    /// Enable experimental support for one-way constraints for the
    /// parameters of closures.
    bool EnableOneWayClosureParameters = false;
  };

  /// Options for controlling the behavior of the Clang importer.
  class ClangImporterOptions final {
  public:
    /// The module cache path which the Clang importer should use.
    std::string ModuleCachePath;

    /// Extra arguments which should be passed to the Clang importer.
    std::vector<std::string> ExtraArgs;

    /// A directory for overriding Clang's resource directory.
    std::string OverrideResourceDir;

    /// The target CPU to compile for.
    ///
    /// Equivalent to Clang's -mcpu=.
    std::string TargetCPU;

    /// The path to which we should store indexing data, if any.
    std::string IndexStorePath;

    /// The bridging header or PCH that will be imported.
    std::string BridgingHeader;

    /// When automatically generating a precompiled header from the bridging
    /// header, place it in this directory.
    std::string PrecompiledHeaderOutputDir;

    /// The optimizaton setting.  This doesn't typically matter for
    /// import, but it can affect Clang's IR generation of static functions.
    std::string Optimization;

    /// Disable validating the persistent PCH.
    bool PCHDisableValidation = false;

    /// \see Mode
    enum class Modes : uint8_t {
      /// Set up Clang for importing modules into Swift and generating IR from
      /// Swift code.
      Normal,
      /// Set up Clang for backend compilation only.
      EmbedBitcode,
      /// Set up Clang to emit a precompiled module from a C/Objective-C module
      /// map or dump debugging info about a precompiled module.
      PrecompiledModule
    };

    /// Controls how Clang is initially set up.
    Modes Mode = Modes::Normal;

    /// When set, preserves more information during import.
    ///
    /// Also \em disables some information that is only needed for object file
    /// generation.
    bool DetailedPreprocessingRecord = false;

    /// If true, Clang diagnostics will be dumped to stderr using Clang's
    /// diagnostic printer as well as being passed to Swift's diagnostic engine.
    bool DumpClangDiagnostics = false;

    /// If true, forward declarations will be imported using unavailable types
    /// instead of dropped altogether when possible.
    bool ImportForwardDeclarations = false;

    /// Whether to use the import as member inference system
    ///
    /// When importing a global, try to infer whether we can import it as a
    /// member of some type instead. This includes inits, computed properties,
    /// and methods.
    bool InferImportAsMember = false;

    /// If true ignore the swift bridged attribute.
    bool DisableSwiftBridgeAttr = false;

    /// When set, don't look for or load overlays.
    bool DisableOverlayModules = false;

    /// When set, don't enforce warnings with -Werror.
    bool DebuggerSupport = false;

    /// When set, ClangImporter is disabled, and all requests go to the
    /// DWARFImporter delegate.
    bool DisableSourceImport = false;

    /// When set, use ExtraArgs alone to configure clang instance because ExtraArgs
    /// contains the full option set.
    bool ExtraArgsOnly = false;

    /// Return a hash code of any components from these options that should
    /// contribute to a Swift Bridging PCH hash.
    llvm::hash_code getPCHHashComponents() const {
      using llvm::hash_combine;
      using llvm::hash_combine_range;

      return hash_combine(ModuleCachePath,
                          hash_combine_range(ExtraArgs.begin(), ExtraArgs.end()),
                          OverrideResourceDir,
                          TargetCPU,
                          BridgingHeader,
                          PrecompiledHeaderOutputDir,
                          static_cast<uint8_t>(Mode),
                          DetailedPreprocessingRecord,
                          ImportForwardDeclarations,
                          InferImportAsMember,
                          DisableSwiftBridgeAttr,
                          DisableOverlayModules);
    }
  };

} // end namespace swift

#endif // SWIFT_BASIC_LANGOPTIONS_H
