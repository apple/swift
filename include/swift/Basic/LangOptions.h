//===--- LangOptions.h - Language & configuration options -------*- C++ -*-===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2020 Apple Inc. and the Swift project authors
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

#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/Basic/Feature.h"
#include "swift/Basic/FixedBitSet.h"
#include "swift/Basic/FunctionBodySkipping.h"
#include "swift/Basic/LLVM.h"
#include "swift/Basic/PlaygroundOption.h"
#include "swift/Basic/Version.h"
#include "swift/Config.h"
#include "clang/CAS/CASOptions.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Option/ArgList.h"
#include "llvm/Support/Regex.h"
#include "llvm/Support/VersionTuple.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/TargetParser/Triple.h"
#include <atomic>
#include <optional>
#include <string>
#include <vector>

namespace swift {

  enum class DiagnosticBehavior : uint8_t;

  /// Kind of implicit platform conditions.
  enum class PlatformConditionKind {
#define PLATFORM_CONDITION(LABEL, IDENTIFIER) LABEL,
#include "swift/AST/PlatformConditionKinds.def"
  };

  /// Describes how strict concurrency checking should be.
  enum class StrictConcurrency {
    /// Enforce Sendable constraints where it has been explicitly adopted and
    /// perform actor-isolation checking wherever code has adopted concurrency.
    Minimal,
    /// Enforce Sendable constraints and perform actor-isolation checking
    /// wherever code has adopted concurrency, including code that has
    /// explicitly adopted Sendable.
    Targeted,
    /// Enforce Sendable constraints and actor-isolation checking throughout
    /// the entire module.
    Complete,
  };

  /// Access or distribution level of a library.
  enum class LibraryLevel : uint8_t {
    /// Application Programming Interface that is publicly distributed so
    /// public decls are really public and only @_spi decls are SPI.
    API,

    /// System Programming Interface that has restricted distribution
    /// all decls in the module are considered to be SPI including public ones.
    SPI,

    /// Internal Programming Interface that is not distributed and only usable
    /// from within a project.
    IPI,

    /// The library has some other undefined distribution.
    Other
  };

  enum class AccessNoteDiagnosticBehavior : uint8_t {
    Ignore,
    RemarkOnFailure,
    RemarkOnFailureOrSuccess,
    ErrorOnFailureRemarkOnSuccess
  };

  enum class ConcurrencyModel : uint8_t {
    Standard,
    TaskToThread,
  };

  /// Describes the code size optimization behavior for code associated with
  /// declarations that are marked unavailable.
  enum class UnavailableDeclOptimization : uint8_t {
    /// No optimization. Unavailable declarations will contribute to the
    /// resulting binary by default in this mode.
    None,

    /// Stub out code associated with unavailable declarations.
    ///
    /// For example, the bodies of unavailable functions should be compiled as
    /// if they just contained a call to fatalError().
    Stub,

    /// Avoid generating any code for unavailable declarations.
    ///
    /// NOTE: This optimization can be ABI breaking for a library evolution
    /// enabled module because existing client binaries built with a
    /// pre-Swift 5.9 compiler may depend on linkable symbols associated with
    /// unavailable declarations.
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
    std::optional<llvm::Triple> TargetVariant;

    /// The target triple to instantiate the internal clang instance.
    /// When not specified, the compiler will use the value of -target to
    /// instantiate the clang instance.
    /// This is mainly used to avoid lowering the target triple to use for clang when
    /// importing a .swiftinterface whose -target value may be different from
    /// the loading module.
    /// The lowering triple may result in multiple versions of the same Clang
    /// modules being built.
    std::optional<llvm::Triple> ClangTarget;

    /// The SDK version, if known.
    std::optional<llvm::VersionTuple> SDKVersion;

    /// The target variant SDK version, if known.
    std::optional<llvm::VersionTuple> VariantSDKVersion;

    /// The SDK canonical name, if known.
    std::string SDKName;

    /// The lowest target OS version that code in this module may be inlined
    /// into. In resilient modules, this should match the minimum
    /// deployment target of the *first* resilient version of the module, since
    /// clients may need to interoperate with versions as far back as that
    /// deployment target.
    llvm::VersionTuple MinimumInliningTargetVersion;

    /// The alternate name to use for the entry point instead of main.
    std::optional<std::string> entryPointFunctionName;

    ///
    /// Language features
    ///

    /// User-overridable language version to compile for.
    version::Version EffectiveLanguageVersion = version::Version{5, 10};

    /// Swift runtime version to compile for.
    version::Version RuntimeVersion = version::Version::getCurrentLanguageVersion();

    /// PackageDescription version to compile for.
    version::Version PackageDescriptionVersion;

    /// Enable experimental string processing
    bool EnableExperimentalStringProcessing = true;

    /// Disable API availability checking.
    bool DisableAvailabilityChecking = false;

    /// Enable optimization to bypass resilience checks in a package
    bool EnableBypassResilienceInPackage = false;

    /// Optimization mode for unavailable declarations.
    std::optional<UnavailableDeclOptimization> UnavailableDeclOptimizationMode;

    /// Causes the compiler to use weak linkage for symbols belonging to
    /// declarations introduced at the deployment target.
    bool WeakLinkAtTarget = false;

    /// Should the editor placeholder error be downgraded to a warning?
    bool WarnOnEditorPlaceholder = false;

    /// Maximum number of typo corrections we are allowed to perform.
    /// This is disabled by default until we can get typo-correction working within acceptable performance bounds.
    unsigned TypoCorrectionLimit = 0;
    
    /// Should access control be respected?
    bool EnableAccessControl = true;

    /// Enable loading a package interface if both client and depdency module are in the
    /// same package determined by `package-name` flag.
    bool EnablePackageInterfaceLoad = false;

    /// Enable 'availability' restrictions for App Extensions.
    bool EnableAppExtensionRestrictions = false;

    /// Enable 'availability' restrictions for App Extension Libraries.
    bool EnableAppExtensionLibraryRestrictions = false;

    /// Diagnostic level to report when a public declarations doesn't declare
    /// an introduction OS version.
    std::optional<DiagnosticBehavior> RequireExplicitAvailability =
        std::nullopt;

    /// Introduction platform and version to suggest as fix-it
    /// when using RequireExplicitAvailability.
    std::string RequireExplicitAvailabilityTarget;

    // Availability macros definitions to be expanded at parsing.
    SmallVector<std::string, 4> AvailabilityMacros;

    /// Require public declarations to declare that they are Sendable (or not).
    bool RequireExplicitSendable = false;

    /// Detect and automatically import modules' cross-import overlays.
    bool EnableCrossImportOverlays = false;

    /// Emit a remark when import resolution implicitly adds a cross-import
    /// overlay.
    bool EnableCrossImportRemarks = false;

    /// Emit a remark after loading a module.
    bool EnableModuleLoadingRemarks = false;

    /// Emit remarks about contextual inconsistencies in loaded modules.
    bool EnableModuleRecoveryRemarks = false;

    /// Emit remarks for unexpected conditions when serializing a module.
    bool EnableModuleSerializationRemarks = false;

    /// Emit remarks about the source of each element exposed by the module API.
    bool EnableModuleApiImportRemarks = false;

     /// Emit a remark after loading a macro implementation.
    bool EnableMacroLoadingRemarks = false;

    /// Emit a remark when indexing a system module.
    bool EnableIndexingSystemModuleRemarks = false;
    
    /// Emit a remark on early exit in explicit interface build
    bool EnableSkipExplicitInterfaceModuleBuildRemarks = false;

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

    /// Turn all throw sites into immediate traps.
    bool ThrowsAsTraps = false;

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

    /// Indicates the specific playground transformations to apply.
    PlaygroundOptionSet PlaygroundOptions;

    /// Keep comments during lexing and attach them to declarations.
    bool AttachCommentsToDecls = false;

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

    /// The C++ interoperability source compatibility version. Defaults
    /// to the Swift language version.
    version::Version cxxInteropCompatVersion;

    void setCxxInteropFromArgs(llvm::opt::ArgList &Args,
                               swift::DiagnosticEngine &Diags);

    bool CForeignReferenceTypes = false;

    /// Imports getters and setters as computed properties.
    bool CxxInteropGettersSettersAsProperties = false;

    /// Should the compiler require C++ interoperability to be enabled
    /// when importing Swift modules that enable C++ interoperability.
    bool RequireCxxInteropToImportCxxInteropModule = true;

    // Workaround for bug when building SwiftCompilerSources (rdar://128013193)
    bool CxxInteropUseOpaquePointerForMoveOnly = false;

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

    /// Whether to record request references for incremental builds.
    bool RecordRequestReferences = true;

    /// Whether to dump debug info for request evaluator cycles.
    bool DebugDumpCycles = false;

    /// Disable SIL substituted function types.
    bool DisableSubstSILFunctionTypes = false;

    /// Whether to diagnose an ephemeral to non-ephemeral conversion as an
    /// error.
    bool DiagnoseInvalidEphemeralnessAsError = false;

    /// The maximum depth to which to test decl circularity.
    unsigned MaxCircularityDepth = 500;

    /// Perform all dynamic allocations using malloc/free instead of
    /// optimized custom allocator, so that memory debugging tools can be used.
    bool UseMalloc = false;

    /// Specifies how strict concurrency checking will be.
    StrictConcurrency StrictConcurrencyLevel = StrictConcurrency::Minimal;

    /// Enable experimental concurrency model.
    bool EnableExperimentalConcurrency = false;

    /// Disable experimental ClangImporter diagnostics.
    bool DisableExperimentalClangImporterDiagnostics = false;

    /// Enable experimental eager Clang module diagnostics.
    bool EnableExperimentalEagerClangModuleDiagnostics = false;

    /// Force ClangImporter's import-as-member extensions to load thier members
    /// immediately, bypassing their SwiftLookupTables. This emulates an
    /// implementation quirk of previous compilers.
    bool DisableNamedLazyImportAsMemberLoading = false;

    /// Enable inference of Sendable conformances for public types.
    bool EnableInferPublicSendable = false;

    /// Disable the implicit import of the _Concurrency module.
    bool DisableImplicitConcurrencyModuleImport =
        !SWIFT_IMPLICIT_CONCURRENCY_IMPORT;

    /// Disable the implicit import of the _StringProcessing module.
    bool DisableImplicitStringProcessingModuleImport = false;

    /// Disable the implicit import of the _Backtracing module.
    bool DisableImplicitBacktracingModuleImport =
        !SWIFT_IMPLICIT_BACKTRACING_IMPORT;

    /// Disable the implicit import of the Cxx module.
    bool DisableImplicitCxxModuleImport = false;

    // Whether to use checked continuations when making an async call from
    // Swift into ObjC. If false, will use unchecked continuations instead.
    bool UseCheckedAsyncObjCBridging = false;

    /// Should we check the target OSs of serialized modules to see that they're
    /// new enough?
    bool EnableTargetOSChecking = true;

    /// Whether to attempt to recover from missing cross-references,
    /// differences in APIs between language versions, and other
    /// errors when deserializing from a binary swiftmodule file.
    ///
    /// This feature should only be disabled for testing as regular builds
    /// rely heavily on it.
    bool EnableDeserializationRecovery = true;

    /// Enable early skipping deserialization of decls that are marked as
    /// unsafe to read.
    bool EnableDeserializationSafety =
      ::getenv("SWIFT_ENABLE_DESERIALIZATION_SAFETY");

    /// Attempt to recover for imported modules with broken modularization
    /// in an unsafe way. Currently applies only to xrefs where the target
    /// decl moved to a different module that is already loaded.
    bool ForceWorkaroundBrokenModules = false;

    /// Whether to enable the new operator decl and precedencegroup lookup
    /// behavior. This is a staging flag, and will be removed in the future.
    bool EnableNewOperatorLookup = false;

    /// Use Clang function types for computing canonical types.
    /// If this option is false, the clang function types will still be computed
    /// but will not be used for checking type equality.
    /// [TODO: Clang-type-plumbing] Turn on for feature rollout.
    bool UseClangFunctionTypes = false;

    /// If set to true, the diagnosis engine can assume the emitted diagnostics
    /// will be used in editor. This usually leads to more aggressive fixit.
    bool DiagnosticsEditorMode = false;

    /// Access or distribution level of the whole module being parsed.
    LibraryLevel LibraryLevel = LibraryLevel::Other;

    /// The name of the package this module belongs to.
    std::string PackageName;

    /// The path to the package manifest in which this target resides.
    std::string PackageManifestFile;

    /// The name of the JSON file containing information about modules
    /// that are visible within the package containing this source file.
    std::string PackageAvailableModulesFile;

    /// The name of this target within its package manifest. This target
    /// must be defined in the `PackageManifestFile`.
    std::string PackageTargetName;

    /// Allow importing a non-package interface from the same package.
    bool AllowNonPackageInterfaceImportFromSamePackage = false;

    /// Diagnose implicit 'override'.
    bool WarnImplicitOverrides = false;

    /// Diagnose uses of NSCoding with classes that have unstable mangled names.
    bool EnableNSKeyedArchiverDiagnostics = true;

    /// Regex for the passes that should report passed and missed optimizations.
    ///
    /// These are shared_ptrs so that this class remains copyable.
    std::shared_ptr<llvm::Regex> OptimizationRemarkPassedPattern;
    std::shared_ptr<llvm::Regex> OptimizationRemarkMissedPattern;

    /// How should we emit diagnostics about access notes?
    AccessNoteDiagnosticBehavior AccessNoteBehavior =
        AccessNoteDiagnosticBehavior::RemarkOnFailureOrSuccess;

    DiagnosticBehavior getAccessNoteFailureLimit() const;

    bool shouldRemarkOnAccessNoteSuccess() const {
      return AccessNoteBehavior >=
          AccessNoteDiagnosticBehavior::RemarkOnFailureOrSuccess;
    }

    /// Whether collect tokens during parsing for syntax coloring.
    bool CollectParsedToken = false;

    /// Whether to disable the evaluation of '#if' decls, such that the bodies
    /// of active clauses aren't hoisted into the enclosing scope.
    bool DisablePoundIfEvaluation = false;

    /// When using fine-grained dependencies, emit dot files for every swiftdeps
    /// file.
    bool EmitFineGrainedDependencySourcefileDotFiles = false;

    /// Enable verification when every SubstitutionMap is constructed.
    bool VerifyAllSubstitutionMaps = false;

    /// Load swiftmodule files in memory as volatile and avoid mmap.
    bool EnableVolatileModules = false;

    /// Enable experimental 'hermetic seal at link' feature. Turns on
    /// dead-stripping optimizations assuming that all users of library code
    /// are present at LTO time.
    bool HermeticSealAtLink = false;

    /// Allow deserializing implementation only dependencies. This should only
    /// be set true by lldb and other tooling, so that deserialization
    /// recovery issues won't bring down the debugger.
    /// TODO: remove this when @_implementationOnly modules are robust enough.
    bool AllowDeserializingImplementationOnly = false;

    // Allow errors during module generation. See corresponding option in
    // FrontendOptions.
    bool AllowModuleWithCompilerErrors = false;

    /// Enable using @_spiOnly on import decls.
    bool EnableSPIOnlyImports = false;

    /// A helper enum to represent whether or not we customized the default
    /// ASTVerifier behavior via a frontend flag. By default, we do not
    /// customize.
    ///
    /// NOTE: The default behavior is to run the ASTVerifier only when asserts
    /// are enabled. This just allows for one to customize that behavior.
    enum class ASTVerifierOverrideKind {
      NoOverride = 0,
      EnableVerifier = 1,
      DisableVerifier = 2,
    };
    ASTVerifierOverrideKind ASTVerifierOverride =
        ASTVerifierOverrideKind::NoOverride;

    /// Enables dumping rewrite systems from the requirement machine.
    bool DumpRequirementMachine = false;

    /// Enables statistics output from the requirement machine.
    bool AnalyzeRequirementMachine = false;

    /// Enables fine-grained debug output from the requirement machine.
    std::string DebugRequirementMachine;

    /// Maximum rule count for requirement machine Knuth-Bendix completion
    /// algorithm.
    unsigned RequirementMachineMaxRuleCount = 4000;

    /// Maximum term length for requirement machine Knuth-Bendix completion
    /// algorithm.
    unsigned RequirementMachineMaxRuleLength = 12;

    /// Maximum concrete type nesting depth for requirement machine property map
    /// algorithm.
    unsigned RequirementMachineMaxConcreteNesting = 30;

    /// Maximum number of attempts to make when splitting concrete equivalence
    /// classes.
    unsigned RequirementMachineMaxSplitConcreteEquivClassAttempts = 2;

    /// Enable preprocessing pass to eliminate conformance requirements
    /// on generic parameters which are made concrete. Usually you want this
    /// enabled. It can be disabled for debugging and testing.
    bool EnableRequirementMachineConcreteContraction = true;

    /// Enable the stronger minimization algorithm. Usually you want this
    /// enabled. It can be disabled for debugging and testing.
    bool EnableRequirementMachineLoopNormalization = true;

    /// Enable reuse of requirement machines for minimization. Usually you want
    /// this enabled. It can be disabled for debugging and testing.
    bool EnableRequirementMachineReuse = true;

    /// Enable experimental, more correct support for opaque result types as
    /// concrete types. This will sometimes fail to produce a convergent
    /// rewrite system.
    bool EnableRequirementMachineOpaqueArchetypes = false;

    /// Enable implicit lifetime dependence for ~Escapable return types.
    bool EnableExperimentalLifetimeDependenceInference = true;

    /// Skips decls that cannot be referenced externally.
    bool SkipNonExportableDecls = false;

    /// True if -experimental-allow-non-resilient-access is passed and built
    /// from source.
    bool AllowNonResilientAccess = false;

    /// Enables dumping type witness systems from associated type inference.
    bool DumpTypeWitnessSystems = false;

    /// Enables dumping macro expansions.
    bool DumpMacroExpansions = false;

    /// The model of concurrency to be used.
    ConcurrencyModel ActiveConcurrencyModel = ConcurrencyModel::Standard;

    /// All block list configuration files to be honored in this compilation.
    std::vector<std::string> BlocklistConfigFilePaths;

    /// Whether to ignore checks that a module is resilient during
    /// type-checking, SIL verification, and IR emission,
    bool BypassResilienceChecks = false;

    /// Disables `DynamicActorIsolation` feature.
    bool DisableDynamicActorIsolation = false;

    /// Whether or not to allow experimental features that are only available
    /// in "production".
#ifdef NDEBUG
    bool RestrictNonProductionExperimentalFeatures = true;
#else
    bool RestrictNonProductionExperimentalFeatures = false;
#endif

    bool isConcurrencyModelTaskToThread() const {
      return ActiveConcurrencyModel == ConcurrencyModel::TaskToThread;
    }

    bool isDynamicActorIsolationCheckingEnabled() const {
      return !DisableDynamicActorIsolation &&
             hasFeature(Feature::DynamicActorIsolation);
    }

    LangOptions();

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
      if (Target.isMacOSX()) {
        llvm::VersionTuple OSVersion;
        Target.getMacOSXVersion(OSVersion);
        return OSVersion;
      } else if (Target.isiOS()) {
        return Target.getiOSVersion();
      } else if (Target.isWatchOS()) {
        return Target.getOSVersion();
      } else if (Target.isXROS()) {
        return Target.getOSVersion();
      }
      return llvm::VersionTuple(/*Major=*/0, /*Minor=*/0, /*Subminor=*/0);
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

    /// Whether the C++ interoperability compatibility version is at least
    /// 'major'.
    bool isCxxInteropCompatVersionAtLeast(unsigned major,
                                          unsigned minor = 0) const {
      return cxxInteropCompatVersion.isVersionAtLeast(major, minor);
    }

    /// Determine whether the given feature is enabled.
    bool hasFeature(Feature feature) const;

    /// Determine whether the given feature is enabled, looking up the feature
    /// by name.
    bool hasFeature(llvm::StringRef featureName) const;

    /// Enable the given feature.
    void enableFeature(Feature feature) { Features.insert(feature); }

    /// Disable the given feature.
    void disableFeature(Feature feature) { Features.remove(feature); }

    /// Sets the "_hasAtomicBitWidth" conditional.
    void setHasAtomicBitWidth(llvm::Triple triple);

    /// Set the max atomic bit widths with the given bit width.
    void setMaxAtomicBitWidth(unsigned maxWidth) {
      switch (maxWidth) {
      case 128:
        AtomicBitWidths.emplace_back("_128");
        LLVM_FALLTHROUGH;
      case 64:
        AtomicBitWidths.emplace_back("_64");
        LLVM_FALLTHROUGH;
      case 32:
        AtomicBitWidths.emplace_back("_32");
        LLVM_FALLTHROUGH;
      case 16:
        AtomicBitWidths.emplace_back("_16");
        LLVM_FALLTHROUGH;
      case 8:
        AtomicBitWidths.emplace_back("_8");
        break;
      default:
        return;
      }
    }

    /// Removes all atomic bit widths.
    void clearAtomicBitWidths() {
      AtomicBitWidths.clear();
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

    /// Return a hash code of any components from these options that should
    /// contribute to a Swift Dependency Scanning hash.
    llvm::hash_code getModuleScanningHashComponents() const {
      auto hashValue = getPCHHashComponents();
      if (TargetVariant.has_value())
        hashValue = llvm::hash_combine(hashValue, TargetVariant.value().str());
      if (ClangTarget.has_value())
        hashValue = llvm::hash_combine(hashValue, ClangTarget.value().str());
      if (SDKVersion.has_value())
        hashValue = llvm::hash_combine(hashValue, SDKVersion.value().getAsString());
      if (VariantSDKVersion.has_value())
        hashValue = llvm::hash_combine(hashValue, VariantSDKVersion.value().getAsString());
      return hashValue;
    }

  private:
    llvm::SmallVector<std::string, 2> AtomicBitWidths;
    llvm::SmallVector<std::pair<PlatformConditionKind, std::string>, 10>
        PlatformConditionValues;
    llvm::SmallVector<std::string, 2> CustomConditionalCompilationFlags;

    /// The set of features that have been enabled. Doesn't include upcoming
    /// features, which are checked against the language version in
    /// `hasFeature`.
    FixedBitSet<numFeatures(), Feature> Features;
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

    /// Controls the function bodies to skip during type-checking.
    FunctionBodySkipping SkipFunctionBodies = FunctionBodySkipping::None;

    ///
    /// Flags for developers
    ///

    /// Debug the generic signatures computed by the generic signature builder.
    bool DebugGenericSignatures = false;

    /// If this is set, we skip the inverse transform and print explicit
    /// Copyable/Escapable requirements in the above.
    bool DebugInverseRequirements = false;

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

    /// Triggers llvm fatal error if the typechecker tries to typecheck a decl
    /// or an identifier reference with any of the provided prefix names. This
    /// is for testing purposes.
    std::vector<std::string> DebugForbidTypecheckPrefixes;

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

    /// See \ref FrontendOptions.PrintFullConvention
    bool PrintFullConvention = false;

    /// Defer typechecking of declarations to their use at runtime
    bool DeferToRuntime = false;

    /// Allow request evalutation to perform type checking lazily, instead of
    /// eagerly typechecking source files after parsing.
    bool EnableLazyTypecheck = false;
  };

  /// Options for controlling the behavior of the Clang importer.
  class ClangImporterOptions final {
  public:
    /// The path to the Clang compiler executable.
    /// Used to detect the default include paths.
    std::string clangPath = "clang";

    /// The module cache path which the Clang importer should use.
    std::string ModuleCachePath;

    /// The Scanning module cache path which the Clang Dependency Scanner should use.
    std::string ClangScannerModuleCachePath;

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

    /// The optimization setting.  This doesn't typically matter for
    /// import, but it can affect Clang's IR generation of static functions.
    std::string Optimization;

    /// Disable validating the persistent PCH.
    bool PCHDisableValidation = false;

    /// Don't verify input files for Clang modules if the module has been
    /// successfully validated or loaded during this build session.
    bool ValidateModulesOnce = false;

    /// Use the last modification time of this file as the underlying Clang
    /// build session timestamp.
    std::string BuildSessionFilePath;

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

    /// If true ignore the swift bridged attribute.
    bool DisableSwiftBridgeAttr = false;

    /// When set, don't look for or load overlays.
    bool DisableOverlayModules = false;

    /// When set, import SPI_AVAILABLE symbols with Swift SPI attributes.
    bool EnableClangSPI = true;

    /// When set, don't enforce warnings with -Werror.
    bool DebuggerSupport = false;

    /// When set, ClangImporter is disabled, and all requests go to the
    /// DWARFImporter delegate.
    bool DisableSourceImport = false;

    /// When building a PCM, rely on the Swift frontend's command-line -Xcc flags
    /// to build the Clang module via Clang frontend directly,
    /// and completely bypass the Clang driver.
    bool DirectClangCC1ModuleBuild = false;

    /// Disable implicitly-built Clang modules because they are explicitly
    /// built and provided to the compiler invocation.
    bool DisableImplicitClangModules = false;

    /// Enable ClangIncludeTree for explicit module builds scanning.
    bool UseClangIncludeTree = false;

    /// Using ClangIncludeTreeRoot for compilation.
    bool HasClangIncludeTreeRoot = false;

    /// Whether the dependency scanner should construct all swift-frontend
    /// invocations directly from clang cc1 args.
    bool ClangImporterDirectCC1Scan = false;

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
                          DisableSwiftBridgeAttr,
                          DisableOverlayModules,
                          EnableClangSPI);
    }

    /// Return a hash code of any components from these options that should
    /// contribute to a Swift Dependency Scanning hash.
    llvm::hash_code getModuleScanningHashComponents() const {
      return getPCHHashComponents();
    }

    std::vector<std::string> getRemappedExtraArgs(
        std::function<std::string(StringRef)> pathRemapCallback) const;

    /// For a swift module dependency, interface build command generation must
    /// inherit
    /// `-Xcc` flags used for configuration of the building instance's
    /// `ClangImporter`. However, we can ignore Clang search path flags because
    /// explicit Swift module build tasks will not rely on them and they may be
    /// source-target-context-specific and hinder module sharing across
    /// compilation source targets.
    std::vector<std::string>
    getReducedExtraArgsForSwiftModuleDependency() const;
  };

} // end namespace swift

#endif // SWIFT_BASIC_LANGOPTIONS_H
