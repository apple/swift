//===--- ASTMangler.h - Swift AST symbol mangling ---------------*- C++ -*-===//
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

#ifndef SWIFT_AST_ASTMANGLER_H
#define SWIFT_AST_ASTMANGLER_H

#include "swift/AST/Decl.h"
#include "swift/AST/FreestandingMacroExpansion.h"
#include "swift/AST/Types.h"
#include "swift/Basic/Mangler.h"
#include "swift/Basic/TaggedUnion.h"
#include "llvm/ADT/Optional.h"

namespace clang {
class NamedDecl;
}

namespace swift {

class AbstractClosureExpr;
class ConformancePath;
class MacroExpansionExpr;
class RootProtocolConformance;

namespace Mangle {

/// The mangler for AST declarations.
class ASTMangler : public Mangler {
protected:
  ModuleDecl *Mod = nullptr;

  /// Optimize out protocol names if a type only conforms to one protocol.
  bool OptimizeProtocolNames = true;

  /// If enabled, use Objective-C runtime names when mangling @objc Swift
  /// protocols and classes.
  bool UseObjCRuntimeNames = false;

  /// If enabled, non-canonical types are allowed and type alias types get a
  /// special mangling.
  bool DWARFMangling = false;

  /// If enabled, entities that ought to have names but don't get a placeholder.
  ///
  /// If disabled, it is an error to try to mangle such an entity.
  bool AllowNamelessEntities = false;

  /// If enabled, some entities will be emitted as symbolic reference
  /// placeholders. The offsets of these references will be stored in the
  /// `SymbolicReferences` vector, and it is up to the consumer of the mangling
  /// to fill these in.
  bool AllowSymbolicReferences = false;

  /// If enabled, allows the use of standard substitutions for types in the
  /// standard library.
  bool AllowStandardSubstitutions = true;

  /// If enabled, allows the use of standard substitutions for types in the
  /// concurrency library.
  bool AllowConcurrencyStandardSubstitutions = true;

  /// If enabled, marker protocols can be encoded in the mangled name.
  bool AllowMarkerProtocols = true;

  /// Whether the mangling predates concurrency, and therefore shouldn't
  /// include concurrency features such as global actors or @Sendable
  /// function types.
  bool Preconcurrency = false;

  /// If enabled, declarations annotated with @_originallyDefinedIn are mangled
  /// as if they're part of their original module. Disabled for debug mangling,
  /// because lldb wants to find declarations in the modules they're currently
  /// defined in.
  bool RespectOriginallyDefinedIn = true;

public:
  class SymbolicReferent {
  public:
    enum Kind {
      NominalType,
      OpaqueType,
      ExtendedExistentialTypeShape,
    };
  private:
    // TODO: make a TaggedUnion variant that works with an explicit
    // kind instead of requiring this redundant kind storage.
    TaggedUnion<const NominalTypeDecl *,
                const OpaqueTypeDecl *,
                Type>
      storage;
    Kind kind;

    SymbolicReferent(Kind kind, Type type) : storage(type), kind(kind) {}
  public:
    SymbolicReferent(const NominalTypeDecl *decl)
       : storage(decl), kind(NominalType) {}
    SymbolicReferent(const OpaqueTypeDecl *decl)
       : storage(decl), kind(OpaqueType) {}
    static SymbolicReferent forExtendedExistentialTypeShape(Type type) {
      return SymbolicReferent(ExtendedExistentialTypeShape, type);
    }

    Kind getKind() const { return kind; }

    bool isNominalType() const { return kind == NominalType; }
    const NominalTypeDecl *getNominalType() const {
      assert(kind == NominalType);
      return storage.get<const NominalTypeDecl *>();
    }

    const OpaqueTypeDecl *getOpaqueType() const {
      assert(kind == OpaqueType);
      return storage.get<const OpaqueTypeDecl *>();
    }

    Type getType() const {
      assert(kind == ExtendedExistentialTypeShape);
      return storage.get<Type>();
    }
  };
protected:

  /// If set, the mangler calls this function to determine whether to symbolic
  /// reference a given entity. If null, the mangler acts as if it's set to
  /// always return true.
  std::function<bool (SymbolicReferent)> CanSymbolicReference;
  
  bool canSymbolicReference(SymbolicReferent referent) {
    // Marker protocols cannot ever be symbolically referenced.
    if (referent.isNominalType()) {
      if (auto proto = dyn_cast<ProtocolDecl>(referent.getNominalType())) {
        if (proto->isMarkerProtocol())
          return false;
      }
    }

    return AllowSymbolicReferences
      && (!CanSymbolicReference || CanSymbolicReference(referent));
  }

  std::vector<std::pair<SymbolicReferent, unsigned>> SymbolicReferences;
  
public:
  enum class SymbolKind {
    Default,
    DynamicThunk,
    SwiftAsObjCThunk,
    ObjCAsSwiftThunk,
    DistributedThunk,
    DistributedAccessor,
    AccessibleFunctionRecord,
    BackDeploymentThunk,
    BackDeploymentFallback,
    HasSymbolQuery,
    RuntimeDiscoverableAttributeRecord,
  };

  /// lldb overrides the defaulted argument to 'true'.
  ASTMangler(bool DWARFMangling = false) {
    if (DWARFMangling) {
      DWARFMangling = true;
      RespectOriginallyDefinedIn = false;
    }
  }

  void addTypeSubstitution(Type type, GenericSignature sig) {
    type = dropProtocolsFromAssociatedTypes(type, sig);
    addSubstitution(type.getPointer());
  }
  bool tryMangleTypeSubstitution(Type type, GenericSignature sig) {
    type = dropProtocolsFromAssociatedTypes(type, sig);
    return tryMangleSubstitution(type.getPointer());
  }

  std::string mangleClosureEntity(const AbstractClosureExpr *closure,
                                  SymbolKind SKind);

  std::string mangleEntity(const ValueDecl *decl,
                           SymbolKind SKind = SymbolKind::Default);

  std::string mangleDestructorEntity(const DestructorDecl *decl,
                                     bool isDeallocating,
                                     SymbolKind SKind = SymbolKind::Default);

  std::string mangleConstructorEntity(const ConstructorDecl *ctor,
                                      bool isAllocating,
                                      SymbolKind SKind = SymbolKind::Default);

  std::string mangleIVarInitDestroyEntity(const ClassDecl *decl,
                                          bool isDestroyer, SymbolKind SKind);

  std::string mangleAccessorEntity(AccessorKind kind,
                                   const AbstractStorageDecl *decl,
                                   bool isStatic,
                                   SymbolKind SKind);

  std::string mangleGlobalGetterEntity(const ValueDecl *decl,
                                       SymbolKind SKind = SymbolKind::Default);

  std::string mangleDefaultArgumentEntity(const DeclContext *func,
                                          unsigned index,
                                          SymbolKind SKind = SymbolKind::Default);

  std::string mangleInitializerEntity(const VarDecl *var, SymbolKind SKind);
  std::string mangleBackingInitializerEntity(const VarDecl *var,
                                             SymbolKind SKind = SymbolKind::Default);
  std::string mangleInitFromProjectedValueEntity(const VarDecl *var,
                                                 SymbolKind SKind = SymbolKind::Default);

  std::string mangleNominalType(const NominalTypeDecl *decl);

  std::string mangleVTableThunk(const FuncDecl *Base,
                                const FuncDecl *Derived);

  std::string mangleConstructorVTableThunk(const ConstructorDecl *Base,
                                           const ConstructorDecl *Derived,
                                           bool isAllocating);

  std::string mangleWitnessTable(const RootProtocolConformance *C);

  std::string mangleWitnessThunk(const ProtocolConformance *Conformance,
                                 const ValueDecl *Requirement);

  std::string mangleClosureWitnessThunk(const ProtocolConformance *Conformance,
                                        const AbstractClosureExpr *Closure);

  std::string mangleGlobalVariableFull(const VarDecl *decl);

  std::string mangleGlobalInit(const PatternBindingDecl *decl,
                               unsigned entry,
                               bool isInitFunc);

  std::string mangleReabstractionThunkHelper(CanSILFunctionType ThunkType,
                                             Type FromType, Type ToType,
                                             Type SelfType,
                                             Type GlobalActorBound,
                                             ModuleDecl *Module);

  std::string mangleDistributedThunk(const AbstractFunctionDecl *thunk);

  /// Mangle a completion handler block implementation function, used for importing ObjC
  /// APIs as async.
  ///
  /// - If `predefined` is true, this mangles the symbol name of the completion handler
  /// predefined in the Swift runtime for the given type signature.
  std::string mangleObjCAsyncCompletionHandlerImpl(
      CanSILFunctionType BlockType, CanType ResultType, CanGenericSignature Sig,
      llvm::Optional<bool> FlagParamIsZeroOnError, bool predefined);

  /// Mangle the derivative function (JVP/VJP), or optionally its vtable entry
  /// thunk, for the given:
  /// - Mangled original function declaration.
  /// - Derivative function kind.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string
  mangleAutoDiffDerivativeFunction(const AbstractFunctionDecl *originalAFD,
                                   AutoDiffDerivativeFunctionKind kind,
                                   const AutoDiffConfig &config,
                                   bool isVTableThunk = false);

  /// Mangle the linear map (differential/pullback) for the given:
  /// - Mangled original function declaration.
  /// - Linear map kind.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string mangleAutoDiffLinearMap(const AbstractFunctionDecl *originalAFD,
                                      AutoDiffLinearMapKind kind,
                                      const AutoDiffConfig &config);

  /// Mangle the linear map self parameter reordering thunk the given:
  /// - Mangled original function declaration.
  /// - Linear map kind.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string mangleAutoDiffSelfReorderingReabstractionThunk(
      CanType fromType, CanType toType, GenericSignature signature,
      AutoDiffLinearMapKind linearMapKind);

  /// Mangle a SIL differentiability witness.
  std::string mangleSILDifferentiabilityWitness(StringRef originalName,
                                                DifferentiabilityKind kind,
                                                const AutoDiffConfig &config);

  /// Mangle the AutoDiff generated declaration for the given:
  /// - Generated declaration kind: linear map struct or branching trace enum.
  /// - Mangled original function name.
  /// - Basic block number.
  /// - Linear map kind: differential or pullback.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string
  mangleAutoDiffGeneratedDeclaration(AutoDiffGeneratedDeclarationKind declKind,
                                     StringRef origFnName, unsigned bbId,
                                     AutoDiffLinearMapKind linearMapKind,
                                     const AutoDiffConfig &config);

  std::string mangleKeyPathGetterThunkHelper(const AbstractStorageDecl *property,
                                             GenericSignature signature,
                                             CanType baseType,
                                             SubstitutionMap subs,
                                             ResilienceExpansion expansion);
  std::string mangleKeyPathSetterThunkHelper(const AbstractStorageDecl *property,
                                             GenericSignature signature,
                                             CanType baseType,
                                             SubstitutionMap subs,
                                             ResilienceExpansion expansion);
  std::string mangleKeyPathEqualsHelper(ArrayRef<CanType> indices,
                                        GenericSignature signature,
                                        ResilienceExpansion expansion);
  std::string mangleKeyPathHashHelper(ArrayRef<CanType> indices,
                                      GenericSignature signature,
                                      ResilienceExpansion expansion);

  std::string mangleTypeForDebugger(Type decl, GenericSignature sig);

  /// Create a mangled name to be used for _typeName constant propagation.
  std::string mangleTypeForTypeName(Type type);

  std::string mangleOpaqueTypeDescriptor(const OpaqueTypeDecl *decl);

  std::string mangleOpaqueTypeDescriptorRecord(const OpaqueTypeDecl *decl);
  
  std::string mangleDeclType(const ValueDecl *decl);
  
  std::string mangleObjCRuntimeName(const NominalTypeDecl *Nominal);

  std::string mangleTypeWithoutPrefix(Type type) {
    appendType(type, nullptr);
    return finalize();
  }

  std::string mangleTypeAsUSR(Type decl);

  std::string mangleTypeAsContextUSR(const NominalTypeDecl *type);

  void appendAnyDecl(const ValueDecl *Decl);
  std::string mangleAnyDecl(const ValueDecl *Decl, bool prefix,
                            bool respectOriginallyDefinedIn = false);
  std::string mangleDeclAsUSR(const ValueDecl *Decl, StringRef USRPrefix);

  std::string mangleAccessorEntityAsUSR(AccessorKind kind,
                                        const AbstractStorageDecl *decl,
                                        StringRef USRPrefix,
                                        bool IsStatic);

  std::string mangleLocalTypeDecl(const TypeDecl *type);

  std::string mangleOpaqueTypeDecl(const OpaqueTypeDecl *decl);

  std::string mangleOpaqueTypeDecl(const ValueDecl *decl);

  std::string mangleGenericSignature(const GenericSignature sig);

  std::string mangleHasSymbolQuery(const ValueDecl *decl);

  std::string
  mangleRuntimeAttributeGeneratorEntity(const ValueDecl *decl, CustomAttr *attr,
                                        SymbolKind SKind = SymbolKind::Default);

  std::string mangleMacroExpansion(const FreestandingMacroExpansion *expansion);
  std::string mangleAttachedMacroExpansion(
      const Decl *decl, CustomAttr *attr, MacroRole role);

  void appendMacroExpansionContext(SourceLoc loc, DeclContext *origDC);
  void appendMacroExpansionOperator(
      StringRef macroName, MacroRole role, unsigned discriminator);

  enum SpecialContext {
    ObjCContext,
    ClangImporterContext,
  };

  static llvm::Optional<SpecialContext>
  getSpecialManglingContext(const ValueDecl *decl, bool useObjCProtocolNames);

  static bool isCXXCFOptionsDefinition(const ValueDecl *decl);
  static const clang::TypedefType *
  getTypeDefForCXXCFOptionsDefinition(const ValueDecl *decl);

  static const clang::NamedDecl *getClangDeclForMangling(const ValueDecl *decl);

  void appendExistentialLayout(
      const ExistentialLayout &layout, GenericSignature sig,
      const ValueDecl *forDecl);

protected:

  void appendSymbolKind(SymbolKind SKind);

  void appendType(Type type, GenericSignature sig,
                  const ValueDecl *forDecl = nullptr);
  
  void appendDeclName(
      const ValueDecl *decl, DeclBaseName name = DeclBaseName());

  GenericTypeParamType *appendAssocType(DependentMemberType *DepTy,
                                        GenericSignature sig,
                                        bool &isAssocTypeAtDepth);

  void appendOpWithGenericParamIndex(StringRef,
                                     const GenericTypeParamType *paramTy,
                                     bool baseIsProtocolSelf = false);

  /// Mangles a sugared type iff we are mangling for the debugger.
  template <class T> void appendSugaredType(Type type,
                                            const ValueDecl *forDecl) {
    assert(DWARFMangling &&
           "sugared types are only legal when mangling for the debugger");
    auto *BlandTy = cast<T>(type.getPointer())->getSinglyDesugaredType();
    appendType(BlandTy, forDecl);
  }

  void appendBoundGenericArgs(Type type, GenericSignature sig,
                              bool &isFirstArgList,
                              const ValueDecl *forDecl = nullptr);

  /// Append the bound generics arguments for the given declaration context
  /// based on a complete substitution map.
  ///
  /// \returns the number of generic parameters that were emitted
  /// thus far.
  unsigned appendBoundGenericArgs(DeclContext *dc,
                                  GenericSignature sig,
                                  SubstitutionMap subs,
                                  bool &isFirstArgList,
                                  const ValueDecl *forDecl = nullptr);
  
  /// Append the bound generic arguments as a flat list, disregarding depth.
  void appendFlatGenericArgs(SubstitutionMap subs,
                             GenericSignature sig,
                             const ValueDecl *forDecl = nullptr);

  /// Append any retroactive conformances.
  void appendRetroactiveConformances(Type type, GenericSignature sig);
  void appendRetroactiveConformances(SubstitutionMap subMap,
                                     GenericSignature sig,
                                     ModuleDecl *fromModule);
  void appendImplFunctionType(SILFunctionType *fn, GenericSignature sig,
                              const ValueDecl *forDecl = nullptr);
  void appendOpaqueTypeArchetype(ArchetypeType *archetype,
                                 OpaqueTypeDecl *opaqueDecl,
                                 SubstitutionMap subs,
                                 GenericSignature sig,
                                 const ValueDecl *forDecl);

  void appendContextOf(const ValueDecl *decl);

  void appendContext(const DeclContext *ctx, StringRef useModuleName);

  void appendModule(const ModuleDecl *module, StringRef useModuleName);

  void appendProtocolName(const ProtocolDecl *protocol,
                          bool allowStandardSubstitution = true);

  void appendAnyGenericType(const GenericTypeDecl *decl);

  enum FunctionManglingKind {
    NoFunctionMangling,
    FunctionMangling,
  };

  void appendFunction(AnyFunctionType *fn, GenericSignature sig,
                    FunctionManglingKind functionMangling = NoFunctionMangling,
                    const ValueDecl *forDecl = nullptr);
  void appendFunctionType(AnyFunctionType *fn, GenericSignature sig,
                          bool isAutoClosure = false,
                          const ValueDecl *forDecl = nullptr);
  void appendClangType(AnyFunctionType *fn);
  template <typename FnType>
  void appendClangType(FnType *fn, llvm::raw_svector_ostream &os);

  void appendFunctionSignature(AnyFunctionType *fn,
                               GenericSignature sig,
                               const ValueDecl *forDecl,
                               FunctionManglingKind functionMangling);

  void appendFunctionInputType(ArrayRef<AnyFunctionType::Param> params,
                               GenericSignature sig,
                               const ValueDecl *forDecl = nullptr);
  void appendFunctionResultType(Type resultType,
                                GenericSignature sig,
                                const ValueDecl *forDecl = nullptr);

  void appendTypeList(Type listTy, GenericSignature sig,
                      const ValueDecl *forDecl = nullptr);
  void appendTypeListElement(Identifier name, Type elementType,
                             ParameterTypeFlags flags,
                             GenericSignature sig,
                             const ValueDecl *forDecl = nullptr);

  /// Append a generic signature to the mangling.
  ///
  /// \param sig The generic signature.
  ///
  /// \param contextSig The signature of the known context. This function
  /// will only mangle the difference between \c sig and \c contextSig.
  ///
  /// \returns \c true if a generic signature was appended, \c false
  /// if it was empty.
  bool appendGenericSignature(GenericSignature sig,
                              GenericSignature contextSig = nullptr);

  /// Append a requirement to the mangling.
  ///
  /// \param reqt The requirement to mangle
  /// \param sig  The generic signature.
  /// \param lhsBaseIsProtocolSelf If \c true, mangle the base of the left-hand
  /// side of the constraint with a special protocol 'Self' sentinel node. This
  /// supports distinguishing requirements rooted at 'Self' in constrained
  /// existentials from ambient generic parameters that would otherwise be
  /// at e.g. (0, 0) as well.
  void appendRequirement(const Requirement &reqt, GenericSignature sig,
                         bool lhsBaseIsProtocolSelf = false);

  void appendGenericSignatureParts(GenericSignature sig,
                                   ArrayRef<CanTypeWrapper<GenericTypeParamType>> params,
                                   unsigned initialParamDepth,
                                   ArrayRef<Requirement> requirements);

  DependentMemberType *dropProtocolFromAssociatedType(DependentMemberType *dmt,
                                                      GenericSignature sig);
  Type dropProtocolsFromAssociatedTypes(Type type,
                                        GenericSignature sig);

  void appendAssociatedTypeName(DependentMemberType *dmt,
                                GenericSignature sig);

  void appendClosureEntity(const SerializedAbstractClosureExpr *closure);
  
  void appendClosureEntity(const AbstractClosureExpr *closure);

  void appendClosureComponents(Type Ty, unsigned discriminator, bool isImplicit,
                               const DeclContext *parentContext);

  void appendDefaultArgumentEntity(const DeclContext *ctx, unsigned index);

  void appendInitializerEntity(const VarDecl *var);
  void appendBackingInitializerEntity(const VarDecl *var);
  void appendInitFromProjectedValueEntity(const VarDecl *var);

  CanType getDeclTypeForMangling(const ValueDecl *decl,
                                 GenericSignature &genericSig,
                                 GenericSignature &parentGenericSig);

  

  void appendDeclType(const ValueDecl *decl,
                    FunctionManglingKind functionMangling = NoFunctionMangling);

  bool tryAppendStandardSubstitution(const GenericTypeDecl *type);

  void appendConstructorEntity(const ConstructorDecl *ctor, bool isAllocating);
  
  void appendDestructorEntity(const DestructorDecl *decl, bool isDeallocating);

  /// \param accessorKindCode The code to describe the accessor and addressor
  /// kind. Usually retrieved using getCodeForAccessorKind.
  /// \param decl The storage decl for which to mangle the accessor
  /// \param isStatic Whether or not the accessor is static
  void appendAccessorEntity(StringRef accessorKindCode,
                            const AbstractStorageDecl *decl, bool isStatic);

  void appendEntity(const ValueDecl *decl, StringRef EntityOp, bool isStatic);

  void appendEntity(const ValueDecl *decl);

  void appendProtocolConformance(const ProtocolConformance *conformance);
  void appendProtocolConformanceRef(const RootProtocolConformance *conformance);
  void appendAnyProtocolConformance(GenericSignature genericSig,
                                    CanType conformingType,
                                    ProtocolConformanceRef conformance);
  void appendConcreteProtocolConformance(
                                        const ProtocolConformance *conformance,
                                        GenericSignature sig);
  void appendDependentProtocolConformance(const ConformancePath &path,
                                          GenericSignature sig);
  void appendOpParamForLayoutConstraint(LayoutConstraint Layout);
  
  void appendSymbolicExtendedExistentialType(SymbolicReferent shapeReferent,
                                             Type type,
                                             GenericSignature sig,
                                             const ValueDecl *forDecl);
  void appendSymbolicReference(SymbolicReferent referent);
  
  void appendOpaqueDeclName(const OpaqueTypeDecl *opaqueDecl);

  void beginManglingWithAutoDiffOriginalFunction(
      const AbstractFunctionDecl *afd);
  void appendAutoDiffFunctionParts(StringRef op, 
                                   Demangle::AutoDiffFunctionKind kind,
                                   const AutoDiffConfig &config);
  void appendIndexSubset(IndexSubset *indexSubset);

  void appendConstrainedExistential(Type base, GenericSignature sig,
                                    const ValueDecl *forDecl);

  void appendRuntimeAttributeGeneratorEntity(const ValueDecl *decl,
                                             CustomAttr *attr);
};

} // end namespace Mangle
} // end namespace swift

#endif // SWIFT_AST_ASTMANGLER_H
