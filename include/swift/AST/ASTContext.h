//===--- ASTContext.h - AST Context Object ----------------------*- C++ -*-===//
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
// This file defines the ASTContext interface.
//
//===----------------------------------------------------------------------===//

#ifndef SWIFT_AST_ASTCONTEXT_H
#define SWIFT_AST_ASTCONTEXT_H

#include "swift/AST/ASTAllocated.h"
#include "swift/AST/Evaluator.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Identifier.h"
#include "swift/AST/Import.h"
#include "swift/AST/SILOptions.h"
#include "swift/AST/SearchPathOptions.h"
#include "swift/AST/Type.h"
#include "swift/AST/TypeAlignments.h"
#include "swift/AST/Types.h"
#include "swift/Basic/LangOptions.h"
#include "swift/Basic/Located.h"
#include "swift/Basic/Malloc.h"
#include "swift/Basic/BlockList.h"
#include "swift/SymbolGraphGen/SymbolGraphOptions.h"
#include "clang/AST/DeclTemplate.h"
#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/MapVector.h"
#include "llvm/ADT/PointerIntPair.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/ADT/TinyPtrVector.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/DataTypes.h"
#include "llvm/Support/VirtualOutputBackend.h"
#include <functional>
#include <memory>
#include <utility>
#include <vector>

namespace clang {
  class Decl;
  class MacroInfo;
  class Module;
  class ObjCInterfaceDecl;
}

namespace llvm {
  class LLVMContext;
}

namespace swift {
  class AbstractFunctionDecl;
  class ASTContext;
  enum class Associativity : unsigned char;
  class AvailabilityContext;
  class BoundGenericType;
  class BuiltinTupleDecl;
  class ClangModuleLoader;
  class ClangNode;
  class ClangTypeConverter;
  class ConcreteDeclRef;
  class ConstructorDecl;
  class Decl;
  class DeclContext;
  class DefaultArgumentInitializer;
  class DerivativeAttr;
  class DifferentiableAttr;
  class ExtensionDecl;
  struct ExternalSourceLocs;
  class LoadedExecutablePlugin;
  class LoadedLibraryPlugin;
  class ForeignRepresentationInfo;
  class FuncDecl;
  class GenericContext;
  class InFlightDiagnostic;
  class IterableDeclContext;
  class LazyContextData;
  class LazyIterableDeclContextData;
  class LazyMemberLoader;
  struct MacroDiscriminatorContext;
  class ModuleDependencyInfo;
  class PatternBindingDecl;
  class PatternBindingInitializer;
  class PluginLoader;
  class SourceFile;
  class SourceLoc;
  class Type;
  class TypeVariableType;
  class TupleType;
  class FunctionType;
  class ArchetypeType;
  class Identifier;
  class InheritedNameSet;
  class ModuleDecl;
  class PackageUnit;
  class ModuleDependenciesCache;
  class ModuleLoader;
  class NominalTypeDecl;
  class NormalProtocolConformance;
  class OpaqueTypeDecl;
  class InheritedProtocolConformance;
  class RootProtocolConformance;
  class SelfProtocolConformance;
  class SpecializedProtocolConformance;
  enum class BuiltinConformanceKind;
  class BuiltinProtocolConformance;
  enum class ProtocolConformanceState;
  class Pattern;
  enum PointerTypeKind : unsigned;
  class PrecedenceGroupDecl;
  class TupleTypeElt;
  class EnumElementDecl;
  class ProtocolDecl;
  class SubstitutableType;
  class SourceManager;
  class ValueDecl;
  class DiagnosticEngine;
  struct RawComment;
  class DocComment;
  class SILBoxType;
  class SILTransform;
  class TypeAliasDecl;
  class VarDecl;
  class UnifiedStatsReporter;
  class IndexSubset;
  struct SILAutoDiffDerivativeFunctionKey;
  struct InterfaceSubContextDelegate;

  enum class KnownProtocolKind : uint8_t;

namespace namelookup {
  class ImportCache;
}

namespace rewriting {
  class RewriteContext;
}

namespace ide {
  class TypeCheckCompletionCallback;
}

/// Lists the set of "known" Foundation entities that are used in the
/// compiler.
///
/// While the names of Foundation types aren't likely to change in
/// Objective-C, their mapping into Swift can. Therefore, when
/// referring to names of Foundation entities in Swift, use this enum
/// and \c swift::getSwiftName or \c ASTContext::getSwiftId.
enum class KnownFoundationEntity {
#define FOUNDATION_ENTITY(Name) Name,
#include "swift/AST/KnownFoundationEntities.def"
};

/// Retrieve the Foundation entity kind for the given Objective-C
/// entity name.
Optional<KnownFoundationEntity> getKnownFoundationEntity(StringRef name);

/// Retrieve the Swift name for the given Foundation entity, where
/// "NS" prefix stripping will apply under omit-needless-words.
StringRef getSwiftName(KnownFoundationEntity kind);

/// Introduces a new constraint checker arena, whose lifetime is
/// tied to the lifetime of this RAII object.
class ConstraintCheckerArenaRAII {
  ASTContext &Self;
  void *Data;

public:
  /// Introduces a new constraint checker arena, supplanting any
  /// existing constraint checker arena.
  ///
  /// \param self The ASTContext into which this constraint checker arena
  /// will be installed.
  ///
  /// \param allocator The allocator used for allocating any data that
  /// goes into the constraint checker arena.
  ConstraintCheckerArenaRAII(ASTContext &self,
                             llvm::BumpPtrAllocator &allocator);

  ConstraintCheckerArenaRAII(const ConstraintCheckerArenaRAII &) = delete;
  ConstraintCheckerArenaRAII(ConstraintCheckerArenaRAII &&) = delete;

  ConstraintCheckerArenaRAII &
  operator=(const ConstraintCheckerArenaRAII &) = delete;

  ConstraintCheckerArenaRAII &
  operator=(ConstraintCheckerArenaRAII &&) = delete;

  ~ConstraintCheckerArenaRAII();
};

class SILLayout; // From SIL

/// A set of missing witnesses for a given conformance. These are temporarily
/// stashed in the ASTContext so the type checker can get at them.
///
/// The only subclass is owned by the type checker, so it can hide its own
/// data structures.
class MissingWitnessesBase {
public:
  virtual ~MissingWitnessesBase();
};

/// ASTContext - This object creates and owns the AST objects.
/// However, this class does more than just maintain context within an AST.
/// It is the closest thing to thread-local or compile-local storage in this
/// code base. Why? SourceKit uses this code with multiple threads per Unix
/// process. Each thread processes a different source file. Each thread has its
/// own instance of ASTContext, and that instance persists for the duration of
/// the thread, throughout all phases of the compilation. (The name "ASTContext"
/// is a bit of a misnomer here.) Why not use thread-local storage? This code
/// may use DispatchQueues and pthread-style TLS won't work with code that uses
/// DispatchQueues. Summary: if you think you need a global or static variable,
/// you probably need to put it here instead.

class ASTContext final {
  ASTContext(const ASTContext&) = delete;
  void operator=(const ASTContext&) = delete;

  ASTContext(
      LangOptions &langOpts, TypeCheckerOptions &typecheckOpts,
      SILOptions &silOpts, SearchPathOptions &SearchPathOpts,
      ClangImporterOptions &ClangImporterOpts,
      symbolgraphgen::SymbolGraphOptions &SymbolGraphOpts,
      SourceManager &SourceMgr, DiagnosticEngine &Diags,
      llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutBackend = nullptr,
      std::function<bool(llvm::StringRef, bool)> PreModuleImportCallback = {});

public:
  // Members that should only be used by ASTContext.cpp.
  struct Implementation;
  Implementation &getImpl() const;

  friend ConstraintCheckerArenaRAII;

  void operator delete(void *Data) throw();

  static ASTContext *
  get(LangOptions &langOpts, TypeCheckerOptions &typecheckOpts,
      SILOptions &silOpts, SearchPathOptions &SearchPathOpts,
      ClangImporterOptions &ClangImporterOpts,
      symbolgraphgen::SymbolGraphOptions &SymbolGraphOpts,
      SourceManager &SourceMgr, DiagnosticEngine &Diags,
      llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutBackend = nullptr,
      std::function<bool(llvm::StringRef, bool)> PreModuleImportCallback = {});
  ~ASTContext();

  /// Optional table of counters to report, nullptr when not collecting.
  ///
  /// This must be initialized early so that Allocate() doesn't try to access
  /// it before being set to null.
  UnifiedStatsReporter *Stats = nullptr;

  /// The language options used for translation.
  const LangOptions &LangOpts;

  /// The type checker options.
  const TypeCheckerOptions &TypeCheckerOpts;

  /// Options for SIL.
  const SILOptions &SILOpts;

  /// The search path options used by this AST context.
  SearchPathOptions &SearchPathOpts;

  /// The clang importer options used by this AST context.
  ClangImporterOptions &ClangImporterOpts;

  /// The symbol graph generation options used by this AST context.
  symbolgraphgen::SymbolGraphOptions &SymbolGraphOpts;

  /// The source manager object.
  SourceManager &SourceMgr;

  /// Diags - The diagnostics engine.
  DiagnosticEngine &Diags;

  /// OutputBackend for writing outputs.
  llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutputBackend;

  /// If the shared pointer is not a \c nullptr and the pointee is \c true,
  /// all operations working on this ASTContext should be aborted at the next
  /// possible opportunity.
  /// This is used by SourceKit to cancel requests for which the result is no
  /// longer of interest.
  /// The returned result will be discarded, so the operation that acknowledges
  /// the cancellation might return with any result.
  std::shared_ptr<std::atomic<bool>> CancellationFlag = nullptr;

  ide::TypeCheckCompletionCallback *CompletionCallback = nullptr;

  /// A callback that will be called when the constraint system found a
  /// solution. Called multiple times if the constraint system has ambiguous
  /// solutions.
  ide::TypeCheckCompletionCallback *SolutionCallback = nullptr;

  /// The request-evaluator that is used to process various requests.
  Evaluator evaluator;

  /// The builtin module.
  ModuleDecl * const TheBuiltinModule;

  /// The standard library module.
  mutable ModuleDecl *TheStdlibModule = nullptr;

  /// The name of the standard library module "Swift".
  Identifier StdlibModuleName;

  /// The name of the SwiftShims module "SwiftShims".
  Identifier SwiftShimsModuleName;

  /// Should we globally ignore swiftmodule files adjacent to swiftinterface
  /// files?
  bool IgnoreAdjacentModules = false;

  // Define the set of known identifiers.
#define IDENTIFIER_WITH_NAME(Name, IdStr) Identifier Id_##Name;
#include "swift/AST/KnownIdentifiers.def"

  /// Cache for names of canonical GenericTypeParamTypes.
  mutable llvm::DenseMap<unsigned, Identifier>
    CanonicalGenericTypeParamTypeNames;

  /// Cache of remapped types (useful for diagnostics).
  llvm::StringMap<Type> RemappedTypes;

  /// The # of times we have performed typo correction.
  unsigned NumTypoCorrections = 0;

  /// Cached mapping from types to their associated tangent spaces.
  llvm::DenseMap<Type, Optional<TangentSpace>> AutoDiffTangentSpaces;

  /// A cache of derivative function types per configuration.
  llvm::DenseMap<SILAutoDiffDerivativeFunctionKey, CanSILFunctionType>
      SILAutoDiffDerivativeFunctions;

  /// Cache of `@differentiable` attributes keyed by parameter indices. Used to
  /// diagnose duplicate `@differentiable` attributes for the same key.
  llvm::DenseMap<std::pair<Decl *, IndexSubset *>, DifferentiableAttr *>
      DifferentiableAttrs;

  /// Cache of `@derivative` attributes keyed by parameter indices and
  /// derivative function kind. Used to diagnose duplicate `@derivative`
  /// attributes for the same key.
  // TODO(TF-1042): remove `DerivativeAttrs` from `ASTContext`. Serialize
  // derivative function configurations per original `AbstractFunctionDecl`.
  llvm::DenseMap<
      std::tuple<Decl *, IndexSubset *, AutoDiffDerivativeFunctionKind>,
      llvm::SmallPtrSet<DerivativeAttr *, 1>>
      DerivativeAttrs;

  /// The Swift module currently being compiled.
  ModuleDecl *MainModule = nullptr;

  /// The block list where we can find special actions based on module name;
  BlockListStore blockListConfig;
private:
  /// The current generation number, which reflects the number of
  /// times that external modules have been loaded.
  ///
  /// Various places in the AST, such as the set of extensions associated with
  /// a nominal type, keep track of the generation number they saw and will
  /// automatically update when they are out of date.
  unsigned CurrentGeneration = 0;

  friend class Pattern;

  /// Mapping from patterns that store interface types that will be lazily
  /// resolved to contextual types, to the declaration context in which the
  /// pattern resides.
  llvm::DenseMap<const Pattern *, DeclContext *>
    DelayedPatternContexts;

  /// Cache of module names that fail the 'canImport' test in this context.
  mutable llvm::StringSet<> FailedModuleImportNames;

  /// Set if a `-module-alias` was passed. Used to store mapping between module aliases and
  /// their corresponding real names, and vice versa for a reverse lookup, which is needed to check
  /// if the module names appearing in source files are aliases or real names.
  /// \see ASTContext::getRealModuleName.
  ///
  /// The boolean in the value indicates whether or not the entry is keyed by an alias vs real name,
  /// i.e. true if the entry is [key: alias_name, value: (real_name, true)].
  mutable llvm::DenseMap<Identifier, std::pair<Identifier, bool>> ModuleAliasMap;

  /// Retrieve the allocator for the given arena.
  llvm::BumpPtrAllocator &
  getAllocator(AllocationArena arena = AllocationArena::Permanent) const;

  /// An optional generic callback function invoked prior to importing a module.
  mutable std::function<bool(llvm::StringRef ModuleName, bool IsOverlay)>
      PreModuleImportCallback;

public:
  /// Allocate - Allocate memory from the ASTContext bump pointer.
  void *Allocate(unsigned long bytes, unsigned alignment,
                 AllocationArena arena = AllocationArena::Permanent) const {
    if (bytes == 0)
      return nullptr;

    if (LangOpts.UseMalloc)
      return AlignedAlloc(bytes, alignment);

    if (arena == AllocationArena::Permanent && Stats)
      Stats->getFrontendCounters().NumASTBytesAllocated += bytes;
    return getAllocator(arena).Allocate(bytes, alignment);
  }

  template <typename T>
  T *Allocate(AllocationArena arena = AllocationArena::Permanent) const {
    T *res = (T *) Allocate(sizeof(T), alignof(T), arena);
    new (res) T();
    return res;
  }

  template <typename T>
  MutableArrayRef<T> AllocateUninitialized(unsigned NumElts,
              AllocationArena Arena = AllocationArena::Permanent) const {
    T *Data = (T *) Allocate(sizeof(T) * NumElts, alignof(T), Arena);
    return { Data, NumElts };
  }

  template <typename T>
  MutableArrayRef<T> Allocate(unsigned numElts,
              AllocationArena arena = AllocationArena::Permanent) const {
    T *res = (T *) Allocate(sizeof(T) * numElts, alignof(T), arena);
    for (unsigned i = 0; i != numElts; ++i)
      new (res+i) T();
    return {res, numElts};
  }

  /// Allocate a copy of the specified object.
  template <typename T>
  typename std::remove_reference<T>::type *AllocateObjectCopy(T &&t,
              AllocationArena arena = AllocationArena::Permanent) const {
    // This function cannot be named AllocateCopy because it would always win
    // overload resolution over the AllocateCopy(ArrayRef<T>).
    using TNoRef = typename std::remove_reference<T>::type;
    TNoRef *res = (TNoRef *) Allocate(sizeof(TNoRef), alignof(TNoRef), arena);
    new (res) TNoRef(std::forward<T>(t));
    return res;
  }

  template <typename T, typename It>
  T *AllocateCopy(It start, It end,
                  AllocationArena arena = AllocationArena::Permanent) const {
    T *res = (T*)Allocate(sizeof(T)*(end-start), alignof(T), arena);
    for (unsigned i = 0; start != end; ++start, ++i)
      new (res+i) T(*start);
    return res;
  }

  template<typename T, size_t N>
  MutableArrayRef<T> AllocateCopy(T (&array)[N],
      AllocationArena arena = AllocationArena::Permanent) const {
    return MutableArrayRef<T>(AllocateCopy<T>(array, array+N, arena), N);
  }

  template<typename T>
  MutableArrayRef<T> AllocateCopy(ArrayRef<T> array,
      AllocationArena arena = AllocationArena::Permanent) const {
    return MutableArrayRef<T>(AllocateCopy<T>(array.begin(),array.end(), arena),
                              array.size());
  }

  template <typename T>
  MutableArrayRef<T>
  AllocateCopy(const std::vector<T> &vec,
               AllocationArena arena = AllocationArena::Permanent) const {
    return AllocateCopy(ArrayRef<T>(vec), arena);
  }

  template<typename T>
  ArrayRef<T> AllocateCopy(const SmallVectorImpl<T> &vec,
      AllocationArena arena = AllocationArena::Permanent) const {
    return AllocateCopy(ArrayRef<T>(vec), arena);
  }

  template<typename T>
  MutableArrayRef<T>
  AllocateCopy(SmallVectorImpl<T> &vec,
               AllocationArena arena = AllocationArena::Permanent) const {
    return AllocateCopy(MutableArrayRef<T>(vec), arena);
  }

  StringRef AllocateCopy(StringRef Str,
                    AllocationArena arena = AllocationArena::Permanent) const {
    ArrayRef<char> Result =
        AllocateCopy(llvm::makeArrayRef(Str.data(), Str.size()), arena);
    return StringRef(Result.data(), Result.size());
  }

  template<typename T, typename Vector, typename Set>
  MutableArrayRef<T>
  AllocateCopy(llvm::SetVector<T, Vector, Set> setVector,
               AllocationArena arena = AllocationArena::Permanent) const {
    return MutableArrayRef<T>(AllocateCopy<T>(setVector.begin(),
                                              setVector.end(),
                                              arena),
                              setVector.size());
  }

  /// Set a new stats reporter.
  void setStatsReporter(UnifiedStatsReporter *stats);

public:
  /// getIdentifier - Return the uniqued and AST-Context-owned version of the
  /// specified string.
  Identifier getIdentifier(StringRef Str) const;

  /// Convert a given alias map to a map of Identifiers between module aliases and their actual names.
  /// For example, if '-module-alias Foo=X -module-alias Bar=Y' input is passed in, the aliases Foo and Bar are
  /// the names of the imported or referenced modules in source files in the main module, and X and Y
  /// are the real (physical) module names on disk.
  void setModuleAliases(const llvm::StringMap<StringRef> &aliasMap);

  /// Look up option used in \c getRealModuleName when module aliasing is applied.
  enum class ModuleAliasLookupOption {
    alwaysRealName,
    realNameFromAlias,
    aliasFromRealName
  };

  /// Look up the module alias map by the given \p key and a lookup \p option.
  ///
  /// \param key A module alias or real name to look up the map by.
  /// \param option A look up option \c ModuleAliasLookupOption. Defaults to alwaysRealName.
  ///
  /// \return The real name or alias mapped to the key.
  ///         If no aliasing is used, return \p key regardless of \p option.
  ///         If \p option is alwaysRealName, return the real module name whether the \p key is an alias
  ///         or a real name.
  ///         If \p option is realNameFromAlias, only return a real name if \p key is an alias.
  ///         If \p option is aliasFromRealName, only return an alias if \p key is a real name.
  ///         Else return a real name or an alias mapped to the \p key.
  Identifier getRealModuleName(Identifier key,
                               ModuleAliasLookupOption option = ModuleAliasLookupOption::alwaysRealName) const;

  /// Decide how to interpret two precedence groups.
  Associativity associateInfixOperators(PrecedenceGroupDecl *left,
                                        PrecedenceGroupDecl *right) const;

  /// Retrieve the declaration of Swift.Error.
  ProtocolDecl *getErrorDecl() const;
  CanType getErrorExistentialType() const;

#define KNOWN_STDLIB_TYPE_DECL(NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
  /** Retrieve the declaration of Swift.NAME. */ \
  DECL_CLASS *get##NAME##Decl() const; \
\
  /** Retrieve the type of Swift.NAME. */ \
  Type get##NAME##Type() const;
#include "swift/AST/KnownStdlibTypes.def"

  /// Retrieve the declaration of Swift.Optional<T>.Some.
  EnumElementDecl *getOptionalSomeDecl() const;
  
  /// Retrieve the declaration of Swift.Optional<T>.None.
  EnumElementDecl *getOptionalNoneDecl() const;

  /// Retrieve the declaration of Swift.Void.
  TypeAliasDecl *getVoidDecl() const;

  /// Retrieve the type of Swift.Void.
  Type getVoidType() const;

  /// Retrieve the declaration of the "pointee" property of a pointer type.
  VarDecl *getPointerPointeePropertyDecl(PointerTypeKind ptrKind) const;

  /// Retrieve the type Swift.Any as an existential type.
  CanType getAnyExistentialType() const;

  /// Retrieve the type Swift.AnyObject as a constraint.
  CanType getAnyObjectConstraint() const;

  /// Retrieve the type Swift.AnyObject as an existential type.
  CanType getAnyObjectType() const;

#define KNOWN_SDK_TYPE_DECL(MODULE, NAME, DECL_CLASS, NUM_GENERIC_PARAMS) \
  /** Retrieve the declaration of MODULE.NAME. */ \
  DECL_CLASS *get##NAME##Decl() const; \
\
  /** Retrieve the type of MODULE.NAME. */ \
  Type get##NAME##Type() const;
#include "swift/AST/KnownSDKTypes.def"

  // Declare accessors for the known declarations.
#define FUNC_DECL(Name, Id) \
  FuncDecl *get##Name() const;
#include "swift/AST/KnownDecls.def"

  // Declare accessors for the known declarations.
#define KNOWN_SDK_FUNC_DECL(Module, Name, Id) \
  FuncDecl *get##Name() const;
#include "swift/AST/KnownSDKDecls.def"

  /// Get the '+' function on two RangeReplaceableCollection.
  FuncDecl *getPlusFunctionOnRangeReplaceableCollection() const;

  /// Get the '+' function on two String.
  FuncDecl *getPlusFunctionOnString() const;

  /// Get Sequence.makeIterator().
  FuncDecl *getSequenceMakeIterator() const;

  /// Get AsyncSequence.makeAsyncIterator().
  FuncDecl *getAsyncSequenceMakeAsyncIterator() const;

  /// Get IteratorProtocol.next().
  FuncDecl *getIteratorNext() const;

  /// Get AsyncIteratorProtocol.next().
  FuncDecl *getAsyncIteratorNext() const;

  /// Check whether the standard library provides all the correct
  /// intrinsic support for Optional<T>.
  ///
  /// If this is true, the four methods above all promise to return
  /// non-null.
  bool hasOptionalIntrinsics() const;

  /// Check whether the standard library provides all the correct
  /// intrinsic support for UnsafeMutablePointer<T> function arguments.
  ///
  /// If this is true, the methods getConvert*ToPointerArgument
  /// all promise to return non-null.
  bool hasPointerArgumentIntrinsics() const;

  /// Check whether the standard library provides all the correct
  /// intrinsic support for array literals.
  ///
  /// If this is true, the method getAllocateUninitializedArray
  /// promises to return non-null.
  bool hasArrayLiteralIntrinsics() const;

  /// Retrieve the declaration of Swift.Bool.init(_builtinBooleanLiteral:)
  ConcreteDeclRef getBoolBuiltinInitDecl() const;

  /// Retrieve the witness for init(_builtinIntegerLiteral:).
  ConcreteDeclRef getIntBuiltinInitDecl(NominalTypeDecl *intDecl) const;

  /// Retrieve the witness for init(_builtinFloatLiteral:).
  ConcreteDeclRef getFloatBuiltinInitDecl(NominalTypeDecl *floatDecl) const;

  /// Retrieve the witness for (_builtinStringLiteral:utf8CodeUnitCount:isASCII:).
  ConcreteDeclRef getStringBuiltinInitDecl(NominalTypeDecl *stringDecl) const;

  ConcreteDeclRef getBuiltinInitDecl(NominalTypeDecl *decl,
                                     KnownProtocolKind builtinProtocol,
                llvm::function_ref<DeclName (ASTContext &ctx)> initName) const;

  /// Retrieve _StringProcessing.Regex.init(_regexString: String, version: Int).
  ConcreteDeclRef getRegexInitDecl(Type regexType) const;

  /// Retrieve the declaration of Swift.<(Int, Int) -> Bool.
  FuncDecl *getLessThanIntDecl() const;

  /// Retrieve the declaration of Swift.==(Int, Int) -> Bool.
  FuncDecl *getEqualIntDecl() const;

  /// Retrieve the declaration of Swift._hashValue<H>(for: H) -> Int.
  FuncDecl *getHashValueForDecl() const;

  /// Retrieve the declaration of Array.append(element:)
  FuncDecl *getArrayAppendElementDecl() const;

  /// Retrieve the declaration of
  /// Array.reserveCapacityForAppend(newElementsCount: Int)
  FuncDecl *getArrayReserveCapacityDecl() const;

  /// Retrieve the declaration of String.init(_builtinStringLiteral ...)
  ConstructorDecl *getMakeUTF8StringDecl() const;

  // Retrieve the declaration of Swift._stdlib_isOSVersionAtLeast.
  FuncDecl *getIsOSVersionAtLeastDecl() const;

  /// Retrieve the declaration of DistributedActorSystem.remoteCall(Void)(...).
  ///
  /// \param actorOrSystem distributed actor or actor system to get the
  /// remoteCall function for. Since the method we're looking for is an ad-hoc
  /// requirement, a specific type MUST be passed here as it is not possible
  /// to obtain the decl from just the `DistributedActorSystem` protocol type.
  /// \param isVoidReturn true if the call will be returning `Void`.
  AbstractFunctionDecl *getRemoteCallOnDistributedActorSystem(
      NominalTypeDecl *actorOrSystem,
      bool isVoidReturn) const;

  /// Retrieve the declaration of DistributedActorSystem.make().
  ///
  /// \param thunk the function from which we'll be invoking things on the obtained
  /// actor system; This way we'll always get the right type, taking care of any
  /// where clauses etc.
  FuncDecl *getMakeInvocationEncoderOnDistributedActorSystem(
      AbstractFunctionDecl *thunk) const;

  /// Indicates whether move-only / noncopyable types are supported.
  bool supportsMoveOnlyTypes() const;

  // Retrieve the declaration of
  // DistributedInvocationEncoder.recordGenericSubstitution(_:).
  //
  // \param nominal optionally provide a 'NominalTypeDecl' from which the
  // function decl shall be extracted. This is useful to avoid witness calls
  // through the protocol which is looked up when nominal is null.
  FuncDecl *getRecordGenericSubstitutionOnDistributedInvocationEncoder(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of DistributedTargetInvocationEncoder.recordArgument(_:).
  //
  // \param nominal optionally provide a 'NominalTypeDecl' from which the
  // function decl shall be extracted. This is useful to avoid witness calls
  // through the protocol which is looked up when nominal is null.
  AbstractFunctionDecl *getRecordArgumentOnDistributedInvocationEncoder(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of DistributedTargetInvocationEncoder.recordReturnType(_:).
  AbstractFunctionDecl *getRecordReturnTypeOnDistributedInvocationEncoder(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of DistributedTargetInvocationEncoder.recordErrorType(_:).
  AbstractFunctionDecl *getRecordErrorTypeOnDistributedInvocationEncoder(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of
  // DistributedTargetInvocationDecoder.getDecodeNextArgumentOnDistributedInvocationDecoder(_:).
  AbstractFunctionDecl *getDecodeNextArgumentOnDistributedInvocationDecoder(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of
  // getOnReturnOnDistributedTargetInvocationResultHandler.onReturn(_:).
  AbstractFunctionDecl *getOnReturnOnDistributedTargetInvocationResultHandler(
      NominalTypeDecl *nominal) const;

  // Retrieve the declaration of DistributedInvocationEncoder.doneRecording().
  //
  // \param nominal optionally provide a 'NominalTypeDecl' from which the
  // function decl shall be extracted. This is useful to avoid witness calls
  // through the protocol which is looked up when nominal is null.
  FuncDecl *getDoneRecordingOnDistributedInvocationEncoder(
      NominalTypeDecl *nominal) const;

  /// Look for the declaration with the given name within the
  /// passed in module.
  void lookupInModule(ModuleDecl *M, StringRef name,
                      SmallVectorImpl<ValueDecl *> &results) const;

  /// Look for the declaration with the given name within the
  /// Swift module.
  void lookupInSwiftModule(StringRef name,
                           SmallVectorImpl<ValueDecl *> &results) const;

  /// Retrieve a specific, known protocol.
  ProtocolDecl *getProtocol(KnownProtocolKind kind) const;
  
  /// Determine whether the given nominal type is one of the standard
  /// library or Cocoa framework types that is known to be bridged by another
  /// module's overlay, for layering or implementation detail reasons.
  bool isTypeBridgedInExternalModule(NominalTypeDecl *nominal) const;

  /// True if the given type is an Objective-C class that serves as the bridged
  /// object type for many Swift value types, meaning that the conversion from
  /// an object to a value is a conditional cast.
  bool isObjCClassWithMultipleSwiftBridgedTypes(Type t);

  /// Get the Objective-C type that a Swift type bridges to, if any.
  /// 
  /// \param dc The context in which bridging is occurring.
  /// \param type The Swift for which we are querying bridging behavior.
  /// \param bridgedValueType The specific value type that is bridged,
  /// which will usually by the same as \c type.
  Type getBridgedToObjC(const DeclContext *dc, Type type,
                        Type *bridgedValueType = nullptr) const;

private:
  ClangTypeConverter &getClangTypeConverter();

public:
  /// Get the Clang type corresponding to a Swift function type.
  ///
  /// \param params The function parameters.
  /// \param resultTy The Swift result type.
  /// \param trueRep The actual calling convention, which must be C-compatible.
  const clang::Type *
  getClangFunctionType(ArrayRef<AnyFunctionType::Param> params, Type resultTy,
                       FunctionTypeRepresentation trueRep);

  /// Get the canonical Clang type corresponding to a SIL function type.
  ///
  /// SIL analog of \c ASTContext::getClangFunctionType .
  const clang::Type *
  getCanonicalClangFunctionType(
    ArrayRef<SILParameterInfo> params, Optional<SILResultInfo> result,
    SILFunctionType::Representation trueRep);

  /// Instantiates "Impl.Converter" if needed, then translate Swift generic
  /// substitutions to equivalent C++ types using \p templateParams and \p
  /// genericArgs. The converted Clang types are placed into \p templateArgs.
  ///
  /// \p templateArgs must be empty. \p templateParams and \p genericArgs must
  /// be equal in size.
  ///
  /// \returns nullptr if successful. If an error occurs, returns a list of
  /// types that couldn't be converted.
  std::unique_ptr<TemplateInstantiationError> getClangTemplateArguments(
      const clang::TemplateParameterList *templateParams,
      ArrayRef<Type> genericArgs,
      SmallVectorImpl<clang::TemplateArgument> &templateArgs);

  /// Get the Swift declaration that a Clang declaration was exported from,
  /// if applicable.
  const Decl *getSwiftDeclForExportedClangDecl(const clang::Decl *decl);

  /// General conversion method from Swift types -> Clang types.
  ///
  /// HACK: This method is only intended to be called from a specific place in
  /// IRGen. For converting function types, strongly prefer using one of the
  /// other methods instead, instead of manually iterating over parameters
  /// and results.
  const clang::Type *getClangTypeForIRGen(Type ty);

  /// Determine whether the given Swift type is representable in a
  /// given foreign language.
  ForeignRepresentationInfo
  getForeignRepresentationInfo(NominalTypeDecl *nominal,
                               ForeignLanguage language,
                               const DeclContext *dc);

  /// Add a cleanup function to be called when the ASTContext is deallocated.
  void addCleanup(std::function<void(void)> cleanup);

  /// Add a cleanup to run the given object's destructor when the ASTContext is
  /// deallocated.
  template<typename T>
  void addDestructorCleanup(T &object) {
    addCleanup([&object]{ object.~T(); });
  }

  /// Get the runtime availability of the class metadata update callback
  /// mechanism for the target platform.
  AvailabilityContext getObjCMetadataUpdateCallbackAvailability();

  /// Get the runtime availability of the objc_getClass() hook for the target
  /// platform.
  AvailabilityContext getObjCGetClassHookAvailability();
  
  /// Get the runtime availability of features introduced in the Swift 5.0
  /// compiler for the target platform.
  AvailabilityContext getSwift50Availability();

  /// Get the runtime availability of the opaque types language feature for the
  /// target platform.
  AvailabilityContext getOpaqueTypeAvailability();

  /// Get the runtime availability of the objc_loadClassref() entry point for
  /// the target platform.
  AvailabilityContext getObjCClassStubsAvailability();

  /// Get the runtime availability of features introduced in the Swift 5.1
  /// compiler for the target platform.
  AvailabilityContext getSwift51Availability();

  /// Get the runtime availability of
  /// swift_getTypeByMangledNameInContextInMetadataState.
  AvailabilityContext getTypesInAbstractMetadataStateAvailability();

  /// Get the runtime availability of support for prespecialized generic 
  /// metadata.
  AvailabilityContext getPrespecializedGenericMetadataAvailability();

  /// Get the runtime availability of the swift_compareTypeContextDescriptors
  /// for the target platform.
  AvailabilityContext getCompareTypeContextDescriptorsAvailability();

  /// Get the runtime availability of the
  /// swift_compareProtocolConformanceDescriptors entry point for the target
  /// platform.
  AvailabilityContext getCompareProtocolConformanceDescriptorsAvailability();

  /// Get the runtime availability of support for inter-module prespecialized
  /// generic metadata.
  AvailabilityContext getIntermodulePrespecializedGenericMetadataAvailability();

  /// Get the runtime availability of support for concurrency.
  AvailabilityContext getConcurrencyAvailability();

  /// Get the back-deployed availability for concurrency.
  AvailabilityContext getBackDeployedConcurrencyAvailability();

  /// The the availability since when distributed actors are able to have custom
  /// executors.
  AvailabilityContext
  getConcurrencyDistributedActorWithCustomExecutorAvailability();

  /// Get the runtime availability of support for differentiation.
  AvailabilityContext getDifferentiationAvailability();

  /// Get the runtime availability of getters and setters of multi payload enum
  /// tag single payloads.
  AvailabilityContext getMultiPayloadEnumTagSinglePayload();

  /// Get the runtime availability of the Objective-C enabled
  /// swift_isUniquelyReferenced functions.
  AvailabilityContext getObjCIsUniquelyReferencedAvailability();

  /// Get the runtime availability of metadata manipulation runtime functions
  /// for extended existential types.
  AvailabilityContext getParameterizedExistentialRuntimeAvailability();

  /// Get the runtime availability of immortal ref-count symbols, which are
  /// needed to place array buffers into constant data sections.
  AvailabilityContext getImmortalRefCountSymbolsAvailability();

  /// Get the runtime availability of runtime functions for
  /// variadic generic types.
  AvailabilityContext getVariadicGenericTypeAvailability();

  /// Get the runtime availability of features introduced in the Swift 5.2
  /// compiler for the target platform.
  AvailabilityContext getSwift52Availability();

  /// Get the runtime availability of features introduced in the Swift 5.3
  /// compiler for the target platform.
  AvailabilityContext getSwift53Availability();

  /// Get the runtime availability of features introduced in the Swift 5.4
  /// compiler for the target platform.
  AvailabilityContext getSwift54Availability();

  /// Get the runtime availability of features introduced in the Swift 5.5
  /// compiler for the target platform.
  AvailabilityContext getSwift55Availability();

  /// Get the runtime availability of features introduced in the Swift 5.6
  /// compiler for the target platform.
  AvailabilityContext getSwift56Availability();

  /// Get the runtime availability of features introduced in the Swift 5.7
  /// compiler for the target platform.
  AvailabilityContext getSwift57Availability();

  /// Get the runtime availability of features introduced in the Swift 5.8
  /// compiler for the target platform.
  AvailabilityContext getSwift58Availability();

  /// Get the runtime availability of features introduced in the Swift 5.9
  /// compiler for the target platform.
  AvailabilityContext getSwift59Availability();

  // Note: Update this function if you add a new getSwiftXYAvailability above.
  /// Get the runtime availability for a particular version of Swift (5.0+).
  AvailabilityContext
  getSwift5PlusAvailability(llvm::VersionTuple swiftVersion);

  /// Get the runtime availability of features that have been introduced in the
  /// Swift compiler for future versions of the target platform.
  AvailabilityContext getSwiftFutureAvailability();

  /// Returns `true` if versioned availability annotations are supported for the
  /// target triple.
  bool supportsVersionedAvailability() const;

  //===--------------------------------------------------------------------===//
  // Diagnostics Helper functions
  //===--------------------------------------------------------------------===//

  bool hadError() const;
  
  //===--------------------------------------------------------------------===//
  // Type manipulation routines.
  //===--------------------------------------------------------------------===//

  // Builtin type and simple types that are used frequently.
  const CanType TheErrorType;             /// This is the ErrorType singleton.
  const CanType TheUnresolvedType;        /// This is the UnresolvedType singleton.
  const CanType TheEmptyTupleType;        /// This is '()', aka Void
  const CanType TheEmptyPackType;
  const CanType TheAnyType;               /// This is 'Any', the empty protocol composition
#define SINGLETON_TYPE(SHORT_ID, ID) \
  const CanType The##SHORT_ID##Type;
#include "swift/AST/TypeNodes.def"

  const CanType TheIEEE32Type;            /// 32-bit IEEE floating point
  const CanType TheIEEE64Type;            /// 64-bit IEEE floating point
  
  // Target specific types.
  const CanType TheIEEE16Type;            /// 16-bit IEEE floating point
  const CanType TheIEEE80Type;            /// 80-bit IEEE floating point
  const CanType TheIEEE128Type;           /// 128-bit IEEE floating point
  const CanType ThePPC128Type;            /// 128-bit PowerPC 2xDouble

  /// Adds a search path to SearchPathOpts, unless it is already present.
  ///
  /// Does any proper bookkeeping to keep all module loaders up to date as well.
  void addSearchPath(StringRef searchPath, bool isFramework, bool isSystem);

  /// Adds a module loader to this AST context.
  ///
  /// \param loader The new module loader, which will be added after any
  ///               existing module loaders.
  /// \param isClang \c true if this module loader is responsible for loading
  ///                Clang modules, which are special-cased in some parts of the
  ///                compiler.
  /// \param isDWARF \c true if this module loader can load Clang modules
  ///                from DWARF.
  /// \param IsInterface \c true if this module loader can load Swift textual
  ///                interface.
  void addModuleLoader(std::unique_ptr<ModuleLoader> loader,
                       bool isClang = false, bool isDWARF = false,
                       bool IsInterface = false);

  /// Add a module interface checker to use for this AST context.
  void addModuleInterfaceChecker(std::unique_ptr<ModuleInterfaceChecker> checker);

  /// Retrieve the module interface checker associated with this AST context.
  ModuleInterfaceChecker *getModuleInterfaceChecker() const;

  /// Retrieve the module dependencies for the module with the given name.
  ///
  Optional<const ModuleDependencyInfo*> getModuleDependencies(
      StringRef moduleName,
      ModuleDependenciesCache &cache,
      InterfaceSubContextDelegate &delegate,
      bool optionalDependencyLookup = false,
      bool isTestableImport = false,
      llvm::Optional<std::pair<std::string, swift::ModuleDependencyKind>> dependencyOf = None);

  /// Retrieve the module dependencies for the Clang module with the given name.
  Optional<const ModuleDependencyInfo*> getClangModuleDependencies(
      StringRef moduleName,
      ModuleDependenciesCache &cache,
      InterfaceSubContextDelegate &delegate);

  /// Retrieve the module dependencies for the Swift module with the given name.
  Optional<const ModuleDependencyInfo*> getSwiftModuleDependencies(
      StringRef moduleName,
      ModuleDependenciesCache &cache,
      InterfaceSubContextDelegate &delegate);

  /// Compute the extra implicit framework search paths on Apple platforms:
  /// $SDKROOT/System/Library/Frameworks/ and $SDKROOT/Library/Frameworks/.
  std::vector<std::string> getDarwinImplicitFrameworkSearchPaths() const;

  /// Return a set of all possible filesystem locations where modules can be found.
  llvm::StringSet<> getAllModuleSearchPathsSet() const;

  /// Load extensions to the given nominal type from the external
  /// module loaders.
  ///
  /// \param nominal The nominal type whose extensions should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains extensions loaded from any generation up to and including this
  /// one.
  void loadExtensions(NominalTypeDecl *nominal, unsigned previousGeneration);

  /// Load the methods within the given type that produce
  /// Objective-C class or instance methods with the given selector.
  ///
  /// \param tyDecl The type in which we are searching for @objc methods.
  /// The search only considers this type and its extensions; not any
  /// superclasses.
  ///
  /// \param selector The selector to search for.
  ///
  /// \param isInstanceMethod Whether we are looking for an instance method
  /// (vs. a class method).
  ///
  /// \param previousGeneration The previous generation with which this
  /// callback was invoked. The list of methods will already contain all of
  /// the results from generations up and including \c previousGeneration.
  ///
  /// \param methods The list of @objc methods in this class that have this
  /// selector and are instance/class methods as requested. This list will be
  /// extended with any methods found in subsequent generations.
  ///
  /// \param swiftOnly If true, only loads methods from imported Swift modules,
  /// skipping the Clang importer.
  ///
  /// \note Passing a protocol is supported, but currently a no-op, because
  /// Objective-C protocols cannot be extended in ways that make the ObjC method
  /// lookup table relevant.
  void loadObjCMethods(NominalTypeDecl *tyDecl, ObjCSelector selector,
                       bool isInstanceMethod, unsigned previousGeneration,
                       llvm::TinyPtrVector<AbstractFunctionDecl *> &methods,
                       bool swiftOnly = false);

  /// Load derivative function configurations for the given
  /// AbstractFunctionDecl.
  ///
  /// \param originalAFD The declaration whose derivative function
  /// configurations should be loaded.
  ///
  /// \param previousGeneration The previous generation number. The AST already
  /// contains derivative function configurations loaded from any generation up
  /// to and including this one.
  void loadDerivativeFunctionConfigurations(
      AbstractFunctionDecl *originalAFD, unsigned previousGeneration,
      llvm::SetVector<AutoDiffConfig> &results);

  /// Retrieve the next macro expansion discriminator within the given
  /// name and context.
  unsigned getNextMacroDiscriminator(MacroDiscriminatorContext context,
                                     DeclBaseName baseName);

  /// Get the next discriminator within the given declaration context.
  unsigned getNextDiscriminator(const DeclContext *dc);

  /// Set the maximum assigned discriminator within the given declaration context.
  void setMaxAssignedDiscriminator(
      const DeclContext *dc, unsigned discriminator);

  /// Retrieve the Clang module loader for this ASTContext.
  ///
  /// If there is no Clang module loader, returns a null pointer.
  /// The loader is owned by the AST context.
  ClangModuleLoader *getClangModuleLoader() const;

  /// Retrieve the DWARF module loader for this ASTContext.
  ///
  /// If there is no Clang module loader, returns a null pointer.
  /// The loader is owned by the AST context.
  ClangModuleLoader *getDWARFModuleLoader() const;

  /// Check whether the module with a given name can be imported without
  /// importing it.
  ///
  /// Note that even if this check succeeds, errors may still occur if the
  /// module is loaded in full.
  bool canImportModuleImpl(ImportPath::Module ModulePath,
                           llvm::VersionTuple version, bool underlyingVersion,
                           bool updateFailingList) const;

public:
  namelookup::ImportCache &getImportCache() const;

  /// Returns an iterator over the modules that are known by this context
  /// to be loaded.
  ///
  /// Iteration order is guaranteed to match the order in which
  /// \c addLoadedModule was called to register the loaded module
  /// with this context.
  iterator_range<llvm::MapVector<Identifier, ModuleDecl *>::const_iterator>
  getLoadedModules() const;

  /// Returns the number of loaded modules known by this context to be loaded.
  unsigned getNumLoadedModules() const {
    auto eltRange = getLoadedModules();
    return std::distance(eltRange.begin(), eltRange.end());
  }

  /// Asks every module loader to verify the ASTs it has loaded.
  ///
  /// Does nothing in non-asserts (NDEBUG) builds.
  void verifyAllLoadedModules() const;

  /// Check whether the module with a given name can be imported without
  /// importing it.
  ///
  /// Note that even if this check succeeds, errors may still occur if the
  /// module is loaded in full.
  bool canImportModule(ImportPath::Module ModulePath,
                       llvm::VersionTuple version = llvm::VersionTuple(),
                       bool underlyingVersion = false);
  bool canImportModule(ImportPath::Module ModulePath,
                       llvm::VersionTuple version = llvm::VersionTuple(),
                       bool underlyingVersion = false) const;

  /// \returns a module with a given name that was already loaded.  If the
  /// module was not loaded, returns nullptr.
  ModuleDecl *getLoadedModule(
      ImportPath::Module ModulePath) const;

  ModuleDecl *getLoadedModule(Identifier ModuleName) const;

  /// Attempts to load a module into this ASTContext.
  ///
  /// If a module by this name has already been loaded, the existing module will
  /// be returned.
  ///
  /// \param ModulePath The module's \c ImportPath which describes
  /// the name of the module being loaded, possibly including submodules.
  /// \param AllowMemoryCached Should we allow reuse of an already loaded
  /// module or force reloading from disk, defaults to true.
  ///
  /// \returns The requested module, or NULL if the module cannot be found.
  ModuleDecl *
  getModule(ImportPath::Module ModulePath, bool AllowMemoryCached = true);

  /// Attempts to load the matching overlay module for the given clang
  /// module into this ASTContext.
  ///
  /// \returns The Swift overlay module corresponding to the given Clang module,
  /// or NULL if the overlay module cannot be found.
  ModuleDecl *getOverlayModule(const FileUnit *ClangModule);

  ModuleDecl *getModuleByName(StringRef ModuleName);

  ModuleDecl *getModuleByIdentifier(Identifier ModuleID);

  /// Returns the standard library module, or null if the library isn't present.
  ///
  /// If \p loadIfAbsent is true, the ASTContext will attempt to load the module
  /// if it hasn't been set yet.
  ModuleDecl *getStdlibModule(bool loadIfAbsent = false);

  ModuleDecl *getStdlibModule() const {
    return const_cast<ASTContext *>(this)->getStdlibModule(false);
  }

  /// Insert an externally-sourced module into the set of known loaded modules
  /// in this context.
  void addLoadedModule(ModuleDecl *M);

  /// Change the behavior of all loaders to ignore swiftmodules next to
  /// swiftinterfaces.
  void setIgnoreAdjacentModules(bool value);

  /// Retrieve the current generation number, which reflects the
  /// number of times a module import has caused mass invalidation of
  /// lookup tables.
  ///
  /// Various places in the AST keep track of the generation numbers at which
  /// their own information is valid, such as the list of extensions associated
  /// with a nominal type.
  unsigned getCurrentGeneration() const { return CurrentGeneration; }

  /// Increase the generation number, implying that various lookup
  /// tables have been significantly altered by the introduction of a new
  /// module import.
  ///
  /// \returns the previous generation number.
  unsigned bumpGeneration() { return CurrentGeneration++; }

  /// Produce a "normal" conformance for a nominal type.
  NormalProtocolConformance *
  getConformance(Type conformingType,
                 ProtocolDecl *protocol,
                 SourceLoc loc,
                 DeclContext *dc,
                 ProtocolConformanceState state,
                 bool isUnchecked);

  /// Produce a self-conformance for the given protocol.
  SelfProtocolConformance *
  getSelfConformance(ProtocolDecl *protocol);

  /// Produce the builtin conformance for some structural type to some protocol.
  BuiltinProtocolConformance *
  getBuiltinConformance(Type type, ProtocolDecl *protocol,
                        GenericSignature genericSig,
                        ArrayRef<Requirement> conditionalRequirements,
                        BuiltinConformanceKind kind);

  /// A callback used to produce a diagnostic for an ill-formed protocol
  /// conformance that was type-checked before we're actually walking the
  /// conformance itself, along with a bit indicating whether this diagnostic
  /// produces an error.
  struct DelayedConformanceDiag {
    const ValueDecl *Requirement;
    std::function<void()> Callback;
    bool IsError;
  };

  /// Check whether current context has any errors associated with
  /// ill-formed protocol conformances which haven't been produced yet.
  ///
  /// @param conformance if non-null, will check only for errors specific to the
  /// provided conformance. Otherwise, checks for _any_ errors.
  ///
  /// @returns true iff there are any delayed diagnostic errors
  bool hasDelayedConformanceErrors(
                  NormalProtocolConformance const* conformance = nullptr) const;

  /// Add a delayed diagnostic produced while type-checking a
  /// particular protocol conformance.
  void addDelayedConformanceDiag(NormalProtocolConformance *conformance,
                                 DelayedConformanceDiag fn);

  /// Retrieve the delayed-conformance diagnostic callbacks for the
  /// given normal protocol conformance.
  std::vector<DelayedConformanceDiag>
  takeDelayedConformanceDiags(NormalProtocolConformance const* conformance);

  /// Add delayed missing witnesses for the given normal protocol conformance.
  void addDelayedMissingWitnesses(
      NormalProtocolConformance *conformance,
      std::unique_ptr<MissingWitnessesBase> missingWitnesses);

  /// Retrieve the delayed missing witnesses for the given normal protocol
  /// conformance.
  std::unique_ptr<MissingWitnessesBase>
  takeDelayedMissingWitnesses(NormalProtocolConformance *conformance);

  /// Produce a specialized conformance, which takes a generic
  /// conformance and substitutions written in terms of the generic
  /// conformance's signature.
  ///
  /// \param type The type for which we are retrieving the conformance.
  ///
  /// \param generic The generic conformance.
  ///
  /// \param substitutions The set of substitutions required to produce the
  /// specialized conformance from the generic conformance.
  ProtocolConformance *
  getSpecializedConformance(Type type,
                            RootProtocolConformance *generic,
                            SubstitutionMap substitutions);

  /// Produce an inherited conformance, for subclasses of a type
  /// that already conforms to a protocol.
  ///
  /// \param type The type for which we are retrieving the conformance.
  ///
  /// \param inherited The inherited conformance.
  ProtocolConformance *
  getInheritedConformance(Type type, ProtocolConformance *inherited);

  /// Get the lazy data for the given declaration.
  ///
  /// \param lazyLoader If non-null, the lazy loader to use when creating the
  /// lazy data. The pointer must either be null or be consistent
  /// across all calls for the same \p func.
  LazyContextData *getOrCreateLazyContextData(const DeclContext *decl,
                                              LazyMemberLoader *lazyLoader);

  /// Get the lazy iterable context for the given iterable declaration context.
  ///
  /// \param lazyLoader If non-null, the lazy loader to use when creating the
  /// iterable context data. The pointer must either be null or be consistent
  /// across all calls for the same \p idc.
  LazyIterableDeclContextData *getOrCreateLazyIterableContextData(
                                              const IterableDeclContext *idc,
                                              LazyMemberLoader *lazyLoader);

  /// Access the side cache for property wrapper backing property types,
  /// used because TypeChecker::typeCheckBinding() needs somewhere to stash
  /// the backing property type.
  Type getSideCachedPropertyWrapperBackingPropertyType(VarDecl *var) const;
  void setSideCachedPropertyWrapperBackingPropertyType(VarDecl *var,
                                                        Type type);
  
  /// Returns memory usage of this ASTContext.
  size_t getTotalMemory() const;
  
  /// Returns memory used exclusively by constraint solver.
  size_t getSolverMemory() const;

  /// Retrieve the Swift identifier for the given Foundation entity, where
  /// "NS" prefix stripping will apply under omit-needless-words.
  Identifier getSwiftId(KnownFoundationEntity kind) {
    return getIdentifier(swift::getSwiftName(kind));
  }

  /// Populate \p names with visible top level module names.
  /// This guarantees that resulted \p names doesn't have duplicated names.
  void getVisibleTopLevelModuleNames(SmallVectorImpl<Identifier> &names) const;

  /// Whether to perform typo correction given the pre-configured correction limit.
  /// Increments \c NumTypoCorrections then checks this against the limit in
  /// the language options.
  bool shouldPerformTypoCorrection();

private:
  friend class IntrinsicInfo;
  /// Retrieve an LLVMContext that is used for scratch space for intrinsic lookup.
  llvm::LLVMContext &getIntrinsicScratchContext() const;

public:
  rewriting::RewriteContext &getRewriteContext();

  /// This is a hack to break cycles. Don't introduce new callers of this
  /// method.
  bool isRecursivelyConstructingRequirementMachine(
      CanGenericSignature sig);

  /// This is a hack to break cycles. Don't introduce new callers of this
  /// method.
  bool isRecursivelyConstructingRequirementMachine(
      const ProtocolDecl *proto);

  /// Retrieve a generic parameter list with a single parameter named `Self`.
  /// This is for parsing @opened archetypes in textual SIL.
  GenericParamList *getSelfGenericParamList(DeclContext *dc) const;

  /// Retrieve a generic signature with a single unconstrained type parameter,
  /// like `<T>`.
  CanGenericSignature getSingleGenericParameterSignature() const;

  /// Retrieve a generic signature with a single type parameter conforming
  /// to the given protocol or composition type, like <T: P>.
  ///
  /// The opened archetype may have a different set of conformances from the
  /// corresponding existential. The opened archetype conformances are dictated
  /// by the ABI for generic arguments, while the existential value conformances
  /// are dictated by their layout (see \c Type::getExistentialLayout()). In
  /// particular, the opened archetype signature does not have requirements for
  /// conformances inherited from superclass constraints while existential
  /// values do.
  CanGenericSignature getOpenedExistentialSignature(Type type,
                                                    GenericSignature parentSig);

  /// Get a generic signature where the generic parameter τ_d_i represents
  /// the element of the pack generic parameter τ_d_i… in \p baseGenericSig.
  ///
  /// This drops the parameter pack bit from each generic parameter,
  /// and converts same-element requirements to same-type requirements.
  CanGenericSignature getOpenedElementSignature(CanGenericSignature baseGenericSig,
                                                CanGenericTypeParamType shapeClass);

  GenericSignature getOverrideGenericSignature(const ValueDecl *base,
                                               const ValueDecl *derived);

  GenericSignature
  getOverrideGenericSignature(const NominalTypeDecl *baseNominal,
                              const NominalTypeDecl *derivedNominal,
                              GenericSignature baseGenericSig,
                              const GenericParamList *derivedParams);

  enum class OverrideGenericSignatureReqCheck {
    /// Base method's generic requirements are satisfied by derived method
    BaseReqSatisfiedByDerived,

    /// Derived method's generic requirements are satisfied by base method
    DerivedReqSatisfiedByBase
  };

  bool overrideGenericSignatureReqsSatisfied(
      const ValueDecl *base, const ValueDecl *derived,
      const OverrideGenericSignatureReqCheck direction);

  /// Whether our effective Swift version is at least 'major'.
  ///
  /// This is usually the check you want; for example, when introducing
  /// a new language feature which is only visible in Swift 5, you would
  /// check for isSwiftVersionAtLeast(5).
  bool isSwiftVersionAtLeast(unsigned major, unsigned minor = 0) const {
    return LangOpts.isSwiftVersionAtLeast(major, minor);
  }

  /// Check whether it's important to respect access control restrictions
  /// in current context.
  bool isAccessControlDisabled() const {
    return !LangOpts.EnableAccessControl;
  }

  /// Each kind and SourceFile has its own cache for a Type.
  Type &getDefaultTypeRequestCache(SourceFile *, KnownProtocolKind);

  using SILTransformCtors = ArrayRef<SILTransform *(*)(void)>;

  /// Register IRGen specific SIL passes such that the SILOptimizer can access
  /// and execute them without directly depending on IRGen.
  void registerIRGenSILTransforms(SILTransformCtors fns);

  /// Retrieve the IRGen specific SIL passes.
  SILTransformCtors getIRGenSILTransforms() const;
  
  /// Check whether a given string would be considered "pure ASCII" by the
  /// standard library's String implementation.
  bool isASCIIString(StringRef s) const;

  /// Retrieve the name of to be used for the entry point, either main or an
  /// alternative specified via the -entry-point-function-name frontend flag.
  std::string getEntryPointFunctionName() const;

  Type getAssociatedTypeOfDistributedSystemOfActor(NominalTypeDecl *actor,
                                            Identifier member);

  /// Find the concrete invocation decoder associated with the given actor.
  NominalTypeDecl *
  getDistributedActorInvocationDecoder(NominalTypeDecl *);

  /// Find `decodeNextArgument<T>(type: T.Type) -> T` method associated with
  /// invocation decoder of the given distributed actor.
  FuncDecl *getDistributedActorArgumentDecodingMethod(NominalTypeDecl *);

  /// The special Builtin.TheTupleType, which parents tuple extensions and
  /// conformances.
  BuiltinTupleDecl *getBuiltinTupleDecl();

  /// The declared interface type of Builtin.TheTupleType.
  BuiltinTupleType *getBuiltinTupleType();

  Type getNamedSwiftType(ModuleDecl *module, StringRef name);

  /// Set the plugin loader.
  void setPluginLoader(std::unique_ptr<PluginLoader> loader);

  /// Get the plugin loader.
  PluginLoader &getPluginLoader();

  /// Get the output backend. The output backend needs to be initialized via
  /// constructor or `setOutputBackend`.
  llvm::vfs::OutputBackend &getOutputBackend() const {
    assert(OutputBackend && "OutputBackend is not setup");
    return *OutputBackend;
  }
  /// Set output backend for virtualized outputs.
  void setOutputBackend(
      llvm::IntrusiveRefCntPtr<llvm::vfs::OutputBackend> OutBackend) {
    OutputBackend = std::move(OutBackend);
  }

private:
  friend Decl;

  Optional<ExternalSourceLocs *> getExternalSourceLocs(const Decl *D);
  void setExternalSourceLocs(const Decl *D, ExternalSourceLocs *Locs);

  friend TypeBase;
  friend ArchetypeType;
  friend OpaqueTypeDecl;

  /// Provide context-level uniquing for SIL lowered type layouts and boxes.
  friend SILLayout;
  friend SILBoxType;
};

} // end namespace swift

#endif
