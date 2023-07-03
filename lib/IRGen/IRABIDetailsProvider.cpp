//===--- IRABIDetailsProvider.cpp - Get ABI details for decls ---*- C++ -*-===//
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

#include "swift/IRGen/IRABIDetailsProvider.h"
#include "Callee.h"
#include "FixedTypeInfo.h"
#include "GenEnum.h"
#include "GenPointerAuth.h"
#include "GenType.h"
#include "GenericRequirement.h"
#include "IRGen.h"
#include "IRGenModule.h"
#include "MetadataLayout.h"
#include "NativeConventionSchema.h"

// FIXME: This include should removed once getFunctionLoweredSignature() is
//        updated to take a different approach.
#include "../SILGen/SILGen.h"

#include "swift/AST/ASTContext.h"
#include "swift/AST/IRGenOptions.h"
#include "swift/AST/ParameterList.h"
#include "swift/AST/Types.h"
#include "swift/IRGen/Linking.h"
#include "swift/SIL/SILFunctionBuilder.h"
#include "swift/SIL/SILModule.h"
#include "swift/Subsystems.h"
#include "clang/CodeGen/ModuleBuilder.h"
#include "clang/CodeGen/SwiftCallingConv.h"
#include "llvm/IR/DerivedTypes.h"

using namespace swift;
using namespace irgen;

static llvm::Optional<Type>
getPrimitiveTypeFromLLVMType(ASTContext &ctx, const llvm::Type *type) {
  if (const auto *intType = dyn_cast<llvm::IntegerType>(type)) {
    switch (intType->getBitWidth()) {
    case 1:
      return ctx.getBoolType();
    case 8:
      return ctx.getUInt8Type();
    case 16:
      return ctx.getUInt16Type();
    case 32:
      return ctx.getUInt32Type();
    case 64:
      return ctx.getUInt64Type();
    default:
      return llvm::None;
    }
  } else if (type->isFloatTy()) {
    return ctx.getFloatType();
  } else if (type->isDoubleTy()) {
    return ctx.getDoubleType();
  } else if (type->isPointerTy()) {
    return ctx.getOpaquePointerType();
  }
  // FIXME: Handle vector type.
  return llvm::None;
}

namespace swift {

class IRABIDetailsProviderImpl {
public:
  IRABIDetailsProviderImpl(ModuleDecl &mod, const IRGenOptions &opts)
      : typeConverter(mod, /*addressLowered=*/true),
        silMod(SILModule::createEmptyModule(&mod, typeConverter, silOpts)),
        IRGen(opts, *silMod), IGM(IRGen, IRGen.createTargetMachine()) {}

  llvm::Optional<IRABIDetailsProvider::SizeAndAlignment>
  getTypeSizeAlignment(const NominalTypeDecl *TD) {
    auto *TI = &IGM.getTypeInfoForUnlowered(TD->getDeclaredTypeInContext());
    auto *fixedTI = dyn_cast<FixedTypeInfo>(TI);
    if (!fixedTI)
      return llvm::None;
    return IRABIDetailsProvider::SizeAndAlignment{
        fixedTI->getFixedSize().getValue(),
        fixedTI->getFixedAlignment().getValue()};
  }

  IRABIDetailsProvider::FunctionABISignature
  getTypeMetadataAccessFunctionSignature() {
    auto &ctx = IGM.getSwiftModule()->getASTContext();
    llvm::StructType *responseTy = IGM.getTypeMetadataResponseTy();
    IRABIDetailsProvider::TypeRecordABIRepresentation::MemberVectorTy members;
    for (auto *elementTy : responseTy->elements())
      members.push_back(*getPrimitiveTypeFromLLVMType(ctx, elementTy));
    auto returnTy =
        IRABIDetailsProvider::TypeRecordABIRepresentation(std::move(members));
    auto paramTy = IRABIDetailsProvider::TypeRecordABIRepresentation(
        {*getPrimitiveTypeFromLLVMType(ctx,
                                       IGM.getTypeMetadataRequestParamTy())});
    return {returnTy, {paramTy}};
  }

  SmallVector<GenericRequirement, 2>
  getTypeMetadataAccessFunctionGenericRequirementParameters(
      NominalTypeDecl *nominal) {
    GenericTypeRequirements requirements(IGM, nominal);
    SmallVector<GenericRequirement, 2> result;
    for (const auto &req : requirements.getRequirements())
      result.push_back(req);
    return result;
  }

  llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
  getEnumTagMapping(const EnumDecl *ED) {
    llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
        elements;
    auto &enumImplStrat = getEnumImplStrategy(
        IGM, ED->DeclContext::getDeclaredTypeInContext()->getCanonicalType());

    for (auto *element : ED->getAllElements()) {
      auto tagIdx = enumImplStrat.getTagIndex(element);
      auto *global = cast<llvm::GlobalVariable>(
          IGM.getAddrOfEnumCase(element, NotForDefinition).getAddress());
      elements.insert({element, {tagIdx, global->getName()}});
    }

    return elements;
  }

  llvm::Optional<LoweredFunctionSignature>
  getFunctionLoweredSignature(AbstractFunctionDecl *fd) {
    auto declRef = SILDeclRef(fd);
    auto function = Lowering::SILGenModule(*silMod, declRef.getModuleContext())
                        .getFunction(declRef, swift::NotForDefinition);

    IGM.lowerSILFunction(function);
    auto silFuncType = function->getLoweredFunctionType();
    // FIXME: Async function support.
    if (silFuncType->isAsync())
      return llvm::None;
    if (silFuncType->getLanguage() != SILFunctionLanguage::Swift)
      return llvm::None;

    // FIXME: Tuple parameter mapping support.
    llvm::SmallVector<const ParamDecl *, 8> silParamMapping;
    for (auto param : *fd->getParameters()) {
      if (auto *tuple =
              param->getType()->getDesugaredType()->getAs<TupleType>()) {
        if (tuple->getNumElements() > 0)
          return llvm::None;
      }
    }

    auto funcPointerKind =
        FunctionPointerKind(FunctionPointerKind::BasicKind::Function);

    auto *abiDetails = new (signatureExpansions.Allocate())
        SignatureExpansionABIDetails(Signature::getUncachedABIDetails(
            IGM, silFuncType, funcPointerKind));

    auto result = LoweredFunctionSignature(fd, *this, *abiDetails);
    // Save metadata source types to avoid keeping the SIL func around.
    for (const auto &typeSource :
         abiDetails->polymorphicSignatureExpandedTypeSources) {
      typeSource.visit(
          [&](const GenericRequirement &reqt) {},
          [&](const MetadataSource &metadataSource) {
            auto index = metadataSource.getParamIndex();
            auto canType =
                silFuncType->getParameters()[index].getInterfaceType();
            result.metadataSourceTypes.push_back(canType);
          });
    }
    // Verify that the signature param count matches the IR param count.
    size_t signatureParamCount = 0;
    result.visitParameterList(
        [&](const LoweredFunctionSignature::IndirectResultValue
                &indirectResult) { ++signatureParamCount; },
        [&](const LoweredFunctionSignature::DirectParameter &param) {
          param.enumerateRecordMembers([&](clang::CharUnits, clang::CharUnits,
                                           Type) { ++signatureParamCount; });
        },
        [&](const LoweredFunctionSignature::IndirectParameter &param) {
          ++signatureParamCount;
        },
        [&](const LoweredFunctionSignature::GenericRequirementParameter
                &genericRequirementParam) { ++signatureParamCount; },
        [&](const LoweredFunctionSignature::MetadataSourceParameter
                &metadataSrcParam) { ++signatureParamCount; },
        [&](const LoweredFunctionSignature::ContextParameter &) {
          ++signatureParamCount;
        },
        [&](const LoweredFunctionSignature::ErrorResultValue &) {
          ++signatureParamCount;
        });
    // Return nothing if we were unable to represent the exact signature
    // parameters.
    if (signatureParamCount != abiDetails->numParamIRTypesInSignature)
      return llvm::None;

    return result;
  }

  using MethodDispatchInfo = IRABIDetailsProvider::MethodDispatchInfo;

  llvm::Optional<MethodDispatchInfo::PointerAuthDiscriminator>
  getMethodPointerAuthInfo(const AbstractFunctionDecl *funcDecl,
                           SILDeclRef method) {
    // FIXME: Async support.
    if (funcDecl->hasAsync())
      return llvm::None;
    const auto &schema = IGM.getOptions().PointerAuth.SwiftClassMethods;
    if (!schema)
      return llvm::None;
    auto discriminator =
        PointerAuthInfo::getOtherDiscriminator(IGM, schema, method);
    return MethodDispatchInfo::PointerAuthDiscriminator{
        discriminator->getZExtValue()};
  }

  llvm::Optional<MethodDispatchInfo>
  getMethodDispatchInfo(const AbstractFunctionDecl *funcDecl) {
    if (funcDecl->isSemanticallyFinal())
      return MethodDispatchInfo::direct();
    // If this is an override of an existing method, then lookup
    // its base method in its base class.
    if (auto *overridenDecl = funcDecl->getOverriddenDecl())
      funcDecl = overridenDecl;
    auto *parentClass = dyn_cast<ClassDecl>(funcDecl->getDeclContext());
    if (!parentClass)
      return MethodDispatchInfo::direct();
    // Resilient indirect calls should go through a thunk.
    if (parentClass->hasResilientMetadata())
      return MethodDispatchInfo::thunk(
          LinkEntity::forDispatchThunk(
              SILDeclRef(const_cast<AbstractFunctionDecl *>(funcDecl)))
              .mangleAsString());
    auto &layout = IGM.getMetadataLayout(parentClass);
    if (!isa<ClassMetadataLayout>(layout))
      return {};
    auto &classLayout = cast<ClassMetadataLayout>(layout);
    auto silDecl = SILDeclRef(const_cast<AbstractFunctionDecl *>(funcDecl));
    auto *mi = classLayout.getStoredMethodInfoIfPresent(silDecl);
    if (!mi)
      return {};
    switch (mi->TheKind) {
    case ClassMetadataLayout::MethodInfo::Kind::DirectImpl:
      return MethodDispatchInfo::direct();
    case ClassMetadataLayout::MethodInfo::Kind::Offset:
      if (mi->TheOffset.isStatic()) {
        return MethodDispatchInfo::indirectVTableStaticOffset(
            /*offset=*/mi->TheOffset.getStaticOffset().getValue(),
            getMethodPointerAuthInfo(funcDecl, silDecl));
      }
      assert(mi->TheOffset.isDynamic());
      return MethodDispatchInfo::indirectVTableRelativeOffset(
          /*offset=*/mi->TheOffset.getRelativeOffset().getValue(),
          /*symbolName=*/
          LinkEntity::forClassMetadataBaseOffset(parentClass).mangleAsString(),
          getMethodPointerAuthInfo(funcDecl, silDecl));
    }
    llvm_unreachable("invalid kind");
  }

  Type getClassBaseOffsetSymbolType() const {
    return *getPrimitiveTypeFromLLVMType(
        silMod->getASTContext(), IGM.ClassMetadataBaseOffsetTy->elements()[0]);
  }

  Lowering::TypeConverter typeConverter;
  // Default silOptions are sufficient, as we don't need to generated SIL.
  SILOptions silOpts;
  std::unique_ptr<SILModule> silMod;
  IRGenerator IRGen;
  IRGenModule IGM;
  llvm::SpecificBumpPtrAllocator<SignatureExpansionABIDetails>
      signatureExpansions;
};

} // namespace swift

LoweredFunctionSignature::LoweredFunctionSignature(
    const AbstractFunctionDecl *FD, IRABIDetailsProviderImpl &owner,
    const irgen::SignatureExpansionABIDetails &abiDetails)
    : FD(FD), owner(owner), abiDetails(abiDetails) {}

LoweredFunctionSignature::DirectResultType::DirectResultType(
    IRABIDetailsProviderImpl &owner, const irgen::TypeInfo &typeDetails)
    : owner(owner), typeDetails(typeDetails) {}

bool LoweredFunctionSignature::DirectResultType::enumerateRecordMembers(
    llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)> callback)
    const {
  auto &schema = typeDetails.nativeReturnValueSchema(owner.IGM);
  assert(!schema.requiresIndirect());
  bool hasError = false;
  schema.enumerateComponents(
      [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
        auto primitiveType = getPrimitiveTypeFromLLVMType(
            owner.IGM.getSwiftModule()->getASTContext(), type);
        if (!primitiveType) {
          hasError = true;
          return;
        }
        callback(offset, end, *primitiveType);
      });
  return hasError;
}

LoweredFunctionSignature::DirectParameter::DirectParameter(
    IRABIDetailsProviderImpl &owner, const irgen::TypeInfo &typeDetails,
    const ParamDecl &paramDecl)
    : owner(owner), typeDetails(typeDetails), paramDecl(paramDecl) {}

LoweredFunctionSignature::IndirectParameter::IndirectParameter(
    const ParamDecl &paramDecl)
    : paramDecl(paramDecl) {}

bool LoweredFunctionSignature::DirectParameter::enumerateRecordMembers(
    llvm::function_ref<void(clang::CharUnits, clang::CharUnits, Type)> callback)
    const {
  auto &schema = typeDetails.nativeParameterValueSchema(owner.IGM);
  assert(!schema.requiresIndirect());
  bool hasError = false;
  schema.enumerateComponents(
      [&](clang::CharUnits offset, clang::CharUnits end, llvm::Type *type) {
        auto primitiveType = getPrimitiveTypeFromLLVMType(
            owner.IGM.getSwiftModule()->getASTContext(), type);
        if (!primitiveType) {
          hasError = true;
          return;
        }
        callback(offset, end, *primitiveType);
      });
  return hasError;
}

LoweredFunctionSignature::GenericRequirementParameter::
    GenericRequirementParameter(const GenericRequirement &requirement)
    : requirement(requirement) {}

LoweredFunctionSignature::MetadataSourceParameter::MetadataSourceParameter(
    const CanType &type)
    : type(type) {}

llvm::Optional<LoweredFunctionSignature::DirectResultType>
LoweredFunctionSignature::getDirectResultType() const {
  if (!abiDetails.directResult)
    return llvm::None;
  return DirectResultType(owner, abiDetails.directResult->typeInfo);
}

size_t LoweredFunctionSignature::getNumIndirectResultValues() const {
  return abiDetails.indirectResults.size();
}

void LoweredFunctionSignature::visitParameterList(
    llvm::function_ref<void(const IndirectResultValue &)> indirectResultVisitor,
    llvm::function_ref<void(const DirectParameter &)> directParamVisitor,
    llvm::function_ref<void(const IndirectParameter &)> indirectParamVisitor,
    llvm::function_ref<void(const GenericRequirementParameter &)>
        genericRequirementVisitor,
    llvm::function_ref<void(const MetadataSourceParameter &)>
        metadataSourceVisitor,
    llvm::function_ref<void(const ContextParameter &)> contextParamVisitor,
    llvm::function_ref<void(const ErrorResultValue &)> errorResultVisitor)
    const {
  // Indirect result values come before parameters.
  for (const auto &r : abiDetails.indirectResults)
    indirectResultVisitor(IndirectResultValue(r.hasSRet));

  // Traverse ABI parameters, mapping them back to the AST parameters.
  llvm::SmallVector<const ParamDecl *, 8> silParamMapping;
  for (auto param : *FD->getParameters()) {
    // FIXME: tuples map to more than one sil param (but they're not yet
    // representable by the consumer).
    if (!param->getInterfaceType()->isVoid())
      silParamMapping.push_back(param);
  }
  size_t currentSilParam = 0;
  for (const auto &abiParam : abiDetails.parameters) {
    bool isIndirect = true;
    if (!isIndirectFormalParameter(abiParam.convention)) {
      const auto &schema =
          abiParam.typeInfo.get().nativeParameterValueSchema(owner.IGM);
      if (!schema.requiresIndirect()) {
        // Skip ABI parameters with empty native representation, as they're not
        // emitted in the LLVM IR signature.
        if (schema.empty())
          continue;
        isIndirect = false;
      }
    }

    const ParamDecl *paramDecl = abiParam.isSelf
                                     ? FD->getImplicitSelfDecl()
                                     : silParamMapping[currentSilParam];
    ++currentSilParam;
    if (!isIndirect) {
      DirectParameter param(owner, abiParam.typeInfo, *paramDecl);
      directParamVisitor(param);
    } else {
      IndirectParameter param(*paramDecl);
      indirectParamVisitor(param);
    }
  }
  // FIXME: Use one assert for indirect self too.
  if (FD->getImplicitSelfDecl())
    assert(currentSilParam == (silParamMapping.size() + 1) ||
           currentSilParam == silParamMapping.size());
  else
    assert(currentSilParam == silParamMapping.size());

  // Generic requirements come next.
  size_t metadataSourceIndex = 0;
  for (const auto &typeSource :
       abiDetails.polymorphicSignatureExpandedTypeSources) {
    typeSource.visit(
        [&](const GenericRequirement &reqt) {
          genericRequirementVisitor(GenericRequirementParameter(reqt));
        },
        [&](const MetadataSource &metadataSource) {
          metadataSourceVisitor(MetadataSourceParameter(
              metadataSourceTypes[metadataSourceIndex]));
          ++metadataSourceIndex;
        });
  }

  if (abiDetails.hasTrailingSelfParam) {
    assert(!abiDetails.hasContextParam);
    assert(FD->hasImplicitSelfDecl());
    indirectParamVisitor(IndirectParameter(*FD->getImplicitSelfDecl()));
  } else if (abiDetails.hasContextParam) {
    contextParamVisitor(ContextParameter());
  }

  if (abiDetails.hasErrorResult)
    errorResultVisitor(ErrorResultValue());
}

IRABIDetailsProvider::IRABIDetailsProvider(ModuleDecl &mod,
                                           const IRGenOptions &opts)
    : impl(std::make_unique<IRABIDetailsProviderImpl>(mod, opts)) {}

IRABIDetailsProvider::~IRABIDetailsProvider() {}

llvm::Optional<IRABIDetailsProvider::SizeAndAlignment>
IRABIDetailsProvider::getTypeSizeAlignment(const NominalTypeDecl *TD) {
  return impl->getTypeSizeAlignment(TD);
}

llvm::Optional<LoweredFunctionSignature>
IRABIDetailsProvider::getFunctionLoweredSignature(AbstractFunctionDecl *fd) {
  return impl->getFunctionLoweredSignature(fd);
}

IRABIDetailsProvider::FunctionABISignature
IRABIDetailsProvider::getTypeMetadataAccessFunctionSignature() {
  return impl->getTypeMetadataAccessFunctionSignature();
}

SmallVector<GenericRequirement, 2>
IRABIDetailsProvider::getTypeMetadataAccessFunctionGenericRequirementParameters(
    NominalTypeDecl *nominal) {
  return impl->getTypeMetadataAccessFunctionGenericRequirementParameters(
      nominal);
}

llvm::MapVector<EnumElementDecl *, IRABIDetailsProvider::EnumElementInfo>
IRABIDetailsProvider::getEnumTagMapping(const EnumDecl *ED) {
  return impl->getEnumTagMapping(ED);
}

llvm::Optional<IRABIDetailsProvider::MethodDispatchInfo>
IRABIDetailsProvider::getMethodDispatchInfo(
    const AbstractFunctionDecl *funcDecl) {
  return impl->getMethodDispatchInfo(funcDecl);
}

Type IRABIDetailsProvider::getClassBaseOffsetSymbolType() const {
  return impl->getClassBaseOffsetSymbolType();
}
