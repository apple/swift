//===--- Builtins.cpp - Swift Language Builtin ASTs -----------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2022 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//
//
//  This file implements the interface to the Builtin APIs.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Builtins.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/ASTSynthesis.h"
#include "swift/AST/FileUnit.h"
#include "swift/AST/Module.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/TypeCheckRequests.h"
#include "swift/AST/Types.h"
#include "swift/Strings.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/ManagedStatic.h"
#include <tuple>

using namespace swift;

struct BuiltinExtraInfoTy {
  const char *Attributes;
};

static const BuiltinExtraInfoTy BuiltinExtraInfo[] = {
    {nullptr},
#define BUILTIN(Id, Name, Attrs) {Attrs},
#include "swift/AST/Builtins.def"
};

bool BuiltinInfo::isReadNone() const {
  return strchr(BuiltinExtraInfo[(unsigned)ID].Attributes, 'n') != nullptr;
}

const llvm::AttributeList &
IntrinsicInfo::getOrCreateAttributes(ASTContext &Ctx) const {
  using DenseMapInfo = llvm::DenseMapInfo<llvm::AttributeList>;
  if (DenseMapInfo::isEqual(Attrs, DenseMapInfo::getEmptyKey())) {
    Attrs = llvm::Intrinsic::getAttributes(Ctx.getIntrinsicScratchContext(), ID);
  }
  return Attrs;
}

Type swift::getBuiltinType(ASTContext &Context, StringRef Name) {
  // Vectors are VecNxT, where "N" is the number of elements and
  // T is the element type.
  if (Name.startswith("Vec")) {
    Name = Name.substr(3);
    StringRef::size_type xPos = Name.find('x');
    if (xPos == StringRef::npos)
      return Type();

    unsigned numElements;
    if (Name.substr(0, xPos).getAsInteger(10, numElements) ||
        numElements == 0 || numElements > 1024)
      return Type();

    Type elementType = getBuiltinType(Context, Name.substr(xPos + 1));
    if (!elementType)
      return Type();

    return BuiltinVectorType::get(Context, elementType, numElements);
  }

  if (Name == "RawPointer")
    return Context.TheRawPointerType;
  if (Name == "RawUnsafeContinuation")
    return Context.TheRawUnsafeContinuationType;
  if (Name == "Job")
    return Context.TheJobType;
  if (Name == "DefaultActorStorage")
    return Context.TheDefaultActorStorageType;
  if (Name == "NonDefaultDistributedActorStorage")
    return Context.TheNonDefaultDistributedActorStorageType;
  if (Name == "Executor")
    return Context.TheExecutorType;
  if (Name == "NativeObject")
    return Context.TheNativeObjectType;
  if (Name == "BridgeObject")
    return Context.TheBridgeObjectType;
  if (Name == "SILToken")
    return Context.TheSILTokenType;
  if (Name == "UnsafeValueBuffer")
    return Context.TheUnsafeValueBufferType;
  if (Name == "PackIndex")
    return Context.ThePackIndexType;
  
  if (Name == "FPIEEE32")
    return Context.TheIEEE32Type;
  if (Name == "FPIEEE64")
    return Context.TheIEEE64Type;

  if (Name == "Word")
    return BuiltinIntegerType::getWordType(Context);

  if (Name == "IntLiteral")
    return Context.TheIntegerLiteralType;

  // Handle 'int8' and friends.
  if (Name.substr(0, 3) == "Int") {
    unsigned BitWidth;
    if (!Name.substr(3).getAsInteger(10, BitWidth) &&
        BitWidth <= 2048 && BitWidth != 0)  // Cap to prevent insane things.
      return BuiltinIntegerType::get(BitWidth, Context);
  }
  
  // Target specific FP types.
  if (Name == "FPIEEE16")
    return Context.TheIEEE16Type;
  if (Name == "FPIEEE80")
    return Context.TheIEEE80Type;
  if (Name == "FPIEEE128")
    return Context.TheIEEE128Type;
  if (Name == "FPPPC128")
    return Context.ThePPC128Type;

  // AnyObject is the empty class-constrained existential.
  if (Name == "AnyObject")
    return CanType(
      ProtocolCompositionType::get(Context, {},
                                   /*HasExplicitAnyObject=*/true));

  return Type();
}

/// getBuiltinBaseName - Decode the type list of a builtin (e.g. mul_Int32) and
/// return the base name (e.g. "mul").
StringRef swift::getBuiltinBaseName(ASTContext &C, StringRef Name,
                                    SmallVectorImpl<Type> &Types) {
  // builtin-id ::= operation-id ('_' type-id)*
  for (StringRef::size_type Underscore = Name.find_last_of('_');
       Underscore != StringRef::npos; Underscore = Name.find_last_of('_')) {
    
    // Check that the type parameter is well-formed and set it up for returning.
    // This allows operations with underscores in them, like "icmp_eq".
    Type Ty = getBuiltinType(C, Name.substr(Underscore + 1));
    if (Ty.isNull()) break;
    
    Types.push_back(Ty);
    
    Name = Name.substr(0, Underscore);
  }
  
  std::reverse(Types.begin(), Types.end());
  return Name;
}

namespace {

/// AST synthesizers (see ASTSynthesis.h for the general pattern)
/// for generics.
enum UnrestrictedGenericParam { _unrestricted };

/// A synthesizer which generates a conformance requirement.
template <class TypeS, class ProtocolS>
struct ConformsToSynthesizer {
  TypeS Type;
  ProtocolS Protocol;
};
template <class TypeS, class ProtocolS>
constexpr ConformsToSynthesizer<TypeS, ProtocolS>
_conformsTo(TypeS type, ProtocolS protocol) {
  return {type, protocol};
}

/// A synthesizer which generates a layout constraint requirement.
template <class TypeS>
struct LayoutConstraintSynthesizer {
  TypeS Type;
  LayoutConstraint Constraint;
};
template <class TypeS>
LayoutConstraintSynthesizer<TypeS>
_layout(TypeS type, LayoutConstraint constraint) {
  return {type, constraint};
}
static LayoutConstraint _classLayout() {
  return LayoutConstraint::getLayoutConstraint(LayoutConstraintKind::Class);
}

/// A synthesizer which generates a generic parameter list.
template <class... ParamS>
struct GenericParamListSynthesizer {
  VariadicSynthesizerStorage<ParamS...> Params;
};
template <class... ParamS>
constexpr GenericParamListSynthesizer<ParamS...>
_generics(ParamS... params) {
  return {{params...}};
}

struct CountGenericParameters {
  unsigned &Count;

  void operator()(UnrestrictedGenericParam _) const {
    Count++;
  }

  template <class TypeS, class ProtoS>
  void operator()(const ConformsToSynthesizer<TypeS, ProtoS> &_) const {
    // not a parameter
  }

  template <class TypeS>
  void operator()(const LayoutConstraintSynthesizer<TypeS> &_) const {
    // not a parameter
  }
};

} // end anonymous namespace

static const char * const GenericParamNames[] = {
  "T",
  "U",
  "V",
  "W",
  "X",
  "Y",
  "Z"
};

static GenericTypeParamDecl*
createGenericParam(ASTContext &ctx, const char *name, unsigned index) {
  ModuleDecl *M = ctx.TheBuiltinModule;
  Identifier ident = ctx.getIdentifier(name);
  return GenericTypeParamDecl::createImplicit(
      &M->getMainFile(FileUnitKind::Builtin), ident, /*depth*/ 0, index);
}

/// Create a generic parameter list with multiple generic parameters.
static GenericParamList *getGenericParams(ASTContext &ctx,
                                          unsigned numParameters) {
  assert(numParameters <= std::size(GenericParamNames));

  SmallVector<GenericTypeParamDecl *, 2> genericParams;
  for (unsigned i = 0; i != numParameters; ++i)
    genericParams.push_back(createGenericParam(ctx, GenericParamNames[i], i));

  auto paramList = GenericParamList::create(ctx, SourceLoc(), genericParams,
                                            SourceLoc());
  return paramList;
}

template <class... ParamsS>
static GenericParamList *synthesizeGenericParamList(SynthesisContext &SC,
                   const GenericParamListSynthesizer<ParamsS...> &params) {
  unsigned count = 0;
  params.Params.visit(CountGenericParameters{count});
  auto paramList = getGenericParams(SC.Context, count);
  SC.GenericParams = paramList;
  return paramList;
}

namespace {
struct CollectGenericParams {
  SynthesisContext &SC;
  SmallVector<GenericTypeParamType *, 2> GenericParamTypes;
  SmallVector<Requirement, 2> AddedRequirements;

  CollectGenericParams(SynthesisContext &SC) : SC(SC) {
    for (auto gp : SC.GenericParams->getParams()) {
      GenericParamTypes.push_back(
          gp->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
    }
  }

  void operator()(UnrestrictedGenericParam _) {}

  template <class TypeS, class ProtoS>
  void operator()(const ConformsToSynthesizer<TypeS, ProtoS> &conf) {
    auto type = synthesizeType(SC, conf.Type);
    auto protocolType = synthesizeType(SC, conf.Protocol);
    AddedRequirements.push_back({RequirementKind::Conformance,
                                 type, protocolType});
  }

  template <class TypeS>
  void operator()(const LayoutConstraintSynthesizer<TypeS> &req) {
    auto type = synthesizeType(SC, req.Type);
    AddedRequirements.push_back({RequirementKind::Layout,
                                 type, req.Constraint});
  }
};

} // end anonymous namespace

template <class... ParamsS>
static GenericSignature
synthesizeGenericSignature(SynthesisContext &SC,
                     const GenericParamListSynthesizer<ParamsS...> &list) {
  assert(SC.GenericParams && "synthesizeGenericParamList not called first");
  CollectGenericParams collector(SC);
  list.Params.visit(collector);

  return buildGenericSignature(SC.Context,
                               GenericSignature(),
                               std::move(collector.GenericParamTypes),
                               std::move(collector.AddedRequirements));
}

/// Build a builtin function declaration.
///
/// This is a "legacy" interface; you should probably use one of
/// the templated overloads below.
static FuncDecl *
getBuiltinFunction(Identifier Id, ArrayRef<Type> argTypes, Type ResType) {
  auto &Context = ResType->getASTContext();
  
  ModuleDecl *M = Context.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);

  SmallVector<ParamDecl*, 4> params;
  for (Type argType : argTypes) {
    auto PD = new (Context) ParamDecl(SourceLoc(), SourceLoc(),
                                      Identifier(), SourceLoc(), Identifier(), DC);
    PD->setSpecifier(ParamSpecifier::Default);
    PD->setInterfaceType(argType);
    PD->setImplicit();
    params.push_back(PD);
  }

  auto *paramList = ParameterList::create(Context, params);
  
  DeclName Name(Context, Id, paramList);
  auto *const FD = FuncDecl::createImplicit(
      Context, StaticSpellingKind::None, Name, /*NameLoc=*/SourceLoc(),
      /*Async=*/false, /*Throws=*/false,
      /*GenericParams=*/nullptr, paramList, ResType, DC);
  FD->setAccess(AccessLevel::Public);
  return FD;
}

template <class ExtInfoS, class ParamsS, class ResultS>
static FuncDecl *
getBuiltinFunctionImpl(SynthesisContext &SC, Identifier id,
                       GenericParamList *genericParams,
                       GenericSignature signature,
                       const ExtInfoS &extInfoS,
                       const ParamsS &paramsS,
                       const ResultS &resultS) {
  auto params = synthesizeParameterList(SC, paramsS);
  auto extInfo = synthesizeExtInfo(SC, extInfoS);
  auto resultType = synthesizeType(SC, resultS);

  DeclName name(SC.Context, id, params);
  auto *FD = FuncDecl::createImplicit(
      SC.Context, StaticSpellingKind::None, name, /*NameLoc=*/SourceLoc(),
      extInfo.isAsync(), extInfo.isThrowing(),
      genericParams, params, resultType, SC.DC);
  FD->setAccess(AccessLevel::Public);
  FD->setGenericSignature(signature);
  return FD;
}

/// Synthesize a non-generic builtin function declaration.
template <class ExtInfoS, class ParamsS, class ResultS>
static FuncDecl *
getBuiltinFunction(ASTContext &ctx, Identifier id,
                   const ExtInfoS &extInfoS, const ParamsS &paramsS,
                   const ResultS &resultS) {
  ModuleDecl *M = ctx.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);

  SynthesisContext SC(ctx, DC);
  return getBuiltinFunctionImpl(SC, id, nullptr, GenericSignature(),
                                extInfoS, paramsS, resultS);
}

/// Synthesize a generic builtin function declaration.
template <class ExtInfoS, class GenericsS, class ParamsS, class ResultS>
static FuncDecl *
getBuiltinFunction(ASTContext &ctx, Identifier id,
                   const ExtInfoS &extInfoS,
                   const GenericsS &genericsS,
                   const ParamsS &paramsS,
                   const ResultS &resultS) {
  ModuleDecl *M = ctx.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);

  SynthesisContext SC(ctx, DC);
  auto genericParamList = synthesizeGenericParamList(SC, genericsS);
  auto genericSignature = synthesizeGenericSignature(SC, genericsS);

  return getBuiltinFunctionImpl(SC, id, genericParamList, genericSignature,
                                extInfoS, paramsS, resultS);
}

namespace {

enum class BuiltinThrowsKind : uint8_t {
  None,
  Throws,
  Rethrows
};

}

/// Build a builtin function declaration.
static FuncDecl *
getBuiltinGenericFunction(Identifier Id,
                          ArrayRef<AnyFunctionType::Param> ArgParamTypes,
                          Type ResType,
                          GenericParamList *GenericParams,
                          GenericSignature Sig,
                          bool Async, BuiltinThrowsKind Throws) {
  assert(GenericParams && "Missing generic parameters");
  auto &Context = ResType->getASTContext();

  ModuleDecl *M = Context.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);

  SmallVector<ParamDecl*, 4> params;
  for (unsigned i = 0, e = ArgParamTypes.size(); i < e; ++i) {
    auto paramIfaceType = ArgParamTypes[i].getPlainType();
    auto specifier =
      ParamDecl::getParameterSpecifierForValueOwnership(
        ArgParamTypes[i].getParameterFlags().getValueOwnership());
    auto PD = new (Context) ParamDecl(SourceLoc(), SourceLoc(),
                                      Identifier(), SourceLoc(),
                                      Identifier(), DC);
    PD->setSpecifier(specifier);
    PD->setInterfaceType(paramIfaceType);
    PD->setImplicit();
    params.push_back(PD);
  }

  auto *paramList = ParameterList::create(Context, params);

  DeclName Name(Context, Id, paramList);
  auto *const func = FuncDecl::createImplicit(
      Context, StaticSpellingKind::None, Name,
      /*NameLoc=*/SourceLoc(),
      Async,
      Throws != BuiltinThrowsKind::None,
      GenericParams, paramList, ResType, DC);

  func->setAccess(AccessLevel::Public);
  func->setGenericSignature(Sig);
  if (Throws == BuiltinThrowsKind::Rethrows)
    func->getAttrs().add(new (Context) RethrowsAttr(/*ThrowsLoc*/ SourceLoc()));

  return func;
}

/// Build a getelementptr operation declaration.
static ValueDecl *getGepRawOperation(ASTContext &ctx,
                                     Identifier id, Type argType) {
  // This is always "(i8*, IntTy) -> i8*"
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer, argType),
                            _rawPointer);
}

static ValueDecl *getStringObjectOrOperation(ASTContext &ctx,
                                             Identifier id, Type argType) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(argType, argType),
                            argType);
}

/// Build a binary operation declaration.
static ValueDecl *getBinaryOperation(ASTContext &ctx,
                                     Identifier id, Type argType) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(argType, argType),
                            argType);
}

/// Build a declaration for a binary operation with overflow.
static ValueDecl *getBinaryOperationWithOverflow(ASTContext &ctx,
                                                 Identifier id,
                                                 Type argType) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(argType, argType, _int(1)),
                            _tuple(argType, _int(1)));
}

static ValueDecl *getUnaryOperation(ASTContext &ctx, Identifier id,
                                    Type argType) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(argType), argType);
}

/// Build a binary predicate declaration.
static ValueDecl *getBinaryPredicate(ASTContext &ctx, Identifier id,
                                     Type argType) {
  if (auto vecType = argType->getAs<BuiltinVectorType>()) {
    return getBuiltinFunction(ctx, id, _thin,
                              _parameters(argType, argType),
                              _vector(vecType->getNumElements(), _int(1)));
  }

  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(argType, argType),
                            _int(1));
}

/// Build a cast.  There is some custom type checking here.
static ValueDecl *getCastOperation(ASTContext &Context, Identifier Id,
                                   BuiltinValueKind VK,
                                   ArrayRef<Type> Types) {
  if (Types.empty() || Types.size() > 2) return nullptr;
  Type Input = Types[0];
  Type Output = Types.size() == 2 ? Types[1] : Type();

  // If both types are vectors, look through the vectors.
  Type CheckInput = Input;
  Type CheckOutput = Output;
  bool UnwrappedVector = false;
  auto InputVec = Input->getAs<BuiltinVectorType>();
  auto OutputVec = Output.isNull()? nullptr :Output->getAs<BuiltinVectorType>();
  if (InputVec && OutputVec &&
      InputVec->getNumElements() == OutputVec->getNumElements()) {
    UnwrappedVector = true;
    CheckInput = InputVec->getElementType();
    CheckOutput = OutputVec->getElementType();
  }

  // Custom type checking.  We know the one or two types have been subjected to
  // the "isBuiltinTypeOverloaded" predicate successfully.
  switch (VK) {
  default: llvm_unreachable("Not a cast operation");

  case BuiltinValueKind::Trunc:
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinIntegerType>() ||
        !CheckOutput->is<BuiltinIntegerType>() ||
        CheckInput->castTo<BuiltinIntegerType>()->getLeastWidth() <=
          CheckOutput->castTo<BuiltinIntegerType>()->getGreatestWidth())
      return nullptr;
    break;
  case BuiltinValueKind::TruncOrBitCast:
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinIntegerType>() ||
        !CheckOutput->is<BuiltinIntegerType>() ||
        CheckInput->castTo<BuiltinIntegerType>()->getLeastWidth() <
          CheckOutput->castTo<BuiltinIntegerType>()->getGreatestWidth())
      return nullptr;
    break;
      
  case BuiltinValueKind::ZExt:
  case BuiltinValueKind::SExt: {
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinIntegerType>() ||
        !CheckOutput->is<BuiltinIntegerType>() ||
        CheckInput->castTo<BuiltinIntegerType>()->getGreatestWidth() >=
          CheckOutput->castTo<BuiltinIntegerType>()->getLeastWidth())
      return nullptr;
    break;
  }
  case BuiltinValueKind::ZExtOrBitCast:
  case BuiltinValueKind::SExtOrBitCast: {
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinIntegerType>() ||
        !CheckOutput->is<BuiltinIntegerType>() ||
        CheckInput->castTo<BuiltinIntegerType>()->getGreatestWidth() >
          CheckOutput->castTo<BuiltinIntegerType>()->getLeastWidth())
      return nullptr;
    break;
  }

  case BuiltinValueKind::FPToUI:
  case BuiltinValueKind::FPToSI:
    if (CheckOutput.isNull() || !CheckInput->is<BuiltinFloatType>() ||
        !CheckOutput->is<BuiltinIntegerType>())
      return nullptr;
    break;

  case BuiltinValueKind::UIToFP:
  case BuiltinValueKind::SIToFP:
    if (CheckOutput.isNull() || !CheckInput->is<BuiltinIntegerType>() ||
        !CheckOutput->is<BuiltinFloatType>())
      return nullptr;
    break;

  case BuiltinValueKind::FPTrunc:
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinFloatType>() ||
        !CheckOutput->is<BuiltinFloatType>() ||
        CheckInput->castTo<BuiltinFloatType>()->getFPKind() <=
        CheckOutput->castTo<BuiltinFloatType>()->getFPKind())
      return nullptr;
    break;
  case BuiltinValueKind::FPExt:
    if (CheckOutput.isNull() ||
        !CheckInput->is<BuiltinFloatType>() ||
        !CheckOutput->is<BuiltinFloatType>() ||
        CheckInput->castTo<BuiltinFloatType>()->getFPKind() >=
        CheckOutput->castTo<BuiltinFloatType>()->getFPKind())
      return nullptr;
    break;

  case BuiltinValueKind::PtrToInt:
    // FIXME: Do we care about vectors of pointers?
    if (!CheckOutput.isNull() || !CheckInput->is<BuiltinIntegerType>() ||
        UnwrappedVector)
      return nullptr;
    Output = Input;
    Input = Context.TheRawPointerType;
    break;
  case BuiltinValueKind::IntToPtr:
    // FIXME: Do we care about vectors of pointers?
    if (!CheckOutput.isNull() || !CheckInput->is<BuiltinIntegerType>() ||
        UnwrappedVector)
      return nullptr;
    Output = Context.TheRawPointerType;
    break;
  case BuiltinValueKind::BitCast:
    if (CheckOutput.isNull()) return nullptr;

    // Support float <-> int bitcast where the types are the same widths.
    if (auto *BIT = CheckInput->getAs<BuiltinIntegerType>())
      if (auto *BFT = CheckOutput->getAs<BuiltinFloatType>())
        if (BIT->isFixedWidth() && BIT->getFixedWidth() == BFT->getBitWidth())
            break;
    if (auto *BFT = CheckInput->getAs<BuiltinFloatType>())
      if (auto *BIT = CheckOutput->getAs<BuiltinIntegerType>())
        if (BIT->isFixedWidth() && BIT->getFixedWidth() == BFT->getBitWidth())
          break;
      
    // Support VecNxInt1 -> IntN bitcast for SIMD comparison results.
    if (auto *Vec = CheckInput->getAs<BuiltinVectorType>())
      if (auto *BIT = CheckOutput->getAs<BuiltinIntegerType>())
        if (auto *Element = Vec->getElementType()->getAs<BuiltinIntegerType>())
          if (Element->getFixedWidth() == 1 &&
              BIT->isFixedWidth() &&
              BIT->getFixedWidth() == Vec->getNumElements())
            break;
    // And IntN -> VecNxInt1 for SIMDMask random generators.
    if (auto *Vec = CheckOutput->getAs<BuiltinVectorType>())
      if (auto *BIT = CheckInput->getAs<BuiltinIntegerType>())
        if (auto *Element = Vec->getElementType()->getAs<BuiltinIntegerType>())
          if (Element->getFixedWidth() == 1 &&
              BIT->isFixedWidth() &&
              BIT->getFixedWidth() == Vec->getNumElements())
            break;

    // FIXME: Implement bitcast typechecking.
    llvm_unreachable("Bitcast not supported yet!");
    return nullptr;
  }

  return getBuiltinFunction(Context, Id, _thin,
                            _parameters(Input),
                            Output);
}

namespace {
  class BuiltinFunctionBuilder {
  public:
    ASTContext &Context;

  private:
    GenericParamList *TheGenericParamList;
    SmallVector<AnyFunctionType::Param, 4> InterfaceParams;
    Type InterfaceResult;
    bool Async = false;
    BuiltinThrowsKind Throws = BuiltinThrowsKind::None;

    // Accumulate params and requirements here, so that we can call
    // `buildGenericSignature()` when `build()` is called.
    SmallVector<GenericTypeParamType *, 2> genericParamTypes;
    SmallVector<Requirement, 2> addedRequirements;

  public:
    BuiltinFunctionBuilder(ASTContext &ctx, unsigned numGenericParams = 1,
                           bool wantsAdditionalAnyObjectRequirement = false)
        : Context(ctx) {
      TheGenericParamList = getGenericParams(ctx, numGenericParams);
      if (wantsAdditionalAnyObjectRequirement) {
        Requirement req(RequirementKind::Conformance,
                        TheGenericParamList->getParams()[0]->getInterfaceType(),
                        ctx.getAnyObjectConstraint());
        addedRequirements.push_back(req);
      }
      for (auto gp : TheGenericParamList->getParams()) {
        genericParamTypes.push_back(
            gp->getDeclaredInterfaceType()->castTo<GenericTypeParamType>());
      }
    }

    template <class G>
    void addParameter(const G &generator,
                      ParamSpecifier ownership = ParamSpecifier::Default) {
      Type gTyIface = generator.build(*this);
      auto flags = ParameterTypeFlags().withOwnershipSpecifier(ownership);
      InterfaceParams.emplace_back(gTyIface, Identifier(), flags);
    }

    template <class G>
    void setResult(const G &generator) {
      InterfaceResult = generator.build(*this);
    }

    template <class G>
    void addConformanceRequirement(const G &generator, ProtocolDecl *proto) {
      Requirement req(RequirementKind::Conformance,
                      generator.build(*this),
                      proto->getDeclaredInterfaceType());
      addedRequirements.push_back(req);
    }

    void setAsync() {
      Async = true;
    }

    void setThrows() {
      Throws = BuiltinThrowsKind::Throws;
    }

    void setRethrows() {
      Throws = BuiltinThrowsKind::Rethrows;
    }

    FuncDecl *build(Identifier name) {
      auto GenericSig = buildGenericSignature(
          Context, GenericSignature(),
          std::move(genericParamTypes),
          std::move(addedRequirements));
      return getBuiltinGenericFunction(name, InterfaceParams,
                                       InterfaceResult,
                                       TheGenericParamList, GenericSig,
                                       Async, Throws);
    }

    // Don't use these generator classes directly; call the make{...}
    // functions which follow this class.

    struct ConcreteGenerator {
      Type TheType;
      Type build(BuiltinFunctionBuilder &builder) const {
        return TheType;
      }
    };
    struct ParameterGenerator {
      unsigned Index;
      Type build(BuiltinFunctionBuilder &builder) const {
        return builder.TheGenericParamList->getParams()[Index]
            ->getDeclaredInterfaceType();
      }
    };
    struct LambdaGenerator {
      std::function<Type(BuiltinFunctionBuilder &)> TheFunction;
      Type build(BuiltinFunctionBuilder &builder) const {
        return TheFunction(builder);
      }
    };
    template <class T>
    struct MetatypeGenerator {
      T Object;
      llvm::Optional<MetatypeRepresentation> Repr;
      Type build(BuiltinFunctionBuilder &builder) const {
        return MetatypeType::get(Object.build(builder), Repr);
      }
    };
  };
} // end anonymous namespace

static BuiltinFunctionBuilder::ConcreteGenerator
makeConcrete(Type type) {
  return { type };
}

static BuiltinFunctionBuilder::ParameterGenerator
makeGenericParam(unsigned index = 0) {
  return { index };
}

template <class... Gs>
static BuiltinFunctionBuilder::LambdaGenerator
makeTuple(const Gs & ...elementGenerators) {
  return {
    [=](BuiltinFunctionBuilder &builder) -> Type {
      TupleTypeElt elts[] = {
        elementGenerators.build(builder)...
      };
      return TupleType::get(elts, builder.Context);
    }
  };
}

template <class... Gs>
static BuiltinFunctionBuilder::LambdaGenerator
makeBoundGenericType(NominalTypeDecl *decl,
                     const Gs & ...argumentGenerators) {
  return {
    [=](BuiltinFunctionBuilder &builder) -> Type {
      Type args[] = {
        argumentGenerators.build(builder)...
      };
      return BoundGenericType::get(decl, Type(), args);
    }
  };
}

template <class T>
static BuiltinFunctionBuilder::MetatypeGenerator<T>
makeMetatype(const T &object,
             llvm::Optional<MetatypeRepresentation> repr = llvm::None) {
  return { object, repr };
}

/// Create a function with type <T> T -> ().
static ValueDecl *getRefCountingOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_typeparam(0)),
                            _void);
}

static ValueDecl *getLoadOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_rawPointer),
                            _typeparam(0));
}

static ValueDecl *getStoreOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_owned(_typeparam(0)),
                                        _rawPointer),
                            _void);
}

static ValueDecl *getDestroyOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0)),
                                        _rawPointer),
                            _void);
}

static ValueDecl *getDestroyArrayOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0)),
                                        _rawPointer,
                                        _word),
                            _void);
}

static ValueDecl *getCopyOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _generics(_unrestricted),
                            _parameters(_typeparam(0)), _typeparam(0));
}

static ValueDecl *getAssumeAlignment(ASTContext &ctx, Identifier id) {
  // This is always "(Builtin.RawPointer, Builtin.Word) -> Builtin.RawPointer"
  return getBuiltinFunction(ctx, id, _thin, _parameters(_rawPointer, _word),
                            _rawPointer);
}

static ValueDecl *getTransferArrayOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0)),
                                        _rawPointer,
                                        _rawPointer,
                                        _word),
                            _void);
}

static ValueDecl *getIsUniqueOperation(ASTContext &ctx, Identifier id) {
  // <T> (@inout T) -> Int1
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_inout(_typeparam(0))),
                            _int(1));
}

static ValueDecl *getEndCOWMutation(ASTContext &ctx, Identifier id) {
  // <T> (@inout T) -> ()
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_inout(_typeparam(0))),
                            _void);
}

static ValueDecl *getBindMemoryOperation(ASTContext &ctx, Identifier id) {
  FuncDecl *fd = getBuiltinFunction(ctx, id, _thin,
                                    _generics(_unrestricted),
                                    _parameters(_rawPointer,
                                                _word,
                                                _metatype(_typeparam(0))),
                                    _word);
  fd->getAttrs().add(new (ctx) DiscardableResultAttr(/*implicit*/true));
  return fd;
}

static ValueDecl *getRebindMemoryOperation(ASTContext &ctx, Identifier id) {
  FuncDecl *fd = getBuiltinFunction(ctx, id, _thin,
                                    _parameters(_rawPointer,
                                                _word),
                                    _word);
  fd->getAttrs().add(new (ctx) DiscardableResultAttr(/*implicit*/true));
  return fd;
}

static ValueDecl *getAllocWithTailElemsOperation(ASTContext &Context,
                                                 Identifier Id,
                                                 int NumTailTypes) {
  if (NumTailTypes < 1 ||
      1 + NumTailTypes > (int)std::size(GenericParamNames))
    return nullptr;
  BuiltinFunctionBuilder builder(Context, 1 + NumTailTypes);
  builder.addParameter(makeMetatype(makeGenericParam(0)));
  for (int Idx = 0; Idx < NumTailTypes; ++Idx) {
    builder.addParameter(makeConcrete(BuiltinIntegerType::getWordType(Context)));
    builder.addParameter(makeMetatype(makeGenericParam(Idx + 1)));
  }
  builder.setResult(makeGenericParam(0));
  return builder.build(Id);
}

static ValueDecl *getProjectTailElemsOperation(ASTContext &ctx,
                                               Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted, _unrestricted),
                            _parameters(_typeparam(0),
                                        _metatype(_typeparam(1))),
                            _rawPointer);
}

/// Build a getelementptr operation declaration.
static ValueDecl *getGepOperation(ASTContext &ctx, Identifier id,
                                  Type argType) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_rawPointer,
                                        argType,
                                        _metatype(_typeparam(0))),
                            _rawPointer);
}

static ValueDecl *getGetTailAddrOperation(ASTContext &ctx, Identifier id,
                                          Type argType) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted, _unrestricted),
                            _parameters(_rawPointer,
                                        argType,
                                        _metatype(_typeparam(0)),
                                        _metatype(_typeparam(1))),
                            _rawPointer);
}

static ValueDecl *getBeginUnpairedAccessOperation(ASTContext &ctx,
                                                  Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_rawPointer,
                                        _rawPointer,
                                        _metatype(_typeparam(0))),
                            _void);
}

static ValueDecl *
getPerformInstantaneousReadAccessOperation(ASTContext &ctx,
                                           Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_rawPointer,
                                        _metatype(_typeparam(0))),
                            _void);
}

static ValueDecl *getEndUnpairedAccessOperation(ASTContext &ctx,
                                                Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer),
                            _void);
}

static ValueDecl *getSizeOrAlignOfOperation(ASTContext &ctx,
                                            Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _word);
}

static ValueDecl *getIsPODOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _int(1));
}

static ValueDecl *getIsConcrete(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _int(1));
}

static ValueDecl *getIsBitwiseTakable(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _int(1));
}

static ValueDecl *getIsOptionalOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _int(1));
}

static ValueDecl *getIsSameMetatypeOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_existentialMetatype(_any),
                                        _existentialMetatype(_any)),
                            _int(1));
}

static ValueDecl *getAllocOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_word, _word),
                            _rawPointer);
}

static ValueDecl *getDeallocOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer, _word, _word),
                            _void);
}

static ValueDecl *getStackAllocOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_word, _word, _word),
                            _rawPointer);
}

static ValueDecl *getStackDeallocOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer),
                            _void);
}

static ValueDecl *getFenceOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(), _void);
}

static ValueDecl *getIfdefOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(), _int(1));
}

static ValueDecl *getVoidErrorOperation(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(_error), _void);
}

static ValueDecl *getUnexpectedErrorOperation(ASTContext &ctx,
                                              Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(_error), _never);
}

static ValueDecl *getCmpXChgOperation(ASTContext &ctx, Identifier id,
                                      Type T) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer, T, T),
                            _tuple(T, _int(1)));
}

static ValueDecl *getAtomicRMWOperation(ASTContext &ctx, Identifier id,
                                        Type T) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer, T),
                            T);
}

static ValueDecl *getAtomicLoadOperation(ASTContext &ctx, Identifier id,
                                         Type T) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer),
                            T);
}

static ValueDecl *getAtomicStoreOperation(ASTContext &ctx, Identifier id,
                                          Type T) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer, T),
                            _void);
}

static ValueDecl *getNativeObjectCast(ASTContext &Context, Identifier Id,
                                      BuiltinValueKind BV) {

  ParamSpecifier ownership;
  Type builtinTy;
  switch (BV) {
  case BuiltinValueKind::CastToNativeObject:
  case BuiltinValueKind::UnsafeCastToNativeObject:
  case BuiltinValueKind::CastFromNativeObject:
    builtinTy = Context.TheNativeObjectType;
    ownership = ParamSpecifier::LegacyOwned;
    break;

  case BuiltinValueKind::BridgeToRawPointer:
  case BuiltinValueKind::BridgeFromRawPointer:
    builtinTy = Context.TheRawPointerType;
    ownership = ParamSpecifier::Default;
    break;

  default:
    llvm_unreachable("unexpected kind");
  }

  BuiltinFunctionBuilder builder(Context);
  if (BV == BuiltinValueKind::CastToNativeObject ||
      BV == BuiltinValueKind::UnsafeCastToNativeObject ||
      BV == BuiltinValueKind::BridgeToRawPointer) {
    builder.addParameter(makeGenericParam(), ownership);
    builder.setResult(makeConcrete(builtinTy));
  } else {
    builder.addParameter(makeConcrete(builtinTy), ownership);
    builder.setResult(makeGenericParam());
  }
  return builder.build(Id);
}

static ValueDecl *getCastToBridgeObjectOperation(ASTContext &ctx,
                                                 Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_owned(_typeparam(0)),
                                        _word),
                            _bridgeObject);
}

static ValueDecl *getCastFromBridgeObjectOperation(ASTContext &ctx,
                                                   Identifier id,
                                                   BuiltinValueKind BV) {
  switch (BV) {
  case BuiltinValueKind::CastReferenceFromBridgeObject: {
    return getBuiltinFunction(ctx, id, _thin,
                              _generics(_unrestricted),
                              _parameters(_owned(_bridgeObject)),
                              _typeparam(0));
  }

  case BuiltinValueKind::CastBitPatternFromBridgeObject: {
    return getBuiltinFunction(ctx, id, _thin,
                              _parameters(_bridgeObject),
                              _word);
  }

  default:
    llvm_unreachable("not a cast from bridge object op");
  }
}

/// ClassifyBridgeObject has type:
///      (Builtin.BridgeObject) -> (Builtin.Int1, Builtin.Int1).
static ValueDecl *getClassifyBridgeObject(ASTContext &C, Identifier Id) {
  Type int1Ty = BuiltinIntegerType::get(1, C);
  Type resultTy = TupleType::get({
    TupleTypeElt(int1Ty, C.getIdentifier("isObjCObject")),
    TupleTypeElt(int1Ty, C.getIdentifier("isObjCTaggedPointer"))
  }, C);

  return getBuiltinFunction(Id, { C.TheBridgeObjectType }, resultTy);
}

static ValueDecl *getValueToBridgeObject(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_typeparam(0)),
                            _bridgeObject);
}

static ValueDecl *getCOWBufferForReading(ASTContext &C, Identifier Id) {
  // <T : AnyObject> T -> T
  //
  BuiltinFunctionBuilder builder(C, 1, true);
  auto T = makeGenericParam();
  builder.addParameter(T);
  builder.setResult(T);
  return builder.build(Id);
}

static ValueDecl *getTypePtrAuthDiscriminator(ASTContext &C, Identifier Id) {
  // <T : AnyObject> (T.Type) -> Int64
  BuiltinFunctionBuilder builder(C);
  builder.addParameter(makeMetatype(makeGenericParam()));
  Type Int64Ty = BuiltinIntegerType::get(64, C);
  builder.setResult(makeConcrete(Int64Ty));
  return builder.build(Id);
}

static ValueDecl *getOnFastPath(ASTContext &Context, Identifier Id) {
  return getBuiltinFunction(Id, {}, TupleType::getEmpty(Context));
}

static ValueDecl *getCastReferenceOperation(ASTContext &ctx,
                                            Identifier name) {
  // <T, U> T -> U
  // SILGen and IRGen check additional constraints during lowering.
  BuiltinFunctionBuilder builder(ctx, 2);
  builder.addParameter(makeGenericParam(0), ParamSpecifier::LegacyOwned);
  builder.setResult(makeGenericParam(1));
  return builder.build(name);
}

static ValueDecl *getReinterpretCastOperation(ASTContext &ctx,
                                              Identifier name) {
  // <T, U> T -> U
  // SILGen and IRGen check additional constraints during lowering.
  BuiltinFunctionBuilder builder(ctx, 2);
  builder.addParameter(makeGenericParam(0), ParamSpecifier::LegacyOwned);
  builder.setResult(makeGenericParam(1));
  return builder.build(name);
}

static ValueDecl *getZeroInitializerOperation(ASTContext &Context,
                                             Identifier Id) {
  // <T> () -> T
  BuiltinFunctionBuilder builder(Context);
  builder.setResult(makeGenericParam());
  return builder.build(Id);
}

static ValueDecl *getGetObjCTypeEncodingOperation(ASTContext &Context,
                                                  Identifier Id) {
  // <T> T.Type -> RawPointer
  BuiltinFunctionBuilder builder(Context);
  builder.addParameter(makeMetatype(makeGenericParam()));
  builder.setResult(makeConcrete(Context.TheRawPointerType));
  return builder.build(Id);
}

static ValueDecl *getAutoDiffApplyDerivativeFunction(
    ASTContext &Context, Identifier Id, AutoDiffDerivativeFunctionKind kind,
    unsigned arity, bool throws) {
  assert(arity >= 1);
  // JVP:
  //   <...T...(arity), R> (@differentiable(_forward) (...T) throws -> R, ...T)
  //       rethrows -> (R, (...T.TangentVector) -> R.TangentVector)
  // VJP:
  //   <...T...(arity), R> (@differentiable(reverse) (...T) throws -> R, ...T)
  //       rethrows -> (R, (R.TangentVector) -> ...T.TangentVector)
  unsigned numGenericParams = 1 + arity;
  BuiltinFunctionBuilder builder(Context, numGenericParams);
  // Get the `Differentiable` protocol.
  auto *diffableProto = Context.getProtocol(KnownProtocolKind::Differentiable);
  // Create type parameters and add conformance constraints.
  auto fnResultGen = makeGenericParam(arity);
  builder.addConformanceRequirement(fnResultGen, diffableProto);
  SmallVector<decltype(fnResultGen), 2> fnParamGens;
  for (auto i : range(arity)) {
    auto T = makeGenericParam(i);
    builder.addConformanceRequirement(T, diffableProto);
    fnParamGens.push_back(T);
  }
  // Generator for the first argument, i.e. the `@differentiable` function.
  BuiltinFunctionBuilder::LambdaGenerator firstArgGen{
      // Generator for the function type at the argument position, i.e. the
      // function being differentiated.
      [=, &fnParamGens](BuiltinFunctionBuilder &builder) -> Type {
        auto extInfo =
            FunctionType::ExtInfoBuilder()
                // TODO: Use `kind.getMinimalDifferentiabilityKind()`.
                .withDifferentiabilityKind(DifferentiabilityKind::Reverse)
                .withNoEscape()
                .withThrows(throws)
                .build();
        SmallVector<FunctionType::Param, 2> params;
        for (auto &paramGen : fnParamGens)
          params.push_back(FunctionType::Param(paramGen.build(builder)));
        return FunctionType::get(params, fnResultGen.build(builder), extInfo);
      }};
  // Eagerly build the type of the first arg, then use that to compute the type
  // of the result.
  auto *diffFnType =
      firstArgGen.build(builder)->castTo<AnyFunctionType>();
  diffFnType = diffFnType->getWithoutDifferentiability()->withExtInfo(
      diffFnType->getExtInfo().withNoEscape(false));
  auto *paramIndices = IndexSubset::get(
      Context, SmallBitVector(diffFnType->getNumParams(), true));
  // Generator for the resultant function type, i.e. the AD derivative function.
  BuiltinFunctionBuilder::LambdaGenerator resultGen{
      [=, &Context](BuiltinFunctionBuilder &builder) -> Type {
        auto derivativeFnTy = diffFnType->getAutoDiffDerivativeFunctionType(
            paramIndices, kind,
            LookUpConformanceInModule(Context.TheBuiltinModule));
        return derivativeFnTy->getResult();
      }};
  builder.addParameter(firstArgGen);
  for (auto argGen : fnParamGens)
    builder.addParameter(argGen);
  if (throws)
    builder.setRethrows();
  builder.setResult(resultGen);
  return builder.build(Id);
}

static ValueDecl *getAutoDiffApplyTransposeFunction(
    ASTContext &Context, Identifier Id, unsigned arity, bool throws) {
  assert(arity >= 1);
  // <...T...(arity), R>
  //     (@differentiable(_linear) (...T) throws -> R, ...R.TangentVector)
  //         rethrows -> (...T.TangentVector)
  unsigned numGenericParams = 1 + arity;
  BuiltinFunctionBuilder builder(Context, numGenericParams);
  auto *diffableProto = Context.getProtocol(KnownProtocolKind::Differentiable);
  auto *addArithProto =
      Context.getProtocol(KnownProtocolKind::AdditiveArithmetic);
  // Create type parameters and add conformance constraints.
  auto linearFnResultGen = makeGenericParam(arity);
  builder.addConformanceRequirement(linearFnResultGen, diffableProto);
  builder.addConformanceRequirement(linearFnResultGen, addArithProto);
  SmallVector<decltype(linearFnResultGen), 2> linearFnParamGens;
  for (auto i : range(arity)) {
    auto T = makeGenericParam(i);
    builder.addConformanceRequirement(T, diffableProto);
    builder.addConformanceRequirement(T, addArithProto);
    linearFnParamGens.push_back(T);
  }
  // Generator for the first argument, i.e. the `@differentiable(_linear)`
  // function.
  BuiltinFunctionBuilder::LambdaGenerator firstArgGen {
    // Generator for the function type at the argument position, i.e. the
    // function being differentiated.
    [=, &linearFnParamGens](BuiltinFunctionBuilder &builder) -> Type {
      FunctionType::ExtInfo ext;
      auto extInfo =
          FunctionType::ExtInfoBuilder()
              .withDifferentiabilityKind(DifferentiabilityKind::Linear)
              .withNoEscape()
              .withThrows(throws)
              .build();
      SmallVector<FunctionType::Param, 2> params;
      for (auto &paramGen : linearFnParamGens)
        params.push_back(FunctionType::Param(paramGen.build(builder)));
      // FIXME: Verify ExtInfo state is correct, not working by accident.
      FunctionType::ExtInfo info;
      auto innerFunction =
          FunctionType::get(params, linearFnResultGen.build(builder), info);
      return innerFunction->withExtInfo(extInfo);
    }
  };
  builder.addParameter(firstArgGen);
  builder.addParameter(linearFnResultGen);
  if (throws)
    builder.setRethrows();
  if (arity == 1)
    builder.setResult(linearFnParamGens.front());
  else {
    BuiltinFunctionBuilder::LambdaGenerator tupleResultGen {
      [&](BuiltinFunctionBuilder &builder) -> Type {
        SmallVector<TupleTypeElt, 2> tupleElts;
        for (auto linearFnParamGen : linearFnParamGens)
          tupleElts.push_back(linearFnParamGen.build(builder));
        return TupleType::get(tupleElts, Context);
      }
    };
    builder.setResult(tupleResultGen);
  }
  return builder.build(Id);
}

static ValueDecl *getGlobalStringTablePointer(ASTContext &Context,
                                              Identifier Id) {
  // String -> Builtin.RawPointer
  auto stringType = Context.getStringType();
  return getBuiltinFunction(Id, {stringType}, Context.TheRawPointerType);
}

static ValueDecl *getConvertStrongToUnownedUnsafe(ASTContext &ctx,
                                                  Identifier id) {
  // We actually want this:
  //
  // (T, inout unowned (unsafe) T) -> ()
  //
  // But for simplicity, we actually accept T, U and do the checking
  // in the emission method that everything works up. This is a
  // builtin, so we can crash.
  BuiltinFunctionBuilder builder(ctx, 2);
  builder.addParameter(makeGenericParam(0));
  builder.addParameter(makeGenericParam(1), ParamSpecifier::InOut);
  builder.setResult(makeConcrete(TupleType::getEmpty(ctx)));
  return builder.build(id);
}

static ValueDecl *getConvertUnownedUnsafeToGuaranteed(ASTContext &ctx,
                                                      Identifier id) {
  // We actually want this:
  //
  ///   <BaseT, T> (BaseT, @inout unowned(unsafe) T) -> T
  //
  // But for simplicity, we actually accept three generic params T, U and do the
  // checking in the emission method that everything works up. This is a
  // builtin, so we can crash.
  BuiltinFunctionBuilder builder(ctx, 3);
  builder.addParameter(makeGenericParam(0));                        // Base
  builder.addParameter(makeGenericParam(1), ParamSpecifier::InOut); // Unmanaged
  builder.setResult(makeGenericParam(2)); // Guaranteed Result
  return builder.build(id);
}

static ValueDecl *getGetCurrentAsyncTask(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(id, { }, ctx.TheNativeObjectType);
}

static ValueDecl *getGetCurrentExecutor(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _async(_thin),
                            _parameters(),
                            _optional(_executor));
}

static ValueDecl *getCancelAsyncTask(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(
      id, { ctx.TheNativeObjectType }, ctx.TheEmptyTupleType);
}

Type swift::getAsyncTaskAndContextType(ASTContext &ctx) {
  TupleTypeElt resultTupleElements[2] = {
    ctx.TheNativeObjectType, // task,
    ctx.TheRawPointerType    // initial context
  };

  return TupleType::get(resultTupleElements, ctx);
}

static ValueDecl *getCreateAsyncTask(ASTContext &ctx, Identifier id) {
  BuiltinFunctionBuilder builder(ctx);
  auto genericParam = makeGenericParam().build(builder);
  builder.addParameter(makeConcrete(ctx.getIntType())); // 0 flags
  auto extInfo = ASTExtInfoBuilder().withAsync().withThrows().build();
  builder.addParameter(
      makeConcrete(FunctionType::get({ }, genericParam, extInfo))); // 1 operation
  builder.setResult(makeConcrete(getAsyncTaskAndContextType(ctx)));
  return builder.build(id);
}

static ValueDecl *getCreateAsyncTaskInGroup(ASTContext &ctx, Identifier id) {
  BuiltinFunctionBuilder builder(ctx);
  auto genericParam = makeGenericParam().build(builder); // <T>
  builder.addParameter(makeConcrete(ctx.getIntType())); // 0 flags
  builder.addParameter(makeConcrete(ctx.TheRawPointerType)); // 1 group
  auto extInfo = ASTExtInfoBuilder().withAsync().withThrows().build();
  builder.addParameter(
      makeConcrete(FunctionType::get({ }, genericParam, extInfo))); // 2 operation
  builder.setResult(makeConcrete(getAsyncTaskAndContextType(ctx)));

  return builder.build(id);
}

static ValueDecl *getTaskRunInline(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(
      ctx, id, _thin, _generics(_unrestricted),
      _parameters(
          _function(_async(_noescape(_thick)), _typeparam(0), _parameters())),
      _typeparam(0));
}

static ValueDecl *getConvertTaskToJob(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id,
                            _thin,
                            _parameters(_owned(_nativeObject)),
                            _job);
}

static ValueDecl *getDefaultActorInitDestroy(ASTContext &ctx,
                                             Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_nativeObject),
                            _void);
}

static ValueDecl *getDistributedActorInitializeRemote(ASTContext &ctx,
                                                      Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted), // TODO(distributed): restrict to DistributedActor
                            _parameters(_metatype(_typeparam(0))),
                            _rawPointer);
}

static ValueDecl *getResumeContinuationReturning(ASTContext &ctx,
                                                 Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_rawUnsafeContinuation,
                                        _owned(_typeparam(0))),
                            _void);
}

static ValueDecl *getResumeContinuationThrowing(ASTContext &ctx,
                                                Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawUnsafeContinuation,
                                        _owned(_error)),
                            _void);
}

static ValueDecl *getStartAsyncLet(ASTContext &ctx, Identifier id) {
  ModuleDecl *M = ctx.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);
  SynthesisContext SC(ctx, DC);

  BuiltinFunctionBuilder builder(ctx);
  auto genericParam = makeGenericParam().build(builder); // <T>

  // AsyncLet*
  builder.addParameter(makeConcrete(OptionalType::get(ctx.TheRawPointerType)));

  // TaskOptionRecord*
  builder.addParameter(makeConcrete(OptionalType::get(ctx.TheRawPointerType)));

  // operation async function pointer: () async throws -> T
  auto extInfo = ASTExtInfoBuilder().withAsync().withThrows().withNoEscape().build();
  builder.addParameter(
      makeConcrete(FunctionType::get({ }, genericParam, extInfo)));

  // -> Builtin.RawPointer
  builder.setResult(makeConcrete(synthesizeType(SC, _rawPointer)));
  return builder.build(id);
}

static ValueDecl *getEndAsyncLet(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer),
                            _void);
}

static ValueDecl *getCreateTaskGroup(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted),
                            _parameters(_metatype(_typeparam(0))),
                            _rawPointer);
}

static ValueDecl *getCreateTaskGroupWithFlags(ASTContext &ctx, Identifier id) {
  ModuleDecl *M = ctx.TheBuiltinModule;
  DeclContext *DC = &M->getMainFile(FileUnitKind::Builtin);
  SynthesisContext SC(ctx, DC);

  BuiltinFunctionBuilder builder(ctx);

  // int
  builder.addParameter(makeConcrete(ctx.getIntType())); // 0 flags

  // T.self
  builder.addParameter(makeMetatype(makeGenericParam(0))); // 1 ChildTaskResult.Type

  // -> Builtin.RawPointer
  builder.setResult(makeConcrete(synthesizeType(SC, _rawPointer)));
  return builder.build(id);
}

static ValueDecl *getDestroyTaskGroup(ASTContext &ctx, Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _parameters(_rawPointer),
                            _void);
}

static ValueDecl *getBuildMainActorExecutorRef(ASTContext &ctx,
                                               Identifier id) {
  return getBuiltinFunction(ctx, id, _thin, _parameters(), _executor);
}

static ValueDecl *getBuildDefaultActorExecutorRef(ASTContext &ctx,
                                                  Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted,
                                      _layout(_typeparam(0), _classLayout())),
                            _parameters(_typeparam(0)),
                            _executor);
}

static ValueDecl *getTargetOSVersionAtLeast(ASTContext &Context,
                                            Identifier Id) {
  auto int32Type = BuiltinIntegerType::get(32, Context);
  return getBuiltinFunction(Id, {int32Type, int32Type, int32Type}, int32Type);
}

static ValueDecl *getBuildOrdinarySerialExecutorRef(ASTContext &ctx,
                                                    Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted,
                              _conformsTo(_typeparam(0), _serialExecutor)),
                            _parameters(_typeparam(0)),
                            _executor);
}

static ValueDecl *getBuildComplexEqualitySerialExecutorRef(ASTContext &ctx,
                                                           Identifier id) {
  return getBuiltinFunction(ctx, id, _thin,
                            _generics(_unrestricted,
                              _conformsTo(_typeparam(0), _serialExecutor)),
                            _parameters(_typeparam(0)),
                            _executor);
}

static ValueDecl *getAutoDiffCreateLinearMapContext(ASTContext &ctx,
                                                    Identifier id) {
  return getBuiltinFunction(
      id, {BuiltinIntegerType::getWordType(ctx)}, ctx.TheNativeObjectType);
}

static ValueDecl *getAutoDiffProjectTopLevelSubcontext(ASTContext &ctx,
                                                       Identifier id) {
  return getBuiltinFunction(
      id, {ctx.TheNativeObjectType}, ctx.TheRawPointerType);
}

static ValueDecl *getAutoDiffAllocateSubcontext(ASTContext &ctx,
                                                Identifier id) {
  return getBuiltinFunction(
      id, {ctx.TheNativeObjectType, BuiltinIntegerType::getWordType(ctx)},
      ctx.TheRawPointerType);
}

static ValueDecl *getPoundAssert(ASTContext &Context, Identifier Id) {
  auto int1Type = BuiltinIntegerType::get(1, Context);
  auto optionalRawPointerType = BoundGenericEnumType::get(
      Context.getOptionalDecl(), Type(), {Context.TheRawPointerType});
  return getBuiltinFunction(Id, {int1Type, optionalRawPointerType},
                            Context.TheEmptyTupleType);
}

static ValueDecl *getTSanInoutAccess(ASTContext &Context, Identifier Id) {
  // <T> T -> ()
  BuiltinFunctionBuilder builder(Context);
  builder.addParameter(makeGenericParam());
  builder.setResult(makeConcrete(Context.TheEmptyTupleType));
  return builder.build(Id);
}

static ValueDecl *getAddressOfOperation(ASTContext &Context, Identifier Id) {
  // <T> (@inout T) -> RawPointer
  BuiltinFunctionBuilder builder(Context);
  builder.addParameter(makeGenericParam(), ParamSpecifier::InOut);
  builder.setResult(makeConcrete(Context.TheRawPointerType));
  return builder.build(Id);
}

static ValueDecl *getAddressOfBorrowOperation(ASTContext &Context,
                                              Identifier Id) {
  // <T> (T) -> RawPointer
  BuiltinFunctionBuilder builder(Context);
  builder.addParameter(makeGenericParam());
  builder.setResult(makeConcrete(Context.TheRawPointerType));
  return builder.build(Id);
}

static ValueDecl *getTypeJoinOperation(ASTContext &Context, Identifier Id) {
  // <T,U,V> (T.Type, U.Type) -> V.Type
  BuiltinFunctionBuilder builder(Context, 3);
  builder.addParameter(makeMetatype(makeGenericParam(0)));
  builder.addParameter(makeMetatype(makeGenericParam(1)));
  builder.setResult(makeMetatype(makeGenericParam(2)));
  return builder.build(Id);
}

static ValueDecl *getTypeJoinInoutOperation(ASTContext &Context,
                                            Identifier Id) {
  // <T,U,V> (inout T, U.Type) -> V.Type
  BuiltinFunctionBuilder builder(Context, 3);
  builder.addParameter(makeGenericParam(0), ParamSpecifier::InOut);
  builder.addParameter(makeMetatype(makeGenericParam(1)));
  builder.setResult(makeMetatype(makeGenericParam(2)));
  return builder.build(Id);
}

static ValueDecl *getTypeJoinMetaOperation(ASTContext &Context, Identifier Id) {
  // <T,U,V> (T.Type, U.Type) -> V.Type
  BuiltinFunctionBuilder builder(Context, 3);
  builder.addParameter(makeMetatype(makeGenericParam(0)));
  builder.addParameter(makeMetatype(makeGenericParam(1)));
  builder.setResult(makeMetatype(makeGenericParam(2)));
  return builder.build(Id);
}

static ValueDecl *getTriggerFallbackDiagnosticOperation(ASTContext &Context,
                                                        Identifier Id) {
  // () -> Void
  return getBuiltinFunction(Id, {}, Context.TheEmptyTupleType);
}

static ValueDecl *getCanBeObjCClassOperation(ASTContext &Context,
                                             Identifier Id) {
  // <T> T.Type -> Builtin.Int8
  BuiltinFunctionBuilder builder(Context);
  builder.addParameter(makeMetatype(makeGenericParam()));
  builder.setResult(makeConcrete(BuiltinIntegerType::get(8, Context)));
  return builder.build(Id);
}

static ValueDecl *getLegacyCondFailOperation(ASTContext &C, Identifier Id) {
  // Int1 -> ()
  auto CondTy = BuiltinIntegerType::get(1, C);
  auto VoidTy = TupleType::getEmpty(C);
  return getBuiltinFunction(Id, {CondTy}, VoidTy);
}

static ValueDecl *getCondFailOperation(ASTContext &C, Identifier Id) {
  // Int1 -> ()
  auto CondTy = BuiltinIntegerType::get(1, C);
  auto MsgTy = C.TheRawPointerType;
  auto VoidTy = TupleType::getEmpty(C);
  return getBuiltinFunction(Id, {CondTy, MsgTy}, VoidTy);
}

static ValueDecl *getAssertConfOperation(ASTContext &C, Identifier Id) {
  // () -> Int32
  auto Int32Ty = BuiltinIntegerType::get(32, C);
  return getBuiltinFunction(Id, {}, Int32Ty);
}

static ValueDecl *getFixLifetimeOperation(ASTContext &C, Identifier Id) {
  // <T> T -> ()
  BuiltinFunctionBuilder builder(C);
  builder.addParameter(makeGenericParam());
  builder.setResult(makeConcrete(TupleType::getEmpty(C)));
  return builder.build(Id);
}

static ValueDecl *getExtractElementOperation(ASTContext &Context, Identifier Id,
                                             Type FirstTy, Type SecondTy) {
  // (Vector<N, T>, Int32) -> T
  auto VecTy = FirstTy->getAs<BuiltinVectorType>();
  if (!VecTy)
    return nullptr;

  auto IndexTy = SecondTy->getAs<BuiltinIntegerType>();
  if (!IndexTy || !IndexTy->isFixedWidth() || IndexTy->getFixedWidth() != 32)
    return nullptr;

  Type ResultTy = VecTy->getElementType();
  return getBuiltinFunction(Id, { VecTy, IndexTy }, ResultTy);
}

static ValueDecl *getInsertElementOperation(ASTContext &Context, Identifier Id,
                                            Type FirstTy, Type SecondTy,
                                            Type ThirdTy) {
  // (Vector<N, T>, T, Int32) -> Vector<N, T>
  auto VecTy = FirstTy->getAs<BuiltinVectorType>();
  if (!VecTy)
    return nullptr;
  auto ElementTy = VecTy->getElementType();

  if (!SecondTy->isEqual(ElementTy))
    return nullptr;

  auto IndexTy = ThirdTy->getAs<BuiltinIntegerType>();
  if (!IndexTy || !IndexTy->isFixedWidth() || IndexTy->getFixedWidth() != 32)
    return nullptr;

  Type ArgElts[] = { VecTy, ElementTy, IndexTy };
  return getBuiltinFunction(Id, ArgElts, VecTy);
}

static ValueDecl *getShuffleVectorOperation(ASTContext &Context, Identifier Id,
                                 Type FirstTy, Type SecondTy) {
  // (Vector<N, T>, Vector<N, T>, Vector<M, Int32) -> Vector<M, T>
  auto VecTy = FirstTy->getAs<BuiltinVectorType>();
  if (!VecTy)
    return nullptr;
  auto ElementTy = VecTy->getElementType();

  auto IndexTy = SecondTy->getAs<BuiltinVectorType>();
  if (!IndexTy)
    return nullptr;
  auto IdxElTy = IndexTy->getElementType()->getAs<BuiltinIntegerType>();
  if (!IdxElTy || !IdxElTy->isFixedWidth() || IdxElTy->getFixedWidth() != 32)
    return nullptr;

  Type ArgElts[] = { VecTy, VecTy, IndexTy };
  Type ResultTy = BuiltinVectorType::get(Context, ElementTy,
                                         IndexTy->getNumElements());
  return getBuiltinFunction(Id, ArgElts, ResultTy);
}

static ValueDecl *getStaticReportOperation(ASTContext &Context, Identifier Id) {
  auto BoolTy = BuiltinIntegerType::get(1, Context);
  auto MessageTy = Context.TheRawPointerType;

  Type ArgElts[] = { BoolTy, BoolTy, MessageTy };
  Type ResultTy = TupleType::getEmpty(Context);
  
  return getBuiltinFunction(Id, ArgElts, ResultTy);
}

static ValueDecl *getCheckedTruncOperation(ASTContext &Context, Identifier Id,
                                           Type InputTy, Type OutputTy,
                                           bool AllowLiteral) {
  auto InTy = InputTy->getAs<AnyBuiltinIntegerType>();
  auto OutTy = OutputTy->getAs<BuiltinIntegerType>();
  if (!InTy || !OutTy)
    return nullptr;
  if (isa<BuiltinIntegerLiteralType>(InTy)) {
    if (!AllowLiteral)
      return nullptr;
  } else if (cast<BuiltinIntegerType>(InTy)->getLeastWidth()
               < OutTy->getGreatestWidth()) {
    return nullptr;
  }

  Type OverflowBitTy = BuiltinIntegerType::get(1, Context);
  TupleTypeElt ResultElts[] = { Type(OutTy), OverflowBitTy };
  Type ResultTy = TupleType::get(ResultElts, Context);
  return getBuiltinFunction(Id, { InTy }, ResultTy);
}

static ValueDecl *getIntToFPWithOverflowOperation(ASTContext &Context,
                                                  Identifier Id, Type InputTy,
                                                  Type OutputTy) {
  auto InTy = InputTy->getAs<BuiltinIntegerLiteralType>();
  auto OutTy = OutputTy->getAs<BuiltinFloatType>();
  if (!InTy || !OutTy)
    return nullptr;

  return getBuiltinFunction(Id, { InTy }, OutTy);
}

static ValueDecl *getBitWidthOperation(
  ASTContext &ctx,
  Identifier id,
  Type valueTy
) {
  if (!valueTy->getAs<BuiltinIntegerLiteralType>()) return nullptr;
  return getBuiltinFunction(ctx, id, _thin, _parameters(valueTy), _word);
}

static ValueDecl *getIsNegativeOperation(
  ASTContext &ctx,
  Identifier id,
  Type valueTy
) {
  if (!valueTy->getAs<BuiltinIntegerLiteralType>()) return nullptr;
  return getBuiltinFunction(ctx, id, _thin, _parameters(valueTy), _int(1));
}

static ValueDecl *getWordAtIndexOperation(
  ASTContext &ctx,
  Identifier id,
  Type valueTy
) {
  if (!valueTy->getAs<BuiltinIntegerLiteralType>()) return nullptr;
  return getBuiltinFunction(ctx, id, _thin, _parameters(valueTy, _word), _word);
}

static ValueDecl *getUnreachableOperation(ASTContext &Context,
                                          Identifier Id) {
  auto NeverTy = Context.getNeverType();
  if (!NeverTy)
    return nullptr;

  // () -> Never
  return getBuiltinFunction(Id, {}, NeverTy);
}

static ValueDecl *getOnceOperation(ASTContext &Context,
                                   Identifier Id,
                                   bool withContext) {
  // (RawPointer, @convention(c) (Context) -> ()[, Context]) -> ()
  
  auto HandleTy = Context.TheRawPointerType;
  auto VoidTy = Context.TheEmptyTupleType;
  SmallVector<AnyFunctionType::Param, 1> CFuncParams;
  swift::CanType ContextTy = Context.TheRawPointerType;
  auto ContextArg = FunctionType::Param(ContextTy);
  CFuncParams.push_back(ContextArg);
  auto Rep = FunctionTypeRepresentation::CFunctionPointer;
  auto ClangType = Context.getClangFunctionType(CFuncParams, VoidTy, Rep);
  auto Thin =
      FunctionType::ExtInfoBuilder(FunctionTypeRepresentation::CFunctionPointer,
                                   /*throws*/ false)
          .withClangFunctionType(ClangType)
          .build();
  auto BlockTy = FunctionType::get(CFuncParams, VoidTy, Thin);
  SmallVector<swift::Type, 3> ArgTypes = {HandleTy, BlockTy};
  if (withContext)
    ArgTypes.push_back(ContextTy);
  return getBuiltinFunction(Id, ArgTypes, VoidTy);
}

static ValueDecl *getPolymorphicBinaryOperation(ASTContext &ctx,
                                                Identifier id) {
  BuiltinFunctionBuilder builder(ctx);
  builder.addParameter(makeGenericParam());
  builder.addParameter(makeGenericParam());
  builder.setResult(makeGenericParam());
  return builder.build(id);
}

static ValueDecl *getWithUnsafeContinuation(ASTContext &ctx,
                                            Identifier id,
                                            bool throws) {
  BuiltinFunctionBuilder builder(ctx);

  auto contTy = ctx.TheRawUnsafeContinuationType;
  SmallVector<AnyFunctionType::Param, 1> params;
  params.emplace_back(contTy);

  auto voidTy = ctx.TheEmptyTupleType;
  auto extInfo = FunctionType::ExtInfoBuilder().withNoEscape().build();
  auto *fnTy = FunctionType::get(params, voidTy, extInfo);

  builder.addParameter(makeConcrete(fnTy));
  builder.setResult(makeGenericParam());

  builder.setAsync();
  if (throws)
    builder.setThrows();

  return builder.build(id);
}

static ValueDecl *getHopToActor(ASTContext &ctx, Identifier id) {
  BuiltinFunctionBuilder builder(ctx);
  auto *actorProto = ctx.getProtocol(KnownProtocolKind::Actor);
  // Create type parameters and add conformance constraints.
  auto actorParam = makeGenericParam();
  builder.addParameter(actorParam);
  builder.addConformanceRequirement(actorParam, actorProto);
  builder.setResult(makeConcrete(TupleType::getEmpty(ctx)));
  return builder.build(id);
}

/// An array of the overloaded builtin kinds.
static const OverloadedBuiltinKind OverloadedBuiltinKinds[] = {
  OverloadedBuiltinKind::None,

// There's deliberately no BUILTIN clause here so that we'll blow up
// if new builtin categories are added there and not here.
#define BUILTIN_CAST_OPERATION(id, attrs, name) \
   OverloadedBuiltinKind::Special,
#define BUILTIN_CAST_OR_BITCAST_OPERATION(id, attrs, name) \
   OverloadedBuiltinKind::Special,
#define BUILTIN_BINARY_OPERATION_OVERLOADED_STATIC(id, name, attrs, overload)  \
  OverloadedBuiltinKind::overload,
#define BUILTIN_BINARY_OPERATION_POLYMORPHIC(id, name)                         \
  OverloadedBuiltinKind::Special,
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload) \
   OverloadedBuiltinKind::overload,
#define BUILTIN_BINARY_PREDICATE(id, name, attrs, overload) \
   OverloadedBuiltinKind::overload,
#define BUILTIN_UNARY_OPERATION(id, name, attrs, overload) \
   OverloadedBuiltinKind::overload,
#define BUILTIN_SIL_OPERATION(id, name, overload) \
   OverloadedBuiltinKind::overload,
#define BUILTIN_MISC_OPERATION(id, name, attrs, overload) \
   OverloadedBuiltinKind::overload,
#define BUILTIN_SANITIZER_OPERATION(id, name, attrs) \
  OverloadedBuiltinKind::None,
#define BUILTIN_TYPE_CHECKER_OPERATION(id, name) OverloadedBuiltinKind::Special,
#define BUILTIN_TYPE_TRAIT_OPERATION(id, name) \
   OverloadedBuiltinKind::Special,
#define BUILTIN_RUNTIME_CALL(id, attrs, name) \
  OverloadedBuiltinKind::Special,
#include "swift/AST/Builtins.def"
};

/// Determines if a builtin type falls within the given category.
inline bool isBuiltinTypeOverloaded(Type T, OverloadedBuiltinKind OK) {
  switch (OK) {
  case OverloadedBuiltinKind::None:
    return false;  // always fail. 
  case OverloadedBuiltinKind::Integer:
    return T->is<BuiltinIntegerType>();
  case OverloadedBuiltinKind::IntegerOrVector:
    return T->is<BuiltinIntegerType>() ||
           (T->is<BuiltinVectorType>() &&
            T->castTo<BuiltinVectorType>()->getElementType()
              ->is<BuiltinIntegerType>());
  case OverloadedBuiltinKind::IntegerOrRawPointer:
    return T->is<BuiltinIntegerType>() || T->is<BuiltinRawPointerType>();
  case OverloadedBuiltinKind::IntegerOrRawPointerOrVector:
    return T->is<BuiltinIntegerType>() || T->is<BuiltinRawPointerType>() ||
           (T->is<BuiltinVectorType>() &&
            T->castTo<BuiltinVectorType>()->getElementType()
              ->is<BuiltinIntegerType>());
  case OverloadedBuiltinKind::Float:
    return T->is<BuiltinFloatType>();
  case OverloadedBuiltinKind::FloatOrVector:
    return T->is<BuiltinFloatType>() ||
           (T->is<BuiltinVectorType>() &&
            T->castTo<BuiltinVectorType>()->getElementType()
              ->is<BuiltinFloatType>());
  case OverloadedBuiltinKind::Special:
    return true;
  }
  llvm_unreachable("bad overloaded builtin kind");
}

bool swift::canBuiltinBeOverloadedForType(BuiltinValueKind ID, Type Ty) {
  if (ID == BuiltinValueKind::None)
    return false;

  return isBuiltinTypeOverloaded(Ty, OverloadedBuiltinKinds[unsigned(ID)]);
}

/// Table of string intrinsic names indexed by enum value.
static const char *const IntrinsicNameTable[] = {
    "not_intrinsic",
#define GET_INTRINSIC_NAME_TABLE
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_NAME_TABLE
};

#define GET_INTRINSIC_TARGET_DATA
#include "llvm/IR/IntrinsicImpl.inc"
#undef GET_INTRINSIC_TARGET_DATA

llvm::Intrinsic::ID swift::getLLVMIntrinsicID(StringRef InName) {
  using namespace llvm;

  // Swift intrinsic names start with int_.
  if (!InName.startswith("int_"))
    return llvm::Intrinsic::not_intrinsic;
  InName = InName.drop_front(strlen("int_"));
  
  // Prepend "llvm." and change _ to . in name.
  SmallString<128> NameS;
  NameS.append("llvm.");
  for (char C : InName)
    NameS.push_back(C == '_' ? '.' : C);

  const char *Name = NameS.c_str();
  ArrayRef<const char *> NameTable(&IntrinsicNameTable[1],
                                   TargetInfos[1].Offset);
  int Idx = Intrinsic::lookupLLVMIntrinsicByName(NameTable, Name);
  return static_cast<Intrinsic::ID>(Idx + 1);
}

llvm::Intrinsic::ID
swift::getLLVMIntrinsicIDForBuiltinWithOverflow(BuiltinValueKind ID) {
  switch (ID) {
    default: break;
    case BuiltinValueKind::SAddOver:
      return llvm::Intrinsic::sadd_with_overflow;
    case BuiltinValueKind::UAddOver:
      return llvm::Intrinsic::uadd_with_overflow;
    case BuiltinValueKind::SSubOver:
      return llvm::Intrinsic::ssub_with_overflow;
    case BuiltinValueKind::USubOver:
      return llvm::Intrinsic::usub_with_overflow;
    case BuiltinValueKind::SMulOver:
      return llvm::Intrinsic::smul_with_overflow;
    case BuiltinValueKind::UMulOver:
      return llvm::Intrinsic::umul_with_overflow;
  }
  llvm_unreachable("Cannot convert the overflow builtin to llvm intrinsic.");
}

namespace {

class IntrinsicTypeDecoder {
  ArrayRef<llvm::Intrinsic::IITDescriptor> &Table;
  ArrayRef<Type> TypeArguments;
  ASTContext &Context;
public:
  IntrinsicTypeDecoder(ArrayRef<llvm::Intrinsic::IITDescriptor> &table,
                       ArrayRef<Type> typeArguments, ASTContext &ctx)
    : Table(table), TypeArguments(typeArguments), Context(ctx) {}

  Type decodeImmediate();

  /// Return the type argument at the given index.
  Type getTypeArgument(unsigned index) {
    if (index >= TypeArguments.size())
      return Type();
    return TypeArguments[index];
  }

  /// Create a pointer type.
  Type makePointer(Type eltType, unsigned addrspace) {
    // Reject non-default address space pointers.
    if (addrspace)
      return Type();

    // For now, always ignore the element type and use RawPointer.
    return Context.TheRawPointerType;
  }

  /// Create a vector type.
  Type makeVector(Type eltType, unsigned width) {
    return BuiltinVectorType::get(Context, eltType, width);
  }

  /// Return the first type or, if the second type is a vector type, a vector
  /// of the first type of the same length as the second type.
  Type maybeMakeVectorized(Type eltType, Type maybeVectorType) {
    if (auto vectorType = maybeVectorType->getAs<BuiltinVectorType>()) {
      return makeVector(eltType, vectorType->getNumElements());
    }
    return eltType;
  }
};

} // end anonymous namespace

static Type DecodeIntrinsicType(ArrayRef<llvm::Intrinsic::IITDescriptor> &table,
                                ArrayRef<Type> typeArguments, ASTContext &ctx) {
  return IntrinsicTypeDecoder(table, typeArguments, ctx).decodeImmediate();
}

Type IntrinsicTypeDecoder::decodeImmediate() {
  typedef llvm::Intrinsic::IITDescriptor IITDescriptor;
  IITDescriptor D = Table.front();
  Table = Table.slice(1);
  switch (D.Kind) {
  case IITDescriptor::BFloat:
  case IITDescriptor::MMX:
  case IITDescriptor::AMX:
  case IITDescriptor::Metadata:
  case IITDescriptor::ExtendArgument:
  case IITDescriptor::TruncArgument:
  case IITDescriptor::HalfVecArgument:
  case IITDescriptor::VarArg:
  case IITDescriptor::Token:
  case IITDescriptor::VecOfAnyPtrsToElt:
  case IITDescriptor::VecOfBitcastsToInt:
  case IITDescriptor::Subdivide2Argument:
  case IITDescriptor::Subdivide4Argument:
  case IITDescriptor::PPCQuad:
  case IITDescriptor::AnyPtrToElt:
    // These types cannot be expressed in swift yet.
    return Type();

  // Fundamental types.
  case IITDescriptor::Void:
    return TupleType::getEmpty(Context);
  case IITDescriptor::Half:
    return Context.TheIEEE16Type;
  case IITDescriptor::Float:
    return Context.TheIEEE32Type;
  case IITDescriptor::Double:
    return Context.TheIEEE64Type;
  case IITDescriptor::Quad:
    return Context.TheIEEE128Type;
  case IITDescriptor::Integer:
    return BuiltinIntegerType::get(D.Integer_Width, Context);

  // A vector of an immediate type.
  case IITDescriptor::Vector: {
    Type eltType = decodeImmediate();
    if (!eltType) return Type();
    return makeVector(eltType, D.Vector_Width.getKnownMinValue());
  }
  
  // The element type of a vector type.
  case IITDescriptor::VecElementArgument: {
    Type argType = getTypeArgument(D.getArgumentNumber());
    if (!argType) return Type();
    auto vecType = argType->getAs<BuiltinVectorType>();
    if (!vecType) return Type();
    return vecType->getElementType();
  }

  // A pointer to an immediate type.
  case IITDescriptor::Pointer: {
    Type pointeeType = decodeImmediate();
    if (!pointeeType) return Type();
    return makePointer(pointeeType, D.Pointer_AddressSpace);
  }

  // A type argument.
  case IITDescriptor::Argument:
    return getTypeArgument(D.getArgumentNumber());

  // A pointer to a type argument.
  case IITDescriptor::PtrToArgument: {
    Type argType = getTypeArgument(D.getArgumentNumber());
    if (!argType) return Type();
    unsigned addrspace = 0; // An apparent limitation of LLVM.
    return makePointer(argType, addrspace);
  }

  // A vector of the same width as a type argument.
  case IITDescriptor::SameVecWidthArgument: {
    Type maybeVectorType = getTypeArgument(D.getArgumentNumber());
    if (!maybeVectorType) return Type();
    Type eltType = decodeImmediate();
    if (!eltType) return Type();
    return maybeMakeVectorized(eltType, maybeVectorType);
  }

  // A pointer to the element type of a type argument, which must be a vector.
  case IITDescriptor::PtrToElt: {
    Type argType = getTypeArgument(D.getArgumentNumber());
    if (!argType) return Type();
    auto vecType = argType->getAs<BuiltinVectorType>();
    if (!vecType) return Type();
    unsigned addrspace = 0; // An apparent limitation of LLVM.
    return makePointer(vecType->getElementType(), addrspace);
  }

  // A struct, which we translate as a tuple.
  case IITDescriptor::Struct: {
    SmallVector<TupleTypeElt, 5> Elts;
    for (unsigned i = 0; i != D.Struct_NumElements; ++i) {
      Type T = decodeImmediate();
      if (!T) return Type();
      
      Elts.push_back(T);
    }
    return TupleType::get(Elts, Context);
  }
  }
  llvm_unreachable("unhandled");
}

/// \returns true on success, false on failure.
static bool
getSwiftFunctionTypeForIntrinsic(llvm::Intrinsic::ID ID,
                                 ArrayRef<Type> TypeArgs,
                                 ASTContext &Context,
                                 SmallVectorImpl<Type> &ArgElts,
                                 Type &ResultTy) {
  typedef llvm::Intrinsic::IITDescriptor IITDescriptor;
  SmallVector<IITDescriptor, 8> Table;
  getIntrinsicInfoTableEntries(ID, Table);

  ArrayRef<IITDescriptor> TableRef = Table;

  // Decode the intrinsic's LLVM IR type, and map it to swift builtin types.
  ResultTy = DecodeIntrinsicType(TableRef, TypeArgs, Context);
  if (!ResultTy)
    return false;

  while (!TableRef.empty()) {
    Type ArgTy = DecodeIntrinsicType(TableRef, TypeArgs, Context);
    if (!ArgTy)
      return false;
    ArgElts.push_back(ArgTy);
  }

  // Translate LLVM function attributes to Swift function attributes.
  IntrinsicInfo II;
  II.ID = ID;
  auto attrs = II.getOrCreateAttributes(Context);
  if (attrs.hasFnAttr(llvm::Attribute::NoReturn)) {
    ResultTy = Context.getNeverType();
    if (!ResultTy)
      return false;
  }

  return true;
}

static bool isValidFenceOrdering(StringRef Ordering) {
  return Ordering == "acquire" || Ordering == "release" ||
         Ordering == "acqrel" || Ordering == "seqcst";
}

static bool isValidRMWOrdering(StringRef Ordering) {
  return Ordering == "unordered" || Ordering == "monotonic" ||
         Ordering == "acquire" || Ordering == "release" ||
         Ordering == "acqrel" || Ordering == "seqcst";
}

static bool isValidLoadOrdering(StringRef Ordering) {
  return Ordering == "unordered" || Ordering == "monotonic" ||
         Ordering == "acquire" ||
         Ordering == "seqcst";
}

static bool isValidStoreOrdering(StringRef Ordering) {
  return Ordering == "unordered" || Ordering == "monotonic" ||
         Ordering == "release" ||
         Ordering == "seqcst";
}

llvm::AtomicOrdering swift::decodeLLVMAtomicOrdering(StringRef O) {
  using namespace llvm;
  return StringSwitch<AtomicOrdering>(O)
    .Case("unordered", AtomicOrdering::Unordered)
    .Case("monotonic", AtomicOrdering::Monotonic)
    .Case("acquire", AtomicOrdering::Acquire)
    .Case("release", AtomicOrdering::Release)
    .Case("acqrel", AtomicOrdering::AcquireRelease)
    .Case("seqcst", AtomicOrdering::SequentiallyConsistent)
    .Default(AtomicOrdering::NotAtomic);
}

static bool isUnknownOrUnordered(llvm::AtomicOrdering ordering) {
  using namespace llvm;
  switch (ordering) {
  case AtomicOrdering::NotAtomic:
  case AtomicOrdering::Unordered:
    return true;

  case AtomicOrdering::Monotonic:
  case AtomicOrdering::Acquire:
  case AtomicOrdering::Release:
  case AtomicOrdering::AcquireRelease:
  case AtomicOrdering::SequentiallyConsistent:
    return false;
  }

  llvm_unreachable("Unhandled AtomicOrdering in switch.");
}

static bool isValidCmpXChgOrdering(StringRef SuccessString, 
                                   StringRef FailureString) {
  using namespace llvm;
  AtomicOrdering SuccessOrdering = decodeLLVMAtomicOrdering(SuccessString);
  AtomicOrdering FailureOrdering = decodeLLVMAtomicOrdering(FailureString);

  // Unordered and unknown values are not allowed.
  if (isUnknownOrUnordered(SuccessOrdering) ||
      isUnknownOrUnordered(FailureOrdering))
    return false;
  // Success must be at least as strong as failure.
  if (!isAtLeastOrStrongerThan(SuccessOrdering, FailureOrdering))
    return false;
  // Failure may not release because no store occurred.
  if (FailureOrdering == AtomicOrdering::Release ||
      FailureOrdering == AtomicOrdering::AcquireRelease)
    return false;

  return true;
}

ValueDecl *swift::getBuiltinValueDecl(ASTContext &Context, Identifier Id) {
  // Builtin.TheTupleType resolves to the singleton instance of BuiltinTupleDecl.
  if (Id == Context.Id_TheTupleType)
    return Context.getBuiltinTupleDecl();

  SmallVector<Type, 4> Types;
  StringRef OperationName = getBuiltinBaseName(Context, Id.str(), Types);

  // If this is the name of an LLVM intrinsic, cons up a swift function with a
  // type that matches the IR types.
  if (llvm::Intrinsic::ID ID = getLLVMIntrinsicID(OperationName)) {
    SmallVector<Type, 8> ArgElts;
    Type ResultTy;
    if (getSwiftFunctionTypeForIntrinsic(ID, Types, Context, ArgElts, ResultTy))
      return getBuiltinFunction(Id, ArgElts, ResultTy);
  }

  // If this starts with fence, we have special suffixes to handle.
  if (OperationName.startswith("ifdef_")) {
    OperationName = OperationName.drop_front(strlen("ifdef_"));
    if (!Types.empty()) return nullptr;
    if (OperationName.empty()) return nullptr;
    return getIfdefOperation(Context, Id);
  }
  
  // If this starts with fence, we have special suffixes to handle.
  if (OperationName.startswith("fence_")) {
    OperationName = OperationName.drop_front(strlen("fence_"));
    
    // Verify we have a single integer, floating point, or pointer type.
    if (!Types.empty()) return nullptr;
    
    // Get and validate the ordering argument, which is required.
    auto Underscore = OperationName.find('_');
    if (!isValidFenceOrdering(OperationName.substr(0, Underscore)))
      return nullptr;
    OperationName = OperationName.substr(Underscore);
    
    // Accept singlethread if present.
    if (OperationName.startswith("_singlethread"))
      OperationName = OperationName.drop_front(strlen("_singlethread"));
    // Nothing else is allowed in the name.
    if (!OperationName.empty())
      return nullptr;
    return getFenceOperation(Context, Id);
  }
  
  // If this starts with cmpxchg, we have special suffixes to handle.
  if (OperationName.startswith("cmpxchg_")) {
    OperationName = OperationName.drop_front(strlen("cmpxchg_"));
    
    // Verify we have a single integer, floating point, or pointer type.
    if (Types.size() != 1) return nullptr;
    Type T = Types[0];
    if (!T->is<BuiltinIntegerType>() && !T->is<BuiltinRawPointerType>() &&
        !T->is<BuiltinFloatType>())
      return nullptr;

    // Get and validate the ordering arguments, which are both required.
    SmallVector<StringRef, 4> Parts;
    OperationName.split(Parts, "_");
    if (Parts.size() < 2)
      return nullptr;
    if (!isValidCmpXChgOrdering(Parts[0], Parts[1]))
      return nullptr;
    auto NextPart = Parts.begin() + 2;

    // Accept weak, volatile, and singlethread if present.
    if (NextPart != Parts.end() && *NextPart == "weak")
      ++NextPart;
    if (NextPart != Parts.end() && *NextPart == "volatile")
      ++NextPart;
    if (NextPart != Parts.end() && *NextPart == "singlethread")
      ++NextPart;
    // Nothing else is allowed in the name.
    if (NextPart != Parts.end())
      return nullptr;
    return getCmpXChgOperation(Context, Id, T);
  }

  // If this starts with atomicrmw, we have special suffixes to handle.
  if (OperationName.startswith("atomicrmw_")) {
    OperationName = OperationName.drop_front(strlen("atomicrmw_"));
    
    // Verify we have a single integer or pointer type.
    if (Types.size() != 1) return nullptr;
    Type Ty = Types[0];
    if (!Ty->is<BuiltinIntegerType>() && !Ty->is<BuiltinRawPointerType>())
      return nullptr;
    
    // Get and validate the suboperation name, which is required.
    auto Underscore = OperationName.find('_');
    if (Underscore == StringRef::npos) return nullptr;
    StringRef SubOp = OperationName.substr(0, Underscore);
    if (SubOp != "xchg" && SubOp != "add" && SubOp != "sub" && SubOp != "and" &&
        SubOp != "nand" && SubOp != "or" && SubOp != "xor" && SubOp != "max" &&
        SubOp != "min" && SubOp != "umax" && SubOp != "umin")
      return nullptr;
    OperationName = OperationName.drop_front(Underscore+1);
    
    // Get and validate the ordering argument, which is required.
    Underscore = OperationName.find('_');
    if (!isValidRMWOrdering(OperationName.substr(0, Underscore)))
      return nullptr;
    OperationName = OperationName.substr(Underscore);
    
    // Accept volatile and singlethread if present.
    if (OperationName.startswith("_volatile"))
      OperationName = OperationName.drop_front(strlen("_volatile"));
    if (OperationName.startswith("_singlethread"))
      OperationName = OperationName.drop_front(strlen("_singlethread"));
    // Nothing else is allowed in the name.
    if (!OperationName.empty())
      return nullptr;
    return getAtomicRMWOperation(Context, Id, Ty);
  }

  // If this starts with atomicload or atomicstore, we have special suffixes to
  // handle.
  if (OperationName.startswith("atomicload_")) {
    OperationName = OperationName.drop_front(strlen("atomicload_"));

    // Verify we have a single integer, floating point, or pointer type.
    if (Types.size() != 1) return nullptr;
    Type T = Types[0];
    if (!T->is<BuiltinIntegerType>() && !T->is<BuiltinRawPointerType>() &&
        !T->is<BuiltinFloatType>())
      return nullptr;

    // Get and validate the ordering argument, which is required.
    auto Underscore = OperationName.find('_');
    if (!isValidLoadOrdering(OperationName.substr(0, Underscore)))
      return nullptr;
    OperationName = OperationName.substr(Underscore);

    // Accept volatile and singlethread if present.
    if (OperationName.startswith("_volatile"))
      OperationName = OperationName.drop_front(strlen("_volatile"));
    if (OperationName.startswith("_singlethread"))
      OperationName = OperationName.drop_front(strlen("_singlethread"));
    // Nothing else is allowed in the name.
    if (!OperationName.empty())
      return nullptr;
    return getAtomicLoadOperation(Context, Id, T);
  }
  if (OperationName.startswith("atomicstore_")) {
    OperationName = OperationName.drop_front(strlen("atomicstore_"));

    // Verify we have a single integer, floating point, or pointer type.
    if (Types.size() != 1) return nullptr;
    Type T = Types[0];
    if (!T->is<BuiltinIntegerType>() && !T->is<BuiltinRawPointerType>() &&
        !T->is<BuiltinFloatType>())
      return nullptr;

    // Get and validate the ordering argument, which is required.
    auto Underscore = OperationName.find('_');
    if (!isValidStoreOrdering(OperationName.substr(0, Underscore)))
      return nullptr;
    OperationName = OperationName.substr(Underscore);

    // Accept volatile and singlethread if present.
    if (OperationName.startswith("_volatile"))
      OperationName = OperationName.drop_front(strlen("_volatile"));
    if (OperationName.startswith("_singlethread"))
      OperationName = OperationName.drop_front(strlen("_singlethread"));
    // Nothing else is allowed in the name.
    if (!OperationName.empty())
      return nullptr;
    return getAtomicStoreOperation(Context, Id, T);
  }
  if (OperationName.startswith("allocWithTailElems_")) {
    OperationName = OperationName.drop_front(strlen("allocWithTailElems_"));
    int NumTailTypes = 0;
    if (OperationName.getAsInteger(10, NumTailTypes))
      return nullptr;

    return getAllocWithTailElemsOperation(Context, Id, NumTailTypes);
  }
  if (OperationName.startswith("applyDerivative_")) {
    AutoDiffDerivativeFunctionKind kind;
    unsigned arity;
    bool throws;
    if (!autodiff::getBuiltinApplyDerivativeConfig(
            OperationName, kind, arity, throws))
      return nullptr;
    return getAutoDiffApplyDerivativeFunction(Context, Id, kind, arity,
                                              throws);
  }
  if (OperationName.startswith("applyTranspose_")) {
    unsigned arity;
    bool throws;
    if (!autodiff::getBuiltinApplyTransposeConfig(
            OperationName, arity, throws))
      return nullptr;
    return getAutoDiffApplyTransposeFunction(Context, Id, arity, throws);
  }

  auto BV = llvm::StringSwitch<BuiltinValueKind>(OperationName)
#define BUILTIN(id, name, Attrs) .Case(name, BuiltinValueKind::id)
#include "swift/AST/Builtins.def"
    .Default(BuiltinValueKind::None);

  // Filter out inappropriate overloads.
  OverloadedBuiltinKind OBK = OverloadedBuiltinKinds[unsigned(BV)];

  // Verify that all types match the overload filter.
  for (Type T : Types)
    if (!isBuiltinTypeOverloaded(T, OBK))
      return nullptr;

  switch (BV) {
  case BuiltinValueKind::Fence:
  case BuiltinValueKind::Ifdef:
  case BuiltinValueKind::CmpXChg:
  case BuiltinValueKind::AtomicRMW:
  case BuiltinValueKind::AtomicLoad:
  case BuiltinValueKind::AtomicStore:
  case BuiltinValueKind::AllocWithTailElems:
    llvm_unreachable("Handled above");
  case BuiltinValueKind::None: return nullptr;

  case BuiltinValueKind::GepRaw:
    if (Types.size() != 1) return nullptr;
    return getGepRawOperation(Context, Id, Types[0]);

  case BuiltinValueKind::StringObjectOr:
    if (Types.size() != 1)
      return nullptr;
    return getStringObjectOrOperation(Context, Id, Types[0]);

  case BuiltinValueKind::Gep:
    if (Types.size() != 1) return nullptr;
    return getGepOperation(Context, Id, Types[0]);

  case BuiltinValueKind::GetTailAddr:
    if (Types.size() != 1) return nullptr;
    return getGetTailAddrOperation(Context, Id, Types[0]);

  case BuiltinValueKind::PerformInstantaneousReadAccess:
    if (!Types.empty()) return nullptr;
      return getPerformInstantaneousReadAccessOperation(Context, Id);

  case BuiltinValueKind::BeginUnpairedModifyAccess:
    if (!Types.empty()) return nullptr;
    return getBeginUnpairedAccessOperation(Context, Id);

  case BuiltinValueKind::EndUnpairedAccess:
    if (!Types.empty()) return nullptr;
    return getEndUnpairedAccessOperation(Context, Id);

  case BuiltinValueKind::Copy:
    if (!Types.empty())
      return nullptr;
    return getCopyOperation(Context, Id);

  case BuiltinValueKind::AssumeAlignment:
    if (!Types.empty())
      return nullptr;
    return getAssumeAlignment(Context, Id);

#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION(id, name, attrs)
#define BUILTIN_BINARY_OPERATION_OVERLOADED_STATIC(id, name, attrs, overload)  \
  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    if (Types.size() != 1) return nullptr;
      return getBinaryOperation(Context, Id, Types[0]);

#define BUILTIN(id, name, attrs)
#define BUILTIN_BINARY_OPERATION(id, name, attrs)
#define BUILTIN_BINARY_OPERATION_POLYMORPHIC(id, name)                         \
  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      if (!Types.empty())
        return nullptr;
      return getPolymorphicBinaryOperation(Context, Id);

#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_OPERATION_WITH_OVERFLOW(id, name, _, attrs, overload)  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
      if (Types.size() != 1) return nullptr;
      return getBinaryOperationWithOverflow(Context, Id, Types[0]);

#define BUILTIN(id, name, Attrs)
#define BUILTIN_BINARY_PREDICATE(id, name, attrs, overload)  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    if (Types.size() != 1) return nullptr;
    return getBinaryPredicate(Context, Id, Types[0]);

#define BUILTIN(id, name, Attrs)
#define BUILTIN_UNARY_OPERATION(id, name, attrs, overload)   case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    if (Types.size() != 1) return nullptr;
    return getUnaryOperation(Context, Id, Types[0]);
      
#define BUILTIN(id, name, Attrs)
#define BUILTIN_CAST_OPERATION(id, name, attrs)  case BuiltinValueKind::id:
#define BUILTIN_CAST_OR_BITCAST_OPERATION(id, name, attrs)  case BuiltinValueKind::id:
#include "swift/AST/Builtins.def"
    return getCastOperation(Context, Id, BV, Types);

  case BuiltinValueKind::Retain:
  case BuiltinValueKind::Release:
  case BuiltinValueKind::Autorelease:
    if (!Types.empty()) return nullptr;
    return getRefCountingOperation(Context, Id);
      
  case BuiltinValueKind::Load:
  case BuiltinValueKind::LoadRaw:
  case BuiltinValueKind::LoadInvariant:
  case BuiltinValueKind::Take:
    if (!Types.empty()) return nullptr;
    return getLoadOperation(Context, Id);
      
  case BuiltinValueKind::Destroy:
    if (!Types.empty()) return nullptr;
    return getDestroyOperation(Context, Id);

  case BuiltinValueKind::Assign:
  case BuiltinValueKind::Init:
    if (!Types.empty()) return nullptr;
    return getStoreOperation(Context, Id);

  case BuiltinValueKind::DestroyArray:
    if (!Types.empty()) return nullptr;
    return getDestroyArrayOperation(Context, Id);
      
  case BuiltinValueKind::CopyArray:
  case BuiltinValueKind::TakeArrayNoAlias:
  case BuiltinValueKind::TakeArrayFrontToBack:
  case BuiltinValueKind::TakeArrayBackToFront:
  case BuiltinValueKind::AssignCopyArrayNoAlias:
  case BuiltinValueKind::AssignCopyArrayFrontToBack:
  case BuiltinValueKind::AssignCopyArrayBackToFront:
  case BuiltinValueKind::AssignTakeArray:
    if (!Types.empty()) return nullptr;
    return getTransferArrayOperation(Context, Id);

  case BuiltinValueKind::IsUnique:
  case BuiltinValueKind::IsUnique_native:
  case BuiltinValueKind::BeginCOWMutation:
  case BuiltinValueKind::BeginCOWMutation_native:
    if (!Types.empty()) return nullptr;
    // BeginCOWMutation has the same signature as IsUnique.
    return getIsUniqueOperation(Context, Id);

  case BuiltinValueKind::EndCOWMutation:
    if (!Types.empty()) return nullptr;
    return getEndCOWMutation(Context, Id);

  case BuiltinValueKind::BindMemory:
    if (!Types.empty()) return nullptr;
    return getBindMemoryOperation(Context, Id);

  case BuiltinValueKind::RebindMemory:
    if (!Types.empty()) return nullptr;
    return getRebindMemoryOperation(Context, Id);

  case BuiltinValueKind::ProjectTailElems:
    if (!Types.empty()) return nullptr;
    return getProjectTailElemsOperation(Context, Id);

  case BuiltinValueKind::Sizeof:
  case BuiltinValueKind::Strideof:
  case BuiltinValueKind::Alignof:
    return getSizeOrAlignOfOperation(Context, Id);

  case BuiltinValueKind::IsPOD:
    return getIsPODOperation(Context, Id);

  case BuiltinValueKind::IsConcrete:
    return getIsConcrete(Context, Id);

  case BuiltinValueKind::IsBitwiseTakable:
    return getIsBitwiseTakable(Context, Id);

  case BuiltinValueKind::IsOptionalType:
    return getIsOptionalOperation(Context, Id);

  case BuiltinValueKind::IsSameMetatype:
    return getIsSameMetatypeOperation(Context, Id);

  case BuiltinValueKind::AllocRaw:
    return getAllocOperation(Context, Id);

  case BuiltinValueKind::DeallocRaw:
    return getDeallocOperation(Context, Id);

  case BuiltinValueKind::StackAlloc:
  case BuiltinValueKind::UnprotectedStackAlloc:
    return getStackAllocOperation(Context, Id);
  case BuiltinValueKind::StackDealloc:
    return getStackDeallocOperation(Context, Id);

  case BuiltinValueKind::CastToNativeObject:
  case BuiltinValueKind::UnsafeCastToNativeObject:
  case BuiltinValueKind::CastFromNativeObject:
  case BuiltinValueKind::BridgeToRawPointer:
  case BuiltinValueKind::BridgeFromRawPointer:
    if (!Types.empty()) return nullptr;
    return getNativeObjectCast(Context, Id, BV);

  case BuiltinValueKind::CastToBridgeObject:
    if (!Types.empty()) return nullptr;
    return getCastToBridgeObjectOperation(Context, Id);
  case BuiltinValueKind::CastReferenceFromBridgeObject:
  case BuiltinValueKind::CastBitPatternFromBridgeObject:
    if (!Types.empty()) return nullptr;
    return getCastFromBridgeObjectOperation(Context, Id, BV);
      
  case BuiltinValueKind::CastReference:
    if (!Types.empty()) return nullptr;
    return getCastReferenceOperation(Context, Id);

  case BuiltinValueKind::ReinterpretCast:
    if (!Types.empty()) return nullptr;
    return getReinterpretCastOperation(Context, Id);
      
  case BuiltinValueKind::AddressOf:
  case BuiltinValueKind::UnprotectedAddressOf:
    if (!Types.empty()) return nullptr;
    return getAddressOfOperation(Context, Id);

  case BuiltinValueKind::LegacyCondFail:
    return getLegacyCondFailOperation(Context, Id);

  case BuiltinValueKind::AddressOfBorrow:
  case BuiltinValueKind::AddressOfBorrowOpaque:
  case BuiltinValueKind::UnprotectedAddressOfBorrow:
  case BuiltinValueKind::UnprotectedAddressOfBorrowOpaque:
    if (!Types.empty()) return nullptr;
    return getAddressOfBorrowOperation(Context, Id);

  case BuiltinValueKind::CondFailMessage:
    return getCondFailOperation(Context, Id);

  case BuiltinValueKind::AssertConf:
    return getAssertConfOperation(Context, Id);
      
  case BuiltinValueKind::FixLifetime:
    return getFixLifetimeOperation(Context, Id);
      
  case BuiltinValueKind::CanBeObjCClass:
    return getCanBeObjCClassOperation(Context, Id);
      
  case BuiltinValueKind::CondUnreachable:
  case BuiltinValueKind::Unreachable:
    return getUnreachableOperation(Context, Id);
      
  case BuiltinValueKind::ZeroInitializer:
    return getZeroInitializerOperation(Context, Id);
      
  case BuiltinValueKind::Once:
  case BuiltinValueKind::OnceWithContext:
    return getOnceOperation(Context, Id, BV == BuiltinValueKind::OnceWithContext);

  case BuiltinValueKind::WillThrow:
  case BuiltinValueKind::ErrorInMain:
    return getVoidErrorOperation(Context, Id);

  case BuiltinValueKind::UnexpectedError:
    return getUnexpectedErrorOperation(Context, Id);

  case BuiltinValueKind::ExtractElement:
    if (Types.size() != 2) return nullptr;
    return getExtractElementOperation(Context, Id, Types[0], Types[1]);

  case BuiltinValueKind::InsertElement:
    if (Types.size() != 3) return nullptr;
    return getInsertElementOperation(Context, Id, Types[0], Types[1], Types[2]);
      
  case BuiltinValueKind::ShuffleVector:
    if (Types.size() != 2) return nullptr;
    return getShuffleVectorOperation(Context, Id, Types[0], Types[1]);

  case BuiltinValueKind::StaticReport:
    if (!Types.empty()) return nullptr;
    return getStaticReportOperation(Context, Id);

  case BuiltinValueKind::SToSCheckedTrunc:
  case BuiltinValueKind::SToUCheckedTrunc:
    if (Types.size() != 2) return nullptr;
    return getCheckedTruncOperation(Context, Id, Types[0], Types[1], true);

  case BuiltinValueKind::UToSCheckedTrunc:
  case BuiltinValueKind::UToUCheckedTrunc:
    if (Types.size() != 2) return nullptr;
    return getCheckedTruncOperation(Context, Id, Types[0], Types[1], false);

  case BuiltinValueKind::ClassifyBridgeObject:
    if (!Types.empty()) return nullptr;
    return getClassifyBridgeObject(Context, Id);
  case BuiltinValueKind::ValueToBridgeObject:
    if (!Types.empty())
      return nullptr;
    return getValueToBridgeObject(Context, Id);

  case BuiltinValueKind::COWBufferForReading:
    return getCOWBufferForReading(Context, Id);

  case BuiltinValueKind::ApplyDerivative:
  case BuiltinValueKind::ApplyTranspose:
    llvm_unreachable("Handled above");

  case BuiltinValueKind::OnFastPath:
    return getOnFastPath(Context, Id);

  case BuiltinValueKind::IntToFPWithOverflow:
    if (Types.size() != 2) return nullptr;
    return getIntToFPWithOverflowOperation(Context, Id, Types[0], Types[1]);

  case BuiltinValueKind::BitWidth:
    if (Types.size() != 1) return nullptr;
    return getBitWidthOperation(Context, Id, Types[0]);

  case BuiltinValueKind::IsNegative:
    if (Types.size() != 1) return nullptr;
    return getIsNegativeOperation(Context, Id, Types[0]);

  case BuiltinValueKind::WordAtIndex:
    if (Types.size() != 1) return nullptr;
    return getWordAtIndexOperation(Context, Id, Types[0]);

  case BuiltinValueKind::GetObjCTypeEncoding:
    return getGetObjCTypeEncodingOperation(Context, Id);

  case BuiltinValueKind::GlobalStringTablePointer:
    return getGlobalStringTablePointer(Context, Id);

  case BuiltinValueKind::ConvertStrongToUnownedUnsafe:
    return getConvertStrongToUnownedUnsafe(Context, Id);

  case BuiltinValueKind::ConvertUnownedUnsafeToGuaranteed:
    return getConvertUnownedUnsafeToGuaranteed(Context, Id);

  case BuiltinValueKind::GetCurrentAsyncTask:
    return getGetCurrentAsyncTask(Context, Id);

  case BuiltinValueKind::GetCurrentExecutor:
    return getGetCurrentExecutor(Context, Id);

  case BuiltinValueKind::CancelAsyncTask:
    return getCancelAsyncTask(Context, Id);

  case BuiltinValueKind::CreateAsyncTask:
    return getCreateAsyncTask(Context, Id);

  case BuiltinValueKind::CreateAsyncTaskInGroup:
    return getCreateAsyncTaskInGroup(Context, Id);

  case BuiltinValueKind::TaskRunInline:
    return getTaskRunInline(Context, Id);

  case BuiltinValueKind::TargetOSVersionAtLeast:
    return getTargetOSVersionAtLeast(Context, Id);

  case BuiltinValueKind::ConvertTaskToJob:
    return getConvertTaskToJob(Context, Id);

  case BuiltinValueKind::BuildMainActorExecutorRef:
    return getBuildMainActorExecutorRef(Context, Id);

  case BuiltinValueKind::BuildDefaultActorExecutorRef:
    return getBuildDefaultActorExecutorRef(Context, Id);

  case BuiltinValueKind::BuildOrdinarySerialExecutorRef:
    return getBuildOrdinarySerialExecutorRef(Context, Id);
  case BuiltinValueKind::BuildComplexEqualitySerialExecutorRef:
    return getBuildComplexEqualitySerialExecutorRef(Context, Id);

  case BuiltinValueKind::PoundAssert:
    return getPoundAssert(Context, Id);

  case BuiltinValueKind::TSanInoutAccess:
    return getTSanInoutAccess(Context, Id);

  case BuiltinValueKind::Swift3ImplicitObjCEntrypoint:
    return getBuiltinFunction(Id,
                              {},
                              TupleType::getEmpty(Context));

  case BuiltinValueKind::TypePtrAuthDiscriminator:
    return getTypePtrAuthDiscriminator(Context, Id);
    
  case BuiltinValueKind::TypeJoin:
    return getTypeJoinOperation(Context, Id);

  case BuiltinValueKind::TypeJoinInout:
    return getTypeJoinInoutOperation(Context, Id);

  case BuiltinValueKind::TypeJoinMeta:
    return getTypeJoinMetaOperation(Context, Id);

  case BuiltinValueKind::TriggerFallbackDiagnostic:
    return getTriggerFallbackDiagnosticOperation(Context, Id);

  case BuiltinValueKind::InitializeDefaultActor:
  case BuiltinValueKind::InitializeNonDefaultDistributedActor:
  case BuiltinValueKind::DestroyDefaultActor:
    return getDefaultActorInitDestroy(Context, Id);

  case BuiltinValueKind::InitializeDistributedRemoteActor:
    return getDistributedActorInitializeRemote(Context, Id);

  case BuiltinValueKind::StartAsyncLet:
  case BuiltinValueKind::StartAsyncLetWithLocalBuffer:
    return getStartAsyncLet(Context, Id);

  case BuiltinValueKind::EndAsyncLet:
  case BuiltinValueKind::EndAsyncLetLifetime:
    return getEndAsyncLet(Context, Id);

  case BuiltinValueKind::CreateTaskGroup:
    return getCreateTaskGroup(Context, Id);
  case BuiltinValueKind::CreateTaskGroupWithFlags:
    return getCreateTaskGroupWithFlags(Context, Id);

  case BuiltinValueKind::DestroyTaskGroup:
    return getDestroyTaskGroup(Context, Id);

  case BuiltinValueKind::ResumeNonThrowingContinuationReturning:
  case BuiltinValueKind::ResumeThrowingContinuationReturning:
    return getResumeContinuationReturning(Context, Id);

  case BuiltinValueKind::ResumeThrowingContinuationThrowing:
    return getResumeContinuationThrowing(Context, Id);

  case BuiltinValueKind::WithUnsafeContinuation:
    return getWithUnsafeContinuation(Context, Id, /*throws=*/false);

  case BuiltinValueKind::WithUnsafeThrowingContinuation:
    return getWithUnsafeContinuation(Context, Id, /*throws=*/true);

  case BuiltinValueKind::HopToActor:
    return getHopToActor(Context, Id);

  case BuiltinValueKind::AutoDiffCreateLinearMapContext:
    return getAutoDiffCreateLinearMapContext(Context, Id);

  case BuiltinValueKind::AutoDiffProjectTopLevelSubcontext:
    return getAutoDiffProjectTopLevelSubcontext(Context, Id);

  case BuiltinValueKind::AutoDiffAllocateSubcontext:
    return getAutoDiffAllocateSubcontext(Context, Id);
  }

  llvm_unreachable("bad builtin value!");
}

StringRef swift::getBuiltinName(BuiltinValueKind ID) {
  switch (ID) {
  case BuiltinValueKind::None:
    llvm_unreachable("no builtin kind");
#define BUILTIN(Id, Name, Attrs) \
  case BuiltinValueKind::Id: \
    return Name;
#include "swift/AST/Builtins.def"
  }
  llvm_unreachable("bad BuiltinValueKind");
}

bool swift::isPolymorphicBuiltin(BuiltinValueKind id) {
  switch (id) {
  case BuiltinValueKind::None:
    llvm_unreachable("no builtin kind");
#define BUILTIN(Id, Name, Attrs)                                               \
  case BuiltinValueKind::Id:                                                   \
    return false;
#define BUILTIN_BINARY_OPERATION_POLYMORPHIC(Id, Name)                         \
  case BuiltinValueKind::Id:                                                   \
    return true;
#include "swift/AST/Builtins.def"
  }
  llvm_unreachable("bad BuiltinValueKind");
}

BuiltinTypeKind BuiltinType::getBuiltinTypeKind() const {
  // If we do not have a vector or an integer our job is easy.
  return BuiltinTypeKind(std::underlying_type<TypeKind>::type(getKind()));
}

StringRef BuiltinType::getTypeName(SmallVectorImpl<char> &result,
                                   bool prependBuiltinNamespace) const {
#ifdef MAYBE_GET_NAMESPACED_BUILTIN
#error                                                                         \
    "We define MAYBE_GET_NAMESPACED_BUILTIN here. Do not define before this?!"
#endif
#define MAYBE_GET_NAMESPACED_BUILTIN(NAME)                                     \
  ((prependBuiltinNamespace) ? NAME : NAME.getWithoutPrefix())

  llvm::raw_svector_ostream printer(result);
  switch (getBuiltinTypeKind()) {
  case BuiltinTypeKind::BuiltinRawPointer:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_RAWPOINTER);
    break;
  case BuiltinTypeKind::BuiltinRawUnsafeContinuation:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_RAWUNSAFECONTINUATION);
    break;
  case BuiltinTypeKind::BuiltinJob:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_JOB);
    break;
  case BuiltinTypeKind::BuiltinExecutor:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_EXECUTOR);
    break;
  case BuiltinTypeKind::BuiltinDefaultActorStorage:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_DEFAULTACTORSTORAGE);
    break;
  case BuiltinTypeKind::BuiltinNonDefaultDistributedActorStorage:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_NONDEFAULTDISTRIBUTEDACTORSTORAGE);
    break;
  case BuiltinTypeKind::BuiltinPackIndex:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_PACKINDEX);
    break;
  case BuiltinTypeKind::BuiltinNativeObject:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_NATIVEOBJECT);
    break;
  case BuiltinTypeKind::BuiltinBridgeObject:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_BRIDGEOBJECT);
    break;
  case BuiltinTypeKind::BuiltinUnsafeValueBuffer:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(
        BUILTIN_TYPE_NAME_UNSAFEVALUEBUFFER);
    break;
  case BuiltinTypeKind::BuiltinIntegerLiteral:
    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_INTLITERAL);
    break;
  case BuiltinTypeKind::BuiltinVector: {
    const auto *t = cast<const BuiltinVectorType>(this);
    llvm::SmallString<32> UnderlyingStrVec;
    StringRef UnderlyingStr;
    {
      // FIXME: Ugly hack: remove the .Builtin from the element type.
      {
        llvm::raw_svector_ostream UnderlyingOS(UnderlyingStrVec);
        t->getElementType().print(UnderlyingOS);
      }
      if (UnderlyingStrVec.startswith(BUILTIN_TYPE_NAME_PREFIX))
        UnderlyingStr = UnderlyingStrVec.substr(8);
      else
        UnderlyingStr = UnderlyingStrVec;
    }

    printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_VEC)
            << t->getNumElements() << "x" << UnderlyingStr;
    break;
  }
  case BuiltinTypeKind::BuiltinInteger: {
    auto width = cast<const BuiltinIntegerType>(this)->getWidth();
    if (width.isFixedWidth()) {
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_INT)
              << width.getFixedWidth();
      break;
    }

    if (width.isPointerWidth()) {
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_WORD);
      break;
    }

    llvm_unreachable("impossible bit width");
  }
  case BuiltinTypeKind::BuiltinFloat: {
    switch (cast<const BuiltinFloatType>(this)->getFPKind()) {
    case BuiltinFloatType::IEEE16:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT) << "16";
      break;
    case BuiltinFloatType::IEEE32:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT) << "32";
      break;
    case BuiltinFloatType::IEEE64:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT) << "64";
      break;
    case BuiltinFloatType::IEEE80:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT) << "80";
      break;
    case BuiltinFloatType::IEEE128:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT) << "128";
      break;
    case BuiltinFloatType::PPC128:
      printer << MAYBE_GET_NAMESPACED_BUILTIN(BUILTIN_TYPE_NAME_FLOAT_PPC)
              << "128";
      break;
    }
    break;
  }
  }
#undef MAYBE_GET_NAMESPACED_BUILTIN

  return printer.str();
}
