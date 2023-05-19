//===--- ASTContext.cpp - ASTContext Implementation -----------------------===//
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
//  This file implements the ASTContext class.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTContext.h"
#include "ClangTypeConverter.h"
#include "ForeignRepresentationInfo.h"
#include "SubstitutionMapStorage.h"
#include "swift/AST/ClangModuleLoader.h"
#include "swift/AST/ConcreteDeclRef.h"
#include "swift/AST/DiagnosticEngine.h"
#include "swift/AST/DiagnosticsFrontend.h"
#include "swift/AST/DiagnosticsSema.h"
#include "swift/AST/DistributedDecl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/ExtInfo.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/ForeignAsyncConvention.h"
#include "swift/AST/ForeignErrorConvention.h"
#include "swift/AST/GenericEnvironment.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/ImportCache.h"
#include "swift/AST/IndexSubset.h"
#include "swift/AST/KnownProtocols.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/MacroDiscriminatorContext.h"
#include "swift/AST/ModuleDependencies.h"
#include "swift/AST/ModuleLoader.h"
#include "swift/AST/NameLookup.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PluginLoader.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/PropertyWrappers.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/RawComment.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/AST/SemanticAttrs.h"
#include "swift/AST/SourceFile.h"
#include "swift/AST/SubstitutionMap.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Compiler.h"
#include "swift/Basic/SourceManager.h"
#include "swift/Basic/Statistic.h"
#include "swift/Basic/StringExtras.h"
#include "swift/ClangImporter/ClangModule.h"
#include "swift/Strings.h"
#include "swift/Subsystems.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "clang/AST/Type.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/FormatVariadic.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include "llvm/Support/VirtualOutputBackends.h"
#include <algorithm>
#include <memory>
#include <queue>

#if !defined(_WIN32)
#include <dlfcn.h>
#endif

#include "RequirementMachine/RewriteContext.h"

using namespace swift;

#define DEBUG_TYPE "ASTContext"
STATISTIC(NumCollapsedSpecializedProtocolConformances,
          "# of specialized protocol conformances collapsed");

void ModuleLoader::anchor() {}
void ClangModuleLoader::anchor() {}

llvm::StringRef swift::getProtocolName(KnownProtocolKind kind) {
  switch (kind) {
#define PROTOCOL_WITH_NAME(Id, Name) \
  case KnownProtocolKind::Id: \
    return Name;
#include "swift/AST/KnownProtocols.def"
  }
  llvm_unreachable("bad KnownProtocolKind");
}

namespace {
  enum class SearchPathKind : uint8_t {
    Import = 1 << 0,
    Framework = 1 << 1,
    CompilerPlugin = 1 << 2
  };
} // end anonymous namespace

using AssociativityCacheType =
  llvm::DenseMap<std::pair<PrecedenceGroupDecl *, PrecedenceGroupDecl *>,
                 Associativity>;

struct OverrideSignatureKey {
  GenericSignature baseMethodSig;
  const NominalTypeDecl *baseNominal;
  const NominalTypeDecl *derivedNominal;
  const GenericParamList *derivedParams;

  OverrideSignatureKey(GenericSignature baseMethodSig,
                       const NominalTypeDecl *baseNominal,
                       const NominalTypeDecl *derivedNominal,
                       const GenericParamList *derivedParams)
    : baseMethodSig(baseMethodSig),
      baseNominal(baseNominal),
      derivedNominal(derivedNominal),
      derivedParams(derivedParams) {}
};

namespace llvm {
template <> struct DenseMapInfo<OverrideSignatureKey> {
  using Type = swift::Type;
  using GenericSignature = swift::GenericSignature;

  static bool isEqual(const OverrideSignatureKey lhs,
                      const OverrideSignatureKey rhs) {
    return lhs.baseMethodSig.getPointer() == rhs.baseMethodSig.getPointer() &&
           lhs.baseNominal == rhs.baseNominal &&
           lhs.derivedNominal == rhs.derivedNominal &&
           lhs.derivedParams == rhs.derivedParams;
  }

  static inline OverrideSignatureKey getEmptyKey() {
    return OverrideSignatureKey(DenseMapInfo<GenericSignature>::getEmptyKey(),
                                DenseMapInfo<NominalTypeDecl *>::getEmptyKey(),
                                DenseMapInfo<NominalTypeDecl *>::getEmptyKey(),
                                DenseMapInfo<GenericParamList *>::getEmptyKey());
  }

  static inline OverrideSignatureKey getTombstoneKey() {
    return OverrideSignatureKey(
        DenseMapInfo<GenericSignature>::getTombstoneKey(),
        DenseMapInfo<NominalTypeDecl *>::getTombstoneKey(),
        DenseMapInfo<NominalTypeDecl *>::getTombstoneKey(),
        DenseMapInfo<GenericParamList *>::getTombstoneKey());
  }

  static unsigned getHashValue(const OverrideSignatureKey &Val) {
    return hash_combine(
        DenseMapInfo<GenericSignature>::getHashValue(Val.baseMethodSig),
        DenseMapInfo<NominalTypeDecl *>::getHashValue(Val.baseNominal),
        DenseMapInfo<NominalTypeDecl *>::getHashValue(Val.derivedNominal),
        DenseMapInfo<GenericParamList *>::getHashValue(Val.derivedParams));
  }
};
} // namespace llvm

struct ASTContext::Implementation {
  Implementation();
  ~Implementation();

  llvm::BumpPtrAllocator Allocator; // used in later initializations

  /// The set of cleanups to be called when the ASTContext is destroyed.
  std::vector<std::function<void(void)>> Cleanups;

  /// The set of top-level modules we have loaded.
  /// This map is used for iteration, therefore it's a MapVector and not a
  /// DenseMap.
  llvm::MapVector<Identifier, ModuleDecl *> LoadedModules;

  // FIXME: This is a StringMap rather than a StringSet because StringSet
  // doesn't allow passing in a pre-existing allocator.
  llvm::StringMap<Identifier::Aligner, llvm::BumpPtrAllocator&>
  IdentifierTable;

  /// The declaration of Swift.AssignmentPrecedence.
  PrecedenceGroupDecl *AssignmentPrecedence = nullptr;

  /// The declaration of Swift.CastingPrecedence.
  PrecedenceGroupDecl *CastingPrecedence = nullptr;

  /// The declaration of Swift.FunctionArrowPrecedence.
  PrecedenceGroupDecl *FunctionArrowPrecedence = nullptr;

  /// The declaration of Swift.TernaryPrecedence.
  PrecedenceGroupDecl *TernaryPrecedence = nullptr;

  /// The declaration of Swift.DefaultPrecedence.
  PrecedenceGroupDecl *DefaultPrecedence = nullptr;

  /// The AnyObject type.
  CanType AnyObjectType;

#define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
  /** The declaration of Swift.NAME. */ \
  DECL_CLASS *NAME##Decl = nullptr;
#include "swift/AST/KnownStdlibTypes.def"

#define KNOWN_SDK_TYPE_DECL(MODULE, NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
  /** The declaration of MODULE.NAME. */ \
  DECL_CLASS *NAME##Decl = nullptr;
#include "swift/AST/KnownSDKTypes.def"

  /// The declaration of '+' function for two RangeReplaceableCollection.
  FuncDecl *PlusFunctionOnRangeReplaceableCollection = nullptr;

  /// The declaration of '+' function for two String.
  FuncDecl *PlusFunctionOnString = nullptr;

  /// The declaration of 'Sequence.makeIterator()'.
  FuncDecl *MakeIterator = nullptr;

  /// The declaration of 'AsyncSequence.makeAsyncIterator()'.
  FuncDecl *MakeAsyncIterator = nullptr;

  /// The declaration of 'IteratorProtocol.next()'.
  FuncDecl *IteratorNext = nullptr;

  /// The declaration of 'AsyncIteratorProtocol.next()'.
  FuncDecl *AsyncIteratorNext = nullptr;

  /// The declaration of Swift.Optional<T>.Some.
  EnumElementDecl *OptionalSomeDecl = nullptr;

  /// The declaration of Swift.Optional<T>.None.
  EnumElementDecl *OptionalNoneDecl = nullptr;

  /// The declaration of Swift.Void.
  TypeAliasDecl *VoidDecl = nullptr;

  /// The declaration of Swift.UnsafeMutableRawPointer.memory.
  VarDecl *UnsafeMutableRawPointerMemoryDecl = nullptr;

  /// The declaration of Swift.UnsafeRawPointer.memory.
  VarDecl *UnsafeRawPointerMemoryDecl = nullptr;

  /// The declaration of Swift.UnsafeMutablePointer<T>.memory.
  VarDecl *UnsafeMutablePointerMemoryDecl = nullptr;
  
  /// The declaration of Swift.UnsafePointer<T>.memory.
  VarDecl *UnsafePointerMemoryDecl = nullptr;
  
  /// The declaration of Swift.AutoreleasingUnsafeMutablePointer<T>.memory.
  VarDecl *AutoreleasingUnsafeMutablePointerMemoryDecl = nullptr;

  /// The declaration of _Concurrency.DefaultActor.
  ClassDecl *DefaultActorDecl = nullptr;

  /// The declaration of _Concurrency.NSObjectDefaultActor.
  ClassDecl *NSObjectDefaultActorDecl = nullptr;

  // Declare cached declarations for each of the known declarations.
#define FUNC_DECL(Name, Id) FuncDecl *Get##Name = nullptr;
#include "swift/AST/KnownDecls.def"

  // Declare cached declarations for each of the known declarations.
#define KNOWN_SDK_FUNC_DECL(Module, Name, Id) FuncDecl *Get##Name = nullptr;
#include "swift/AST/KnownSDKDecls.def"
  
  /// func <Int, Int) -> Bool
  FuncDecl *LessThanIntDecl = nullptr;
  
  /// func ==(Int, Int) -> Bool
  FuncDecl *EqualIntDecl = nullptr;

  /// func _hashValue<H: Hashable>(for: H) -> Int
  FuncDecl *HashValueForDecl = nullptr;

  /// func append(Element) -> void
  FuncDecl *ArrayAppendElementDecl = nullptr;

  /// init(Builtin.RawPointer, Builtin.Word, Builtin.Int1)
  ConstructorDecl *MakeUTF8StringDecl = nullptr;

  /// func reserveCapacityForAppend(newElementsCount: Int)
  FuncDecl *ArrayReserveCapacityDecl = nullptr;

  /// func _stdlib_isOSVersionAtLeast(Builtin.Word,Builtin.Word, Builtin.word)
  ///    -> Builtin.Int1
  FuncDecl *IsOSVersionAtLeastDecl = nullptr;

  /// The set of known protocols, lazily populated as needed.
  ProtocolDecl *KnownProtocols[NumKnownProtocols] = { };

  /// The module interface checker owned by the ASTContext.
  std::unique_ptr<ModuleInterfaceChecker> InterfaceChecker;

  /// The various module loaders that import external modules into this
  /// ASTContext.
  SmallVector<std::unique_ptr<swift::ModuleLoader>, 4> ModuleLoaders;

  /// Singleton used to cache the import graph.
  swift::namelookup::ImportCache TheImportCache;

  /// The module loader used to load Clang modules.
  ClangModuleLoader *TheClangModuleLoader = nullptr;

  /// The module loader used to load Clang modules from DWARF.
  ClangModuleLoader *TheDWARFModuleLoader = nullptr;

  /// Map from Swift declarations to deserialized resolved locations, ie.
  /// actual \c SourceLocs that require opening their external buffer.
  llvm::DenseMap<const Decl *, ExternalSourceLocs *> ExternalSourceLocs;

  /// Map from declarations to foreign error conventions.
  /// This applies to both actual imported functions and to @objc functions.
  llvm::DenseMap<const AbstractFunctionDecl *,
                 ForeignErrorConvention> ForeignErrorConventions;

  /// Map from declarations to foreign async conventions.
  llvm::DenseMap<const AbstractFunctionDecl *,
                 ForeignAsyncConvention> ForeignAsyncConventions;

  /// Cache of previously looked-up precedence queries.
  AssociativityCacheType AssociativityCache;

  /// Map from normal protocol conformances to diagnostics that have
  /// been delayed until the conformance is fully checked.
  llvm::DenseMap<NormalProtocolConformance *,
                 std::vector<ASTContext::DelayedConformanceDiag>>
    DelayedConformanceDiags;

  /// Map from normal protocol conformances to missing witnesses that have
  /// been delayed until the conformance is fully checked, so that we can
  /// issue a fixit that fills the entire protocol stub.
  llvm::DenseMap<
      NormalProtocolConformance *, std::unique_ptr<MissingWitnessesBase>>
    DelayedMissingWitnesses;

  /// Stores information about lazy deserialization of various declarations.
  llvm::DenseMap<const DeclContext *, LazyContextData *> LazyContexts;

  /// A fake generic parameter list <Self> for parsing @opened archetypes
  /// in textual SIL.
  GenericParamList *SelfGenericParamList = nullptr;

  /// The single-parameter generic signature with no constraints, <T>.
  CanGenericSignature SingleGenericParameterSignature;

  /// The existential signature <T : P> for each P.
  llvm::DenseMap<std::pair<CanType, const GenericSignatureImpl *>, CanGenericSignature>
      ExistentialSignatures;

  /// The element signature for a generic signature, which contains a clone
  /// of the context generic signature with new type parameters and requirements
  /// for opened pack elements in the given shape equivalence class.
  llvm::DenseMap<std::pair<CanType, const GenericSignatureImpl *>,
                 CanGenericSignature> ElementSignatures;

  /// Overridden declarations.
  llvm::DenseMap<const ValueDecl *, ArrayRef<ValueDecl *>> Overrides;

  /// Default witnesses.
  llvm::DenseMap<std::pair<const ProtocolDecl *, ValueDecl *>, Witness>
    DefaultWitnesses;

  /// Default type witnesses for protocols.
  llvm::DenseMap<std::pair<const ProtocolDecl *, AssociatedTypeDecl *>, Type>
    DefaultTypeWitnesses;

  /// Default associated conformance witnesses for protocols.
  llvm::DenseMap<std::tuple<const ProtocolDecl *, CanType, ProtocolDecl *>,
                 ProtocolConformanceRef>
    DefaultAssociatedConformanceWitnesses;

  /// Caches of default types for DefaultTypeRequest.
  /// Used to be instance variables in the TypeChecker.
  /// There is a logically separate cache for each SourceFile and
  /// KnownProtocolKind.
  llvm::DenseMap<SourceFile *, std::array<Type, NumKnownProtocols>>
      DefaultTypeRequestCaches;

  /// Mapping from property declarations to the backing variable types.
  llvm::DenseMap<const VarDecl *, Type> PropertyWrapperBackingVarTypes;

  /// A mapping from the backing storage of a property that has a wrapper
  /// to the original property with the wrapper.
  llvm::DenseMap<const VarDecl *, VarDecl *> OriginalWrappedProperties;

  /// The builtin initializer witness for a literal. Used when building
  /// LiteralExprs in fully-checked AST.
  llvm::DenseMap<const NominalTypeDecl *, ConcreteDeclRef> BuiltinInitWitness;

  /// Mapping from the function decl to its original body's source range. This
  /// is populated if the body is reparsed from other source buffers.
  llvm::DenseMap<const AbstractFunctionDecl *, SourceRange> OriginalBodySourceRanges;

  /// Macro discriminators per context.
  llvm::DenseMap<std::pair<const void *, Identifier>, unsigned>
      NextMacroDiscriminator;

  /// Structure that captures data that is segregated into different
  /// arenas.
  struct Arena {
    static_assert(alignof(TypeBase) >= 8, "TypeBase not 8-byte aligned?");
    static_assert(alignof(TypeBase) > static_cast<unsigned>(
               MetatypeRepresentation::Last_MetatypeRepresentation) + 1,
               "Use std::pair for MetatypeTypes and ExistentialMetatypeTypes.");

    llvm::DenseMap<Type, ErrorType *> ErrorTypesWithOriginal;
    llvm::FoldingSet<TypeAliasType> TypeAliasTypes;
    llvm::FoldingSet<TupleType> TupleTypes;
    llvm::FoldingSet<PackType> PackTypes;
    llvm::FoldingSet<PackExpansionType> PackExpansionTypes;
    llvm::DenseMap<llvm::PointerIntPair<TypeBase*, 3, unsigned>,
                   MetatypeType*> MetatypeTypes;
    llvm::DenseMap<llvm::PointerIntPair<TypeBase*, 3, unsigned>,
                   ExistentialMetatypeType*> ExistentialMetatypeTypes;
    llvm::DenseMap<Type, ArraySliceType*> ArraySliceTypes;
    llvm::DenseMap<Type, VariadicSequenceType*> VariadicSequenceTypes;
    llvm::DenseMap<std::pair<Type, Type>, DictionaryType *> DictionaryTypes;
    llvm::DenseMap<Type, OptionalType*> OptionalTypes;
    llvm::DenseMap<Type, ParenType*> ParenTypes;
    llvm::DenseMap<uintptr_t, ReferenceStorageType*> ReferenceStorageTypes;
    llvm::DenseMap<Type, LValueType*> LValueTypes;
    llvm::DenseMap<Type, InOutType*> InOutTypes;
    llvm::DenseMap<std::pair<Type, void*>, DependentMemberType *>
      DependentMemberTypes;
    llvm::DenseMap<void *, PlaceholderType *> PlaceholderTypes;
    llvm::DenseMap<Type, DynamicSelfType *> DynamicSelfTypes;
    llvm::DenseMap<std::pair<EnumDecl*, Type>, EnumType*> EnumTypes;
    llvm::DenseMap<std::pair<StructDecl*, Type>, StructType*> StructTypes;
    llvm::DenseMap<std::pair<ClassDecl*, Type>, ClassType*> ClassTypes;
    llvm::DenseMap<std::pair<ProtocolDecl*, Type>, ProtocolType*> ProtocolTypes;
    llvm::DenseMap<Type, ExistentialType *> ExistentialTypes;
    llvm::FoldingSet<UnboundGenericType> UnboundGenericTypes;
    llvm::FoldingSet<BoundGenericType> BoundGenericTypes;
    llvm::FoldingSet<ProtocolCompositionType> ProtocolCompositionTypes;
    llvm::FoldingSet<ParameterizedProtocolType> ParameterizedProtocolTypes;
    llvm::FoldingSet<LayoutConstraintInfo> LayoutConstraints;
    llvm::DenseMap<std::pair<OpaqueTypeDecl *, SubstitutionMap>,
                   GenericEnvironment *> OpaqueArchetypeEnvironments;

    /// The set of function types.
    llvm::FoldingSet<FunctionType> FunctionTypes;

    /// The set of normal protocol conformances.
    llvm::FoldingSet<NormalProtocolConformance> NormalConformances;

    // The set of self protocol conformances.
    llvm::DenseMap<ProtocolDecl*, SelfProtocolConformance*> SelfConformances;

    /// The set of specialized protocol conformances.
    llvm::FoldingSet<SpecializedProtocolConformance> SpecializedConformances;

    /// The set of inherited protocol conformances.
    llvm::FoldingSet<InheritedProtocolConformance> InheritedConformances;

    /// The set of builtin protocol conformances.
    llvm::DenseMap<std::pair<Type, ProtocolDecl *>,
                   BuiltinProtocolConformance *> BuiltinConformances;

    /// The set of pack conformances.
    llvm::FoldingSet<PackConformance> PackConformances;

    /// The set of substitution maps (uniqued by their storage).
    llvm::FoldingSet<SubstitutionMap::Storage> SubstitutionMaps;

    ~Arena() {
      for (auto &conformance : SpecializedConformances)
        conformance.~SpecializedProtocolConformance();
      // Work around MSVC warning: local variable is initialized but
      // not referenced.
#if SWIFT_COMPILER_IS_MSVC
#pragma warning (disable: 4189)
#endif
      for (auto &conformance : InheritedConformances)
        conformance.~InheritedProtocolConformance();
#if SWIFT_COMPILER_IS_MSVC
#pragma warning (default: 4189)
#endif

      // Call the normal conformance destructors last since they could be
      // referenced by the other conformance types.
      for (auto &conformance : NormalConformances)
        conformance.~NormalProtocolConformance();
    }

    size_t getTotalMemory() const;
  };

  llvm::DenseMap<ModuleDecl*, ModuleType*> ModuleTypes;
  llvm::DenseMap<std::pair<unsigned, unsigned>, GenericTypeParamType *>
    GenericParamTypes;
  llvm::FoldingSet<GenericFunctionType> GenericFunctionTypes;
  llvm::FoldingSet<SILFunctionType> SILFunctionTypes;
  llvm::FoldingSet<SILPackType> SILPackTypes;
  llvm::DenseMap<CanType, SILBlockStorageType *> SILBlockStorageTypes;
  llvm::DenseMap<CanType, SILMoveOnlyWrappedType *> SILMoveOnlyWrappedTypes;
  llvm::FoldingSet<SILBoxType> SILBoxTypes;
  llvm::DenseMap<BuiltinIntegerWidth, BuiltinIntegerType*> IntegerTypes;
  llvm::FoldingSet<BuiltinVectorType> BuiltinVectorTypes;
  llvm::FoldingSet<DeclName::CompoundDeclName> CompoundNames;
  llvm::DenseMap<UUID, GenericEnvironment *> OpenedExistentialEnvironments;
  llvm::DenseMap<UUID, GenericEnvironment *> OpenedElementEnvironments;
  llvm::FoldingSet<IndexSubset> IndexSubsets;
  llvm::FoldingSet<AutoDiffDerivativeFunctionIdentifier>
      AutoDiffDerivativeFunctionIdentifiers;

  llvm::FoldingSet<GenericSignatureImpl> GenericSignatures;

  /// A cache of information about whether particular nominal types
  /// are representable in a foreign language.
  llvm::DenseMap<NominalTypeDecl *, ForeignRepresentationInfo>
    ForeignRepresentableCache;

  llvm::StringMap<OptionSet<SearchPathKind>> SearchPathsSet;

  /// Plugin loader.
  std::unique_ptr<swift::PluginLoader> Plugins;

  /// The permanent arena.
  Arena Permanent;

  /// Temporary arena used for a constraint solver.
  struct ConstraintSolverArena : public Arena {
    /// The allocator used for all allocations within this arena.
    llvm::BumpPtrAllocator &Allocator;

    ConstraintSolverArena(llvm::BumpPtrAllocator &allocator)
      : Allocator(allocator) { }

    ConstraintSolverArena(const ConstraintSolverArena &) = delete;
    ConstraintSolverArena(ConstraintSolverArena &&) = delete;
    ConstraintSolverArena &operator=(const ConstraintSolverArena &) = delete;
    ConstraintSolverArena &operator=(ConstraintSolverArena &&) = delete;
  };

  /// The current constraint solver arena, if any.
  std::unique_ptr<ConstraintSolverArena> CurrentConstraintSolverArena;

  Arena &getArena(AllocationArena arena) {
    switch (arena) {
    case AllocationArena::Permanent:
      return Permanent;

    case AllocationArena::ConstraintSolver:
      assert(CurrentConstraintSolverArena && "No constraint solver active?");
      return *CurrentConstraintSolverArena;
    }
    llvm_unreachable("bad AllocationArena");
  }
  
  llvm::FoldingSet<SILLayout> SILLayouts;

  llvm::DenseMap<OverrideSignatureKey, GenericSignature> overrideSigCache;

  Optional<ClangTypeConverter> Converter;

  /// The IRGen specific SIL transforms that have been registered.
  SILTransformCtors IRGenSILPasses;

  /// The scratch context used to allocate intrinsic data on behalf of \c swift::IntrinsicInfo
  std::unique_ptr<llvm::LLVMContext> IntrinsicScratchContext;

  /// Memory allocation arena for the term rewriting system.
  std::unique_ptr<rewriting::RewriteContext> TheRewriteContext;

  /// The singleton Builtin.TheTupleType.
  BuiltinTupleDecl *TheTupleTypeDecl = nullptr;

  /// The declared interface type of Builtin.TheTupleType.
  BuiltinTupleType *TheTupleType = nullptr;
};

ASTContext::Implementation::Implementation()
    : IdentifierTable(Allocator),
      IntrinsicScratchContext(new llvm::LLVMContext()) {}
ASTContext::Implementation::~Implementation() {
  for (auto &cleanup : Cleanups)
    cleanup();
}

ConstraintCheckerArenaRAII::
ConstraintCheckerArenaRAII(ASTContext &self, llvm::BumpPtrAllocator &allocator)
  : Self(self), Data(self.getImpl().CurrentConstraintSolverArena.release())
{
  Self.getImpl().CurrentConstraintSolverArena.reset(
    new ASTContext::Implementation::ConstraintSolverArena(allocator));
}

ConstraintCheckerArenaRAII::~ConstraintCheckerArenaRAII() {
  Self.getImpl().CurrentConstraintSolverArena.reset(
    (ASTContext::Implementation::ConstraintSolverArena *)Data);
}

static ModuleDecl *createBuiltinModule(ASTContext &ctx) {
  auto M = ModuleDecl::create(ctx.getIdentifier(BUILTIN_NAME), ctx);
  M->addFile(*new (ctx) BuiltinUnit(*M));
  M->setHasResolvedImports();
  return M;
}

inline ASTContext::Implementation &ASTContext::getImpl() const {
  auto pointer = reinterpret_cast<char*>(const_cast<ASTContext*>(this));
  auto offset = llvm::alignAddr((void *)sizeof(*this),
                                llvm::Align(alignof(Implementation)));
  return *reinterpret_cast<Implementation*>(pointer + offset);
}

void ASTContext::operator delete(void *Data) throw() {
  AlignedFree(Data);
}

ASTContext *ASTContext::get(
    LangOptions &langOpts, TypeCheckerOptions &typecheckOpts,
    SILOptions &silOpts, SearchPathOptions &SearchPathOpts,
    ClangImporterOptions &ClangImporterOpts,
    symbolgraphgen::SymbolGraphOptions &SymbolGraphOpts,
    SourceManager &SourceMgr, DiagnosticEngine &Diags,
    llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutputBackend,
    std::function<bool(llvm::StringRef, bool)> PreModuleImportCallback) {
  // If more than two data structures are concatentated, then the aggregate
  // size math needs to become more complicated due to per-struct alignment
  // constraints.
  auto align = std::max(alignof(ASTContext), alignof(Implementation));
  auto size = llvm::alignTo(sizeof(ASTContext) + sizeof(Implementation), align);
  auto mem = AlignedAlloc(size, align);
  auto impl = reinterpret_cast<void*>((char*)mem + sizeof(ASTContext));
  impl = reinterpret_cast<void *>(
      llvm::alignAddr(impl, llvm::Align(alignof(Implementation))));
  new (impl) Implementation();
  return new (mem)
      ASTContext(langOpts, typecheckOpts, silOpts, SearchPathOpts,
                 ClangImporterOpts, SymbolGraphOpts, SourceMgr, Diags,
                 std::move(OutputBackend), PreModuleImportCallback);
}

ASTContext::ASTContext(
    LangOptions &langOpts, TypeCheckerOptions &typecheckOpts,
    SILOptions &silOpts, SearchPathOptions &SearchPathOpts,
    ClangImporterOptions &ClangImporterOpts,
    symbolgraphgen::SymbolGraphOptions &SymbolGraphOpts,
    SourceManager &SourceMgr, DiagnosticEngine &Diags,
    llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutBackend,
    std::function<bool(llvm::StringRef, bool)> PreModuleImportCallback)
    : LangOpts(langOpts), TypeCheckerOpts(typecheckOpts), SILOpts(silOpts),
      SearchPathOpts(SearchPathOpts), ClangImporterOpts(ClangImporterOpts),
      SymbolGraphOpts(SymbolGraphOpts), SourceMgr(SourceMgr), Diags(Diags),
      OutputBackend(std::move(OutBackend)), evaluator(Diags, langOpts),
      TheBuiltinModule(createBuiltinModule(*this)),
      StdlibModuleName(getIdentifier(STDLIB_NAME)),
      SwiftShimsModuleName(getIdentifier(SWIFT_SHIMS_NAME)),
      PreModuleImportCallback(PreModuleImportCallback),
      TheErrorType(new (*this, AllocationArena::Permanent) ErrorType(
          *this, Type(), RecursiveTypeProperties::HasError)),
      TheUnresolvedType(new(*this, AllocationArena::Permanent)
                            UnresolvedType(*this)),
      TheEmptyTupleType(TupleType::get(ArrayRef<TupleTypeElt>(), *this)),
      TheEmptyPackType(PackType::get(*this, {})),
      TheAnyType(ProtocolCompositionType::get(*this, ArrayRef<Type>(),
                                              /*HasExplicitAnyObject=*/false)),
#define SINGLETON_TYPE(SHORT_ID, ID) \
    The##SHORT_ID##Type(new (*this, AllocationArena::Permanent) \
                          ID##Type(*this)),
#include "swift/AST/TypeNodes.def"
      TheIEEE32Type(new (*this, AllocationArena::Permanent)
                        BuiltinFloatType(BuiltinFloatType::IEEE32, *this)),
      TheIEEE64Type(new (*this, AllocationArena::Permanent)
                        BuiltinFloatType(BuiltinFloatType::IEEE64, *this)),
      TheIEEE16Type(new (*this, AllocationArena::Permanent)
                        BuiltinFloatType(BuiltinFloatType::IEEE16, *this)),
      TheIEEE80Type(new (*this, AllocationArena::Permanent)
                        BuiltinFloatType(BuiltinFloatType::IEEE80, *this)),
      TheIEEE128Type(new (*this, AllocationArena::Permanent)
                         BuiltinFloatType(BuiltinFloatType::IEEE128, *this)),
      ThePPC128Type(new (*this, AllocationArena::Permanent)
                        BuiltinFloatType(BuiltinFloatType::PPC128, *this)) {

  // Initialize all of the known identifiers.
#define IDENTIFIER_WITH_NAME(Name, IdStr) Id_##Name = getIdentifier(IdStr);
#include "swift/AST/KnownIdentifiers.def"

  // Record the initial set of search paths.
  for (StringRef path : SearchPathOpts.getImportSearchPaths())
    getImpl().SearchPathsSet[path] |= SearchPathKind::Import;
  for (const auto &framepath : SearchPathOpts.getFrameworkSearchPaths())
    getImpl().SearchPathsSet[framepath.Path] |= SearchPathKind::Framework;
  for (StringRef path : SearchPathOpts.getCompilerPluginLibraryPaths())
    getImpl().SearchPathsSet[path] |= SearchPathKind::CompilerPlugin;

  // Register any request-evaluator functions available at the AST layer.
  registerAccessRequestFunctions(evaluator);
  registerNameLookupRequestFunctions(evaluator);

  // Provide a default OnDiskOutputBackend if user didn't supply one.
  if (!OutputBackend)
    OutputBackend = llvm::makeIntrusiveRefCnt<llvm::vfs::OnDiskOutputBackend>();
  // Insert all block list config paths.
  for (auto path: langOpts.BlocklistConfigFilePaths) {
    blockListConfig.addConfigureFilePath(path);
  }
}

ASTContext::~ASTContext() {
  getImpl().~Implementation();
}

llvm::BumpPtrAllocator &ASTContext::getAllocator(AllocationArena arena) const {
  switch (arena) {
  case AllocationArena::Permanent:
    return getImpl().Allocator;

  case AllocationArena::ConstraintSolver:
    assert(getImpl().CurrentConstraintSolverArena != nullptr);
    return getImpl().CurrentConstraintSolverArena->Allocator;
  }
  llvm_unreachable("bad AllocationArena");
}

void *detail::allocateInASTContext(size_t bytes, const ASTContext &ctx,
                                   AllocationArena arena, unsigned alignment) {
  return ctx.Allocate(bytes, alignment, arena);
}

ImportPath::Raw
swift::detail::ImportPathBuilder_copyToImpl(ASTContext &ctx,
                                            ImportPath::Raw raw) {
  return ctx.AllocateCopy(raw);
}

Identifier
swift::detail::ImportPathBuilder_getIdentifierImpl(ASTContext &ctx,
                                                   StringRef string) {
  return ctx.getIdentifier(string);
}

/// Set a new stats reporter.
void ASTContext::setStatsReporter(UnifiedStatsReporter *stats) {
  if (stats) {
    stats->getFrontendCounters().NumASTBytesAllocated =
        getAllocator().getBytesAllocated();
  }
  evaluator.setStatsReporter(stats);
  Stats = stats;
}

/// getIdentifier - Return the uniqued and AST-Context-owned version of the
/// specified string.
Identifier ASTContext::getIdentifier(StringRef Str) const {
  // Make sure null pointers stay null.
  if (Str.data() == nullptr)
    return Identifier(nullptr);

  auto pair = std::make_pair(Str, Identifier::Aligner());
  auto I = getImpl().IdentifierTable.insert(pair).first;
  return Identifier(I->getKeyData());
}

void ASTContext::lookupInModule(
    ModuleDecl *M,
    StringRef name,
    SmallVectorImpl<ValueDecl *> &results) const {
  if (!M)
    return;

  // Find all of the declarations with this name in the Swift module.
  auto identifier = getIdentifier(name);
  M->lookupValue(identifier, NLKind::UnqualifiedLookup, results);
}

void ASTContext::lookupInSwiftModule(
                   StringRef name,
                   SmallVectorImpl<ValueDecl *> &results) const {
  lookupInModule(getStdlibModule(), name, results);
}

FuncDecl *ASTContext::getPlusFunctionOnRangeReplaceableCollection() const {
  if (getImpl().PlusFunctionOnRangeReplaceableCollection) {
    return getImpl().PlusFunctionOnRangeReplaceableCollection;
  }
  // Find all of the declarations with this name in the Swift module.
  SmallVector<ValueDecl *, 1> Results;
  lookupInSwiftModule("+", Results);
  for (auto Result : Results) {
    if (auto *FD = dyn_cast<FuncDecl>(Result)) {
      if (!FD->getOperatorDecl())
        continue;
      for (auto Req: FD->getGenericRequirements()) {
        if (Req.getKind() == RequirementKind::Conformance &&
              Req.getProtocolDecl() ==
              getProtocol(KnownProtocolKind::RangeReplaceableCollection)) {
          getImpl().PlusFunctionOnRangeReplaceableCollection = FD;
        }
      }
    }
  }
  return getImpl().PlusFunctionOnRangeReplaceableCollection;
}

FuncDecl *ASTContext::getPlusFunctionOnString() const {
  if (getImpl().PlusFunctionOnString) {
    return getImpl().PlusFunctionOnString;
  }
  // Find all of the declarations with this name in the Swift module.
  SmallVector<ValueDecl *, 1> Results;
  lookupInSwiftModule("+", Results);
  for (auto Result : Results) {
    if (auto *FD = dyn_cast<FuncDecl>(Result)) {
      if (!FD->getOperatorDecl())
        continue;
      auto ResultType = FD->getResultInterfaceType();
      if (!ResultType->isString())
        continue;
      auto ParamList = FD->getParameters();
      if (ParamList->size() != 2)
        continue;
      if (ParamList->get(0)->getInterfaceType()->isString() &&
          ParamList->get(1)->getInterfaceType()->isString()) {
        getImpl().PlusFunctionOnString = FD;
        break;
      }
    }
  }
  return getImpl().PlusFunctionOnString;
}

static FuncDecl *lookupRequirement(ProtocolDecl *proto,
                                   Identifier requirement) {
  for (auto result : proto->lookupDirect(requirement)) {
    if (result->getDeclContext() != proto)
      continue;

    if (auto func = dyn_cast<FuncDecl>(result)) {
      if (func->getParameters()->size() != 0)
        continue;

      return func;
    }
  }

  return nullptr;
}

FuncDecl *ASTContext::getSequenceMakeIterator() const {
  if (getImpl().MakeIterator) {
    return getImpl().MakeIterator;
  }

  auto proto = getProtocol(KnownProtocolKind::Sequence);
  if (!proto)
    return nullptr;

  if (auto *func = lookupRequirement(proto, Id_makeIterator)) {
    getImpl().MakeIterator = func;
    return func;
  }

  return nullptr;
}

FuncDecl *ASTContext::getAsyncSequenceMakeAsyncIterator() const {
  if (getImpl().MakeAsyncIterator) {
    return getImpl().MakeAsyncIterator;
  }

  auto proto = getProtocol(KnownProtocolKind::AsyncSequence);
  if (!proto)
    return nullptr;

  if (auto *func = lookupRequirement(proto, Id_makeAsyncIterator)) {
    getImpl().MakeAsyncIterator = func;
    return func;
  }

  return nullptr;
}

FuncDecl *ASTContext::getIteratorNext() const {
  if (getImpl().IteratorNext) {
    return getImpl().IteratorNext;
  }

  auto proto = getProtocol(KnownProtocolKind::IteratorProtocol);
  if (!proto)
    return nullptr;

  if (auto *func = lookupRequirement(proto, Id_next)) {
    getImpl().IteratorNext = func;
    return func;
  }

  return nullptr;
}

FuncDecl *ASTContext::getAsyncIteratorNext() const {
  if (getImpl().AsyncIteratorNext) {
    return getImpl().AsyncIteratorNext;
  }

  auto proto = getProtocol(KnownProtocolKind::AsyncIteratorProtocol);
  if (!proto)
    return nullptr;

  if (auto *func = lookupRequirement(proto, Id_next)) {
    getImpl().AsyncIteratorNext = func;
    return func;
  }

  return nullptr;
}

#define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
DECL_CLASS *ASTContext::get##NAME##Decl() const { \
  if (getImpl().NAME##Decl) \
    return getImpl().NAME##Decl; \
  SmallVector<ValueDecl *, 1> results; \
  lookupInSwiftModule(#NAME, results); \
  for (auto result : results) { \
    if (auto type = dyn_cast<DECL_CLASS>(result)) { \
      auto params = type->getGenericParams(); \
      if (NUM_GENERIC_PARAMS == (params == nullptr ? 0 : params->size())) { \
        getImpl().NAME##Decl = type; \
        return type; \
      } \
    } \
  } \
  return nullptr; \
} \
\
Type ASTContext::get##NAME##Type() const { \
  if (!get##NAME##Decl()) \
    return Type(); \
  return get##NAME##Decl()->getDeclaredInterfaceType(); \
}
#include "swift/AST/KnownStdlibTypes.def"

CanType ASTContext::getErrorExistentialType() const {
  if (auto *errorProto = getErrorDecl()) {
    return errorProto->getDeclaredExistentialType()->getCanonicalType();
  } else {
    // Use Builtin.NativeObject just as a stand-in.
    return TheNativeObjectType;
  }
}

ProtocolDecl *ASTContext::getErrorDecl() const {
  return getProtocol(KnownProtocolKind::Error);
}

EnumElementDecl *ASTContext::getOptionalSomeDecl() const {
  if (!getImpl().OptionalSomeDecl)
    getImpl().OptionalSomeDecl = getOptionalDecl()->getUniqueElement(/*hasVal*/true);
  return getImpl().OptionalSomeDecl;
}

EnumElementDecl *ASTContext::getOptionalNoneDecl() const {
  if (!getImpl().OptionalNoneDecl)
    getImpl().OptionalNoneDecl =getOptionalDecl()->getUniqueElement(/*hasVal*/false);
  return getImpl().OptionalNoneDecl;
}

TypeAliasDecl *ASTContext::getVoidDecl() const {
  if (getImpl().VoidDecl) {
    return getImpl().VoidDecl;
  }

  SmallVector<ValueDecl *, 1> results;
  lookupInSwiftModule("Void", results);
  for (auto result : results) {
    if (auto typealias = dyn_cast<TypeAliasDecl>(result)) {
      getImpl().VoidDecl = typealias;
      return typealias;
    }
  }

  return nullptr;
}

Type ASTContext::getVoidType() const {
  auto decl = getVoidDecl();
  if (!decl)
    return Type();
  return decl->getDeclaredInterfaceType();
}

static VarDecl *getPointeeProperty(VarDecl *&cache,
                           NominalTypeDecl *(ASTContext::*getNominal)() const,
                                  const ASTContext &ctx) {
  if (cache) return cache;

  // There must be a generic type with one argument.
  NominalTypeDecl *nominal = (ctx.*getNominal)();
  if (!nominal) return nullptr;
  auto sig = nominal->getGenericSignature();
  if (sig.getGenericParams().size() != 1) return nullptr;

  // There must be a property named "pointee".
  auto identifier = ctx.getIdentifier("pointee");
  auto results = nominal->lookupDirect(identifier);
  if (results.size() != 1) return nullptr;

  // The property must have type T.
  auto *property = dyn_cast<VarDecl>(results[0]);
  if (!property) return nullptr;
  if (!property->getInterfaceType()->isEqual(sig.getGenericParams()[0]))
    return nullptr;

  cache = property;
  return property;
}

VarDecl *
ASTContext::getPointerPointeePropertyDecl(PointerTypeKind ptrKind) const {
  switch (ptrKind) {
  case PTK_UnsafeMutableRawPointer:
    return getPointeeProperty(getImpl().UnsafeMutableRawPointerMemoryDecl,
                             &ASTContext::getUnsafeMutableRawPointerDecl,
                             *this);
  case PTK_UnsafeRawPointer:
    return getPointeeProperty(getImpl().UnsafeRawPointerMemoryDecl,
                             &ASTContext::getUnsafeRawPointerDecl,
                             *this);
  case PTK_UnsafeMutablePointer:
    return getPointeeProperty(getImpl().UnsafeMutablePointerMemoryDecl,
                             &ASTContext::getUnsafeMutablePointerDecl,
                             *this);
  case PTK_UnsafePointer:
    return getPointeeProperty(getImpl().UnsafePointerMemoryDecl,
                             &ASTContext::getUnsafePointerDecl,
                             *this);
  case PTK_AutoreleasingUnsafeMutablePointer:
    return getPointeeProperty(getImpl().AutoreleasingUnsafeMutablePointerMemoryDecl,
                         &ASTContext::getAutoreleasingUnsafeMutablePointerDecl,
                             *this);
  }
  llvm_unreachable("bad pointer kind");
}

CanType ASTContext::getAnyExistentialType() const {
  return ExistentialType::get(TheAnyType)->getCanonicalType();
}

CanType ASTContext::getAnyObjectConstraint() const {
  if (getImpl().AnyObjectType) {
    return getImpl().AnyObjectType;
  }

  getImpl().AnyObjectType = CanType(
    ProtocolCompositionType::get(
      *this, {}, /*HasExplicitAnyObject=*/true));
  return getImpl().AnyObjectType;
}

CanType ASTContext::getAnyObjectType() const {
  return ExistentialType::get(getAnyObjectConstraint())
      ->getCanonicalType();
}

#define KNOWN_SDK_TYPE_DECL(MODULE, NAME, DECLTYPE, GENERIC_ARGS) \
DECLTYPE *ASTContext::get##NAME##Decl() const { \
  if (!getImpl().NAME##Decl) { \
    if (ModuleDecl *M = getLoadedModule(Id_##MODULE)) { \
      /* Note: lookupQualified() will search both the Swift overlay \
       * and the Clang module it imports. */ \
      SmallVector<ValueDecl *, 1> decls; \
      M->lookupQualified(M, DeclNameRef(getIdentifier(#NAME)), NL_OnlyTypes, \
                         decls); \
      if (decls.size() == 1 && isa<DECLTYPE>(decls[0])) { \
        auto decl = cast<DECLTYPE>(decls[0]); \
        if (isa<ProtocolDecl>(decl) \
            || (bool)decl->getGenericParams() == (bool)GENERIC_ARGS) { \
          getImpl().NAME##Decl = decl; \
        } \
      } \
    } \
  } \
  \
  return getImpl().NAME##Decl; \
} \
\
Type ASTContext::get##NAME##Type() const { \
  auto *decl = get##NAME##Decl(); \
  if (!decl) \
    return Type(); \
  return decl->getDeclaredInterfaceType(); \
}

#include "swift/AST/KnownSDKTypes.def"

ProtocolDecl *ASTContext::getProtocol(KnownProtocolKind kind) const {
  // Check whether we've already looked for and cached this protocol.
  unsigned index = (unsigned)kind;
  assert(index < NumKnownProtocols && "Number of known protocols is wrong");
  if (getImpl().KnownProtocols[index])
    return getImpl().KnownProtocols[index];

  // Find all of the declarations with this name in the appropriate module.
  SmallVector<ValueDecl *, 1> results;

  const ModuleDecl *M;
  switch (kind) {
  case KnownProtocolKind::BridgedNSError:
  case KnownProtocolKind::BridgedStoredNSError:
  case KnownProtocolKind::ErrorCodeProtocol:
    M = getLoadedModule(Id_Foundation);
    break;
  case KnownProtocolKind::CFObject:
    M = getLoadedModule(Id_CoreFoundation);
    break;
  case KnownProtocolKind::Differentiable:
    M = getLoadedModule(Id_Differentiation);
    break;
  case KnownProtocolKind::Actor:
  case KnownProtocolKind::GlobalActor:
  case KnownProtocolKind::AsyncSequence:
  case KnownProtocolKind::AsyncIteratorProtocol:
  case KnownProtocolKind::Executor:
  case KnownProtocolKind::SerialExecutor:
    M = getLoadedModule(Id_Concurrency);
    break;
  case KnownProtocolKind::DistributedActor:
  case KnownProtocolKind::DistributedActorSystem:
  case KnownProtocolKind::DistributedTargetInvocationEncoder:
  case KnownProtocolKind::DistributedTargetInvocationDecoder:
  case KnownProtocolKind::DistributedTargetInvocationResultHandler:
    M = getLoadedModule(Id_Distributed);
    break;
  case KnownProtocolKind::CxxConvertibleToCollection:
  case KnownProtocolKind::CxxDictionary:
  case KnownProtocolKind::CxxPair:
  case KnownProtocolKind::CxxOptional:
  case KnownProtocolKind::CxxRandomAccessCollection:
  case KnownProtocolKind::CxxSet:
  case KnownProtocolKind::CxxSequence:
  case KnownProtocolKind::UnsafeCxxInputIterator:
  case KnownProtocolKind::UnsafeCxxRandomAccessIterator:
    M = getLoadedModule(Id_Cxx);
    break;
  default:
    M = getStdlibModule();
    break;
  }

  if (!M)
    return nullptr;
  M->lookupValue(getIdentifier(getProtocolName(kind)),
                 NLKind::UnqualifiedLookup,
                 ModuleLookupFlags::ExcludeMacroExpansions,
                 results);

  for (auto result : results) {
    if (auto protocol = dyn_cast<ProtocolDecl>(result)) {
      getImpl().KnownProtocols[index] = protocol;
      return protocol;
    }
  }

  return nullptr;
}

/// Find the implementation for the given "intrinsic" library function,
/// in the passed in module.
static FuncDecl *findLibraryIntrinsic(const ASTContext &ctx,
                                      ModuleDecl *M,
                                      StringRef name) {
  SmallVector<ValueDecl *, 1> results;
  ctx.lookupInModule(M, name, results);
  if (results.size() == 1)
    return dyn_cast_or_null<FuncDecl>(results.front());
  return nullptr;
}

/// Find the implementation for the given "intrinsic" library function.
static FuncDecl *findLibraryIntrinsic(const ASTContext &ctx,
                                      StringRef name) {
  return findLibraryIntrinsic(ctx, ctx.getStdlibModule(), name);
}

/// Returns the type of an intrinsic function if it is not generic, otherwise
/// returns nullptr.
static FunctionType *
getIntrinsicCandidateType(FuncDecl *fn, bool allowTypeMembers) {
  auto type = fn->getInterfaceType();
  if (allowTypeMembers && fn->getDeclContext()->isTypeContext()) {
    auto fnType = type->getAs<FunctionType>();
    if (!fnType) return nullptr;

    type = fnType->getResult();
  }
  return type->getAs<FunctionType>();
}

/// Check whether the given type is Builtin.Int1.
static bool isBuiltinInt1Type(Type type) {
  if (auto intType = type->getAs<BuiltinIntegerType>())
    return intType->isFixedWidth() && intType->getFixedWidth() == 1;
  return false;
}

/// Check whether the given type is Builtin.Word.
static bool isBuiltinWordType(Type type) {
  if (auto intType = type->getAs<BuiltinIntegerType>())
    return intType->getWidth().isPointerWidth();
  return false;
}

/// Looks up all implementations of an operator (globally and declared in types)
/// and passes potential matches to the given callback. The search stops when
/// the predicate returns true (in which case the matching function declaration
/// is returned); otherwise, nullptr is returned if there are no matches.
/// \p C The AST context.
/// \p oper The name of the operator.
/// \p contextType If the operator is declared on a type, then only operators
///     defined on this type should be considered.
/// \p pred A callback predicate that takes as its argument the type of a
///     candidate function declaration and returns true if the function matches
///     the desired criteria.
/// \return The matching function declaration, or nullptr if there was no match.
static FuncDecl *
lookupOperatorFunc(const ASTContext &ctx, StringRef oper, Type contextType,
                   llvm::function_ref<bool(FunctionType *)> pred) {
  SmallVector<ValueDecl *, 32> candidates;
  ctx.lookupInSwiftModule(oper, candidates);

  for (auto candidate : candidates) {
    // All operator declarations should be functions, but make sure.
    auto *fnDecl = dyn_cast<FuncDecl>(candidate);
    if (!fnDecl)
      continue;

    if (fnDecl->getDeclContext()->isTypeContext()) {
      auto contextTy = fnDecl->getDeclContext()->getDeclaredInterfaceType();
      if (!contextTy->isEqual(contextType)) continue;
    }

    auto *funcTy = getIntrinsicCandidateType(fnDecl, /*allowTypeMembers=*/true);
    if (!funcTy)
      continue;

    if (pred(funcTy))
      return fnDecl;
  }

  return nullptr;
}

ConcreteDeclRef ASTContext::getBoolBuiltinInitDecl() const {
  auto fn = [&](ASTContext &ctx) {
    return DeclName(ctx, DeclBaseName::createConstructor(),
                    { Id_builtinBooleanLiteral });
  };
  auto builtinProtocolKind =
    KnownProtocolKind::ExpressibleByBuiltinBooleanLiteral;
  return getBuiltinInitDecl(getBoolDecl(), builtinProtocolKind, fn);
}

ConcreteDeclRef
ASTContext::getIntBuiltinInitDecl(NominalTypeDecl *intDecl) const {
  auto fn = [&](ASTContext &ctx) {
    return DeclName(ctx, DeclBaseName::createConstructor(),
                    { Id_builtinIntegerLiteral });
  };
  auto builtinProtocolKind =
    KnownProtocolKind::ExpressibleByBuiltinIntegerLiteral;
  return getBuiltinInitDecl(intDecl, builtinProtocolKind, fn);
}

ConcreteDeclRef
ASTContext::getFloatBuiltinInitDecl(NominalTypeDecl *floatDecl) const {
  auto fn = [&](ASTContext &ctx) {
    return DeclName(ctx, DeclBaseName::createConstructor(),
                    { Id_builtinFloatLiteral });
  };

  auto builtinProtocolKind =
    KnownProtocolKind::ExpressibleByBuiltinFloatLiteral;
  return getBuiltinInitDecl(floatDecl, builtinProtocolKind, fn);
}

ConcreteDeclRef
ASTContext::getStringBuiltinInitDecl(NominalTypeDecl *stringDecl) const {
  auto fn = [&](ASTContext &ctx) {
    return DeclName(ctx, DeclBaseName::createConstructor(),
                    { Id_builtinStringLiteral,
                      getIdentifier("utf8CodeUnitCount"),
                      getIdentifier("isASCII") });
  };

  auto builtinProtocolKind =
    KnownProtocolKind::ExpressibleByBuiltinStringLiteral;
  return getBuiltinInitDecl(stringDecl, builtinProtocolKind, fn);
}

ConcreteDeclRef
ASTContext::getBuiltinInitDecl(NominalTypeDecl *decl,
                               KnownProtocolKind builtinProtocolKind,
               llvm::function_ref<DeclName (ASTContext &ctx)> initName) const {
  auto &witness = getImpl().BuiltinInitWitness[decl];
  if (witness)
    return witness;

  auto type = decl->getDeclaredInterfaceType();
  auto builtinProtocol = getProtocol(builtinProtocolKind);
  auto builtinConformance = getStdlibModule()->lookupConformance(
      type, builtinProtocol);
  if (builtinConformance.isInvalid()) {
    assert(false && "Missing required conformance");
    witness = ConcreteDeclRef();
    return witness;
  }

  auto *ctx = const_cast<ASTContext *>(this);
  witness = builtinConformance.getWitnessByName(type, initName(*ctx));
  if (!witness) {
    assert(false && "Missing required witness");
    witness = ConcreteDeclRef();
    return witness;
  }

  return witness;
}

ConcreteDeclRef ASTContext::getRegexInitDecl(Type regexType) const {
  auto *spModule = getLoadedModule(Id_StringProcessing);
  DeclName name(*const_cast<ASTContext *>(this),
                DeclBaseName::createConstructor(),
                {Id_regexString, Id_version});
  SmallVector<ValueDecl *, 1> results;
  spModule->lookupQualified(getRegexType(), DeclNameRef(name),
                            NL_IncludeUsableFromInline, results);
  assert(results.size() == 1);
  auto *foundDecl = cast<ConstructorDecl>(results[0]);
  auto subs = regexType->getMemberSubstitutionMap(spModule, foundDecl);
  return ConcreteDeclRef(foundDecl, subs);
}

static
FuncDecl *getBinaryComparisonOperatorIntDecl(const ASTContext &C, StringRef op,
                                             FuncDecl *&cached) {
  if (cached)
    return cached;

  if (!C.getIntDecl() || !C.getBoolDecl())
    return nullptr;

  auto isIntParam = [&](AnyFunctionType::Param param) {
    return (!param.isVariadic() && !param.isInOut() &&
            param.getPlainType()->isInt());
  };

  auto decl = lookupOperatorFunc(C, op, C.getIntType(),
                                 [=](FunctionType *type) {
    // Check for the signature: (Int, Int) -> Bool
    if (type->getParams().size() != 2) return false;
    if (!isIntParam(type->getParams()[0]) ||
        !isIntParam(type->getParams()[1])) return false;
    return type->getResult()->isBool();
  });
  cached = decl;
  return decl;
}
FuncDecl *ASTContext::getLessThanIntDecl() const {
  return getBinaryComparisonOperatorIntDecl(*this, "<", getImpl().LessThanIntDecl);
}
FuncDecl *ASTContext::getEqualIntDecl() const {
  return getBinaryComparisonOperatorIntDecl(*this, "==", getImpl().EqualIntDecl);
}

FuncDecl *ASTContext::getMakeInvocationEncoderOnDistributedActorSystem(
    AbstractFunctionDecl *thunk) const {
  auto systemTy = getConcreteReplacementForProtocolActorSystemType(thunk);
  assert(systemTy && "No specific ActorSystem type found!");

  auto systemNominal = systemTy->getNominalOrBoundGenericNominal();
  assert(systemNominal && "No system nominal type found!");

  for (auto result : systemNominal->lookupDirect(Id_makeInvocationEncoder)) {
    auto *func = dyn_cast<FuncDecl>(result);
    if (func && func->isDistributedActorSystemMakeInvocationEncoder()) {
      return func;
    }
  }

  return nullptr;
}

FuncDecl *
ASTContext::getRecordGenericSubstitutionOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) const {
  for (auto result : nominal->lookupDirect(Id_recordGenericSubstitution)) {
    auto *func = dyn_cast<FuncDecl>(result);
    if (func &&
        func->isDistributedTargetInvocationEncoderRecordGenericSubstitution()) {
      return func;
    }
  }

  return nullptr;
}

AbstractFunctionDecl *ASTContext::getRecordArgumentOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) const {
  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordArgumentFunctionRequest{nominal},
      nullptr);
}

AbstractFunctionDecl *ASTContext::getRecordReturnTypeOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) const {
  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordReturnTypeFunctionRequest{nominal},
      nullptr);
}

AbstractFunctionDecl *ASTContext::getRecordErrorTypeOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) const {
  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationEncoderRecordErrorTypeFunctionRequest{nominal},
      nullptr);
}

AbstractFunctionDecl *ASTContext::getDecodeNextArgumentOnDistributedInvocationDecoder(
    NominalTypeDecl *nominal) const {
  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationDecoderDecodeNextArgumentFunctionRequest{nominal},
      nullptr);
}

AbstractFunctionDecl *ASTContext::getOnReturnOnDistributedTargetInvocationResultHandler(
    NominalTypeDecl *nominal) const {
  return evaluateOrDefault(
      nominal->getASTContext().evaluator,
      GetDistributedTargetInvocationResultHandlerOnReturnFunctionRequest{nominal},
      nullptr);
}

FuncDecl *ASTContext::getDoneRecordingOnDistributedInvocationEncoder(
    NominalTypeDecl *nominal) const {
  for (auto result : nominal->lookupDirect(Id_doneRecording)) {
    auto *fd = dyn_cast<FuncDecl>(result);
    if (!fd)
      continue;

    if (fd->getParameters()->size() != 0)
      continue;

    if (fd->getResultInterfaceType()->isVoid() &&
        fd->hasThrows() &&
        !fd->hasAsync())
      return fd;
  }

  return nullptr;
}

FuncDecl *ASTContext::getHashValueForDecl() const {
  if (getImpl().HashValueForDecl)
    return getImpl().HashValueForDecl;

  SmallVector<ValueDecl *, 1> results;
  lookupInSwiftModule("_hashValue", results);
  for (auto result : results) {
    auto *fd = dyn_cast<FuncDecl>(result);
    if (!fd)
      continue;
    auto paramList = fd->getParameters();
    if (paramList->size() != 1)
      continue;
    auto paramDecl = paramList->get(0);
    if (paramDecl->getArgumentName() != Id_for)
      continue;
    auto genericParams = fd->getGenericParams();
    if (!genericParams || genericParams->size() != 1)
      continue;
    getImpl().HashValueForDecl = fd;
    return fd;
  }
  return nullptr;
}

FuncDecl *ASTContext::getArrayAppendElementDecl() const {
  if (getImpl().ArrayAppendElementDecl)
    return getImpl().ArrayAppendElementDecl;

  auto AppendFunctions = getArrayDecl()->lookupDirect(getIdentifier("append"));

  for (auto CandidateFn : AppendFunctions) {
    auto FnDecl = dyn_cast<FuncDecl>(CandidateFn);
    auto Attrs = FnDecl->getAttrs();
    for (auto *A : Attrs.getAttributes<SemanticsAttr, false>()) {
      if (A->Value != "array.append_element")
        continue;

      auto SelfDecl = FnDecl->getImplicitSelfDecl();
      if (!SelfDecl->isInOut())
        return nullptr;

      auto SelfInOutTy = SelfDecl->getInterfaceType();

      if (!SelfInOutTy->isArray())
        return nullptr;

      auto ParamList = FnDecl->getParameters();
      if (ParamList->size() != 1)
        return nullptr;

      GenericTypeParamType *ElementType = ParamList->get(0)->
                             getInterfaceType()->getAs<GenericTypeParamType>();
      if (!ElementType)
        return nullptr;
      if (ElementType->getName() != getIdentifier("Element"))
        return nullptr;

      if (!FnDecl->getResultInterfaceType()->isVoid())
        return nullptr;

      getImpl().ArrayAppendElementDecl = FnDecl;
      return FnDecl;
    }
  }
  return nullptr;
}

FuncDecl *ASTContext::getArrayReserveCapacityDecl() const {
  if (getImpl().ArrayReserveCapacityDecl)
    return getImpl().ArrayReserveCapacityDecl;

  auto ReserveFunctions = getArrayDecl()->lookupDirect(
                                   getIdentifier("reserveCapacityForAppend"));

  for (auto CandidateFn : ReserveFunctions) {
    auto FnDecl = dyn_cast<FuncDecl>(CandidateFn);
    auto Attrs = FnDecl->getAttrs();
    for (auto *A : Attrs.getAttributes<SemanticsAttr, false>()) {
      if (A->Value != "array.reserve_capacity_for_append")
        continue;

      auto SelfDecl = FnDecl->getImplicitSelfDecl();
      if (!SelfDecl->isInOut())
        return nullptr;

      auto SelfInOutTy = SelfDecl->getInterfaceType();

      if (!SelfInOutTy->isArray())
        return nullptr;

      auto ParamList = FnDecl->getParameters();
      if (ParamList->size() != 1)
        return nullptr;
      StructType *IntType =
        ParamList->get(0)->getInterfaceType()->getAs<StructType>();
      if (!IntType)
        return nullptr;

      StructDecl *IntDecl = IntType->getDecl();
      auto StoredProperties = IntDecl->getStoredProperties();
      if (StoredProperties.size() != 1)
        return nullptr;
      VarDecl *field = StoredProperties[0];
      if (field->hasClangNode())
        return nullptr;
      if (!field->getInterfaceType()->is<BuiltinIntegerType>())
        return nullptr;

      if (!FnDecl->getResultInterfaceType()->isVoid())
        return nullptr;

      getImpl().ArrayReserveCapacityDecl = FnDecl;
      return FnDecl;
    }
  }
  return nullptr;
}

ConstructorDecl *ASTContext::getMakeUTF8StringDecl() const {
  if (getImpl().MakeUTF8StringDecl)
    return getImpl().MakeUTF8StringDecl;

  auto initializers =
    getStringDecl()->lookupDirect(DeclBaseName::createConstructor());

  for (Decl *initializer : initializers) {
    auto *constructor = cast<ConstructorDecl>(initializer);
    auto Attrs = constructor->getAttrs();
    for (auto *A : Attrs.getAttributes<SemanticsAttr, false>()) {
      if (A->Value != semantics::STRING_MAKE_UTF8)
        continue;
      auto ParamList = constructor->getParameters();
      if (ParamList->size() != 3)
        continue;
      ParamDecl *param = constructor->getParameters()->get(0);
      if (param->getArgumentName().str() != "_builtinStringLiteral")
        continue;

      getImpl().MakeUTF8StringDecl = constructor;
      return constructor;
    }
  }
  return nullptr;
}

FuncDecl *ASTContext::getIsOSVersionAtLeastDecl() const {
  if (getImpl().IsOSVersionAtLeastDecl)
    return getImpl().IsOSVersionAtLeastDecl;

  // Look for the function.
  auto decl =
      findLibraryIntrinsic(*this, "_stdlib_isOSVersionAtLeast");
  if (!decl)
    return nullptr;

  auto *fnType = getIntrinsicCandidateType(decl, /*allowTypeMembers=*/false);
  if (!fnType)
    return nullptr;

  // Input must be (Builtin.Word, Builtin.Word, Builtin.Word)
  auto intrinsicsParams = fnType->getParams();
  if (intrinsicsParams.size() != 3)
    return nullptr;

  if (llvm::any_of(intrinsicsParams, [](AnyFunctionType::Param param) {
    return (param.isVariadic() || param.isInOut() ||
            !isBuiltinWordType(param.getPlainType()));
  })) {
    return nullptr;
  }

  // Output must be Builtin.Int1
  if (!isBuiltinInt1Type(fnType->getResult()))
    return nullptr;

  getImpl().IsOSVersionAtLeastDecl = decl;
  return decl;
}

static bool isHigherPrecedenceThan(PrecedenceGroupDecl *a,
                                   PrecedenceGroupDecl *b) {
  assert(a != b && "exact match should already have been filtered");

  SmallVector<PrecedenceGroupDecl*, 4> stack;

  // Compute the transitive set of precedence groups that are
  // explicitly lower than 'b', including 'b' itself.  This is expected
  // to be very small, since it's only legal in downstream modules.
  SmallPtrSet<PrecedenceGroupDecl*, 4> targets;
  targets.insert(b);
  stack.push_back(b);
  do {
    auto cur = stack.pop_back_val();
    for (auto &rel : cur->getLowerThan()) {
      auto group = rel.Group;

      // If we ever see 'a', we're done.
      if (group == a) return true;

      // Protect against invalid ASTs where the group isn't actually set.
      if (!group) continue;

      // If we've already inserted this, don't add it to the queue.
      if (!targets.insert(group).second) continue;

      stack.push_back(group);
    }
  } while (!stack.empty());

  // Walk down the higherThan relationships from 'a' and look for
  // anything in the set we just built.
  stack.push_back(a);
  do {
    auto cur = stack.pop_back_val();
    assert(!targets.count(cur));

    for (auto &rel : cur->getHigherThan()) {
      auto group = rel.Group;

      if (!group) continue;

      // If we ever see a group that's in the targets set, we're done.
      if (targets.count(group)) return true;

      stack.push_back(group);
    }
  } while (!stack.empty());

  return false;
}

static Associativity computeAssociativity(AssociativityCacheType &cache,
                                          PrecedenceGroupDecl *left,
                                          PrecedenceGroupDecl *right) {
  auto it = cache.find({left, right});
  if (it != cache.end()) return it->second;

  auto result = Associativity::None;
  if (isHigherPrecedenceThan(left, right))
    result = Associativity::Left;
  else if (isHigherPrecedenceThan(right, left))
    result = Associativity::Right;
  cache.insert({{left, right}, result});
  return result;
}

Associativity
ASTContext::associateInfixOperators(PrecedenceGroupDecl *left,
                                    PrecedenceGroupDecl *right) const {
  // If the operators are in the same precedence group, use the group's
  // associativity.
  if (left == right) {
    return left->getAssociativity();
  }

  // This relationship is antisymmetric, so we can canonicalize to avoid
  // computing it twice.  Arbitrarily, if the pointer value of 'left'
  // is greater than the pointer value of 'right', we flip them and
  // then flip the result.

  if (uintptr_t(left) < uintptr_t(right)) {
    return computeAssociativity(getImpl().AssociativityCache, left, right);
  }

  switch (computeAssociativity(getImpl().AssociativityCache, right, left)) {
  case Associativity::Left: return Associativity::Right;
  case Associativity::Right: return Associativity::Left;
  case Associativity::None: return Associativity::None;
  }
  llvm_unreachable("bad associativity");
}

// Find library intrinsic function.
static FuncDecl *findLibraryFunction(const ASTContext &ctx, FuncDecl *&cache,
                                     StringRef name) {
  if (cache) return cache;

  // Look for a generic function.
  cache = findLibraryIntrinsic(ctx, name);
  return cache;
}

// Find library intrinsic function in passed in module
static FuncDecl *findLibraryFunction(const ASTContext &ctx,
                                     ModuleDecl *M, FuncDecl *&cache,
                                     StringRef name) {
  if (cache) return cache;

  // Look for a generic function.
  cache = findLibraryIntrinsic(ctx, M, name);
  return cache;
}

#define FUNC_DECL(Name, Id)                                    \
FuncDecl *ASTContext::get##Name() const {                      \
  return findLibraryFunction(*this, getImpl().Get##Name, Id);  \
}
#include "swift/AST/KnownDecls.def"

#define KNOWN_SDK_FUNC_DECL(Module, Name, Id)                                \
FuncDecl *ASTContext::get##Name() const {                                    \
  if (ModuleDecl *M = getLoadedModule(Id_##Module)) {                        \
    return findLibraryFunction(*this, M, getImpl().Get##Name, Id);           \
  } else {                                                                   \
    return findLibraryFunction(*this, getImpl().Get##Name, Id);              \
  }                                                                          \
}
#include "swift/AST/KnownSDKDecls.def"

bool ASTContext::hasOptionalIntrinsics() const {
  return getOptionalDecl() &&
         getOptionalSomeDecl() &&
         getOptionalNoneDecl() &&
         getDiagnoseUnexpectedNilOptional();
}

bool ASTContext::hasPointerArgumentIntrinsics() const {
  return getUnsafeMutableRawPointerDecl()
    && getUnsafeRawPointerDecl()
    && getUnsafeMutablePointerDecl()
    && getUnsafePointerDecl()
    && (!LangOpts.EnableObjCInterop || getAutoreleasingUnsafeMutablePointerDecl())
    && getUnsafeBufferPointerDecl()
    && getUnsafeMutableBufferPointerDecl()
    && getUnsafeRawBufferPointerDecl()
    && getUnsafeMutableRawBufferPointerDecl()
    && getConvertPointerToPointerArgument()
    && getConvertMutableArrayToPointerArgument()
    && getConvertConstArrayToPointerArgument()
    && getConvertConstStringToUTF8PointerArgument()
    && getConvertInOutToPointerArgument();
}

bool ASTContext::hasArrayLiteralIntrinsics() const {
  return getArrayDecl()
    && getAllocateUninitializedArray()
    && getDeallocateUninitializedArray();
}

void ASTContext::addCleanup(std::function<void(void)> cleanup) {
  getImpl().Cleanups.push_back(std::move(cleanup));
}

bool ASTContext::hadError() const {
  return Diags.hadAnyError();
}

/// Retrieve the arena from which we should allocate storage for a type.
static AllocationArena getArena(RecursiveTypeProperties properties) {
  return properties.isSolverAllocated() ? AllocationArena::ConstraintSolver
                                        : AllocationArena::Permanent;
}

void ASTContext::addSearchPath(StringRef searchPath, bool isFramework,
                               bool isSystem) {
  OptionSet<SearchPathKind> &loaded = getImpl().SearchPathsSet[searchPath];
  auto kind = isFramework ? SearchPathKind::Framework : SearchPathKind::Import;
  if (loaded.contains(kind))
    return;
  loaded |= kind;

  if (isFramework) {
    SearchPathOpts.addFrameworkSearchPath({searchPath, isSystem},
                                          SourceMgr.getFileSystem().get());
  } else {
    SearchPathOpts.addImportSearchPath(searchPath,
                                       SourceMgr.getFileSystem().get());
  }

  if (auto *clangLoader = getClangModuleLoader())
    clangLoader->addSearchPath(searchPath, isFramework, isSystem);
}

void ASTContext::addModuleLoader(std::unique_ptr<ModuleLoader> loader,
                                 bool IsClang, bool IsDwarf, bool IsInterface) {
  if (IsClang && !IsDwarf && !getImpl().TheClangModuleLoader)
    getImpl().TheClangModuleLoader =
        static_cast<ClangModuleLoader *>(loader.get());
  if (IsClang && IsDwarf && !getImpl().TheDWARFModuleLoader)
    getImpl().TheDWARFModuleLoader =
        static_cast<ClangModuleLoader *>(loader.get());
  getImpl().ModuleLoaders.push_back(std::move(loader));
}

void ASTContext::addModuleInterfaceChecker(
    std::unique_ptr<ModuleInterfaceChecker> checker) {
  assert(!getImpl().InterfaceChecker && "Checker has been set already");
  getImpl().InterfaceChecker = std::move(checker);
}

void ASTContext::setModuleAliases(const llvm::StringMap<StringRef> &aliasMap) {
  // This setter should be called only once after ASTContext has been initialized
  assert(ModuleAliasMap.empty());
  
  for (auto k: aliasMap.keys()) {
    auto v = aliasMap.lookup(k);
    if (!v.empty()) {
      auto key = getIdentifier(k);
      auto val = getIdentifier(v);
      // key is a module alias, val is its corresponding real name
      ModuleAliasMap[key] = std::make_pair(val, true);
      // add an entry with an alias as key for an easier lookup later
      ModuleAliasMap[val] = std::make_pair(key, false);
    }
  }
}

Identifier ASTContext::getRealModuleName(Identifier key, ModuleAliasLookupOption option) const {
  auto found = ModuleAliasMap.find(key);
  if (found == ModuleAliasMap.end())
    return key; // No module aliasing was used, so just return the given key

  // Found an entry
  auto value = found->second;

  // With the alwaysRealName option, look up the real name by treating
  // the given key as an alias; if the key's not an alias, return the key
  // itself since that's the real name.
  if (option == ModuleAliasLookupOption::alwaysRealName) {
     return value.second ? value.first : key;
  }

  // With realNameFromAlias or aliasFromRealName option, only return the value
  // if the given key matches the description (whether it's an alias or real name)
  // by looking up the value.second (true if keyed by an alias). If not matched,
  // return an empty Identifier.
  if ((option == ModuleAliasLookupOption::realNameFromAlias && !value.second) ||
      (option == ModuleAliasLookupOption::aliasFromRealName && value.second))
      return Identifier();

  // Otherwise return the value found (whether the key is an alias or real name)
  return value.first;
}

using ModuleDependencyIDSet =
    std::unordered_set<ModuleDependencyID,
                       llvm::pair_hash<std::string,
                                       ModuleDependencyKind>>;
static void findPath_dfs(ModuleDependencyID X,
                         ModuleDependencyID Y,
                         ModuleDependencyIDSet &visited,
                         std::vector<ModuleDependencyID> &stack,
                         std::vector<ModuleDependencyID> &result,
                         const ModuleDependenciesCache &cache) {
  stack.push_back(X);
  if (X == Y) {
    copy(stack.begin(), stack.end(), std::back_inserter(result));
    return;
  }
  visited.insert(X);
  auto optionalNode = cache.findDependency(X.first, X.second);
  auto node = optionalNode.value();
  assert(optionalNode.has_value() && "Expected cache value for dependency.");
  for (const auto &dep : node->getModuleImports()) {
    Optional<ModuleDependencyKind> lookupKind = None;
    // Underlying Clang module needs an explicit lookup to avoid confusing it
    // with the parent Swift module.
    if ((dep == X.first && node->isSwiftModule()) || node->isClangModule())
      lookupKind = ModuleDependencyKind::Clang;

    auto optionalDepNode = cache.findDependency(dep, lookupKind);
    if (!optionalDepNode.has_value())
      continue;
    auto depNode = optionalDepNode.value();
    auto depID = std::make_pair(dep, depNode->getKind());
    if (!visited.count(depID)) {
      findPath_dfs(depID, Y, visited, stack, result, cache);
    }
  }
  stack.pop_back();
}

static std::vector<ModuleDependencyID>
findPathToDependency(ModuleDependencyID dependency,
                     const ModuleDependenciesCache &cache) {
  auto mainModuleDep = cache.findDependency(cache.getMainModuleName(),
                                              ModuleDependencyKind::SwiftSource);
  // We may be in a batch scan instance which does not have this dependency
  if (!mainModuleDep.has_value())
    return {};
  auto mainModuleID = std::make_pair(cache.getMainModuleName().str(),
                                     ModuleDependencyKind::SwiftSource);
  auto visited = ModuleDependencyIDSet();
  auto stack = std::vector<ModuleDependencyID>();
  auto dependencyPath = std::vector<ModuleDependencyID>();
  findPath_dfs(mainModuleID, dependency, visited, stack, dependencyPath, cache);
  return dependencyPath;
}

// Diagnose scanner failure and attempt to reconstruct the dependency
// path from the main module to the missing dependency.
static void diagnoseScannerFailure(StringRef moduleName,
                                   DiagnosticEngine &Diags,
                                   const ModuleDependenciesCache &cache,
                                   llvm::Optional<ModuleDependencyID> dependencyOf) {
  Diags.diagnose(SourceLoc(), diag::dependency_scan_module_not_found, moduleName);
  if (dependencyOf.has_value()) {
    auto path = findPathToDependency(dependencyOf.value(), cache);
    // We may fail to construct a path in some cases, such as a Swift overlay of a Clang
    // module dependnecy.
    if (path.empty())
      path = {dependencyOf.value()};
    
    for (auto it = path.rbegin(), end = path.rend(); it != end; ++it) {
      const auto &entry = *it;
      auto optionalEntryNode = cache.findDependency(entry.first, entry.second);
      assert(optionalEntryNode.has_value());
      auto entryNode = optionalEntryNode.value();
      std::string moduleFilePath = "";
      bool isClang = false;
      switch (entryNode->getKind()) {
        case swift::ModuleDependencyKind::SwiftSource:
          Diags.diagnose(SourceLoc(), diag::dependency_as_imported_by_main_module, entry.first);
          continue;
        case swift::ModuleDependencyKind::SwiftInterface:
          moduleFilePath = entryNode->getAsSwiftInterfaceModule()->swiftInterfaceFile;
          break;
        case swift::ModuleDependencyKind::SwiftBinary:
          moduleFilePath = entryNode->getAsSwiftBinaryModule()->compiledModulePath;
          break;
        case swift::ModuleDependencyKind::SwiftPlaceholder:
          moduleFilePath = entryNode->getAsPlaceholderDependencyModule()->compiledModulePath;
          break;
        case swift::ModuleDependencyKind::Clang:
          moduleFilePath = entryNode->getAsClangModule()->moduleMapFile;
          isClang = true;
          break;
        default:
          llvm_unreachable("Unexpected dependency kind");
      }
      
      // TODO: Consider turning the module file (interface or modulemap) into the SourceLoc
      // of this diagnostic instead. Ideally with the location of the `import` statement
      // that this dependency originates from.
      Diags.diagnose(SourceLoc(), diag::dependency_as_imported_by, entry.first, moduleFilePath, isClang);
    }
  }
}

Optional<const ModuleDependencyInfo*> ASTContext::getModuleDependencies(
    StringRef moduleName, ModuleDependenciesCache &cache,
    InterfaceSubContextDelegate &delegate,
    bool optionalDependencyLookup,
    bool isTestableImport,
    llvm::Optional<ModuleDependencyID> dependencyOf) {
  // Retrieve the dependencies for this module.
  // Check whether we've cached this result.
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftSource))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftInterface))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftBinary))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftPlaceholder))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::Clang))
    return found;

  for (auto &loader : getImpl().ModuleLoaders) {
    if (auto dependencies =
        loader->getModuleDependencies(moduleName, cache, delegate,
                                      isTestableImport))
      return dependencies;
  }

  if (!optionalDependencyLookup)
    diagnoseScannerFailure(moduleName, Diags, cache,
                           dependencyOf);
  return None;
}

Optional<const ModuleDependencyInfo*>
ASTContext::getSwiftModuleDependencies(StringRef moduleName,
                                       ModuleDependenciesCache &cache,
                                       InterfaceSubContextDelegate &delegate) {
  // Check whether we've cached this result.
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftSource))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftInterface))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftBinary))
    return found;
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::SwiftPlaceholder))
    return found;

  for (auto &loader : getImpl().ModuleLoaders) {
    if (loader.get() == getImpl().TheClangModuleLoader)
      continue;
    if (auto dependencies = loader->getModuleDependencies(moduleName, cache,
                                                          delegate))
      return dependencies;
  }
  return None;
}

Optional<const ModuleDependencyInfo*>
ASTContext::getClangModuleDependencies(StringRef moduleName,
                                       ModuleDependenciesCache &cache,
                                       InterfaceSubContextDelegate &delegate) {
  // Check whether we've cached this result.
  if (auto found =
      cache.findDependency(moduleName, ModuleDependencyKind::Clang))
    return found;

  for (auto &loader : getImpl().ModuleLoaders) {
    if (loader.get() != getImpl().TheClangModuleLoader)
      continue;
    if (auto dependencies = loader->getModuleDependencies(moduleName, cache,
                                                          delegate))
      return dependencies;
  }
  return None;
}

namespace {
  static StringRef
  pathStringFromFrameworkSearchPath(const SearchPathOptions::FrameworkSearchPath &next) {
    return next.Path;
  }
}

std::vector<std::string> ASTContext::getDarwinImplicitFrameworkSearchPaths()
const {
  assert(LangOpts.Target.isOSDarwin());
  return SearchPathOpts.getDarwinImplicitFrameworkSearchPaths();
}

llvm::StringSet<> ASTContext::getAllModuleSearchPathsSet()
const {
  llvm::StringSet<> result;
  auto ImportSearchPaths = SearchPathOpts.getImportSearchPaths();
  result.insert(ImportSearchPaths.begin(), ImportSearchPaths.end());

  // Framework paths are "special", they contain more than path strings,
  // but path strings are all we care about here.
  using FrameworkPathView = ArrayRefView<SearchPathOptions::FrameworkSearchPath,
                                         StringRef,
                                         pathStringFromFrameworkSearchPath>;
  FrameworkPathView frameworkPathsOnly{
      SearchPathOpts.getFrameworkSearchPaths()};
  result.insert(frameworkPathsOnly.begin(), frameworkPathsOnly.end());

  if (LangOpts.Target.isOSDarwin()) {
    auto implicitFrameworkSearchPaths = getDarwinImplicitFrameworkSearchPaths();
    result.insert(implicitFrameworkSearchPaths.begin(),
                  implicitFrameworkSearchPaths.end());
  }
  result.insert(SearchPathOpts.RuntimeLibraryImportPaths.begin(),
                SearchPathOpts.RuntimeLibraryImportPaths.end());

  // ClangImporter special-cases the path for SwiftShims, so do the same here
  // If there are no shims in the resource dir, add a search path in the SDK.
  SmallString<128> shimsPath(SearchPathOpts.RuntimeResourcePath);
  llvm::sys::path::append(shimsPath, "shims");
  if (!llvm::sys::fs::exists(shimsPath)) {
    shimsPath = SearchPathOpts.getSDKPath();
    llvm::sys::path::append(shimsPath, "usr", "lib", "swift", "shims");
  }
  result.insert(shimsPath.str());

  // Clang system modules are found in the SDK root
  SmallString<128> clangSysRootPath(SearchPathOpts.getSDKPath());
  llvm::sys::path::append(clangSysRootPath, "usr", "include");
  result.insert(clangSysRootPath.str());
  return result;
}

void ASTContext::loadExtensions(NominalTypeDecl *nominal,
                                unsigned previousGeneration) {
  PrettyStackTraceDecl stackTrace("loading extensions for", nominal);
  for (auto &loader : getImpl().ModuleLoaders) {
    loader->loadExtensions(nominal, previousGeneration);
  }
}

void ASTContext::loadObjCMethods(
    NominalTypeDecl *tyDecl, ObjCSelector selector, bool isInstanceMethod,
    unsigned previousGeneration,
    llvm::TinyPtrVector<AbstractFunctionDecl *> &methods, bool swiftOnly) {
  PrettyStackTraceSelector stackTraceSelector("looking for", selector);
  PrettyStackTraceDecl stackTraceDecl("...in", tyDecl);

  for (auto &loader : getImpl().ModuleLoaders) {
    // Ignore the Clang importer if we've been asked for Swift-only results.
    if (swiftOnly && loader.get() == getClangModuleLoader())
      continue;

    loader->loadObjCMethods(tyDecl, selector, isInstanceMethod,
                            previousGeneration, methods);
  }
}

void ASTContext::loadDerivativeFunctionConfigurations(
    AbstractFunctionDecl *originalAFD, unsigned previousGeneration,
    llvm::SetVector<AutoDiffConfig> &results) {
  PrettyStackTraceDecl stackTrace(
      "loading derivative function configurations for", originalAFD);
  for (auto &loader : getImpl().ModuleLoaders) {
    loader->loadDerivativeFunctionConfigurations(originalAFD,
                                                 previousGeneration, results);
  }
}

unsigned ASTContext::getNextMacroDiscriminator(
    MacroDiscriminatorContext context,
    DeclBaseName baseName
) {
  std::pair<const void *, Identifier> key(
      context.getOpaqueValue(), baseName.getIdentifier());
  return getImpl().NextMacroDiscriminator[key]++;
}

void ASTContext::verifyAllLoadedModules() const {
#ifndef NDEBUG
  FrontendStatsTracer tracer(Stats, "verify-all-loaded-modules");
  for (auto &loader : getImpl().ModuleLoaders)
    loader->verifyAllModules();
#endif
}

swift::namelookup::ImportCache &ASTContext::getImportCache() const {
  return getImpl().TheImportCache;
}

ClangModuleLoader *ASTContext::getClangModuleLoader() const {
  return getImpl().TheClangModuleLoader;
}

ClangModuleLoader *ASTContext::getDWARFModuleLoader() const {
  return getImpl().TheDWARFModuleLoader;
}

ModuleInterfaceChecker *ASTContext::getModuleInterfaceChecker() const {
  auto *result = getImpl().InterfaceChecker.get();
  assert(result);
  return result;
}

ModuleDecl *ASTContext::getLoadedModule(
    ImportPath::Module ModulePath) const {
  assert(!ModulePath.empty());

  // TODO: Swift submodules.
  if (ModulePath.size() == 1) {
    return getLoadedModule(ModulePath[0].Item);
  }
  return nullptr;
}

iterator_range<llvm::MapVector<Identifier, ModuleDecl *>::const_iterator>
ASTContext::getLoadedModules() const {
  return {getImpl().LoadedModules.begin(), getImpl().LoadedModules.end()};
}

ModuleDecl *ASTContext::getLoadedModule(Identifier ModuleName) const {
  // Look up a loaded module using an actual module name (physical name
  // on disk). If the -module-alias option is used, the module name that
  // appears in source code will be different from the real module name
  // on disk, otherwise the same.
  //
  // For example, if '-module-alias Foo=Bar' is passed in to the frontend,
  // and a source file has 'import Foo', a module called Bar (real name)
  // will be loaded and returned.
  auto realName = getRealModuleName(ModuleName);
  return getImpl().LoadedModules.lookup(realName);
}

void ASTContext::addLoadedModule(ModuleDecl *M) {
  assert(M);
  // Add a loaded module using an actual module name (physical name
  // on disk), in case -module-alias is used (otherwise same).
  //
  // For example, if '-module-alias Foo=Bar' is passed in to the frontend,
  // and a source file has 'import Foo', a module called Bar (real name)
  // will be loaded and added to the map.
  getImpl().LoadedModules[M->getRealName()] = M;
}

void ASTContext::setIgnoreAdjacentModules(bool value) {
  IgnoreAdjacentModules = value;
}

rewriting::RewriteContext &
ASTContext::getRewriteContext() {
  auto &rewriteCtx = getImpl().TheRewriteContext;
  if (!rewriteCtx)
    rewriteCtx.reset(new rewriting::RewriteContext(*this));

  return *rewriteCtx;
}

bool ASTContext::isRecursivelyConstructingRequirementMachine(
      CanGenericSignature sig) {
  return getRewriteContext().isRecursivelyConstructingRequirementMachine(sig);
}

bool ASTContext::isRecursivelyConstructingRequirementMachine(
      const ProtocolDecl *proto) {
  return getRewriteContext().isRecursivelyConstructingRequirementMachine(proto);
}

Optional<llvm::TinyPtrVector<ValueDecl *>>
OverriddenDeclsRequest::getCachedResult() const {
  auto decl = std::get<0>(getStorage());
  if (!decl->LazySemanticInfo.hasOverriddenComputed)
    return None;

  // If there are no overridden declarations (the common case), return.
  llvm::TinyPtrVector<ValueDecl *> overridden;
  if (!decl->LazySemanticInfo.hasOverridden) return overridden;

  // Retrieve the set of overrides from the ASTContext.
  ASTContext &ctx = decl->getASTContext();
  auto known = ctx.getImpl().Overrides.find(decl);
  assert(known != ctx.getImpl().Overrides.end());
  overridden.insert(overridden.end(),
                    known->second.begin(), known->second.end());
  return overridden;
}

void OverriddenDeclsRequest::cacheResult(
                                llvm::TinyPtrVector<ValueDecl *> value) const {
  auto decl = std::get<0>(getStorage());
  decl->LazySemanticInfo.hasOverriddenComputed = true;
  decl->LazySemanticInfo.hasOverridden = !value.empty();

  if (value.empty())
    return;

  // Sanity-check the declarations we were given.
  for (auto overriddenDecl : value) {
    assert(overriddenDecl->getKind() == decl->getKind() &&
           "Overridden decl kind mismatch");
    if (auto func = dyn_cast<AbstractFunctionDecl>(overriddenDecl))
      func->setIsOverridden();
  }

  // Record the overrides in the context.
  auto &ctx = decl->getASTContext();
  auto overriddenCopy =
    ctx.AllocateCopy(value.operator ArrayRef<ValueDecl *>());
  (void)ctx.getImpl().Overrides.insert({decl, overriddenCopy});
}

/// Returns the default witness for a requirement, or nullptr if there is
/// no default.
Witness ProtocolDecl::getDefaultWitness(ValueDecl *requirement) const {
  loadAllMembers();

  ASTContext &ctx = getASTContext();
  auto found = ctx.getImpl().DefaultWitnesses.find({this, requirement});
  if (found == ctx.getImpl().DefaultWitnesses.end())
    return Witness();
  return found->second;
}

/// Record the default witness for a requirement.
void ProtocolDecl::setDefaultWitness(ValueDecl *requirement, Witness witness) {
  assert(witness);
  ASTContext &ctx = getASTContext();
  auto pair = ctx.getImpl().DefaultWitnesses.insert(
                std::make_pair(std::make_pair(this, requirement), witness));
  assert(pair.second && "Already have a default witness!");
  (void) pair;
}

/// Returns the default type witness for an associated type, or a null
/// type if there is no default.
Type ProtocolDecl::getDefaultTypeWitness(AssociatedTypeDecl *assocType) const {
  auto &ctx = getASTContext();
  auto found = ctx.getImpl().DefaultTypeWitnesses.find({this, assocType});
  if (found == ctx.getImpl().DefaultTypeWitnesses.end())
    return Type();

  return found->second;
}

/// Set the default type witness for an associated type.
void ProtocolDecl::setDefaultTypeWitness(AssociatedTypeDecl *assocType,
                                         Type witness) {
  assert(witness);
  assert(!witness->hasArchetype() && "Only record interface types");
  ASTContext &ctx = getASTContext();
  auto pair = ctx.getImpl().DefaultTypeWitnesses.insert(
                std::make_pair(std::make_pair(this, assocType), witness));
  assert(pair.second && "Already have a default witness");
  (void)pair;
}

ProtocolConformanceRef ProtocolDecl::getDefaultAssociatedConformanceWitness(
    CanType association, ProtocolDecl *requirement) const {
  auto &ctx = getASTContext();
  auto found =
    ctx.getImpl().DefaultAssociatedConformanceWitnesses.find(
      std::make_tuple(this, association, requirement));
  if (found == ctx.getImpl().DefaultAssociatedConformanceWitnesses.end())
    return ProtocolConformanceRef::forInvalid();

  return found->second;
}

void ProtocolDecl::setDefaultAssociatedConformanceWitness(
                                          CanType association,
                                          ProtocolDecl *requirement,
                                          ProtocolConformanceRef conformance) {
  auto &ctx = getASTContext();
  auto pair = ctx.getImpl().DefaultAssociatedConformanceWitnesses.insert(
                std::make_pair(std::make_tuple(this, association, requirement),
                               conformance));
  assert(pair.second && "Already have a default associated conformance");
  (void)pair;
}

void ASTContext::getVisibleTopLevelModuleNames(
    SmallVectorImpl<Identifier> &names) const {
  names.clear();
  for (auto &importer : getImpl().ModuleLoaders)
    importer->collectVisibleTopLevelModuleNames(names);

  // Sort and unique.
  std::sort(names.begin(), names.end(), [](Identifier LHS, Identifier RHS) {
    return LHS.str().compare_insensitive(RHS.str()) < 0;
  });
  names.erase(std::unique(names.begin(), names.end()), names.end());
}

bool ASTContext::shouldPerformTypoCorrection() {
  NumTypoCorrections += 1;
  return NumTypoCorrections <= LangOpts.TypoCorrectionLimit;
}

static bool isClangModuleVersion(const ModuleLoader::ModuleVersionInfo &info) {
  switch (info.getSourceKind()) {
  case ModuleLoader::ModuleVersionSourceKind::ClangModuleTBD:
    return true;
  case ModuleLoader::ModuleVersionSourceKind::SwiftBinaryModule:
  case ModuleLoader::ModuleVersionSourceKind::SwiftInterface:
    return false;
  }
}

static StringRef
getModuleVersionKindString(const ModuleLoader::ModuleVersionInfo &info) {
  switch (info.getSourceKind()) {
  case ModuleLoader::ModuleVersionSourceKind::ClangModuleTBD:
    return "Clang";
  case ModuleLoader::ModuleVersionSourceKind::SwiftBinaryModule:
  case ModuleLoader::ModuleVersionSourceKind::SwiftInterface:
    return "Swift";
  }
}

bool ASTContext::canImportModuleImpl(ImportPath::Module ModuleName,
                                     llvm::VersionTuple version,
                                     bool underlyingVersion,
                                     bool updateFailingList) const {
  SmallString<64> FullModuleName;
  ModuleName.getString(FullModuleName);
  auto ModuleNameStr = FullModuleName.str();

  // If we've failed loading this module before, don't look for it again.
  if (FailedModuleImportNames.count(ModuleNameStr))
    return false;

  if (version.empty()) {
    // If this module has already been successfully imported, it is importable.
    if (getLoadedModule(ModuleName) != nullptr)
      return true;

    // Otherwise, ask whether any module loader can load the module.
    for (auto &importer : getImpl().ModuleLoaders) {
      if (importer->canImportModule(ModuleName, nullptr))
        return true;
    }

    if (updateFailingList)
      FailedModuleImportNames.insert(ModuleNameStr);

    return false;
  }

  // We need to check whether the version of the module is high enough.
  // Retrieve a module version from each module loader that can find the module
  // and use the best source available for the query.
  ModuleLoader::ModuleVersionInfo bestVersionInfo;
  for (auto &importer : getImpl().ModuleLoaders) {
    ModuleLoader::ModuleVersionInfo versionInfo;

    if (!importer->canImportModule(ModuleName, &versionInfo))
      continue; // The loader can't find the module.

    if (!versionInfo.isValid())
      continue; // The loader didn't attempt to parse a version.

    if (underlyingVersion && !isClangModuleVersion(versionInfo))
      continue; // We're only matching Clang module versions.

    if (bestVersionInfo.isValid() &&
        versionInfo.getSourceKind() <= bestVersionInfo.getSourceKind())
      continue; // This module version's source is lower priority.

    bestVersionInfo = versionInfo;
  }

  if (!bestVersionInfo.isValid())
    return false;

  if (bestVersionInfo.getVersion().empty()) {
    // The module version could not be parsed from the preferred source for
    // this query. Diagnose and treat the query as if it succeeded.
    auto mID = ModuleName[0];
    Diags.diagnose(mID.Loc, diag::cannot_find_project_version,
                   getModuleVersionKindString(bestVersionInfo), mID.Item.str());
    return true;
  }

  return version <= bestVersionInfo.getVersion();
}

bool ASTContext::canImportModule(ImportPath::Module ModuleName,
                                 llvm::VersionTuple version,
                                 bool underlyingVersion) {
  return canImportModuleImpl(ModuleName, version, underlyingVersion, true);
}

bool ASTContext::canImportModule(ImportPath::Module ModuleName,
                                 llvm::VersionTuple version,
                                 bool underlyingVersion) const {
  return canImportModuleImpl(ModuleName, version, underlyingVersion, false);
}

ModuleDecl *
ASTContext::getModule(ImportPath::Module ModulePath, bool AllowMemoryCached) {
  assert(!ModulePath.empty());

  if (AllowMemoryCached)
    if (auto *M = getLoadedModule(ModulePath))
      return M;

  auto moduleID = ModulePath[0];
  if (PreModuleImportCallback)
    PreModuleImportCallback(moduleID.Item.str(), false /*=IsOverlay*/);
  for (auto &importer : getImpl().ModuleLoaders) {
    if (ModuleDecl *M = importer->loadModule(moduleID.Loc, ModulePath,
                                             AllowMemoryCached)) {
      if (LangOpts.EnableModuleLoadingRemarks) {
        Diags.diagnose(ModulePath.getSourceRange().Start,
                       diag::module_loaded,
                       M->getName(),
                       /*overlay=*/false,
                       M->getModuleSourceFilename(),
                       M->getModuleLoadedFilename());
      }
      return M;
    }
  }

  return nullptr;
}

ModuleDecl *ASTContext::getOverlayModule(const FileUnit *FU) {
  assert(FU && FU->getKind() == FileUnitKind::ClangModule &&
         "Overlays can only be retrieved for clang modules!");
  ImportPath::Module::Builder builder(FU->getParentModule()->getName());
  auto ModPath = builder.get();
  if (auto *Existing = getLoadedModule(ModPath)) {
    if (!Existing->isNonSwiftModule())
      return Existing;
  }

  if (PreModuleImportCallback) {
    SmallString<16> path;
    ModPath.getString(path);
    if (!path.empty())
      PreModuleImportCallback(path.str(), /*IsOverlay=*/true);
  }
  for (auto &importer : getImpl().ModuleLoaders) {
    if (importer.get() == getClangModuleLoader())
      continue;
    if (ModuleDecl *M = importer->loadModule(SourceLoc(), ModPath)) {
      if (LangOpts.EnableModuleLoadingRemarks) {
        Diags.diagnose(SourceLoc(),
                       diag::module_loaded,
                       M->getName(),
                       /*overlay=*/true,
                       M->getModuleSourceFilename(),
                       M->getModuleLoadedFilename());
      }
      return M;
    }
  }

  return nullptr;
}

ModuleDecl *ASTContext::getModuleByName(StringRef ModuleName) {
  ImportPath::Module::Builder builder(*this, ModuleName, /*separator=*/'.');
  return getModule(builder.get());
}

ModuleDecl *ASTContext::getModuleByIdentifier(Identifier ModuleID) {
  ImportPath::Module::Builder builder(ModuleID);
  return getModule(builder.get());
}

ModuleDecl *ASTContext::getStdlibModule(bool loadIfAbsent) {
  if (TheStdlibModule)
    return TheStdlibModule;

  if (loadIfAbsent) {
    auto mutableThis = const_cast<ASTContext*>(this);
    TheStdlibModule = mutableThis->getModuleByIdentifier(StdlibModuleName);
  } else {
    TheStdlibModule = getLoadedModule(StdlibModuleName);
  }
  return TheStdlibModule;
}

Optional<ExternalSourceLocs *>
ASTContext::getExternalSourceLocs(const Decl *D) {
  auto Known = getImpl().ExternalSourceLocs.find(D);
  if (Known == getImpl().ExternalSourceLocs.end())
    return None;

  return Known->second;
}

void ASTContext::setExternalSourceLocs(const Decl *D,
                                       ExternalSourceLocs *Locs) {
  getImpl().ExternalSourceLocs[D] = Locs;
}

NormalProtocolConformance *
ASTContext::getConformance(Type conformingType,
                           ProtocolDecl *protocol,
                           SourceLoc loc,
                           DeclContext *dc,
                           ProtocolConformanceState state,
                           bool isUnchecked) {
  assert(dc->isTypeContext());

  llvm::FoldingSetNodeID id;
  NormalProtocolConformance::Profile(id, protocol, dc);

  // Did we already record the normal conformance?
  void *insertPos;
  auto &normalConformances =
    getImpl().getArena(AllocationArena::Permanent).NormalConformances;
  if (auto result = normalConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new normal protocol conformance.
  auto result
    = new (*this, AllocationArena::Permanent)
        NormalProtocolConformance(
          conformingType, protocol, loc, dc, state,isUnchecked);
  normalConformances.InsertNode(result, insertPos);

  return result;
}

/// Produce a self-conformance for the given protocol.
SelfProtocolConformance *
ASTContext::getSelfConformance(ProtocolDecl *protocol) {
  auto &selfConformances =
    getImpl().getArena(AllocationArena::Permanent).SelfConformances;
  auto &entry = selfConformances[protocol];
  if (!entry) {
    entry = new (*this, AllocationArena::Permanent)
      SelfProtocolConformance(protocol->getDeclaredExistentialType());
  }
  return entry;
}

/// Produce the builtin conformance for some non-nominal to some protocol.
BuiltinProtocolConformance *
ASTContext::getBuiltinConformance(
    Type type, ProtocolDecl *protocol,
    GenericSignature genericSig,
    ArrayRef<Requirement> conditionalRequirements,
    BuiltinConformanceKind kind
) {
  auto key = std::make_pair(type, protocol);
  AllocationArena arena = getArena(type->getRecursiveProperties());
  auto &builtinConformances = getImpl().getArena(arena).BuiltinConformances;

  auto &entry = builtinConformances[key];
  if (!entry) {
    auto size = BuiltinProtocolConformance::
        totalSizeToAlloc<Requirement>(conditionalRequirements.size());
    auto mem = this->Allocate(size, alignof(BuiltinProtocolConformance), arena);
    entry = new (mem) BuiltinProtocolConformance(
        type, protocol, genericSig, conditionalRequirements, kind);
  }
  return entry;
}

static bool collapseSpecializedConformance(Type type,
                                           RootProtocolConformance *conformance,
                                           SubstitutionMap substitutions) {
  if (!conformance->getType()->isEqual(type))
    return false;

  for (auto subConformance : substitutions.getConformances()) {
    if (!subConformance.isAbstract())
      return false;
  }

  return true;
}

ProtocolConformance *
ASTContext::getSpecializedConformance(Type type,
                                      RootProtocolConformance *generic,
                                      SubstitutionMap substitutions) {
  // If the specialization is a no-op, use the root conformance instead.
  if (collapseSpecializedConformance(type, generic, substitutions)) {
    ++NumCollapsedSpecializedProtocolConformances;
    return generic;
  }

  llvm::FoldingSetNodeID id;
  SpecializedProtocolConformance::Profile(id, type, generic, substitutions);

  // Figure out which arena this conformance should go into.
  AllocationArena arena = getArena(type->getRecursiveProperties());

  // Did we already record the specialized conformance?
  void *insertPos;
  auto &specializedConformances = getImpl().getArena(arena).SpecializedConformances;
  if (auto result = specializedConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new specialized conformance.
  auto result
    = new (*this, arena) SpecializedProtocolConformance(type, generic,
                                                        substitutions);
  auto node = specializedConformances.FindNodeOrInsertPos(id, insertPos);
  (void)node;
  assert(!node);
  specializedConformances.InsertNode(result, insertPos);
  return result;
}

ProtocolConformance *
ASTContext::getInheritedConformance(Type type, ProtocolConformance *inherited) {
  // Collapse multiple levels of inherited conformance.
  if (auto *otherInherited = dyn_cast<InheritedProtocolConformance>(inherited))
    inherited = otherInherited->getInheritedConformance();

  assert(isa<SpecializedProtocolConformance>(inherited) ||
         isa<NormalProtocolConformance>(inherited) ||
         isa<BuiltinProtocolConformance>(inherited));

  // Collapse useless inherited conformances. Conformance lookup with aa
  // archetype T that has a superclass bound C will return a concrete
  // conformance if C conforms to the protocol P. This is wrapped in an
  // inherited conformance with the archetype type T. If you then substitute
  // T := C, you don't want to form an inherited conformance with a type of
  // C, because the underlying conformance already has a type of C.
  if (inherited->getType()->isEqual(type))
    return inherited;

  llvm::FoldingSetNodeID id;
  InheritedProtocolConformance::Profile(id, type, inherited);

  // Figure out which arena this conformance should go into.
  AllocationArena arena = getArena(type->getRecursiveProperties());

  // Did we already record the inherited protocol conformance?
  void *insertPos;
  auto &inheritedConformances = getImpl().getArena(arena).InheritedConformances;
  if (auto result
        = inheritedConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new inherited protocol conformance.
  auto result = new (*this, arena) InheritedProtocolConformance(type, inherited);
  inheritedConformances.InsertNode(result, insertPos);
  return result;
}

PackConformance *PackConformance::get(PackType *conformingType,
                                      ProtocolDecl *protocol,
                                      ArrayRef<ProtocolConformanceRef> conformances) {
  auto properties = conformingType->getRecursiveProperties();

  for (auto conformance : conformances) {
    if (conformance.isAbstract() || conformance.isInvalid())
      continue;

    auto *concrete = conformance.getConcrete();
    properties |= concrete->getType()->getRecursiveProperties();
  }

  auto &ctx = protocol->getASTContext();

  llvm::FoldingSetNodeID id;
  PackConformance::Profile(id, conformingType, protocol, conformances);

  // Figure out which arena this conformance should go into.
  AllocationArena arena = getArena(properties);

  // Did we already record the pack conformance?
  void *insertPos;
  auto &packConformances = ctx.getImpl().getArena(arena).PackConformances;
  if (auto result = packConformances.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new pack conformance.
  auto size = totalSizeToAlloc<ProtocolConformanceRef>(conformances.size());
  auto mem = ctx.Allocate(size, alignof(PackConformance), arena);

  auto result
    = new (mem) PackConformance(conformingType, protocol,
                                conformances);
  auto node = packConformances.FindNodeOrInsertPos(id, insertPos);
  (void)node;
  assert(!node);
  packConformances.InsertNode(result, insertPos);

  return result;
}

LazyContextData *ASTContext::getOrCreateLazyContextData(
                                                const DeclContext *dc,
                                                LazyMemberLoader *lazyLoader) {
  LazyContextData *&entry = getImpl().LazyContexts[dc];
  if (entry) {
    // Make sure we didn't provide an incompatible lazy loader.
    assert(!lazyLoader || lazyLoader == entry->loader);
    return entry;
  }

  // Create new lazy context data with the given loader.
  assert(lazyLoader && "Queried lazy data for non-lazy iterable context");
  if (isa<ProtocolDecl>(dc))
    entry = Allocate<LazyProtocolData>();
  else {
    assert(isa<NominalTypeDecl>(dc) || isa<ExtensionDecl>(dc));
    entry = Allocate<LazyIterableDeclContextData>();
  }

  entry->loader = lazyLoader;
  return entry;
}

LazyIterableDeclContextData *ASTContext::getOrCreateLazyIterableContextData(
                                            const IterableDeclContext *idc,
                                            LazyMemberLoader *lazyLoader) {
  if (auto ext = dyn_cast<ExtensionDecl>(idc)) {
    return (LazyIterableDeclContextData *)getOrCreateLazyContextData(
                                                              ext, lazyLoader);
  }

  auto nominal = cast<NominalTypeDecl>(idc);
  return (LazyIterableDeclContextData *)getOrCreateLazyContextData(nominal,
                                                                   lazyLoader);
}

bool ASTContext::hasDelayedConformanceErrors(
                          NormalProtocolConformance const* conformance) const {

  auto hasDelayedErrors = [](std::vector<DelayedConformanceDiag> const& diags) {
    return std::any_of(diags.begin(), diags.end(),
                    [](ASTContext::DelayedConformanceDiag const& diag) {
                      return diag.IsError;
                    });
  };

  if (conformance) {
    auto entry = getImpl().DelayedConformanceDiags.find(conformance);
    if (entry != getImpl().DelayedConformanceDiags.end())
      return hasDelayedErrors(entry->second);

    return false; // unknown conformance, so no delayed diags either.
  }
  
  // check all conformances for any delayed errors
  for (const auto &entry : getImpl().DelayedConformanceDiags) {
    auto const& diagnostics = entry.getSecond();
    if (hasDelayedErrors(diagnostics))
      return true;
  }

  return false;
}

MissingWitnessesBase::~MissingWitnessesBase() { }

void ASTContext::addDelayedConformanceDiag(
       NormalProtocolConformance *conformance,
       DelayedConformanceDiag fn) {
  getImpl().DelayedConformanceDiags[conformance].push_back(std::move(fn));
}

void ASTContext::addDelayedMissingWitnesses(
    NormalProtocolConformance *conformance,
    std::unique_ptr<MissingWitnessesBase> missingWitnesses) {
  getImpl().DelayedMissingWitnesses[conformance] = std::move(missingWitnesses);
}

std::unique_ptr<MissingWitnessesBase>
ASTContext::takeDelayedMissingWitnesses(
    NormalProtocolConformance *conformance) {
  std::unique_ptr<MissingWitnessesBase> result;
  auto known = getImpl().DelayedMissingWitnesses.find(conformance);
  if (known != getImpl().DelayedMissingWitnesses.end()) {
    result = std::move(known->second);
    getImpl().DelayedMissingWitnesses.erase(known);
  }
  return result;
}

std::vector<ASTContext::DelayedConformanceDiag>
ASTContext::takeDelayedConformanceDiags(NormalProtocolConformance const* cnfrm){
  std::vector<ASTContext::DelayedConformanceDiag> result;
  auto known = getImpl().DelayedConformanceDiags.find(cnfrm);
  if (known != getImpl().DelayedConformanceDiags.end()) {
    result = std::move(known->second);
    getImpl().DelayedConformanceDiags.erase(known);
  }
  return result;
}

size_t ASTContext::getTotalMemory() const {
  size_t Size = sizeof(*this) +
    // LoadedModules ?
    llvm::capacity_in_bytes(CanonicalGenericTypeParamTypeNames) +
    // RemappedTypes ?
    sizeof(getImpl()) +
    getImpl().Allocator.getTotalMemory() +
    getImpl().Cleanups.capacity() +
    llvm::capacity_in_bytes(getImpl().ModuleLoaders) +
    llvm::capacity_in_bytes(getImpl().ModuleTypes) +
    llvm::capacity_in_bytes(getImpl().GenericParamTypes) +
    // getImpl().GenericFunctionTypes ?
    // getImpl().SILFunctionTypes ?
    llvm::capacity_in_bytes(getImpl().SILBlockStorageTypes) +
    llvm::capacity_in_bytes(getImpl().IntegerTypes) +
    // getImpl().ProtocolCompositionTypes ?
    // getImpl().BuiltinVectorTypes ?
    // getImpl().GenericSignatures ?
    // getImpl().CompoundNames ?
    getImpl().OpenedExistentialEnvironments.getMemorySize() +
    getImpl().Permanent.getTotalMemory();

    Size += getSolverMemory();

    return Size;
}

size_t ASTContext::getSolverMemory() const {
  size_t Size = 0;
  
  if (getImpl().CurrentConstraintSolverArena) {
    Size += getImpl().CurrentConstraintSolverArena->getTotalMemory();
    Size += getImpl().CurrentConstraintSolverArena->Allocator.getBytesAllocated();
  }
  
  return Size;
}

size_t ASTContext::Implementation::Arena::getTotalMemory() const {
  return sizeof(*this) +
    // TupleTypes ?
    llvm::capacity_in_bytes(MetatypeTypes) +
    llvm::capacity_in_bytes(ExistentialMetatypeTypes) +
    llvm::capacity_in_bytes(ArraySliceTypes) +
    llvm::capacity_in_bytes(DictionaryTypes) +
    llvm::capacity_in_bytes(OptionalTypes) +
    llvm::capacity_in_bytes(VariadicSequenceTypes) +
    llvm::capacity_in_bytes(ParenTypes) +
    llvm::capacity_in_bytes(ReferenceStorageTypes) +
    llvm::capacity_in_bytes(LValueTypes) +
    llvm::capacity_in_bytes(InOutTypes) +
    llvm::capacity_in_bytes(DependentMemberTypes) +
    llvm::capacity_in_bytes(EnumTypes) +
    llvm::capacity_in_bytes(StructTypes) +
    llvm::capacity_in_bytes(ClassTypes) +
    llvm::capacity_in_bytes(ProtocolTypes) +
    llvm::capacity_in_bytes(DynamicSelfTypes);
    // FunctionTypes ?
    // UnboundGenericTypes ?
    // BoundGenericTypes ?
    // NormalConformances ?
    // SpecializedConformances ?
    // InheritedConformances ?
    // BuiltinConformances ?
}

void AbstractFunctionDecl::setForeignErrorConvention(
                                         const ForeignErrorConvention &conv) {
  assert(hasThrows() && "setting error convention on non-throwing decl");
  auto &conventionsMap = getASTContext().getImpl().ForeignErrorConventions;
  assert(!conventionsMap.count(this) && "error convention already set");
  conventionsMap.insert({this, conv});
}

Optional<ForeignErrorConvention>
AbstractFunctionDecl::getForeignErrorConvention() const {
  if (!hasThrows())
    return None;
  auto &conventionsMap = getASTContext().getImpl().ForeignErrorConventions;
  auto it = conventionsMap.find(this);
  if (it == conventionsMap.end()) return None;
  return it->second;
}

void AbstractFunctionDecl::setForeignAsyncConvention(
                                         const ForeignAsyncConvention &conv) {
  assert(hasAsync() && "setting error convention on non-throwing decl");
  auto &conventionsMap = getASTContext().getImpl().ForeignAsyncConventions;
  assert(!conventionsMap.count(this) && "error convention already set");
  conventionsMap.insert({this, conv});
}

Optional<ForeignAsyncConvention>
AbstractFunctionDecl::getForeignAsyncConvention() const {
  if (!hasAsync())
    return None;
  auto &conventionsMap = getASTContext().getImpl().ForeignAsyncConventions;
  auto it = conventionsMap.find(this);
  if (it == conventionsMap.end()) return None;
  return it->second;
}

Optional<KnownFoundationEntity> swift::getKnownFoundationEntity(StringRef name){
  return llvm::StringSwitch<Optional<KnownFoundationEntity>>(name)
#define FOUNDATION_ENTITY(Name) .Case(#Name, KnownFoundationEntity::Name)
#include "swift/AST/KnownFoundationEntities.def"
    .Default(None);
}

StringRef swift::getSwiftName(KnownFoundationEntity kind) {
  StringRef objcName;
  switch (kind) {
#define FOUNDATION_ENTITY(Name) case KnownFoundationEntity::Name:  \
    objcName = #Name;                                             \
    break;
#include "swift/AST/KnownFoundationEntities.def"
  }

  return objcName;
}

//===----------------------------------------------------------------------===//
// Type manipulation routines.
//===----------------------------------------------------------------------===//

TypeAliasType::TypeAliasType(TypeAliasDecl *typealias, Type parent,
                             SubstitutionMap substitutions,
                             Type underlying,
                             RecursiveTypeProperties properties)
    : SugarType(TypeKind::TypeAlias, underlying, properties),
      typealias(typealias) {
  // Record the parent (or absence of a parent).
  if (parent) {
    Bits.TypeAliasType.HasParent = true;
    *getTrailingObjects<Type>() = parent;
  } else {
    Bits.TypeAliasType.HasParent = false;
  }

  // Record the substitutions.
  if (substitutions) {
    assert(typealias->getGenericSignature()->isEqual(
        substitutions.getGenericSignature()));
    Bits.TypeAliasType.HasSubstitutionMap = true;
    *getTrailingObjects<SubstitutionMap>() = substitutions;
  } else {
    assert(!typealias->isGenericContext());
    Bits.TypeAliasType.HasSubstitutionMap = false;
  }
}

TypeAliasType *TypeAliasType::get(TypeAliasDecl *typealias, Type parent,
                                  SubstitutionMap substitutions,
                                  Type underlying) {
  // Compute the recursive properties.
  //
  auto properties = underlying->getRecursiveProperties();
  if (parent)
    properties |= parent->getRecursiveProperties();

  for (auto substGP : substitutions.getReplacementTypes())
    properties |= substGP->getRecursiveProperties();

  // Figure out which arena this type will go into.
  auto &ctx = underlying->getASTContext();
  auto arena = getArena(properties);

  // Profile the type.
  llvm::FoldingSetNodeID id;
  TypeAliasType::Profile(id, typealias, parent, substitutions, underlying);

  // Did we already record this type?
  void *insertPos;
  auto &types = ctx.getImpl().getArena(arena).TypeAliasTypes;
  if (auto result = types.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Build a new type.
  auto genericSig = substitutions.getGenericSignature();
  auto size = totalSizeToAlloc<Type, SubstitutionMap>(parent ? 1 : 0,
                                                      genericSig ? 1 : 0);
  auto mem = ctx.Allocate(size, alignof(TypeAliasType), arena);
  auto result = new (mem) TypeAliasType(typealias, parent, substitutions,
                                        underlying, properties);
  types.InsertNode(result, insertPos);
  return result;
}

void TypeAliasType::Profile(llvm::FoldingSetNodeID &id) const {
  Profile(id, getDecl(), getParent(), getSubstitutionMap(),
          Type(getSinglyDesugaredType()));
}

void TypeAliasType::Profile(
                           llvm::FoldingSetNodeID &id,
                           TypeAliasDecl *typealias,
                           Type parent, SubstitutionMap substitutions,
                           Type underlying) {
  id.AddPointer(typealias);
  id.AddPointer(parent.getPointer());
  substitutions.profile(id);
  id.AddPointer(underlying.getPointer());
}

// Simple accessors.
Type ErrorType::get(const ASTContext &C) { return C.TheErrorType; }

Type ErrorType::get(Type originalType) {
  assert(originalType);

  auto originalProperties = originalType->getRecursiveProperties();
  auto arena = getArena(originalProperties);

  auto &ctx = originalType->getASTContext();
  auto &entry = ctx.getImpl().getArena(arena).ErrorTypesWithOriginal[originalType];
  if (entry) return entry;

  void *mem = ctx.Allocate(sizeof(ErrorType) + sizeof(Type),
                           alignof(ErrorType), arena);
  RecursiveTypeProperties properties = RecursiveTypeProperties::HasError;

  // We need to preserve the solver allocated bit, to ensure any wrapping
  // types are solver allocated too.
  if (originalProperties.isSolverAllocated())
    properties |= RecursiveTypeProperties::SolverAllocated;

  return entry = new (mem) ErrorType(ctx, originalType, properties);
}

Type PlaceholderType::get(ASTContext &ctx, Originator originator) {
  assert(originator);

  auto originatorProps = [&]() -> RecursiveTypeProperties {
    if (auto *tv = originator.dyn_cast<TypeVariableType *>())
      return tv->getRecursiveProperties();

    if (auto *depTy = originator.dyn_cast<DependentMemberType *>())
      return depTy->getRecursiveProperties();

    return RecursiveTypeProperties();
  }();
  auto arena = getArena(originatorProps);

  auto &cache = ctx.getImpl().getArena(arena).PlaceholderTypes;
  auto &entry = cache[originator.getOpaqueValue()];
  if (entry)
    return entry;

  RecursiveTypeProperties properties = RecursiveTypeProperties::HasPlaceholder;

  // We need to preserve the solver allocated bit, to ensure any wrapping
  // types are solver allocated too.
  if (originatorProps.isSolverAllocated())
    properties |= RecursiveTypeProperties::SolverAllocated;

  entry = new (ctx, arena) PlaceholderType(ctx, originator, properties);
  return entry;
}

BuiltinIntegerType *BuiltinIntegerType::get(BuiltinIntegerWidth BitWidth,
                                            const ASTContext &C) {
  assert(!BitWidth.isArbitraryWidth());
  BuiltinIntegerType *&Result = C.getImpl().IntegerTypes[BitWidth];
  if (Result == nullptr)
    Result = new (C, AllocationArena::Permanent) BuiltinIntegerType(BitWidth,C);
  return Result;
}

BuiltinVectorType *BuiltinVectorType::get(const ASTContext &context,
                                          Type elementType,
                                          unsigned numElements) {
  llvm::FoldingSetNodeID id;
  BuiltinVectorType::Profile(id, elementType, numElements);

  void *insertPos;
  if (BuiltinVectorType *vecType
        = context.getImpl().BuiltinVectorTypes.FindNodeOrInsertPos(id, insertPos))
    return vecType;

  assert(elementType->isCanonical() && "Non-canonical builtin vector?");
  BuiltinVectorType *vecTy
    = new (context, AllocationArena::Permanent)
       BuiltinVectorType(context, elementType, numElements);
  context.getImpl().BuiltinVectorTypes.InsertNode(vecTy, insertPos);
  return vecTy;
}

ParenType *ParenType::get(const ASTContext &C, Type underlying) {
  auto properties = underlying->getRecursiveProperties();
  auto arena = getArena(properties);
  ParenType *&Result = C.getImpl().getArena(arena).ParenTypes[underlying];
  if (Result == nullptr) {
    Result = new (C, arena) ParenType(underlying, properties);
    assert((C.hadError() || !underlying->is<InOutType>()) &&
           "Cannot wrap InOutType");
  }
  return Result;
}

CanTupleType TupleType::getEmpty(const ASTContext &C) {
  return cast<TupleType>(CanType(C.TheEmptyTupleType));
}

void TupleType::Profile(llvm::FoldingSetNodeID &ID,
                        ArrayRef<TupleTypeElt> Fields) {
  ID.AddInteger(Fields.size());
  for (const TupleTypeElt &Elt : Fields) {
    ID.AddPointer(Elt.Name.get());
    ID.AddPointer(Elt.getType().getPointer());
  }
}

/// getTupleType - Return the uniqued tuple type with the specified elements.
TupleType *TupleType::get(ArrayRef<TupleTypeElt> Fields, const ASTContext &C) {
  RecursiveTypeProperties properties;
  for (const TupleTypeElt &Elt : Fields) {
    auto eltTy = Elt.getType();
    if (!eltTy) continue;
    properties |= eltTy->getRecursiveProperties();
  }

  auto arena = getArena(properties);

  void *InsertPos = nullptr;
  // Check to see if we've already seen this tuple before.
  llvm::FoldingSetNodeID ID;
  TupleType::Profile(ID, Fields);

  if (TupleType *TT
        = C.getImpl().getArena(arena).TupleTypes.FindNodeOrInsertPos(ID,InsertPos))
    return TT;

  bool IsCanonical = true;   // All canonical elts means this is canonical.
  for (const TupleTypeElt &Elt : Fields) {
    if (Elt.getType().isNull() || !Elt.getType()->isCanonical()) {
      IsCanonical = false;
      break;
    }
  }

  size_t bytes = totalSizeToAlloc<TupleTypeElt>(Fields.size());
  // TupleType will copy the fields list into ASTContext owned memory.
  void *mem = C.Allocate(bytes, alignof(TupleType), arena);
  auto New = new (mem) TupleType(Fields, IsCanonical ? &C : nullptr,
                                 properties);
  C.getImpl().getArena(arena).TupleTypes.InsertNode(New, InsertPos);
  return New;
}

TupleTypeElt::TupleTypeElt(Type ty, Identifier name)
    : Name(name), ElementType(ty) {
  assert(!ty->is<InOutType>() && "Cannot have InOutType in a tuple");
}

PackExpansionType::PackExpansionType(Type patternType, Type countType,
                                     RecursiveTypeProperties properties,
                                     const ASTContext *canCtx)
  : TypeBase(TypeKind::PackExpansion, canCtx, properties),
    patternType(patternType), countType(countType) {}

CanPackExpansionType
CanPackExpansionType::get(CanType patternType, CanType countType) {
  return CanPackExpansionType(PackExpansionType::get(patternType, countType));
}

PackExpansionType *PackExpansionType::get(Type patternType, Type countType) {
  assert(!patternType->is<PackExpansionType>());
  assert(!countType->is<PackExpansionType>());
  // FIXME: stop doing this deliberately in PackExpansionMatcher
  //assert(!patternType->is<PackType>());
  //assert(!countType->is<PackType>());

  auto properties = patternType->getRecursiveProperties();
  properties |= countType->getRecursiveProperties();

  auto arena = getArena(properties);

  auto &context = patternType->getASTContext();
  llvm::FoldingSetNodeID id;
  PackExpansionType::Profile(id, patternType, countType);

  void *insertPos;
  if (PackExpansionType *expType =
        context.getImpl().getArena(arena)
          .PackExpansionTypes.FindNodeOrInsertPos(id, insertPos))
    return expType;

  // The canonical pack expansion type uses the canonical shape.
  // For interface types, we'd need a signature to do this properly,
  // but for archetypes we can do it directly.
  bool countIsCanonical = countType->isCanonical();
  if (countIsCanonical) {
    if (auto archetype = dyn_cast<PackArchetypeType>(countType.getPointer())) {
      auto reducedShape = archetype->getReducedShape();
      countIsCanonical = (reducedShape.getPointer() == archetype);
    }
  }

  const ASTContext *canCtx =
      (patternType->isCanonical() && countIsCanonical)
      ? &context : nullptr;
  PackExpansionType *expansionType =
      new (context, arena) PackExpansionType(patternType, countType, properties,
                                             canCtx);
  context.getImpl().getArena(arena).PackExpansionTypes.InsertNode(expansionType,
                                                                  insertPos);
  return expansionType;
}

void PackExpansionType::Profile(llvm::FoldingSetNodeID &ID,
                                Type patternType,
                                Type countType) {
  ID.AddPointer(patternType.getPointer());
  ID.AddPointer(countType.getPointer());
}

PackType *PackType::getEmpty(const ASTContext &C) {
  return cast<PackType>(CanType(C.TheEmptyPackType));
}

CanPackType CanPackType::get(const ASTContext &C, ArrayRef<CanType> elements) {
  SmallVector<Type, 8> ncElements(elements.begin(), elements.end());
  return CanPackType(PackType::get(C, ncElements));
}

CanPackType CanPackType::get(const ASTContext &C,
                             CanTupleEltTypeArrayRef elements) {
  SmallVector<Type, 8> ncElements(elements.begin(), elements.end());
  return CanPackType(PackType::get(C, ncElements));
}

CanPackType CanPackType::get(const ASTContext &C,
                             AnyFunctionType::CanParamArrayRef params) {
  SmallVector<Type, 8> ncElements;
  ncElements.reserve(params.size());
  for (auto param : params) {
    ncElements.push_back(param.getParameterType());
  }
  return CanPackType(PackType::get(C, ncElements));
}

PackType *PackType::get(const ASTContext &C, ArrayRef<Type> elements) {
  RecursiveTypeProperties properties;
  bool isCanonical = true;
  for (Type eltTy : elements) {
    assert(!eltTy->is<PackType>() &&
           "Cannot have pack directly inside another pack");

    properties |= eltTy->getRecursiveProperties();
    if (!eltTy->isCanonical())
      isCanonical = false;
  }

  auto arena = getArena(properties);

  void *InsertPos = nullptr;
  // Check to see if we've already seen this pack before.
  llvm::FoldingSetNodeID ID;
  PackType::Profile(ID, elements);

  if (PackType *TT
        = C.getImpl().getArena(arena).PackTypes.FindNodeOrInsertPos(ID,InsertPos))
    return TT;

  size_t bytes = totalSizeToAlloc<Type>(elements.size());
  // TupleType will copy the fields list into ASTContext owned memory.
  void *mem = C.Allocate(bytes, alignof(PackType), arena);
  auto New =
      new (mem) PackType(elements, isCanonical ? &C : nullptr, properties);
  C.getImpl().getArena(arena).PackTypes.InsertNode(New, InsertPos);
  return New;
}

void PackType::Profile(llvm::FoldingSetNodeID &ID, ArrayRef<Type> Elements) {
  ID.AddInteger(Elements.size());
  for (Type Ty : Elements) {
    ID.AddPointer(Ty.getPointer());
  }
}

CanSILPackType SILPackType::get(const ASTContext &C, ExtInfo info,
                                ArrayRef<CanType> elements) {
  RecursiveTypeProperties properties;
  for (CanType eltTy : elements) {
    assert(!isa<SILPackType>(eltTy) &&
           "Cannot have pack directly inside another pack");
    properties |= eltTy->getRecursiveProperties();
  }
  assert(getArena(properties) == AllocationArena::Permanent &&
         "SILPackType has elements requiring temporary allocation?");

  void *insertPos = nullptr;
  // Check to see if we've already seen this pack before.
  llvm::FoldingSetNodeID ID;
  SILPackType::Profile(ID, info, elements);

  if (SILPackType *existing
        = C.getImpl().SILPackTypes.FindNodeOrInsertPos(ID, insertPos))
    return CanSILPackType(existing);

  size_t bytes = totalSizeToAlloc<CanType>(elements.size());
  void *mem = C.Allocate(bytes, alignof(SILPackType));
  auto builtType = new (mem) SILPackType(C, properties, info, elements);
  C.getImpl().SILPackTypes.InsertNode(builtType, insertPos);
  return CanSILPackType(builtType);
}

void SILPackType::Profile(llvm::FoldingSetNodeID &ID, ExtInfo info,
                          ArrayRef<CanType> elements) {
  ID.AddBoolean(info.ElementIsAddress);
  ID.AddInteger(elements.size());
  for (CanType element : elements) {
    ID.AddPointer(element.getPointer());
  }
}

Type AnyFunctionType::Param::getOldType() const {
  if (Flags.isInOut()) return InOutType::get(Ty);
  return Ty;
}

AnyFunctionType::Param swift::computeSelfParam(AbstractFunctionDecl *AFD,
                                               bool isInitializingCtor,
                                               bool wantDynamicSelf) {
  auto *dc = AFD->getDeclContext();
  auto &Ctx = dc->getASTContext();
  
  // Determine the type of the container.
  auto containerTy = dc->getDeclaredInterfaceType();
  if (!containerTy || containerTy->hasError())
    return AnyFunctionType::Param(ErrorType::get(Ctx));

  // Determine the type of 'self' inside the container.
  auto selfTy = dc->getSelfInterfaceType();
  if (!selfTy || selfTy->hasError())
    return AnyFunctionType::Param(ErrorType::get(Ctx));

  bool isStatic = false;
  SelfAccessKind selfAccess = SelfAccessKind::NonMutating;
  bool isDynamicSelf = false;

  if (auto *FD = dyn_cast<FuncDecl>(AFD)) {
    isStatic = FD->isStatic();
    selfAccess = FD->getSelfAccessKind();

    // `self`s type for subscripts and properties
    if (auto *AD = dyn_cast<AccessorDecl>(AFD)) {
      if (wantDynamicSelf && AD->getStorage()
          ->getValueInterfaceType()->hasDynamicSelfType())
        isDynamicSelf = true;
    }
    // Methods returning 'Self' have a dynamic 'self'.
    //
    // FIXME: All methods of non-final classes should have this.
    else if (wantDynamicSelf && FD->hasDynamicSelfResult())
      isDynamicSelf = true;

  } else if (auto *CD = dyn_cast<ConstructorDecl>(AFD)) {
    if (isInitializingCtor) {
      // initializing constructors of value types always have an implicitly
      // inout self.
      if (!containerTy->hasReferenceSemantics())
        selfAccess = SelfAccessKind::Mutating;

      // FIXME(distributed): pending swift-evolution, allow `self =` in class
      //  inits in general.
      //  See also: https://github.com/apple/swift/pull/19151 general impl
      auto ext = dyn_cast<ExtensionDecl>(AFD->getDeclContext());
      auto distProto =
          Ctx.getProtocol(KnownProtocolKind::DistributedActor);
      if (distProto && ext && ext->getExtendedNominal() &&
          ext->getExtendedNominal()->getInterfaceType()
              ->isEqual(distProto->getInterfaceType())) {
        auto name = CD->getName();
        auto params = name.getArgumentNames();
        if (params.size() == 1 && params[0] == Ctx.Id_from) {
          // FIXME(distributed): this is a workaround to allow init(from:) to
          //  be implemented in AST by allowing the self to be mutable in the
          //  decoding initializer. This should become a general Swift
          //  feature, allowing this in all classes:
          //  https://forums.swift.org/t/allow-self-x-in-class-convenience-initializers/15924
          selfAccess = SelfAccessKind::Mutating;
        }
      }
    } else {
      // allocating constructors have metatype 'self'.
      isStatic = true;
    }

    // Convenience initializers have a dynamic 'self' in '-swift-version 5'.
    //
    // NOTE: it's important that we check if it's a convenience init only after
    // confirming it's not semantically final, or else there can be a request
    // evaluator cycle to determine the init kind for actors, which are final.
    if (Ctx.isSwiftVersionAtLeast(5)) {
      if (wantDynamicSelf)
        if (auto *classDecl = selfTy->getClassOrBoundGenericClass())
          if (!classDecl->isSemanticallyFinal() && CD->isConvenienceInit())
            isDynamicSelf = true;
    }
  } else if (isa<DestructorDecl>(AFD)) {
    // Destructors only correctly appear on classes today. (If move-only types
    // have destructors, they probably would want to consume self.)
    // Note that we can't assert(containerTy->hasReferenceSemantics()) here
    // since incorrect or incomplete code could have deinit decls in invalid
    // contexts, and we need to recover gracefully in those cases.
  }

  if (isDynamicSelf)
    selfTy = DynamicSelfType::get(selfTy, Ctx);

  // 'static' functions have 'self' of type metatype<T>.
  if (isStatic)
    return AnyFunctionType::Param(MetatypeType::get(selfTy, Ctx));

  // `self` is isolated if typechecker says the function is isolated to it.
  bool isIsolated =
      evaluateOrDefault(Ctx.evaluator, HasIsolatedSelfRequest{AFD}, false);

  auto flags = ParameterTypeFlags().withIsolated(isIsolated);
  switch (selfAccess) {
  case SelfAccessKind::LegacyConsuming:
    flags = flags.withOwnershipSpecifier(ParamSpecifier::LegacyOwned);
    break;
  case SelfAccessKind::Consuming:
    flags = flags.withOwnershipSpecifier(ParamSpecifier::Consuming);
    break;
  case SelfAccessKind::Borrowing:
    flags = flags.withOwnershipSpecifier(ParamSpecifier::Borrowing);
    break;
  case SelfAccessKind::Mutating:
    flags = flags.withInOut(true);
    break;
  case SelfAccessKind::NonMutating:
    // The default flagless state.
    break;
  }

  return AnyFunctionType::Param(selfTy, Identifier(), flags);
}

void UnboundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                                 GenericTypeDecl *TheDecl, Type Parent) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
}

UnboundGenericType *UnboundGenericType::
get(GenericTypeDecl *TheDecl, Type Parent, const ASTContext &C) {
  llvm::FoldingSetNodeID ID;
  UnboundGenericType::Profile(ID, TheDecl, Parent);
  void *InsertPos = nullptr;
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  if (auto unbound = C.getImpl().getArena(arena).UnboundGenericTypes
                        .FindNodeOrInsertPos(ID, InsertPos))
    return unbound;

  auto result = new (C, arena) UnboundGenericType(TheDecl, Parent, C,
                                                  properties);
  C.getImpl().getArena(arena).UnboundGenericTypes.InsertNode(result, InsertPos);
  return result;
}

void BoundGenericType::Profile(llvm::FoldingSetNodeID &ID,
                               NominalTypeDecl *TheDecl, Type Parent,
                               ArrayRef<Type> GenericArgs) {
  ID.AddPointer(TheDecl);
  ID.AddPointer(Parent.getPointer());
  ID.AddInteger(GenericArgs.size());
  for (Type Arg : GenericArgs) {
    ID.AddPointer(Arg.getPointer());
  }
}

BoundGenericType::BoundGenericType(TypeKind theKind,
                                   NominalTypeDecl *theDecl,
                                   Type parent,
                                   ArrayRef<Type> genericArgs,
                                   const ASTContext *context,
                                   RecursiveTypeProperties properties)
    : NominalOrBoundGenericNominalType(theDecl, parent, theKind, context,
                                       properties) {
  Bits.BoundGenericType.GenericArgCount = genericArgs.size();
  // Subtypes are required to provide storage for the generic arguments
  std::uninitialized_copy(genericArgs.begin(), genericArgs.end(),
                          getTrailingObjectsPointer());
}

BoundGenericType *BoundGenericType::get(NominalTypeDecl *TheDecl,
                                        Type Parent,
                                        ArrayRef<Type> GenericArgs) {
  assert(TheDecl->getGenericParams() && "must be a generic type decl");
  assert((!Parent || Parent->is<NominalType>() ||
          Parent->is<BoundGenericType>() ||
          Parent->is<UnboundGenericType>()) &&
         "parent must be a nominal type");

  ASTContext &C = TheDecl->getDeclContext()->getASTContext();
  llvm::FoldingSetNodeID ID;
  BoundGenericType::Profile(ID, TheDecl, Parent, GenericArgs);
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  for (Type Arg : GenericArgs) {
    properties |= Arg->getRecursiveProperties();
  }

  auto arena = getArena(properties);

  void *InsertPos = nullptr;
  if (BoundGenericType *BGT =
        C.getImpl().getArena(arena).BoundGenericTypes.FindNodeOrInsertPos(ID,
                                                                     InsertPos))
    return BGT;

  bool IsCanonical = !Parent || Parent->isCanonical();
  if (IsCanonical) {
    for (Type Arg : GenericArgs) {
      if (!Arg->isCanonical()) {
        IsCanonical = false;
        break;
      }
    }
  }

  BoundGenericType *newType;
  if (auto theClass = dyn_cast<ClassDecl>(TheDecl)) {
    auto sz = BoundGenericClassType::totalSizeToAlloc<Type>(GenericArgs.size());
    auto mem = C.Allocate(sz, alignof(BoundGenericClassType), arena);
    newType = new (mem) BoundGenericClassType(
        theClass, Parent, GenericArgs, IsCanonical ? &C : nullptr, properties);
  } else if (auto theStruct = dyn_cast<StructDecl>(TheDecl)) {
    auto sz = BoundGenericStructType::totalSizeToAlloc<Type>(GenericArgs.size());
    auto mem = C.Allocate(sz, alignof(BoundGenericStructType), arena);
    newType = new (mem) BoundGenericStructType(
        theStruct, Parent, GenericArgs, IsCanonical ? &C : nullptr, properties);
  } else if (auto theEnum = dyn_cast<EnumDecl>(TheDecl)) {
    auto sz = BoundGenericEnumType::totalSizeToAlloc<Type>(GenericArgs.size());
    auto mem = C.Allocate(sz, alignof(BoundGenericEnumType), arena);
    newType = new (mem) BoundGenericEnumType(
        theEnum, Parent, GenericArgs, IsCanonical ? &C : nullptr, properties);
  } else {
    llvm_unreachable("Unhandled NominalTypeDecl");
  }
  C.getImpl().getArena(arena).BoundGenericTypes.InsertNode(newType, InsertPos);

  return newType;
}

NominalType *NominalType::get(NominalTypeDecl *D, Type Parent, const ASTContext &C) {
  assert((isa<ProtocolDecl>(D) || !D->getGenericParams()) &&
         "must be a non-generic type decl");
  assert((!Parent || Parent->is<NominalType>() ||
          Parent->is<BoundGenericType>() ||
          Parent->is<UnboundGenericType>()) &&
         "parent must be a nominal type");

  switch (D->getKind()) {
  case DeclKind::Enum:
    return EnumType::get(cast<EnumDecl>(D), Parent, C);
  case DeclKind::Struct:
    return StructType::get(cast<StructDecl>(D), Parent, C);
  case DeclKind::Class:
    return ClassType::get(cast<ClassDecl>(D), Parent, C);
  case DeclKind::Protocol: {
    return ProtocolType::get(cast<ProtocolDecl>(D), Parent, C);
  }

  default:
    llvm_unreachable("Not a nominal declaration!");
  }
}

EnumType::EnumType(EnumDecl *TheDecl, Type Parent, const ASTContext &C,
                     RecursiveTypeProperties properties)
  : NominalType(TypeKind::Enum, &C, TheDecl, Parent, properties) { }

EnumType *EnumType::get(EnumDecl *D, Type Parent, const ASTContext &C) {
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  auto *&known = C.getImpl().getArena(arena).EnumTypes[{D, Parent}];
  if (!known) {
    known = new (C, arena) EnumType(D, Parent, C, properties);
  }
  return known;
}

StructType::StructType(StructDecl *TheDecl, Type Parent, const ASTContext &C,
                       RecursiveTypeProperties properties)
  : NominalType(TypeKind::Struct, &C, TheDecl, Parent, properties) { }

StructType *StructType::get(StructDecl *D, Type Parent, const ASTContext &C) {
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  auto *&known = C.getImpl().getArena(arena).StructTypes[{D, Parent}];
  if (!known) {
    known = new (C, arena) StructType(D, Parent, C, properties);
  }
  return known;
}

ClassType::ClassType(ClassDecl *TheDecl, Type Parent, const ASTContext &C,
                     RecursiveTypeProperties properties)
  : NominalType(TypeKind::Class, &C, TheDecl, Parent, properties) { }

ClassType *ClassType::get(ClassDecl *D, Type Parent, const ASTContext &C) {
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  auto *&known = C.getImpl().getArena(arena).ClassTypes[{D, Parent}];
  if (!known) {
    known = new (C, arena) ClassType(D, Parent, C, properties);
  }
  return known;
}

ProtocolCompositionType *
ProtocolCompositionType::build(const ASTContext &C, ArrayRef<Type> Members,
                               bool HasExplicitAnyObject) {
  assert(Members.size() != 1 || HasExplicitAnyObject);

  // Check to see if we've already seen this protocol composition before.
  void *InsertPos = nullptr;
  llvm::FoldingSetNodeID ID;
  ProtocolCompositionType::Profile(ID, Members, HasExplicitAnyObject);

  bool isCanonical = true;
  RecursiveTypeProperties properties;
  for (Type t : Members) {
    if (!t->isCanonical())
      isCanonical = false;
    properties |= t->getRecursiveProperties();
  }

  // Create a new protocol composition type.
  auto arena = getArena(properties);

  if (auto compTy
      = C.getImpl().getArena(arena).ProtocolCompositionTypes
          .FindNodeOrInsertPos(ID, InsertPos))
    return compTy;

  // Use trailing objects for member type storage
  auto size = totalSizeToAlloc<Type>(Members.size());
  auto mem = C.Allocate(size, alignof(ProtocolCompositionType), arena);
  auto compTy = new (mem) ProtocolCompositionType(isCanonical ? &C : nullptr,
                                                  Members,
                                                  HasExplicitAnyObject,
                                                  properties);
  C.getImpl().getArena(arena).ProtocolCompositionTypes.InsertNode(
      compTy, InsertPos);
  return compTy;
}

ParameterizedProtocolType *ParameterizedProtocolType::get(const ASTContext &C,
                                                          ProtocolType *baseTy,
                                                          ArrayRef<Type> args) {
  assert(args.size() > 0);

  bool isCanonical = baseTy->isCanonical();
  RecursiveTypeProperties properties = baseTy->getRecursiveProperties();
  for (auto arg : args) {
    properties |= arg->getRecursiveProperties();
    isCanonical &= arg->isCanonical();
  }

  auto arena = getArena(properties);

  void *InsertPos = nullptr;
  llvm::FoldingSetNodeID ID;
  ParameterizedProtocolType::Profile(ID, baseTy, args);

  if (auto paramTy
      = C.getImpl().getArena(arena).ParameterizedProtocolTypes
          .FindNodeOrInsertPos(ID, InsertPos))
    return paramTy;

  auto size = totalSizeToAlloc<Type>(args.size());
  auto mem = C.Allocate(size, alignof(ParameterizedProtocolType), arena);

  properties |= RecursiveTypeProperties::HasParameterizedExistential;

  auto paramTy = new (mem) ParameterizedProtocolType(
        isCanonical ? &C : nullptr, baseTy, args, properties);
  C.getImpl().getArena(arena).ParameterizedProtocolTypes.InsertNode(
      paramTy, InsertPos);
  return paramTy;
}

ReferenceStorageType *ReferenceStorageType::get(Type T,
                                                ReferenceOwnership ownership,
                                                const ASTContext &C) {
  assert(!T->hasTypeVariable()); // not meaningful in type-checker
  assert(!T->hasPlaceholder());
  switch (optionalityOf(ownership)) {
  case ReferenceOwnershipOptionality::Disallowed:
    assert(!T->getOptionalObjectType() && "optional type is disallowed");
    break;
  case ReferenceOwnershipOptionality::Allowed:
    break;
  case ReferenceOwnershipOptionality::Required:
    assert(T->getOptionalObjectType() && "optional type is required");
    break;
  }

  auto properties = T->getRecursiveProperties();
  auto arena = getArena(properties);

  auto key = uintptr_t(T.getPointer()) | unsigned(ownership);
  auto &entry = C.getImpl().getArena(arena).ReferenceStorageTypes[key];
  if (entry) return entry;

  switch (ownership) {
  case ReferenceOwnership::Strong:
    llvm_unreachable("strong ownership does not use ReferenceStorageType");
#define REF_STORAGE(Name, ...) \
  case ReferenceOwnership::Name: \
    return entry = new (C, arena) \
      Name##StorageType(T, T->isCanonical() ? &C : nullptr, properties);
#include "swift/AST/ReferenceStorage.def"
  }
  llvm_unreachable("bad ownership");
}

AnyMetatypeType::AnyMetatypeType(TypeKind kind, const ASTContext *C,
                                 RecursiveTypeProperties properties,
                                 Type instanceType,
                                 Optional<MetatypeRepresentation> repr)
    : TypeBase(kind, C, properties), InstanceType(instanceType) {
  if (repr) {
    Bits.AnyMetatypeType.Representation = static_cast<char>(*repr) + 1;
  } else {
    Bits.AnyMetatypeType.Representation = 0;
  }
}

MetatypeType *MetatypeType::get(Type T, Optional<MetatypeRepresentation> Repr,
                                const ASTContext &Ctx) {
  auto properties = T->getRecursiveProperties();
  auto arena = getArena(properties);

  unsigned reprKey;
  if (Repr.has_value())
    reprKey = static_cast<unsigned>(*Repr) + 1;
  else
    reprKey = 0;

  auto pair = llvm::PointerIntPair<TypeBase*, 3, unsigned>(T.getPointer(),
                                                           reprKey);

  MetatypeType *&Entry = Ctx.getImpl().getArena(arena).MetatypeTypes[pair];
  if (Entry) return Entry;

  return Entry = new (Ctx, arena) MetatypeType(
             T, T->isCanonical() ? &Ctx : nullptr, properties, Repr);
}

MetatypeType::MetatypeType(Type T, const ASTContext *C,
                           RecursiveTypeProperties properties,
                           Optional<MetatypeRepresentation> repr)
  : AnyMetatypeType(TypeKind::Metatype, C, properties, T, repr) {
}

ExistentialMetatypeType *
ExistentialMetatypeType::get(Type T, Optional<MetatypeRepresentation> repr,
                             const ASTContext &ctx) {
  // If we're creating an existential metatype from an
  // existential type, wrap the constraint type direcly.
  if (auto existential = T->getAs<ExistentialType>())
    T = existential->getConstraintType();

  auto properties = T->getRecursiveProperties();
  auto arena = getArena(properties);

  unsigned reprKey;
  if (repr.has_value())
    reprKey = static_cast<unsigned>(*repr) + 1;
  else
    reprKey = 0;

  auto pair = llvm::PointerIntPair<TypeBase*, 3, unsigned>(T.getPointer(),
                                                           reprKey);

  auto &entry = ctx.getImpl().getArena(arena).ExistentialMetatypeTypes[pair];
  if (entry) return entry;

  return entry = new (ctx, arena) ExistentialMetatypeType(
             T, T->isCanonical() ? &ctx : nullptr, properties, repr);
}

ExistentialMetatypeType::ExistentialMetatypeType(Type T,
                                                 const ASTContext *C,
                                       RecursiveTypeProperties properties,
                                       Optional<MetatypeRepresentation> repr)
  : AnyMetatypeType(TypeKind::ExistentialMetatype, C, properties, T, repr) {
  if (repr) {
    assert(*repr != MetatypeRepresentation::Thin &&
           "creating a thin existential metatype?");
    assert(getASTContext().LangOpts.EnableObjCInterop ||
           *repr != MetatypeRepresentation::ObjC);
  }
}

Type ExistentialMetatypeType::getExistentialInstanceType() {
  return ExistentialType::get(getInstanceType());
}

ModuleType *ModuleType::get(ModuleDecl *M) {
  ASTContext &C = M->getASTContext();

  ModuleType *&Entry = C.getImpl().ModuleTypes[M];
  if (Entry) return Entry;

  return Entry = new (C, AllocationArena::Permanent) ModuleType(M, C);
}

DynamicSelfType *DynamicSelfType::get(Type selfType, const ASTContext &ctx) {
  assert(selfType->isMaterializable()
         && "non-materializable dynamic self?");
  
  auto properties = selfType->getRecursiveProperties();
  auto arena = getArena(properties);

  auto &dynamicSelfTypes = ctx.getImpl().getArena(arena).DynamicSelfTypes;
  auto known = dynamicSelfTypes.find(selfType);
  if (known != dynamicSelfTypes.end())
    return known->second;

  auto result = new (ctx, arena) DynamicSelfType(selfType, ctx, properties);
  dynamicSelfTypes.insert({selfType, result});
  return result;
}

static RecursiveTypeProperties
getFunctionRecursiveProperties(ArrayRef<AnyFunctionType::Param> params,
                               Type result, Type globalActor) {
  RecursiveTypeProperties properties;
  for (auto param : params)
    properties |= param.getPlainType()->getRecursiveProperties();
  properties |= result->getRecursiveProperties();
  if (globalActor)
    properties |= globalActor->getRecursiveProperties();
  properties &= ~RecursiveTypeProperties::IsLValue;
  return properties;
}

static bool
isAnyFunctionTypeCanonical(ArrayRef<AnyFunctionType::Param> params,
                        Type result) {
  for (auto param : params) {
    if (!param.getPlainType()->isCanonical())
      return false;
    if (!param.getInternalLabel().empty()) {
      // Canonical types don't have internal labels
      return false;
    }
  }

  return result->isCanonical();
}

// For now, generic function types cannot be dependent (in fact,
// they erase dependence) or contain type variables, and they're
// always materializable.
static RecursiveTypeProperties
getGenericFunctionRecursiveProperties(ArrayRef<AnyFunctionType::Param> params,
                                      Type result) {
  static_assert(RecursiveTypeProperties::BitWidth == 16,
                "revisit this if you add new recursive type properties");
  RecursiveTypeProperties properties;

  for (auto param : params) {
    if (param.getPlainType()->getRecursiveProperties().hasError())
      properties |= RecursiveTypeProperties::HasError;
  }

  if (result->getRecursiveProperties().hasDynamicSelf())
    properties |= RecursiveTypeProperties::HasDynamicSelf;
  if (result->getRecursiveProperties().hasError())
    properties |= RecursiveTypeProperties::HasError;

  return properties;
}

static bool
isGenericFunctionTypeCanonical(GenericSignature sig,
                               ArrayRef<AnyFunctionType::Param> params,
                               Type result) {
  if (!sig->isCanonical())
    return false;

  for (auto param : params) {
    if (!sig->isReducedType(param.getPlainType()))
      return false;
    if (!param.getInternalLabel().empty()) {
      // Canonical types don't have internal labels
      return false;
    }
  }

  return sig->isReducedType(result);
}

AnyFunctionType *AnyFunctionType::withExtInfo(ExtInfo info) const {
  if (isa<FunctionType>(this))
    return FunctionType::get(getParams(), getResult(), info);

  auto *genFnTy = cast<GenericFunctionType>(this);
  return GenericFunctionType::get(genFnTy->getGenericSignature(),
                                  getParams(), getResult(), info);
}

Type AnyFunctionType::Param::getParameterType(bool forCanonical,
                                              ASTContext *ctx) const {
  Type type = getPlainType();
  if (isVariadic()) {
    if (!ctx) ctx = &type->getASTContext();
    auto arrayDecl = ctx->getArrayDecl();
    if (!arrayDecl)
      type = ErrorType::get(*ctx);
    else if (type->is<PackType>())
      return type;
    else if (forCanonical)
      type = BoundGenericType::get(arrayDecl, Type(), {type});
    else
      type = VariadicSequenceType::get(type);
  }
  return type;
}

Type AnyFunctionType::composeTuple(ASTContext &ctx, ArrayRef<Param> params,
                                   ParameterFlagHandling paramFlagHandling) {
  SmallVector<TupleTypeElt, 4> elements;
  for (const auto &param : params) {
    switch (paramFlagHandling) {
    case ParameterFlagHandling::IgnoreNonEmpty:
      break;
    case ParameterFlagHandling::AssertEmpty:
      assert(param.getParameterFlags().isNone());
      break;
    }
    elements.emplace_back(param.getParameterType(), param.getLabel());
  }
  if (elements.size() == 1 && !elements[0].hasName())
    return ParenType::get(ctx, elements[0].getType());
  return TupleType::get(elements, ctx);
}

bool AnyFunctionType::equalParams(ArrayRef<AnyFunctionType::Param> a,
                                  ArrayRef<AnyFunctionType::Param> b) {
  if (a.size() != b.size())
    return false;

  for (unsigned i = 0, n = a.size(); i != n; ++i) {
    if (a[i] != b[i])
      return false;
  }

  return true;
}

bool AnyFunctionType::equalParams(CanParamArrayRef a, CanParamArrayRef b) {
  if (a.size() != b.size())
    return false;

  for (unsigned i = 0, n = a.size(); i != n; ++i) {
    if (a[i] != b[i])
      return false;
  }

  return true;
}

void AnyFunctionType::relabelParams(MutableArrayRef<Param> params,
                                    ArgumentList *argList) {
  assert(params.size() == argList->size());
  for (auto i : indices(params)) {
    auto &param = params[i];
    param = AnyFunctionType::Param(param.getPlainType(), argList->getLabel(i),
                                   param.getParameterFlags(),
                                   param.getInternalLabel());
  }
}

/// Profile \p params into \p ID. In contrast to \c == on \c Param, the profile
/// *does* take the internal label into account and *does not* canonicalize
/// the param's type.
static void profileParams(llvm::FoldingSetNodeID &ID,
                          ArrayRef<AnyFunctionType::Param> params) {
  ID.AddInteger(params.size());
  for (auto param : params) {
    ID.AddPointer(param.getLabel().get());
    ID.AddPointer(param.getInternalLabel().get());
    ID.AddPointer(param.getPlainType().getPointer());
    ID.AddInteger(param.getParameterFlags().toRaw());
  }
}

void FunctionType::Profile(llvm::FoldingSetNodeID &ID,
                           ArrayRef<AnyFunctionType::Param> params, Type result,
                           Optional<ExtInfo> info) {
  profileParams(ID, params);
  ID.AddPointer(result.getPointer());
  if (info.has_value()) {
    auto infoKey = info.value().getFuncAttrKey();
    ID.AddInteger(std::get<0>(infoKey));
    ID.AddPointer(std::get<1>(infoKey));
    ID.AddPointer(std::get<2>(infoKey));
  }
}

FunctionType *FunctionType::get(ArrayRef<AnyFunctionType::Param> params,
                                Type result, Optional<ExtInfo> info) {
  Type globalActor;
  if (info.has_value())
    globalActor = info->getGlobalActor();

  auto properties = getFunctionRecursiveProperties(params, result, globalActor);
  auto arena = getArena(properties);

  if (info.has_value()) {
    // Canonicalize all thin functions to be escaping (to keep compatibility
    // with generic parameters). Note that one can pass SIL-level representation
    // here, so we need additional check for maximum non-SIL value.
    Representation rep = info.value().getRepresentation();
    if (rep <= FunctionTypeRepresentation::Last &&
        isThinRepresentation(rep))
      info = info->withNoEscape(false);
  }

  llvm::FoldingSetNodeID id;
  FunctionType::Profile(id, params, result, info);

  const ASTContext &ctx = result->getASTContext();

  // Do we already have this generic function type?
  void *insertPos;
  if (auto funcTy =
        ctx.getImpl().getArena(arena).FunctionTypes.FindNodeOrInsertPos(id, insertPos)) {
    return funcTy;
  }

  ClangTypeInfo clangTypeInfo;
  if (info.has_value())
    clangTypeInfo = info.value().getClangTypeInfo();

  bool hasClangInfo =
      info.has_value() && !info.value().getClangTypeInfo().empty();

  size_t allocSize = totalSizeToAlloc<
      AnyFunctionType::Param, ClangTypeInfo, Type
    >(params.size(), hasClangInfo ? 1 : 0, globalActor ? 1 : 0);
  void *mem = ctx.Allocate(allocSize, alignof(FunctionType), arena);

  bool isCanonical = isAnyFunctionTypeCanonical(params, result);
  if (!clangTypeInfo.empty()) {
    if (ctx.LangOpts.UseClangFunctionTypes)
      isCanonical &= clangTypeInfo.getType()->isCanonicalUnqualified();
    else
      isCanonical = false;
  }

  if (globalActor && !globalActor->isCanonical())
    isCanonical = false;

  auto funcTy = new (mem) FunctionType(params, result, info,
                                       isCanonical ? &ctx : nullptr,
                                       properties);
  ctx.getImpl().getArena(arena).FunctionTypes.InsertNode(funcTy, insertPos);
  return funcTy;
}

// If the input and result types are canonical, then so is the result.
FunctionType::FunctionType(ArrayRef<AnyFunctionType::Param> params, Type output,
                           Optional<ExtInfo> info, const ASTContext *ctx,
                           RecursiveTypeProperties properties)
    : AnyFunctionType(TypeKind::Function, ctx, output, properties,
                      params.size(), info) {
  std::uninitialized_copy(params.begin(), params.end(),
                          getTrailingObjects<AnyFunctionType::Param>());
  if (info.has_value()) {
    auto clangTypeInfo = info.value().getClangTypeInfo();
    if (!clangTypeInfo.empty())
      *getTrailingObjects<ClangTypeInfo>() = clangTypeInfo;
    if (Type globalActor = info->getGlobalActor())
      *getTrailingObjects<Type>() = globalActor;
  }
}

void GenericFunctionType::Profile(llvm::FoldingSetNodeID &ID,
                                  GenericSignature sig,
                                  ArrayRef<AnyFunctionType::Param> params,
                                  Type result, Optional<ExtInfo> info) {
  ID.AddPointer(sig.getPointer());
  profileParams(ID, params);
  ID.AddPointer(result.getPointer());
  if (info.has_value()) {
    auto infoKey = info.value().getFuncAttrKey();
    ID.AddInteger(std::get<0>(infoKey));
    ID.AddPointer(std::get<1>(infoKey));
    ID.AddPointer(std::get<2>(infoKey));
  }
}

GenericFunctionType *GenericFunctionType::get(GenericSignature sig,
                                              ArrayRef<Param> params,
                                              Type result,
                                              Optional<ExtInfo> info) {
  assert(sig && "no generic signature for generic function type?!");

  // We do not allow type variables in GenericFunctionTypes. Note that if this
  // ever changes, we'll need to setup arena-specific allocation for
  // GenericFunctionTypes.
  assert(llvm::none_of(params, [](Param param) {
    return param.getPlainType()->hasTypeVariable();
  }));
  assert(!result->hasTypeVariable());

  llvm::FoldingSetNodeID id;
  GenericFunctionType::Profile(id, sig, params, result, info);

  const ASTContext &ctx = result->getASTContext();

  // Do we already have this generic function type?
  void *insertPos;
  if (auto result
        = ctx.getImpl().GenericFunctionTypes.FindNodeOrInsertPos(id, insertPos)) {
    return result;
  }

  // We have to construct this generic function type. Determine whether
  // it's canonical.  Unfortunately, isReducedType() can cause
  // new GenericFunctionTypes to be created and thus invalidate our insertion
  // point.
  bool isCanonical = isGenericFunctionTypeCanonical(sig, params, result);

  assert((!info.has_value() || info.value().getClangTypeInfo().empty()) &&
         "Generic functions do not have Clang types at the moment.");

  if (auto funcTy
        = ctx.getImpl().GenericFunctionTypes.FindNodeOrInsertPos(id, insertPos)) {
    return funcTy;
  }

  Type globalActor;
  if (info.has_value())
    globalActor = info->getGlobalActor();

  if (globalActor && !sig->isReducedType(globalActor))
    isCanonical = false;

  size_t allocSize = totalSizeToAlloc<AnyFunctionType::Param, Type>(
      params.size(), globalActor ? 1 : 0);
  void *mem = ctx.Allocate(allocSize, alignof(GenericFunctionType));

  auto properties = getGenericFunctionRecursiveProperties(params, result);
  auto funcTy = new (mem) GenericFunctionType(sig, params, result, info,
                                              isCanonical ? &ctx : nullptr,
                                              properties);

  ctx.getImpl().GenericFunctionTypes.InsertNode(funcTy, insertPos);
  return funcTy;
}

GenericFunctionType::GenericFunctionType(
                       GenericSignature sig,
                       ArrayRef<AnyFunctionType::Param> params,
                       Type result,
                       Optional<ExtInfo> info,
                       const ASTContext *ctx,
                       RecursiveTypeProperties properties)
  : AnyFunctionType(TypeKind::GenericFunction, ctx, result,
                    properties, params.size(), info), Signature(sig) {
  std::uninitialized_copy(params.begin(), params.end(),
                          getTrailingObjects<AnyFunctionType::Param>());
  if (info) {
    if (Type globalActor = info->getGlobalActor())
      *getTrailingObjects<Type>() = globalActor;
  }
}

GenericTypeParamType *GenericTypeParamType::get(bool isParameterPack,
                                                unsigned depth, unsigned index,
                                                const ASTContext &ctx) {
  const auto depthKey = depth | ((isParameterPack ? 1 : 0) << 30);
  auto known = ctx.getImpl().GenericParamTypes.find({depthKey, index});
  if (known != ctx.getImpl().GenericParamTypes.end())
    return known->second;

  RecursiveTypeProperties props = RecursiveTypeProperties::HasTypeParameter;
  if (isParameterPack)
    props |= RecursiveTypeProperties::HasParameterPack;

  auto result = new (ctx, AllocationArena::Permanent)
      GenericTypeParamType(isParameterPack, depth, index, props, ctx);
  ctx.getImpl().GenericParamTypes[{depthKey, index}] = result;
  return result;
}

TypeArrayView<GenericTypeParamType>
GenericFunctionType::getGenericParams() const {
  return Signature.getGenericParams();
}

/// Retrieve the requirements of this polymorphic function type.
ArrayRef<Requirement> GenericFunctionType::getRequirements() const {
  return Signature.getRequirements();
}

void SILFunctionType::Profile(
    llvm::FoldingSetNodeID &id,
    GenericSignature genericParams,
    ExtInfo info,
    SILCoroutineKind coroutineKind,
    ParameterConvention calleeConvention,
    ArrayRef<SILParameterInfo> params,
    ArrayRef<SILYieldInfo> yields,
    ArrayRef<SILResultInfo> results,
    Optional<SILResultInfo> errorResult,
    ProtocolConformanceRef conformance,
    SubstitutionMap patternSubs,
    SubstitutionMap invocationSubs) {
  id.AddPointer(genericParams.getPointer());
  auto infoKey = info.getFuncAttrKey();
  id.AddInteger(infoKey.first);
  id.AddPointer(infoKey.second);
  id.AddInteger(unsigned(coroutineKind));
  id.AddInteger(unsigned(calleeConvention));
  id.AddInteger(params.size());
  for (auto param : params)
    param.profile(id);
  id.AddInteger(yields.size());
  for (auto yield : yields)
    yield.profile(id);
  id.AddInteger(results.size());
  for (auto result : results)
    result.profile(id);

  // Just allow the profile length to implicitly distinguish the
  // presence of an error result.
  if (errorResult) errorResult->profile(id);
  patternSubs.profile(id);
  invocationSubs.profile(id);
  id.AddBoolean((bool)conformance);
  if (conformance)
    id.AddPointer(conformance.getRequirement());
}

SILFunctionType::SILFunctionType(
    GenericSignature genericSig,
    ExtInfo ext,
    SILCoroutineKind coroutineKind,
    ParameterConvention calleeConvention,
    ArrayRef<SILParameterInfo> params,
    ArrayRef<SILYieldInfo> yields,
    ArrayRef<SILResultInfo> normalResults,
    Optional<SILResultInfo> errorResult,
    SubstitutionMap patternSubs,
    SubstitutionMap invocationSubs,
    const ASTContext &ctx,
    RecursiveTypeProperties properties,
    ProtocolConformanceRef witnessMethodConformance)
    : TypeBase(TypeKind::SILFunction, &ctx, properties),
      InvocationGenericSig(CanGenericSignature(genericSig)),
      WitnessMethodConformance(witnessMethodConformance) {

  Bits.SILFunctionType.HasErrorResult = errorResult.has_value();
  Bits.SILFunctionType.ExtInfoBits = ext.getBits();
  Bits.SILFunctionType.HasClangTypeInfo = false;
  Bits.SILFunctionType.HasPatternSubs = (bool) patternSubs;
  Bits.SILFunctionType.HasInvocationSubs = (bool) invocationSubs;
  // The use of both assert() and static_assert() below is intentional.
  assert(Bits.SILFunctionType.ExtInfoBits == ext.getBits() &&
         "Bits were dropped!");
  static_assert(SILExtInfoBuilder::NumMaskBits == NumSILExtInfoBits,
                "ExtInfo and SILFunctionTypeBitfields must agree on bit size");
  Bits.SILFunctionType.HasClangTypeInfo = !ext.getClangTypeInfo().empty();
  Bits.SILFunctionType.CoroutineKind = unsigned(coroutineKind);
  NumParameters = params.size();
  if (coroutineKind == SILCoroutineKind::None) {
    assert(yields.empty());
    NumAnyResults = normalResults.size();
    NumAnyIndirectFormalResults = 0;
    NumPackResults = 0;
    for (auto &resultInfo : normalResults) {
      if (resultInfo.isFormalIndirect())
        NumAnyIndirectFormalResults++;
      if (resultInfo.isPack())
        NumPackResults++;
    }
    memcpy(getMutableResults().data(), normalResults.data(),
           normalResults.size() * sizeof(SILResultInfo));
  } else {
    assert(normalResults.empty());
    NumAnyResults = yields.size();
    NumAnyIndirectFormalResults = 0;
    NumPackResults = 0;
    for (auto &yieldInfo : yields) {
      if (yieldInfo.isFormalIndirect())
        NumAnyIndirectFormalResults++;
      if (yieldInfo.isPack())
        NumPackResults++;
    }
    memcpy(getMutableYields().data(), yields.data(),
           yields.size() * sizeof(SILYieldInfo));
  }

  assert(!isIndirectFormalParameter(calleeConvention));
  Bits.SILFunctionType.CalleeConvention = unsigned(calleeConvention);

  memcpy(getMutableParameters().data(), params.data(),
         params.size() * sizeof(SILParameterInfo));
  if (errorResult)
    getMutableErrorResult() = *errorResult;

  if (patternSubs)
    getMutablePatternSubs() = patternSubs;
  if (invocationSubs)
    getMutableInvocationSubs() = invocationSubs;

  if (hasResultCache()) {
    getMutableFormalResultsCache() = CanType();
    getMutableAllResultsCache() = CanType();
  }
  if (!ext.getClangTypeInfo().empty())
    *getTrailingObjects<ClangTypeInfo>() = ext.getClangTypeInfo();

#ifndef NDEBUG
  if (ext.getRepresentation() == Representation::WitnessMethod)
    assert(!WitnessMethodConformance.isInvalid() &&
           "witness_method SIL function without a conformance");
  else
    assert(WitnessMethodConformance.isInvalid() &&
           "non-witness_method SIL function with a conformance");

  // Make sure the type follows invariants.
  assert((!invocationSubs || genericSig)
         && "can only have substitutions with a generic signature");
        
  if (invocationSubs) {
    assert(invocationSubs.getGenericSignature().getCanonicalSignature() ==
               genericSig.getCanonicalSignature() &&
           "substitutions must match generic signature");
  }
        
  if (genericSig) {
    assert(!genericSig->areAllParamsConcrete() &&
           "If all generic parameters are concrete, SILFunctionType should "
           "not have a generic signature at all");

    for (auto gparam : genericSig.getGenericParams()) {
      (void)gparam;
      assert(gparam->isCanonical() && "generic signature is not canonicalized");
    }
  }

  if (genericSig || patternSubs) {
    for (auto param : getParameters()) {
      (void)param;
      assert(!param.getInterfaceType()->hasError()
             && "interface type of parameter should not contain error types");
      assert(!param.getInterfaceType()->hasArchetype()
             && "interface type of parameter should not contain context archetypes");
    }
    for (auto result : getResults()) {
      (void)result;
      assert(!result.getInterfaceType()->hasError()
             && "interface type of result should not contain error types");
      assert(!result.getInterfaceType()->hasArchetype()
             && "interface type of result should not contain context archetypes");
    }
    for (auto yield : getYields()) {
      (void)yield;
      assert(!yield.getInterfaceType()->hasError()
             && "interface type of yield should not contain error types");
      assert(!yield.getInterfaceType()->hasArchetype()
             && "interface type of yield should not contain context archetypes");
    }
    if (hasErrorResult()) {
      assert(!getErrorResult().getInterfaceType()->hasError()
             && "interface type of result should not contain error types");
      assert(!getErrorResult().getInterfaceType()->hasArchetype()
             && "interface type of result should not contain context archetypes");
    }

    if (genericSig && patternSubs) {
      assert(!patternSubs.hasArchetypes()
             && "pattern substitutions should not contain context archetypes");
    }
  }
  for (auto result : getResults()) {
    assert(!isa<PackExpansionType>(result.getInterfaceType()) &&
           "Cannot have a pack expansion directly as a result");
    (void)result;
    assert((result.getConvention() == ResultConvention::Pack) ==
           (isa<SILPackType>(result.getInterfaceType())) &&
           "Packs must have pack convention");
    if (auto *FnType = result.getInterfaceType()->getAs<SILFunctionType>()) {
      assert(!FnType->isNoEscape() &&
             "Cannot return an @noescape function type");
    }
  }
  for (auto param : getParameters()) {
    (void)param;
    assert(!isa<PackExpansionType>(param.getInterfaceType()) &&
           "Cannot have a pack expansion directly as a parameter");
    assert(param.isPack() == isa<SILPackType>(param.getInterfaceType()) &&
           "Packs must have pack convention");
  }
  for (auto yield : getYields()) {
    (void)yield;
    assert(!isa<PackExpansionType>(yield.getInterfaceType()) &&
           "Cannot have a pack expansion directly as a yield");
    assert(yield.isPack() == isa<SILPackType>(yield.getInterfaceType()) &&
           "Packs must have pack convention");
  }

  // Check that `@noDerivative` parameters and results only exist in
  // `@differentiable` function types.
  if (!ext.isDifferentiable()) {
    for (auto param : getParameters()) {
      assert(param.getDifferentiability() ==
                 SILParameterDifferentiability::DifferentiableOrNotApplicable &&
             "non-`@differentiable` function type should not have "
             "`@noDerivative` parameter");
    }
    for (auto result : getResults()) {
      assert(result.getDifferentiability() ==
                 SILResultDifferentiability::DifferentiableOrNotApplicable &&
             "non-`@differentiable` function type should not have "
             "`@noDerivative` result");
    }
  }
#endif
}

CanSILMoveOnlyWrappedType SILMoveOnlyWrappedType::get(CanType innerType) {
  ASTContext &ctx = innerType->getASTContext();
  auto found = ctx.getImpl().SILMoveOnlyWrappedTypes.find(innerType);
  if (found != ctx.getImpl().SILMoveOnlyWrappedTypes.end())
    return CanSILMoveOnlyWrappedType(found->second);

  void *mem = ctx.Allocate(sizeof(SILMoveOnlyWrappedType),
                           alignof(SILMoveOnlyWrappedType));

  auto *storageTy = new (mem) SILMoveOnlyWrappedType(innerType);
  ctx.getImpl().SILMoveOnlyWrappedTypes.insert({innerType, storageTy});
  return CanSILMoveOnlyWrappedType(storageTy);
}

CanSILBlockStorageType SILBlockStorageType::get(CanType captureType) {
  ASTContext &ctx = captureType->getASTContext();
  auto found = ctx.getImpl().SILBlockStorageTypes.find(captureType);
  if (found != ctx.getImpl().SILBlockStorageTypes.end())
    return CanSILBlockStorageType(found->second);
  
  void *mem = ctx.Allocate(sizeof(SILBlockStorageType),
                           alignof(SILBlockStorageType));
  
  SILBlockStorageType *storageTy = new (mem) SILBlockStorageType(captureType);
  ctx.getImpl().SILBlockStorageTypes.insert({captureType, storageTy});
  return CanSILBlockStorageType(storageTy);
}

CanSILFunctionType SILFunctionType::get(
    GenericSignature genericSig,
    ExtInfo ext, SILCoroutineKind coroutineKind,
    ParameterConvention callee,
    ArrayRef<SILParameterInfo> params,
    ArrayRef<SILYieldInfo> yields,
    ArrayRef<SILResultInfo> normalResults,
    Optional<SILResultInfo> errorResult,
    SubstitutionMap patternSubs,
    SubstitutionMap invocationSubs,
    const ASTContext &ctx,
    ProtocolConformanceRef witnessMethodConformance) {
  assert(coroutineKind == SILCoroutineKind::None || normalResults.empty());
  assert(coroutineKind != SILCoroutineKind::None || yields.empty());
  assert(!ext.isPseudogeneric() || genericSig ||
         coroutineKind != SILCoroutineKind::None);

  patternSubs = patternSubs.getCanonical();
  invocationSubs = invocationSubs.getCanonical();

  // [FIXME: Clang-type-plumbing]
  if (ctx.LangOpts.UseClangFunctionTypes) {
    if (auto error = ext.checkClangType()) {
      error.value().dump();
      llvm_unreachable("Unexpected Clang type in SILExtInfo.");
    }
  } else if (!ext.getClangTypeInfo().empty()) {
    // Unlike AnyFunctionType, SILFunctionType is always canonical. Hence,
    // conditionalizing canonical type computation based on
    // UseClangFunctionTypes like AnyFunctionType is not feasible. It is simpler
    // to drop the Clang type altogether.
    ext = ext.intoBuilder().withClangFunctionType(nullptr).build();
  }

  // Canonicalize all thin functions to be escaping (to keep compatibility
  // with generic parameters)
  if (isThinRepresentation(ext.getRepresentation()))
    ext = ext.intoBuilder().withNoEscape(false);
  
  llvm::FoldingSetNodeID id;
  SILFunctionType::Profile(id, genericSig, ext, coroutineKind, callee, params,
                           yields, normalResults, errorResult,
                           witnessMethodConformance,
                           patternSubs, invocationSubs);

  // Do we already have this generic function type?
  void *insertPos;
  if (auto result
        = ctx.getImpl().SILFunctionTypes.FindNodeOrInsertPos(id, insertPos))
    return CanSILFunctionType(result);

  // All SILFunctionTypes are canonical.

  // See [NOTE: SILFunctionType-layout]
  bool hasResultCache = normalResults.size() > 1;
  size_t bytes = totalSizeToAlloc<SILParameterInfo, SILResultInfo, SILYieldInfo,
                                  SubstitutionMap, CanType, ClangTypeInfo>(
      params.size(), normalResults.size() + (errorResult ? 1 : 0),
      yields.size(), (patternSubs ? 1 : 0) + (invocationSubs ? 1 : 0),
      hasResultCache ? 2 : 0, ext.getClangTypeInfo().empty() ? 0 : 1);

  void *mem = ctx.Allocate(bytes, alignof(SILFunctionType));

  RecursiveTypeProperties properties;
  static_assert(RecursiveTypeProperties::BitWidth == 16,
                "revisit this if you add new recursive type properties");
  for (auto &param : params)
    properties |= param.getInterfaceType()->getRecursiveProperties();
  for (auto &yield : yields)
    properties |= yield.getInterfaceType()->getRecursiveProperties();
  for (auto &result : normalResults)
    properties |= result.getInterfaceType()->getRecursiveProperties();
  if (errorResult)
    properties |= errorResult->getInterfaceType()->getRecursiveProperties();
  
  // FIXME: If we ever have first-class polymorphic values, we'll need to
  // revisit this.
  if (genericSig || patternSubs) {
    properties.removeHasTypeParameter();
    properties.removeHasDependentMember();
  }

  auto outerSubs = genericSig ? invocationSubs : patternSubs;
  for (auto replacement : outerSubs.getReplacementTypes()) {
    properties |= replacement->getRecursiveProperties();
  }

  auto fnType =
      new (mem) SILFunctionType(genericSig, ext, coroutineKind, callee,
                                params, yields, normalResults, errorResult,
                                patternSubs, invocationSubs,
                                ctx, properties, witnessMethodConformance);
  assert(fnType->hasResultCache() == hasResultCache);

  ctx.getImpl().SILFunctionTypes.InsertNode(fnType, insertPos);
  return CanSILFunctionType(fnType);
}

ArraySliceType *ArraySliceType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  ArraySliceType *&entry = C.getImpl().getArena(arena).ArraySliceTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) ArraySliceType(C, base, properties);
}

VariadicSequenceType *VariadicSequenceType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  VariadicSequenceType *&entry = C.getImpl().getArena(arena).VariadicSequenceTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) VariadicSequenceType(C, base, properties);
}

DictionaryType *DictionaryType::get(Type keyType, Type valueType) {
  auto properties = keyType->getRecursiveProperties() 
                  | valueType->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = keyType->getASTContext();

  DictionaryType *&entry
    = C.getImpl().getArena(arena).DictionaryTypes[{keyType, valueType}];
  if (entry) return entry;

  return entry = new (C, arena) DictionaryType(C, keyType, valueType, 
                                               properties);
}

OptionalType *OptionalType::get(Type base) {
  auto properties = base->getRecursiveProperties();
  auto arena = getArena(properties);

  const ASTContext &C = base->getASTContext();

  OptionalType *&entry = C.getImpl().getArena(arena).OptionalTypes[base];
  if (entry) return entry;

  return entry = new (C, arena) OptionalType(C, base, properties);
}

ProtocolType *ProtocolType::get(ProtocolDecl *D, Type Parent,
                                const ASTContext &C) {
  RecursiveTypeProperties properties;
  if (Parent) properties |= Parent->getRecursiveProperties();
  auto arena = getArena(properties);

  auto *&known = C.getImpl().getArena(arena).ProtocolTypes[{D, Parent}];
  if (!known) {
    known = new (C, arena) ProtocolType(D, Parent, C, properties);
  }
  return known;
}

ProtocolType::ProtocolType(ProtocolDecl *TheDecl, Type Parent,
                           const ASTContext &Ctx,
                           RecursiveTypeProperties properties)
  : NominalType(TypeKind::Protocol, &Ctx, TheDecl, Parent, properties) { }

Type ExistentialType::get(Type constraint) {
  auto &C = constraint->getASTContext();
  // ExistentialMetatypeType is already an existential type.
  if (constraint->is<ExistentialMetatypeType>())
    return constraint;

  bool printWithAny = true;
  if (constraint->isEqual(C.TheAnyType) || constraint->isAnyObject())
    printWithAny = false;

  auto properties = constraint->getRecursiveProperties();
  auto arena = getArena(properties);

  auto &entry = C.getImpl().getArena(arena).ExistentialTypes[constraint];
  if (entry)
    return entry;

  const ASTContext *canonicalContext = constraint->isCanonical() ? &C : nullptr;
  return entry = new (C, arena) ExistentialType(constraint, printWithAny,
                                                canonicalContext,
                                                properties);
}

BuiltinTupleType::BuiltinTupleType(BuiltinTupleDecl *TheDecl,
                                   const ASTContext &Ctx)
  : NominalType(TypeKind::BuiltinTuple, &Ctx, TheDecl, Type(),
                RecursiveTypeProperties()) { }

LValueType *LValueType::get(Type objectTy) {
  assert(!objectTy->is<LValueType>() && !objectTy->is<InOutType>() &&
         "cannot have 'inout' or @lvalue wrapped inside an @lvalue");

  auto properties = objectTy->getRecursiveProperties()
                    | RecursiveTypeProperties::IsLValue;
  auto arena = getArena(properties);

  auto &C = objectTy->getASTContext();
  auto &entry = C.getImpl().getArena(arena).LValueTypes[objectTy];
  if (entry)
    return entry;

  const ASTContext *canonicalContext = objectTy->isCanonical() ? &C : nullptr;
  return entry = new (C, arena) LValueType(objectTy, canonicalContext,
                                           properties);
}

InOutType *InOutType::get(Type objectTy) {
  assert(!objectTy->is<LValueType>() && !objectTy->is<InOutType>() &&
         "cannot have 'inout' or @lvalue wrapped inside an 'inout'");

  auto properties = objectTy->getRecursiveProperties();

  properties &= ~RecursiveTypeProperties::IsLValue;
  auto arena = getArena(properties);

  auto &C = objectTy->getASTContext();
  auto &entry = C.getImpl().getArena(arena).InOutTypes[objectTy];
  if (entry)
    return entry;

  const ASTContext *canonicalContext = objectTy->isCanonical() ? &C : nullptr;
  return entry = new (C, arena) InOutType(objectTy, canonicalContext,
                                          properties);
}

DependentMemberType *DependentMemberType::get(Type base, Identifier name) {
  auto properties = base->getRecursiveProperties();
  properties |= RecursiveTypeProperties::HasDependentMember;
  auto arena = getArena(properties);

  llvm::PointerUnion<Identifier, AssociatedTypeDecl *> stored(name);
  const ASTContext &ctx = base->getASTContext();
  auto *&known = ctx.getImpl().getArena(arena).DependentMemberTypes[
                                            {base, stored.getOpaqueValue()}];
  if (!known) {
    const ASTContext *canonicalCtx = base->isCanonical() ? &ctx : nullptr;
    known = new (ctx, arena) DependentMemberType(base, name, canonicalCtx,
                                                 properties);
  }
  return known;
}

DependentMemberType *DependentMemberType::get(Type base,
                                              AssociatedTypeDecl *assocType) {
  assert(assocType && "Missing associated type");
  auto properties = base->getRecursiveProperties();
  properties |= RecursiveTypeProperties::HasDependentMember;
  auto arena = getArena(properties);

  llvm::PointerUnion<Identifier, AssociatedTypeDecl *> stored(assocType);
  const ASTContext &ctx = base->getASTContext();
  auto *&known = ctx.getImpl().getArena(arena).DependentMemberTypes[
                                            {base, stored.getOpaqueValue()}];
  if (!known) {
    const ASTContext *canonicalCtx = base->isCanonical() ? &ctx : nullptr;
    known = new (ctx, arena) DependentMemberType(base, assocType, canonicalCtx,
                                                 properties);
  }
  return known;
}

/// Compute the recursive type properties of an opaque type archetype.
static RecursiveTypeProperties getOpaqueTypeArchetypeProperties(
    SubstitutionMap subs) {
  // An opaque type isn't contextually dependent like other archetypes, so
  // by itself, it doesn't impose the "Has Archetype" recursive property,
  // but the substituted types might. A disjoint "Has Opaque Archetype" tracks
  // the presence of opaque archetypes.
  RecursiveTypeProperties properties =
    RecursiveTypeProperties::HasOpaqueArchetype;
  for (auto type : subs.getReplacementTypes()) {
    properties |= type->getRecursiveProperties();
  }
  return properties;
}

OpaqueTypeArchetypeType *OpaqueTypeArchetypeType::getNew(
    GenericEnvironment *environment, Type interfaceType,
    ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
    LayoutConstraint layout) {
  auto properties = getOpaqueTypeArchetypeProperties(
      environment->getOpaqueSubstitutions());
  auto arena = getArena(properties);
  auto size = OpaqueTypeArchetypeType::totalSizeToAlloc<
      ProtocolDecl *, Type, LayoutConstraint>(
         conformsTo.size(), superclass ? 1 : 0, layout ? 1 : 0);
  ASTContext &ctx = interfaceType->getASTContext();
  auto mem = ctx.Allocate(size, alignof(OpaqueTypeArchetypeType), arena);
  return ::new (mem)
      OpaqueTypeArchetypeType(environment, properties, interfaceType,
                              conformsTo, superclass, layout);
}

Type OpaqueTypeArchetypeType::get(
    OpaqueTypeDecl *Decl, Type interfaceType, SubstitutionMap Substitutions) {
  auto *env = GenericEnvironment::forOpaqueType(Decl, Substitutions);
  return env->getOrCreateArchetypeFromInterfaceType(interfaceType);
}

CanTypeWrapper<OpenedArchetypeType> OpenedArchetypeType::getNew(
    GenericEnvironment *environment, Type interfaceType,
    ArrayRef<ProtocolDecl *> conformsTo, Type superclass,
    LayoutConstraint layout) {
  // FIXME: It'd be great if all of our callers could submit interface types.
  // But the constraint solver submits archetypes when e.g. trying to issue
  // checks against members of existential types.
  //  assert((!superclass || !superclass->hasArchetype())
  //         && "superclass must be interface type");
  auto arena = AllocationArena::Permanent;
  ASTContext &ctx = interfaceType->getASTContext();
  void *mem = ctx.Allocate(
    OpenedArchetypeType::totalSizeToAlloc<ProtocolDecl *,Type,LayoutConstraint>(
      conformsTo.size(),
      superclass ? 1 : 0,
      layout ? 1 : 0),
      alignof(OpenedArchetypeType), arena);

  return CanOpenedArchetypeType(::new (mem) OpenedArchetypeType(
      environment, interfaceType, conformsTo, superclass, layout));
}

CanTypeWrapper<OpenedArchetypeType>
OpenedArchetypeType::get(CanType existential, GenericSignature parentSig,
                         Optional<UUID> knownID) {
  assert(existential->isExistentialType());
  auto interfaceType = OpenedArchetypeType::getSelfInterfaceTypeFromContext(
      parentSig, existential->getASTContext());
  return get(existential, interfaceType, parentSig, knownID);
}

CanOpenedArchetypeType OpenedArchetypeType::get(CanType existential,
                                                Type interfaceType,
                                                GenericSignature parentSig,
                                                Optional<UUID> knownID) {
  assert(!interfaceType->hasArchetype() && "must be interface type");

  if (!knownID)
    knownID = UUID::fromTime();

  auto *genericEnv =
      GenericEnvironment::forOpenedExistential(existential, parentSig, *knownID);

  // Map the interface type into that environment.
  auto result = genericEnv->mapTypeIntoContext(interfaceType)
      ->castTo<OpenedArchetypeType>();
  return CanOpenedArchetypeType(result);
}

CanType OpenedArchetypeType::getAny(CanType existential, Type interfaceType,
                                    GenericSignature parentSig) {
  assert(existential->isAnyExistentialType());
  if (auto metatypeTy = existential->getAs<ExistentialMetatypeType>()) {
    auto instanceTy =
        metatypeTy->getExistentialInstanceType()->getCanonicalType();
    return CanMetatypeType::get(
        OpenedArchetypeType::getAny(instanceTy, interfaceType, parentSig));
  }
  assert(existential->isExistentialType());
  return OpenedArchetypeType::get(existential, interfaceType, parentSig);
}

CanType OpenedArchetypeType::getAny(CanType existential,
                                    GenericSignature parentSig) {
  auto interfaceTy = OpenedArchetypeType::getSelfInterfaceTypeFromContext(
      parentSig, existential->getASTContext());
  return getAny(existential, interfaceTy, parentSig);
}

void SubstitutionMap::Storage::Profile(
                               llvm::FoldingSetNodeID &id,
                               GenericSignature genericSig,
                               ArrayRef<Type> replacementTypes,
                               ArrayRef<ProtocolConformanceRef> conformances) {
  id.AddPointer(genericSig.getPointer());
  if (!genericSig) return;

  // Profile those replacement types that corresponding to canonical generic
  // parameters within the generic signature.
  id.AddInteger(replacementTypes.size());

  unsigned i = 0;
  genericSig->forEachParam([&](GenericTypeParamType *gp, bool canonical) {
    if (canonical)
      id.AddPointer(replacementTypes[i].getPointer());
    else
      id.AddPointer(nullptr);
    ++i;
  });

  // Conformances.
  id.AddInteger(conformances.size());
  for (auto conformance : conformances)
    id.AddPointer(conformance.getOpaqueValue());
}

SubstitutionMap::Storage *SubstitutionMap::Storage::get(
                            GenericSignature genericSig,
                            ArrayRef<Type> replacementTypes,
                            ArrayRef<ProtocolConformanceRef> conformances) {
  // If there is no generic signature, we need no storage.
  if (!genericSig) {
    assert(replacementTypes.empty());
    assert(conformances.empty());
    return nullptr;
  }

  // Figure out which arena this should go in.
  RecursiveTypeProperties properties;
  for (auto type : replacementTypes) {
    if (type)
      properties |= type->getRecursiveProperties();
  }

  // Profile the substitution map.
  llvm::FoldingSetNodeID id;
  SubstitutionMap::Storage::Profile(id, genericSig, replacementTypes,
                                    conformances);

  auto arena = getArena(properties);

  // Did we already record this substitution map?
  auto &ctx = genericSig->getASTContext();
  void *insertPos;
  auto &substitutionMaps = ctx.getImpl().getArena(arena).SubstitutionMaps;
  if (auto result = substitutionMaps.FindNodeOrInsertPos(id, insertPos))
    return result;

  // Allocate the appropriate amount of storage for the signature and its
  // replacement types and conformances.
  auto size = Storage::totalSizeToAlloc<Type, ProtocolConformanceRef>(
                                                      replacementTypes.size(),
                                                      conformances.size());
  auto mem = ctx.Allocate(size, alignof(Storage), arena);

  auto result = new (mem) Storage(genericSig, replacementTypes, conformances);
  substitutionMaps.InsertNode(result, insertPos);
  return result;
}

void GenericSignatureImpl::Profile(llvm::FoldingSetNodeID &ID,
                              TypeArrayView<GenericTypeParamType> genericParams,
                              ArrayRef<Requirement> requirements) {
  for (auto p : genericParams)
    ID.AddPointer(p);

  for (auto &reqt : requirements) {
    ID.AddPointer(reqt.getFirstType().getPointer());
    if (reqt.getKind() != RequirementKind::Layout)
      ID.AddPointer(reqt.getSecondType().getPointer());
    else
      ID.AddPointer(reqt.getLayoutConstraint().getPointer());
    ID.AddInteger(unsigned(reqt.getKind()));
  }
}

GenericSignature 
GenericSignature::get(ArrayRef<GenericTypeParamType *> params,
                      ArrayRef<Requirement> requirements,
                      bool isKnownCanonical) {
  SmallVector<Type, 4> paramTypes;
  for (auto param : params)
    paramTypes.push_back(param);
  auto paramsView = TypeArrayView<GenericTypeParamType>(paramTypes);
  return get(paramsView, requirements, isKnownCanonical);
}

GenericSignature
GenericSignature::get(TypeArrayView<GenericTypeParamType> params,
                      ArrayRef<Requirement> requirements,
                      bool isKnownCanonical) {
  assert(!params.empty());

#ifndef NDEBUG
  for (auto req : requirements) {
    assert(req.getFirstType()->isTypeParameter());
    assert(!req.getFirstType()->hasTypeVariable());
    assert(req.getKind() == RequirementKind::Layout ||
           !req.getSecondType()->hasTypeVariable());
  }
#endif

  // Check for an existing generic signature.
  llvm::FoldingSetNodeID ID;
  GenericSignatureImpl::Profile(ID, params, requirements);

  auto &ctx = getASTContext(params, requirements);
  void *insertPos;
  auto &sigs = ctx.getImpl().GenericSignatures;
  if (auto *sig = sigs.FindNodeOrInsertPos(ID, insertPos)) {
    if (isKnownCanonical)
      sig->CanonicalSignatureOrASTContext = &ctx;

    return sig;
  }

  // Allocate and construct the new signature.
  size_t bytes =
      GenericSignatureImpl::template totalSizeToAlloc<Type, Requirement>(
          params.size(), requirements.size());
  void *mem = ctx.Allocate(bytes, alignof(GenericSignatureImpl));
  auto *newSig =
      new (mem) GenericSignatureImpl(params, requirements, isKnownCanonical);
  ctx.getImpl().GenericSignatures.InsertNode(newSig, insertPos);
  return newSig;
}

GenericEnvironment *GenericEnvironment::forPrimary(GenericSignature signature) {
  auto &ctx = signature->getASTContext();

  // Allocate and construct the new environment.
  unsigned numGenericParams = signature.getGenericParams().size();
  size_t bytes = totalSizeToAlloc<OpaqueEnvironmentData,
                                  OpenedExistentialEnvironmentData,
                                  OpenedElementEnvironmentData, Type>(
      0, 0, 0, numGenericParams);
  void *mem = ctx.Allocate(bytes, alignof(GenericEnvironment));
  return new (mem) GenericEnvironment(signature);
}

/// Create a new generic environment for an opaque type with the given set of
/// outer substitutions.
GenericEnvironment *GenericEnvironment::forOpaqueType(
    OpaqueTypeDecl *opaque, SubstitutionMap subs) {
  // TODO: We could attempt to preserve type sugar in the substitution map.
  // Currently archetypes are assumed to be always canonical in many places,
  // though, so doing so would require fixing those places.
  subs = subs.getCanonical();

  auto &ctx = opaque->getASTContext();

  auto properties = getOpaqueTypeArchetypeProperties(subs);
  auto arena = getArena(properties);
  auto &environments
    = ctx.getImpl().getArena(arena).OpaqueArchetypeEnvironments;
  GenericEnvironment *env = environments[{opaque, subs}];

  if (!env) {
    // Allocate and construct the new environment.
    auto signature = opaque->getOpaqueInterfaceGenericSignature();
    unsigned numGenericParams = signature.getGenericParams().size();
    size_t bytes = totalSizeToAlloc<OpaqueEnvironmentData,
                                    OpenedExistentialEnvironmentData,
                                    OpenedElementEnvironmentData, Type>(
        1, 0, 0, numGenericParams);
    void *mem = ctx.Allocate(bytes, alignof(GenericEnvironment), arena);
    env = new (mem) GenericEnvironment(signature, opaque, subs);

    environments[{opaque, subs}] = env;
  }

  return env;
}

/// Create a new generic environment for an opened archetype.
GenericEnvironment *
GenericEnvironment::forOpenedExistential(
    Type existential, GenericSignature parentSig, UUID uuid) {
  assert(existential->isExistentialType());
  // FIXME: Opened archetypes can't be transformed because the
  // the identity of the archetype has to be preserved. This
  // means that simplifying an opened archetype in the constraint
  // system to replace type variables with fixed types is not
  // yet supported. For now, assert that an opened archetype never
  // contains type variables to catch cases where type variables
  // would be applied to the type-checked AST.
  assert(!existential->hasTypeVariable() &&
         "opened existentials containing type variables cannot be simplified");

  auto &ctx = existential->getASTContext();

  auto &openedExistentialEnvironments =
      ctx.getImpl().OpenedExistentialEnvironments;
  auto found = openedExistentialEnvironments.find(uuid);

  if (found != openedExistentialEnvironments.end()) {
    auto *existingEnv = found->second;
    assert(existingEnv->getOpenedExistentialType()->isEqual(existential));
    assert(existingEnv->getOpenedExistentialParentSignature().getPointer() == parentSig.getPointer());
    assert(existingEnv->getOpenedExistentialUUID() == uuid);

    return existingEnv;
  }

  auto signature = ctx.getOpenedExistentialSignature(existential, parentSig);

  // Allocate and construct the new environment.
  unsigned numGenericParams = signature.getGenericParams().size();
  size_t bytes = totalSizeToAlloc<OpaqueEnvironmentData,
                                  OpenedExistentialEnvironmentData,
                                  OpenedElementEnvironmentData, Type>(
      0, 1, 0, numGenericParams);
  void *mem = ctx.Allocate(bytes, alignof(GenericEnvironment));
  auto *genericEnv =
      new (mem) GenericEnvironment(signature, existential, parentSig, uuid);

  openedExistentialEnvironments[uuid] = genericEnv;

  return genericEnv;
}

/// Create a new generic environment for an element archetype.
GenericEnvironment *
GenericEnvironment::forOpenedElement(GenericSignature signature,
                                     UUID uuid,
                                     CanGenericTypeParamType shapeClass,
                                     SubstitutionMap outerSubs) {
  auto &ctx = signature->getASTContext();

  auto &openedElementEnvironments =
      ctx.getImpl().OpenedElementEnvironments;
  auto found = openedElementEnvironments.find(uuid);

  if (found != openedElementEnvironments.end()) {
    auto *existingEnv = found->second;
    assert(existingEnv->getGenericSignature().getPointer() == signature.getPointer());
    assert(existingEnv->getOpenedElementShapeClass()->isEqual(shapeClass));
    assert(existingEnv->getOpenedElementUUID() == uuid);

    return existingEnv;
  }

  // Allocate and construct the new environment.
  unsigned numGenericParams = signature.getGenericParams().size();
  unsigned numOpenedParams = signature.getInnermostGenericParams().size();
  size_t bytes = totalSizeToAlloc<OpaqueEnvironmentData,
                                  OpenedExistentialEnvironmentData,
                                  OpenedElementEnvironmentData,
                                  Type>(
      0, 0, 1, numGenericParams + numOpenedParams);
  void *mem = ctx.Allocate(bytes, alignof(GenericEnvironment));
  auto *genericEnv = new (mem) GenericEnvironment(signature,
                                                  uuid, shapeClass,
                                                  outerSubs);

  openedElementEnvironments[uuid] = genericEnv;

  return genericEnv;
}

void DeclName::CompoundDeclName::Profile(llvm::FoldingSetNodeID &id,
                                         DeclBaseName baseName,
                                         ArrayRef<Identifier> argumentNames) {
  id.AddPointer(baseName.getAsOpaquePointer());
  id.AddInteger(argumentNames.size());
  for (auto arg : argumentNames)
    id.AddPointer(arg.get());
}

void DeclName::initialize(ASTContext &C, DeclBaseName baseName,
                          ArrayRef<Identifier> argumentNames) {
  llvm::FoldingSetNodeID id;
  CompoundDeclName::Profile(id, baseName, argumentNames);

  void *insert = nullptr;
  if (CompoundDeclName *compoundName
        = C.getImpl().CompoundNames.FindNodeOrInsertPos(id, insert)) {
    BaseNameOrCompound = compoundName;
    return;
  }

  size_t size =
      CompoundDeclName::totalSizeToAlloc<Identifier>(argumentNames.size());
  auto buf = C.Allocate(size, alignof(CompoundDeclName));
  auto compoundName = new (buf) CompoundDeclName(baseName,argumentNames.size());
  std::uninitialized_copy(argumentNames.begin(), argumentNames.end(),
                          compoundName->getArgumentNames().begin());
  BaseNameOrCompound = compoundName;
  C.getImpl().CompoundNames.InsertNode(compoundName, insert);
}

/// Build a compound value name given a base name and a set of argument names
/// extracted from a parameter list.
DeclName::DeclName(ASTContext &C, DeclBaseName baseName,
                   ParameterList *paramList) {
  SmallVector<Identifier, 4> names;
  
  for (auto P : *paramList)
    names.push_back(P->getArgumentName());
  initialize(C, baseName, names);
}

/// Find the implementation of the named type in the given module.
static NominalTypeDecl *findUnderlyingTypeInModule(ASTContext &ctx, 
                                                   Identifier name,
                                                   ModuleDecl *module) {
  // Find all of the declarations with this name in the Swift module.
  SmallVector<ValueDecl *, 1> results;
  module->lookupValue(name, NLKind::UnqualifiedLookup, results);
  for (auto result : results) {
    if (auto nominal = dyn_cast<NominalTypeDecl>(result))
      return nominal;

    // Look through typealiases.
    if (auto typealias = dyn_cast<TypeAliasDecl>(result)) {
      return typealias->getDeclaredInterfaceType()->getAnyNominal();
    }
  }

  return nullptr;
}

bool ForeignRepresentationInfo::isRepresentableAsOptional() const {
  switch (getKind()) {
  case ForeignRepresentableKind::None:
    llvm_unreachable("this type is not representable");

  case ForeignRepresentableKind::Trivial:
    return Storage.getPointer() != 0;

  case ForeignRepresentableKind::Bridged: {
    auto KPK_ObjectiveCBridgeable = KnownProtocolKind::ObjectiveCBridgeable;
    ProtocolDecl *proto = getConformance()->getProtocol();
    assert(proto->isSpecificProtocol(KPK_ObjectiveCBridgeable) &&
           "unknown protocol; does it support optional?");
    (void)proto;
    (void)KPK_ObjectiveCBridgeable;

    return true;
  }

  case ForeignRepresentableKind::BridgedError:
    return true;

  case ForeignRepresentableKind::Object:
  case ForeignRepresentableKind::StaticBridged:
    llvm_unreachable("unexpected kind in ForeignRepresentableCacheEntry");
  }

  llvm_unreachable("Unhandled ForeignRepresentableKind in switch.");
}

ForeignRepresentationInfo
ASTContext::getForeignRepresentationInfo(NominalTypeDecl *nominal,
                                         ForeignLanguage language,
                                         const DeclContext *dc) {
  // Local function to add a type with the given name and module as
  // trivially-representable.
  auto addTrivial = [&](Identifier name, ModuleDecl *module,
                        bool allowOptional = false) {
    if (auto type = findUnderlyingTypeInModule(*this, name, module)) {
      auto info = ForeignRepresentationInfo::forTrivial();
      if (allowOptional)
        info = ForeignRepresentationInfo::forTrivialWithOptional();
      getImpl().ForeignRepresentableCache.insert({type, info});
    }
  };

  if (getImpl().ForeignRepresentableCache.empty()) {
    // Pre-populate the foreign-representable cache with known types.
    if (auto stdlib = getStdlibModule()) {
      addTrivial(getIdentifier("OpaquePointer"), stdlib, true);

      // Builtin types
      // FIXME: Layering violation to use the ClangImporter's define.
#define MAP_BUILTIN_TYPE(CLANG_BUILTIN_KIND, SWIFT_TYPE_NAME) \
      addTrivial(getIdentifier(#SWIFT_TYPE_NAME), stdlib);
#include "swift/ClangImporter/BuiltinMappedTypes.def"

      // Even though we may never import types directly as Int or UInt
      // (e.g. on 64-bit Windows, where CLong maps to Int32 and
      // CLongLong to Int64), it's always possible to convert an Int
      // or UInt to a C type.
      addTrivial(getIdentifier("Int"), stdlib);
      addTrivial(getIdentifier("UInt"), stdlib);
    }

    if (auto darwin = getLoadedModule(Id_Darwin)) {
      // Note: DarwinBoolean is odd because it's bridged to Bool in APIs,
      // but can also be trivially bridged.
      addTrivial(getIdentifier("DarwinBoolean"), darwin);
    }

    if (auto winsdk = getLoadedModule(Id_WinSDK)) {
      // NOTE: WindowsBool is odd because it is bridged to Bool in APIs, but can
      // also be trivially bridged.
      addTrivial(getIdentifier("WindowsBool"), winsdk);
    }

    if (auto objectiveC = getLoadedModule(Id_ObjectiveC)) {
      addTrivial(Id_Selector, objectiveC, true);

      // Note: ObjCBool is odd because it's bridged to Bool in APIs,
      // but can also be trivially bridged.
      addTrivial(getIdentifier("ObjCBool"), objectiveC);

      addTrivial(getSwiftId(KnownFoundationEntity::NSZone), objectiveC, true);
    }

    if (auto coreGraphics = getLoadedModule(getIdentifier("CoreGraphics"))) {
      addTrivial(Id_CGFloat, coreGraphics);
    }

    if (auto coreFoundation = getLoadedModule(getIdentifier("CoreFoundation"))) {
      addTrivial(Id_CGFloat, coreFoundation);
    }

    // Pull SIMD types of size 2...4 from the SIMD module, if it exists.
    if (auto simd = getLoadedModule(Id_simd)) {
#define MAP_SIMD_TYPE(BASENAME, _, __)                                  \
      {                                                                 \
        char name[] = #BASENAME "0";                                    \
        for (unsigned i = 2; i <= SWIFT_MAX_IMPORTED_SIMD_ELEMENTS; ++i) { \
          *(std::end(name) - 2) = '0' + i;                              \
          addTrivial(getIdentifier(name), simd);                        \
        }                                                               \
      }
#include "swift/ClangImporter/SIMDMappedTypes.def"      
    }
  }

  // Determine whether we know anything about this nominal type
  // yet. If we've never seen this nominal type before, or if we have
  // an out-of-date negative cached value, we'll have to go looking.
  auto known = getImpl().ForeignRepresentableCache.find(nominal);
  bool wasNotFoundInCache = known == getImpl().ForeignRepresentableCache.end();

  // For the REPL. We might have initialized the cache above before CoreGraphics
  // was loaded.
  //   let s = "" // Here we initialize the ForeignRepresentableCache.
  //   import Foundation
  //   let pt = CGPoint(x: 1.0, y: 2.0) // Here we query for CGFloat.
  // Add CGFloat as trivial if we encounter it later.
  // If the type was not found check if it would be found after having recently
  // loaded the module.
  // Similar for types for other non stdlib modules.
  auto conditionallyAddTrivial = [&](NominalTypeDecl *nominalDecl,
                                     Identifier typeName, Identifier moduleName,
                                     bool allowOptional = false) {
    if (nominal->getName() == typeName && wasNotFoundInCache) {
      if (auto module = getLoadedModule(moduleName)) {
        addTrivial(typeName, module, allowOptional);
        known = getImpl().ForeignRepresentableCache.find(nominal);
        wasNotFoundInCache = known == getImpl().ForeignRepresentableCache.end();
      }
    }
  };
  conditionallyAddTrivial(nominal, getIdentifier("DarwinBoolean") , Id_Darwin);
  conditionallyAddTrivial(nominal, getIdentifier("WindowsBool"), Id_WinSDK);
  conditionallyAddTrivial(nominal, Id_Selector, Id_ObjectiveC, true);
  conditionallyAddTrivial(nominal, getIdentifier("ObjCBool"), Id_ObjectiveC);
  conditionallyAddTrivial(nominal, getSwiftId(KnownFoundationEntity::NSZone), Id_ObjectiveC, true);
  conditionallyAddTrivial(nominal, Id_CGFloat, getIdentifier("CoreGraphics"));
  conditionallyAddTrivial(nominal, Id_CGFloat, getIdentifier("CoreFoundation"));
#define MAP_SIMD_TYPE(BASENAME, _, __)                                         \
  {                                                                            \
    char name[] = #BASENAME "0";                                               \
    for (unsigned i = 2; i <= SWIFT_MAX_IMPORTED_SIMD_ELEMENTS; ++i) {         \
      *(std::end(name) - 2) = '0' + i;                                         \
      conditionallyAddTrivial(nominal, getIdentifier(name), Id_simd);          \
    }                                                                          \
  }
#include "swift/ClangImporter/SIMDMappedTypes.def"

  if (wasNotFoundInCache ||
      (known->second.getKind() == ForeignRepresentableKind::None &&
       known->second.getGeneration() < CurrentGeneration)) {
    Optional<ForeignRepresentationInfo> result;

    // Look for a conformance to _ObjectiveCBridgeable (other than Optional's--
    // we don't want to allow exposing APIs with double-optional types like
    // NSObject??, even though Optional is bridged to its underlying type).
    //
    // FIXME: We're implicitly depending on the fact that lookupConformance
    // is global, ignoring the module we provide for it.
    if (nominal != dc->getASTContext().getOptionalDecl()) {
      if (auto objcBridgeable
            = getProtocol(KnownProtocolKind::ObjectiveCBridgeable)) {
        auto conformance = dc->getParentModule()->lookupConformance(
            nominal->getDeclaredInterfaceType(), objcBridgeable);
        if (conformance) {
          result =
              ForeignRepresentationInfo::forBridged(conformance.getConcrete());
        }
      }
    }

    // Error is bridged to NSError, when it's available.
    if (nominal == getErrorDecl() && getNSErrorDecl())
      result = ForeignRepresentationInfo::forBridgedError();

    // If we didn't find anything, mark the result as "None".
    if (!result)
      result = ForeignRepresentationInfo::forNone(CurrentGeneration);
    
    // Cache the result.
    known = getImpl().ForeignRepresentableCache.insert({ nominal, *result }).first;
  }

  // Map a cache entry to a result for this specific 
  auto entry = known->second;
  if (entry.getKind() == ForeignRepresentableKind::None)
    return entry;

  // Extract the protocol conformance.
  auto conformance = entry.getConformance();

  // If the conformance is not visible, fail.
  if (conformance && !conformance->isVisibleFrom(dc))
    return ForeignRepresentationInfo::forNone();

  // Language-specific filtering.
  switch (language) {
  case ForeignLanguage::C:
    // Ignore _ObjectiveCBridgeable conformances in C.
    if (conformance &&
        conformance->getProtocol()->isSpecificProtocol(
          KnownProtocolKind::ObjectiveCBridgeable))
      return ForeignRepresentationInfo::forNone();

    // Ignore error bridging in C.
    if (entry.getKind() == ForeignRepresentableKind::BridgedError)
      return ForeignRepresentationInfo::forNone();

    LLVM_FALLTHROUGH;

  case ForeignLanguage::ObjectiveC:
    return entry;
  }

  llvm_unreachable("Unhandled ForeignLanguage in switch.");
}

bool ASTContext::isTypeBridgedInExternalModule(
     NominalTypeDecl *nominal) const {
  return (nominal == getBoolDecl() ||
          nominal == getIntDecl() ||
          nominal == getInt64Decl() ||
          nominal == getInt32Decl() ||
          nominal == getInt16Decl() ||
          nominal == getInt8Decl() ||
          nominal == getUIntDecl() ||
          nominal == getUInt64Decl() ||
          nominal == getUInt32Decl() ||
          nominal == getUInt16Decl() ||
          nominal == getUInt8Decl() ||
          nominal == getFloatDecl() ||
          nominal == getDoubleDecl() ||
          nominal == getArrayDecl() ||
          nominal == getCollectionDifferenceDecl() ||
          (nominal->getDeclContext()->getAsDecl() ==
            getCollectionDifferenceDecl() &&
            nominal->getBaseName() == Id_Change) ||
          nominal == getDictionaryDecl() ||
          nominal == getSetDecl() ||
          nominal == getStringDecl() ||
          nominal == getSubstringDecl() ||
          nominal == getErrorDecl() ||
          nominal == getAnyHashableDecl() ||
          // Foundation's overlay depends on the CoreGraphics overlay, but
          // CoreGraphics value types bridge to Foundation objects such as
          // NSValue and NSNumber, so to avoid circular dependencies, the
          // bridging implementations of CG types appear in the Foundation
          // module.
          nominal->getParentModule()->getName() == Id_CoreGraphics ||
          nominal->getParentModule()->getName() == Id_CoreFoundation ||
          // CoreMedia is a dependency of AVFoundation, but the bridged
          // NSValue implementations for CMTime, CMTimeRange, and
          // CMTimeMapping are provided by AVFoundation, and AVFoundation
          // gets upset if you don't use the NSValue subclasses its factory
          // methods instantiate.
          nominal->getParentModule()->getName() == Id_CoreMedia);
}

bool ASTContext::isObjCClassWithMultipleSwiftBridgedTypes(Type t) {
  auto clazz = t->getClassOrBoundGenericClass();
  if (!clazz)
    return false;
  
  if (clazz == getNSErrorDecl())
    return true;
  if (clazz == getNSNumberDecl())
    return true;
  if (clazz == getNSValueDecl())
    return true;
  
  return false;
}

Type ASTContext::getBridgedToObjC(const DeclContext *dc, Type type,
                                  Type *bridgedValueType) const {
  if (type->isBridgeableObjectType()) {
    if (bridgedValueType) *bridgedValueType = type;

    return type;
  }

  if (auto metaTy = type->getAs<MetatypeType>())
    if (metaTy->getInstanceType()->mayHaveSuperclass())
      return type;

  if (auto existentialMetaTy = type->getAs<ExistentialMetatypeType>())
    if (existentialMetaTy->getInstanceType()->isObjCExistentialType())
      return type;

  // Check whether the type is an existential that contains
  // Error. If so, it's bridged to NSError.
  if (type->isExistentialWithError()) {
    if (auto nsErrorTy = getNSErrorType()) {
      // The corresponding value type is Error.
      if (bridgedValueType)
        *bridgedValueType = getErrorExistentialType();

      return nsErrorTy;
    }
  }

  // Try to find a conformance that will enable bridging.
  auto findConformance =
      [&](KnownProtocolKind known) -> ProtocolConformanceRef {
    // Don't ascribe any behavior to Optional other than what we explicitly
    // give it. We don't want things like AnyObject?? to work.
    if (type->isOptional())
      return ProtocolConformanceRef::forInvalid();

    // Find the protocol.
    auto proto = getProtocol(known);
    if (!proto)
      return ProtocolConformanceRef::forInvalid();

    return dc->getParentModule()->lookupConformance(type, proto);
  };

  // Do we conform to _ObjectiveCBridgeable?
  if (auto conformance =
          findConformance(KnownProtocolKind::ObjectiveCBridgeable)) {
    // The corresponding value type is... the type.
    if (bridgedValueType)
      *bridgedValueType = type;

    // Find the Objective-C class type we bridge to.
    Type witnessTy = conformance.getTypeWitnessByName(type, Id_ObjectiveCType);
    // If Objective-C import is broken, witness type would be a dependent member
    // with `<<error type>>` base.
    return (witnessTy && !witnessTy->hasError()) ? witnessTy : Type();
  }

  // Do we conform to Error?
  if (findConformance(KnownProtocolKind::Error)) {
    // The corresponding value type is Error.
    if (bridgedValueType)
      *bridgedValueType = getErrorExistentialType();

    // Bridge to NSError.
    if (auto nsErrorTy = getNSErrorType())
      return nsErrorTy;
  }

  // No special bridging to Objective-C, but this can become an 'Any'.
  return Type();
}

ClangTypeConverter &ASTContext::getClangTypeConverter() {
  auto &impl = getImpl();
  if (!impl.Converter) {
    auto *cml = getClangModuleLoader();
    impl.Converter.emplace(*this, cml->getClangASTContext(), LangOpts.Target);
  }
  return impl.Converter.value();
}

const clang::Type *
ASTContext::getClangFunctionType(ArrayRef<AnyFunctionType::Param> params,
                                 Type resultTy,
                                 FunctionTypeRepresentation trueRep) {
  return getClangTypeConverter().getFunctionType(params, resultTy, trueRep);
}

const clang::Type *
ASTContext::getCanonicalClangFunctionType(
    ArrayRef<SILParameterInfo> params,
    Optional<SILResultInfo> result,
    SILFunctionType::Representation trueRep) {
  auto *ty = getClangTypeConverter().getFunctionType(params, result, trueRep);
  return ty ? ty->getCanonicalTypeInternal().getTypePtr() : nullptr;
}

std::unique_ptr<TemplateInstantiationError>
ASTContext::getClangTemplateArguments(
    const clang::TemplateParameterList *templateParams,
    ArrayRef<Type> genericArgs,
    SmallVectorImpl<clang::TemplateArgument> &templateArgs) {
  auto &impl = getImpl();
  if (!impl.Converter) {
    auto *cml = getClangModuleLoader();
    impl.Converter.emplace(*this, cml->getClangASTContext(), LangOpts.Target);
  }

  return impl.Converter->getClangTemplateArguments(templateParams, genericArgs,
                                                   templateArgs);
}

const Decl *
ASTContext::getSwiftDeclForExportedClangDecl(const clang::Decl *decl) {
  auto &impl = getImpl();

  // If we haven't exported anything yet, this must not be how we found
  // this declaration.
  if (!impl.Converter) return nullptr;

  return impl.Converter->getSwiftDeclForExportedClangDecl(decl);
}

const clang::Type *
ASTContext::getClangTypeForIRGen(Type ty) {
  return getClangTypeConverter().convert(ty).getTypePtrOrNull();
}

GenericParamList *ASTContext::getSelfGenericParamList(DeclContext *dc) const {
  auto *theParamList = getImpl().SelfGenericParamList;
  if (theParamList)
    return theParamList;

  // Note: we always return a GenericParamList rooted at the first
  // DeclContext this was called with. Since this is just a giant
  // hack for SIL mode, that should be OK.
  auto *selfParam = GenericTypeParamDecl::createImplicit(
      dc, Id_Self, /*depth*/ 0, /*index*/ 0);

  theParamList = GenericParamList::create(
      const_cast<ASTContext &>(*this), SourceLoc(), {selfParam}, SourceLoc());
  getImpl().SelfGenericParamList = theParamList;

  return theParamList;
}

CanGenericSignature ASTContext::getSingleGenericParameterSignature() const {
  if (auto theSig = getImpl().SingleGenericParameterSignature)
    return theSig;

  auto param = GenericTypeParamType::get(/*isParameterPack*/ false,
                                         /*depth*/ 0, /*index*/ 0, *this);
  auto sig = GenericSignature::get(param, { });
  auto canonicalSig = CanGenericSignature(sig);
  getImpl().SingleGenericParameterSignature = canonicalSig;
  return canonicalSig;
}

Type OpenedArchetypeType::getSelfInterfaceTypeFromContext(GenericSignature parentSig,
                                                          ASTContext &ctx) {
  unsigned depth = 0;
  if (!parentSig.getGenericParams().empty())
    depth = parentSig.getGenericParams().back()->getDepth() + 1;
  return GenericTypeParamType::get(/*isParameterPack=*/ false,
                                   /*depth=*/ depth, /*index=*/ 0,
                                   ctx);
}

CanGenericSignature
ASTContext::getOpenedExistentialSignature(Type type, GenericSignature parentSig) {
  assert(type->isExistentialType());

  if (auto existential = type->getAs<ExistentialType>())
    type = existential->getConstraintType();

  const CanType constraint = type->getCanonicalType();
  assert(parentSig || !constraint->hasTypeParameter() &&
         "Interface type here requires a parent signature");

  // The opened archetype signature for a protocol type is identical
  // to the protocol's own canonical generic signature if there aren't any
  // outer generic parameters to worry about.
  if (parentSig.isNull()) {
    if (const auto protoTy = dyn_cast<ProtocolType>(constraint)) {
      return protoTy->getDecl()->getGenericSignature().getCanonicalSignature();
    }
  }

  // Otherwise we need to build a generic signature that captures any outer
  // generic parameters. This ensures that we keep e.g. generic superclass
  // existentials contained in a well-formed generic context.
  auto canParentSig = parentSig.getCanonicalSignature();
  auto key = std::make_pair(constraint, canParentSig.getPointer());
  auto found = getImpl().ExistentialSignatures.find(key);
  if (found != getImpl().ExistentialSignatures.end())
    return found->second;

  auto genericParam = OpenedArchetypeType::getSelfInterfaceTypeFromContext(
      canParentSig, type->getASTContext())
    ->castTo<GenericTypeParamType>();
  Requirement requirement(RequirementKind::Conformance, genericParam,
                          constraint);
  auto genericSig = buildGenericSignature(
      *this, canParentSig,
      {genericParam}, {requirement});

  CanGenericSignature canGenericSig(genericSig);

  auto result = getImpl().ExistentialSignatures.insert(
      std::make_pair(key, canGenericSig));
  assert(result.second);
  (void) result;

  return canGenericSig;
}

CanGenericSignature
ASTContext::getOpenedElementSignature(CanGenericSignature baseGenericSig,
                                      CanGenericTypeParamType shapeClass) {
  auto &sigs = getImpl().ElementSignatures;
  auto key = std::make_pair(shapeClass, baseGenericSig.getPointer());
  auto found = sigs.find(key);
  if (found != sigs.end())
    return found->second;

  // This operation doesn't make sense if the input signature does not contain`
  // any pack generic parameters.
#ifndef NDEBUG
  {
    auto found = std::find_if(baseGenericSig.getGenericParams().begin(),
                              baseGenericSig.getGenericParams().end(),
                              [](GenericTypeParamType *paramType) {
                                return paramType->isParameterPack();
                              });
    assert(found != baseGenericSig.getGenericParams().end());
  }
#endif

  // The pack element signature includes all type parameters and requirements
  // from the outer context, plus a new set of type parameters representing
  // open pack elements and their corresponding element requirements.

  llvm::SmallMapVector<GenericTypeParamType *,
                       GenericTypeParamType *, 2> packElementParams;
  SmallVector<GenericTypeParamType *, 2> genericParams(
      baseGenericSig.getGenericParams().begin(), baseGenericSig.getGenericParams().end());
  SmallVector<Requirement, 2> requirements;

  auto packElementDepth =
      baseGenericSig.getInnermostGenericParams().front()->getDepth() + 1;

  for (auto paramType : baseGenericSig.getGenericParams()) {
    if (!paramType->isParameterPack())
      continue;

    // Only include opened element parameters for packs in the given
    // shape equivalence class.
    if (!baseGenericSig->haveSameShape(paramType, shapeClass))
      continue;

    auto *elementParam = GenericTypeParamType::get(/*isParameterPack*/false,
                                                   packElementDepth,
                                                   packElementParams.size(),
                                                   *this);
    genericParams.push_back(elementParam);
    packElementParams[paramType] = elementParam;
  }

  auto eraseParameterPackRec = [&](Type type) -> Type {
    return type.transformRec([&](Type t) -> Optional<Type> {
      if (auto *paramType = t->getAs<GenericTypeParamType>()) {
        if (packElementParams.find(paramType) != packElementParams.end()) {
          return Type(packElementParams[paramType]);
        }

        return t;
      }
      return None;
    });
  };

  for (auto requirement : baseGenericSig.getRequirements()) {
    requirements.push_back(requirement);

    // If this requirement contains parameter packs, create a new requirement
    // for the corresponding pack element.
    switch (requirement.getKind()) {
    case RequirementKind::SameShape:
      // Drop same-shape requirements from the element signature.
      break;
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
    case RequirementKind::SameType: {
      auto firstType = eraseParameterPackRec(requirement.getFirstType());
      auto secondType = eraseParameterPackRec(requirement.getSecondType());
      if (firstType->isEqual(requirement.getFirstType()) &&
          secondType->isEqual(requirement.getSecondType()))
        break;

      requirements.emplace_back(requirement.getKind(),
                                firstType, secondType);
      break;
    }
    case RequirementKind::Layout: {
      auto firstType = eraseParameterPackRec(requirement.getFirstType());
      if (firstType->isEqual(requirement.getFirstType()))
        break;

      requirements.emplace_back(requirement.getKind(), firstType,
                                requirement.getLayoutConstraint());
      break;
    }
    }
  }

  auto elementSig = buildGenericSignature(
      *this, GenericSignature(), genericParams, requirements)
          .getCanonicalSignature();
  sigs[key] = elementSig;
  return elementSig;
}

GenericSignature 
ASTContext::getOverrideGenericSignature(const ValueDecl *base,
                                        const ValueDecl *derived) {
  assert(isa<AbstractFunctionDecl>(base) || isa<SubscriptDecl>(base));
  assert(isa<AbstractFunctionDecl>(derived) || isa<SubscriptDecl>(derived));

  const auto baseNominal = base->getDeclContext()->getSelfNominalTypeDecl();
  const auto derivedNominal = derived->getDeclContext()->getSelfNominalTypeDecl();

  assert(baseNominal != nullptr);
  assert(derivedNominal != nullptr);

  const auto baseGenericSig =
      base->getAsGenericContext()->getGenericSignature();
  const auto *derivedParams =
      derived->getAsGenericContext()->getGenericParams();

  return getOverrideGenericSignature(baseNominal, derivedNominal,
                                     baseGenericSig, derivedParams);
}

GenericSignature
ASTContext::getOverrideGenericSignature(const NominalTypeDecl *baseNominal,
                                        const NominalTypeDecl *derivedNominal,
                                        GenericSignature baseGenericSig,
                                        const GenericParamList *derivedParams) {
  if (baseNominal == derivedNominal)
    return baseGenericSig;

  const auto derivedNominalSig = derivedNominal->getGenericSignature();

  if (derivedNominalSig.isNull() && derivedParams == nullptr)
    return nullptr;

  if (baseGenericSig.isNull())
    return derivedNominalSig;

  auto key = OverrideSignatureKey(baseGenericSig,
                                  baseNominal,
                                  derivedNominal,
                                  derivedParams);

  if (getImpl().overrideSigCache.find(key) !=
      getImpl().overrideSigCache.end()) {
    return getImpl().overrideSigCache.lookup(key);
  }

  SmallVector<GenericTypeParamType *, 2> addedGenericParams;
  if (derivedParams) {
    for (auto gp : *derivedParams) {
      addedGenericParams.push_back(
          gp->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
    }
  }

  SmallVector<Requirement, 2> addedRequirements;

  OverrideSubsInfo info(baseNominal, derivedNominal,
                        baseGenericSig, derivedParams);

  for (auto reqt : baseGenericSig.getRequirements()) {
    auto substReqt = reqt.subst(QueryOverrideSubs(info),
                                LookUpConformanceInOverrideSubs(info));
    addedRequirements.push_back(substReqt);
  }

  auto genericSig = buildGenericSignature(*this, derivedNominalSig,
                                          std::move(addedGenericParams),
                                          std::move(addedRequirements));
  getImpl().overrideSigCache.insert(std::make_pair(key, genericSig));
  return genericSig;
}

bool ASTContext::overrideGenericSignatureReqsSatisfied(
    const ValueDecl *base, const ValueDecl *derived,
    const OverrideGenericSignatureReqCheck direction) {
  auto *baseCtx = base->getAsGenericContext();
  auto *derivedCtx = derived->getAsGenericContext();

  if (baseCtx->isGeneric() != derivedCtx->isGeneric())
    return false;

  if (baseCtx->isGeneric() &&
      (baseCtx->getGenericParams()->size() !=
       derivedCtx->getGenericParams()->size()))
    return false;

  auto sig = getOverrideGenericSignature(base, derived);
  if (!sig)
    return true;

  auto derivedSig = derivedCtx->getGenericSignature();

  switch (direction) {
  case OverrideGenericSignatureReqCheck::BaseReqSatisfiedByDerived:
    return sig.requirementsNotSatisfiedBy(derivedSig).empty();
  case OverrideGenericSignatureReqCheck::DerivedReqSatisfiedByBase:
    return derivedSig.requirementsNotSatisfiedBy(sig).empty();
  }
  llvm_unreachable("Unhandled OverrideGenericSignatureReqCheck in switch");
}

void ASTContext::registerIRGenSILTransforms(SILTransformCtors ctors) {
  assert(getImpl().IRGenSILPasses.empty() && "Already registered");
  getImpl().IRGenSILPasses = ctors;
}

ASTContext::SILTransformCtors ASTContext::getIRGenSILTransforms() const {
  auto passes = getImpl().IRGenSILPasses;
  assert(!passes.empty() && "Didn't register the necessary passes");
  return passes;
}

std::string ASTContext::getEntryPointFunctionName() const {
  return LangOpts.entryPointFunctionName;
}

SILLayout *SILLayout::get(ASTContext &C,
                          CanGenericSignature Generics,
                          ArrayRef<SILField> Fields,
                          bool CapturesGenericEnvironment) {
  // The "captures generic environment" flag is meaningless if there are
  // no generic arguments to capture.
  if (!Generics || Generics->areAllParamsConcrete()) {
    CapturesGenericEnvironment = false;
  }
  
  // Profile the layout parameters.
  llvm::FoldingSetNodeID id;
  Profile(id, Generics, Fields, CapturesGenericEnvironment);
  
  // Return an existing layout if there is one.
  void *insertPos;
  auto &Layouts = C.getImpl().SILLayouts;
  
  if (auto existing = Layouts.FindNodeOrInsertPos(id, insertPos))
    return existing;
  
  // Allocate a new layout.
  void *memory = C.Allocate(totalSizeToAlloc<SILField>(Fields.size()),
                            alignof(SILLayout));
  
  auto newLayout = ::new (memory) SILLayout(Generics, Fields,
                                            CapturesGenericEnvironment);
  Layouts.InsertNode(newLayout, insertPos);
  return newLayout;
}

CanSILBoxType SILBoxType::get(ASTContext &C,
                              SILLayout *Layout,
                              SubstitutionMap Substitutions) {
  // Canonicalize substitutions.
  Substitutions = Substitutions.getCanonical();

  // Return an existing layout if there is one.
  void *insertPos;
  auto &SILBoxTypes = C.getImpl().SILBoxTypes;
  llvm::FoldingSetNodeID id;
  Profile(id, Layout, Substitutions);
  if (auto existing = SILBoxTypes.FindNodeOrInsertPos(id, insertPos))
    return CanSILBoxType(existing);

  auto newBox = new (C, AllocationArena::Permanent) SILBoxType(C, Layout,
                                                               Substitutions);
  SILBoxTypes.InsertNode(newBox, insertPos);
  return CanSILBoxType(newBox);
}

/// TODO: Transitional factory to present the single-type SILBoxType::get
/// interface.
CanSILBoxType SILBoxType::get(CanType boxedType) {
  auto &ctx = boxedType->getASTContext();
  auto singleGenericParamSignature = ctx.getSingleGenericParameterSignature();
  auto genericParam = singleGenericParamSignature.getGenericParams()[0];
  auto layout = SILLayout::get(ctx, singleGenericParamSignature,
                               SILField(CanType(genericParam),
                                        /*mutable*/ true),
                               /*captures generic env*/ false);

  auto subMap =
    SubstitutionMap::get(
      singleGenericParamSignature,
      [&](SubstitutableType *type) -> Type {
        if (type->isEqual(genericParam)) return boxedType;

        return nullptr;
      },
      MakeAbstractConformanceForGenericType());
  return get(boxedType->getASTContext(), layout, subMap);
}

LayoutConstraint
LayoutConstraint::getLayoutConstraint(LayoutConstraintKind Kind,
                                      ASTContext &C) {
  return getLayoutConstraint(Kind, 0, 0, C);
}

LayoutConstraint LayoutConstraint::getLayoutConstraint(LayoutConstraintKind Kind,
                                                      unsigned SizeInBits,
                                                      unsigned Alignment,
                                                      ASTContext &C) {
  if (!LayoutConstraintInfo::isKnownSizeTrivial(Kind)) {
    assert(SizeInBits == 0);
    assert(Alignment == 0);
    return getLayoutConstraint(Kind);
  }

  // Check to see if we've already seen this tuple before.
  llvm::FoldingSetNodeID ID;
  LayoutConstraintInfo::Profile(ID, Kind, SizeInBits, Alignment);

  void *InsertPos = nullptr;
  if (LayoutConstraintInfo *Layout =
          C.getImpl().getArena(AllocationArena::Permanent)
              .LayoutConstraints.FindNodeOrInsertPos(ID, InsertPos))
    return LayoutConstraint(Layout);

  LayoutConstraintInfo *New =
      LayoutConstraintInfo::isTrivial(Kind)
          ? new (C, AllocationArena::Permanent)
                LayoutConstraintInfo(Kind, SizeInBits, Alignment)
          : new (C, AllocationArena::Permanent) LayoutConstraintInfo(Kind);
  C.getImpl().getArena(AllocationArena::Permanent)
      .LayoutConstraints.InsertNode(New, InsertPos);
  return LayoutConstraint(New);
}

Type &ASTContext::getDefaultTypeRequestCache(SourceFile *SF,
                                             KnownProtocolKind kind) {
  return getImpl().DefaultTypeRequestCaches[SF][size_t(kind)];
}

Type ASTContext::getSideCachedPropertyWrapperBackingPropertyType(
    VarDecl *var) const {
  return getImpl().PropertyWrapperBackingVarTypes[var];
}

void ASTContext::setSideCachedPropertyWrapperBackingPropertyType(
    VarDecl *var, Type type) {
  assert(!getImpl().PropertyWrapperBackingVarTypes[var] ||
         getImpl().PropertyWrapperBackingVarTypes[var]->isEqual(type));
  getImpl().PropertyWrapperBackingVarTypes[var] = type;
}

VarDecl *VarDecl::getOriginalWrappedProperty(
    Optional<PropertyWrapperSynthesizedPropertyKind> kind) const {
  if (!Bits.VarDecl.IsPropertyWrapperBackingProperty)
    return nullptr;

  ASTContext &ctx = getASTContext();
  assert(ctx.getImpl().OriginalWrappedProperties.count(this) > 0);
  auto original = ctx.getImpl().OriginalWrappedProperties[this];
  if (!kind)
    return original;

  auto wrapperInfo = original->getPropertyWrapperAuxiliaryVariables();
  switch (*kind) {
  case PropertyWrapperSynthesizedPropertyKind::Backing:
    return this == wrapperInfo.backingVar ? original : nullptr;

  case PropertyWrapperSynthesizedPropertyKind::Projection:
    return this == wrapperInfo.projectionVar ? original : nullptr;
  }
  llvm_unreachable("covered switch");
}

void VarDecl::setOriginalWrappedProperty(VarDecl *originalProperty) {
  Bits.VarDecl.IsPropertyWrapperBackingProperty = true;
  ASTContext &ctx = getASTContext();
  assert(ctx.getImpl().OriginalWrappedProperties.count(this) == 0);
  ctx.getImpl().OriginalWrappedProperties[this] = originalProperty;
}

#ifndef NDEBUG
static bool isSourceLocInOrignalBuffer(const Decl *D, SourceLoc Loc) {
  assert(Loc.isValid());
  auto bufferID = D->getDeclContext()->getParentSourceFile()->getBufferID();
  assert(bufferID.has_value() && "Source buffer ID must be set");
  auto &SM = D->getASTContext().SourceMgr;
  return SM.getRangeForBuffer(*bufferID).contains(Loc);
}
#endif

void AbstractFunctionDecl::keepOriginalBodySourceRange() {
  auto &impl = getASTContext().getImpl();
  auto result =
      impl.OriginalBodySourceRanges.insert({this, getBodySourceRange()});
  assert((!result.second ||
          isSourceLocInOrignalBuffer(this, result.first->getSecond().Start)) &&
         "This function must be called before setting new body range");
  (void)result;
}

SourceRange AbstractFunctionDecl::getOriginalBodySourceRange() const {
  auto &impl = getASTContext().getImpl();
  auto found = impl.OriginalBodySourceRanges.find(this);
  if (found != impl.OriginalBodySourceRanges.end()) {
    return found->getSecond();
  } else {
    return getBodySourceRange();
  }
}

IndexSubset *
IndexSubset::get(ASTContext &ctx, const SmallBitVector &indices) {
  auto &foldingSet = ctx.getImpl().IndexSubsets;
  llvm::FoldingSetNodeID id;
  unsigned capacity = indices.size();
  id.AddInteger(capacity);
  for (unsigned index : indices.set_bits())
    id.AddInteger(index);
  void *insertPos = nullptr;
  auto *existing = foldingSet.FindNodeOrInsertPos(id, insertPos);
  if (existing)
    return existing;
  auto sizeToAlloc = sizeof(IndexSubset) +
      getNumBytesNeededForCapacity(capacity);
  auto *buf = reinterpret_cast<IndexSubset *>(
      ctx.Allocate(sizeToAlloc, alignof(IndexSubset)));
  auto *newNode = new (buf) IndexSubset(indices);
  foldingSet.InsertNode(newNode, insertPos);
  return newNode;
}

AutoDiffDerivativeFunctionIdentifier *AutoDiffDerivativeFunctionIdentifier::get(
    AutoDiffDerivativeFunctionKind kind, IndexSubset *parameterIndices,
    GenericSignature derivativeGenericSignature, ASTContext &C) {
  assert(parameterIndices);
  auto &foldingSet = C.getImpl().AutoDiffDerivativeFunctionIdentifiers;
  llvm::FoldingSetNodeID id;
  id.AddInteger((unsigned)kind);
  id.AddPointer(parameterIndices);
  auto derivativeCanGenSig = derivativeGenericSignature.getCanonicalSignature();
  id.AddPointer(derivativeCanGenSig.getPointer());

  void *insertPos;
  auto *existing = foldingSet.FindNodeOrInsertPos(id, insertPos);
  if (existing)
    return existing;

  void *mem = C.Allocate(sizeof(AutoDiffDerivativeFunctionIdentifier),
                         alignof(AutoDiffDerivativeFunctionIdentifier));
  auto *newNode = ::new (mem) AutoDiffDerivativeFunctionIdentifier(
      kind, parameterIndices, derivativeGenericSignature);
  foldingSet.InsertNode(newNode, insertPos);

  return newNode;
}

llvm::LLVMContext &ASTContext::getIntrinsicScratchContext() const {
  return *getImpl().IntrinsicScratchContext.get();
}

bool ASTContext::isASCIIString(StringRef s) const {
  for (unsigned char c : s) {
    if (c > 127) {
      return false;
    }
  }
  return true;
}

/// The special Builtin.TheTupleType, which parents tuple extensions and
/// conformances.
BuiltinTupleDecl *ASTContext::getBuiltinTupleDecl() {
  auto &result = getImpl().TheTupleTypeDecl;

  if (result)
    return result;

  result = new (*this) BuiltinTupleDecl(Id_TheTupleType,
                                        TheBuiltinModule->getFiles()[0]);
  result->setAccess(AccessLevel::Public);

  return result;
}

/// The declared interface type of Builtin.TheTupleType.
BuiltinTupleType *ASTContext::getBuiltinTupleType() {
  auto &result = getImpl().TheTupleType;

  if (result)
    return result;

  result = new (*this) BuiltinTupleType(getBuiltinTupleDecl(), *this);

  return result;
}

void ASTContext::setPluginLoader(std::unique_ptr<PluginLoader> loader) {
  getImpl().Plugins = std::move(loader);
}

PluginLoader &ASTContext::getPluginLoader() {
  assert(getImpl().Plugins && "PluginLoader must be setup before using");
  return *getImpl().Plugins;
}

Type ASTContext::getNamedSwiftType(ModuleDecl *module, StringRef name) {
  if (!module)
    return Type();

  // Look for the type.
  Identifier identifier = getIdentifier(name);
  SmallVector<ValueDecl *, 2> results;

  // Check if the lookup we're about to perform a lookup within is
  // a Clang module.
  for (auto *file : module->getFiles()) {
    if (auto clangUnit = dyn_cast<ClangModuleUnit>(file)) {
      // If we have an overlay, look in the overlay. Otherwise, skip
      // the lookup to avoid infinite recursion.
      if (auto module = clangUnit->getOverlayModule())
        module->lookupValue(identifier, NLKind::UnqualifiedLookup, results);
    } else {
      file->lookupValue(identifier, NLKind::UnqualifiedLookup, { }, results);
    }
  }

  if (results.size() != 1)
    return Type();

  auto decl = dyn_cast<TypeDecl>(results.front());
  if (!decl)
    return Type();

  assert(!decl->hasClangNode() && "picked up the original type?");

  if (auto *nominalDecl = dyn_cast<NominalTypeDecl>(decl))
    return nominalDecl->getDeclaredType();
  return decl->getDeclaredInterfaceType();
}

bool ASTContext::supportsMoveOnlyTypes() const {
  // currently the only thing holding back whether the types can appear is this.
  return SILOpts.LexicalLifetimes != LexicalLifetimesOption::Off;
}
