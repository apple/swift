//===--- IRGenOptions.h - Swift Language IR Generation Options --*- C++ -*-===//
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
//
// This file defines the options which control the generation of IR for
// swift files.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_IRGENOPTIONS_H
#define SWIFT_AST_IRGENOPTIONS_H

#include "swift/AST/LinkLibrary.h"
#include "swift/Basic/PathRemapper.h"
#include "swift/Basic/Sanitizers.h"
#include "swift/Basic/OptionSet.h"
#include "swift/Basic/OptimizationMode.h"
#include "swift/Config.h"
#include "clang/Basic/PointerAuthOptions.h"
#include "llvm/IR/CallingConv.h"
// FIXME: This include is just for llvm::SanitizerCoverageOptions. We should
// split the header upstream so we don't include so much.
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/VersionTuple.h"
#include <string>
#include <vector>

namespace swift {

enum class IRGenOutputKind : unsigned {
  /// Just generate an LLVM module and return it.
  Module,

  /// Generate an LLVM module and write it out as LLVM assembly.
  LLVMAssemblyBeforeOptimization,

  /// Generate an LLVM module and write it out as LLVM assembly.
  LLVMAssemblyAfterOptimization,

  /// Generate an LLVM module and write it out as LLVM bitcode.
  LLVMBitcode,

  /// Generate an LLVM module and compile it to assembly.
  NativeAssembly,

  /// Generate an LLVM module, compile it, and assemble into an object file.
  ObjectFile
};

enum class IRGenDebugInfoLevel : unsigned {
  None,       ///< No debug info.
  LineTables, ///< Line tables only.
  ASTTypes,   ///< Line tables + AST type references.
  DwarfTypes, ///< Line tables + AST type references + DWARF types.
  Normal = ASTTypes ///< The setting LLDB prefers.
};

enum class IRGenDebugInfoFormat : unsigned {
  None,
  DWARF,
  CodeView
};

enum class IRGenLLVMLTOKind : unsigned {
  None,
  Thin,
  Full,
};

enum class IRGenEmbedMode : unsigned {
  None,
  EmbedMarker,
  EmbedBitcode
};

enum class SwiftAsyncFramePointerKind : unsigned {
   Auto, // Choose Swift async extended frame info based on deployment target.
   Always, // Unconditionally emit Swift async extended frame info.
   Never,  // Don't emit Swift async extended frame info.
};

enum class ReflectionMetadataMode : unsigned {
  None,         ///< Don't emit reflection metadata at all.
  DebuggerOnly, ///< Emit reflection metadata for the debugger, don't link them
                ///  into runtime metadata and don't force them to stay alive.
  Runtime,      ///< Make reflection metadata fully available.
};

using clang::PointerAuthSchema;

struct PointerAuthOptions : clang::PointerAuthOptions {
  /// Native opaque function types, both thin and thick.
  /// Never address-sensitive.
  PointerAuthSchema SwiftFunctionPointers;

  /// Swift key path helpers.
  PointerAuthSchema KeyPaths;

  /// Swift value witness functions.
  PointerAuthSchema ValueWitnesses;

  /// Swift protocol witness functions.
  PointerAuthSchema ProtocolWitnesses;

  /// Swift protocol witness table associated type metadata access functions.
  PointerAuthSchema ProtocolAssociatedTypeAccessFunctions;

  /// Swift protocol witness table associated conformance witness table
  /// access functions.
  PointerAuthSchema ProtocolAssociatedTypeWitnessTableAccessFunctions;

  /// Swift class v-table functions.
  PointerAuthSchema SwiftClassMethods;

  /// Swift dynamic replacement implementations.
  PointerAuthSchema SwiftDynamicReplacements;
  PointerAuthSchema SwiftDynamicReplacementKeys;

  /// Swift class v-table functions not signed with an address. This is the
  /// return type of swift_lookUpClassMethod().
  PointerAuthSchema SwiftClassMethodPointers;

  /// Swift heap metadata destructors.
  PointerAuthSchema HeapDestructors;

  /// Non-constant function pointers captured in a partial-apply context.
  PointerAuthSchema PartialApplyCapture;

  /// Type descriptor data pointers.
  PointerAuthSchema TypeDescriptors;

  /// Type descriptor data pointers when passed as arguments.
  PointerAuthSchema TypeDescriptorsAsArguments;

  /// Protocol conformance descriptors.
  PointerAuthSchema ProtocolConformanceDescriptors;

  /// Protocol conformance descriptors when passed as arguments.
  PointerAuthSchema ProtocolConformanceDescriptorsAsArguments;

  /// Resumption functions from yield-once coroutines.
  PointerAuthSchema YieldOnceResumeFunctions;

  /// Resumption functions from yield-many coroutines.
  PointerAuthSchema YieldManyResumeFunctions;

  /// Resilient class stub initializer callbacks.
  PointerAuthSchema ResilientClassStubInitCallbacks;

  /// Like SwiftFunctionPointers but for use with AsyncFunctionPointer values.
  PointerAuthSchema AsyncSwiftFunctionPointers;

  /// Like SwiftClassMethods but for use with AsyncFunctionPointer values.
  PointerAuthSchema AsyncSwiftClassMethods;

  /// Like ProtocolWitnesses but for use with AsyncFunctionPointer values.
  PointerAuthSchema AsyncProtocolWitnesses;

  /// Like SwiftClassMethodPointers but for use with AsyncFunctionPointer
  /// values.
  PointerAuthSchema AsyncSwiftClassMethodPointers;

  /// Like SwiftDynamicReplacements but for use with AsyncFunctionPointer
  /// values.
  PointerAuthSchema AsyncSwiftDynamicReplacements;

  /// Like PartialApplyCapture but for use with AsyncFunctionPointer values.
  PointerAuthSchema AsyncPartialApplyCapture;

  /// The parent async context stored within a child async context.
  PointerAuthSchema AsyncContextParent;

  /// The function to call to resume running in the parent context.
  PointerAuthSchema AsyncContextResume;

  /// The resume function stored in AsyncTask.
  PointerAuthSchema TaskResumeFunction;

  /// The async context stored in AsyncTask.
  PointerAuthSchema TaskResumeContext;

  /// The swift async context entry in the extended frame info.
  PointerAuthSchema AsyncContextExtendedFrameEntry;

  /// Extended existential type shapes in flight.
  PointerAuthSchema ExtendedExistentialTypeShape;

  // The c type descriminator for TaskContinuationFunction*.
  PointerAuthSchema ClangTypeTaskContinuationFunction;

  /// Non-unique extended existential type shapes in flight.
  PointerAuthSchema NonUniqueExtendedExistentialTypeShape;

  /// C type GetExtraInhabitantTag function descriminator.
  PointerAuthSchema GetExtraInhabitantTagFunction;

  /// C type StoreExtraInhabitantTag function descriminator.
  PointerAuthSchema StoreExtraInhabitantTagFunction;

  /// Relative protocol witness table descriminator.
  PointerAuthSchema RelativeProtocolWitnessTable;

  /// Type layout string descriminator.
  PointerAuthSchema TypeLayoutString;
};

enum class JITDebugArtifact : unsigned {
  None,   ///< None
  LLVMIR, ///< LLVM IR
  Object, ///< Object File
};

/// The set of options supported by IR generation.
class IRGenOptions {
public:
  std::string ModuleName;

  /// The compilation directory for the debug info.
  std::string DebugCompilationDir;

  /// The DWARF version of debug info.
  unsigned DWARFVersion;

  /// The command line string that is to be stored in the debug info.
  std::string DebugFlags;

  /// List of -Xcc -D macro definitions.
  std::vector<std::string> ClangDefines;

  /// The libraries and frameworks specified on the command line.
  SmallVector<LinkLibrary, 4> LinkLibraries;

  /// The public dependent libraries specified on the command line.
  std::vector<std::string> PublicLinkLibraries;

  /// If non-empty, the (unmangled) name of a dummy symbol to emit that can be
  /// used to force-load this module.
  std::string ForceLoadSymbolName;

  /// The kind of compilation we should do.
  IRGenOutputKind OutputKind : 3;

  /// Should we spend time verifying that the IR we produce is
  /// well-formed?
  unsigned Verify : 1;

  OptimizationMode OptMode;

  /// Which sanitizer is turned on.
  OptionSet<SanitizerKind> Sanitizers;

  /// Which sanitizer(s) have recovery instrumentation enabled.
  OptionSet<SanitizerKind> SanitizersWithRecoveryInstrumentation;

  /// Whether to enable ODR indicators when building with ASan.
  unsigned SanitizeAddressUseODRIndicator : 1;

  /// Path prefixes that should be rewritten in debug info.
  PathRemapper DebugPrefixMap;

  /// Path prefixes that should be rewritten in coverage info.
  PathRemapper CoveragePrefixMap;

  /// Path prefixes that should be rewritten in info besides debug and coverage
  /// (use DebugPrefixMap and CoveragePrefixMap for those) - currently just
  /// indexing info.
  PathRemapper FilePrefixMap;

  /// What level of debug info to generate.
  IRGenDebugInfoLevel DebugInfoLevel : 2;

  /// What type of debug info to generate.
  IRGenDebugInfoFormat DebugInfoFormat : 2;

  /// Whether to leave DWARF breadcrumbs pointing to imported Clang modules.
  unsigned DisableClangModuleSkeletonCUs : 1;

  /// Whether we're generating IR for the JIT.
  unsigned UseJIT : 1;
  
  /// Whether we should run LLVM optimizations after IRGen.
  unsigned DisableLLVMOptzns : 1;

  /// Whether we should run swift specific LLVM optimizations after IRGen.
  unsigned DisableSwiftSpecificLLVMOptzns : 1;

  /// Special codegen for playgrounds.
  unsigned Playground : 1;

  /// Emit runtime calls to check the end of the lifetime of stack promoted
  /// objects.
  unsigned EmitStackPromotionChecks : 1;

  unsigned UseSingleModuleLLVMEmission : 1;

  /// Emit functions to separate sections.
  unsigned FunctionSections : 1;

  /// The maximum number of bytes used on a stack frame for stack promotion
  /// (includes alloc_stack allocations).
  unsigned StackPromotionSizeLimit = 1024;

  /// Emit code to verify that static and runtime type layout are consistent for
  /// the given type names.
  SmallVector<StringRef, 1> VerifyTypeLayoutNames;

  /// Frameworks that we should not autolink against.
  SmallVector<std::string, 1> DisableAutolinkFrameworks;

  /// Print the LLVM inline tree at the end of the LLVM pass pipeline.
  unsigned PrintInlineTree : 1;

  /// Always recompile the output even if the module hash might match.
  unsigned AlwaysCompile : 1;

  /// Whether we should embed the bitcode file.
  IRGenEmbedMode EmbedMode : 2;

  IRGenLLVMLTOKind LLVMLTOKind : 2;

  SwiftAsyncFramePointerKind SwiftAsyncFramePointer : 2;

  /// Add names to LLVM values.
  unsigned HasValueNamesSetting : 1;
  unsigned ValueNames : 1;

  /// Emit nominal type field metadata.
  ReflectionMetadataMode ReflectionMetadata : 2;

  /// Emit names of struct stored properties and enum cases.
  unsigned EnableReflectionNames : 1;

  /// Emit mangled names of anonymous context descriptors.
  unsigned EnableAnonymousContextMangledNames : 1;

  /// Force public linkage for private symbols. Used only by the LLDB
  /// expression evaluator.
  unsigned ForcePublicLinkage : 1;
  
  /// Force lazy initialization of class metadata
  /// Used on Windows to avoid cross-module references.
  unsigned LazyInitializeClassMetadata : 1;
  unsigned LazyInitializeProtocolConformances : 1;
  unsigned IndirectAsyncFunctionPointer : 1;

  /// Use absolute function references instead of relative ones.
  /// Mainly used on WebAssembly, that doesn't support relative references
  /// to code from data.
  unsigned CompactAbsoluteFunctionPointer : 1;

  /// Normally if the -read-legacy-type-info flag is not specified, we look for
  /// a file named "legacy-<arch>.yaml" in SearchPathOpts.RuntimeLibraryPath.
  /// Passing this flag completely disables this behavior.
  unsigned DisableLegacyTypeInfo : 1;

  /// Create metadata specializations for generic types at statically known type
  /// arguments.
  unsigned PrespecializeGenericMetadata : 1;

  /// The path to load legacy type layouts from.
  StringRef ReadLegacyTypeInfoPath;

  /// Should we try to build incrementally by not emitting an object file if it
  /// has the same IR hash as the module that we are preparing to emit?
  ///
  /// This is a debugging option meant to make it easier to perform compile time
  /// measurements on a non-clean build directory.
  unsigned UseIncrementalLLVMCodeGen : 1;

  /// Enable the use of type layouts for value witness functions and use
  /// vw functions instead of outlined copy/destroy functions.
  unsigned UseTypeLayoutValueHandling : 1;

  /// Also force structs to be lowered to aligned group TypeLayouts rather than
  /// using TypeInfo entries.
  unsigned ForceStructTypeLayouts : 1;

  /// Enable generation and use of layout string based value witnesses
  unsigned EnableLayoutStringValueWitnesses : 1;

  /// Enable runtime instantiation of value witness strings for generic types
  unsigned EnableLayoutStringValueWitnessesInstantiation : 1;

  /// Instrument code to generate profiling information.
  unsigned GenerateProfile : 1;

  /// Enable chaining of dynamic replacements.
  unsigned EnableDynamicReplacementChaining : 1;

  /// Disable round-trip verification of mangled debug types.
  unsigned DisableRoundTripDebugTypes : 1;

  /// Whether to disable shadow copies for local variables on the stack. This is
  /// only used for testing.
  unsigned DisableDebuggerShadowCopies : 1;
  
  /// Whether to disable using mangled names for accessing concrete type metadata.
  unsigned DisableConcreteTypeMetadataMangledNameAccessors : 1;

  /// Whether to disable referencing stdlib symbols via mangled names in
  /// reflection mangling.
  unsigned DisableStandardSubstitutionsInReflectionMangling : 1;

  unsigned EnableGlobalISel : 1;

  unsigned VirtualFunctionElimination : 1;

  unsigned WitnessMethodElimination : 1;

  unsigned ConditionalRuntimeRecords : 1;

  unsigned InternalizeAtLink : 1;

  /// Internalize symbols (static library) - do not export any public symbols.
  unsigned InternalizeSymbols : 1;

  /// Emit a section with references to class_ro_t* in generic class patterns.
  unsigned EmitGenericRODatas : 1;

  /// Whether to avoid emitting zerofill globals as preallocated type metadata
  /// and protocol conformance caches.
  unsigned NoPreallocatedInstantiationCaches : 1;

  unsigned DisableReadonlyStaticObjects : 1;

  /// Collocate metadata functions in their own section.
  unsigned CollocatedMetadataFunctions : 1;

  /// Colocate type descriptors in their own section.
  unsigned ColocateTypeDescriptors : 1;

  /// Use relative (and constant) protocol witness tables.
  unsigned UseRelativeProtocolWitnessTables : 1;

  /// The number of threads for multi-threaded code generation.
  unsigned NumThreads = 0;

  /// Path to the profdata file to be used for PGO, or the empty string.
  std::string UseProfile = "";

  /// List of backend command-line options for -embed-bitcode.
  std::vector<uint8_t> CmdArgs;

  /// Which sanitizer coverage is turned on.
  llvm::SanitizerCoverageOptions SanitizeCoverage;

  /// Pointer authentication.
  PointerAuthOptions PointerAuth;

  /// The different modes for dumping IRGen type info.
  enum class TypeInfoDumpFilter {
    All,
    Resilient,
    Fragile
  };

  TypeInfoDumpFilter TypeInfoFilter;
  
  /// Pull in runtime compatibility shim libraries by autolinking.
  Optional<llvm::VersionTuple> AutolinkRuntimeCompatibilityLibraryVersion;
  Optional<llvm::VersionTuple> AutolinkRuntimeCompatibilityDynamicReplacementLibraryVersion;
  Optional<llvm::VersionTuple>
      AutolinkRuntimeCompatibilityConcurrencyLibraryVersion;
  bool AutolinkRuntimeCompatibilityBytecodeLayoutsLibrary;

  JITDebugArtifact DumpJIT = JITDebugArtifact::None;

  /// If not an empty string, trap intrinsics are lowered to calls to this
  /// function instead of to trap instructions.
  std::string TrapFuncName = "";

  /// The calling convention used to perform non-swift calls.
  llvm::CallingConv::ID PlatformCCallingConvention;

  IRGenOptions()
      : DWARFVersion(2),
        OutputKind(IRGenOutputKind::LLVMAssemblyAfterOptimization),
        Verify(true), OptMode(OptimizationMode::NotSet),
        Sanitizers(OptionSet<SanitizerKind>()),
        SanitizersWithRecoveryInstrumentation(OptionSet<SanitizerKind>()),
        SanitizeAddressUseODRIndicator(false),
        DebugInfoLevel(IRGenDebugInfoLevel::None),
        DebugInfoFormat(IRGenDebugInfoFormat::None),
        DisableClangModuleSkeletonCUs(false), UseJIT(false),
        DisableLLVMOptzns(false), DisableSwiftSpecificLLVMOptzns(false),
        Playground(false),
        EmitStackPromotionChecks(false), UseSingleModuleLLVMEmission(false),
        FunctionSections(false), PrintInlineTree(false), AlwaysCompile(false),
        EmbedMode(IRGenEmbedMode::None), LLVMLTOKind(IRGenLLVMLTOKind::None),
        SwiftAsyncFramePointer(SwiftAsyncFramePointerKind::Auto),
        HasValueNamesSetting(false), ValueNames(false),
        ReflectionMetadata(ReflectionMetadataMode::Runtime),
        EnableReflectionNames(true), EnableAnonymousContextMangledNames(false),
        ForcePublicLinkage(false), LazyInitializeClassMetadata(false),
        LazyInitializeProtocolConformances(false),
        IndirectAsyncFunctionPointer(false),
        CompactAbsoluteFunctionPointer(false), DisableLegacyTypeInfo(false),
        PrespecializeGenericMetadata(false), UseIncrementalLLVMCodeGen(true),
        UseTypeLayoutValueHandling(true), ForceStructTypeLayouts(false),
        EnableLayoutStringValueWitnesses(false),
        EnableLayoutStringValueWitnessesInstantiation(false),
        GenerateProfile(false), EnableDynamicReplacementChaining(false),
        DisableDebuggerShadowCopies(false),
        DisableConcreteTypeMetadataMangledNameAccessors(false),
        DisableStandardSubstitutionsInReflectionMangling(false),
        EnableGlobalISel(false), VirtualFunctionElimination(false),
        WitnessMethodElimination(false), ConditionalRuntimeRecords(false),
        InternalizeAtLink(false), InternalizeSymbols(false),
        EmitGenericRODatas(false), NoPreallocatedInstantiationCaches(false),
        DisableReadonlyStaticObjects(false),
        CollocatedMetadataFunctions(false),
        ColocateTypeDescriptors(true),
        UseRelativeProtocolWitnessTables(false), CmdArgs(),
        SanitizeCoverage(llvm::SanitizerCoverageOptions()),
        TypeInfoFilter(TypeInfoDumpFilter::All),
        PlatformCCallingConvention(llvm::CallingConv::C) {
#ifndef NDEBUG
    DisableRoundTripDebugTypes = false;
#else
    DisableRoundTripDebugTypes = true;
#endif
  }

  /// Appends to \p os an arbitrary string representing all options which
  /// influence the llvm compilation but are not reflected in the llvm module
  /// itself.
  void writeLLVMCodeGenOptionsTo(llvm::raw_ostream &os) const {
    // We put a letter between each value simply to keep them from running into
    // one another. There might be a vague correspondence between meaning and
    // letter, but don't sweat it.
    os << 'O' << (unsigned)OptMode
       << 'd' << DisableLLVMOptzns
       << 'D' << DisableSwiftSpecificLLVMOptzns
       << 'p' << GenerateProfile
       << 's' << Sanitizers.toRaw();
  }

  /// Should LLVM IR value names be emitted and preserved?
  bool shouldProvideValueNames() const {
    // If the command line contains an explicit request about whether to add
    // LLVM value names, honor it.  Otherwise, add value names only if the
    // final result is textual LLVM assembly.
    if (HasValueNamesSetting) {
      return ValueNames;
    } else {
      return OutputKind == IRGenOutputKind::LLVMAssemblyBeforeOptimization ||
             OutputKind == IRGenOutputKind::LLVMAssemblyAfterOptimization;
    }
  }

  bool shouldOptimize() const {
    return OptMode > OptimizationMode::NoOptimization;
  }

  bool optimizeForSize() const {
    return OptMode == OptimizationMode::ForSize;
  }

  std::string getDebugFlags(StringRef PrivateDiscriminator,
                            bool EnableCXXInterop) const {
    std::string Flags = DebugFlags;
    if (!PrivateDiscriminator.empty()) {
      if (!Flags.empty())
        Flags += " ";
      Flags += ("-private-discriminator " + PrivateDiscriminator).str();
    }
    if (EnableCXXInterop) {
      if (!Flags.empty())
        Flags += " ";
      Flags += "-enable-experimental-cxx-interop";
    }
    return Flags;
  }

  /// Return a hash code of any components from these options that should
  /// contribute to a Swift Bridging PCH hash.
  llvm::hash_code getPCHHashComponents() const {
    return llvm::hash_value(0);
  }

  /// Return a hash code of any components from these options that should
  /// contribute to a Swift Dependency Scanning hash.
  llvm::hash_code getModuleScanningHashComponents() const {
    return llvm::hash_value(0);
  }

  bool hasMultipleIRGenThreads() const { return !UseSingleModuleLLVMEmission && NumThreads > 1; }
  bool shouldPerformIRGenerationInParallel() const { return !UseSingleModuleLLVMEmission && NumThreads != 0; }
  bool hasMultipleIGMs() const { return hasMultipleIRGenThreads(); }
};

} // end namespace swift

#endif
