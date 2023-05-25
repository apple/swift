//===--- SILGlobalVariable.cpp - Defines SILGlobalVariable structure ------===//
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

#include "swift/SIL/SILBridging.h"
#include "swift/SIL/SILFunction.h"
#include "swift/SIL/SILGlobalVariable.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILLinkage.h"
#include "swift/SIL/SILModule.h"

using namespace swift;

SwiftMetatype SILGlobalVariable::registeredMetatype;
    
SILGlobalVariable *SILGlobalVariable::create(SILModule &M, SILLinkage linkage,
                                             IsSerialized_t isSerialized,
                                             StringRef name,
                                             SILType loweredType,
                                             Optional<SILLocation> loc,
                                             VarDecl *Decl) {
  // Get a StringMapEntry for the variable.
  llvm::StringMapEntry<SILGlobalVariable*> *entry = nullptr;
  assert(!name.empty() && "Name required");

  entry = &*M.GlobalVariableMap.insert(std::make_pair(name, nullptr)).first;
  assert(!entry->getValue() && "global variable already exists");
  name = entry->getKey();

  auto var = new (M) SILGlobalVariable(M, linkage, isSerialized, name,
                                       loweredType, loc, Decl);

  if (entry) entry->setValue(var);
  return var;
}


SILGlobalVariable::SILGlobalVariable(SILModule &Module, SILLinkage Linkage,
                                     IsSerialized_t isSerialized,
                                     StringRef Name, SILType LoweredType,
                                     Optional<SILLocation> Loc, VarDecl *Decl)
  : SwiftObjectHeader(registeredMetatype),
    Module(Module),
    Name(Name),
    LoweredType(LoweredType),
    Location(Loc.value_or(SILLocation::invalid())),
    Linkage(unsigned(Linkage)),
    HasLocation(Loc.has_value()),
    VDecl(Decl) {
  setSerialized(isSerialized);
  IsDeclaration = isAvailableExternally(Linkage);
  setLet(Decl ? Decl->isLet() : false);
  Module.silGlobals.push_back(this);
}

SILGlobalVariable::~SILGlobalVariable() {
  clear();
}

bool SILGlobalVariable::isPossiblyUsedExternally() const {
  SILLinkage linkage = getLinkage();
  return swift::isPossiblyUsedExternally(linkage, getModule().isWholeModule());
}

/// Get this global variable's fragile attribute.
IsSerialized_t SILGlobalVariable::isSerialized() const {
  return Serialized ? IsSerialized : IsNotSerialized;
}
void SILGlobalVariable::setSerialized(IsSerialized_t isSerialized) {
  Serialized = isSerialized ? 1 : 0;
}

/// Return the value that is written into the global variable.
SILInstruction *SILGlobalVariable::getStaticInitializerValue() {
  if (StaticInitializerBlock.empty())
    return nullptr;

  return &StaticInitializerBlock.back();
}

/// Return whether this variable corresponds to a Clang node.
bool SILGlobalVariable::hasClangNode() const {
  return (VDecl ? VDecl->hasClangNode() : false);
}

/// Return the Clang node associated with this variable if it has one.
ClangNode SILGlobalVariable::getClangNode() const {
  return (VDecl ? VDecl->getClangNode() : ClangNode());
}
const clang::Decl *SILGlobalVariable::getClangDecl() const {
  return (VDecl ? VDecl->getClangDecl() : nullptr);
}

//===----------------------------------------------------------------------===//
// Utilities for verification and optimization.
//===----------------------------------------------------------------------===//

static SILGlobalVariable *getStaticallyInitializedVariable(SILFunction *AddrF) {
  if (AddrF->isExternalDeclaration())
    return nullptr;

  auto *RetInst = cast<ReturnInst>(AddrF->findReturnBB()->getTerminator());
  auto *API = dyn_cast<AddressToPointerInst>(RetInst->getOperand());
  if (!API)
    return nullptr;
  auto *GAI = dyn_cast<GlobalAddrInst>(API->getOperand());
  if (!GAI)
    return nullptr;

  return GAI->getReferencedGlobal();
}

SILGlobalVariable *swift::getVariableOfGlobalInit(SILFunction *AddrF) {
  if (!AddrF->isGlobalInit())
    return nullptr;

  if (auto *SILG = getStaticallyInitializedVariable(AddrF))
    return SILG;

  // If the addressor contains a single "once" call, it calls globalinit_func,
  // and the globalinit_func is called by "once" from a single location,
  // continue; otherwise bail.
  BuiltinInst *CallToOnce;
  auto *InitF = findInitializer(AddrF, CallToOnce);

  if (!InitF)
    return nullptr;

  return getVariableOfStaticInitializer(InitF);
}

SILFunction *swift::getCalleeOfOnceCall(BuiltinInst *BI) {
  assert(BI->getNumOperands() == 2 && "once call should have 2 operands.");

  auto Callee = BI->getOperand(1);
  assert(Callee->getType().castTo<SILFunctionType>()->getRepresentation()
           == SILFunctionTypeRepresentation::CFunctionPointer &&
         "Expected C function representation!");

  if (auto *FR = dyn_cast<FunctionRefInst>(Callee))
    return FR->getReferencedFunction();

  return nullptr;
}

// Find the globalinit_func by analyzing the body of the addressor.
SILFunction *swift::findInitializer(SILFunction *AddrF,
                                    BuiltinInst *&CallToOnce) {
  // We only handle a single SILBasicBlock for now.
  if (AddrF->size() != 1)
    return nullptr;

  CallToOnce = nullptr;
  SILBasicBlock *BB = &AddrF->front();
  for (auto &I : *BB) {
    // Find the builtin "once" call.
    if (auto *BI = dyn_cast<BuiltinInst>(&I)) {
      const BuiltinInfo &Builtin =
        BI->getModule().getBuiltinInfo(BI->getName());
      if (Builtin.ID != BuiltinValueKind::Once)
        continue;

      // Bail if we have multiple "once" calls in the addressor.
      if (CallToOnce)
        return nullptr;

      CallToOnce = BI;
    }
  }
  if (!CallToOnce)
    return nullptr;
  SILFunction *callee = getCalleeOfOnceCall(CallToOnce);
  if (!callee->isGlobalInitOnceFunction())
    return nullptr;
  return callee;
}

SILGlobalVariable *swift::getVariableOfStaticInitializer(SILFunction *InitFunc) {
  // We only handle a single SILBasicBlock for now.
  if (InitFunc->size() != 1)
    return nullptr;

  for (auto &inst : InitFunc->front()) {
    if (auto *agi = dyn_cast<AllocGlobalInst>(&inst)) {
      return agi->getReferencedGlobal();
    }
  }
  return nullptr;
}

SILType
SILGlobalVariable::getLoweredTypeInContext(TypeExpansionContext context) const {
  auto ty = getLoweredType();
  if (!ty.getASTType()->hasOpaqueArchetype() ||
      !context.shouldLookThroughOpaqueTypeArchetypes())
    return ty;
  auto resultTy =
      getModule().Types.getTypeLowering(ty, context).getLoweredType();
  return resultTy.getCategoryType(ty.getCategory());
}
