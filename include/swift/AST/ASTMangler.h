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

#include "swift/Basic/Mangler.h"
#include "swift/AST/Types.h"
#include "swift/AST/Decl.h"

namespace clang {
class NamedDecl;
}

namespace swift {

class AbstractClosureExpr;
class ConformanceAccessPath;
class RootProtocolConformance;

namespace Mangle {

/// The mangler for AST declarations.
class ASTMangler : public Mangler {
protected:
  CanGenericSignature CurGenericSignature;
  ModuleDecl *Mod = nullptr;

  /// Optimize out protocol names if a type only conforms to one protocol.
  bool OptimizeProtocolNames = true;

  /// If enabled, use Objective-C runtime names when mangling @objc Swift
  /// protocols and classes.
  bool UseObjCRuntimeNames = false;

  /// If enabled, non-canonical types are allowed and type alias types get a
  /// special mangling.
  bool DWARFMangling;

  /// If enabled, entities that ought to have names but don't get a placeholder.
  ///
  /// If disabled, it is an error to try to mangle such an entity.
  bool AllowNamelessEntities = false;

  /// If enabled, some entities will be emitted as symbolic reference
  /// placeholders. The offsets of these references will be stored in the
  /// `SymbolicReferences` vector, and it is up to the consumer of the mangling
  /// to fill these in.
  bool AllowSymbolicReferences = false;

public:
  using SymbolicReferent = llvm::PointerUnion<const NominalTypeDecl *,
                                              const OpaqueTypeDecl *,
                                              const ProtocolConformance *>;
protected:

  /// If set, the mangler calls this function to determine whether to symbolic
  /// reference a given entity. If null, the mangler acts as if it's set to
  /// always return true.
  std::function<bool (SymbolicReferent)> CanSymbolicReference;
  
  bool canSymbolicReference(SymbolicReferent referent) {
    // Marker protocols cannot ever be symbolically referenced.
    if (auto nominal = referent.dyn_cast<const NominalTypeDecl *>()) {
      if (auto proto = dyn_cast<ProtocolDecl>(nominal)) {
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
    AsyncHandlerBody,
    DynamicThunk,
    SwiftAsObjCThunk,
    ObjCAsSwiftThunk,
  };

  ASTMangler(bool DWARFMangling = false)
    : DWARFMangling(DWARFMangling) {}

  void addTypeSubstitution(Type type) {
    type = dropProtocolsFromAssociatedTypes(type);
    addSubstitution(type.getPointer());
  }
  bool tryMangleTypeSubstitution(Type type) {
    type = dropProtocolsFromAssociatedTypes(type);
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
                                             ModuleDecl *Module);

  /// Mangle a completion handler block implementation function, used for importing ObjC
  /// APIs as async.
  ///
  /// - If `predefined` is true, this mangles the symbol name of the completion handler
  /// predefined in the Swift runtime for the given type signature.
  std::string mangleObjCAsyncCompletionHandlerImpl(CanSILFunctionType BlockType,
                                                   CanType ResultType,
                                                   bool predefined);
  
  /// Mangle the derivative function (JVP/VJP) for the given:
  /// - Mangled original function declaration.
  /// - Derivative function kind.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string
  mangleAutoDiffDerivativeFunction(const AbstractFunctionDecl *originalAFD,
                                   AutoDiffDerivativeFunctionKind kind,
                                   AutoDiffConfig config);

  /// Mangle the linear map (differential/pullback) for the given:
  /// - Mangled original function declaration.
  /// - Linear map kind.
  /// - Derivative function configuration: parameter/result indices and
  ///   derivative generic signature.
  std::string mangleAutoDiffLinearMap(const AbstractFunctionDecl *originalAFD,
                                      AutoDiffLinearMapKind kind,
                                      AutoDiffConfig config);

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
                                     AutoDiffConfig config);

  /// Mangle a SIL differentiability witness key:
  /// - Mangled original function name.
  /// - Parameter indices.
  /// - Result indices.
  /// - Derivative generic signature (optional).
  std::string
  mangleSILDifferentiabilityWitnessKey(SILDifferentiabilityWitnessKey key);

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
  
  std::string mangleDeclType(const ValueDecl *decl);
  
  std::string mangleObjCRuntimeName(const NominalTypeDecl *Nominal);

  std::string mangleTypeWithoutPrefix(Type type) {
    appendType(type);
    return finalize();
  }

  std::string mangleTypeAsUSR(Type decl);

  std::string mangleTypeAsContextUSR(const NominalTypeDecl *type);

  std::string mangleDeclAsUSR(const ValueDecl *Decl, StringRef USRPrefix);

  std::string mangleAccessorEntityAsUSR(AccessorKind kind,
                                        const AbstractStorageDecl *decl,
                                        StringRef USRPrefix,
                                        bool IsStatic);

  std::string mangleLocalTypeDecl(const TypeDecl *type);

  std::string mangleOpaqueTypeDecl(const OpaqueTypeDecl *decl);

  std::string mangleOpaqueTypeDecl(const ValueDecl *decl);

  std::string mangleGenericSignature(const GenericSignature sig);

  enum SpecialContext {
    ObjCContext,
    ClangImporterContext,
  };
  
  static Optional<SpecialContext>
  getSpecialManglingContext(const ValueDecl *decl, bool useObjCProtocolNames);

  static const clang::NamedDecl *
  getClangDeclForMangling(const ValueDecl *decl);

protected:

  void appendSymbolKind(SymbolKind SKind);

  void appendType(Type type, const ValueDecl *forDecl = nullptr);
  
  void appendDeclName(const ValueDecl *decl);

  GenericTypeParamType *appendAssocType(DependentMemberType *DepTy,
                                        bool &isAssocTypeAtDepth);

  void appendOpWithGenericParamIndex(StringRef,
                                     const GenericTypeParamType *paramTy);

  void bindGenericParameters(GenericSignature sig);

  /// Mangles a sugared type iff we are mangling for the debugger.
  template <class T> void appendSugaredType(Type type,
                                            const ValueDecl *forDecl) {
    assert(DWARFMangling &&
           "sugared types are only legal when mangling for the debugger");
    auto *BlandTy = cast<T>(type.getPointer())->getSinglyDesugaredType();
    appendType(BlandTy, forDecl);
  }

  void appendBoundGenericArgs(Type type, bool &isFirstArgList);

  /// Append the bound generics arguments for the given declaration context
  /// based on a complete substitution map.
  ///
  /// \returns the number of generic parameters that were emitted
  /// thus far.
  unsigned appendBoundGenericArgs(DeclContext *dc,
                                  SubstitutionMap subs,
                                  bool &isFirstArgList);
  
  /// Append the bound generic arguments as a flat list, disregarding depth.
  void appendFlatGenericArgs(SubstitutionMap subs);

  /// Append any retroactive conformances.
  void appendRetroactiveConformances(Type type);
  void appendRetroactiveConformances(SubstitutionMap subMap,
                                     ModuleDecl *fromModule);
  void appendImplFunctionType(SILFunctionType *fn);

  void appendContextOf(const ValueDecl *decl);

  void appendContext(const DeclContext *ctx, StringRef useModuleName);

  void appendModule(const ModuleDecl *module, StringRef useModuleName);

  void appendProtocolName(const ProtocolDecl *protocol,
                          bool allowStandardSubstitution = true);

  void appendAnyGenericType(const GenericTypeDecl *decl);

  enum FunctionManglingKind {
    NoFunctionMangling,
    FunctionMangling,
    AsyncHandlerBodyMangling
  };

  void appendFunction(AnyFunctionType *fn,
                    FunctionManglingKind functionMangling = NoFunctionMangling,
                    const ValueDecl *forDecl = nullptr);
  void appendFunctionType(AnyFunctionType *fn, bool isAutoClosure = false,
                          const ValueDecl *forDecl = nullptr);
  void appendClangType(AnyFunctionType *fn);
  template <typename FnType>
  void appendClangType(FnType *fn, llvm::raw_svector_ostream &os);

  void appendFunctionSignature(AnyFunctionType *fn,
                               const ValueDecl *forDecl,
                               FunctionManglingKind functionMangling);

  void appendFunctionInputType(ArrayRef<AnyFunctionType::Param> params,
                               const ValueDecl *forDecl = nullptr);
  void appendFunctionResultType(Type resultType,
                                const ValueDecl *forDecl = nullptr);

  void appendTypeList(Type listTy, const ValueDecl *forDecl = nullptr);
  void appendTypeListElement(Identifier name, Type elementType,
                             ParameterTypeFlags flags,
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

  void appendRequirement(const Requirement &reqt);

  void appendGenericSignatureParts(TypeArrayView<GenericTypeParamType> params,
                                   unsigned initialParamDepth,
                                   ArrayRef<Requirement> requirements);

  DependentMemberType *dropProtocolFromAssociatedType(DependentMemberType *dmt);
  Type dropProtocolsFromAssociatedTypes(Type type);

  void appendAssociatedTypeName(DependentMemberType *dmt);

  void appendClosureEntity(const SerializedAbstractClosureExpr *closure);
  
  void appendClosureEntity(const AbstractClosureExpr *closure);

  void appendClosureComponents(Type Ty, unsigned discriminator, bool isImplicit,
                               const DeclContext *parentContext);

  void appendDefaultArgumentEntity(const DeclContext *ctx, unsigned index);

  void appendInitializerEntity(const VarDecl *var);
  void appendBackingInitializerEntity(const VarDecl *var);

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

  void appendEntity(const ValueDecl *decl, bool isAsyncHandlerBody = false);

  void appendProtocolConformance(const ProtocolConformance *conformance);
  void appendProtocolConformanceRef(const RootProtocolConformance *conformance);
  void appendAnyProtocolConformance(CanGenericSignature genericSig,
                                    CanType conformingType,
                                    ProtocolConformanceRef conformance);
  void appendConcreteProtocolConformance(
                                        const ProtocolConformance *conformance);
  void appendDependentProtocolConformance(const ConformanceAccessPath &path);
  void appendOpParamForLayoutConstraint(LayoutConstraint Layout);
  
  void appendSymbolicReference(SymbolicReferent referent);
  
  void appendOpaqueDeclName(const OpaqueTypeDecl *opaqueDecl);

  void beginManglingWithAutoDiffOriginalFunction(
      const AbstractFunctionDecl *afd);
  void appendAutoDiffFunctionParts(char functionKindCode,
                                   AutoDiffConfig config);
  void appendIndexSubset(IndexSubset *indexSubset);
};

} // end namespace Mangle
} // end namespace swift

#endif // SWIFT_AST_ASTMANGLER_H
