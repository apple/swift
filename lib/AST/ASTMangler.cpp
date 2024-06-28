//===--- ASTMangler.cpp - Swift AST symbol mangling -----------------------===//
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
//  This file implements declaration name mangling in Swift.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/ASTMangler.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTVisitor.h"
#include "swift/AST/AutoDiff.h"
#include "swift/AST/Decl.h"
#include "swift/AST/ExistentialLayout.h"
#include "swift/AST/Expr.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/GenericSignature.h"
#include "swift/AST/Initializer.h"
#include "swift/AST/LazyResolver.h"
#include "swift/AST/LocalArchetypeRequirementCollector.h"
#include "swift/AST/MacroDiscriminatorContext.h"
#include "swift/AST/Module.h"
#include "swift/AST/Ownership.h"
#include "swift/AST/PackConformance.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/PrettyStackTrace.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/AST/ProtocolConformanceRef.h"
#include "swift/AST/SILLayout.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Defer.h"
#include "swift/Basic/SourceManager.h"
#include "swift/ClangImporter/ClangImporter.h"
#include "swift/Demangling/Demangler.h"
#include "swift/Demangling/ManglingMacros.h"
#include "swift/Demangling/ManglingUtils.h"
#include "swift/Strings.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/AST/Decl.h"
#include "clang/AST/DeclObjC.h"
#include "clang/AST/DeclTemplate.h"
#include "clang/AST/Mangle.h"
#include "clang/Basic/CharInfo.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/SaveAndRestore.h"
#include "llvm/Support/raw_ostream.h"

#include <memory>

using namespace swift;
using namespace swift::Mangle;

bool ASTMangler::inversesAllowed(const Decl *decl) {
  if (!decl)
    return true;

  if (auto accessor = dyn_cast<AccessorDecl>(decl))
    if (auto *storage = accessor->getStorage())
      decl = storage;

  return !decl->getAttrs().hasAttribute<PreInverseGenericsAttr>();
}

bool ASTMangler::inversesAllowedIn(const DeclContext *ctx) {
  assert(ctx);
  return inversesAllowed(ctx->getInnermostDeclarationDeclContext());
}

static StringRef getCodeForAccessorKind(AccessorKind kind) {
  switch (kind) {
  case AccessorKind::Get:
    return "g";
  case AccessorKind::DistributedGet:
    // TODO(distributed): probably does not matter since we mangle distributed
    //  thunk getters as the name of the Storage?
    return "g";
  case AccessorKind::Set:
    return "s";
  case AccessorKind::WillSet:
    return "w";
  case AccessorKind::DidSet:
    return "W";
  case AccessorKind::Read:
    return "r";
  case AccessorKind::Modify:
    return "M";
  case AccessorKind::Address:
    // 'l' is for location. 'A' was taken.
    return "lu";
  case AccessorKind::MutableAddress:
    return "au";
  case AccessorKind::Init:
    return "i";
  }
  llvm_unreachable("bad accessor kind");
}

std::string ASTMangler::mangleClosureEntity(const AbstractClosureExpr *closure,
                                            SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowedIn(closure));
  beginMangling();
  appendClosureEntity(closure);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleEntity(const ValueDecl *decl, SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(decl));
  beginMangling();
  appendEntity(decl);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDestructorEntity(const DestructorDecl *decl,
                                               bool isDeallocating,
                                               SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(decl));
  beginMangling();
  appendDestructorEntity(decl, isDeallocating);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleConstructorEntity(const ConstructorDecl *ctor,
                                                bool isAllocating,
                                                SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(ctor));
  beginMangling();
  appendConstructorEntity(ctor, isAllocating);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleIVarInitDestroyEntity(const ClassDecl *decl,
                                                    bool isDestroyer,
                                                    SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(decl));
  beginMangling();
  BaseEntitySignature base(decl);
  appendContext(decl, base, decl->getAlternateModuleName());
  appendOperator(isDestroyer ? "fE" : "fe");
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleAccessorEntity(AccessorKind kind,
                                             const AbstractStorageDecl *decl,
                                             bool isStatic,
                                             SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(decl));
  beginMangling();
  appendAccessorEntity(getCodeForAccessorKind(kind), decl, isStatic);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleGlobalGetterEntity(const ValueDecl *decl,
                                                 SymbolKind SKind) {
  assert(isa<VarDecl>(decl) && "Only variables can have global getters");
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(decl));
  beginMangling();
  BaseEntitySignature base(decl);
  appendEntity(decl, base, "vG", /*isStatic*/false);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleDefaultArgumentEntity(const DeclContext *func,
                                                    unsigned index,
                                                    SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowedIn(func));
  beginMangling();
  appendDefaultArgumentEntity(func, index);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleInitializerEntity(const VarDecl *var,
                                                SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  beginMangling();
  appendInitializerEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleBackingInitializerEntity(const VarDecl *var,
                                                       SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  beginMangling();
  appendBackingInitializerEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleInitFromProjectedValueEntity(const VarDecl *var,
                                                           SymbolKind SKind) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  beginMangling();
  appendInitFromProjectedValueEntity(var);
  appendSymbolKind(SKind);
  return finalize();
}

std::string ASTMangler::mangleNominalType(const NominalTypeDecl *decl) {
  beginMangling();
  appendAnyGenericType(decl);
  return finalize();
}

std::string ASTMangler::mangleVTableThunk(const FuncDecl *Base,
                                          const FuncDecl *Derived) {
  beginMangling();

  appendEntity(Derived);
  appendEntity(Base);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleConstructorVTableThunk(
                                               const ConstructorDecl *Base,
                                               const ConstructorDecl *Derived,
                                               bool isAllocating) {
  beginMangling();

  appendConstructorEntity(Derived, isAllocating);
  appendConstructorEntity(Base, isAllocating);
  appendOperator("TV");

  return finalize();
}

std::string ASTMangler::mangleWitnessTable(const RootProtocolConformance *C) {
  llvm::SaveAndRestore X(AllowInverses,
                         inversesAllowedIn(C->getDeclContext()));

  beginMangling();
  if (isa<NormalProtocolConformance>(C)) {
    appendProtocolConformance(C);
    appendOperator("WP");
  } else if (isa<SelfProtocolConformance>(C)) {
    appendProtocolName(cast<SelfProtocolConformance>(C)->getProtocol());
    appendOperator("WS");
  } else {
    llvm_unreachable("mangling unknown conformance kind");
  }
  return finalize();
}

std::string ASTMangler::mangleWitnessThunk(
                                     const ProtocolConformance *Conformance,
                                           const ValueDecl *Requirement) {
  beginMangling();
  // Concrete witness thunks get a special mangling.
  if (Conformance) {
    if (!isa<SelfProtocolConformance>(Conformance)) {
      appendProtocolConformance(Conformance);
    }
  }

  if (auto ctor = dyn_cast<ConstructorDecl>(Requirement)) {
    appendConstructorEntity(ctor, /*isAllocating=*/true);
  } else {
    assert(isa<FuncDecl>(Requirement) && "expected function");
    appendEntity(cast<FuncDecl>(Requirement));
  }

  if (Conformance) {
    if (isa<SelfProtocolConformance>(Conformance)) {
      appendOperator("TS");
    } else {
      appendOperator("TW");
    }
  }

  return finalize();
}

std::string ASTMangler::mangleClosureWitnessThunk(
                                         const ProtocolConformance *Conformance,
                                         const AbstractClosureExpr *Closure) {
  beginMangling();
  appendProtocolConformance(Conformance);
  appendClosureEntity(Closure);
  appendOperator("TW");
  return finalize();
}

std::string ASTMangler::mangleGlobalVariableFull(const VarDecl *decl) {
  // Clang globals get mangled using Clang's mangler.
  if (auto clangDecl =
      dyn_cast_or_null<clang::DeclaratorDecl>(decl->getClangDecl())) {
    if (auto asmLabel = clangDecl->getAttr<clang::AsmLabelAttr>()) {
      Buffer << '\01' << asmLabel->getLabel();
    } else {
      if (clangDecl->getDeclContext()->isTranslationUnit()) {
        Buffer << clangDecl->getName();
      } else {
        std::unique_ptr<clang::MangleContext> mangler(
            decl->getClangDecl()->getASTContext().createMangleContext());
        mangler->mangleName(clangDecl, Buffer);
      }
    }
    return finalize();
  }
  beginMangling();
  appendEntity(decl);
  return finalize();
}

std::string ASTMangler::mangleKeyPathGetterThunkHelper(
                                            const AbstractStorageDecl *property,
                                            GenericSignature signature,
                                            CanType baseType,
                                            SubstitutionMap subs,
                                            ResilienceExpansion expansion) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType, signature);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto sub : subs.getReplacementTypes()) {
      sub = sub->mapTypeOutOfContext();

      // FIXME: This seems wrong. We used to just mangle opened archetypes as
      // their interface type. Let's make that explicit now.
      sub = sub.transformRec([](Type t) -> std::optional<Type> {
        if (auto *openedExistential = t->getAs<OpenedArchetypeType>())
          return openedExistential->getInterfaceType();
        return std::nullopt;
      });

      appendType(sub->getCanonicalType(), signature);
    }
  }
  appendOperator("TK");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathSetterThunkHelper(
                                          const AbstractStorageDecl *property,
                                          GenericSignature signature,
                                          CanType baseType,
                                          SubstitutionMap subs,
                                          ResilienceExpansion expansion) {
  beginMangling();
  appendEntity(property);
  if (signature)
    appendGenericSignature(signature);
  appendType(baseType, signature);
  if (isa<SubscriptDecl>(property)) {
    // Subscripts can be generic, and different key paths could capture the same
    // subscript at different generic arguments.
    for (auto sub : subs.getReplacementTypes()) {
      sub = sub->mapTypeOutOfContext();

      // FIXME: This seems wrong. We used to just mangle opened archetypes as
      // their interface type. Let's make that explicit now.
      sub = sub.transformRec([](Type t) -> std::optional<Type> {
        if (auto *openedExistential = t->getAs<OpenedArchetypeType>())
          return openedExistential->getInterfaceType();
        return std::nullopt;
      });

      appendType(sub->getCanonicalType(), signature);
    }
  }
  appendOperator("Tk");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathEqualsHelper(ArrayRef<CanType> indices,
                                                  GenericSignature signature,
                                                  ResilienceExpansion expansion) {
  beginMangling();
  for (auto &index : indices)
    appendType(index, nullptr);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("TH");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleKeyPathHashHelper(ArrayRef<CanType> indices,
                                                GenericSignature signature,
                                                ResilienceExpansion expansion) {
  beginMangling();
  for (auto &index : indices)
    appendType(index, nullptr);
  if (signature)
    appendGenericSignature(signature);
  appendOperator("Th");
  if (expansion == ResilienceExpansion::Minimal)
    appendOperator("q");
  return finalize();
}

std::string ASTMangler::mangleGlobalInit(const PatternBindingDecl *pd,
                                         unsigned pbdEntry,
                                         bool isInitFunc) {
  beginMangling();
  
  Pattern *pattern = pd->getPattern(pbdEntry);
  bool first = true;
  pattern->forEachVariable([&](VarDecl *D) {
    if (first) {
      BaseEntitySignature base(D);
      appendContextOf(D, base);
      first = false;
    }
    appendDeclName(D);
    appendListSeparator();
  });
  assert(!first && "no variables in pattern binding?!");
  
  if (isInitFunc) {
    appendOperator("WZ");
  } else {
    appendOperator("Wz");
  }
  
  return finalize();
}

std::string ASTMangler::mangleReabstractionThunkHelper(
                                            CanSILFunctionType ThunkType,
                                            Type FromType,
                                            Type ToType,
                                            Type SelfType,
                                            Type GlobalActorBound,
                                            ModuleDecl *Module) {
  Mod = Module;
  assert(ThunkType->getPatternSubstitutions().empty() && "not implemented");
  GenericSignature GenSig = ThunkType->getInvocationGenericSignature();

  beginMangling();
  appendType(FromType, GenSig);
  appendType(ToType, GenSig);
  if (SelfType)
    appendType(SelfType, GenSig);

  if (GenSig)
    appendGenericSignature(GenSig);

  if (SelfType)
    appendOperator("Ty");
  else
    appendOperator("TR");
  
  if (GlobalActorBound) {
    appendType(GlobalActorBound, GenSig);
    appendOperator("TU");
  }

  return finalize();
}

std::string ASTMangler::mangleObjCAsyncCompletionHandlerImpl(
    CanSILFunctionType BlockType, CanType ResultType, CanGenericSignature Sig,
    std::optional<bool> ErrorOnZero, bool predefined) {
  beginMangling();
  appendType(BlockType, Sig);
  appendType(ResultType, Sig);
  if (Sig)
    appendGenericSignature(Sig);
  if (ErrorOnZero)
    appendOperator(predefined ? "TZ" : "Tz", Index(*ErrorOnZero + 1));
  else
    appendOperator(predefined ? "TZ" : "Tz", Index(0));
  return finalize();
}

std::string ASTMangler::mangleAutoDiffDerivativeFunction(
    const AbstractFunctionDecl *originalAFD,
    AutoDiffDerivativeFunctionKind kind,
    const AutoDiffConfig &config,
    bool isVTableThunk) {
  beginManglingWithAutoDiffOriginalFunction(originalAFD);
  appendAutoDiffFunctionParts(
      isVTableThunk ? "TJV" : "TJ", getAutoDiffFunctionKind(kind), config);
  return finalize();
}

std::string ASTMangler::mangleAutoDiffLinearMap(
    const AbstractFunctionDecl *originalAFD, AutoDiffLinearMapKind kind,
    const AutoDiffConfig &config) {
  beginManglingWithAutoDiffOriginalFunction(originalAFD);
  appendAutoDiffFunctionParts("TJ", getAutoDiffFunctionKind(kind), config);
  return finalize();
}

void ASTMangler::beginManglingWithAutoDiffOriginalFunction(
    const AbstractFunctionDecl *afd) {
  if (auto *attr = afd->getAttrs().getAttribute<SILGenNameAttr>()) {
    beginManglingWithoutPrefix();
    appendOperator(attr->Name);
    return;
  }

  auto beginManglingClangDecl = [&](const clang::NamedDecl *decl) {
    beginManglingWithoutPrefix();
    appendOperator(decl->getName());
  };

  // For imported Clang declarations, use the Clang name in order to match how
  // DifferentiationMangler handles these.
  if (auto clangDecl = getClangDeclForMangling(afd)) {
    beginManglingClangDecl(clangDecl);
    return;
  } else if (auto typedefType = getTypeDefForCXXCFOptionsDefinition(afd)) {
    beginManglingClangDecl(typedefType->getDecl());
    return;
  }
  beginMangling();
  if (auto *cd = dyn_cast<ConstructorDecl>(afd))
    appendConstructorEntity(cd, /*isAllocating*/ !cd->isConvenienceInit());
  else
    appendEntity(afd);
}

void ASTMangler::appendAutoDiffFunctionParts(StringRef op,
                                             AutoDiffFunctionKind kind,
                                             const AutoDiffConfig &config) {
  if (auto sig = config.derivativeGenericSignature)
    appendGenericSignature(sig);
  auto kindCode = (char)kind;
  appendOperator(op, StringRef(&kindCode, 1));
  appendIndexSubset(config.parameterIndices);
  appendOperator("p");
  appendIndexSubset(config.resultIndices);
  appendOperator("r");
}

std::string ASTMangler::mangleAutoDiffSelfReorderingReabstractionThunk(
    CanType fromType, CanType toType, GenericSignature signature,
    AutoDiffLinearMapKind linearMapKind) {
  beginMangling();
  appendType(fromType, signature);
  appendType(toType, signature);
  if (signature)
    appendGenericSignature(signature);
  auto kindCode = (char)getAutoDiffFunctionKind(linearMapKind);
  appendOperator("TJO", StringRef(&kindCode, 1));
  return finalize();
}

/// Mangle the index subset.
void ASTMangler::appendIndexSubset(IndexSubset *indices) {
  Buffer << indices->getString();
}

static NodePointer mangleSILDifferentiabilityWitnessAsNode(
    StringRef originalName, DifferentiabilityKind kind,
    const AutoDiffConfig &config, Demangler &demangler) {
  auto *diffWitnessNode = demangler.createNode(
      Node::Kind::DifferentiabilityWitness);
  auto origNode = demangler.demangleSymbol(originalName);
  assert(origNode->getKind() == Node::Kind::Global);
  for (auto *child : *origNode)
    diffWitnessNode->addChild(child, demangler);
  diffWitnessNode->addChild(
      demangler.createNode(
          Node::Kind::Index,
          (Node::IndexType)getMangledDifferentiabilityKind(kind)),
      demangler);
  diffWitnessNode->addChild(
      demangler.createNode(
          Node::Kind::IndexSubset, config.parameterIndices->getString()),
      demangler);
  diffWitnessNode->addChild(
      demangler.createNode(
          Node::Kind::IndexSubset, config.resultIndices->getString()),
      demangler);
  if (auto genSig = config.derivativeGenericSignature) {
    ASTMangler genSigMangler;
    auto genSigSymbol = genSigMangler.mangleGenericSignature(genSig);
    auto demangledGenSig = demangler.demangleSymbol(genSigSymbol);
    assert(demangledGenSig);
    for (auto *child : *demangledGenSig)
      diffWitnessNode->addChild(child, demangler);
  }
  return diffWitnessNode;
}

std::string ASTMangler::mangleSILDifferentiabilityWitness(StringRef originalName,
                                              DifferentiabilityKind kind,
                                              const AutoDiffConfig &config) {
  // If the original name was a mangled name, differentiability witnesses must
  // be mangled as node because they contain generic signatures which may repeat
  // entities in the original function name. Mangling as node will make sure the
  // substitutions are mangled correctly.
  if (isMangledName(originalName)) {
    Demangler demangler;
    auto *node = mangleSILDifferentiabilityWitnessAsNode(
        originalName, kind, config, demangler);
    auto mangling = mangleNode(node);
    if (!mangling.isSuccess()) {
      llvm_unreachable("unexpected mangling failure");
    }
    return mangling.result();
  }
  beginManglingWithoutPrefix();
  appendOperator(originalName);
  if (auto genSig = config.derivativeGenericSignature)
    appendGenericSignature(genSig);
  auto diffKindCode = (char)getMangledDifferentiabilityKind(kind);
  appendOperator("WJ", StringRef(&diffKindCode, 1));
  appendIndexSubset(config.parameterIndices);
  appendOperator("p");
  appendIndexSubset(config.resultIndices);
  appendOperator("r");
  return finalize();
}

std::string ASTMangler::mangleAutoDiffGeneratedDeclaration(
    AutoDiffGeneratedDeclarationKind declKind, StringRef origFnName,
    unsigned bbId, AutoDiffLinearMapKind linearMapKind,
    const AutoDiffConfig &config) {
  beginManglingWithoutPrefix();

  Buffer << "_AD__" << origFnName << "_bb" + std::to_string(bbId);
  switch (declKind) {
  case AutoDiffGeneratedDeclarationKind::LinearMapStruct:
    switch (linearMapKind) {
    case AutoDiffLinearMapKind::Differential:
      Buffer << "__DF__";
      break;
    case AutoDiffLinearMapKind::Pullback:
      Buffer << "__PB__";
      break;
    }
    break;
  case AutoDiffGeneratedDeclarationKind::BranchingTraceEnum:
    switch (linearMapKind) {
    case AutoDiffLinearMapKind::Differential:
      Buffer << "__Succ__";
      break;
    case AutoDiffLinearMapKind::Pullback:
      Buffer << "__Pred__";
      break;
    }
    break;
  }
  Buffer << config.mangle();
  if (config.derivativeGenericSignature) {
    Buffer << '_';
    appendGenericSignature(config.derivativeGenericSignature);
  }

  auto result = Storage.str().str();
  Storage.clear();
  return result;
}

// In order for the remangler to work correctly, it must agree with
// AST mangler on the substitution scheme. The AST mangler will use a
// substitution if a mangled type is identical to a previous type.
//
// In the DWARF mangling, we don't canonicalize types. Therefore, any
// two types that differ by sugar must have distinct manglings. If this
// invariant is not maintained, then demangling and remangling a type
// will no longer be idempotent.
//
// Since we don't have a distinct mangling for sugared generic
// parameter types, we must desugar them here.
static Type getTypeForDWARFMangling(Type t) {
  return t.subst(
    [](SubstitutableType *t) -> Type {
      if (t->isRootParameterPack()) {
        return PackType::getSingletonPackExpansion(t->getCanonicalType());
      }
      return t->getCanonicalType();
    },
    MakeAbstractConformanceForGenericType(),
    SubstFlags::AllowLoweredTypes);
}

std::string ASTMangler::mangleTypeForDebugger(Type Ty, GenericSignature sig) {
  PrettyStackTraceType prettyStackTrace(Ty->getASTContext(),
                                        "mangling type for debugger", Ty);

  DWARFMangling = true;
  RespectOriginallyDefinedIn = false;
  OptimizeProtocolNames = false;
  beginMangling();

  Ty = getTypeForDWARFMangling(Ty);

  appendType(Ty, sig);
  appendOperator("D");
  return finalize();
}

std::string ASTMangler::mangleTypeForTypeName(Type type) {
  beginManglingWithoutPrefix();
  appendType(type, nullptr);
  return finalize();
}

std::string ASTMangler::mangleDeclType(const ValueDecl *decl) {
  DWARFMangling = true;
  RespectOriginallyDefinedIn = false;
  BaseEntitySignature base(decl);
  beginMangling();
  appendDeclType(decl, base);
  appendOperator("D");
  return finalize();
}

#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
static bool isPrivate(const NominalTypeDecl *Nominal) {
  return Nominal->getFormalAccess() <= AccessLevel::FilePrivate;
}
#endif

std::string ASTMangler::mangleObjCRuntimeName(const NominalTypeDecl *Nominal) {
#ifdef USE_NEW_MANGLING_FOR_OBJC_RUNTIME_NAMES
  // Using the new mangling for ObjC runtime names (except for top-level
  // classes). This is currently disabled to support old archives.
  // TODO: re-enable this as we switch to the new mangling for ObjC names.
  DeclContext *Ctx = Nominal->getDeclContext();

  if (Ctx->isModuleScopeContext() && !isPrivate(Nominal)) {
    // Use the old mangling for non-private top-level classes and protocols.
    // This is what the ObjC runtime needs to demangle.
    // TODO: Use new mangling scheme as soon as the ObjC runtime
    // can demangle it.
    //
    // Don't use word-substitutions and punycode encoding.
    MaxNumWords = 0;
    UsePunycode = false;
    UseSubstitutions = false;
    Buffer << "_Tt";
    bool isProto = false;
    if (isa<ClassDecl>(Nominal)) {
      Buffer << 'C';
    } else {
      isProto = true;
      assert(isa<ProtocolDecl>(Nominal));
      Buffer << 'P';
    }
    appendModule(Ctx->getParentModule(), StringRef());
    appendIdentifier(Nominal->getName().str());
    if (isProto)
      Buffer << '_';
    return finalize();
  }
  // For all other cases, we can use the new mangling.
  beginMangling();
  appendAnyGenericType(Nominal);
  return finalize();
#else
  // Use the old mangling for ObjC runtime names.
  beginMangling();
  appendAnyGenericType(Nominal);
  std::string NewName = finalize();
  Demangle::Demangler Dem;
  Demangle::Node *Root = Dem.demangleSymbol(NewName);
  assert(Root->getKind() == Node::Kind::Global);
  Node *NomTy = Root->getFirstChild();
  if (NomTy->getKind() == Node::Kind::Protocol) {
    // Protocol types are mangled as protocol lists.
    Node *PTy = Dem.createNode(Node::Kind::Type);
    PTy->addChild(NomTy, Dem);
    Node *TList = Dem.createNode(Node::Kind::TypeList);
    TList->addChild(PTy, Dem);
    NomTy = Dem.createNode(Node::Kind::ProtocolList);
    NomTy->addChild(TList, Dem);
  }
  // Add a TypeMangling node at the top
  Node *Ty = Dem.createNode(Node::Kind::Type);
  Ty->addChild(NomTy, Dem);
  Node *TyMangling = Dem.createNode(Node::Kind::TypeMangling);
  TyMangling->addChild(Ty, Dem);
  Node *NewGlobal = Dem.createNode(Node::Kind::Global);
  NewGlobal->addChild(TyMangling, Dem);
  auto mangling = mangleNodeOld(NewGlobal);
  if (!mangling.isSuccess()) {
    llvm_unreachable("unexpected mangling failure");
  }
  return mangling.result();
#endif
}

std::string ASTMangler::mangleTypeAsContextUSR(const NominalTypeDecl *type) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  BaseEntitySignature base(type);
  appendContext(type, base, type->getAlternateModuleName());
  return finalize();
}

std::string ASTMangler::mangleTypeAsUSR(Type Ty) {
  DWARFMangling = true;
  RespectOriginallyDefinedIn = false;
  beginMangling();

  Ty = getTypeForDWARFMangling(Ty);

  if (auto *fnType = Ty->getAs<AnyFunctionType>()) {
    appendFunction(fnType, nullptr);
  } else {
    appendType(Ty, nullptr);
  }

  appendOperator("D");
  return finalize();
}

void ASTMangler::appendAnyDecl(const ValueDecl *Decl) {
  if (auto Ctor = dyn_cast<ConstructorDecl>(Decl)) {
    appendConstructorEntity(Ctor, /*isAllocating=*/false);
  } else if (auto Dtor = dyn_cast<DestructorDecl>(Decl)) {
    appendDestructorEntity(Dtor, /*isDeallocating=*/false);
  } else if (auto GTD = dyn_cast<GenericTypeDecl>(Decl)) {
    appendAnyGenericType(GTD);
  } else if (isa<AssociatedTypeDecl>(Decl)) {
    BaseEntitySignature base(Decl);
    appendContextOf(Decl, base);
    appendDeclName(Decl);
    appendOperator("Qa");
  } else {
    appendEntity(Decl);
  }
}

std::string
ASTMangler::mangleAnyDecl(const ValueDecl *Decl,
                          bool prefix,
                          bool respectOriginallyDefinedIn) {
  DWARFMangling = true;
  RespectOriginallyDefinedIn = respectOriginallyDefinedIn;
  if (prefix) {
    beginMangling();
  } else {
    beginManglingWithoutPrefix();
  }
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);

  appendAnyDecl(Decl);

  // We have a custom prefix, so finalize() won't verify for us. If we're not
  // in invalid code (coming from an IDE caller) verify manually.
  if (!Decl->isInvalid())
    verify(Storage.str());
  return finalize();
}

std::string ASTMangler::mangleDeclAsUSR(const ValueDecl *Decl,
                                        StringRef USRPrefix) {
  return (llvm::Twine(USRPrefix) + mangleAnyDecl(Decl, false)).str();
}

std::string ASTMangler::mangleAccessorEntityAsUSR(AccessorKind kind,
                                                  const AbstractStorageDecl *decl,
                                                  StringRef USRPrefix,
                                                  bool isStatic) {
  beginManglingWithoutPrefix();
  llvm::SaveAndRestore<bool> allowUnnamedRAII(AllowNamelessEntities, true);
  Buffer << USRPrefix;
  appendAccessorEntity(getCodeForAccessorKind(kind), decl, isStatic);
  // We have a custom prefix, so finalize() won't verify for us. If we're not
  // in invalid code (coming from an IDE caller) verify manually.
  if (!decl->isInvalid())
    verify(Storage.str().drop_front(USRPrefix.size()));
  return finalize();
}

std::string ASTMangler::mangleLocalTypeDecl(const TypeDecl *type) {
  beginManglingWithoutPrefix();
  AllowNamelessEntities = true;
  OptimizeProtocolNames = false;

  // Local types are not ABI anyway. To avoid problems with the ASTDemangler,
  // don't respect @_originallyDefinedIn here, since we don't respect it
  // when mangling DWARF types for debug info.
  RespectOriginallyDefinedIn = false;

  if (auto GTD = dyn_cast<GenericTypeDecl>(type)) {
    appendAnyGenericType(GTD);
  } else {
    assert(isa<AssociatedTypeDecl>(type));
    BaseEntitySignature nullBase(nullptr);
    appendContextOf(type, nullBase);
    appendDeclName(type);
    appendOperator("Qa");
  }

  return finalize();
}

std::string ASTMangler::mangleOpaqueTypeDecl(const OpaqueTypeDecl *decl) {
  return mangleOpaqueTypeDecl(decl->getNamingDecl());
}

std::string ASTMangler::mangleOpaqueTypeDecl(const ValueDecl *decl) {
  OptimizeProtocolNames = false;

  beginMangling();
  appendEntity(decl);
  return finalize();
}

std::string ASTMangler::mangleGenericSignature(const GenericSignature sig) {
  beginMangling();
  appendGenericSignature(sig);
  return finalize();
}

std::string ASTMangler::mangleHasSymbolQuery(const ValueDecl *Decl) {
  beginMangling();

  if (auto Ctor = dyn_cast<ConstructorDecl>(Decl)) {
    appendConstructorEntity(Ctor, /*isAllocating=*/false);
  } else if (auto Dtor = dyn_cast<DestructorDecl>(Decl)) {
    appendDestructorEntity(Dtor, /*isDeallocating=*/false);
  } else if (auto GTD = dyn_cast<GenericTypeDecl>(Decl)) {
    appendAnyGenericType(GTD);
  } else if (isa<AssociatedTypeDecl>(Decl)) {
    BaseEntitySignature nullBase(nullptr);
    appendContextOf(Decl, nullBase);
    appendDeclName(Decl);
    appendOperator("Qa");
  } else {
    appendEntity(Decl);
  }

  appendSymbolKind(ASTMangler::SymbolKind::HasSymbolQuery);

  return finalize();
}

void ASTMangler::appendSymbolKind(SymbolKind SKind) {
  switch (SKind) {
    case SymbolKind::Default: return;
    case SymbolKind::DynamicThunk: return appendOperator("TD");
    case SymbolKind::SwiftAsObjCThunk: return appendOperator("To");
    case SymbolKind::ObjCAsSwiftThunk: return appendOperator("TO");
    case SymbolKind::DistributedThunk: return appendOperator("TE");
    case SymbolKind::DistributedAccessor: return appendOperator("TF");
    case SymbolKind::AccessibleFunctionRecord: return appendOperator("HF");
    case SymbolKind::BackDeploymentThunk: return appendOperator("Twb");
    case SymbolKind::BackDeploymentFallback: return appendOperator("TwB");
    case SymbolKind::HasSymbolQuery: return appendOperator("TwS");
  }
}

static bool getUnnamedParamIndex(const ParameterList *ParamList,
                                 const ParamDecl *D,
                                 unsigned &UnnamedIndex) {
  for (auto Param : *ParamList) {
    if (!Param->hasName()) {
      if (Param == D)
        return true;
      ++UnnamedIndex;
    }
  }
  return false;
}

static unsigned getUnnamedParamIndex(const ParamDecl *D) {
  ParameterList *ParamList;
  auto *DC = D->getDeclContext();
  if (isa<AbstractClosureExpr>(DC)) {
    ParamList = cast<AbstractClosureExpr>(DC)->getParameters();
  } else {
    ParamList = getParameterList(cast<ValueDecl>(DC->getAsDecl()));
  }

  unsigned UnnamedIndex = 0;
  if (getUnnamedParamIndex(ParamList, D, UnnamedIndex))
    return UnnamedIndex;

  llvm_unreachable("param not found");
}

static StringRef getPrivateDiscriminatorIfNecessary(const Decl *decl) {
  if (!decl->isOutermostPrivateOrFilePrivateScope())
    return StringRef();

  // Mangle non-local private declarations with a textual discriminator
  // based on their enclosing file.
  auto topLevelSubcontext = decl->getDeclContext()->getModuleScopeContext();
  auto fileUnit = cast<FileUnit>(topLevelSubcontext);

  Identifier discriminator =
      fileUnit->getDiscriminatorForPrivateDecl(decl);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  (void)&isNonAscii;
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");
  return discriminator.str();
}

/// If the declaration is an @objc protocol defined in Swift and the
/// Objective-C name has been overridden from the default, return the
/// specified name.
///
/// \param useObjCProtocolNames When false, always returns \c None.
static std::optional<std::string>
getOverriddenSwiftProtocolObjCName(const ValueDecl *decl,
                                   bool useObjCProtocolNames) {
  if (!useObjCProtocolNames)
    return std::nullopt;

  auto proto = dyn_cast<ProtocolDecl>(decl);
  if (!proto)
    return std::nullopt;

  if (!proto->isObjC())
    return std::nullopt;

  // If there is an 'objc' attribute with a name, use that name.
  if (auto objc = proto->getAttrs().getAttribute<ObjCAttr>()) {
    if (auto name = objc->getName()) {
      llvm::SmallString<4> buffer;
      return std::string(name->getString(buffer));
    }
  }

  return std::nullopt;
}

void ASTMangler::appendDeclName(const ValueDecl *decl, DeclBaseName name) {
  if (name.empty())
    name = decl->getBaseName();
  assert(!name.isSpecial() && "Cannot print special names");

  auto *synthesizedTypeAttr =
      decl->getAttrs().getAttribute<ClangImporterSynthesizedTypeAttr>();

  if (synthesizedTypeAttr) {
    assert(!isDigit(synthesizedTypeAttr->originalTypeName[0]) &&
           "synthesized type's original name must be a valid Swift identifier");
    appendIdentifier(synthesizedTypeAttr->originalTypeName);
  } else if (name.isOperator()) {
    appendIdentifier(translateOperator(name.getIdentifier().str()));
    switch (decl->getAttrs().getUnaryOperatorKind()) {
      case UnaryOperatorKind::Prefix:
        appendOperator("op");
        break;
      case UnaryOperatorKind::Postfix:
        appendOperator("oP");
        break;
      case UnaryOperatorKind::None:
        appendOperator("oi");
        break;
    }
  } else if (auto objCName =
               getOverriddenSwiftProtocolObjCName(decl, UseObjCRuntimeNames)) {
    // @objc Swift protocols should be mangled as Objective-C protocols,
    // so append the Objective-C runtime name.
    appendIdentifier(*objCName);
  } else if (!name.empty()) {
    appendIdentifier(name.getIdentifier().str());
  } else {
    assert(AllowNamelessEntities && "attempt to mangle unnamed decl");
    // Fall back to an unlikely name, so that we still generate a valid
    // mangled name.
    appendIdentifier("_");
  }

  if (decl->getDeclContext()->isLocalContext()) {
    if (auto *paramDecl = dyn_cast<ParamDecl>(decl)) {
      if (!decl->hasName()) {
        // Mangle unnamed params with their ordering.
        return appendOperator("L", Index(getUnnamedParamIndex(paramDecl)));
      }
    }
    // Mangle local declarations with a numeric discriminator.
    return appendOperator("L", Index(decl->getLocalDiscriminator()));
  }

  if (synthesizedTypeAttr) {
    StringRef relatedEntityKind = synthesizedTypeAttr->getManglingName();
    assert(relatedEntityKind.size() == 1 &&
           "'L' operator only supports a single letter payload");
    assert(((relatedEntityKind[0] >= 'a' && relatedEntityKind[0] <= 'j') ||
            (relatedEntityKind[0] >= 'A' && relatedEntityKind[0] <= 'J')) &&
           "Only [a-jA-J] are reserved for related entity kinds");
    return appendOperatorParam("L", relatedEntityKind);
  }

  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(decl);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator.str());
    return appendOperator("LL");
  }
}

static const char *getMetatypeRepresentationOp(MetatypeRepresentation Rep) {
  switch (Rep) {
    case MetatypeRepresentation::Thin:
      return "t";
    case MetatypeRepresentation::Thick:
      return "T";
    case MetatypeRepresentation::ObjC:
      return "o";
  }

  llvm_unreachable("Unhandled MetatypeRepresentation in switch.");
}

static char getParamConvention(ParameterConvention conv) {
  // @in and @out are mangled the same because they're put in
  // different places.
  switch (conv) {
    case ParameterConvention::Indirect_In: return 'i';
    case ParameterConvention::Indirect_Inout: return 'l';
    case ParameterConvention::Indirect_InoutAliasable: return 'b';
    case ParameterConvention::Indirect_In_Guaranteed: return 'n';
    case ParameterConvention::Indirect_In_CXX: return 'X';
    case ParameterConvention::Direct_Owned: return 'x';
    case ParameterConvention::Direct_Unowned: return 'y';
    case ParameterConvention::Direct_Guaranteed: return 'g';
    case ParameterConvention::Pack_Owned: return 'v';
    case ParameterConvention::Pack_Inout: return 'm';
    case ParameterConvention::Pack_Guaranteed: return 'p';
  }
  llvm_unreachable("bad parameter convention");
}

/// Whether to mangle the given type as generic.
static bool shouldMangleAsGeneric(Type type) {
  if (!type)
    return false;

  if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
    return !typeAlias->getSubstitutionMap().empty();

  return type->isSpecialized();
}

void ASTMangler::appendOpaqueDeclName(const OpaqueTypeDecl *opaqueDecl) {
  if (canSymbolicReference(opaqueDecl)) {
    appendSymbolicReference(opaqueDecl);
  } else if (auto namingDecl = opaqueDecl->getNamingDecl()) {
    // Set this to true temporarily, even if we're doing DWARF
    // mangling for debug info, where it is false. Otherwise,
    // the mangled opaque result type name will not be able to
    // be looked up, since we rely on an exact match with the
    // ABI name.
    llvm::SaveAndRestore<bool> savedRespectOriginallyDefinedIn(
        RespectOriginallyDefinedIn, true);

    appendEntity(namingDecl);
    appendOperator("QO");
  } else {
    llvm_unreachable("todo: independent opaque type decls");
  }
}

void ASTMangler::appendExistentialLayout(
    const ExistentialLayout &layout, GenericSignature sig,
    const ValueDecl *forDecl) {
  bool First = true;
  bool DroppedRequiresClass = false;
  bool SawRequiresClass = false;
  for (auto proto : layout.getProtocols()) {
    // Skip requirements to conform to an invertible protocols.
    // We only mangle inverse requirements, but as a constrained existential.
    if (proto->getInvertibleProtocolKind())
      continue;

    // If we aren't allowed to emit marker protocols, suppress them here.
    if (!AllowMarkerProtocols && proto->isMarkerProtocol()) {
      if (proto->requiresClass())
        DroppedRequiresClass = true;

      continue;
    }

    if (proto->requiresClass())
      SawRequiresClass = true;

    appendProtocolName(proto);
    appendListSeparator(First);
  }
  if (First)
    appendOperator("y");

  if (auto superclass = layout.explicitSuperclass) {
    appendType(superclass, sig, forDecl);
    return appendOperator("Xc");
  } else if (layout.hasExplicitAnyObject ||
             (DroppedRequiresClass && !SawRequiresClass)) {
    return appendOperator("Xl");
  }
  return appendOperator("p");
}

/// Mangle a type into the buffer.
///
void ASTMangler::appendType(Type type, GenericSignature sig,
                            const ValueDecl *forDecl) {
  assert((DWARFMangling || type->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");
  TypeBase *tybase = type.getPointer();
  switch (type->getKind()) {
    case TypeKind::TypeVariable:
      llvm_unreachable("mangling type variable");

    case TypeKind::ErrorUnion:
      llvm_unreachable("Error unions should not persist to mangling");

    case TypeKind::Module:
      llvm_unreachable("Cannot mangle module type yet");

    case TypeKind::Error:
    case TypeKind::Unresolved:
    case TypeKind::Placeholder:
      appendOperator("Xe");
      return;

      // We don't care about these types being a bit verbose because we
      // don't expect them to come up that often in API names.
    case TypeKind::BuiltinFloat:
      switch (cast<BuiltinFloatType>(tybase)->getFPKind()) {
        case BuiltinFloatType::IEEE16: appendOperator("Bf16_"); return;
        case BuiltinFloatType::IEEE32: appendOperator("Bf32_"); return;
        case BuiltinFloatType::IEEE64: appendOperator("Bf64_"); return;
        case BuiltinFloatType::IEEE80: appendOperator("Bf80_"); return;
        case BuiltinFloatType::IEEE128: appendOperator("Bf128_"); return;
        case BuiltinFloatType::PPC128: llvm_unreachable("ppc128 not supported");
      }
      llvm_unreachable("bad floating-point kind");
    case TypeKind::BuiltinInteger: {
      auto width = cast<BuiltinIntegerType>(tybase)->getWidth();
      if (width.isFixedWidth())
        appendOperator("Bi", Index(width.getFixedWidth() + 1));
      else if (width.isPointerWidth())
        appendOperator("Bw");
      else
        llvm_unreachable("impossible width value");
      return;
    }
    case TypeKind::BuiltinIntegerLiteral:
      return appendOperator("BI");
    case TypeKind::BuiltinJob:
      return appendOperator("Bj");
    case TypeKind::BuiltinExecutor:
      return appendOperator("Be");
    case TypeKind::BuiltinDefaultActorStorage:
      return appendOperator("BD");
    case TypeKind::BuiltinNonDefaultDistributedActorStorage:
      return appendOperator("Bd");
    case TypeKind::BuiltinPackIndex:
      return appendOperator("BP");
    case TypeKind::BuiltinRawPointer:
      return appendOperator("Bp");
    case TypeKind::BuiltinRawUnsafeContinuation:
      return appendOperator("Bc");
    case TypeKind::BuiltinNativeObject:
      return appendOperator("Bo");
    case TypeKind::BuiltinBridgeObject:
      return appendOperator("Bb");
    case TypeKind::BuiltinUnsafeValueBuffer:
      return appendOperator("BB");
    case TypeKind::SILToken:
      return appendOperator("Bt");
    case TypeKind::BuiltinVector:
      appendType(cast<BuiltinVectorType>(tybase)->getElementType(), sig,
                 forDecl);
      // The mangling calls for using the actual element count, which we have
      // to adjust by 1 in order to mangle it as an index.
      return appendOperator("Bv",
                  Index(cast<BuiltinVectorType>(tybase)->getNumElements() + 1));
    case TypeKind::TypeAlias: {
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      auto aliasTy = cast<TypeAliasType>(tybase);

      // It's not possible to mangle the context of the builtin module.
      // For the DWARF output we want to mangle the type alias + context,
      // unless the type alias references a builtin type.
      auto underlyingType = aliasTy->getSinglyDesugaredType();
      TypeAliasDecl *decl = aliasTy->getDecl();
      if (decl->getModuleContext() == decl->getASTContext().TheBuiltinModule) {
        return appendType(underlyingType, sig, forDecl);
      }

      if (decl->getDeclaredInterfaceType()
            .subst(aliasTy->getSubstitutionMap()).getPointer()
            != aliasTy) {
        return appendType(underlyingType, sig, forDecl);
      }

      if (aliasTy->getSubstitutionMap()) {
        // Try to mangle the entire name as a substitution.
        if (tryMangleTypeSubstitution(tybase, sig))
          return;

        appendAnyGenericType(decl);
        bool isFirstArgList = true;
        appendBoundGenericArgs(type, sig, isFirstArgList, forDecl);
        appendRetroactiveConformances(type, sig);
        appendOperator("G");
        addTypeSubstitution(type, sig);
        return;
      }

      return appendAnyGenericType(decl);
    }

    case TypeKind::PackExpansion: {
      auto expansionTy = cast<PackExpansionType>(tybase);
      appendType(expansionTy->getPatternType(), sig, forDecl);
      appendType(expansionTy->getCountType(), sig, forDecl);
      appendOperator("Qp");
      return;
    }

    case TypeKind::PackElement: {
      auto elementType = cast<PackElementType>(tybase);
      appendType(elementType->getPackType(), sig, forDecl);
      // If this ever changes, just mangle level 0 as a plain type parameter.
      assert(elementType->getLevel() > 0);
      appendOperator("Qe", Index(elementType->getLevel() - 1));

      return;
    }

    case TypeKind::Pack: {
      auto packTy = cast<PackType>(tybase);

      if (packTy->getNumElements() == 0)
        appendOperator("y");
      else {
        bool firstField = true;
        for (auto element : packTy->getElementTypes()) {
          appendType(element, sig, forDecl);
          appendListSeparator(firstField);
        }
      }
      appendOperator("QP");
      return;
    }

    case TypeKind::SILPack: {
      auto packTy = cast<SILPackType>(tybase);

      if (packTy->getNumElements() == 0)
        appendOperator("y");
      else {
        bool firstField = true;
        for (auto element : packTy->getElementTypes()) {
          appendType(element, sig, forDecl);
          appendListSeparator(firstField);
        }
      }
      appendOperator("QS");
      Buffer << (packTy->isElementAddress() ? 'i' : 'd');
      return;
    }

    case TypeKind::Paren:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<ParenType>(tybase)->getUnderlyingType(), sig, forDecl);
      appendOperator("XSp");
      return;

    case TypeKind::ArraySlice:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<ArraySliceType>(tybase)->getBaseType(), sig, forDecl);
      appendOperator("XSa");
      return;

    case TypeKind::VariadicSequence:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<VariadicSequenceType>(tybase)->getBaseType(), sig, forDecl);
      appendOperator("XSa");
      return;

    case TypeKind::Optional:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<OptionalType>(tybase)->getBaseType(), sig, forDecl);
      appendOperator("XSq");
      return;

    case TypeKind::Dictionary:
      assert(DWARFMangling && "sugared types are only legal for the debugger");
      appendType(cast<DictionaryType>(tybase)->getKeyType(), sig, forDecl);
      appendType(cast<DictionaryType>(tybase)->getValueType(), sig, forDecl);
      appendOperator("XSD");
      return;

    case TypeKind::ExistentialMetatype: {
      ExistentialMetatypeType *EMT = cast<ExistentialMetatypeType>(tybase);

      // ExtendedExistentialTypeShapes consider existential metatypes to
      // be part of the existential, so if we're symbolically referencing
      // shapes, we need to handle that at this level.
      if (EMT->hasParameterizedExistential()) {
        auto referent = SymbolicReferent::forExtendedExistentialTypeShape(EMT);
        if (canSymbolicReference(referent)) {
          appendSymbolicExtendedExistentialType(referent, EMT, sig, forDecl);
          return;
        }
      }

      if (EMT->getInstanceType()->isExistentialType() &&
          EMT->hasParameterizedExistential())
        appendConstrainedExistential(EMT->getInstanceType(), sig, forDecl);
      else
        appendType(EMT->getInstanceType(), sig, forDecl);

      if (EMT->hasRepresentation()) {
        appendOperator("Xm",
                       getMetatypeRepresentationOp(EMT->getRepresentation()));
      } else {
        appendOperator("Xp");
      }
      return;
    }
    case TypeKind::Metatype: {
      MetatypeType *MT = cast<MetatypeType>(tybase);
      appendType(MT->getInstanceType(), sig, forDecl);
      if (MT->hasRepresentation()) {
        appendOperator("XM",
                       getMetatypeRepresentationOp(MT->getRepresentation()));
      } else {
        appendOperator("m");
      }
      return;
    }
    case TypeKind::LValue:
      llvm_unreachable("@lvalue types should not occur in function interfaces");

    case TypeKind::InOut:
      appendType(cast<InOutType>(tybase)->getObjectType(), sig, forDecl);
      return appendOperator("z");

#define REF_STORAGE(Name, ...) \
    case TypeKind::Name##Storage: \
      appendType(cast<Name##StorageType>(tybase)->getReferentType(), sig, forDecl); \
      return appendOperator(manglingOf(ReferenceOwnership::Name));
#include "swift/AST/ReferenceStorage.def"

    case TypeKind::Tuple:
      appendTypeList(type, sig, forDecl);
      return appendOperator("t");

    case TypeKind::Protocol: {
      return appendExistentialLayout(
          ExistentialLayout(CanProtocolType(cast<ProtocolType>(tybase))),
          sig, forDecl);
    }

    case TypeKind::ProtocolComposition: {
      auto *PCT = cast<ProtocolCompositionType>(tybase);

      if (!AllowMarkerProtocols) {
        auto strippedTy = PCT->withoutMarkerProtocols();
        if (!strippedTy->isEqual(PCT))
          return appendType(strippedTy, sig, forDecl);
      }

      if (PCT->hasParameterizedExistential()
          || (PCT->hasInverse() && AllowInverses))
        return appendConstrainedExistential(PCT, sig, forDecl);

      // We mangle ProtocolType and ProtocolCompositionType using the
      // same production:
      auto layout = PCT->getExistentialLayout();
      return appendExistentialLayout(layout, sig, forDecl);
    }

    case TypeKind::ParameterizedProtocol:
      return appendConstrainedExistential(tybase, sig, forDecl);

    case TypeKind::Existential: {
      auto *ET = cast<ExistentialType>(tybase);
      if (ET->hasParameterizedExistential()) {
        auto referent = SymbolicReferent::forExtendedExistentialTypeShape(ET);
        if (canSymbolicReference(referent)) {
          appendSymbolicExtendedExistentialType(referent, ET, sig, forDecl);
          return;
        }

        return appendConstrainedExistential(ET->getConstraintType(), sig,
                                            forDecl);
      }
      return appendType(ET->getConstraintType(), sig, forDecl);
    }

    case TypeKind::UnboundGeneric:
    case TypeKind::Class:
    case TypeKind::Enum:
    case TypeKind::Struct:
    case TypeKind::BoundGenericClass:
    case TypeKind::BoundGenericEnum:
    case TypeKind::BoundGenericStruct:
    case TypeKind::BuiltinTuple: {
      GenericTypeDecl *Decl;
      if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer()))
        Decl = typeAlias->getDecl();
      else
        Decl = type->getAnyGeneric();
      if (shouldMangleAsGeneric(type)) {
        // Try to mangle the entire name as a substitution.
        if (tryMangleTypeSubstitution(tybase, sig))
          return;

        if (Decl->isStdlibDecl() && Decl->getName().str() == "Optional") {
          auto GenArgs = type->castTo<BoundGenericType>()->getGenericArgs();
          assert(GenArgs.size() == 1);
          appendType(GenArgs[0], sig, forDecl);
          appendOperator("Sg");
        } else {
          appendAnyGenericType(Decl);
          bool isFirstArgList = true;
          appendBoundGenericArgs(type, sig, isFirstArgList, forDecl);
          appendRetroactiveConformances(type, sig);
          appendOperator("G");
        }
        addTypeSubstitution(type, sig);
        return;
      }
      appendAnyGenericType(type->getAnyGeneric());
      return;
    }

    case TypeKind::SILFunction:
      return appendImplFunctionType(cast<SILFunctionType>(tybase), sig,
                                    forDecl);

      // type ::= archetype
    case TypeKind::PrimaryArchetype:
    case TypeKind::PackArchetype:
    case TypeKind::ElementArchetype:
    case TypeKind::OpenedArchetype:
      llvm_unreachable("Cannot mangle free-standing archetypes");

    case TypeKind::OpaqueTypeArchetype: {
      auto opaqueType = cast<OpaqueTypeArchetypeType>(tybase);
      auto opaqueDecl = opaqueType->getDecl();
      return appendOpaqueTypeArchetype(
          opaqueType, opaqueDecl, opaqueType->getSubstitutions(), sig, forDecl);
    }
      
    case TypeKind::DynamicSelf: {
      auto dynamicSelf = cast<DynamicSelfType>(tybase);
      if (dynamicSelf->getSelfType()->getAnyNominal()) {
        appendType(dynamicSelf->getSelfType(), sig, forDecl);
        return appendOperator("XD");
      }
      return appendType(dynamicSelf->getSelfType(), sig, forDecl);
    }

    case TypeKind::GenericFunction: {
      auto genFunc = cast<GenericFunctionType>(tybase);
      appendFunctionType(genFunc, genFunc->getGenericSignature(),
                         /*autoclosure*/ false, forDecl);
      appendGenericSignature(genFunc->getGenericSignature());
      appendOperator("u");
      return;
    }

    case TypeKind::GenericTypeParam: {
      auto paramTy = cast<GenericTypeParamType>(tybase);
      // If this assertion fires, it probably means the type being mangled here
      // didn't go through getTypeForDWARFMangling().
      assert(paramTy->getDecl() == nullptr &&
             "cannot mangle non-canonical generic parameter");
      // A special mangling for the very first generic parameter. This shows up
      // frequently because it corresponds to 'Self' in protocol requirement
      // generic signatures.
      if (paramTy->getDepth() == 0 && paramTy->getIndex() == 0)
        return appendOperator("x");

      return appendOpWithGenericParamIndex("q", paramTy);
    }

    case TypeKind::DependentMember: {
      auto *DepTy = cast<DependentMemberType>(tybase);
      if (tryMangleTypeSubstitution(DepTy, sig))
        return;

      bool isAssocTypeAtDepth = false;
      if (GenericTypeParamType *gpBase = appendAssocType(DepTy, sig,
                                                         isAssocTypeAtDepth)) {
        if (gpBase->getDepth() == 0 && gpBase->getIndex() == 0) {
          appendOperator(isAssocTypeAtDepth ? "QZ" : "Qz");
        } else {
          appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "QY" : "Qy",
                                        gpBase);
        }
      } else {
        // Dependent members of non-generic-param types are not canonical, but
        // we may still want to mangle them for debugging or indexing purposes.
        appendType(DepTy->getBase(), sig, forDecl);
        appendIdentifier(DepTy->getName().str());
        appendOperator("Qa");
      }
      addTypeSubstitution(DepTy, sig);
      return;
    }
      
    case TypeKind::Function:
      appendFunctionType(cast<FunctionType>(tybase), sig,
                         /*autoclosure*/ false,
                         forDecl);
      return;
      
    case TypeKind::SILBox: {
      auto box = cast<SILBoxType>(tybase);
      auto layout = box->getLayout();
      bool firstField = true;
      for (auto &field : layout->getFields()) {
        appendType(field.getLoweredType(), sig, forDecl);
        if (field.isMutable()) {
          // Use the `inout` mangling to represent a mutable field.
          appendOperator("z");
        }
        appendListSeparator(firstField);
      }
      if (firstField)
        appendOperator("y");

      if (auto sig = layout->getGenericSignature()) {
        bool firstType = true;
        for (Type type : box->getSubstitutions().getReplacementTypes()) {
          appendType(type, sig, forDecl);
          appendListSeparator(firstType);
        }
        if (firstType)
          appendOperator("y");

        appendGenericSignature(sig);
        appendOperator("XX");
      } else {
        appendOperator("Xx");
      }

      return;
    }
    case TypeKind::SILMoveOnlyWrapped:
      // If we hit this, we just mangle the underlying name and move on.
      llvm_unreachable("should never be mangled?");
    case TypeKind::SILBlockStorage:
      llvm_unreachable("should never be mangled");
  }
  llvm_unreachable("bad type kind");
}

GenericTypeParamType *ASTMangler::appendAssocType(DependentMemberType *DepTy,
                                                  GenericSignature sig,
                                                  bool &isAssocTypeAtDepth) {
  auto base = DepTy->getBase()->getCanonicalType();
  // 't_0_0.Member'
  if (auto gpBase = dyn_cast<GenericTypeParamType>(base)) {
    appendAssociatedTypeName(DepTy, sig);
    isAssocTypeAtDepth = false;
    return gpBase;
  }

  // 't_0_0.Member.Member...'
  SmallVector<DependentMemberType*, 2> path;
  path.push_back(DepTy);
  while (auto dmBase = dyn_cast<DependentMemberType>(base)) {
    path.push_back(dmBase);
    base = dmBase.getBase();
  }
  if (auto gpRoot = dyn_cast<GenericTypeParamType>(base)) {
    bool first = true;
    for (auto *member : llvm::reverse(path)) {
      appendAssociatedTypeName(member, sig);
      appendListSeparator(first);
    }
    isAssocTypeAtDepth = true;
    return gpRoot;
  }
  return nullptr;
}

void ASTMangler::appendOpWithGenericParamIndex(
    StringRef Op, const GenericTypeParamType *paramTy,
    bool baseIsProtocolSelf) {
  llvm::SmallVector<char, 8> OpBuf(Op.begin(), Op.end());
  if (paramTy->getDepth() > 0) {
    OpBuf.push_back('d');
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()),
                          Index(paramTy->getDepth() - 1),
                          Index(paramTy->getIndex()));
  }
  if (paramTy->getIndex() == 0) {
    if (baseIsProtocolSelf) {
      OpBuf.push_back('s');
    } else {
      OpBuf.push_back('z');
    }
    return appendOperator(StringRef(OpBuf.data(), OpBuf.size()));
  }
  appendOperator(Op, Index(paramTy->getIndex() - 1));
}

void ASTMangler::appendFlatGenericArgs(SubstitutionMap subs,
                                       GenericSignature sig,
                                       const ValueDecl *forDecl) {
  appendOperator("y");

  for (auto replacement : subs.getReplacementTypes()) {
    if (replacement->hasArchetype())
      replacement = replacement->mapTypeOutOfContext();
    appendType(replacement, sig, forDecl);
  }
}

unsigned ASTMangler::appendBoundGenericArgs(DeclContext *dc,
                                            GenericSignature sig,
                                            SubstitutionMap subs,
                                            bool &isFirstArgList,
                                            const ValueDecl *forDecl) {
  auto decl = dc->getInnermostDeclarationDeclContext();
  if (!decl) return 0;

  // For a non-protocol extension declaration, use the nominal type declaration
  // instead.
  //
  // This is important when extending a nested type, because the generic
  // parameters will line up with the (semantic) nesting of the nominal type.
  if (auto ext = dyn_cast<ExtensionDecl>(decl))
    decl = ext->getSelfNominalTypeDecl();

  // Handle the generic arguments of the parent.
  unsigned currentGenericParamIdx =
    appendBoundGenericArgs(decl->getDeclContext(), sig, subs, isFirstArgList,
                           forDecl);

  // If this is potentially a generic context, emit a generic argument list.
  if (auto genericContext = decl->getAsGenericContext()) {
    if (isFirstArgList) {
      appendOperator("y");
      isFirstArgList = false;
    } else {
      appendOperator("_");
    }

    // If we are generic at this level, emit all of the replacements at
    // this level.
    bool treatAsGeneric;
    if (auto opaque = dyn_cast<OpaqueTypeDecl>(decl)) {
      // For opaque type declarations, the generic parameters of the opaque
      // type declaration are not part of the mangling, so check whether the
      // naming declaration has generic parameters.
      auto namedGenericContext = opaque->getNamingDecl()->getAsGenericContext();
      treatAsGeneric = namedGenericContext && namedGenericContext->isGeneric();
    } else {
      treatAsGeneric = genericContext->isGeneric();
    }
    if (treatAsGeneric) {
      auto genericParams = subs.getGenericSignature().getGenericParams();
      unsigned depth = genericParams[currentGenericParamIdx]->getDepth();
      auto replacements = subs.getReplacementTypes();
      for (unsigned lastGenericParamIdx = genericParams.size();
           (currentGenericParamIdx != lastGenericParamIdx &&
            genericParams[currentGenericParamIdx]->getDepth() == depth);
           ++currentGenericParamIdx) {
        Type replacementType = replacements[currentGenericParamIdx];
        if (replacementType->hasArchetype())
          replacementType = replacementType->mapTypeOutOfContext();

        appendType(replacementType, sig, forDecl);
      }
    }
  }

  return currentGenericParamIdx;
}

void ASTMangler::appendBoundGenericArgs(Type type, GenericSignature sig,
                                        bool &isFirstArgList,
                                        const ValueDecl *forDecl) {
  TypeBase *typePtr = type.getPointer();
  ArrayRef<Type> genericArgs;
  if (auto *typeAlias = dyn_cast<TypeAliasType>(typePtr)) {
    appendBoundGenericArgs(typeAlias->getDecl(), sig,
                           typeAlias->getSubstitutionMap(),
                           isFirstArgList, forDecl);
    return;
  }

  if (auto *unboundType = dyn_cast<UnboundGenericType>(typePtr)) {
    if (Type parent = unboundType->getParent())
      appendBoundGenericArgs(parent->getDesugaredType(), sig, isFirstArgList,
                             forDecl);
  } else if (auto *nominalType = dyn_cast<NominalType>(typePtr)) {
    if (Type parent = nominalType->getParent())
      appendBoundGenericArgs(parent->getDesugaredType(), sig, isFirstArgList,
                             forDecl);
  } else {
    auto boundType = cast<BoundGenericType>(typePtr);
    genericArgs = boundType->getGenericArgs();
    if (Type parent = boundType->getParent()) {
      GenericTypeDecl *decl = boundType->getAnyGeneric();
      if (!getSpecialManglingContext(decl, UseObjCRuntimeNames))
        appendBoundGenericArgs(parent->getDesugaredType(), sig, isFirstArgList,
                               forDecl);
    }
  }
  if (isFirstArgList) {
    appendOperator("y");
    isFirstArgList = false;
  } else {
    appendOperator("_");
  }
  for (Type arg : genericArgs) {
    appendType(arg, sig, forDecl);
  }
}

static bool conformanceHasIdentity(const RootProtocolConformance *root) {
  auto conformance = dyn_cast<NormalProtocolConformance>(root);
  if (!conformance) {
    assert(isa<SelfProtocolConformance>(root) ||
           isa<BuiltinProtocolConformance>(root));
    return true;
  }

  // Synthesized conformances can have multiple copies, so they don't
  // provide identity.
  if (conformance->isSynthesized())
    return false;

  // Objective-C protocol conformances are checked by the ObjC runtime.
  if (conformance->getProtocol()->isObjC())
    return false;

  return true;
}

/// Determine whether the given protocol conformance is itself retroactive,
/// meaning that there might be multiple conflicting conformances of the
/// same type to the same protocol.
static bool isRetroactiveConformance(const RootProtocolConformance *root) {
  auto conformance = dyn_cast<NormalProtocolConformance>(root);
  if (!conformance) {
    assert(isa<SelfProtocolConformance>(root) ||
           isa<BuiltinProtocolConformance>(root));
    return false; // self-conformances are never retroactive. nor are builtin.
  }

  // Don't consider marker protocols at all.
  if (conformance->getProtocol()->isMarkerProtocol())
    return false;

  return conformance->isRetroactive();
}

template<typename Fn>
static bool forEachConditionalConformance(const ProtocolConformance *conformance,
                                          Fn fn) {
  auto *rootConformance = conformance->getRootConformance();

  auto subMap = conformance->getSubstitutionMap();
  for (auto requirement : rootConformance->getConditionalRequirements()) {
    if (requirement.getKind() != RequirementKind::Conformance)
      continue;
    ProtocolDecl *proto = requirement.getProtocolDecl();
    auto conformance = subMap.lookupConformance(
        requirement.getFirstType()->getCanonicalType(), proto);
    if (conformance.isInvalid()) {
      // This should only happen when mangling invalid ASTs, but that happens
      // for indexing purposes.
      continue;
    }

    if (fn(requirement.getFirstType().subst(subMap), conformance))
      return true;
  }

  return false;
}

/// Determine whether the given protocol conformance contains a retroactive
/// protocol conformance anywhere in it.
static bool containsRetroactiveConformance(
                                      ProtocolConformanceRef conformanceRef) {
  if (!conformanceRef.isPack() && !conformanceRef.isConcrete())
    return false;

  if (conformanceRef.isPack()) {
    for (auto patternConf : conformanceRef.getPack()->getPatternConformances()) {
      if (containsRetroactiveConformance(patternConf))
        return true;
    }

    return false;
  }

  auto *conformance = conformanceRef.getConcrete();

  // If the root conformance is retroactive, it's retroactive.
  const RootProtocolConformance *rootConformance =
      conformance->getRootConformance();
  if (isRetroactiveConformance(rootConformance) &&
      conformanceHasIdentity(rootConformance))
    return true;

  // If the conformance is conditional and any of the substitutions used to
  // satisfy the conditions are retroactive, it's retroactive.
  return forEachConditionalConformance(conformance,
    [&](Type substType, ProtocolConformanceRef substConf) -> bool {
      return containsRetroactiveConformance(substConf);
    });
}

void ASTMangler::appendRetroactiveConformances(SubstitutionMap subMap,
                                               GenericSignature sig) {
  if (subMap.empty()) return;

  unsigned numProtocolRequirements = 0;
  for (auto conformance : subMap.getConformances()) {
    if (conformance.isInvalid())
      continue;

    if (conformance.getRequirement()->isMarkerProtocol())
      continue;

    SWIFT_DEFER {
      ++numProtocolRequirements;
    };

    // Ignore abstract conformances.
    if (!conformance.isConcrete() && !conformance.isPack())
      continue;

    // Skip non-retroactive conformances.
    if (!containsRetroactiveConformance(conformance))
      continue;

    if (conformance.isConcrete())
      appendConcreteProtocolConformance(conformance.getConcrete(), sig);
    else
      appendPackProtocolConformance(conformance.getPack(), sig);

    appendOperator("g", Index(numProtocolRequirements));
  }
}

void ASTMangler::appendRetroactiveConformances(Type type, GenericSignature sig) {
  // Dig out the substitution map to use.
  SubstitutionMap subMap;
  ModuleDecl *module;
  if (auto typeAlias = dyn_cast<TypeAliasType>(type.getPointer())) {
    module = Mod ? Mod : typeAlias->getDecl()->getModuleContext();
    subMap = typeAlias->getSubstitutionMap();
  } else {
    if (type->hasUnboundGenericType())
      return;

    auto nominal = type->getAnyNominal();
    if (!nominal) return;

    module = Mod ? Mod : nominal->getModuleContext();
    subMap = type->getContextSubstitutionMap(module, nominal);
  }

  appendRetroactiveConformances(subMap, sig);
}

void ASTMangler::appendSymbolicExtendedExistentialType(
                                             SymbolicReferent shapeReferent,
                                             Type type,
                                             GenericSignature sig,
                                             const ValueDecl *forDecl) {
  assert(shapeReferent.getKind() ==
           SymbolicReferent::ExtendedExistentialTypeShape);
  assert(canSymbolicReference(shapeReferent));
  assert(type->isAnyExistentialType());

  // type ::= symbolic-extended-existential-type-shape
  //          type* retroactive-conformance* 'Xj'

  appendSymbolicReference(shapeReferent);

  auto genInfo = ExistentialTypeGeneralization::get(type);
  if (genInfo.Generalization) {
    for (auto argType : genInfo.Generalization.getReplacementTypes())
      appendType(argType, sig, forDecl);

    appendRetroactiveConformances(genInfo.Generalization, sig);
  }

  appendOperator("Xj");
}

static std::optional<char>
getParamDifferentiability(SILParameterInfo::Options options) {
  if (options.contains(SILParameterInfo::NotDifferentiable))
    return 'w';
  return {};
}

static char getResultConvention(ResultConvention conv) {
  switch (conv) {
    case ResultConvention::Indirect: return 'r';
    case ResultConvention::Owned: return 'o';
    case ResultConvention::Unowned: return 'd';
    case ResultConvention::UnownedInnerPointer: return 'u';
    case ResultConvention::Autoreleased: return 'a';
    case ResultConvention::Pack: return 'k';
  }
  llvm_unreachable("bad result convention");
}

static std::optional<char>
getResultDifferentiability(SILResultInfo::Options options) {
  if (options.contains(SILResultInfo::NotDifferentiable))
    return 'w';
  return {};
}

void ASTMangler::appendImplFunctionType(SILFunctionType *fn,
                                        GenericSignature outerGenericSig,
                                        const ValueDecl *forDecl,
                                        bool isInRecursion) {

  llvm::SmallVector<char, 32> OpArgs;

  if (fn->getPatternSubstitutions()) {
    OpArgs.push_back('s');
  }
  if (fn->getInvocationSubstitutions()) {
    OpArgs.push_back('I');
  }
  
  if (fn->isPolymorphic() && fn->isPseudogeneric())
    OpArgs.push_back('P');

  if (!fn->isNoEscape())
    OpArgs.push_back('e');

  switch (fn->getIsolation()) {
  case SILFunctionTypeIsolation::Unknown:
    break;
  case SILFunctionTypeIsolation::Erased:
    OpArgs.push_back('A');
    break;
  }

  // Differentiability kind.
  auto diffKind = fn->getExtInfo().getDifferentiabilityKind();
  if (diffKind != DifferentiabilityKind::NonDifferentiable) {
    OpArgs.push_back((char)getMangledDifferentiabilityKind(diffKind));
  }

  // <impl-callee-convention>
  if (fn->getExtInfo().hasContext()) {
    OpArgs.push_back(getParamConvention(fn->getCalleeConvention()));
  } else {
    OpArgs.push_back('t');
  }

  bool mangleClangType = fn->getASTContext().LangOpts.UseClangFunctionTypes &&
                         fn->hasNonDerivableClangType();

  auto appendClangTypeToVec = [this, fn](auto &Vec) {
    llvm::raw_svector_ostream OpArgsOS(Vec);
    appendClangType(fn, OpArgsOS);
  };

  switch (fn->getRepresentation()) {
    case SILFunctionTypeRepresentation::Thick:
    case SILFunctionTypeRepresentation::Thin:
      break;
    case SILFunctionTypeRepresentation::Block:
      if (!mangleClangType) {
        OpArgs.push_back('B');
        break;
      }
      OpArgs.push_back('z');
      OpArgs.push_back('B');
      appendClangTypeToVec(OpArgs);
      break;
    case SILFunctionTypeRepresentation::CXXMethod:
    case SILFunctionTypeRepresentation::CFunctionPointer:
      if (!mangleClangType) {
        OpArgs.push_back('C');
        break;
      }
      OpArgs.push_back('z');
      OpArgs.push_back('C');
      appendClangTypeToVec(OpArgs);
      break;
    case SILFunctionTypeRepresentation::ObjCMethod:
      OpArgs.push_back('O');
      break;
    case SILFunctionTypeRepresentation::Method:
      OpArgs.push_back('M');
      break;
    case SILFunctionTypeRepresentation::Closure:
      OpArgs.push_back('K');
      break;
    case SILFunctionTypeRepresentation::WitnessMethod:
      OpArgs.push_back('W');
      break;
    case SILFunctionTypeRepresentation::KeyPathAccessorGetter:
    case SILFunctionTypeRepresentation::KeyPathAccessorSetter:
    case SILFunctionTypeRepresentation::KeyPathAccessorEquals:
    case SILFunctionTypeRepresentation::KeyPathAccessorHash:
      // KeyPath accessors are mangled separately based on their index types
      // by mangleKeyPathGetterThunkHelper, and so on.
      llvm_unreachable("key path accessors should not mangle its function type");
  }

  // Coroutine kind.  This is mangled in all pointer auth modes.
  switch (fn->getCoroutineKind()) {
  case SILCoroutineKind::None:
    break;
  case SILCoroutineKind::YieldOnce:
    OpArgs.push_back('A');
    break;
  case SILCoroutineKind::YieldMany:
    OpArgs.push_back('G');
    break;
  }

  // Concurrent functions.
  if (fn->isSendable()) {
    OpArgs.push_back('h');
  }

  // Asynchronous functions.
  if (fn->isAsync()) {
    OpArgs.push_back('H');
  }

  GenericSignature sig = fn->getSubstGenericSignature();
  
  // Mangle the parameters.
  for (auto param : fn->getParameters()) {
    OpArgs.push_back(getParamConvention(param.getConvention()));
    if (param.hasOption(SILParameterInfo::Sending))
      OpArgs.push_back('T');
    if (auto diffKind = getParamDifferentiability(param.getOptions()))
      OpArgs.push_back(*diffKind);
    appendType(param.getInterfaceType(), sig, forDecl);
  }

  // Mangle if we have a transferring result.
  if (isInRecursion && fn->hasSendingResult())
    OpArgs.push_back('T');

  // Mangle the results.
  for (auto result : fn->getResults()) {
    OpArgs.push_back(getResultConvention(result.getConvention()));
    if (auto diffKind = getResultDifferentiability(result.getOptions()))
      OpArgs.push_back(*diffKind);
    appendType(result.getInterfaceType(), sig, forDecl);
  }

  // Mangle the yields.
  for (auto yield : fn->getYields()) {
    OpArgs.push_back('Y');
    OpArgs.push_back(getParamConvention(yield.getConvention()));
    appendType(yield.getInterfaceType(), sig, forDecl);
  }

  // Mangle the error result if present.
  if (fn->hasErrorResult()) {
    auto error = fn->getErrorResult();
    OpArgs.push_back('z');
    OpArgs.push_back(getResultConvention(error.getConvention()));
    appendType(error.getInterfaceType(), sig, forDecl);
  }

  if (auto invocationSig = fn->getInvocationGenericSignature()) {
    appendGenericSignature(invocationSig);
    sig = outerGenericSig;
  }
  if (auto subs = fn->getInvocationSubstitutions()) {
    appendFlatGenericArgs(subs, sig, forDecl);
    appendRetroactiveConformances(subs, sig);
  }
  if (auto subs = fn->getPatternSubstitutions()) {
    appendGenericSignature(subs.getGenericSignature());
    sig =
      fn->getInvocationGenericSignature()
        ? fn->getInvocationGenericSignature()
        : outerGenericSig;
    appendFlatGenericArgs(subs, sig, forDecl);
    appendRetroactiveConformances(subs, sig);
  }

  OpArgs.push_back('_');

  appendOperator("I", StringRef(OpArgs.data(), OpArgs.size()));
}

void ASTMangler::appendOpaqueTypeArchetype(ArchetypeType *archetype,
                                           OpaqueTypeDecl *opaqueDecl,
                                           SubstitutionMap subs,
                                           GenericSignature sig,
                                           const ValueDecl *forDecl) {
  Type interfaceType = archetype->getInterfaceType();
  auto genericParam = interfaceType->getAs<GenericTypeParamType>();

  // If this is the opaque return type of the declaration currently being
  // mangled, use a short mangling to represent it.
  if (genericParam && opaqueDecl->getNamingDecl() == forDecl) {
    assert(subs.isIdentity());
    if (genericParam->getIndex() == 0)
      return appendOperator("Qr");

    return appendOperator("QR", Index(genericParam->getIndex() - 1));
  }

  // Otherwise, try to substitute it.
  if (tryMangleTypeSubstitution(Type(archetype), sig))
    return;

  // Mangling at the root, described by a generic parameter.
  if (genericParam) {
    // Use the fully elaborated explicit mangling.
    appendOpaqueDeclName(opaqueDecl);
    bool isFirstArgList = true;
    appendBoundGenericArgs(opaqueDecl, sig, subs, isFirstArgList, forDecl);
    appendRetroactiveConformances(subs, sig);

    appendOperator("Qo", Index(genericParam->getIndex()));
  } else {
    // Mangle associated types of opaque archetypes like dependent member
    // types, so that they can be accurately demangled at runtime.
    appendType(Type(archetype->getRoot()), sig, forDecl);
    bool isAssocTypeAtDepth = false;
    appendAssocType(
        archetype->getInterfaceType()->castTo<DependentMemberType>(),
        sig, isAssocTypeAtDepth);
    appendOperator(isAssocTypeAtDepth ? "QX" : "Qx");
  }

  addTypeSubstitution(Type(archetype), sig);
}

std::optional<ASTMangler::SpecialContext>
ASTMangler::getSpecialManglingContext(const ValueDecl *decl,
                                      bool useObjCProtocolNames) {
  // Declarations provided by a C module have a special context mangling.
  //   known-context ::= 'So'
  //
  // Also handle top-level imported declarations that don't have corresponding
  // Clang decls. Check getKind() directly to avoid a layering dependency.
  //   known-context ::= 'SC'
  if (auto file = dyn_cast<FileUnit>(decl->getDeclContext())) {
    if (file->getKind() == FileUnitKind::ClangModule ||
        file->getKind() == FileUnitKind::DWARFModule) {
      if (decl->getClangDecl())
        return ASTMangler::ObjCContext;
      return ASTMangler::ClangImporterContext;
    }
  }

  // If @objc Swift protocols should be mangled as Objective-C protocols,
  // they are defined in the Objective-C context.
  if (getOverriddenSwiftProtocolObjCName(decl, useObjCProtocolNames))
    return ASTMangler::ObjCContext;

  // Nested types imported from C should also get use the special "So" context.
  if (isa<TypeDecl>(decl)) {
    if (auto *clangDecl = cast_or_null<clang::NamedDecl>(decl->getClangDecl())){
      bool hasNameForLinkage;
      if (auto *tagDecl = dyn_cast<clang::TagDecl>(clangDecl))
        // Clang does not always populate the fields that determine if a tag
        // decl has a linkage name. This is particularly the case for the
        // C++ definition of CF_OPTIONS in the sdk. However, we use the
        // name of the backing typedef as a linkage name, despite
        // the enum itself not having one.
        hasNameForLinkage =
            tagDecl->hasNameForLinkage() || isCXXCFOptionsDefinition(decl);
      else
        hasNameForLinkage = !clangDecl->getDeclName().isEmpty();
      if (hasNameForLinkage) {
        auto *clangDC = clangDecl->getDeclContext();
        // In C, "nested" structs, unions, enums, etc. will become siblings:
        //   struct Foo { struct Bar { }; }; -> struct Foo { }; struct Bar { };
        // Whereas in C++, nested records will actually be nested. So if this is
        // a C++ record, simply treat it like a namespace and exit early.
        if (isa<clang::NamespaceDecl>(clangDC) ||
            isa<clang::CXXRecordDecl>(clangDC))
          return std::nullopt;
        assert(clangDC->getRedeclContext()->isTranslationUnit() &&
               "non-top-level Clang types not supported yet");
        return ASTMangler::ObjCContext;
      }
    }

    // Types apparently defined in the Builtin module are actually
    // synthetic declarations for types defined in the runtime,
    // and they should be mangled as C-namespace entities; see e.g.
    // IRGenModule::getObjCRuntimeBaseClass.
    if (decl->getModuleContext()->isBuiltinModule())
      return ASTMangler::ObjCContext;
  }

  // Importer-synthesized types should always be mangled in the
  // ClangImporterContext, even if an __attribute__((swift_name())) nests them
  // inside a Swift type syntactically.
  if (decl->getAttrs().hasAttribute<ClangImporterSynthesizedTypeAttr>())
    return ASTMangler::ClangImporterContext;

  return std::nullopt;
}

/// Mangle the context of the given declaration as a <context.
/// This is the top-level entrypoint for mangling <context>.
void ASTMangler::appendContextOf(const ValueDecl *decl,
                                 BaseEntitySignature &base) {
  // Check for a special mangling context.
  if (auto context = getSpecialManglingContext(decl, UseObjCRuntimeNames)) {
    switch (*context) {
    case ClangImporterContext:
      return appendOperator("SC");
    case ObjCContext:
      return appendOperator("So");
    }
  }

  // Mangle the decl's DC.
  appendContext(decl->getDeclContext(), base, decl->getAlternateModuleName());
}

namespace {
  class FindFirstVariable :
    public PatternVisitor<FindFirstVariable, VarDecl *> {
  public:
    VarDecl *visitNamedPattern(NamedPattern *P) {
      return P->getDecl();
    }

    VarDecl *visitTuplePattern(TuplePattern *P) {
      for (auto &elt : P->getElements()) {
        VarDecl *var = visit(elt.getPattern());
        if (var) return var;
      }
      return nullptr;
    }

    VarDecl *visitParenPattern(ParenPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitBindingPattern(BindingPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitTypedPattern(TypedPattern *P) {
      return visit(P->getSubPattern());
    }
    VarDecl *visitAnyPattern(AnyPattern *P) {
      return nullptr;
    }

    // Refutable patterns shouldn't ever come up.
#define REFUTABLE_PATTERN(ID, BASE)                                        \
    VarDecl *visit##ID##Pattern(ID##Pattern *P) {                          \
      llvm_unreachable("shouldn't be visiting a refutable pattern here!"); \
    }
#define PATTERN(ID, BASE)
#include "swift/AST/PatternNodes.def"
  };
} // end anonymous namespace

/// Find the first identifier bound by the given binding.  This
/// assumes that field and global-variable bindings always bind at
/// least one name, which is probably a reasonable assumption but may
/// not be adequately enforced.
static std::optional<VarDecl *> findFirstVariable(PatternBindingDecl *binding) {
  for (auto idx : range(binding->getNumPatternEntries())) {
    auto var = FindFirstVariable().visit(binding->getPattern(idx));
    if (var)
      return var;
  }
  // Pattern-binding bound without variables exists in erroneous code, e.g.
  // during code completion.
  return std::nullopt;
}

void ASTMangler::appendContext(const DeclContext *ctx,
                               BaseEntitySignature &base,
                               StringRef useModuleName) {
  switch (ctx->getContextKind()) {
  case DeclContextKind::Package:
    return;
  case DeclContextKind::Module:
    return appendModule(cast<ModuleDecl>(ctx), useModuleName);

  case DeclContextKind::FileUnit:
    assert(!isa<BuiltinUnit>(ctx) && "mangling member of builtin module!");
    appendContext(ctx->getParent(), base, useModuleName);
    return;

  case DeclContextKind::GenericTypeDecl: {
    auto gtd = cast<GenericTypeDecl>(ctx);
    bool innermost = base.reachedInnermostTypeDecl();
    appendAnyGenericType(gtd, base);
    if (innermost)
      appendContextualInverses(gtd, base, ctx->getParentModule(), useModuleName);
    return;
  }

  case DeclContextKind::ExtensionDecl:
    appendExtension(cast<ExtensionDecl>(ctx), base, useModuleName);
    return;

  case DeclContextKind::AbstractClosureExpr:
    return appendClosureEntity(cast<AbstractClosureExpr>(ctx));

  case DeclContextKind::SerializedAbstractClosure:
    return appendClosureEntity(cast<SerializedAbstractClosureExpr>(ctx));

  case DeclContextKind::AbstractFunctionDecl: {
    auto fn = cast<AbstractFunctionDecl>(ctx);
    // Constructors and destructors as contexts are always mangled
    // using the non-(de)allocating variants.
    if (auto ctor = dyn_cast<ConstructorDecl>(fn)) {
      return appendConstructorEntity(ctor, /*allocating*/ false);
    }
    
    if (auto dtor = dyn_cast<DestructorDecl>(fn))
      return appendDestructorEntity(dtor, /*deallocating*/ false);
    
    return appendEntity(fn);
  }

  case DeclContextKind::EnumElementDecl: {
    auto eed = cast<EnumElementDecl>(ctx);
    return appendEntity(eed);
  }

  case DeclContextKind::SubscriptDecl: {
    auto sd = cast<SubscriptDecl>(ctx);
    return appendEntity(sd);
  }
      
  case DeclContextKind::Initializer: {
    switch (cast<Initializer>(ctx)->getInitializerKind()) {
    case InitializerKind::DefaultArgument: {
      auto argInit = cast<DefaultArgumentInitializer>(ctx);
      return appendDefaultArgumentEntity(ctx->getParent(), argInit->getIndex());
    }

    case InitializerKind::PatternBinding: {
      auto patternInit = cast<PatternBindingInitializer>(ctx);
      if (auto var = findFirstVariable(patternInit->getBinding())) {
        appendInitializerEntity(var.value());
      } else {
        BaseEntitySignature nullBase(nullptr);
        // This is incorrect in that it does not produce a /unique/ mangling,
        // but it will at least produce a /valid/ mangling.
        appendContext(ctx->getParent(), nullBase, useModuleName);
      }
      return;
    }

    case InitializerKind::PropertyWrapper: {
      auto wrapperInit = cast<PropertyWrapperInitializer>(ctx);
      switch (wrapperInit->getKind()) {
      case PropertyWrapperInitializer::Kind::WrappedValue:
        appendBackingInitializerEntity(wrapperInit->getWrappedVar());
        break;
      case PropertyWrapperInitializer::Kind::ProjectedValue:
        appendInitFromProjectedValueEntity(wrapperInit->getWrappedVar());
        break;
      }
      return;
    }
    }
    llvm_unreachable("bad initializer kind");
    }

  case DeclContextKind::TopLevelCodeDecl:
  case DeclContextKind::SerializedTopLevelCodeDecl:
    // Mangle the containing module context.
    return appendContext(ctx->getParent(), base, useModuleName);

  case DeclContextKind::MacroDecl:
    return appendContext(ctx->getParent(), base, useModuleName);
  }

  llvm_unreachable("bad decl context");
}

void ASTMangler::appendModule(const ModuleDecl *module,
                              StringRef useModuleName) {
  assert(!module->getParent() && "cannot mangle nested modules!");

  // Use the module real name in mangling; this is the physical name
  // of the module on-disk, which can be different if -module-alias is
  // used.
  //
  // For example, if a module Foo has 'import Bar', and '-module-alias Bar=Baz'
  // was passed, the name 'Baz' will be used for mangling besides loading.
  StringRef ModName = module->getRealName().str();

  // If RespectOriginallyDefinedIn is not set, ignore the ABI name only for
  // _Concurrency and swift-syntax (which adds "Compiler" as a prefix when
  // building swift-syntax as part of the compiler).
  // TODO: Mangling for the debugger should respect originally defined in, but
  // as of right now there is not enough information in the mangled name to
  // reconstruct AST types from mangled names when using that attribute.
  if ((RespectOriginallyDefinedIn ||
       (module->getName().str() != SWIFT_CONCURRENCY_NAME &&
        !module->getABIName().str().starts_with(
            SWIFT_MODULE_ABI_NAME_PREFIX))) &&
      module->getABIName() != module->getName())
    ModName = module->getABIName().str();

  // Try the special 'swift' substitution.
  if (ModName == STDLIB_NAME) {
    if (useModuleName.empty()) {
      appendOperator("s");
    } else if (!RespectOriginallyDefinedIn) {
      appendOperator("s");
    } else {
      appendIdentifier(useModuleName);
    }
    return;
  }

  if (ModName == MANGLING_MODULE_OBJC) {
    assert(useModuleName.empty());
    return appendOperator("So");
  }
  if (ModName == MANGLING_MODULE_CLANG_IMPORTER) {
    assert(useModuleName.empty());
    return appendOperator("SC");
  }

  // Disabling RespectOriginallyDefinedIn indicate the mangled names are not part
  // of the ABI, probably used by the debugger or IDE (USR). These mangled names
  // will not be demangled successfully if we use the original module name instead
  // of the actual module name.
  if (!useModuleName.empty() && RespectOriginallyDefinedIn)
    appendIdentifier(useModuleName);
  else
    appendIdentifier(ModName);
}

/// Mangle the name of a protocol as a substitution candidate.
void ASTMangler::appendProtocolName(const ProtocolDecl *protocol,
                                    bool allowStandardSubstitution) {
  assert(AllowMarkerProtocols || !protocol->isMarkerProtocol());

  if (allowStandardSubstitution && tryAppendStandardSubstitution(protocol))
    return;

  // We can use a symbolic reference if they're allowed in this context.
  if (canSymbolicReference(protocol)) {
    // Try to use a symbolic reference substitution.
    if (tryMangleSubstitution(protocol))
      return;
  
    appendSymbolicReference(protocol);
    // Substitutions can refer back to the symbolic reference.
    addSubstitution(protocol);
    return;
  }

  BaseEntitySignature base(protocol);
  appendContextOf(protocol, base);
  auto *clangDecl = protocol->getClangDecl();
  auto clangProto = cast_or_null<clang::ObjCProtocolDecl>(clangDecl);
  if (clangProto && UseObjCRuntimeNames)
    appendIdentifier(clangProto->getObjCRuntimeNameAsString());
  else if (clangProto)
    appendIdentifier(clangProto->getName());
  else
    appendDeclName(protocol);
}

bool ASTMangler::isCXXCFOptionsDefinition(const ValueDecl *decl) {
  return getTypeDefForCXXCFOptionsDefinition(decl);
}

const clang::TypedefType *
ASTMangler::getTypeDefForCXXCFOptionsDefinition(const ValueDecl *decl) {
  const clang::Decl *clangDecl = decl->getClangDecl();
  if (!clangDecl)
    return nullptr;

  const auto &clangModuleLoader = decl->getASTContext().getClangModuleLoader();
  return clangModuleLoader->getTypeDefForCXXCFOptionsDefinition(clangDecl);
}

const clang::NamedDecl *
ASTMangler::getClangDeclForMangling(const ValueDecl *vd) {
  auto namedDecl = dyn_cast_or_null<clang::NamedDecl>(vd->getClangDecl());
  if (!namedDecl)
    return nullptr;

  // Use an anonymous enum's enclosing typedef for the mangled name, if
  // present. This matches C++'s rules for linkage names of tag declarations.
  if (namedDecl->getDeclName().isEmpty())
    if (auto *tagDecl = dyn_cast<clang::TagDecl>(namedDecl))
      if (auto *typedefDecl = tagDecl->getTypedefNameForAnonDecl())
        namedDecl = typedefDecl;

  if (namedDecl->getDeclName().isEmpty())
    return nullptr;

  return namedDecl;
}

void ASTMangler::appendSymbolicReference(SymbolicReferent referent) {
  // Drop in a placeholder. The real reference value has to be filled in during
  // lowering to IR.
  auto offset = Buffer.str().size();
  Buffer << StringRef("\0\0\0\0\0", 5);
  SymbolicReferences.emplace_back(referent, offset);
}

// Canonicalizes a list of inverse requirements for the entity in a few ways:
//
// - Filters out inverse requirements that were eliminated by the entity's
//   generic signature because that nested signature introduced a conformance
//   requirement.
//
// - Filters out inverse requirements that were already mangled into the context
//
// - Sorts the inverse requirements in a canonical way.
static void reconcileInverses(
           SmallVector<InverseRequirement, 2> &inverses,
           GenericSignature sig,
           std::optional<unsigned> inversesAlreadyMangledDepth,
           std::optional<unsigned> suppressedInnermostDepth) {
  CanGenericSignature baseSig;
  if (sig)
    baseSig = sig.getCanonicalSignature();

  if (baseSig || inversesAlreadyMangledDepth || suppressedInnermostDepth)
    llvm::erase_if(inverses, [&](InverseRequirement const& inv) -> bool {
      // Drop inverses that aren't applicable in the nested / child signature,
      // because of an added requirement.
      if (baseSig && baseSig->requiresProtocol(inv.subject, inv.protocol))
        return true;

      auto gp = inv.subject->castTo<GenericTypeParamType>();

      // Remove inverses that were either already mangled for this entity,
      // or chosen not to be included in the output.
      if (auto limit = inversesAlreadyMangledDepth)
        if (gp->getDepth() <= limit)
          return true;

      if (suppressedInnermostDepth &&
          gp->getDepth() == *suppressedInnermostDepth)
        return true;

      return false;
    });

  // Sort inverse requirements for stability.
  llvm::array_pod_sort(
      inverses.begin(), inverses.end(),
    [](const InverseRequirement *lhs, const InverseRequirement *rhs) -> int {
      return lhs->compare(*rhs);
    });
}

// If we're mangling an entity in a `nominal-type` context and it has inverse
// requirements, then after appending the `nominal-type` context, we follow it
// with the mangling of a constrained extension. For example, if we've appended
// a context consisting of just a nominal-type:
//
//    context ::= entity
//    entity ::= nominal-type
//
// Then this function `appendContextualInverses` will tack-on extension mangling
// with a generic signature containing the inverses for that nominal-type's
// generic parameters:
//
//    context ::= entity module generic-signature? 'E'
//
// If there are no inverses, then we do not append extension mangling.
//
// The end goal is that we mangle members of a nominal type, where the nominal
// has inverse requirements in its signature, as though the members are in an
// extension with a generic signature with those inverse requirements:
//
//     struct X<Y: ~Copyable> {
//       func foo<Z: ~Copyable>() {} // has the same mangling as the `foo` below
//     }
//
//     extension X where Y: ~Copyable {
//       func foo<Z: ~Copyable>() {}
//     }
//
// Thus, this function appends the remaining piece to an existing context to
// make it look as if the context is an extension:
//
//     context ::= context* nominal-type module generic-signature? 'E'
//                                       ^~~~~~~~~~~~~~~~~~~~~~~~~~~~~
//                                         this piece will be appended
//
// Only the inverse requirements from the given signature will be included!
void ASTMangler::appendContextualInverses(const GenericTypeDecl *contextDecl,
                                          BaseEntitySignature &base,
                                          const ModuleDecl *module,
                                          StringRef alternateModuleName) {
  // If we are nested within a real extension, don't append inverses.
  if (base.reachedExtension())
    return;

  // Only append for contexts non-protocol nominals that support extensions.
  if (!isa<ClassDecl, EnumDecl, StructDecl>(contextDecl))
    return;

  GenericSignature sig = contextDecl->getGenericSignature();
  GenericSignatureParts parts;
  gatherGenericSignatureParts(contextDecl->getGenericSignature(),
                              /*contextSig=*/nullptr,
                              base,
                              parts);

  if (parts.inverses.empty())
    return;

  // Ignore normal requirements.
  parts.requirements.clear();

  // Pretend we're an extension, and `sig` is the nominal type decl's signature
  // that we're extending.

  // There are no generic parameters in this extension itself.
  parts.params = std::nullopt;

  // The depth of parameters for this extension is +1 of the extended signature.
  parts.initialParamDepth = sig.getNextDepth();

  appendModule(module, alternateModuleName);
  appendGenericSignatureParts(sig, parts);
  return appendOperator("E");
}

void ASTMangler::appendExtension(const ExtensionDecl* ext,
                                 BaseEntitySignature &base,
                                 StringRef useModuleName) {
  auto decl = ext->getExtendedNominal();
  // Recover from erroneous extension.
  if (!decl)
    return appendContext(ext->getDeclContext(), base, useModuleName);

  auto nominalSig = ext->getSelfNominalTypeDecl()
                       ->getGenericSignatureOfContext();
  auto sig = ext->getGenericSignature();
  base.setReachedExtension();

  // Determine what parts of this extension's generic signature actually needs
  // to be mangled
  GenericSignatureParts sigParts;
  gatherGenericSignatureParts(sig, nominalSig, base, sigParts);

  // Mangle the extension unless:
  // 1. the extension is defined in the same module as the original
  //   nominal type decl, and
  // 2. the extension is unconstrained, and
  // 3. the extension is not for a protocol.
  // FIXME: In a world where protocol extensions are dynamically dispatched,
  // "extension is to a protocol" would no longer be a reason to use the
  // extension mangling, because an extension method implementation could be
  // resiliently moved into the original protocol itself.
  if (ext->isInSameDefiningModule() // case 1
        && !sigParts.hasRequirements() // case 2
        && !ext->getDeclaredInterfaceType()->isExistentialType()) { // case 3
    // skip extension mangling
    return appendAnyGenericType(decl);
  }

  // Perform extension mangling
  appendAnyGenericType(decl);
  appendModule(ext->getParentModule(), useModuleName);
  // If the extension is constrained, mangle the generic signature that
  // constrains it.
  if (!sigParts.isNull()) {
    Mod = ext->getModuleContext();
    appendGenericSignatureParts(sig, sigParts);
  }
  return appendOperator("E");
}

void ASTMangler::appendAnyGenericType(const GenericTypeDecl *decl) {
  BaseEntitySignature base(decl);
  appendAnyGenericType(decl, base);
}

void ASTMangler::appendAnyGenericType(const GenericTypeDecl *decl,
                                      BaseEntitySignature &base) {
  auto *nominal = dyn_cast<NominalTypeDecl>(decl);

  if (nominal && isa<BuiltinTupleDecl>(nominal))
    return appendOperator("BT");

  // Check for certain standard types.
  if (tryAppendStandardSubstitution(decl))
    return;

  // Mangle opaque type names.
  if (auto opaque = dyn_cast<OpaqueTypeDecl>(decl)) {
    appendOpaqueDeclName(opaque);
    return;
  }

  // For generic types, this uses the unbound type.
  if (nominal) {
    if (tryMangleTypeSubstitution(nominal->getDeclaredType(), nullptr))
      return;
  } else {
    if (tryMangleSubstitution(cast<TypeAliasDecl>(decl)))
      return;
  }
  
  // Try to mangle a symbolic reference for a nominal type.
  if (nominal && canSymbolicReference(nominal)) {
    appendSymbolicReference(nominal);
    // Substitutions can refer back to the symbolic reference.
    addTypeSubstitution(nominal->getDeclaredType(), nullptr);
    return;
  }

  appendContextOf(decl, base);

  // Always use Clang names for imported Clang declarations, unless they don't
  // have one.
  auto tryAppendClangName = [this, decl]() -> bool {
    auto *nominal = dyn_cast<NominalTypeDecl>(decl);
    auto namedDecl = getClangDeclForMangling(decl);
    if (!namedDecl) {
      if (auto typedefType = getTypeDefForCXXCFOptionsDefinition(decl)) {
        // To make sure the C++ definition of CF_OPTIONS mangles the
        // same way as the Objective-C definition, we mangle using the
        // name of the backing typedef, but pretend as if it was an enum.
        // See CFAvailability.h to understand how the definitions differ
        // in C++ and Objective-C
        appendIdentifier(typedefType->getDecl()->getName());
        appendOperator("V");
        return true;
      }

      return false;
    }

    // Mangle ObjC classes using their runtime names.
    auto interface = dyn_cast<clang::ObjCInterfaceDecl>(namedDecl);
    auto protocol = dyn_cast<clang::ObjCProtocolDecl>(namedDecl);
    
    if (UseObjCRuntimeNames && interface) {
      appendIdentifier(interface->getObjCRuntimeNameAsString());
    } else if (UseObjCRuntimeNames && protocol) {
      appendIdentifier(protocol->getObjCRuntimeNameAsString());
    } else if (auto ctsd = dyn_cast<clang::ClassTemplateSpecializationDecl>(namedDecl)) {
      // If this is a `ClassTemplateSpecializationDecl`, it was
      // imported as a Swift decl with `__CxxTemplateInst...` name.
      // `ClassTemplateSpecializationDecl`'s name does not include information about
      // template arguments, and in order to prevent name clashes we use the
      // name of the Swift decl which does include template arguments.
      appendIdentifier(nominal->getName().str());
    } else {
      appendIdentifier(namedDecl->getName());
    }

    // The important distinctions to maintain here are Objective-C's various
    // namespaces: protocols, tags (struct/enum/union), and unqualified names.
    // We continue to mangle "class" the standard Swift way because it feels
    // weird to call that an alias, but they're really in the same namespace.
    if (interface) {
      appendOperator("C");
    } else if (protocol) {
      appendOperator("P");
    } else if (isa<clang::TagDecl>(namedDecl)) {
      // Note: This includes enums, but that's okay. A Clang enum is not always
      // imported as a Swift enum.
      appendOperator("V");
    } else if (isa<clang::TypedefNameDecl>(namedDecl) ||
               isa<clang::ObjCCompatibleAliasDecl>(namedDecl)) {
      appendOperator("a");
    } else if (isa<clang::NamespaceDecl>(namedDecl)) {
      // Note: Namespaces are not really enums, but since namespaces are
      // imported as enums, be consistent.
      appendOperator("O");
    } else if (isa<clang::ClassTemplateDecl>(namedDecl)) {
      appendIdentifier(nominal->getName().str());
    } else {
      llvm_unreachable("unknown imported Clang type");
    }

    return true;
  };

  if (!tryAppendClangName()) {
    appendDeclName(decl);

    switch (decl->getKind()) {
    default:
      llvm_unreachable("not a nominal type");

    case DeclKind::TypeAlias:
      appendOperator("a");
      break;
    case DeclKind::Protocol:
      assert(AllowMarkerProtocols ||
             !cast<ProtocolDecl>(decl)->isMarkerProtocol());
      appendOperator("P");
      break;
    case DeclKind::Class:
      appendOperator("C");
      break;
    case DeclKind::Enum:
      appendOperator("O");
      break;
    case DeclKind::Struct:
      appendOperator("V");
      break;
    case DeclKind::BuiltinTuple:
      llvm_unreachable("Not implemented");
    }
  }

  if (nominal)
    addTypeSubstitution(nominal->getDeclaredType(), nullptr);
  else
    addSubstitution(cast<TypeAliasDecl>(decl));
}

void ASTMangler::appendFunction(AnyFunctionType *fn, GenericSignature sig,
                                FunctionManglingKind functionMangling,
                                const ValueDecl *forDecl, bool isRecursedInto) {
  // Append parameter labels right before the signature/type.
  auto parameters = fn->getParams();
  auto firstLabel = std::find_if(
                  parameters.begin(), parameters.end(),
                  [&](AnyFunctionType::Param param) { return param.hasLabel(); });

  if (firstLabel != parameters.end()) {
    for (auto param : parameters) {
      auto label = param.getLabel();
      if (!label.empty())
        appendIdentifier(label.str());
      else
        appendOperator("_");
    }
  } else if (!parameters.empty()) {
    appendOperator("y");
  }

  if (functionMangling != NoFunctionMangling) {
    appendFunctionSignature(fn, sig, forDecl, functionMangling, isRecursedInto);
  } else {
    appendFunctionType(fn, sig, /*autoclosure*/ false, forDecl, isRecursedInto);
  }
}

void ASTMangler::appendFunctionType(AnyFunctionType *fn, GenericSignature sig,
                                    bool isAutoClosure,
                                    const ValueDecl *forDecl,
                                    bool isRecursedInto) {
  assert((DWARFMangling || fn->isCanonical()) &&
         "expecting canonical types when not mangling for the debugger");

  appendFunctionSignature(fn, sig, forDecl, NoFunctionMangling, isRecursedInto);

  bool mangleClangType = fn->getASTContext().LangOpts.UseClangFunctionTypes &&
                         fn->hasNonDerivableClangType();

  // Note that we do not currently use thin representations in the AST
  // for the types of function decls.  This may need to change at some
  // point, in which case the uncurry logic can probably migrate to that
  // case.
  //
  // It would have been cleverer if we'd used 'f' for thin functions
  // and something else for uncurried functions, but oh well.
  //
  // Or maybe we can change the mangling at the same time we make
  // changes to better support thin functions.
  switch (fn->getRepresentation()) {
  case AnyFunctionType::Representation::Block:
    if (mangleClangType) {
      appendOperator("XzB");
      return appendClangType(fn);
    }
    // We distinguish escaping and non-escaping blocks, but only in the DWARF
    // mangling, because the ABI is already set.
    if (!fn->isNoEscape() && DWARFMangling)
      return appendOperator("XL");
    return appendOperator("XB");
  case AnyFunctionType::Representation::Thin:
    return appendOperator("Xf");
  case AnyFunctionType::Representation::Swift:
    if (isAutoClosure) {
      if (fn->isNoEscape())
        return appendOperator("XK");
      else
        return appendOperator("XA");
    } else if (fn->isNoEscape()) {
      return appendOperator("XE");
    }
    return appendOperator("c");

  case AnyFunctionType::Representation::CFunctionPointer:
    if (mangleClangType) {
      appendOperator("XzC");
      return appendClangType(fn);
    }
    return appendOperator("XC");
  }
}

template <typename FnType>
void ASTMangler::appendClangType(FnType *fn, llvm::raw_svector_ostream &out) {
  auto clangType = fn->getClangTypeInfo().getType();
  SmallString<64> scratch;
  llvm::raw_svector_ostream scratchOS(scratch);
  clang::ASTContext &clangCtx =
      fn->getASTContext().getClangModuleLoader()->getClangASTContext();
  std::unique_ptr<clang::ItaniumMangleContext> mangler{
      clang::ItaniumMangleContext::create(clangCtx, clangCtx.getDiagnostics())};
  mangler->mangleTypeName(clang::QualType(clangType, 0), scratchOS);
  out << scratchOS.str().size() << scratchOS.str();
}

void ASTMangler::appendClangType(AnyFunctionType *fn) {
  appendClangType(fn, Buffer);
}

void ASTMangler::appendFunctionSignature(AnyFunctionType *fn,
                                         GenericSignature sig,
                                         const ValueDecl *forDecl,
                                         FunctionManglingKind functionMangling,
                                         bool isRecursedInto) {
  appendFunctionResultType(fn->getResult(), sig, forDecl);
  appendFunctionInputType(fn->getParams(), fn->getLifetimeDependenceInfo(), sig,
                          forDecl, isRecursedInto);
  if (fn->isAsync())
    appendOperator("Ya");
  if (fn->isSendable())
    appendOperator("Yb");
  if (auto thrownError = fn->getEffectiveThrownErrorType()) {
    if ((*thrownError)->isEqual(fn->getASTContext().getErrorExistentialType())){
      appendOperator("K");
    } else {
      appendType(*thrownError, sig);
      appendOperator("YK");
    }
  }
  switch (auto diffKind = fn->getDifferentiabilityKind()) {
  case DifferentiabilityKind::NonDifferentiable:
    break;
  case DifferentiabilityKind::Forward:
    appendOperator("Yjf");
    break;
  case DifferentiabilityKind::Reverse:
    appendOperator("Yjr");
    break;
  case DifferentiabilityKind::Normal:
    appendOperator("Yjd");
    break;
  case DifferentiabilityKind::Linear:
    appendOperator("Yjl");
    break;
  }

  auto isolation = fn->getIsolation();
  switch (isolation.getKind()) {
  case FunctionTypeIsolation::Kind::NonIsolated:
    break;
  case FunctionTypeIsolation::Kind::Parameter:
    // Parameter isolation is already mangled in the parameters.
    break;
  case FunctionTypeIsolation::Kind::GlobalActor:
    appendType(isolation.getGlobalActorType(), sig);
    appendOperator("Yc");
    break;
  case FunctionTypeIsolation::Kind::Erased:
    appendOperator("YA");
    break;
  }

  if (isRecursedInto && fn->hasSendingResult()) {
    appendOperator("YT");
  }

  if (auto *afd = dyn_cast_or_null<AbstractFunctionDecl>(forDecl)) {
    if (afd->hasImplicitSelfDecl()) {
      auto lifetimeDependenceKind =
          fn->getLifetimeDependenceInfo().getLifetimeDependenceOnParam(
              /*selfIndex*/ afd->getParameters()->size());
      if (lifetimeDependenceKind) {
        appendLifetimeDependenceKind(*lifetimeDependenceKind,
                                     /*isSelfDependence*/ true);
      }
    }
  }
}

static ParamSpecifier
getDefaultOwnership(const ValueDecl *forDecl) {
  // `consuming` is the default ownership for initializers and setters.
  // Everything else defaults to borrowing.
  if (!forDecl) {
    return ParamSpecifier::Borrowing;
  }
  auto forFuncDecl = dyn_cast<AbstractFunctionDecl>(forDecl);
  if (!forFuncDecl) {
    return ParamSpecifier::Borrowing;
  }
  
  if (isa<ConstructorDecl>(forFuncDecl)) {
    return ParamSpecifier::Consuming;
  } else if (auto accessor = dyn_cast<AccessorDecl>(forFuncDecl)) {
    switch (accessor->getAccessorKind()) {
    case AccessorKind::Modify:
    case AccessorKind::Set:
      return ParamSpecifier::Consuming;
    default:
      return ParamSpecifier::Borrowing;
    }
  }
  
  return ParamSpecifier::Borrowing;
}

static ParameterTypeFlags
getParameterFlagsForMangling(ParameterTypeFlags flags,
                             ParamSpecifier defaultSpecifier,
                             bool isInRecursion = true) {
  bool initiallySending = flags.isSending();

  // If we have been recursed into, then remove sending from our flags.
  if (!isInRecursion) {
    flags = flags.withSending(false);
  }

  switch (auto specifier = flags.getOwnershipSpecifier()) {
  // If no parameter specifier was provided, mangle as-is, because we are by
  // definition using the default convention.
  case ParamSpecifier::Default:
  // If the legacy `__shared` or `__owned` modifier was provided, mangle as-is,
  // because we need to maintain compatibility with their existing behavior.
  case ParamSpecifier::LegacyOwned:
  // `inout` should already be specified in the flags.
  case ParamSpecifier::InOut:
    return flags;
  case ParamSpecifier::ImplicitlyCopyableConsuming:
  case ParamSpecifier::Consuming:
  case ParamSpecifier::Borrowing:
    // Only mangle the ownership if it diverges from the default.
    if (specifier == defaultSpecifier) {
      flags = flags.withOwnershipSpecifier(ParamSpecifier::Default);
    }
    return flags;
  case ParamSpecifier::LegacyShared:
    // If we were originally sending and by default we are borrowing, suppress
    // this and set ownership specifier to default so we do not mangle in
    // __shared.
    //
    // This is a work around in the short term since shared borrow is not
    // supported.
    if (initiallySending && ParamSpecifier::Borrowing == defaultSpecifier)
      return flags.withOwnershipSpecifier(ParamSpecifier::Default);
    return flags;
  }
}

void ASTMangler::appendFunctionInputType(
    ArrayRef<AnyFunctionType::Param> params,
    LifetimeDependenceInfo lifetimeDependenceInfo, GenericSignature sig,
    const ValueDecl *forDecl, bool isRecursedInto) {
  auto defaultSpecifier = getDefaultOwnership(forDecl);
  
  switch (params.size()) {
  case 0:
    appendOperator("y");
    break;

  case 1: {
    const auto &param = params.front();
    auto type = param.getPlainType();

    // If the sole unlabeled parameter has a non-tuple type, encode
    // the parameter list as a single type.
    if (!param.hasLabel() && !param.isVariadic() &&
        !isa<TupleType>(type.getPointer())) {
      // Note that we pass `nullptr` as the `forDecl` argument, since the type
      // of the input is no longer directly the type of the declaration, so we
      // don't want it to pick up contextual behavior, such as default ownership,
      // from the top-level declaration type.
      appendParameterTypeListElement(
          Identifier(), type,
          getParameterFlagsForMangling(param.getParameterFlags(),
                                       defaultSpecifier, isRecursedInto),
          lifetimeDependenceInfo.getLifetimeDependenceOnParam(/*paramIndex*/ 0),
          sig, nullptr);
      break;
    }

    // If this is a tuple type with a single labeled element
    // let's handle it as a general case.
    LLVM_FALLTHROUGH;
  }

  default:
    bool isFirstParam = true;
    unsigned paramIndex = 0;
    for (auto &param : params) {
      // Note that we pass `nullptr` as the `forDecl` argument, since the type
      // of the input is no longer directly the type of the declaration, so we
      // don't want it to pick up contextual behavior, such as default ownership,
      // from the top-level declaration type.
      appendParameterTypeListElement(
          Identifier(), param.getPlainType(),
          getParameterFlagsForMangling(param.getParameterFlags(),
                                       defaultSpecifier, isRecursedInto),
          lifetimeDependenceInfo.getLifetimeDependenceOnParam(paramIndex), sig,
          nullptr);
      appendListSeparator(isFirstParam);
      paramIndex++;
    }
    appendOperator("t");
    break;
  }
}

void ASTMangler::appendFunctionResultType(Type resultType, GenericSignature sig,
                                          const ValueDecl *forDecl) {
  return resultType->isVoid() ? appendOperator("y")
                              : appendType(resultType, sig, forDecl);
}

void ASTMangler::appendTypeList(Type listTy, GenericSignature sig,
                                const ValueDecl *forDecl) {
  if (TupleType *tuple = listTy->getAs<TupleType>()) {
    if (tuple->getNumElements() == 0)
      return appendOperator("y");
    bool firstField = true;
    for (auto &field : tuple->getElements()) {
      appendTupleTypeListElement(field.getName(), field.getType(), sig,
                                 forDecl);
      appendListSeparator(firstField);
    }
  } else {
    appendType(listTy, sig, forDecl);
    appendListSeparator();
  }
}

void ASTMangler::appendParameterTypeListElement(
    Identifier name, Type elementType, ParameterTypeFlags flags,
    std::optional<LifetimeDependenceKind> lifetimeDependenceKind,
    GenericSignature sig, const ValueDecl *forDecl) {
  if (auto *fnType = elementType->getAs<FunctionType>())
    appendFunctionType(fnType, sig, flags.isAutoClosure(), forDecl);
  else
    appendType(elementType, sig, forDecl);

  if (flags.isNoDerivative()) {
    appendOperator("Yk");
  }
  switch (flags.getValueOwnership()) {
  case ValueOwnership::Default:
    /* nothing */
    break;
  case ValueOwnership::InOut:
    appendOperator("z");
    break;
  case ValueOwnership::Shared:
    appendOperator("h");
    break;
  case ValueOwnership::Owned:
    appendOperator("n");
    break;
  }
  if (flags.isIsolated())
    appendOperator("Yi");
  if (flags.isSending())
    appendOperator("Yu");
  if (flags.isCompileTimeConst())
    appendOperator("Yt");

  if (lifetimeDependenceKind) {
    appendLifetimeDependenceKind(*lifetimeDependenceKind,
                                 /*isSelfDependence*/ false);
  }

  if (!name.empty())
    appendIdentifier(name.str());
  if (flags.isVariadic())
    appendOperator("d");
}

void ASTMangler::appendLifetimeDependenceKind(LifetimeDependenceKind kind,
                                              bool isSelfDependence) {
  if (kind == LifetimeDependenceKind::Scope) {
    if (isSelfDependence) {
      appendOperator("YLs");
    } else {
      appendOperator("Yls");
    }
  } else {
    assert(kind == LifetimeDependenceKind::Inherit);
    if (isSelfDependence) {
      appendOperator("YLi");
    } else {
      appendOperator("Yli");
    }
  }
}

void ASTMangler::appendTupleTypeListElement(Identifier name, Type elementType,
                                            GenericSignature sig,
                                            const ValueDecl *forDecl) {
  if (auto *fnType = elementType->getAs<FunctionType>())
    appendFunctionType(fnType, sig, /*isAutoClosure*/ false, forDecl);
  else
    appendType(elementType, sig, forDecl);

  if (!name.empty())
    appendIdentifier(name.str());
}

bool ASTMangler::GenericSignatureParts::isNull() const {
  return params.empty() && !hasRequirements();
}

bool ASTMangler::GenericSignatureParts::hasRequirements() const {
  return !requirements.empty() || !inverses.empty();
}

void ASTMangler::GenericSignatureParts::clear() {
  params = std::nullopt;
  requirements.clear();
  inverses.clear();
  initialParamDepth = 0;
}

bool ASTMangler::appendGenericSignature(GenericSignature sig) {
  BaseEntitySignature nullBase(nullptr);
  return appendGenericSignature(sig, /*contextSig=*/nullptr, nullBase);
}

bool ASTMangler::appendGenericSignature(GenericSignature sig,
                                        GenericSignature contextSig,
                                        BaseEntitySignature &base) {
  GenericSignatureParts parts;
  gatherGenericSignatureParts(sig, contextSig, base, parts);

  if (parts.isNull())
    return false;

  appendGenericSignatureParts(sig, parts);
  return true;
}

template<typename T>
bool same(ArrayRef<T> as, ArrayRef<T> bs) {
  if (as.size() != bs.size())
    return false;

  for (size_t i = 0; i < as.size(); ++i) {
    if (as[i] != bs[i])
      return false;
  }
  return true;
}

void ASTMangler::gatherGenericSignatureParts(GenericSignature sig,
                                             GenericSignature contextSig,
                                             BaseEntitySignature &base,
                                             GenericSignatureParts &parts) {
  assert(parts.isNull());

  // No signature, parts.
  if (!sig)
    return;

  auto canSig = sig.getCanonicalSignature();

  // Separate requirements and inverses.
  auto &reqs = parts.requirements;
  auto &inverseReqs = parts.inverses;
  canSig->getRequirementsWithInverses(reqs, inverseReqs);

  // Process inverses relative to the base entity's signature.
  if (AllowInverses) {
    // Simplify and canonicalize inverses.
    reconcileInverses(inverseReqs, base.getSignature(), base.getDepth(),
                      base.getSuppressedInnermostInversesDepth());
  } else {
    inverseReqs.clear();
  }
  base.setDepth(canSig->getMaxDepth());

  unsigned &initialParamDepth = parts.initialParamDepth;
  auto &genericParams = parts.params;

  if (!contextSig) {
    // Use the complete canonical signature.
    initialParamDepth = 0;
    genericParams = canSig.getGenericParams();
    return;
  }

  auto canContextSig = contextSig.getCanonicalSignature();
  SmallVector<Requirement, 2> contextReqs;
  {
    SmallVector<InverseRequirement, 2> __ignored;
    canContextSig->getRequirementsWithInverses(contextReqs, __ignored);
  }

  // The signature depth starts above the depth of the context signature.
  if (!contextSig.getGenericParams().empty()) {
    initialParamDepth = contextSig.getNextDepth();
  }

  // If both signatures have exactly the same requirements, ignoring
  // conformances for invertible protocols, and the same generic parameters,
  // and there's no inverses to mangle, then there's nothing to do.
  if (inverseReqs.empty()
      && same(canContextSig.getGenericParams(), canSig.getGenericParams())
      && same(llvm::ArrayRef(contextReqs), llvm::ArrayRef(reqs))) {
    parts.clear();
    return;
  }

  // Find the parameters at this depth (or greater).
  genericParams = canSig.getGenericParams();
  unsigned firstParam = genericParams.size();
  while (firstParam > 1 &&
         genericParams[firstParam-1]->getDepth() >= initialParamDepth)
    --firstParam;
  genericParams = genericParams.slice(firstParam);

  // Special case: if we would be mangling zero generic parameters, but
  // the context signature is a single, unconstrained generic parameter,
  // it's better to mangle the complete canonical signature because we
  // have a special-case mangling for that.
  if (genericParams.empty() &&
      contextSig.getGenericParams().size() == 1 &&
      contextReqs.empty() &&
      inverseReqs.empty()) {
    initialParamDepth = 0;
    genericParams = canSig.getGenericParams();
  } else {
    llvm::erase_if(reqs, [&](Requirement req) {
      return contextSig->isRequirementSatisfied(req);
    });
  }
}

ASTMangler::RequirementSubject ASTMangler::appendRequirementSubject(
    CanType subjectType, GenericSignature sig) {
  // Handle dependent types.
  if (auto *depTy = subjectType->getAs<DependentMemberType>()) {
    if (tryMangleTypeSubstitution(depTy, sig)) {
      return RequirementSubject {RequirementSubject::Substitution};
    }

    bool isAssocTypeAtDepth = false;
    GenericTypeParamType *gpBase = appendAssocType(depTy, sig,
                                                   isAssocTypeAtDepth);
    addTypeSubstitution(depTy, sig);
    return RequirementSubject {
      isAssocTypeAtDepth ? RequirementSubject::AssociatedTypeAtDepth
                         : RequirementSubject::AssociatedType,
      gpBase
    };
  }

  GenericTypeParamType *gpBase = subjectType->castTo<GenericTypeParamType>();
  return RequirementSubject { RequirementSubject::GenericParameter, gpBase };
}

void ASTMangler::appendRequirement(const Requirement &reqt,
                                   GenericSignature sig,
                                   bool lhsBaseIsProtocolSelf) {

  CanType FirstTy = reqt.getFirstType()->getCanonicalType();

  switch (reqt.getKind()) {
  case RequirementKind::Layout:
    break;
  case RequirementKind::Conformance: {
    // If we don't allow marker protocols but we have one here, skip it.
    if (!AllowMarkerProtocols &&
        reqt.getProtocolDecl()->isMarkerProtocol())
      return;

    appendProtocolName(reqt.getProtocolDecl());
  } break;
  case RequirementKind::Superclass:
  case RequirementKind::SameType:
  case RequirementKind::SameShape: {
    Type SecondTy = reqt.getSecondType();
    appendType(SecondTy->getCanonicalType(), sig);
    break;
  }
  }

  auto subject = appendRequirementSubject(FirstTy, sig);
  switch (subject.kind) {
  case RequirementSubject::GenericParameter:
    switch (reqt.getKind()) {
    case RequirementKind::Conformance:
      return appendOpWithGenericParamIndex("R", subject.gpBase);
    case RequirementKind::Layout:
      appendOpWithGenericParamIndex("Rl", subject.gpBase);
      appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
      return;
    case RequirementKind::Superclass:
      return appendOpWithGenericParamIndex("Rb", subject.gpBase);
    case RequirementKind::SameType:
      return appendOpWithGenericParamIndex("Rs", subject.gpBase);
    case RequirementKind::SameShape:
      return appendOpWithGenericParamIndex("Rh", subject.gpBase);
    }
    break;

  case RequirementSubject::Substitution:
    switch (reqt.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement with dependent member type?");
    case RequirementKind::Conformance:
      return appendOperator("RQ");
    case RequirementKind::Layout:
      appendOperator("RL");
      appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
      return;
    case RequirementKind::Superclass:
      return appendOperator("RB");
    case RequirementKind::SameType:
      return appendOperator("RS");
    }
    break;

  case RequirementSubject::AssociatedType:
  case RequirementSubject::AssociatedTypeAtDepth:
    bool isAssocTypeAtDepth =
        subject.kind == RequirementSubject::AssociatedTypeAtDepth;
    auto gpBase = subject.gpBase;
    switch (reqt.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement with a dependent member type?");
    case RequirementKind::Conformance:
      return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RP" : "Rp",
                                           gpBase, lhsBaseIsProtocolSelf);
    case RequirementKind::Layout:
      appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RM" : "Rm", gpBase,
                                    lhsBaseIsProtocolSelf);
      appendOpParamForLayoutConstraint(reqt.getLayoutConstraint());
      return;
    case RequirementKind::Superclass:
      return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RC" : "Rc",
                                           gpBase, lhsBaseIsProtocolSelf);
    case RequirementKind::SameType:
      return appendOpWithGenericParamIndex(isAssocTypeAtDepth ? "RT" : "Rt",
                                           gpBase, lhsBaseIsProtocolSelf);
    }
    break;
  }
}

void ASTMangler::appendInverseRequirement(const InverseRequirement &req,
                                          GenericSignature sig,
                                          bool lhsBaseIsProtocolSelf) {
  unsigned bit = static_cast<uint8_t>(req.getKind());
  auto firstType = req.subject->getCanonicalType();
  auto subject = appendRequirementSubject(firstType, sig);
  switch (subject.kind) {
  case RequirementSubject::GenericParameter:
    appendOperator("Ri", Index(bit));
    return appendOpWithGenericParamIndex("", subject.gpBase,
                                         lhsBaseIsProtocolSelf);

  case RequirementSubject::Substitution:
    return appendOperator("RI", Index(bit));

  case RequirementSubject::AssociatedType:
    appendOperator("Rj", Index(bit));
    return appendOpWithGenericParamIndex("", subject.gpBase,
                                         lhsBaseIsProtocolSelf);

  case RequirementSubject::AssociatedTypeAtDepth:
    appendOperator("RJ", Index(bit));
    return appendOpWithGenericParamIndex("", subject.gpBase,
                                         lhsBaseIsProtocolSelf);
  }
}

void ASTMangler::appendGenericSignatureParts(
                                     GenericSignature sig,
                                     GenericSignatureParts const& parts) {
  assert(!parts.isNull());
  ArrayRef<CanGenericTypeParamType> params = parts.params;
  unsigned initialParamDepth = parts.initialParamDepth;
  ArrayRef<Requirement> requirements = parts.requirements;
  ArrayRef<InverseRequirement> inverseRequirements = parts.inverses;

  // Mangle which generic parameters are pack parameters.
  for (auto param : params) {
    if (param->isParameterPack())
      appendOpWithGenericParamIndex("Rv", param);
  }

  // Mangle the requirements.
  for (const Requirement &reqt : requirements) {
    appendRequirement(reqt, sig);
  }

  // Mangle the inverse requirements.
  for (auto inverseReq : inverseRequirements) {
    appendInverseRequirement(inverseReq, sig);
  }

  if (params.size() == 1 && params[0]->getDepth() == initialParamDepth)
    return appendOperator("l");

  llvm::SmallVector<char, 16> OpStorage;
  llvm::raw_svector_ostream OpBuffer(OpStorage);

  // Mangle the number of parameters.
  unsigned depth = 0;
  unsigned count = 0;
  
  // Since it's unlikely (but not impossible) to have zero generic parameters
  // at a depth, encode indexes starting from 1, and use a special mangling
  // for zero.
  auto mangleGenericParamCount = [&](unsigned depth, unsigned count) {
    if (depth < initialParamDepth)
      return;
    if (count == 0)
      OpBuffer << 'z';
    else
      OpBuffer << Index(count - 1);
  };
  
  // As a special case, mangle nothing if there's a single generic parameter
  // at the initial depth.
  for (auto param : params) {
    if (param->getDepth() != depth) {
      assert(param->getDepth() > depth && "generic params not ordered");
      while (depth < param->getDepth()) {
        mangleGenericParamCount(depth, count);
        ++depth;
        count = 0;
      }
    }
    assert(param->getIndex() == count && "generic params not ordered");
    ++count;
  }
  mangleGenericParamCount(depth, count);
  OpBuffer << 'l';

  appendOperator("r", StringRef(OpStorage.data(), OpStorage.size()));
}

/// Determine whether an associated type reference into the given set of
/// protocols is unambiguous.
static bool associatedTypeRefIsUnambiguous(GenericSignature sig, Type t) {
  // FIXME: This should be an assertion.
  if (!sig->isValidTypeParameter(t))
    return false;

  unsigned numProtocols = 0;
  for (auto proto : sig->getRequiredProtocols(t)) {
    // Skip marker protocols, which cannot have associated types.
    if (proto->isMarkerProtocol())
      continue;

    ++numProtocols;
  }

  return numProtocols <= 1;
}

// If the base type is known to have a single protocol conformance
// in the current generic context, then we don't need to disambiguate the
// associated type name by protocol.
DependentMemberType *
ASTMangler::dropProtocolFromAssociatedType(DependentMemberType *dmt,
                                           GenericSignature sig) {
  auto baseTy = dmt->getBase();
  bool unambiguous =
      (!dmt->getAssocType() ||
       associatedTypeRefIsUnambiguous(sig, baseTy));

  if (auto *baseDMT = baseTy->getAs<DependentMemberType>())
    baseTy = dropProtocolFromAssociatedType(baseDMT, sig);

  if (unambiguous)
    return DependentMemberType::get(baseTy, dmt->getName());

  return DependentMemberType::get(baseTy, dmt->getAssocType());
}

Type
ASTMangler::dropProtocolsFromAssociatedTypes(Type type,
                                             GenericSignature sig) {
  if (!OptimizeProtocolNames || !sig)
    return type;

  if (!type->hasDependentMember())
    return type;

  return type.transform([&](Type t) -> Type {
    if (auto *dmt = dyn_cast<DependentMemberType>(t.getPointer()))
      return dropProtocolFromAssociatedType(dmt, sig);
    return t;
  });
}

void ASTMangler::appendAssociatedTypeName(DependentMemberType *dmt,
                                          GenericSignature sig) {
  if (auto assocTy = dmt->getAssocType()) {
    appendIdentifier(assocTy->getName().str());

    // If the base type is known to have a single protocol conformance
    // in the current generic context, then we don't need to disambiguate the
    // associated type name by protocol.
    if (!OptimizeProtocolNames || !sig ||
        !associatedTypeRefIsUnambiguous(
            sig, dmt->getBase())) {
      BaseEntitySignature base(assocTy);
      appendAnyGenericType(assocTy->getProtocol(), base);
    }
    return;
  }

  appendIdentifier(dmt->getName().str());
}

void ASTMangler::appendClosureEntity(
                              const SerializedAbstractClosureExpr *closure) {
  assert(!closure->getType()->hasLocalArchetype() &&
         "Not enough information here to handle this case");

  appendClosureComponents(closure->getType(), closure->getDiscriminator(),
                          closure->isImplicit(), closure->getParent(),
                          ArrayRef<GenericEnvironment *>());
}

void ASTMangler::appendClosureEntity(const AbstractClosureExpr *closure) {
  ArrayRef<GenericEnvironment *> capturedEnvs;

  auto type = closure->getType();

  // FIXME: We can end up with a null type here in the presence of invalid
  // code; the type-checker currently isn't strict about producing typed
  // expression nodes when it fails. Once we enforce that, we can remove this.
  if (!type)
    type = ErrorType::get(closure->getASTContext());

  if (type->hasLocalArchetype())
    capturedEnvs = closure->getCaptureInfo().getGenericEnvironments();

  appendClosureComponents(type, closure->getDiscriminator(),
                          isa<AutoClosureExpr>(closure), closure->getParent(),
                          capturedEnvs);
}

void ASTMangler::appendClosureComponents(Type Ty, unsigned discriminator,
                                         bool isImplicit,
                                         const DeclContext *parentContext,
                                         ArrayRef<GenericEnvironment *> capturedEnvs) {
  assert(discriminator != AbstractClosureExpr::InvalidDiscriminator
         && "closure must be marked correctly with discriminator");

  BaseEntitySignature base(parentContext->getInnermostDeclarationDeclContext());
  appendContext(parentContext, base, StringRef());

  auto Sig = parentContext->getGenericSignatureOfContext();

  Ty = Ty.subst(MapLocalArchetypesOutOfContext(Sig, capturedEnvs),
                MakeAbstractConformanceForGenericType(),
                SubstFlags::PreservePackExpansionLevel);

  appendType(Ty->getCanonicalType(), Sig);
  appendOperator(isImplicit ? "fu" : "fU", Index(discriminator));
}

void ASTMangler::appendDefaultArgumentEntity(const DeclContext *func,
                                             unsigned index) {
  BaseEntitySignature base(func->getInnermostDeclarationDeclContext());
  appendContext(func, base, StringRef());
  appendOperator("fA", Index(index));
}

void ASTMangler::appendInitializerEntity(const VarDecl *var) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  BaseEntitySignature base(var);
  appendEntity(var, base, "vp", var->isStatic());
  appendOperator("fi");
}

void ASTMangler::appendBackingInitializerEntity(const VarDecl *var) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  BaseEntitySignature base(var);
  appendEntity(var, base, "vp", var->isStatic());
  appendOperator("fP");
}

void ASTMangler::appendInitFromProjectedValueEntity(const VarDecl *var) {
  llvm::SaveAndRestore X(AllowInverses, inversesAllowed(var));
  BaseEntitySignature base(var);
  appendEntity(var, base, "vp", var->isStatic());
  appendOperator("fW");
}

/// Is this declaration a method for mangling purposes? If so, we'll leave the
/// Self type out of its mangling.
static bool isMethodDecl(const Decl *decl) {
  return isa<AbstractFunctionDecl>(decl)
    && decl->getDeclContext()->isTypeContext();
}

CanType ASTMangler::getDeclTypeForMangling(
                                       const ValueDecl *decl,
                                       GenericSignature &genericSig,
                                       GenericSignature &parentGenericSig) {
  genericSig = GenericSignature();
  parentGenericSig = GenericSignature();

  auto &C = decl->getASTContext();

  auto ty = decl->getInterfaceType()->getReferenceStorageReferent();
  if (ty->hasError()) {
    if (isa<AbstractFunctionDecl>(decl) || isa<EnumElementDecl>(decl) ||
        isa<SubscriptDecl>(decl)) {
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      CanFunctionType::ExtInfo info;
      return CanFunctionType::get({AnyFunctionType::Param(C.TheErrorType)},
                                  C.TheErrorType, info);
    }
    return C.TheErrorType;
  }

  // If this declaration predates concurrency, adjust its type to not
  // contain type features that were not available pre-concurrency. This
  // cannot alter the ABI in any way.
  if (decl->preconcurrency()) {
    ty = ty->stripConcurrency(/*recurse=*/true, /*dropGlobalActor=*/true);
  }

  auto canTy = ty->getCanonicalType();

  if (auto gft = dyn_cast<GenericFunctionType>(canTy)) {
    genericSig = gft.getGenericSignature();

    canTy = CanFunctionType::get(gft.getParams(), gft.getResult(),
                                 gft->getExtInfo());
  }

  if (!canTy->hasError()) {
    // Shed the 'self' type and generic requirements from method manglings.
    if (isMethodDecl(decl)) {
      // Drop the Self argument clause from the type.
      canTy = cast<AnyFunctionType>(canTy).getResult();
    }

    if (isMethodDecl(decl) || isa<SubscriptDecl>(decl))
      parentGenericSig = decl->getDeclContext()->getGenericSignatureOfContext();
  }

  return canTy;
}

void ASTMangler::appendDeclType(const ValueDecl *decl,
                                BaseEntitySignature &base,
                                FunctionManglingKind functionMangling) {
  Mod = decl->getModuleContext();
  GenericSignature genericSig;
  GenericSignature parentGenericSig;
  auto type = getDeclTypeForMangling(decl, genericSig, parentGenericSig);

  auto sig = (genericSig
              ? genericSig
              : decl->getDeclContext()->getGenericSignatureOfContext());

  if (AnyFunctionType *FuncTy = type->getAs<AnyFunctionType>()) {
    appendFunction(FuncTy, sig, functionMangling, decl,
                   false /*is recursed into*/);
  } else {
    appendType(type, sig, decl);
  }
  
  // Mangle the generic signature, if any.
  if (genericSig
      && appendGenericSignature(genericSig, parentGenericSig, base)) {
    // The 'F' function mangling doesn't need a 'u' for its generic signature.
    if (functionMangling == NoFunctionMangling)
      appendOperator("u");
  }
}

bool ASTMangler::tryAppendStandardSubstitution(const GenericTypeDecl *decl) {
  // Bail out if our parent isn't the swift standard library.
  auto dc = decl->getDeclContext();
  if (!dc->isModuleScopeContext() ||
      !dc->getParentModule()->hasStandardSubstitutions())
    return false;

  if (!AllowStandardSubstitutions)
    return false;

  if (isa<NominalTypeDecl>(decl)) {
    if (auto Subst = getStandardTypeSubst(
            decl->getName().str(), AllowConcurrencyStandardSubstitutions)) {
      if (!SubstMerging.tryMergeSubst(*this, *Subst, /*isStandardSubst*/ true)){
        appendOperator("S", *Subst);
      }
      return true;
    }
  }
  return false;
}

void ASTMangler::appendConstructorEntity(const ConstructorDecl *ctor,
                                         bool isAllocating) {
  BaseEntitySignature base(ctor);
  appendContextOf(ctor, base);
  appendDeclType(ctor, base);
  StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(ctor);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator);
    appendOperator("Ll");
  }
  appendOperator(isAllocating ? "fC" : "fc");
}

void ASTMangler::appendDestructorEntity(const DestructorDecl *dtor,
                                        bool isDeallocating) {
  BaseEntitySignature base(dtor);
  appendContextOf(dtor, base);
  appendOperator(isDeallocating ? "fD" : "fd");
}

void ASTMangler::appendAccessorEntity(StringRef accessorKindCode,
                                      const AbstractStorageDecl *decl,
                                      bool isStatic) {
  BaseEntitySignature base(decl);
  appendContextOf(decl, base);
  if (auto *varDecl = dyn_cast<VarDecl>(decl)) {
    appendDeclName(decl);
    appendDeclType(decl, base);
    appendOperator("v", accessorKindCode);
  } else if (auto *subscriptDecl = dyn_cast<SubscriptDecl>(decl)) {
    appendDeclType(decl, base);

    StringRef privateDiscriminator = getPrivateDiscriminatorIfNecessary(decl);
    if (!privateDiscriminator.empty()) {
      appendIdentifier(privateDiscriminator);
      appendOperator("Ll");
    }

    appendOperator("i", accessorKindCode);
  } else {
    llvm_unreachable("Unknown type of AbstractStorageDecl");
  }
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl,
                              BaseEntitySignature &base,
                              StringRef EntityOp,
                              bool isStatic) {
  appendContextOf(decl, base);
  appendDeclName(decl);
  appendDeclType(decl, base);
  appendOperator(EntityOp);
  if (isStatic)
    appendOperator("Z");
}

void ASTMangler::appendEntity(const ValueDecl *decl) {
  assert(!isa<ConstructorDecl>(decl));
  assert(!isa<DestructorDecl>(decl));
  
  // Handle accessors specially, they are mangled as modifiers on the accessed
  // declaration.
  if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
    return appendAccessorEntity(
        getCodeForAccessorKind(accessor->getAccessorKind()),
        accessor->getStorage(), accessor->isStatic());
  }

  if (auto storageDecl = dyn_cast<AbstractStorageDecl>(decl))
    return appendAccessorEntity("p", storageDecl, decl->isStatic());

  if (isa<GenericTypeParamDecl>(decl)) {
    BaseEntitySignature base(decl);
    return appendEntity(decl, base, "fp", decl->isStatic());
  }

  if (auto macro = dyn_cast<MacroDecl>(decl)) {
    BaseEntitySignature base(macro);
    return appendEntity(decl, base, "fm", false);
  }

  if (auto expansion = dyn_cast<MacroExpansionDecl>(decl)) {
    appendMacroExpansionContext(
        expansion->getLoc(), expansion->getDeclContext());
    appendMacroExpansionOperator(
        expansion->getMacroName().getBaseName().userFacingName(),
        MacroRole::Declaration,
        expansion->getDiscriminator());
    return;
  }

  assert(isa<AbstractFunctionDecl>(decl) || isa<EnumElementDecl>(decl));

  BaseEntitySignature base(decl);
  appendContextOf(decl, base);
  appendDeclName(decl);
  appendDeclType(decl, base, FunctionMangling);
  appendOperator("F");
  if (decl->isStatic())
    appendOperator("Z");
}

void
ASTMangler::appendProtocolConformance(const ProtocolConformance *conformance) {
  auto topLevelSubcontext =
      conformance->getDeclContext()->getModuleScopeContext();
  Mod = topLevelSubcontext->getParentModule();

  auto conformingType = conformance->getType();
  appendType(conformingType->getCanonicalType(), nullptr);

  appendProtocolName(conformance->getProtocol());

  bool needsModule = true;
  if (auto *file = dyn_cast<FileUnit>(topLevelSubcontext)) {
    if (file->getKind() == FileUnitKind::ClangModule ||
        file->getKind() == FileUnitKind::DWARFModule) {
      if (conformance->getProtocol()->hasClangNode())
        appendOperator("So");
      else
        appendOperator("SC");
      needsModule = false;
    }
  }
  if (needsModule) {
    auto *DC = conformance->getDeclContext();
    assert(DC->getAsDecl());
    appendModule(Mod, DC->getAsDecl()->getAlternateModuleName());
  }

  // If this is a non-nominal type, we're done.
  if (!conformingType->getAnyNominal())
    return;

  auto contextSig =
    conformingType->getAnyNominal()->getGenericSignatureOfContext();

  if (GenericSignature Sig = conformance->getGenericSignature()) {
    BaseEntitySignature nullBase(nullptr);
    appendGenericSignature(Sig, contextSig, nullBase);
  }
}

void ASTMangler::appendProtocolConformanceRef(
                                const RootProtocolConformance *conformance) {
  // FIXME: Symbolic reference to the protocol conformance descriptor.
  appendProtocolName(conformance->getProtocol());

  // For retroactive conformances, add a reference to the module in which the
  // conformance resides. Otherwise, use an operator to indicate which known
  // module it's associated with.
  if (!conformanceHasIdentity(conformance)) {
    // Same as "conformance module matches type", below.
    appendOperator("HP");
  } else if (isRetroactiveConformance(conformance)) {
    auto *DC = conformance->getDeclContext();
    assert(DC->getAsDecl());
    appendModule(conformance->getDeclContext()->getParentModule(),
                 DC->getAsDecl()->getAlternateModuleName());
  // Builtin conformances are always from the Swift module.
  } else if (isa<BuiltinProtocolConformance>(conformance)) {
    appendOperator("HP");
  } else if (conformance->getDeclContext()->getParentModule() ==
               conformance->getType()->getAnyNominal()->getParentModule()) {
    appendOperator("HP");
  } else {
    appendOperator("Hp");
  }
}

/// Retrieve the index of the conformance requirement indicated by the
/// conformance path entry within the given set of requirements.
static unsigned conformanceRequirementIndex(
                                      const ConformancePath::Entry &entry,
                                      ArrayRef<Requirement> requirements) {
  unsigned result = 0;
  for (const auto &req : requirements) {
    if (req.getKind() != RequirementKind::Conformance)
      continue;

    // This is an ABI compatibility hack for noncopyable generics.
    // We should have really been skipping marker protocols here all along,
    // but it's too late now, so skip Copyable and Escapable specifically.
    if (req.getProtocolDecl()->getInvertibleProtocolKind())
      continue;

    if (req.getFirstType()->isEqual(entry.first) &&
        req.getProtocolDecl() == entry.second)
      return result;

    ++result;
  }

  llvm::errs() <<"Conformance access path step is missing from requirements";
  abort();
}

void ASTMangler::appendDependentProtocolConformance(
                                            const ConformancePath &path,
                                            GenericSignature sig) {
  ProtocolDecl *currentProtocol = nullptr;
  for (const auto &entry : path) {
    // After each step, update the current protocol to refer to where we
    // are.
    SWIFT_DEFER {
      currentProtocol = entry.second;
      sig = currentProtocol->getGenericSignature();
    };

    // The first entry is the "root". Find this requirement in the generic
    // signature.
    if (!currentProtocol) {
      appendType(entry.first, sig);
      appendProtocolName(entry.second);
      auto index =
        conformanceRequirementIndex(entry,
                                    sig.getRequirements());
      // This is never an unknown index and so must be adjusted by 2 per ABI.
      appendOperator("HD", Index(index + 2));
      continue;
    }

    // Conformances are relative to the current protocol's requirement
    // signature.
    auto reqs = currentProtocol->getRequirementSignature().getRequirements();
    auto index = conformanceRequirementIndex(entry, reqs);

    // Inherited conformance.
    bool isInheritedConformance =
      entry.first->isEqual(currentProtocol->getSelfInterfaceType());
    if (isInheritedConformance) {
      appendProtocolName(entry.second);
      // For now, this is never an unknown index and so must be adjusted by 2.
      appendOperator("HI", Index(index + 2));
      continue;
    }

    // Associated conformance.
    // FIXME: Symbolic reference.
    appendType(entry.first, sig);
    appendProtocolName(entry.second);

    // For resilient protocols, the index is unknown, so we use the special
    // value 1; otherwise we adjust by 2.
    bool isResilient =
      currentProtocol->isResilient(Mod, ResilienceExpansion::Maximal);
    appendOperator("HA", Index(isResilient ? 1 : index + 2));
  }
}

void ASTMangler::appendAnyProtocolConformance(
                                           GenericSignature genericSig,
                                           CanType conformingType,
                                           ProtocolConformanceRef conformance) {
  // If we have a conformance to a marker protocol but we aren't allowed to
  // emit marker protocols, skip it.
  if (!AllowMarkerProtocols &&
      conformance.getRequirement()->isMarkerProtocol())
    return;

  if (conformingType->isTypeParameter()) {
    assert(genericSig && "Need a generic signature to resolve conformance");
    auto path = genericSig->getConformancePath(conformingType,
                                               conformance.getAbstract());
    appendDependentProtocolConformance(path, genericSig);
  } else if (auto opaqueType = conformingType->getAs<OpaqueTypeArchetypeType>()) {
    GenericSignature opaqueSignature =
        opaqueType->getDecl()->getOpaqueInterfaceGenericSignature();
    ConformancePath conformancePath =
        opaqueSignature->getConformancePath(
          opaqueType->getInterfaceType(),
          conformance.getAbstract());

    // Append the conformance path with the signature of the opaque type.
    appendDependentProtocolConformance(conformancePath, opaqueSignature);
    appendType(conformingType, genericSig);
    appendOperator("HO");
  } else if (conformance.isConcrete()) {
    appendConcreteProtocolConformance(conformance.getConcrete(), genericSig);
  } else if (conformance.isPack()) {
    appendPackProtocolConformance(conformance.getPack(), genericSig);
  } else {
    llvm::errs() << "Bad conformance in mangler: ";
    conformance.dump(llvm::errs());
    abort();
  }
}

void ASTMangler::appendConcreteProtocolConformance(
                                      const ProtocolConformance *conformance,
                                      GenericSignature sig) {
  // Conforming type.
  Type conformingType = conformance->getType();
  if (conformingType->hasArchetype())
    conformingType = conformingType->mapTypeOutOfContext();
  appendType(conformingType->getReducedType(sig), sig);

  // Protocol conformance reference.
  appendProtocolConformanceRef(conformance->getRootConformance());

  // Conditional conformance requirements.
  bool firstRequirement = true;
  forEachConditionalConformance(conformance,
    [&](Type substType, ProtocolConformanceRef substConf) -> bool {
      if (substType->hasArchetype())
        substType = substType->mapTypeOutOfContext();
      CanType canType = substType->getReducedType(sig);
      appendAnyProtocolConformance(sig, canType, substConf);
      appendListSeparator(firstRequirement);
      return false;
    });

  if (firstRequirement)
    appendOperator("y");

  appendOperator("HC");
}

void ASTMangler::appendPackProtocolConformance(
                                      const PackConformance *conformance,
                                      GenericSignature sig) {
  auto conformingType = conformance->getType();
  auto patternConformances = conformance->getPatternConformances();
  assert(conformingType->getNumElements() == patternConformances.size());

  if (conformingType->getNumElements() == 0) {
    appendOperator("y");
  } else {
    bool firstField = true;
    for (unsigned i = 0, e = conformingType->getNumElements(); i < e; ++i) {
      auto type = conformingType->getElementType(i);
      auto conf = patternConformances[i];

      if (auto *expansionTy = type->getAs<PackExpansionType>())
        type = expansionTy->getPatternType();

      appendAnyProtocolConformance(sig, type->getCanonicalType(), conf);
      appendListSeparator(firstField);
    }
  }

  appendOperator("HX");
}

void ASTMangler::appendOpParamForLayoutConstraint(LayoutConstraint layout) {
  assert(layout);
  switch (layout->getKind()) {
  case LayoutConstraintKind::UnknownLayout:
    appendOperatorParam("U");
    break;
  case LayoutConstraintKind::RefCountedObject:
    appendOperatorParam("R");
    break;
  case LayoutConstraintKind::NativeRefCountedObject:
    appendOperatorParam("N");
    break;
  case LayoutConstraintKind::Class:
    appendOperatorParam("C");
    break;
  case LayoutConstraintKind::NativeClass:
    appendOperatorParam("D");
    break;
  case LayoutConstraintKind::Trivial:
    appendOperatorParam("T");
    break;
  case LayoutConstraintKind::TrivialOfExactSize:
    if (!layout->getAlignmentInBits())
      appendOperatorParam("e", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("E", Index(layout->getTrivialSizeInBits()),
                          Index(layout->getAlignmentInBits()));
    break;
  case LayoutConstraintKind::TrivialOfAtMostSize:
    if (!layout->getAlignmentInBits())
      appendOperatorParam("m", Index(layout->getTrivialSizeInBits()));
    else
      appendOperatorParam("M", Index(layout->getTrivialSizeInBits()),
                          Index(layout->getAlignmentInBits()));
    break;
  case LayoutConstraintKind::BridgeObject:
    appendOperatorParam("B");
    break;
  case LayoutConstraintKind::TrivialStride:
    appendOperatorParam("S", Index(layout->getTrivialSizeInBits()));
    break;
  }
}

std::string ASTMangler::mangleOpaqueTypeDescriptor(const OpaqueTypeDecl *decl) {
  beginMangling();
  appendOpaqueDeclName(decl);
  appendOperator("MQ");
  return finalize();
}

std::string
ASTMangler::mangleOpaqueTypeDescriptorRecord(const OpaqueTypeDecl *decl) {
  beginMangling();
  appendOpaqueDeclName(decl);
  appendOperator("Ho");
  return finalize();
}

void ASTMangler::appendDistributedThunk(
    const AbstractFunctionDecl *thunk, bool asReference) {
  // Marker protocols cannot be checked at runtime, so there is no point
  // in recording them for distributed thunks.
  llvm::SaveAndRestore<bool> savedAllowMarkerProtocols(AllowMarkerProtocols,
                                                       false);
  // TODO: add a flag to skip class/struct information from parameter types

  BaseEntitySignature base(thunk);

  // Since computed property SILDeclRef's refer to the "originator"
  // of the thunk, we need to mangle distributed thunks of accessors
  // specially.
  if (auto *accessor = dyn_cast<AccessorDecl>(thunk)) {
    // TODO: This needs to use accessor type instead of
    //       distributed thunk after all SILDeclRefs are switched
    //       to use "originator" instead of the thunk itself.
    //
    // ```
    // beginMangling();
    // appendContextOf(thunk);
    // appendDeclName(accessor->getStorage());
    // appendDeclType(accessor, FunctionMangling);
    // appendOperator("F");
    // appendSymbolKind(SymbolKind::DistributedThunk);
    // return finalize();
    // ```
    auto *storage = accessor->getStorage();
    thunk = storage->getDistributedThunk();
    assert(thunk);
  }
  assert(isa<AbstractFunctionDecl>(thunk) &&
         "distributed thunk to mangle must be function decl");
  assert(thunk->getContextKind() == DeclContextKind::AbstractFunctionDecl);

  auto referenceInProtocolContextOrRequirement =
      [&thunk, asReference]() -> ProtocolDecl * {
    auto *DC = thunk->getDeclContext();
    if (!asReference)
      return dyn_cast_or_null<ProtocolDecl>(DC);

    if (auto extension = dyn_cast<ExtensionDecl>(DC))
      return dyn_cast_or_null<ProtocolDecl>(extension->getExtendedNominal());

    return nullptr;
  };

  if (auto *P = referenceInProtocolContextOrRequirement()) {
    appendContext(P->getDeclContext(), base,
                  thunk->getAlternateModuleName());
    appendIdentifier(Twine("$", P->getNameStr()).str());
    appendOperator("C"); // necessary for roundtrip, though we don't use it
  } else {
    appendContextOf(thunk, base);
  }

  if (auto accessor = dyn_cast<AccessorDecl>(thunk)) {
    assert(accessor->getAccessorKind() == AccessorKind::DistributedGet &&
           "Only accessors marked as _distributed_get are expected to be "
           "mangled as thunks");
    // A distributed getter is mangled as the name of its storage (i.e. "the
    // var")
    appendIdentifier(accessor->getStorage()->getBaseIdentifier().str());
  } else {
    appendIdentifier(thunk->getBaseIdentifier().str());
  }

  appendDeclType(thunk, base, FunctionMangling);
  appendOperator("F");
  appendSymbolKind(SymbolKind::DistributedThunk);
}

std::string ASTMangler::mangleDistributedThunkRef(const AbstractFunctionDecl *thunk) {
  beginMangling();
  appendDistributedThunk(thunk, /*asReference=*/true);
  return finalize();
}
std::string ASTMangler::mangleDistributedThunkRecord(const AbstractFunctionDecl *thunk) {
  beginMangling();
  appendDistributedThunk(thunk, /*asReference=*/true);
  appendSymbolKind(SymbolKind::AccessibleFunctionRecord);
  return finalize();
}
std::string ASTMangler::mangleDistributedThunk(const AbstractFunctionDecl *thunk) {
  beginMangling();
  appendDistributedThunk(thunk, /*asReference=*/false);
  return finalize();
}

void ASTMangler::appendMacroExpansionContext(
    SourceLoc loc, DeclContext *origDC
) {
  origDC = MacroDiscriminatorContext::getInnermostMacroContext(origDC);
  BaseEntitySignature nullBase(nullptr);

  if (loc.isInvalid())
    return appendContext(origDC, nullBase, StringRef());

  ASTContext &ctx = origDC->getASTContext();
  SourceManager &sourceMgr = ctx.SourceMgr;

  auto appendMacroExpansionLoc = [&]() {
    appendIdentifier(origDC->getParentModule()->getName().str());

    auto *SF = origDC->getParentSourceFile();
    appendIdentifier(llvm::sys::path::filename(SF->getFilename()));

    auto lineColumn = sourceMgr.getLineAndColumnInBuffer(loc);
    appendOperator("fMX", Index(lineColumn.first), Index(lineColumn.second));
  };

  auto bufferID = sourceMgr.findBufferContainingLoc(loc);
  auto generatedSourceInfo = sourceMgr.getGeneratedSourceInfo(bufferID);
  if (!generatedSourceInfo) {
    return appendMacroExpansionLoc();
  }

  SourceLoc outerExpansionLoc;
  DeclContext *outerExpansionDC;
  DeclBaseName baseName;
  unsigned discriminator;

  // Determine the macro role.
  MacroRole role;
  switch (generatedSourceInfo->kind) {
#define MACRO_ROLE(Name, Description)               \
  case GeneratedSourceInfo::Name##MacroExpansion: \
    role = MacroRole::Name;                       \
    break;
#include "swift/Basic/MacroRoles.def"

  case GeneratedSourceInfo::PrettyPrinted:
  case GeneratedSourceInfo::ReplacedFunctionBody:
  case GeneratedSourceInfo::DefaultArgument:
    return appendMacroExpansionLoc();
  }
  
  switch (generatedSourceInfo->kind) {
  // Freestanding macros
#define FREESTANDING_MACRO_ROLE(Name, Description) \
  case GeneratedSourceInfo::Name##MacroExpansion:
#define ATTACHED_MACRO_ROLE(Name, Description, MangledChar)
#include "swift/Basic/MacroRoles.def"
  {
    auto parent = ASTNode::getFromOpaqueValue(generatedSourceInfo->astNode);
    if (auto expr =
            cast_or_null<MacroExpansionExpr>(parent.dyn_cast<Expr *>())) {
      outerExpansionLoc = expr->getLoc();
      baseName = expr->getMacroName().getBaseName();
      discriminator = expr->getDiscriminator();
      outerExpansionDC = expr->getDeclContext();
    } else {
      auto decl = cast<MacroExpansionDecl>(parent.get<Decl *>());
      outerExpansionLoc = decl->getLoc();
      baseName = decl->getMacroName().getBaseName();
      discriminator = decl->getDiscriminator();
      outerExpansionDC = decl->getDeclContext();
    }
    break;
  }

  // Attached macros
#define FREESTANDING_MACRO_ROLE(Name, Description)
#define ATTACHED_MACRO_ROLE(Name, Description, MangledChar)      \
    case GeneratedSourceInfo::Name##MacroExpansion:
#include "swift/Basic/MacroRoles.def"
  {
    auto decl = ASTNode::getFromOpaqueValue(generatedSourceInfo->astNode)
      .get<Decl *>();
    auto attr = generatedSourceInfo->attachedMacroCustomAttr;
    outerExpansionLoc = decl->getLoc();
    outerExpansionDC = decl->getDeclContext();

    if (auto *macroDecl = decl->getResolvedMacro(attr))
      baseName = macroDecl->getBaseName();
    else
      baseName = ctx.getIdentifier("__unknown_macro__");

    discriminator = decl->getAttachedMacroDiscriminator(baseName, role, attr);
    break;
  }

  case GeneratedSourceInfo::PrettyPrinted:
  case GeneratedSourceInfo::ReplacedFunctionBody:
  case GeneratedSourceInfo::DefaultArgument:
    llvm_unreachable("Exited above");
  }

  // If we hit the point where the structure is represented as a DeclContext,
  // we're done.
  if (origDC->isChildContextOf(outerExpansionDC))
    return appendMacroExpansionLoc();

  // Append our own context and discriminator.
  appendMacroExpansionContext(outerExpansionLoc, origDC);
  appendMacroExpansionOperator(
      baseName.userFacingName(), role, discriminator);
}

void ASTMangler::appendMacroExpansionOperator(
    StringRef macroName, MacroRole role, unsigned discriminator
) {
  appendIdentifier(macroName);

  switch (role) {
#define FREESTANDING_MACRO_ROLE(Name, Description) case MacroRole::Name:
#define ATTACHED_MACRO_ROLE(Name, Description, MangledChar)
#include "swift/Basic/MacroRoles.def"
    appendOperator("fMf", Index(discriminator));
    break;

#define FREESTANDING_MACRO_ROLE(Name, Description)
#define ATTACHED_MACRO_ROLE(Name, Description, MangledChar) \
  case MacroRole::Name:                                     \
    appendOperator("fM" MangledChar, Index(discriminator)); \
    break;
#include "swift/Basic/MacroRoles.def"
  }
}

static StringRef getPrivateDiscriminatorIfNecessary(
      const MacroExpansionExpr *expansion) {
  auto dc = MacroDiscriminatorContext::getInnermostMacroContext(
      expansion->getDeclContext());
  auto decl = dc->getAsDecl();
  if (decl && !decl->isOutermostPrivateOrFilePrivateScope())
    return StringRef();

  // Mangle non-local private declarations with a textual discriminator
  // based on their enclosing file.
  auto topLevelSubcontext = dc->getModuleScopeContext();
  SourceFile *sf = dyn_cast<SourceFile>(topLevelSubcontext);
  if (!sf)
    return StringRef();

  Identifier discriminator =
      sf->getPrivateDiscriminator(/*createIfMissing=*/true);
  assert(!discriminator.empty());
  assert(!isNonAscii(discriminator.str()) &&
         "discriminator contains non-ASCII characters");
  (void)&isNonAscii;
  assert(!clang::isDigit(discriminator.str().front()) &&
         "not a valid identifier");
  return discriminator.str();
}

static StringRef getPrivateDiscriminatorIfNecessary(
    const FreestandingMacroExpansion *expansion) {
  switch (expansion->getFreestandingMacroKind()) {
  case FreestandingMacroKind::Expr:
    return getPrivateDiscriminatorIfNecessary(
        cast<MacroExpansionExpr>(expansion));
  case FreestandingMacroKind::Decl:
    return getPrivateDiscriminatorIfNecessary(
        cast<Decl>(cast<MacroExpansionDecl>(expansion)));
  }
}

std::string
ASTMangler::mangleMacroExpansion(const FreestandingMacroExpansion *expansion) {
  beginMangling();
  appendMacroExpansionContext(expansion->getPoundLoc(),
                              expansion->getDeclContext());
  auto privateDiscriminator = getPrivateDiscriminatorIfNecessary(expansion);
  if (!privateDiscriminator.empty()) {
    appendIdentifier(privateDiscriminator);
    appendOperator("Ll");
  }
  appendMacroExpansionOperator(
      expansion->getMacroName().getBaseName().userFacingName(),
      MacroRole::Declaration,
      expansion->getDiscriminator());
  return finalize();
}

std::string ASTMangler::mangleAttachedMacroExpansion(
    const Decl *decl, CustomAttr *attr, MacroRole role) {
  // FIXME(kavon): using the decl causes a cycle. Is a null base fine?
  BaseEntitySignature nullBase(nullptr);

  beginMangling();

  // Append the context and name of the declaration.
  // We don't mangle the declaration itself because doing so requires semantic
  // information (e.g., its interface type), which introduces cyclic
  // dependencies.
  const Decl *attachedTo = decl;
  DeclBaseName attachedToName;
  if (auto accessor = dyn_cast<AccessorDecl>(decl)) {
    auto storage = accessor->getStorage();
    appendContextOf(storage, nullBase);

    // Introduce an identifier mangling that includes var/subscript, accessor
    // kind, and static.
    // FIXME: THIS IS A HACK. We need something different.
    {
      llvm::SmallString<16> name;
      {
        llvm::raw_svector_ostream out(name);
        out << storage->getName().getBaseName().userFacingName()
            << "__";
        if (isa<VarDecl>(storage)) {
          out << "v";
        } else {
          assert(isa<SubscriptDecl>(storage));
          out << "i";
        }

        out << getCodeForAccessorKind(accessor->getAccessorKind());
        if (storage->isStatic())
          out << "Z";
      }

      attachedToName = decl->getASTContext().getIdentifier(name);
    }

    appendDeclName(storage, attachedToName);

    // For member attribute macros, the attribute is attached to the enclosing
    // declaration.
    if (role == MacroRole::MemberAttribute) {
      attachedTo = storage->getDeclContext()->getAsDecl();
    }
  } else if (auto valueDecl = dyn_cast<ValueDecl>(decl)) {
    appendContextOf(valueDecl, nullBase);

    // Mangle the name, replacing special names with their user-facing names.
    attachedToName = valueDecl->getName().getBaseName();
    if (attachedToName.isSpecial()) {
      attachedToName =
          decl->getASTContext().getIdentifier(attachedToName.userFacingName());
    }
    appendDeclName(valueDecl, attachedToName);

    // For member attribute macros, the attribute is attached to the enclosing
    // declaration.
    if (role == MacroRole::MemberAttribute) {
      attachedTo = decl->getDeclContext()->getAsDecl();
    }
  } else {
    appendContext(decl->getDeclContext(), nullBase, "");
    appendIdentifier("_");
  }

  // Determine the name of the macro.
  DeclBaseName macroName;
  if (auto *macroDecl = attachedTo->getResolvedMacro(attr)) {
    macroName = macroDecl->getName().getBaseName();
  } else {
    macroName = decl->getASTContext().getIdentifier("__unknown_macro__");
  }

  // FIXME: attached macro discriminators should take attachedToName into
  // account.
  appendMacroExpansionOperator(
      macroName.userFacingName(), role,
      decl->getAttachedMacroDiscriminator(macroName, role, attr));
  return finalize();
}

static void gatherExistentialRequirements(SmallVectorImpl<Requirement> &reqs,
                                          ParameterizedProtocolType *PPT) {
  auto protoTy = PPT->getBaseType();
  PPT->getRequirements(protoTy->getDecl()->getSelfInterfaceType(), reqs);
}

/// Extracts a list of inverse requirements from a PCT serving as the constraint
/// type of an existential.
static void extractExistentialInverseRequirements(
                                SmallVectorImpl<InverseRequirement> &inverses,
                                ProtocolCompositionType *PCT) {
  if (!PCT->hasInverse())
    return;

  auto &ctx = PCT->getASTContext();

  // Form a parameter referring to the existential's Self.
  auto existentialSelf =
      GenericTypeParamType::get(/*isParameterPack=*/false,
                                /*depth=*/0, /*index=*/0, ctx);

  for (auto ip : PCT->getInverses()) {
    auto *proto = ctx.getProtocol(getKnownProtocolKind(ip));
    assert(proto);
    inverses.push_back({existentialSelf, proto, SourceLoc()});
  }
}

void ASTMangler::appendConstrainedExistential(Type base, GenericSignature sig,
                                              const ValueDecl *forDecl) {
  auto layout = base->getExistentialLayout();
  appendExistentialLayout(layout, sig, forDecl);
  SmallVector<Requirement, 4> requirements;
  SmallVector<InverseRequirement, NumInvertibleProtocols> inverses;
  assert(!base->is<ProtocolType>() &&
         "plain protocol type constraint has no generalization structure");
  if (auto *PCT = base->getAs<ProtocolCompositionType>()) {
    for (auto memberTy : PCT->getMembers()) {
      if (auto *PPT = memberTy->getAs<ParameterizedProtocolType>())
        gatherExistentialRequirements(requirements, PPT);
    }

    if (AllowInverses)
      extractExistentialInverseRequirements(inverses, PCT);

  } else {
    auto *PPT = base->castTo<ParameterizedProtocolType>();
    gatherExistentialRequirements(requirements, PPT);
  }

  assert(requirements.size() + inverses.size() > 0
          && "Unconstrained existential?");
  // Sort the requirements to canonicalize their order.
  llvm::array_pod_sort(
      requirements.begin(), requirements.end(),
      [](const Requirement *lhs, const Requirement *rhs) -> int {
        return lhs->compare(*rhs);
      });
  llvm::array_pod_sort(
      inverses.begin(), inverses.end(),
      [](const InverseRequirement *lhs, const InverseRequirement *rhs) -> int {
        return lhs->compare(*rhs);
      });

  bool firstRequirement = true;
  for (const auto &reqt : requirements) {
    switch (reqt.getKind()) {
    case RequirementKind::SameShape:
      llvm_unreachable("Same-shape requirement not supported here");
    case RequirementKind::Layout:
    case RequirementKind::Conformance:
    case RequirementKind::Superclass:
      // The surface language cannot express these requirements yet, so
      // we have no mangling for them.
      assert(false && "Unexpected requirement in constrained existential!");
      continue;

    case RequirementKind::SameType: {
      break;
    }
    }

    appendRequirement(reqt, sig, /*baseIsProtocolSelf*/ true);
    appendListSeparator(firstRequirement);
  }

  for (const auto invReq : inverses) {
    appendInverseRequirement(invReq, sig, /*baseIsProtocolSelf*/ true);
    appendListSeparator(firstRequirement);
  }

  return appendOperator("XP");
}

/// Determine whether this declaration can only occur within the primary
/// type definition.
static bool canOnlyOccurInPrimaryTypeDefinition(const Decl *decl) {
  // Enum elements always occur within the primary definition.
  if (isa<EnumElementDecl>(decl))
    return true;

  return false;
}

/// When the immediate enclosing context of this declaration is
/// a generic type (with its own generic parameters), return the depth of
/// the innermost generic parameters.
static std::optional<unsigned> getEnclosingTypeGenericDepth(const Decl *decl) {
  auto typeDecl = dyn_cast<GenericTypeDecl>(decl->getDeclContext());
  if (!typeDecl)
    return std::nullopt;

  if (!typeDecl->isGeneric())
    return std::nullopt;

  return typeDecl->getGenericSignature()->getMaxDepth();
}

ASTMangler::BaseEntitySignature::BaseEntitySignature(const Decl *decl)
    : sig(nullptr), innermostTypeDecl(true), extension(false),
      mangledDepth(std::nullopt) {
  if (decl) {
    switch (decl->getKind()) {
    case DeclKind::Var:
    case DeclKind::Subscript:
    case DeclKind::Constructor:
    case DeclKind::Destructor:
    case DeclKind::Func:
    case DeclKind::Accessor:
    case DeclKind::Enum:
    case DeclKind::Struct:
    case DeclKind::Class:
    case DeclKind::EnumElement:
      sig = decl->getInnermostDeclContext()->getGenericSignatureOfContext();

      // Protocol members never mangle inverse constraints on `Self`.
      if (isa<ProtocolDecl>(decl->getDeclContext()))
        setDepth(0);

      // Declarations that can only occur in the primary type definition should
      // not mangle inverses for the generic parameters of that type definition.
      // This allows types to introduce conditional conformances to invertible
      // protocols without breaking ABI.
      if (canOnlyOccurInPrimaryTypeDefinition(decl)) {
        if (auto depth = getEnclosingTypeGenericDepth(decl))
          suppressedInnermostDepth = depth;
      }

      break;

    case DeclKind::TypeAlias: // FIXME: is this right? typealiases have a generic signature!
    case DeclKind::PatternBinding:
    case DeclKind::Protocol:
    case DeclKind::BuiltinTuple:
    case DeclKind::OpaqueType:
    case DeclKind::GenericTypeParam:
    case DeclKind::AssociatedType:
    case DeclKind::Module:
    case DeclKind::Param:
    case DeclKind::Macro:
    case DeclKind::Extension:
    case DeclKind::TopLevelCode:
    case DeclKind::Import:
    case DeclKind::IfConfig:
    case DeclKind::PoundDiagnostic:
    case DeclKind::PrecedenceGroup:
    case DeclKind::Missing:
    case DeclKind::MissingMember:
    case DeclKind::EnumCase:
    case DeclKind::InfixOperator:
    case DeclKind::PrefixOperator:
    case DeclKind::PostfixOperator:
    case DeclKind::MacroExpansion:
      break;
    };
  }
}
