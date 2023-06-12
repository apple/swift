//===--- SILInstructions.cpp - Instructions for SIL code ------------------===//
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
// This file defines the high-level SILInstruction classes used for SIL code.
//
//===----------------------------------------------------------------------===//

#include "swift/AST/Expr.h"
#include "swift/AST/ProtocolConformance.h"
#include "swift/Basic/AssertImplements.h"
#include "swift/Basic/Unicode.h"
#include "swift/Basic/type_traits.h"
#include "swift/SIL/DynamicCasts.h"
#include "swift/SIL/FormalLinkage.h"
#include "swift/SIL/Projection.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILCloner.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SIL/SILModule.h"
#include "swift/SIL/SILSymbolVisitor.h"
#include "swift/SIL/SILVisitor.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"

using namespace swift;
using namespace Lowering;

/// Allocate an instruction that inherits from llvm::TrailingObjects<>.
template <class Inst, class... TrailingTypes, class... CountTypes>
static void *allocateTrailingInst(SILFunction &F, CountTypes... counts) {
  return F.getModule().allocateInst(
             Inst::template totalSizeToAlloc<TrailingTypes...>(counts...),
             alignof(Inst));
}

namespace {
class TypeDependentOperandCollector {
  SmallVector<CanLocalArchetypeType, 4> rootLocalArchetypes;
  bool hasDynamicSelf = false;
public:
  void collect(CanType type);
  void collect(SubstitutionMap subs);
  void collect(SILType type) {
    collect(type.getASTType());
  }
  template <class T>
  void collect(ArrayRef<T> array) {
    for (auto &elt: array)
      collect(elt);
  }

  void collectAll() {}
  template <class T, class... Ts>
  void collectAll(T &&first, Ts &&...rest) {
    collect(first);
    collectAll(std::forward<Ts>(rest)...);
  }

  void addTo(SmallVectorImpl<SILValue> &typeDependentOperands,
             SILFunction &f);
};

}

/// Collect root open archetypes from a given type into \p RootLocalArchetypes.
/// \p RootLocalArchetypes is being used as a set. We don't use a real set type
/// here for performance reasons.
void TypeDependentOperandCollector::collect(CanType type) {
  if (!type)
    return;
  if (type->hasDynamicSelfType())
    hasDynamicSelf = true;
  if (!type->hasLocalArchetype())
    return;
  type.visit([&](CanType t) {
    if (const auto local = dyn_cast<LocalArchetypeType>(t)) {
      const auto root = local.getRoot();

      // Add this root local archetype if it was not seen yet.
      // We don't use a set here, because the number of open archetypes
      // is usually very small and using a real set may introduce too
      // much overhead.
      if (std::find(rootLocalArchetypes.begin(), rootLocalArchetypes.end(),
                    root) == rootLocalArchetypes.end())
        rootLocalArchetypes.push_back(root);
    }
  });
}

/// Collect type dependencies from the replacement types of a
/// substitution map.
void TypeDependentOperandCollector::collect(SubstitutionMap subs) {
  for (Type replacement : subs.getReplacementTypes()) {
    // Substitutions in SIL should really be canonical.
    auto ReplTy = replacement->getCanonicalType();
    collect(ReplTy);
  }
}

/// Given that we've collected a set of type dependencies, add operands
/// for those dependencies to the given vector.
void TypeDependentOperandCollector::addTo(SmallVectorImpl<SILValue> &operands,
                                          SILFunction &F) {
  size_t firstArchetypeOperand = operands.size();
  for (CanLocalArchetypeType archetype : rootLocalArchetypes) {
    SILValue def = F.getModule().getRootLocalArchetypeDef(archetype, &F);
    assert(def->getFunction() == &F &&
           "def of root local archetype is in wrong function");

    // The archetypes in rootLocalArchetypes have already been uniqued,
    // but a single instruction can open multiple archetypes (e.g.
    // open_pack_element), so we also unique the actual operand values.
    // As above, we assume there are very few values in practice and so
    // a linear scan is better than maintaining a set.
    if (std::find(operands.begin() + firstArchetypeOperand, operands.end(),
                  def) == operands.end())
      operands.push_back(def);
  }
  if (hasDynamicSelf)
    operands.push_back(F.getDynamicSelfMetadata());
}

/// Collects all root local archetypes from a type and a substitution list, and
/// forms a corresponding list of operands.
/// We need to know the number of root local archetypes to estimate the number
/// of corresponding operands for the instruction being formed, because we need
/// to reserve enough memory for these operands.
template <class... Sources>
static void collectTypeDependentOperands(
                      SmallVectorImpl<SILValue> &typeDependentOperands,
                      SILFunction &F, Sources &&... sources) {
  TypeDependentOperandCollector collector;
  collector.collectAll(std::forward<Sources>(sources)...);
  collector.addTo(typeDependentOperands, F);
}

//===----------------------------------------------------------------------===//
// SILInstruction Subclasses
//===----------------------------------------------------------------------===//

template <typename INST>
static void *allocateDebugVarCarryingInst(SILModule &M,
                                          Optional<SILDebugVariable> Var,
                                          ArrayRef<SILValue> Operands = {}) {
  return M.allocateInst(
      sizeof(INST) + (Var ? Var->Name.size() : 0) +
          (Var && Var->Type ? sizeof(SILType) : 0) +
          (Var && Var->Loc ? sizeof(SILLocation) : 0) +
          (Var && Var->Scope ? sizeof(const SILDebugScope *) : 0) +
          sizeof(SILDIExprElement) * (Var ? Var->DIExpr.getNumElements() : 0) +
          sizeof(Operand) * Operands.size(),
      alignof(INST));
}

TailAllocatedDebugVariable::TailAllocatedDebugVariable(
    Optional<SILDebugVariable> Var, char *buf, SILType *AuxVarType,
    SILLocation *DeclLoc, const SILDebugScope **DeclScope,
    SILDIExprElement *DIExprOps) {
  if (!Var) {
    Bits.RawValue = 0;
    return;
  }

  Bits.Data.HasValue = true;
  Bits.Data.Constant = Var->Constant;
  Bits.Data.ArgNo = Var->ArgNo;
  Bits.Data.Implicit = Var->Implicit;
  Bits.Data.NameLength = Var->Name.size();
  assert(Bits.Data.ArgNo == Var->ArgNo && "Truncation");
  assert(Bits.Data.NameLength == Var->Name.size() && "Truncation");
  memcpy(buf, Var->Name.data(), Bits.Data.NameLength);
  if (AuxVarType && Var->Type)
    *AuxVarType = *Var->Type;
  if (DeclLoc && Var->Loc)
    *DeclLoc = *Var->Loc;
  if (DeclScope && Var->Scope)
    *DeclScope = Var->Scope;
  if (DIExprOps) {
    llvm::ArrayRef<SILDIExprElement> Ops(Var->DIExpr.Elements);
    memcpy(DIExprOps, Ops.data(), sizeof(SILDIExprElement) * Ops.size());
  }
}

StringRef TailAllocatedDebugVariable::getName(const char *buf) const {
  if (Bits.Data.NameLength)
    return StringRef(buf, Bits.Data.NameLength);
  return StringRef();
}

Optional<SILDebugVariable>
SILDebugVariable::createFromAllocation(const AllocationInst *AI) {
  Optional<SILDebugVariable> VarInfo;
  if (const auto *ASI = dyn_cast_or_null<AllocStackInst>(AI))
    VarInfo = ASI->getVarInfo();
  // TODO: Support AllocBoxInst

  if (!VarInfo)
    return {};

  // TODO: Support variables with expressions.
  if (VarInfo->DIExpr)
    return {};

  // Coalesce the debug loc attached on AI into VarInfo
  SILType Type = AI->getType();
  SILLocation InstLoc = AI->getLoc();
  const SILDebugScope *InstDS = AI->getDebugScope();
  if (!VarInfo->Type)
    VarInfo->Type = Type;
  if (!VarInfo->Loc)
    VarInfo->Loc = InstLoc;
  if (!VarInfo->Scope)
    VarInfo->Scope = InstDS;

  return VarInfo;
}

AllocStackInst::AllocStackInst(SILDebugLocation Loc, SILType elementType,
                               ArrayRef<SILValue> TypeDependentOperands,
                               SILFunction &F, Optional<SILDebugVariable> Var,
                               bool hasDynamicLifetime, bool isLexical,
                               bool usesMoveableValueDebugInfo)
    : InstructionBase(Loc, elementType.getAddressType()),
      SILDebugVariableSupplement(Var ? Var->DIExpr.getNumElements() : 0,
                                 Var ? Var->Type.has_value() : false,
                                 Var ? Var->Loc.has_value() : false,
                                 Var ? Var->Scope != nullptr : false),
      // Initialize VarInfo with a temporary raw value of 0. The real
      // initialization can only be done after `numOperands` is set (see below).
      VarInfo(0) {
  sharedUInt8().AllocStackInst.dynamicLifetime = hasDynamicLifetime;
  sharedUInt8().AllocStackInst.lexical = isLexical;
  sharedUInt8().AllocStackInst.usesMoveableValueDebugInfo =
      usesMoveableValueDebugInfo || elementType.isMoveOnly();
  sharedUInt32().AllocStackInst.numOperands = TypeDependentOperands.size();

  // VarInfo must be initialized after
  // `sharedUInt32().AllocStackInst.numOperands`! Otherwise the trailing object
  // addresses are wrong.
  VarInfo = TailAllocatedDebugVariable(
      Var, getTrailingObjects<char>(), getTrailingObjects<SILType>(),
      getTrailingObjects<SILLocation>(),
      getTrailingObjects<const SILDebugScope *>(),
      getTrailingObjects<SILDIExprElement>());

  assert(sharedUInt32().AllocStackInst.numOperands ==
             TypeDependentOperands.size() &&
         "Truncation");
  auto *VD = Loc.getLocation().getAsASTNode<VarDecl>();
  if (Var && VD) {
    VarInfo.setImplicit(VD->isImplicit() || VarInfo.isImplicit());
  }
  TrailingOperandsList::InitOperandsList(getAllOperands().begin(), this,
                                         TypeDependentOperands);
}

AllocStackInst *AllocStackInst::create(SILDebugLocation Loc,
                                       SILType elementType, SILFunction &F,
                                       Optional<SILDebugVariable> Var,
                                       bool hasDynamicLifetime, bool isLexical,
                                       bool wasMoved) {
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F,
                               elementType.getASTType());
  void *Buffer = allocateDebugVarCarryingInst<AllocStackInst>(
      F.getModule(), Var, TypeDependentOperands);
  return ::new (Buffer)
      AllocStackInst(Loc, elementType, TypeDependentOperands, F, Var,
                     hasDynamicLifetime, isLexical, wasMoved);
}

VarDecl *AllocationInst::getDecl() const {
  return getLoc().getAsASTNode<VarDecl>();
}

DeallocStackInst *AllocStackInst::getSingleDeallocStack() const {
  DeallocStackInst *Dealloc = nullptr;
  for (auto *U : getUses()) {
    if (auto DS = dyn_cast<DeallocStackInst>(U->getUser())) {
      if (Dealloc == nullptr) {
        Dealloc = DS;
        continue;
      }
      // Already saw a dealloc_stack.
      return nullptr;
    }
  }
  return Dealloc;
}

AllocPackInst *AllocPackInst::create(SILDebugLocation loc,
                                     SILType packType,
                                     SILFunction &F) {
  assert(packType.isObject());
  assert(packType.is<SILPackType>() && "pack type must be lowered");
  auto resultType = packType.getAddressType();

  SmallVector<SILValue, 8> allOperands;
  collectTypeDependentOperands(allOperands, F, packType);

  auto size = totalSizeToAlloc<swift::Operand>(allOperands.size());
  auto buffer = F.getModule().allocateInst(size, alignof(AllocPackInst));
  return ::new (buffer) AllocPackInst(loc, resultType, allOperands);
}

AllocRefInstBase::AllocRefInstBase(SILInstructionKind Kind,
                                   SILDebugLocation Loc,
                                   SILType ObjectType,
                                   bool objc, bool canBeOnStack,
                                   ArrayRef<SILType> ElementTypes)
    : AllocationInst(Kind, Loc, ObjectType) {
  sharedUInt8().AllocRefInstBase.objC = objc;
  sharedUInt8().AllocRefInstBase.onStack = canBeOnStack;
  sharedUInt8().AllocRefInstBase.numTailTypes = ElementTypes.size();
  assert(sharedUInt8().AllocRefInstBase.numTailTypes ==
         ElementTypes.size() && "Truncation");
  assert(!objc || ElementTypes.empty());
}

AllocRefInst *AllocRefInst::create(SILDebugLocation Loc, SILFunction &F,
                                   SILType ObjectType,
                                   bool objc, bool canBeOnStack,
                                   ArrayRef<SILType> ElementTypes,
                                   ArrayRef<SILValue> ElementCountOperands) {
  assert(ElementTypes.size() == ElementCountOperands.size());
  assert(!objc || ElementTypes.empty());
  SmallVector<SILValue, 8> AllOperands(ElementCountOperands.begin(),
                                       ElementCountOperands.end());
  collectTypeDependentOperands(AllOperands, F, ElementTypes, ObjectType);
  auto Size = totalSizeToAlloc<swift::Operand, SILType>(AllOperands.size(),
                                                        ElementTypes.size());
  auto Buffer = F.getModule().allocateInst(Size, alignof(AllocRefInst));
  return ::new (Buffer) AllocRefInst(Loc, F, ObjectType, objc, canBeOnStack,
                                     ElementTypes, AllOperands);
}

AllocRefDynamicInst *
AllocRefDynamicInst::create(SILDebugLocation DebugLoc, SILFunction &F,
                            SILValue metatypeOperand, SILType ty, bool objc,
                            bool canBeOnStack,
                            ArrayRef<SILType> ElementTypes,
                            ArrayRef<SILValue> ElementCountOperands) {
  SmallVector<SILValue, 8> AllOperands(ElementCountOperands.begin(),
                                       ElementCountOperands.end());
  AllOperands.push_back(metatypeOperand);
  collectTypeDependentOperands(AllOperands, F, ty, ElementTypes);
  auto Size = totalSizeToAlloc<swift::Operand, SILType>(AllOperands.size(),
                                                        ElementTypes.size());
  auto Buffer = F.getModule().allocateInst(Size, alignof(AllocRefDynamicInst));
  return ::new (Buffer)
      AllocRefDynamicInst(DebugLoc, ty, objc, canBeOnStack, ElementTypes,
                          AllOperands);
}

bool AllocRefDynamicInst::isDynamicTypeDeinitAndSizeKnownEquivalentToBaseType() const {
  auto baseType = this->getType();
  auto classType = baseType.getASTType();
  // We know that the dynamic type for _ContiguousArrayStorage is compatible
  // with the base type in size and deinit behavior.
  if (classType->is_ContiguousArrayStorage())
    return true;
  return false;
}

AllocBoxInst::AllocBoxInst(SILDebugLocation Loc, CanSILBoxType BoxType,
                           ArrayRef<SILValue> TypeDependentOperands,
                           SILFunction &F, Optional<SILDebugVariable> Var,
                           bool hasDynamicLifetime, bool reflection,
                           bool usesMoveableValueDebugInfo,
                           bool hasPointerEscape)
    : NullaryInstructionWithTypeDependentOperandsBase(
          Loc, TypeDependentOperands, SILType::getPrimitiveObjectType(BoxType)),
      VarInfo(Var, getTrailingObjects<char>()) {
  sharedUInt8().AllocBoxInst.dynamicLifetime = hasDynamicLifetime;
  sharedUInt8().AllocBoxInst.reflection = reflection;

  // If we have a noncopyable type, always set uses mvoeable value debug info.
  usesMoveableValueDebugInfo |=
      BoxType->getLayout()->getFields()[0].getObjectType().isMoveOnly();

  sharedUInt8().AllocBoxInst.usesMoveableValueDebugInfo =
      usesMoveableValueDebugInfo;

  sharedUInt8().AllocBoxInst.pointerEscape = hasPointerEscape;
}

AllocBoxInst *AllocBoxInst::create(SILDebugLocation Loc, CanSILBoxType BoxType,
                                   SILFunction &F,
                                   Optional<SILDebugVariable> Var,
                                   bool hasDynamicLifetime, bool reflection,
                                   bool usesMoveableValueDebugInfo,
                                   bool hasPointerEscape) {
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, BoxType);
  auto Sz = totalSizeToAlloc<swift::Operand, char>(TypeDependentOperands.size(),
                                                   Var ? Var->Name.size() : 0);
  auto Buf = F.getModule().allocateInst(Sz, alignof(AllocBoxInst));
  return ::new (Buf) AllocBoxInst(Loc, BoxType, TypeDependentOperands, F, Var,
                                  hasDynamicLifetime, reflection,
                                  usesMoveableValueDebugInfo, hasPointerEscape);
}

SILType AllocBoxInst::getAddressType() const {
  return getSILBoxFieldType(TypeExpansionContext(*this->getFunction()),
                            getBoxType(), getModule().Types, 0)
      .getAddressType();
}

DebugValueInst::DebugValueInst(SILDebugLocation DebugLoc, SILValue Operand,
                               SILDebugVariable Var, bool poisonRefs,
                               bool usesMoveableValueDebugInfo, bool trace)
    : UnaryInstructionBase(DebugLoc, Operand),
      SILDebugVariableSupplement(Var.DIExpr.getNumElements(),
                                 Var.Type.has_value(), Var.Loc.has_value(),
                                 Var.Scope),
      VarInfo(Var, getTrailingObjects<char>(), getTrailingObjects<SILType>(),
              getTrailingObjects<SILLocation>(),
              getTrailingObjects<const SILDebugScope *>(),
              getTrailingObjects<SILDIExprElement>()) {
  if (auto *VD = DebugLoc.getLocation().getAsASTNode<VarDecl>())
    VarInfo.setImplicit(VD->isImplicit() || VarInfo.isImplicit());
  setPoisonRefs(poisonRefs);
  if (usesMoveableValueDebugInfo || Operand->getType().isMoveOnly())
    setUsesMoveableValueDebugInfo();
  setTrace(trace);
}

DebugValueInst *DebugValueInst::create(SILDebugLocation DebugLoc,
                                       SILValue Operand, SILModule &M,
                                       SILDebugVariable Var, bool poisonRefs,
                                       bool wasMoved, bool trace) {
  void *buf = allocateDebugVarCarryingInst<DebugValueInst>(M, Var);
  return ::new (buf)
    DebugValueInst(DebugLoc, Operand, Var, poisonRefs, wasMoved, trace);
}

DebugValueInst *DebugValueInst::createAddr(SILDebugLocation DebugLoc,
                                           SILValue Operand, SILModule &M,
                                           SILDebugVariable Var,
                                           bool wasMoved, bool trace) {
  // For alloc_stack, debug_value is used to annotate the associated
  // memory location, so we shouldn't attach op_deref.
  if (!isa<AllocStackInst>(Operand))
    Var.DIExpr.prependElements(
      {SILDIExprElement::createOperator(SILDIExprOperator::Dereference)});
  void *buf = allocateDebugVarCarryingInst<DebugValueInst>(M, Var);
  return ::new (buf) DebugValueInst(DebugLoc, Operand, Var,
                                    /*poisonRefs=*/false, wasMoved, trace);
}

bool DebugValueInst::exprStartsWithDeref() const {
  if (!NumDIExprOperands)
    return false;

  llvm::ArrayRef<SILDIExprElement> DIExprElements(
      getTrailingObjects<SILDIExprElement>(), NumDIExprOperands);
  return DIExprElements.front().getAsOperator()
          == SILDIExprOperator::Dereference;
}

VarDecl *DebugValueInst::getDecl() const {
  return getLoc().getAsASTNode<VarDecl>();
}

AllocExistentialBoxInst *AllocExistentialBoxInst::create(
    SILDebugLocation Loc, SILType ExistentialType, CanType ConcreteType,
    ArrayRef<ProtocolConformanceRef> Conformances,
    SILFunction *F) {
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, ConcreteType);
  SILModule &Mod = F->getModule();
  auto Size = totalSizeToAlloc<swift::Operand>(TypeDependentOperands.size());
  auto Buffer = Mod.allocateInst(Size, alignof(AllocExistentialBoxInst));
  return ::new (Buffer) AllocExistentialBoxInst(Loc,
                                                ExistentialType,
                                                ConcreteType,
                                                Conformances,
                                                TypeDependentOperands,
                                                F);
}

BuiltinInst *BuiltinInst::create(SILDebugLocation Loc, Identifier Name,
                                 SILType ReturnType,
                                 SubstitutionMap Substitutions,
                                 ArrayRef<SILValue> Args,
                                 SILModule &M) {
  auto Size = totalSizeToAlloc<swift::Operand>(Args.size());
  auto Buffer = M.allocateInst(Size, alignof(BuiltinInst));
  return ::new (Buffer) BuiltinInst(Loc, Name, ReturnType, Substitutions,
                                    Args);
}

BuiltinInst::BuiltinInst(SILDebugLocation Loc, Identifier Name,
                         SILType ReturnType, SubstitutionMap Subs,
                         ArrayRef<SILValue> Args)
    : InstructionBaseWithTrailingOperands(Args, Loc, ReturnType), Name(Name),
      Substitutions(Subs) {
}

IncrementProfilerCounterInst *IncrementProfilerCounterInst::create(
    SILDebugLocation Loc, unsigned CounterIdx, StringRef PGOFuncName,
    unsigned NumCounters, uint64_t PGOFuncHash, SILModule &M) {

  auto PGOFuncNameLength = PGOFuncName.size();
  auto Size = totalSizeToAlloc<char>(PGOFuncNameLength);
  auto Buffer = M.allocateInst(Size, alignof(IncrementProfilerCounterInst));

  auto *Inst = ::new (Buffer) IncrementProfilerCounterInst(
      Loc, CounterIdx, PGOFuncNameLength, NumCounters, PGOFuncHash);

  std::uninitialized_copy(PGOFuncName.begin(), PGOFuncName.end(),
                          Inst->getTrailingObjects<char>());
  return Inst;
}

TestSpecificationInst *
TestSpecificationInst::create(SILDebugLocation Loc,
                              StringRef ArgumentsSpecification, SILModule &M) {
  auto ArgumentsSpecificationLength = ArgumentsSpecification.size();
  auto Size = totalSizeToAlloc<char>(ArgumentsSpecificationLength);
  auto Buffer = M.allocateInst(Size, alignof(TestSpecificationInst));

  auto *Inst =
      ::new (Buffer) TestSpecificationInst(Loc, ArgumentsSpecificationLength);
  std::uninitialized_copy(ArgumentsSpecification.begin(),
                          ArgumentsSpecification.end(),
                          Inst->getTrailingObjects<char>());
  return Inst;
}

InitBlockStorageHeaderInst *
InitBlockStorageHeaderInst::create(SILFunction &F,
                               SILDebugLocation DebugLoc, SILValue BlockStorage,
                               SILValue InvokeFunction, SILType BlockType,
                               SubstitutionMap Subs) {
  void *Buffer = F.getModule().allocateInst(
    sizeof(InitBlockStorageHeaderInst),
    alignof(InitBlockStorageHeaderInst));
  
  return ::new (Buffer) InitBlockStorageHeaderInst(DebugLoc, BlockStorage,
                                                   InvokeFunction, BlockType,
                                                   Subs);
}

ApplyInst::ApplyInst(SILDebugLocation Loc, SILValue Callee,
                     SILType SubstCalleeTy, SILType Result,
                     SubstitutionMap Subs,
                     ArrayRef<SILValue> Args,
                     ArrayRef<SILValue> TypeDependentOperands,
                     ApplyOptions options,
                     const GenericSpecializationInformation *SpecializationInfo)
    : InstructionBase(Loc, Callee, SubstCalleeTy, Subs, Args,
                      TypeDependentOperands, SpecializationInfo, Result) {
  setApplyOptions(options);
  assert(!SubstCalleeTy.castTo<SILFunctionType>()->isCoroutine());
}

ApplyInst *
ApplyInst::create(SILDebugLocation Loc, SILValue Callee, SubstitutionMap Subs,
                  ArrayRef<SILValue> Args, ApplyOptions Options,
                  Optional<SILModuleConventions> ModuleConventions,
                  SILFunction &F,
                  const GenericSpecializationInformation *SpecializationInfo) {
  SILType SubstCalleeSILTy = Callee->getType().substGenericArgs(
      F.getModule(), Subs, F.getTypeExpansionContext());
  auto SubstCalleeTy = SubstCalleeSILTy.getAs<SILFunctionType>();
  
  SILFunctionConventions Conv(SubstCalleeTy,
                              ModuleConventions.has_value()
                                  ? ModuleConventions.value()
                                  : SILModuleConventions(F.getModule()));
  SILType Result = Conv.getSILResultType(F.getTypeExpansionContext());

  SmallVector<SILValue, 32> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F,
                               SubstCalleeSILTy.getASTType(), Subs);
  void *Buffer =
    allocateTrailingInst<ApplyInst, Operand>(
      F, getNumAllOperands(Args, TypeDependentOperands));
  return ::new(Buffer) ApplyInst(Loc, Callee, SubstCalleeSILTy,
                                 Result, Subs, Args,
                                 TypeDependentOperands,
                                 Options,
                                 SpecializationInfo);
}

BeginApplyInst::BeginApplyInst(SILDebugLocation loc, SILValue callee,
                               SILType substCalleeTy,
                               ArrayRef<SILType> allResultTypes,
                               ArrayRef<ValueOwnershipKind> allResultOwnerships,
                               SubstitutionMap subs,
                               ArrayRef<SILValue> args,
                               ArrayRef<SILValue> typeDependentOperands,
                               ApplyOptions options,
                     const GenericSpecializationInformation *specializationInfo)
    : InstructionBase(loc, callee, substCalleeTy, subs, args,
                      typeDependentOperands, specializationInfo),
      MultipleValueInstructionTrailingObjects(this, allResultTypes,
                                              allResultOwnerships) {
  setApplyOptions(options);
  assert(substCalleeTy.castTo<SILFunctionType>()->isCoroutine());
}

BeginApplyInst *
BeginApplyInst::create(SILDebugLocation loc, SILValue callee,
                       SubstitutionMap subs, ArrayRef<SILValue> args,
                       ApplyOptions options,
                       Optional<SILModuleConventions> moduleConventions,
                       SILFunction &F,
                  const GenericSpecializationInformation *specializationInfo) {
  SILType substCalleeSILType = callee->getType().substGenericArgs(
      F.getModule(), subs, F.getTypeExpansionContext());
  auto substCalleeType = substCalleeSILType.castTo<SILFunctionType>();

  SILFunctionConventions conv(substCalleeType,
                              moduleConventions.has_value()
                                  ? moduleConventions.value()
                                  : SILModuleConventions(F.getModule()));

  SmallVector<SILType, 8> resultTypes;
  SmallVector<ValueOwnershipKind, 8> resultOwnerships;

  for (auto &yield : substCalleeType->getYields()) {
    auto yieldType = conv.getSILType(yield, F.getTypeExpansionContext());
    auto argConvention = SILArgumentConvention(yield.getConvention());
    resultTypes.push_back(yieldType);
    resultOwnerships.push_back(ValueOwnershipKind(
        F, yieldType, argConvention,
        moduleConventions.has_value() ? moduleConventions.value()
                                     : SILModuleConventions(F.getModule())));
  }

  resultTypes.push_back(SILType::getSILTokenType(F.getASTContext()));
  // The begin_apply token represents the borrow scope of all owned and
  // guaranteed call arguments. Although SILToken is (currently) trivially
  // typed, it must have guaranteed ownership so end_apply and abort_apply will
  // be recognized as lifetime-ending uses.
  resultOwnerships.push_back(OwnershipKind::Guaranteed);

  SmallVector<SILValue, 32> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F, substCalleeType, subs);
  void *buffer =
    allocateTrailingInst<BeginApplyInst, Operand, MultipleValueInstruction*,
                         MultipleValueInstructionResult>(
      F, getNumAllOperands(args, typeDependentOperands),
      1, resultTypes.size());
  return ::new(buffer) BeginApplyInst(loc, callee, substCalleeSILType,
                                      resultTypes, resultOwnerships, subs,
                                      args, typeDependentOperands,
                                      options, specializationInfo);
}

void BeginApplyInst::getCoroutineEndPoints(
    SmallVectorImpl<EndApplyInst *> &endApplyInsts,
    SmallVectorImpl<AbortApplyInst *> &abortApplyInsts) const {
  for (auto *tokenUse : getTokenResult()->getUses()) {
    auto *user = tokenUse->getUser();
    if (auto *end = dyn_cast<EndApplyInst>(user)) {
      endApplyInsts.push_back(end);
      continue;
    }

    abortApplyInsts.push_back(cast<AbortApplyInst>(user));
  }
}

void BeginApplyInst::getCoroutineEndPoints(
    SmallVectorImpl<Operand *> &endApplyInsts,
    SmallVectorImpl<Operand *> &abortApplyInsts) const {
  for (auto *tokenUse : getTokenResult()->getUses()) {
    auto *user = tokenUse->getUser();
    if (isa<EndApplyInst>(user)) {
      endApplyInsts.push_back(tokenUse);
      continue;
    }

    assert(isa<AbortApplyInst>(user));
    abortApplyInsts.push_back(tokenUse);
  }
}

bool swift::doesApplyCalleeHaveSemantics(SILValue callee, StringRef semantics) {
  if (auto *FRI = dyn_cast<FunctionRefBaseInst>(callee))
    if (auto *F = FRI->getReferencedFunctionOrNull())
      return F->hasSemanticsAttr(semantics);
  return false;
}

PartialApplyInst::PartialApplyInst(
    SILDebugLocation Loc, SILValue Callee, SILType SubstCalleeTy,
    SubstitutionMap Subs, ArrayRef<SILValue> Args,
    ArrayRef<SILValue> TypeDependentOperands, SILType ClosureType,
    const GenericSpecializationInformation *SpecializationInfo)
    // FIXME: the callee should have a lowered SIL function type, and
    // PartialApplyInst
    // should derive the type of its result by partially applying the callee's
    // type.
    : InstructionBase(Loc, Callee, SubstCalleeTy, Subs, Args,
                      TypeDependentOperands, SpecializationInfo, ClosureType) {}

PartialApplyInst *PartialApplyInst::create(
    SILDebugLocation Loc, SILValue Callee, ArrayRef<SILValue> Args,
    SubstitutionMap Subs, ParameterConvention CalleeConvention, SILFunction &F,
    const GenericSpecializationInformation *SpecializationInfo,
    OnStackKind onStack) {
  SILType SubstCalleeTy = Callee->getType().substGenericArgs(
      F.getModule(), Subs, F.getTypeExpansionContext());

  SILType ClosureType = SILBuilder::getPartialApplyResultType(
      F.getTypeExpansionContext(), SubstCalleeTy, Args.size(), F.getModule(), {},
      CalleeConvention, onStack);

  SmallVector<SILValue, 32> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F,
                               SubstCalleeTy.getASTType(), Subs);
  void *Buffer =
    allocateTrailingInst<PartialApplyInst, Operand>(
      F, getNumAllOperands(Args, TypeDependentOperands));
  return ::new(Buffer) PartialApplyInst(Loc, Callee, SubstCalleeTy,
                                        Subs, Args,
                                        TypeDependentOperands, ClosureType,
                                        SpecializationInfo);
}

TryApplyInstBase::TryApplyInstBase(SILInstructionKind kind,
                                   SILDebugLocation loc,
                                   SILBasicBlock *normalBB,
                                   SILBasicBlock *errorBB)
    : TermInst(kind, loc), DestBBs{{{this, normalBB}, {this, errorBB}}} {}

TryApplyInst::TryApplyInst(
    SILDebugLocation Loc, SILValue callee, SILType substCalleeTy,
    SubstitutionMap subs, ArrayRef<SILValue> args,
    ArrayRef<SILValue> TypeDependentOperands, SILBasicBlock *normalBB,
    SILBasicBlock *errorBB,
    ApplyOptions options,
    const GenericSpecializationInformation *SpecializationInfo)
    : InstructionBase(Loc, callee, substCalleeTy, subs, args,
                      TypeDependentOperands, SpecializationInfo, normalBB,
                      errorBB) {
  setApplyOptions(options);
}

TryApplyInst *TryApplyInst::create(
    SILDebugLocation loc, SILValue callee, SubstitutionMap subs,
    ArrayRef<SILValue> args, SILBasicBlock *normalBB, SILBasicBlock *errorBB,
    ApplyOptions options,
    SILFunction &F,
    const GenericSpecializationInformation *specializationInfo) {
  SILType substCalleeTy = callee->getType().substGenericArgs(
      F.getModule(), subs, F.getTypeExpansionContext());

  SmallVector<SILValue, 32> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F,
                               substCalleeTy.getASTType(),
                               subs);
  void *buffer =
    allocateTrailingInst<TryApplyInst, Operand>(
      F, getNumAllOperands(args, typeDependentOperands));
  return ::new (buffer) TryApplyInst(loc, callee, substCalleeTy, subs, args,
                                     typeDependentOperands,
                                     normalBB, errorBB, options,
                                     specializationInfo);
}

SILType DifferentiableFunctionInst::getDifferentiableFunctionType(
    SILValue OriginalFunction, IndexSubset *ParameterIndices,
    IndexSubset *ResultIndices) {
  assert(!ResultIndices->isEmpty());
  auto fnTy = OriginalFunction->getType().castTo<SILFunctionType>();
  auto diffTy = fnTy->getWithDifferentiability(DifferentiabilityKind::Reverse,
                                               ParameterIndices, ResultIndices);
  return SILType::getPrimitiveObjectType(diffTy);
}

ValueOwnershipKind DifferentiableFunctionInst::getMergedOwnershipKind(
    SILValue OriginalFunction, ArrayRef<SILValue> DerivativeFunctions) {
  if (DerivativeFunctions.empty())
    return OriginalFunction->getOwnershipKind();
  return mergeSILValueOwnership(
      {OriginalFunction, DerivativeFunctions[0], DerivativeFunctions[1]});
}

DifferentiableFunctionInst::DifferentiableFunctionInst(
    SILDebugLocation Loc, IndexSubset *ParameterIndices,
    IndexSubset *ResultIndices, SILValue OriginalFunction,
    ArrayRef<SILValue> DerivativeFunctions,
    ValueOwnershipKind forwardingOwnershipKind)
    : InstructionBaseWithTrailingOperands(
          OriginalFunction, DerivativeFunctions, Loc,
          getDifferentiableFunctionType(OriginalFunction, ParameterIndices,
                                        ResultIndices),
          forwardingOwnershipKind),
      ParameterIndices(ParameterIndices), ResultIndices(ResultIndices),
      HasDerivativeFunctions(!DerivativeFunctions.empty()) {
  assert(DerivativeFunctions.empty() || DerivativeFunctions.size() == 2);
}

DifferentiableFunctionInst *DifferentiableFunctionInst::create(
    SILModule &Module, SILDebugLocation Loc, IndexSubset *ParameterIndices,
    IndexSubset *ResultIndices, SILValue OriginalFunction,
    Optional<std::pair<SILValue, SILValue>> VJPAndJVPFunctions,
    ValueOwnershipKind forwardingOwnershipKind) {
  auto derivativeFunctions =
      VJPAndJVPFunctions.has_value()
          ? ArrayRef<SILValue>(
                reinterpret_cast<SILValue *>(VJPAndJVPFunctions.getPointer()),
                2)
          : ArrayRef<SILValue>();
  size_t size = totalSizeToAlloc<Operand>(1 + derivativeFunctions.size());
  void *buffer = Module.allocateInst(size, alignof(DifferentiableFunctionInst));
  return ::new (buffer) DifferentiableFunctionInst(
      Loc, ParameterIndices, ResultIndices, OriginalFunction,
      derivativeFunctions, forwardingOwnershipKind);
}

SILType LinearFunctionInst::getLinearFunctionType(
    SILValue OriginalFunction, IndexSubset *ParameterIndices) {
  auto fnTy = OriginalFunction->getType().castTo<SILFunctionType>();
  auto *resultIndices =
      IndexSubset::get(fnTy->getASTContext(), /*capacity*/ 1, /*indices*/ {0});
  auto diffTy = fnTy->getWithDifferentiability(DifferentiabilityKind::Linear,
                                               ParameterIndices, resultIndices);
  return SILType::getPrimitiveObjectType(diffTy);
}

LinearFunctionInst::LinearFunctionInst(
    SILDebugLocation Loc, IndexSubset *ParameterIndices,
    SILValue OriginalFunction, Optional<SILValue> TransposeFunction,
    ValueOwnershipKind forwardingOwnershipKind)
    : InstructionBaseWithTrailingOperands(
          OriginalFunction,
          TransposeFunction.has_value()
              ? ArrayRef<SILValue>(TransposeFunction.getPointer(), 1)
              : ArrayRef<SILValue>(),
          Loc, getLinearFunctionType(OriginalFunction, ParameterIndices),
          forwardingOwnershipKind),
      ParameterIndices(ParameterIndices),
      HasTransposeFunction(TransposeFunction.has_value()) {}

LinearFunctionInst *LinearFunctionInst::create(
    SILModule &Module, SILDebugLocation Loc, IndexSubset *ParameterIndices,
    SILValue OriginalFunction, Optional<SILValue> TransposeFunction,
    ValueOwnershipKind forwardingOwnershipKind) {
  size_t size = totalSizeToAlloc<Operand>(TransposeFunction.has_value() ? 2 : 1);
  void *buffer = Module.allocateInst(size, alignof(DifferentiableFunctionInst));
  return ::new (buffer)
      LinearFunctionInst(Loc, ParameterIndices, OriginalFunction,
                         TransposeFunction, forwardingOwnershipKind);
}

SILType DifferentiableFunctionExtractInst::getExtracteeType(
    SILValue function, NormalDifferentiableFunctionTypeComponent extractee,
    SILModule &module) {
  auto fnTy = function->getType().castTo<SILFunctionType>();
  // TODO: Ban 'Normal' and 'Forward'.
  assert(
      fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Reverse ||
      fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Normal ||
      fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Forward);
  auto originalFnTy = fnTy->getWithoutDifferentiability();
  auto kindOpt = extractee.getAsDerivativeFunctionKind();
  if (!kindOpt) {
    assert(extractee == NormalDifferentiableFunctionTypeComponent::Original);
    return SILType::getPrimitiveObjectType(originalFnTy);
  }
  auto resultFnTy = originalFnTy->getAutoDiffDerivativeFunctionType(
      fnTy->getDifferentiabilityParameterIndices(),
      fnTy->getDifferentiabilityResultIndices(), *kindOpt, module.Types,
      LookUpConformanceInModule(module.getSwiftModule()));
  return SILType::getPrimitiveObjectType(resultFnTy);
}

DifferentiableFunctionExtractInst::DifferentiableFunctionExtractInst(
    SILModule &module, SILDebugLocation debugLoc,
    NormalDifferentiableFunctionTypeComponent extractee, SILValue function,
    ValueOwnershipKind forwardingOwnershipKind, Optional<SILType> extracteeType)
    : UnaryInstructionBase(debugLoc, function,
                           extracteeType
                               ? *extracteeType
                               : getExtracteeType(function, extractee, module),
                           forwardingOwnershipKind),
      Extractee(extractee), HasExplicitExtracteeType(extracteeType.has_value()) {
}

SILType LinearFunctionExtractInst::
getExtracteeType(
    SILValue function, LinearDifferentiableFunctionTypeComponent extractee,
    SILModule &module) {
  auto fnTy = function->getType().castTo<SILFunctionType>();
  assert(fnTy->getDifferentiabilityKind() == DifferentiabilityKind::Linear);
  auto originalFnTy = fnTy->getWithoutDifferentiability();
  switch (extractee) {
  case LinearDifferentiableFunctionTypeComponent::Original:
    return SILType::getPrimitiveObjectType(originalFnTy);
  case LinearDifferentiableFunctionTypeComponent::Transpose:
    auto transposeFnTy = originalFnTy->getAutoDiffTransposeFunctionType(
        fnTy->getDifferentiabilityParameterIndices(), module.Types,
        LookUpConformanceInModule(module.getSwiftModule()));
    return SILType::getPrimitiveObjectType(transposeFnTy);
  }
  llvm_unreachable("invalid extractee");
}

LinearFunctionExtractInst::LinearFunctionExtractInst(
    SILModule &module, SILDebugLocation debugLoc,
    LinearDifferentiableFunctionTypeComponent extractee, SILValue function,
    ValueOwnershipKind forwardingOwnershipKind)
    : UnaryInstructionBase(debugLoc, function,
                           getExtracteeType(function, extractee, module),
                           forwardingOwnershipKind),
      extractee(extractee) {}

SILType DifferentiabilityWitnessFunctionInst::getDifferentiabilityWitnessType(
    SILModule &module, DifferentiabilityWitnessFunctionKind witnessKind,
    SILDifferentiabilityWitness *witness) {
  auto fnTy = witness->getOriginalFunction()->getLoweredFunctionType();
  auto witnessCanGenSig = witness->getDerivativeGenericSignature().getCanonicalSignature();
  auto *parameterIndices = witness->getParameterIndices();
  auto *resultIndices = witness->getResultIndices();
  if (auto derivativeKind = witnessKind.getAsDerivativeFunctionKind()) {
    bool isReabstractionThunk =
        witness->getOriginalFunction()->isThunk() == IsReabstractionThunk;
    auto diffFnTy = fnTy->getAutoDiffDerivativeFunctionType(
        parameterIndices, resultIndices, *derivativeKind, module.Types,
        LookUpConformanceInModule(module.getSwiftModule()), witnessCanGenSig,
        isReabstractionThunk);
    return SILType::getPrimitiveObjectType(diffFnTy);
  }
  assert(witnessKind == DifferentiabilityWitnessFunctionKind::Transpose);
  auto transposeFnTy = fnTy->getAutoDiffTransposeFunctionType(
      parameterIndices, module.Types,
      LookUpConformanceInModule(module.getSwiftModule()), witnessCanGenSig);
  return SILType::getPrimitiveObjectType(transposeFnTy);
}

DifferentiabilityWitnessFunctionInst::DifferentiabilityWitnessFunctionInst(
    SILModule &module, SILDebugLocation debugLoc,
    DifferentiabilityWitnessFunctionKind witnessKind,
    SILDifferentiabilityWitness *witness, Optional<SILType> functionType)
    : InstructionBase(debugLoc, functionType
                                    ? *functionType
                                    : getDifferentiabilityWitnessType(
                                          module, witnessKind, witness)),
      witnessKind(witnessKind), witness(witness),
      hasExplicitFunctionType(functionType) {
  assert(witness && "Differentiability witness must not be null");
#ifndef NDEBUG
  if (functionType.has_value()) {
    assert(module.getStage() == SILStage::Lowered &&
           "Explicit type is valid only in lowered SIL");
  }
#endif
}

FunctionRefBaseInst::FunctionRefBaseInst(SILInstructionKind Kind,
                                         SILDebugLocation DebugLoc,
                                         SILFunction *F,
                                         TypeExpansionContext context)
    : LiteralInst(Kind, DebugLoc, F->getLoweredTypeInContext(context)), f(F) {
  F->incrementRefCount();
}

void FunctionRefBaseInst::dropReferencedFunction() {
  if (auto *Function = getInitiallyReferencedFunction())
    Function->decrementRefCount();
  f = nullptr;
}

FunctionRefBaseInst::~FunctionRefBaseInst() {
  if (getInitiallyReferencedFunction())
    getInitiallyReferencedFunction()->decrementRefCount();
}

FunctionRefInst::FunctionRefInst(SILDebugLocation Loc, SILFunction *F,
                                 TypeExpansionContext context)
    : FunctionRefBaseInst(SILInstructionKind::FunctionRefInst, Loc, F,
                          context) {
  assert(!F->isDynamicallyReplaceable());
}

DynamicFunctionRefInst::DynamicFunctionRefInst(SILDebugLocation Loc,
                                               SILFunction *F,
                                               TypeExpansionContext context)
    : FunctionRefBaseInst(SILInstructionKind::DynamicFunctionRefInst, Loc, F,
                          context) {
  assert(F->isDynamicallyReplaceable());
}

PreviousDynamicFunctionRefInst::PreviousDynamicFunctionRefInst(
    SILDebugLocation Loc, SILFunction *F, TypeExpansionContext context)
    : FunctionRefBaseInst(SILInstructionKind::PreviousDynamicFunctionRefInst,
                          Loc, F, context) {
  assert(!F->isDynamicallyReplaceable());
}

AllocGlobalInst::AllocGlobalInst(SILDebugLocation Loc,
                                 SILGlobalVariable *Global)
    : InstructionBase(Loc),
      Global(Global) {}

GlobalAddrInst::GlobalAddrInst(SILDebugLocation DebugLoc,
                               SILGlobalVariable *Global,
                               TypeExpansionContext context)
    : InstructionBase(DebugLoc,
                      Global->getLoweredTypeInContext(context).getAddressType(),
                      Global) {}

GlobalValueInst::GlobalValueInst(SILDebugLocation DebugLoc,
                                 SILGlobalVariable *Global,
                                 TypeExpansionContext context)
    : InstructionBase(DebugLoc,
                      Global->getLoweredTypeInContext(context).getObjectType(),
                      Global) {}

const IntrinsicInfo &BuiltinInst::getIntrinsicInfo() const {
  return getModule().getIntrinsicInfo(getName());
}

const BuiltinInfo &BuiltinInst::getBuiltinInfo() const {
  return getModule().getBuiltinInfo(getName());
}

static unsigned getWordsForBitWidth(unsigned bits) {
  return ((bits + llvm::APInt::APINT_BITS_PER_WORD - 1)
          / llvm::APInt::APINT_BITS_PER_WORD);
}

template<typename INST>
static void *allocateLiteralInstWithTextSize(SILModule &M, unsigned length) {
  return M.allocateInst(sizeof(INST) + length, alignof(INST));
}

template<typename INST>
static void *allocateLiteralInstWithBitSize(SILModule &M, unsigned bits) {
  unsigned words = getWordsForBitWidth(bits);
  return M.allocateInst(
      sizeof(INST) + sizeof(llvm::APInt::WordType)*words, alignof(INST));
}

IntegerLiteralInst::IntegerLiteralInst(SILDebugLocation Loc, SILType Ty,
                                       const llvm::APInt &Value)
    : InstructionBase(Loc, Ty) {
  sharedUInt32().IntegerLiteralInst.numBits = Value.getBitWidth();
  std::uninitialized_copy_n(Value.getRawData(), Value.getNumWords(),
                            getTrailingObjects<llvm::APInt::WordType>());
}

IntegerLiteralInst *IntegerLiteralInst::create(SILDebugLocation Loc,
                                               SILType Ty, const APInt &Value,
                                               SILModule &M) {
#ifndef NDEBUG
  if (auto intTy = Ty.getAs<BuiltinIntegerType>()) {
    assert(intTy->getGreatestWidth() == Value.getBitWidth() &&
           "IntegerLiteralInst APInt value's bit width doesn't match type");
  } else {
    assert(Ty.is<BuiltinIntegerLiteralType>());
    assert(Value.getBitWidth() == Value.getMinSignedBits());
  }
#endif

  void *buf = allocateLiteralInstWithBitSize<IntegerLiteralInst>(M,
                                                          Value.getBitWidth());
  return ::new (buf) IntegerLiteralInst(Loc, Ty, Value);
}

static APInt getAPInt(AnyBuiltinIntegerType *anyIntTy, intmax_t value) {
  // If we're forming a fixed-width type, build using the greatest width.
  if (auto intTy = dyn_cast<BuiltinIntegerType>(anyIntTy))
    return APInt(intTy->getGreatestWidth(), value);

  // Otherwise, build using the size of the type and then truncate to the
  // minimum width necessary.
  APInt result(8 * sizeof(value), value, /*signed*/ true);
  result = result.trunc(result.getMinSignedBits());
  return result;
}

IntegerLiteralInst *IntegerLiteralInst::create(SILDebugLocation Loc,
                                               SILType Ty, intmax_t Value,
                                               SILModule &M) {
  auto intTy = Ty.castTo<AnyBuiltinIntegerType>();
  return create(Loc, Ty, getAPInt(intTy, Value), M);
}

static SILType getGreatestIntegerType(Type type, SILModule &M) {
  if (auto intTy = type->getAs<BuiltinIntegerType>()) {
    return SILType::getBuiltinIntegerType(intTy->getGreatestWidth(),
                                          M.getASTContext());
  } else {
    assert(type->is<BuiltinIntegerLiteralType>());
    return SILType::getBuiltinIntegerLiteralType(M.getASTContext());
  }
}

IntegerLiteralInst *IntegerLiteralInst::create(IntegerLiteralExpr *E,
                                               SILDebugLocation Loc,
                                               SILModule &M) {
  return create(Loc, getGreatestIntegerType(E->getType(), M), E->getValue(), M);
}

/// getValue - Return the APInt for the underlying integer literal.
APInt IntegerLiteralInst::getValue() const {
  auto numBits = sharedUInt32().IntegerLiteralInst.numBits;
  return APInt(numBits, {getTrailingObjects<llvm::APInt::WordType>(),
                         getWordsForBitWidth(numBits)});
}

FloatLiteralInst::FloatLiteralInst(SILDebugLocation Loc, SILType Ty,
                                   const APInt &Bits)
    : InstructionBase(Loc, Ty) {
  sharedUInt32().FloatLiteralInst.numBits = Bits.getBitWidth();
  std::uninitialized_copy_n(Bits.getRawData(), Bits.getNumWords(),
                            getTrailingObjects<llvm::APInt::WordType>());
}

FloatLiteralInst *FloatLiteralInst::create(SILDebugLocation Loc, SILType Ty,
                                           const APFloat &Value,
                                           SILModule &M) {
  auto floatTy = Ty.castTo<BuiltinFloatType>();
  assert(&floatTy->getAPFloatSemantics() == &Value.getSemantics() &&
         "FloatLiteralInst value's APFloat semantics do not match type");
  (void)floatTy;

  APInt Bits = Value.bitcastToAPInt();

  void *buf = allocateLiteralInstWithBitSize<FloatLiteralInst>(M,
                                                            Bits.getBitWidth());
  return ::new (buf) FloatLiteralInst(Loc, Ty, Bits);
}

FloatLiteralInst *FloatLiteralInst::create(FloatLiteralExpr *E,
                                           SILDebugLocation Loc,
                                           SILModule &M) {
  return create(Loc,
                // Builtin floating-point types are always valid SIL types.
                SILType::getBuiltinFloatType(
                    E->getType()->castTo<BuiltinFloatType>()->getFPKind(),
                    M.getASTContext()),
                E->getValue(), M);
}

APInt FloatLiteralInst::getBits() const {
  auto numBits = sharedUInt32().FloatLiteralInst.numBits;
  return APInt(numBits, {getTrailingObjects<llvm::APInt::WordType>(),
                         getWordsForBitWidth(numBits)});
}

APFloat FloatLiteralInst::getValue() const {
  return APFloat(getType().castTo<BuiltinFloatType>()->getAPFloatSemantics(),
                 getBits());
}

StringLiteralInst::StringLiteralInst(SILDebugLocation Loc, StringRef Text,
                                     Encoding encoding, SILType Ty)
    : InstructionBase(Loc, Ty) {
  sharedUInt8().StringLiteralInst.encoding = uint8_t(encoding);
  sharedUInt32().StringLiteralInst.length = Text.size();
  memcpy(getTrailingObjects<char>(), Text.data(), Text.size());

  // It is undefined behavior to feed ill-formed UTF-8 into `Swift.String`;
  // however, the compiler creates string literals in many places, so there's a
  // risk of a mistake. StringLiteralInsts can be optimized into
  // IntegerLiteralInsts before reaching IRGen, so this constructor is the best
  // chokepoint to validate *all* string literals that may eventually end up in
  // a binary.
  assert((encoding == Encoding::Bytes || unicode::isWellFormedUTF8(Text))
            && "Created StringLiteralInst with ill-formed UTF-8");
}

StringLiteralInst *StringLiteralInst::create(SILDebugLocation Loc,
                                             StringRef text, Encoding encoding,
                                             SILModule &M) {
  void *buf
    = allocateLiteralInstWithTextSize<StringLiteralInst>(M, text.size());

  auto Ty = SILType::getRawPointerType(M.getASTContext());
  return ::new (buf) StringLiteralInst(Loc, text, encoding, Ty);
}

CondFailInst::CondFailInst(SILDebugLocation DebugLoc, SILValue Operand,
                           StringRef Message)
      : UnaryInstructionBase(DebugLoc, Operand),
        MessageSize(Message.size()) {
  memcpy(getTrailingObjects<char>(), Message.data(), Message.size());
}

CondFailInst *CondFailInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                                   StringRef Message, SILModule &M) {

  auto Size = totalSizeToAlloc<char>(Message.size());
  auto Buffer = M.allocateInst(Size, alignof(CondFailInst));
  return ::new (Buffer) CondFailInst(DebugLoc, Operand, Message);
}

uint64_t StringLiteralInst::getCodeUnitCount() {
  return sharedUInt32().StringLiteralInst.length;
}

StoreInst::StoreInst(
    SILDebugLocation Loc, SILValue Src, SILValue Dest,
    StoreOwnershipQualifier Qualifier = StoreOwnershipQualifier::Unqualified)
    : InstructionBase(Loc), Operands(this, Src, Dest) {
  sharedUInt8().StoreInst.ownershipQualifier = uint8_t(Qualifier);
}

StoreBorrowInst::StoreBorrowInst(SILDebugLocation DebugLoc, SILValue Src,
                                 SILValue Dest)
    : InstructionBase(DebugLoc, Dest->getType()),
      Operands(this, Src, Dest) {}

StringRef swift::getSILAccessKindName(SILAccessKind kind) {
  switch (kind) {
  case SILAccessKind::Init: return "init";
  case SILAccessKind::Read: return "read";
  case SILAccessKind::Modify: return "modify";
  case SILAccessKind::Deinit: return "deinit";
  }
  llvm_unreachable("bad access kind");
}

StringRef swift::getSILAccessEnforcementName(SILAccessEnforcement enforcement) {
  switch (enforcement) {
  case SILAccessEnforcement::Unknown: return "unknown";
  case SILAccessEnforcement::Static: return "static";
  case SILAccessEnforcement::Dynamic: return "dynamic";
  case SILAccessEnforcement::Unsafe: return "unsafe";
  case SILAccessEnforcement::Signed:
    return "signed";
  }
  llvm_unreachable("bad access enforcement");
}

AssignInst::AssignInst(SILDebugLocation Loc, SILValue Src, SILValue Dest,
                       AssignOwnershipQualifier Qualifier) :
    AssignInstBase(Loc, Src, Dest) {
  sharedUInt8().AssignInst.ownershipQualifier = uint8_t(Qualifier);
}

AssignByWrapperInst::AssignByWrapperInst(SILDebugLocation Loc,
                                         SILValue Src, SILValue Dest,
                                         SILValue Initializer, SILValue Setter,
                                         AssignByWrapperInst::Mode mode)
    : AssignInstBase(Loc, Src, Dest, Initializer, Setter) {
  assert(Initializer->getType().is<SILFunctionType>());
  sharedUInt8().AssignByWrapperInst.mode = uint8_t(mode);
}

AssignOrInitInst::AssignOrInitInst(SILDebugLocation Loc, SILValue Src,
                                   SILValue Initializer, SILValue Setter,
                                   AssignOrInitInst::Mode Mode)
    : InstructionBase<SILInstructionKind::AssignOrInitInst, NonValueInstruction>(Loc),
      Operands(this, Src, Initializer, Setter) {
  assert(Initializer->getType().is<SILFunctionType>());
  sharedUInt8().AssignOrInitInst.mode = uint8_t(Mode);
  Assignments.resize(getNumInitializedProperties());
}

void AssignOrInitInst::markAsInitialized(VarDecl *property) {
  auto toInitProperties = getInitializedProperties();
  for (unsigned index : indices(toInitProperties)) {
    if (toInitProperties[index] == property) {
      markAsInitialized(index);
      break;
    }
  }
}

void AssignOrInitInst::markAsInitialized(unsigned propertyIdx) {
  assert(propertyIdx < getNumInitializedProperties());
  Assignments.set(propertyIdx);
}

bool AssignOrInitInst::isPropertyAlreadyInitialized(unsigned propertyIdx) {
  assert(propertyIdx < Assignments.size());
  return Assignments.test(propertyIdx);
}

AccessorDecl *AssignOrInitInst::getReferencedInitAccessor() const {
  auto *initRef = cast<FunctionRefInst>(getInitializer());
  auto *accessorRef = initRef->getReferencedFunctionOrNull();
  assert(accessorRef);
  return dyn_cast_or_null<AccessorDecl>(accessorRef->getDeclContext());
}

unsigned AssignOrInitInst::getNumInitializedProperties() const {
  if (auto *accessor = getReferencedInitAccessor()) {
    auto *initAttr = accessor->getAttrs().getAttribute<InitializesAttr>();
    return initAttr ? initAttr->getNumProperties() : 0;
  }
  return 0;
}

ArrayRef<VarDecl *> AssignOrInitInst::getInitializedProperties() const {
  if (auto *accessor = getReferencedInitAccessor()) {
    if (auto *initAttr = accessor->getAttrs().getAttribute<InitializesAttr>())
      return initAttr->getPropertyDecls(accessor);
  }
  return {};
}

ArrayRef<VarDecl *> AssignOrInitInst::getAccessedProperties() const {
  if (auto *accessor = getReferencedInitAccessor()) {
    if (auto *accessAttr = accessor->getAttrs().getAttribute<AccessesAttr>())
      return accessAttr->getPropertyDecls(accessor);
  }
  return {};
}

MarkFunctionEscapeInst *
MarkFunctionEscapeInst::create(SILDebugLocation Loc,
                               ArrayRef<SILValue> Elements, SILFunction &F) {
  auto Size = totalSizeToAlloc<swift::Operand>(Elements.size());
  auto Buf = F.getModule().allocateInst(Size, alignof(MarkFunctionEscapeInst));
  return ::new(Buf) MarkFunctionEscapeInst(Loc, Elements);
}

CopyAddrInst::CopyAddrInst(SILDebugLocation Loc, SILValue SrcLValue,
                           SILValue DestLValue, IsTake_t isTakeOfSrc,
                           IsInitialization_t isInitializationOfDest)
    : InstructionBase(Loc), Operands(this, SrcLValue, DestLValue) {
    sharedUInt8().CopyAddrInst.isTakeOfSrc = bool(isTakeOfSrc);
    sharedUInt8().CopyAddrInst.isInitializationOfDest =
      bool(isInitializationOfDest);
  }

  ExplicitCopyAddrInst::ExplicitCopyAddrInst(
      SILDebugLocation Loc, SILValue SrcLValue, SILValue DestLValue,
      IsTake_t isTakeOfSrc, IsInitialization_t isInitializationOfDest)
      : InstructionBase(Loc), Operands(this, SrcLValue, DestLValue) {
    sharedUInt8().ExplicitCopyAddrInst.isTakeOfSrc = bool(isTakeOfSrc);
    sharedUInt8().ExplicitCopyAddrInst.isInitializationOfDest =
        bool(isInitializationOfDest);
  }

BindMemoryInst *
BindMemoryInst::create(SILDebugLocation Loc, SILValue Base, SILValue Index,
                       SILType BoundType, SILFunction &F) {
  auto tokenTy = SILType::getBuiltinWordType(F.getASTContext());
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F,
                               BoundType.getASTType());
  auto Size = totalSizeToAlloc<swift::Operand>(TypeDependentOperands.size() +
                                               NumFixedOpers);
  auto Buffer = F.getModule().allocateInst(Size, alignof(BindMemoryInst));
  return ::new (Buffer) BindMemoryInst(Loc, Base, Index, BoundType, tokenTy,
                                       TypeDependentOperands);
}

UncheckedRefCastAddrInst::
UncheckedRefCastAddrInst(SILDebugLocation Loc, SILValue src, CanType srcType,
                         SILValue dest, CanType targetType,
                         ArrayRef<SILValue> TypeDependentOperands)
    : AddrCastInstBase(Loc, src, srcType, dest, targetType,
        TypeDependentOperands) {}

UncheckedRefCastAddrInst *
UncheckedRefCastAddrInst::create(SILDebugLocation Loc, SILValue src,
        CanType srcType, SILValue dest, CanType targetType, SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 4> allOperands;
  collectTypeDependentOperands(allOperands, F, srcType, targetType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(2 + allOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedRefCastAddrInst));
  return ::new (Buffer) UncheckedRefCastAddrInst(Loc, src, srcType,
    dest, targetType, allOperands);
}

UnconditionalCheckedCastAddrInst::UnconditionalCheckedCastAddrInst(
    SILDebugLocation Loc, SILValue src, CanType srcType, SILValue dest,
    CanType targetType, ArrayRef<SILValue> TypeDependentOperands)
    : AddrCastInstBase(Loc, src, srcType, dest, targetType,
        TypeDependentOperands) {}

UnconditionalCheckedCastAddrInst *
UnconditionalCheckedCastAddrInst::create(SILDebugLocation Loc, SILValue src,
        CanType srcType, SILValue dest, CanType targetType, SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 4> allOperands;
  collectTypeDependentOperands(allOperands, F, srcType, targetType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(2 + allOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UnconditionalCheckedCastAddrInst));
  return ::new (Buffer) UnconditionalCheckedCastAddrInst(Loc, src, srcType,
    dest, targetType, allOperands);
}

CheckedCastAddrBranchInst::CheckedCastAddrBranchInst(
  SILDebugLocation DebugLoc, CastConsumptionKind consumptionKind,
  SILValue src, CanType srcType, SILValue dest, CanType targetType,
  ArrayRef<SILValue> TypeDependentOperands,
  SILBasicBlock *successBB, SILBasicBlock *failureBB,
  ProfileCounter Target1Count, ProfileCounter Target2Count)
      : AddrCastInstBase(DebugLoc, src, srcType, dest,
            targetType, TypeDependentOperands, consumptionKind,
            successBB, failureBB, Target1Count, Target2Count) {
  assert(consumptionKind != CastConsumptionKind::BorrowAlways &&
         "BorrowAlways is not supported on addresses");
}

CheckedCastAddrBranchInst *
CheckedCastAddrBranchInst::create(SILDebugLocation DebugLoc,
         CastConsumptionKind consumptionKind,
         SILValue src, CanType srcType, SILValue dest, CanType targetType,
         SILBasicBlock *successBB, SILBasicBlock *failureBB,
         ProfileCounter Target1Count, ProfileCounter Target2Count,
         SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 4> allOperands;
  collectTypeDependentOperands(allOperands, F, srcType, targetType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(2 + allOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(CheckedCastAddrBranchInst));
  return ::new (Buffer) CheckedCastAddrBranchInst(DebugLoc, consumptionKind,
    src, srcType, dest, targetType, allOperands,
    successBB, failureBB, Target1Count, Target2Count);
}

StructInst *StructInst::create(SILDebugLocation Loc, SILType Ty,
                               ArrayRef<SILValue> Elements, SILModule &M,
                               ValueOwnershipKind forwardingOwnershipKind) {
  auto Size = totalSizeToAlloc<swift::Operand>(Elements.size());
  auto Buffer = M.allocateInst(Size, alignof(StructInst));
  return ::new (Buffer) StructInst(Loc, Ty, Elements, forwardingOwnershipKind);
}

StructInst::StructInst(SILDebugLocation Loc, SILType Ty,
                       ArrayRef<SILValue> Elems,
                       ValueOwnershipKind forwardingOwnershipKind)
    : InstructionBaseWithTrailingOperands(Elems, Loc, Ty,
                                          forwardingOwnershipKind) {
  assert(!Ty.getStructOrBoundGenericStruct()->hasUnreferenceableStorage());
}

ObjectInst *ObjectInst::create(SILDebugLocation Loc, SILType Ty,
                               ArrayRef<SILValue> Elements,
                               unsigned NumBaseElements, SILModule &M,
                               ValueOwnershipKind forwardingOwnershipKind) {
  auto Size = totalSizeToAlloc<swift::Operand>(Elements.size());
  auto Buffer = M.allocateInst(Size, alignof(ObjectInst));
  return ::new (Buffer)
      ObjectInst(Loc, Ty, Elements, NumBaseElements, forwardingOwnershipKind);
}

TupleInst *TupleInst::create(SILDebugLocation Loc, SILType Ty,
                             ArrayRef<SILValue> Elements, SILModule &M,
                             ValueOwnershipKind forwardingOwnershipKind) {
  auto Size = totalSizeToAlloc<swift::Operand>(Elements.size());
  auto Buffer = M.allocateInst(Size, alignof(TupleInst));
  return ::new (Buffer) TupleInst(Loc, Ty, Elements, forwardingOwnershipKind);
}

bool TupleExtractInst::isTrivialEltOfOneRCIDTuple() const {
  auto *F = getFunction();

  // If we are not trivial, bail.
  if (!getType().isTrivial(*F))
    return false;

  // If the elt we are extracting is trivial, we cannot have any non trivial
  // fields.
  if (getOperand()->getType().isTrivial(*F))
    return false;

  // Ok, now we know that our tuple has non-trivial fields. Make sure that our
  // parent tuple has only one non-trivial field.
  bool FoundNonTrivialField = false;
  SILType OpTy = getOperand()->getType();
  unsigned FieldNo = getFieldIndex();

  // For each element index of the tuple...
  for (unsigned i = 0, e = getNumTupleElts(); i != e; ++i) {
    // If the element index is the one we are extracting, skip it...
    if (i == FieldNo)
      continue;

    // Otherwise check if we have a non-trivial type. If we don't have one,
    // continue.
    if (OpTy.getTupleElementType(i).isTrivial(*F))
      continue;

    // Ok, this type is non-trivial. If we have not seen a non-trivial field
    // yet, set the FoundNonTrivialField flag.
    if (!FoundNonTrivialField) {
      FoundNonTrivialField = true;
      continue;
    }

    // If we have seen a field and thus the FoundNonTrivialField flag is set,
    // return false.
    return false;
  }

  // We found only one trivial field.
  assert(FoundNonTrivialField && "Tuple is non-trivial, but does not have a "
                                 "non-trivial element?!");
  return true;
}

bool TupleExtractInst::isEltOnlyNonTrivialElt() const {
  auto *F = getFunction();

  // If the elt we are extracting is trivial, we cannot be a non-trivial
  // field... return false.
  if (getType().isTrivial(*F))
    return false;

  // Ok, we know that the elt we are extracting is non-trivial. Make sure that
  // we have no other non-trivial elts.
  SILType OpTy = getOperand()->getType();
  unsigned FieldNo = getFieldIndex();

  // For each element index of the tuple...
  for (unsigned i = 0, e = getNumTupleElts(); i != e; ++i) {
    // If the element index is the one we are extracting, skip it...
    if (i == FieldNo)
      continue;

    // Otherwise check if we have a non-trivial type. If we don't have one,
    // continue.
    if (OpTy.getTupleElementType(i).isTrivial(*F))
      continue;

    // If we do have a non-trivial type, return false. We have multiple
    // non-trivial types violating our condition.
    return false;
  }

  // We checked every other elt of the tuple and did not find any
  // non-trivial elt except for ourselves. Return true.
  return true;
}

unsigned swift::getNumFieldsInNominal(NominalTypeDecl *decl) {
  unsigned count = 0;
  if (auto *classDecl = dyn_cast<ClassDecl>(decl)) {
    for (auto *superDecl = classDecl->getSuperclassDecl(); superDecl != nullptr;
         superDecl = superDecl->getSuperclassDecl()) {
      count += superDecl->getStoredProperties().size();
    }
  }
  return count + decl->getStoredProperties().size();
}

/// Get the property for a struct or class by its unique index.
VarDecl *swift::getIndexedField(NominalTypeDecl *decl, unsigned index) {
  if (auto *structDecl = dyn_cast<StructDecl>(decl)) {
    return structDecl->getStoredProperties()[index];
  }
  auto *classDecl = cast<ClassDecl>(decl);
  SmallVector<ClassDecl *, 3> superclasses;
  for (auto *superDecl = classDecl; superDecl != nullptr;
       superDecl = superDecl->getSuperclassDecl()) {
    superclasses.push_back(superDecl);
  }
  std::reverse(superclasses.begin(), superclasses.end());
  for (auto *superDecl : superclasses) {
    if (index < superDecl->getStoredProperties().size()) {
      return superDecl->getStoredProperties()[index];
    }
    index -= superDecl->getStoredProperties().size();
  }
  return nullptr;
}

// FIXME: this should be cached during cacheFieldIndex().
bool StructExtractInst::isTrivialFieldOfOneRCIDStruct() const {
  auto *F = getFunction();

  // If we are not trivial, bail.
  if (!getType().isTrivial(*F))
    return false;

  SILType StructTy = getOperand()->getType();

  // If the elt we are extracting is trivial, we cannot have any non trivial
  // fields.
  if (StructTy.isTrivial(*F))
    return false;

  // Ok, now we know that our tuple has non-trivial fields. Make sure that our
  // parent tuple has only one non-trivial field.
  bool FoundNonTrivialField = false;

  // For each element index of the tuple...
  for (VarDecl *D : getStructDecl()->getStoredProperties()) {
    // If the field is the one we are extracting, skip it...
    if (getField() == D)
      continue;

    // Otherwise check if we have a non-trivial type. If we don't have one,
    // continue.
    if (StructTy.getFieldType(D, F->getModule(), TypeExpansionContext(*F))
            .isTrivial(*F))
      continue;

    // Ok, this type is non-trivial. If we have not seen a non-trivial field
    // yet, set the FoundNonTrivialField flag.
    if (!FoundNonTrivialField) {
      FoundNonTrivialField = true;
      continue;
    }

    // If we have seen a field and thus the FoundNonTrivialField flag is set,
    // return false.
    return false;
  }

  // We found only one trivial field.
  assert(FoundNonTrivialField && "Struct is non-trivial, but does not have a "
                                 "non-trivial field?!");
  return true;
}

/// Return true if we are extracting the only non-trivial field of out parent
/// struct. This implies that a ref count operation on the aggregate is
/// equivalent to a ref count operation on this field.
///
/// FIXME: this should be cached during cacheFieldIndex().
bool StructExtractInst::isFieldOnlyNonTrivialField() const {
  auto *F = getFunction();

  // If the field we are extracting is trivial, we cannot be a non-trivial
  // field... return false.
  if (getType().isTrivial(*F))
    return false;

  SILType StructTy = getOperand()->getType();

  // Ok, we are visiting a non-trivial field. Then for every stored field...
  for (VarDecl *D : getStructDecl()->getStoredProperties()) {
    // If we are visiting our own field continue.
    if (getField() == D)
      continue;

    // Ok, we have a field that is not equal to the field we are
    // extracting. If that field is trivial, we do not care about
    // it... continue.
    if (StructTy.getFieldType(D, F->getModule(), TypeExpansionContext(*F))
            .isTrivial(*F))
      continue;

    // We have found a non trivial member that is not the member we are
    // extracting, fail.
    return false;
  }

  // We checked every other field of the struct and did not find any
  // non-trivial fields except for ourselves. Return true.
  return true;
}

//===----------------------------------------------------------------------===//
// Instructions representing terminators
//===----------------------------------------------------------------------===//


TermInst::SuccessorListTy TermInst::getSuccessors() {
  switch (getKind()) {
#define TERMINATOR(ID, NAME, PARENT, MEMBEHAVIOR, MAYRELEASE) \
  case SILInstructionKind::ID: return cast<ID>(this)->getSuccessors();
#include "swift/SIL/SILNodes.def"
  default: llvm_unreachable("not a terminator");
  }
  llvm_unreachable("bad instruction kind");
}

void TermInst::replaceBranchTarget(SILBasicBlock *oldDest, SILBasicBlock *newDest) {
  for (SILSuccessor &succ : getSuccessors()) {
    if (succ.getBB() == oldDest) {
      succ = newDest;
    }
  }
}

bool TermInst::isFunctionExiting() const {
  switch (getTermKind()) {
  case TermKind::AwaitAsyncContinuationInst:
  case TermKind::BranchInst:
  case TermKind::CondBranchInst:
  case TermKind::SwitchValueInst:
  case TermKind::SwitchEnumInst:
  case TermKind::SwitchEnumAddrInst:
  case TermKind::DynamicMethodBranchInst:
  case TermKind::CheckedCastBranchInst:
  case TermKind::CheckedCastAddrBranchInst:
  case TermKind::UnreachableInst:
  case TermKind::TryApplyInst:
  case TermKind::YieldInst:
    return false;
  case TermKind::ReturnInst:
  case TermKind::ThrowInst:
  case TermKind::UnwindInst:
    return true;
  }

  llvm_unreachable("Unhandled TermKind in switch.");
}

bool TermInst::isProgramTerminating() const {
  switch (getTermKind()) {
  case TermKind::AwaitAsyncContinuationInst:
  case TermKind::BranchInst:
  case TermKind::CondBranchInst:
  case TermKind::SwitchValueInst:
  case TermKind::SwitchEnumInst:
  case TermKind::SwitchEnumAddrInst:
  case TermKind::DynamicMethodBranchInst:
  case TermKind::CheckedCastBranchInst:
  case TermKind::CheckedCastAddrBranchInst:
  case TermKind::ReturnInst:
  case TermKind::ThrowInst:
  case TermKind::UnwindInst:
  case TermKind::TryApplyInst:
  case TermKind::YieldInst:
    return false;
  case TermKind::UnreachableInst:
    return true;
  }

  llvm_unreachable("Unhandled TermKind in switch.");
}

TermInst::SuccessorBlockArgumentListTy
TermInst::getSuccessorBlockArgumentLists() const {
  function_ref<ArrayRef<SILArgument *>(const SILSuccessor &)> op;
  op = [](const SILSuccessor &succ) -> ArrayRef<SILArgument *> {
    return succ.getBB()->getArguments();
  };
  return SuccessorBlockArgumentListTy(getSuccessors(), op);
}

const Operand *TermInst::forwardedOperand() const {
  switch (getTermKind()) {
  case TermKind::UnwindInst:
  case TermKind::UnreachableInst:
  case TermKind::ReturnInst:
  case TermKind::ThrowInst:
  case TermKind::YieldInst:
  case TermKind::TryApplyInst:
  case TermKind::CondBranchInst:
  case TermKind::BranchInst:
  case TermKind::SwitchEnumAddrInst:
  case TermKind::SwitchValueInst:
  case TermKind::DynamicMethodBranchInst:
  case TermKind::CheckedCastAddrBranchInst:
  case TermKind::AwaitAsyncContinuationInst:
    return nullptr;
  case TermKind::SwitchEnumInst: {
    auto *switchEnum = cast<SwitchEnumInst>(this);
    if (!switchEnum->preservesOwnership())
      return nullptr;

    return &switchEnum->getOperandRef();
  }
  case TermKind::CheckedCastBranchInst: {
    auto *checkedCast = cast<CheckedCastBranchInst>(this);
    if (!checkedCast->preservesOwnership())
      return nullptr;

    return &checkedCast->getOperandRef();
  }
  }
  llvm_unreachable("Covered switch isn't covered.");
}

YieldInst *YieldInst::create(SILDebugLocation loc,
                             ArrayRef<SILValue> yieldedValues,
                             SILBasicBlock *normalBB, SILBasicBlock *unwindBB,
                             SILFunction &F) {
  auto Size = totalSizeToAlloc<swift::Operand>(yieldedValues.size());
  void *Buffer = F.getModule().allocateInst(Size, alignof(YieldInst));
  return ::new (Buffer) YieldInst(loc, yieldedValues, normalBB, unwindBB);
}

SILYieldInfo YieldInst::getYieldInfoForOperand(const Operand &op) const {
  // We expect op to be our operand.
  assert(op.getUser() == this);
  auto conv = getFunction()->getConventions();
  return conv.getYieldInfoForOperandIndex(op.getOperandNumber());
}

SILArgumentConvention
YieldInst::getArgumentConventionForOperand(const Operand &op) const {
  auto conv = getYieldInfoForOperand(op).getConvention();
  return SILArgumentConvention(conv);
}

BranchInst *BranchInst::create(SILDebugLocation Loc, SILBasicBlock *DestBB,
                               SILFunction &F) {
  return create(Loc, DestBB, {}, F);
}

BranchInst *BranchInst::create(SILDebugLocation Loc,
                               SILBasicBlock *DestBB, ArrayRef<SILValue> Args,
                               SILFunction &F) {
  auto Size = totalSizeToAlloc<swift::Operand>(Args.size());
  auto Buffer = F.getModule().allocateInst(Size, alignof(BranchInst));
  return ::new (Buffer) BranchInst(Loc, DestBB, Args);
}

CondBranchInst::CondBranchInst(SILDebugLocation Loc, SILValue Condition,
                               SILBasicBlock *TrueBB, SILBasicBlock *FalseBB,
                               ArrayRef<SILValue> Args, unsigned NumTrue,
                               unsigned NumFalse, ProfileCounter TrueBBCount,
                               ProfileCounter FalseBBCount)
    : InstructionBaseWithTrailingOperands(Condition, Args, Loc),
      DestBBs{{{this, TrueBB, TrueBBCount}, {this, FalseBB, FalseBBCount}}},
      numTrueArguments(NumTrue) {
  assert(Args.size() == (NumTrue + NumFalse) && "Invalid number of args");
  assert(TrueBB != FalseBB && "Identical destinations");
}

CondBranchInst *CondBranchInst::create(SILDebugLocation Loc, SILValue Condition,
                                       SILBasicBlock *TrueBB,
                                       SILBasicBlock *FalseBB,
                                       ProfileCounter TrueBBCount,
                                       ProfileCounter FalseBBCount,
                                       SILFunction &F) {
  return create(Loc, Condition, TrueBB, {}, FalseBB, {}, TrueBBCount,
                FalseBBCount, F);
}

CondBranchInst *
CondBranchInst::create(SILDebugLocation Loc, SILValue Condition,
                       SILBasicBlock *TrueBB, ArrayRef<SILValue> TrueArgs,
                       SILBasicBlock *FalseBB, ArrayRef<SILValue> FalseArgs,
                       ProfileCounter TrueBBCount, ProfileCounter FalseBBCount,
                       SILFunction &F) {
  SmallVector<SILValue, 4> Args;
  Args.append(TrueArgs.begin(), TrueArgs.end());
  Args.append(FalseArgs.begin(), FalseArgs.end());

  auto Size = totalSizeToAlloc<swift::Operand>(Args.size() + NumFixedOpers);
  auto Buffer = F.getModule().allocateInst(Size, alignof(CondBranchInst));
  return ::new (Buffer) CondBranchInst(Loc, Condition, TrueBB, FalseBB, Args,
                                       TrueArgs.size(), FalseArgs.size(),
                                       TrueBBCount, FalseBBCount);
}

Operand *CondBranchInst::getOperandForDestBB(const SILBasicBlock *destBlock,
                                             const SILArgument *arg) const {
  return getOperandForDestBB(destBlock, arg->getIndex());
}

Operand *CondBranchInst::getOperandForDestBB(const SILBasicBlock *destBlock,
                                             unsigned argIndex) const {
  // If TrueBB and FalseBB equal, we cannot find an arg for this DestBB so
  // return an empty SILValue.
  if (getTrueBB() == getFalseBB()) {
    assert(destBlock == getTrueBB() &&
           "DestBB is not a target of this cond_br");
    return nullptr;
  }

  auto *self = const_cast<CondBranchInst *>(this);
  if (destBlock == getTrueBB()) {
    return &self->getAllOperands()[NumFixedOpers + argIndex];
  }

  assert(destBlock == getFalseBB() &&
         "By process of elimination BB must be false BB");
  return &self->getAllOperands()[NumFixedOpers + getNumTrueArgs() + argIndex];
}

void CondBranchInst::swapSuccessors() {
  // Swap our destinations.
  SILBasicBlock *First = DestBBs[0].getBB();
  DestBBs[0] = DestBBs[1].getBB();
  DestBBs[1] = First;

  // If we don't have any arguments return.
  if (!getNumTrueArgs() && !getNumFalseArgs())
    return;

  // Otherwise swap our true and false arguments.
  MutableArrayRef<Operand> Ops = getAllOperands();
  llvm::SmallVector<SILValue, 4> TrueOps;
  for (SILValue V : getTrueArgs())
    TrueOps.push_back(V);

  auto FalseArgs = getFalseArgs();
  for (unsigned i = 0, e = getNumFalseArgs(); i < e; ++i) {
    Ops[NumFixedOpers+i].set(FalseArgs[i]);
  }

  for (unsigned i = 0, e = getNumTrueArgs(); i < e; ++i) {
    Ops[NumFixedOpers+i+getNumFalseArgs()].set(TrueOps[i]);
  }

  // Finally swap the number of arguments that we have. The number of false
  // arguments is derived from the number of true arguments, therefore:
  numTrueArguments = getNumFalseArgs();
}

SwitchValueInst::SwitchValueInst(SILDebugLocation Loc, SILValue Operand,
                                 SILBasicBlock *DefaultBB,
                                 ArrayRef<SILValue> Cases,
                                 ArrayRef<SILBasicBlock *> BBs)
    : InstructionBaseWithTrailingOperands(Operand, Cases, Loc) {
  sharedUInt8().SwitchValueInst.hasDefault = bool(DefaultBB);
  // Initialize the successor array.
  auto *succs = getSuccessorBuf();
  unsigned OperandBitWidth = 0;

  if (auto OperandTy = Operand->getType().getAs<BuiltinIntegerType>()) {
    OperandBitWidth = OperandTy->getGreatestWidth();
  }

  for (unsigned i = 0, size = Cases.size(); i < size; ++i) {
    // If we have undef, just add the case and continue.
    if (isa<SILUndef>(Cases[i])) {
      ::new (succs + i) SILSuccessor(this, BBs[i]);
      continue;
    }

    if (OperandBitWidth) {
      auto *IL = dyn_cast<IntegerLiteralInst>(Cases[i]);
      assert(IL && "switch_value case value should be of an integer type");
      assert(IL->getValue().getBitWidth() == OperandBitWidth &&
             "switch_value case value is not same bit width as operand");
      (void)IL;
    } else {
      auto *FR = dyn_cast<FunctionRefInst>(Cases[i]);
      if (!FR) {
        if (auto *CF = dyn_cast<ConvertFunctionInst>(Cases[i])) {
          FR = dyn_cast<FunctionRefInst>(CF->getOperand());
        }
      }
      assert(FR && "switch_value case value should be a function reference");
    }
    ::new (succs + i) SILSuccessor(this, BBs[i]);
  }

  if (hasDefault())
    ::new (succs + getNumCases()) SILSuccessor(this, DefaultBB);
}

SwitchValueInst::~SwitchValueInst() {
  // Destroy the successor records to keep the CFG up to date.
  auto *succs = getSuccessorBuf();
  for (unsigned i = 0, end = getNumCases() + hasDefault(); i < end; ++i) {
    succs[i].~SILSuccessor();
  }
}

SwitchValueInst *SwitchValueInst::create(
    SILDebugLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
    ArrayRef<std::pair<SILValue, SILBasicBlock *>> CaseBBs, SILFunction &F) {
  // Allocate enough room for the instruction with tail-allocated data for all
  // the case values and the SILSuccessor arrays. There are `CaseBBs.size()`
  // SILValues and `CaseBBs.size() + (DefaultBB ? 1 : 0)` successors.
  SmallVector<SILValue, 8> Cases;
  SmallVector<SILBasicBlock *, 8> BBs;
  unsigned numCases = CaseBBs.size();
  unsigned numSuccessors = numCases + (DefaultBB ? 1 : 0);
  for (auto pair: CaseBBs) {
    Cases.push_back(pair.first);
    BBs.push_back(pair.second);
  }
  auto size = totalSizeToAlloc<swift::Operand, SILSuccessor>(numCases + 1,
                                                             numSuccessors);
  auto buf = F.getModule().allocateInst(size, alignof(SwitchValueInst));
  return ::new (buf) SwitchValueInst(Loc, Operand, DefaultBB, Cases, BBs);
}

SelectValueInst::SelectValueInst(SILDebugLocation DebugLoc, SILValue Operand,
                                 SILType Type, SILValue DefaultResult,
                                 ArrayRef<SILValue> CaseValuesAndResults)
    : InstructionBaseWithTrailingOperands(Operand, CaseValuesAndResults,
                                          DebugLoc, Type) {}

SelectValueInst *
SelectValueInst::create(SILDebugLocation Loc, SILValue Operand, SILType Type,
                        SILValue DefaultResult,
                        ArrayRef<std::pair<SILValue, SILValue>> CaseValues,
                        SILModule &M) {
  // Allocate enough room for the instruction with tail-allocated data for all
  // the case values and the SILSuccessor arrays. There are `CaseBBs.size()`
  // SILValues and `CaseBBs.size() + (DefaultBB ? 1 : 0)` successors.
  SmallVector<SILValue, 8> CaseValuesAndResults;
  for (auto pair : CaseValues) {
    CaseValuesAndResults.push_back(pair.first);
    CaseValuesAndResults.push_back(pair.second);
  }

  if ((bool)DefaultResult)
    CaseValuesAndResults.push_back(DefaultResult);

  auto Size = totalSizeToAlloc<swift::Operand>(CaseValuesAndResults.size() + 1);
  auto Buf = M.allocateInst(Size, alignof(SelectValueInst));
  return ::new (Buf)
      SelectValueInst(Loc, Operand, Type, DefaultResult, CaseValuesAndResults);
}

template <typename SELECT_ENUM_INST>
SELECT_ENUM_INST *SelectEnumInstBase::createSelectEnum(
    SILDebugLocation Loc, SILValue Operand, SILType Ty, SILValue DefaultValue,
    ArrayRef<std::pair<EnumElementDecl *, SILValue>> DeclsAndValues,
    SILModule &Mod, Optional<ArrayRef<ProfileCounter>> CaseCounts,
    ProfileCounter DefaultCount, ValueOwnershipKind forwardingOwnership) {
  // Allocate enough room for the instruction with tail-allocated
  // EnumElementDecl and operand arrays. There are `CaseBBs.size()` decls
  // and `CaseBBs.size() + (DefaultBB ? 1 : 0)` values.
  SmallVector<SILValue, 4> CaseValues;
  SmallVector<EnumElementDecl*, 4> CaseDecls;
  for (auto &pair : DeclsAndValues) {
    CaseValues.push_back(pair.second);
    CaseDecls.push_back(pair.first);
  }

  if (DefaultValue)
    CaseValues.push_back(DefaultValue);

  auto Size = SELECT_ENUM_INST::template
    totalSizeToAlloc<swift::Operand, EnumElementDecl*>(CaseValues.size() + 1,
                                                       CaseDecls.size());
  auto Buf = Mod.allocateInst(Size + sizeof(ProfileCounter),
                              alignof(SELECT_ENUM_INST));
  return ::new (Buf) SELECT_ENUM_INST(Loc, Operand, Ty, bool(DefaultValue),
                                      CaseValues, CaseDecls, CaseCounts,
                                      DefaultCount, forwardingOwnership);
}

SelectEnumInst *SelectEnumInst::create(
    SILDebugLocation Loc, SILValue Operand, SILType Type, SILValue DefaultValue,
    ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues, SILModule &M,
    Optional<ArrayRef<ProfileCounter>> CaseCounts, ProfileCounter DefaultCount,
    ValueOwnershipKind forwardingOwnership) {
  return createSelectEnum<SelectEnumInst>(Loc, Operand, Type, DefaultValue,
                                          CaseValues, M, CaseCounts,
                                          DefaultCount, forwardingOwnership);
}

SelectEnumAddrInst *SelectEnumAddrInst::create(
    SILDebugLocation Loc, SILValue Operand, SILType Type, SILValue DefaultValue,
    ArrayRef<std::pair<EnumElementDecl *, SILValue>> CaseValues, SILModule &M,
    Optional<ArrayRef<ProfileCounter>> CaseCounts,
    ProfileCounter DefaultCount) {
  // We always pass in false since SelectEnumAddrInst doesn't use ownership. We
  // have to pass something in since SelectEnumInst /does/ need to consider
  // ownership and both use the same creation function.
  return createSelectEnum<SelectEnumAddrInst>(
      Loc, Operand, Type, DefaultValue, CaseValues, M, CaseCounts, DefaultCount,
      ValueOwnershipKind(OwnershipKind::None));
}

namespace {
  template <class Inst> EnumElementDecl *
  getUniqueCaseForDefaultValue(Inst *inst, SILValue enumValue) {
    assert(inst->hasDefault() && "doesn't have a default");
    SILType enumType = enumValue->getType();

    EnumDecl *decl = enumType.getEnumOrBoundGenericEnum();
    assert(decl && "switch_enum operand is not an enum");

    const SILFunction *F = inst->getFunction();
    if (!decl->isEffectivelyExhaustive(F->getModule().getSwiftModule(),
                                       F->getResilienceExpansion())) {
      return nullptr;
    }

    llvm::SmallPtrSet<EnumElementDecl *, 4> unswitchedElts;
    for (auto elt : decl->getAllElements())
      unswitchedElts.insert(elt);

    for (unsigned i = 0, e = inst->getNumCases(); i != e; ++i) {
      auto Entry = inst->getCase(i);
      unswitchedElts.erase(Entry.first);
    }

    if (unswitchedElts.size() == 1)
      return *unswitchedElts.begin();

    return nullptr;
  }
} // end anonymous namespace

NullablePtr<EnumElementDecl> SelectEnumInstBase::getUniqueCaseForDefault() {
  return getUniqueCaseForDefaultValue(this, getEnumOperand());
}

NullablePtr<EnumElementDecl> SelectEnumInstBase::getSingleTrueElement() const {
  auto SEIType = getType().getAs<BuiltinIntegerType>();
  if (!SEIType)
    return nullptr;
  if (SEIType->getWidth() != BuiltinIntegerWidth::fixed(1))
    return nullptr;

  // Try to find a single literal "true" case.
  Optional<EnumElementDecl*> TrueElement;
  for (unsigned i = 0, e = getNumCases(); i < e; ++i) {
    auto casePair = getCase(i);
    if (auto intLit = dyn_cast<IntegerLiteralInst>(casePair.second)) {
      if (intLit->getValue() == APInt(1, 1)) {
        if (!TrueElement)
          TrueElement = casePair.first;
        else
          // Use Optional(nullptr) to represent more than one.
          TrueElement = Optional<EnumElementDecl*>(nullptr);
      }
    }
  }

  if (!TrueElement || !*TrueElement)
    return nullptr;
  return *TrueElement;
}

template <typename BaseTy>
template <typename SWITCH_ENUM_INST, typename... RestTys>
SWITCH_ENUM_INST *SwitchEnumInstBase<BaseTy>::createSwitchEnum(
    SILDebugLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
    ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
    SILFunction &F, Optional<ArrayRef<ProfileCounter>> CaseCounts,
    ProfileCounter DefaultCount, RestTys &&... restArgs) {
  // Allocate enough room for the instruction with tail-allocated
  // EnumElementDecl and SILSuccessor arrays. There are `CaseBBs.size()` decls
  // and `CaseBBs.size() + (DefaultBB ? 1 : 0)` successors.
  unsigned numCases = CaseBBs.size();
  unsigned numSuccessors = numCases + (DefaultBB ? 1 : 0);

  void *buf = F.getModule().allocateInst(
      sizeof(SWITCH_ENUM_INST) + sizeof(EnumElementDecl *) * numCases +
          sizeof(SILSuccessor) * numSuccessors,
      alignof(SWITCH_ENUM_INST));
  return ::new (buf)
      SWITCH_ENUM_INST(Loc, Operand, DefaultBB, CaseBBs, CaseCounts,
                       DefaultCount, std::forward<RestTys>(restArgs)...);
}

SwitchEnumInst *SwitchEnumInst::create(
    SILDebugLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
    ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
    SILFunction &F, Optional<ArrayRef<ProfileCounter>> CaseCounts,
    ProfileCounter DefaultCount, ValueOwnershipKind forwardingOwnershipKind) {
  return createSwitchEnum<SwitchEnumInst>(Loc, Operand, DefaultBB, CaseBBs, F,
                                          CaseCounts, DefaultCount,
                                          forwardingOwnershipKind);
}

SwitchEnumAddrInst *SwitchEnumAddrInst::create(
    SILDebugLocation Loc, SILValue Operand, SILBasicBlock *DefaultBB,
    ArrayRef<std::pair<EnumElementDecl *, SILBasicBlock *>> CaseBBs,
    SILFunction &F, Optional<ArrayRef<ProfileCounter>> CaseCounts,
    ProfileCounter DefaultCount) {
  return createSwitchEnum<SwitchEnumAddrInst>(Loc, Operand, DefaultBB, CaseBBs,
                                              F, CaseCounts, DefaultCount);
}

DynamicMethodBranchInst::DynamicMethodBranchInst(SILDebugLocation Loc,
                                                 SILValue Operand,
                                                 SILDeclRef Member,
                                                 SILBasicBlock *HasMethodBB,
                                                 SILBasicBlock *NoMethodBB)
  : InstructionBase(Loc),
    Member(Member),
    DestBBs{{{this, HasMethodBB}, {this, NoMethodBB}}},
    Operands(this, Operand)
{
}

DynamicMethodBranchInst *
DynamicMethodBranchInst::create(SILDebugLocation Loc, SILValue Operand,
                                SILDeclRef Member, SILBasicBlock *HasMethodBB,
                                SILBasicBlock *NoMethodBB, SILFunction &F) {
  void *Buffer = F.getModule().allocateInst(sizeof(DynamicMethodBranchInst),
                                            alignof(DynamicMethodBranchInst));
  return ::new (Buffer)
      DynamicMethodBranchInst(Loc, Operand, Member, HasMethodBB, NoMethodBB);
}

WitnessMethodInst *
WitnessMethodInst::create(SILDebugLocation Loc, CanType LookupType,
                          ProtocolConformanceRef Conformance, SILDeclRef Member,
                          SILType Ty, SILFunction *F) {
  assert(cast<ProtocolDecl>(Member.getDecl()->getDeclContext())
         == Conformance.getRequirement());

  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, LookupType);
  auto Size = totalSizeToAlloc<swift::Operand>(TypeDependentOperands.size());
  auto Buffer = Mod.allocateInst(Size, alignof(WitnessMethodInst));

  return ::new (Buffer) WitnessMethodInst(Loc, LookupType, Conformance, Member,
                                          Ty, TypeDependentOperands);
}

ObjCMethodInst *
ObjCMethodInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                       SILDeclRef Member, SILType Ty, SILFunction *F) {
  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, Ty.getASTType());

  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(ObjCMethodInst));
  return ::new (Buffer) ObjCMethodInst(DebugLoc, Operand,
                                       TypeDependentOperands,
                                       Member, Ty);
}

InitExistentialAddrInst *InitExistentialAddrInst::create(
    SILDebugLocation Loc, SILValue Existential, CanType ConcreteType,
    SILType ConcreteLoweredType, ArrayRef<ProtocolConformanceRef> Conformances,
    SILFunction *F) {
  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, ConcreteType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size,
                                  alignof(InitExistentialAddrInst));
  return ::new (Buffer) InitExistentialAddrInst(Loc, Existential,
                                                TypeDependentOperands,
                                                ConcreteType,
                                                ConcreteLoweredType,
                                                Conformances);
}

InitExistentialValueInst *InitExistentialValueInst::create(
    SILDebugLocation Loc, SILType ExistentialType, CanType ConcreteType,
    SILValue Instance, ArrayRef<ProtocolConformanceRef> Conformances,
    SILFunction *F) {
  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, ConcreteType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());

  void *Buffer = Mod.allocateInst(size, alignof(InitExistentialRefInst));
  return ::new (Buffer)
      InitExistentialValueInst(Loc, ExistentialType, ConcreteType, Instance,
                                TypeDependentOperands, Conformances);
}

InitExistentialRefInst *InitExistentialRefInst::create(
    SILDebugLocation Loc, SILType ExistentialType, CanType ConcreteType,
    SILValue Instance, ArrayRef<ProtocolConformanceRef> Conformances,
    SILFunction *F, ValueOwnershipKind forwardingOwnershipKind) {
  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F, ConcreteType);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());

  void *Buffer = Mod.allocateInst(size, alignof(InitExistentialRefInst));
  return ::new (Buffer) InitExistentialRefInst(
      Loc, ExistentialType, ConcreteType, Instance, TypeDependentOperands,
      Conformances, forwardingOwnershipKind);
}

InitExistentialMetatypeInst::InitExistentialMetatypeInst(
    SILDebugLocation Loc, SILType existentialMetatypeType, SILValue metatype,
    ArrayRef<SILValue> TypeDependentOperands,
    ArrayRef<ProtocolConformanceRef> conformances)
    : UnaryInstructionWithTypeDependentOperandsBase(Loc, metatype,
                                                    TypeDependentOperands,
                                                    existentialMetatypeType),
      NumConformances(conformances.size()) {
  std::uninitialized_copy(conformances.begin(), conformances.end(),
                          getTrailingObjects<ProtocolConformanceRef>());
}

InitExistentialMetatypeInst *InitExistentialMetatypeInst::create(
    SILDebugLocation Loc, SILType existentialMetatypeType, SILValue metatype,
    ArrayRef<ProtocolConformanceRef> conformances, SILFunction *F) {
  SILModule &M = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F,
                               existentialMetatypeType.getASTType());

  unsigned size = totalSizeToAlloc<swift::Operand, ProtocolConformanceRef>(
      1 + TypeDependentOperands.size(), conformances.size());

  void *buffer = M.allocateInst(size, alignof(InitExistentialMetatypeInst));
  return ::new (buffer) InitExistentialMetatypeInst(
      Loc, existentialMetatypeType, metatype,
      TypeDependentOperands, conformances);
}

ArrayRef<ProtocolConformanceRef>
InitExistentialMetatypeInst::getConformances() const {
  return {getTrailingObjects<ProtocolConformanceRef>(), NumConformances};
}

OpenedExistentialAccess swift::getOpenedExistentialAccessFor(AccessKind access) {
  switch (access) {
  case AccessKind::Read:
    return OpenedExistentialAccess::Immutable;
  case AccessKind::ReadWrite:
  case AccessKind::Write:
    return OpenedExistentialAccess::Mutable;
  }
  llvm_unreachable("Uncovered covered switch?");
}

OpenExistentialAddrInst::OpenExistentialAddrInst(
    SILDebugLocation DebugLoc, SILValue Operand, SILType SelfTy,
    OpenedExistentialAccess AccessKind)
    : UnaryInstructionBase(DebugLoc, Operand, SelfTy), ForAccess(AccessKind) {}

OpenExistentialRefInst::OpenExistentialRefInst(
    SILDebugLocation DebugLoc, SILValue Operand, SILType Ty,
    ValueOwnershipKind forwardingOwnershipKind)
    : UnaryInstructionBase(DebugLoc, Operand, Ty, forwardingOwnershipKind) {
  assert(Operand->getType().isObject() && "Operand must be an object.");
  assert(Ty.isObject() && "Result type must be an object type.");
}

OpenExistentialMetatypeInst::OpenExistentialMetatypeInst(
    SILDebugLocation DebugLoc, SILValue operand, SILType ty)
    : UnaryInstructionBase(DebugLoc, operand, ty) {
}

OpenExistentialBoxInst::OpenExistentialBoxInst(
    SILDebugLocation DebugLoc, SILValue operand, SILType ty)
    : UnaryInstructionBase(DebugLoc, operand, ty) {
}

OpenExistentialBoxValueInst::OpenExistentialBoxValueInst(
    SILDebugLocation DebugLoc, SILValue operand, SILType ty,
    ValueOwnershipKind forwardingOwnershipKind)
    : UnaryInstructionBase(DebugLoc, operand, ty, forwardingOwnershipKind) {}

OpenExistentialValueInst::OpenExistentialValueInst(
    SILDebugLocation debugLoc, SILValue operand, SILType selfTy,
    ValueOwnershipKind forwardingOwnershipKind)
    : UnaryInstructionBase(debugLoc, operand, selfTy, forwardingOwnershipKind) {
}

PackLengthInst *PackLengthInst::create(SILFunction &F,
                                       SILDebugLocation loc,
                                       CanPackType packType) {
  auto resultType = SILType::getBuiltinWordType(F.getASTContext());

  // Always reduce the pack shape.
  packType = packType->getReducedShape();

  // Under current limitations, that should reliably eliminate
  // any local archetypes from the pack, but there's no real need to
  // assume that in the SIL representation.
  SmallVector<SILValue, 8> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F, packType);

  size_t size =
    totalSizeToAlloc<swift::Operand>(typeDependentOperands.size());
  void *buffer =
    F.getModule().allocateInst(size, alignof(PackLengthInst));
  return ::new (buffer)
      PackLengthInst(loc, typeDependentOperands, resultType, packType);
}

DynamicPackIndexInst *DynamicPackIndexInst::create(SILFunction &F,
                                                   SILDebugLocation loc,
                                                   SILValue indexOperand,
                                                   CanPackType packType) {
  auto packIndexType = SILType::getPackIndexType(F.getASTContext());

  SmallVector<SILValue, 8> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F, packType);

  size_t size =
    totalSizeToAlloc<swift::Operand>(1 + typeDependentOperands.size());
  void *buffer =
    F.getModule().allocateInst(size, alignof(DynamicPackIndexInst));
  return ::new (buffer)
      DynamicPackIndexInst(loc, indexOperand, typeDependentOperands,
                           packIndexType, packType);
}

PackPackIndexInst *PackPackIndexInst::create(SILFunction &F,
                                             SILDebugLocation loc,
                                             unsigned componentStartIndex,
                                             SILValue indexWithinComponent,
                                             CanPackType packType) {
  assert(componentStartIndex < packType->getNumElements() &&
         "component start index is out of bounds for indexed-into pack type");
  // TODO: assert that the shapes are similar?

  auto packIndexType = SILType::getPackIndexType(F.getASTContext());

  SmallVector<SILValue, 8> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F, packType);

  size_t size =
    totalSizeToAlloc<swift::Operand>(1 + typeDependentOperands.size());
  void *buffer =
    F.getModule().allocateInst(size, alignof(PackPackIndexInst));
  return ::new (buffer)
      PackPackIndexInst(loc, componentStartIndex, indexWithinComponent,
                        typeDependentOperands, packIndexType, packType);
}

ScalarPackIndexInst *ScalarPackIndexInst::create(SILFunction &F,
                                                 SILDebugLocation loc,
                                                 unsigned componentIndex,
                                                 CanPackType packType) {
  assert(componentIndex < packType->getNumElements() &&
         "component index is out of bounds for indexed-into pack type");
  assert(!isa<PackExpansionType>(packType.getElementType(componentIndex)) &&
         "component index for scalar pack index is a pack expansion");

  auto packIndexType = SILType::getPackIndexType(F.getASTContext());

  SmallVector<SILValue, 8> typeDependentOperands;
  collectTypeDependentOperands(typeDependentOperands, F, packType);

  size_t size =
    totalSizeToAlloc<swift::Operand>(typeDependentOperands.size());
  void *buffer =
    F.getModule().allocateInst(size, alignof(ScalarPackIndexInst));
  return ::new (buffer)
      ScalarPackIndexInst(loc, componentIndex, typeDependentOperands,
                          packIndexType, packType);
}

OpenPackElementInst::OpenPackElementInst(
    SILDebugLocation debugLoc, SILValue packIndexOperand,
    ArrayRef<SILValue> typeDependentOperands,
    SILType type, GenericEnvironment *env)
    : UnaryInstructionWithTypeDependentOperandsBase(debugLoc, packIndexOperand,
                                                    typeDependentOperands, type),
      Env(env) {
}

OpenPackElementInst *OpenPackElementInst::create(
    SILFunction &F, SILDebugLocation debugLoc, SILValue indexOperand,
    GenericEnvironment *env) {
  // We can't assert that this is a pack-indexing instruction here
  // because of forward declarations while parsing/deserializing, but
  // we can at least assert the type.
  assert(indexOperand->getType().is<BuiltinPackIndexType>());

  SmallVector<SILValue, 8> typeDependentOperands;

  // open_pack_element references the pack substitutions and
  // the types used in the shape class.
  TypeDependentOperandCollector collector;
  env->forEachPackElementBinding([&](ElementArchetypeType *elementType,
                                     PackType *packSubstitution) {
    collector.collect(packSubstitution->getCanonicalType());
  });
  collector.addTo(typeDependentOperands, F);

  SILType type = SILType::getSILTokenType(F.getASTContext());

  auto size = totalSizeToAlloc<swift::Operand>(1 + typeDependentOperands.size());
  auto buffer = F.getModule().allocateInst(size, alignof(OpenPackElementInst));
  return ::new (buffer) OpenPackElementInst(debugLoc, indexOperand,
                                            typeDependentOperands, type, env);
}

CanPackType OpenPackElementInst::getOpenedShapeClass() const {
  PackType *pack = nullptr;
  auto env = getOpenedGenericEnvironment();
  env->forEachPackElementBinding([&](ElementArchetypeType *elementType,
                                     PackType *packSubstitution) {
    // Just pick one of these, they all have to have the same shape class.
    pack = packSubstitution;
  });
  assert(pack);
  return cast<PackType>(pack->getCanonicalType());
}

PackElementGetInst *PackElementGetInst::create(SILFunction &F,
                                               SILDebugLocation debugLoc,
                                               SILValue indexOperand,
                                               SILValue packOperand,
                                               SILType elementType) {
  assert(indexOperand->getType().is<BuiltinPackIndexType>());
  assert(packOperand->getType().is<SILPackType>());

  SmallVector<SILValue, 8> allOperands;
  allOperands.push_back(indexOperand);
  allOperands.push_back(packOperand);
  collectTypeDependentOperands(allOperands, F, elementType);

  auto size = totalSizeToAlloc<swift::Operand>(allOperands.size());
  auto buffer = F.getModule().allocateInst(size, alignof(PackElementGetInst));
  return ::new (buffer) PackElementGetInst(debugLoc, allOperands, elementType);
}

TuplePackElementAddrInst *
TuplePackElementAddrInst::create(SILFunction &F,
                                 SILDebugLocation debugLoc,
                                 SILValue indexOperand,
                                 SILValue tupleOperand,
                                 SILType elementType) {
  assert(indexOperand->getType().is<BuiltinPackIndexType>());
  assert(tupleOperand->getType().isAddress() &&
         tupleOperand->getType().is<TupleType>());

  SmallVector<SILValue, 8> allOperands;
  allOperands.push_back(indexOperand);
  allOperands.push_back(tupleOperand);
  collectTypeDependentOperands(allOperands, F, elementType);

  auto size = totalSizeToAlloc<swift::Operand>(allOperands.size());
  auto buffer =
    F.getModule().allocateInst(size, alignof(TuplePackElementAddrInst));
  return ::new (buffer) TuplePackElementAddrInst(debugLoc, allOperands,
                                                 elementType);
}

BeginCOWMutationInst::BeginCOWMutationInst(SILDebugLocation loc,
                               SILValue operand,
                               ArrayRef<SILType> resultTypes,
                               ArrayRef<ValueOwnershipKind> resultOwnerships,
                               bool isNative)
    : UnaryInstructionBase(loc, operand),
      MultipleValueInstructionTrailingObjects(this, resultTypes,
                                              resultOwnerships) {
  assert(resultTypes.size() == 2 && resultOwnerships.size() == 2);
  assert(operand->getType() == resultTypes[1]);
  setNative(isNative);
}

BeginCOWMutationInst *
BeginCOWMutationInst::create(SILDebugLocation loc, SILValue operand,
                             SILType boolTy, SILFunction &F, bool isNative) {

  SILType resultTypes[2] = { boolTy, operand->getType() };
  ValueOwnershipKind ownerships[2] = {OwnershipKind::None,
                                      OwnershipKind::Owned};

  void *buffer =
    allocateTrailingInst<BeginCOWMutationInst, MultipleValueInstruction*,
                         MultipleValueInstructionResult>(
      F, 1, 2);
  return ::new(buffer) BeginCOWMutationInst(loc, operand,
                                  ArrayRef<SILType>(resultTypes, 2),
                                  ArrayRef<ValueOwnershipKind>(ownerships, 2),
                                  isNative);
}

UncheckedRefCastInst *
UncheckedRefCastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                             SILType Ty, SILFunction &F,
                             ValueOwnershipKind forwardingOwnershipKind) {
  assert(Operand->getType().getCategory() == SILValueCategory::Object);
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedRefCastInst));
  return ::new (Buffer) UncheckedRefCastInst(
      DebugLoc, Operand, TypeDependentOperands, Ty, forwardingOwnershipKind);
}

UncheckedValueCastInst *
UncheckedValueCastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                               SILType Ty, SILFunction &F,
                               ValueOwnershipKind forwardingOwnershipKind) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedValueCastInst));
  return ::new (Buffer) UncheckedValueCastInst(
      DebugLoc, Operand, TypeDependentOperands, Ty, forwardingOwnershipKind);
}

UncheckedAddrCastInst *
UncheckedAddrCastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                              SILType Ty, SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedAddrCastInst));
  return ::new (Buffer) UncheckedAddrCastInst(DebugLoc, Operand,
                                              TypeDependentOperands, Ty);
}

UncheckedTrivialBitCastInst *
UncheckedTrivialBitCastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                              SILType Ty, SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedTrivialBitCastInst));
  return ::new (Buffer) UncheckedTrivialBitCastInst(DebugLoc, Operand,
                                                    TypeDependentOperands,
                                                    Ty);
}

UncheckedBitwiseCastInst *
UncheckedBitwiseCastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                                 SILType Ty, SILFunction &F) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UncheckedBitwiseCastInst));
  return ::new (Buffer) UncheckedBitwiseCastInst(DebugLoc, Operand,
                                                 TypeDependentOperands, Ty);
}

UnconditionalCheckedCastInst *UnconditionalCheckedCastInst::create(
    SILDebugLocation DebugLoc, SILValue Operand, SILType DestLoweredTy,
    CanType DestFormalTy, SILFunction &F,
    ValueOwnershipKind forwardingOwnershipKind) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, DestFormalTy);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UnconditionalCheckedCastInst));
  return ::new (Buffer) UnconditionalCheckedCastInst(
      DebugLoc, Operand, TypeDependentOperands, DestLoweredTy, DestFormalTy,
      forwardingOwnershipKind);
}

CheckedCastBranchInst *CheckedCastBranchInst::create(
    SILDebugLocation DebugLoc, bool IsExact, SILValue Operand,
    SILType DestLoweredTy, CanType DestFormalTy, SILBasicBlock *SuccessBB,
    SILBasicBlock *FailureBB, SILFunction &F,
    ProfileCounter Target1Count, ProfileCounter Target2Count,
    ValueOwnershipKind forwardingOwnershipKind) {
  SILModule &module = F.getModule();
  bool preservesOwnership = doesCastPreserveOwnershipForTypes(
    module, Operand->getType().getASTType(), DestFormalTy);
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, DestFormalTy);
  unsigned size =
      totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = module.allocateInst(size, alignof(CheckedCastBranchInst));
  return ::new (Buffer) CheckedCastBranchInst(
      DebugLoc, IsExact, Operand, TypeDependentOperands, DestLoweredTy,
      DestFormalTy, SuccessBB, FailureBB, Target1Count, Target2Count,
      forwardingOwnershipKind, preservesOwnership);
}

MetatypeInst *MetatypeInst::create(SILDebugLocation Loc, SILType Ty,
                                   SILFunction *F) {
  SILModule &Mod = F->getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, *F,
                               Ty.castTo<MetatypeType>().getInstanceType());
  auto Size = totalSizeToAlloc<swift::Operand>(TypeDependentOperands.size());
  auto Buffer = Mod.allocateInst(Size, alignof(MetatypeInst));
  return ::new (Buffer) MetatypeInst(Loc, Ty, TypeDependentOperands);
}

UpcastInst *UpcastInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                               SILType Ty, SILFunction &F,
                               ValueOwnershipKind forwardingOwnershipKind) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
    totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(UpcastInst));
  return ::new (Buffer) UpcastInst(DebugLoc, Operand, TypeDependentOperands, Ty,
                                   forwardingOwnershipKind);
}

ThinToThickFunctionInst *
ThinToThickFunctionInst::create(SILDebugLocation DebugLoc, SILValue Operand,
                                SILType Ty, SILModule &Mod, SILFunction *F,
                                ValueOwnershipKind forwardingOwnershipKind) {
  SmallVector<SILValue, 8> TypeDependentOperands;
  if (F) {
    assert(&F->getModule() == &Mod);
    collectTypeDependentOperands(TypeDependentOperands, *F, Ty.getASTType());
  }
  unsigned size =
    totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(ThinToThickFunctionInst));
  return ::new (Buffer) ThinToThickFunctionInst(
      DebugLoc, Operand, TypeDependentOperands, Ty, forwardingOwnershipKind);
}

ConvertFunctionInst *ConvertFunctionInst::create(
    SILDebugLocation DebugLoc, SILValue Operand, SILType Ty, SILModule &Mod,
    SILFunction *F,
    bool WithoutActuallyEscaping, ValueOwnershipKind forwardingOwnershipKind) {
  SmallVector<SILValue, 8> TypeDependentOperands;
  if (F) {
    assert(&F->getModule() == &Mod);
    collectTypeDependentOperands(TypeDependentOperands, *F, Ty.getASTType());
  }
  unsigned size =
    totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(ConvertFunctionInst));
  auto *CFI = ::new (Buffer)
      ConvertFunctionInst(DebugLoc, Operand, TypeDependentOperands, Ty,
                          WithoutActuallyEscaping, forwardingOwnershipKind);
  // If we do not have lowered SIL, make sure that are not performing
  // ABI-incompatible conversions.
  //
  // *NOTE* We purposely do not use an early return here to ensure that in
  // builds without assertions this whole if statement is optimized out.
  if (Mod.getStage() != SILStage::Lowered) {
    // Make sure we are not performing ABI-incompatible conversions.
    CanSILFunctionType opTI =
        CFI->getOperand()->getType().castTo<SILFunctionType>();
    (void)opTI;
    CanSILFunctionType resTI = CFI->getType().castTo<SILFunctionType>();
    (void)resTI;
    assert((!F || opTI->isABICompatibleWith(resTI, *F).isCompatible()) &&
           "Can not convert in between ABI incompatible function types");
  }
  return CFI;
}

bool ConvertFunctionInst::onlyConvertsSubstitutions() const {
  auto fromType = getOperand()->getType().castTo<SILFunctionType>();
  auto toType = getType().castTo<SILFunctionType>();
  auto &M = getModule();
  
  return fromType->getUnsubstitutedType(M) == toType->getUnsubstitutedType(M);
}

ConvertEscapeToNoEscapeInst *ConvertEscapeToNoEscapeInst::create(
    SILDebugLocation DebugLoc, SILValue Operand, SILType Ty, SILFunction &F,
    bool isLifetimeGuaranteed) {
  SILModule &Mod = F.getModule();
  SmallVector<SILValue, 8> TypeDependentOperands;
  collectTypeDependentOperands(TypeDependentOperands, F, Ty.getASTType());
  unsigned size =
    totalSizeToAlloc<swift::Operand>(1 + TypeDependentOperands.size());
  void *Buffer = Mod.allocateInst(size, alignof(ConvertEscapeToNoEscapeInst));
  auto *CFI = ::new (Buffer) ConvertEscapeToNoEscapeInst(
      DebugLoc, Operand, TypeDependentOperands, Ty, isLifetimeGuaranteed);
  // If we do not have lowered SIL, make sure that are not performing
  // ABI-incompatible conversions.
  //
  // *NOTE* We purposely do not use an early return here to ensure that in
  // builds without assertions this whole if statement is optimized out.
  if (F.getModule().getStage() != SILStage::Lowered) {
    // Make sure we are not performing ABI-incompatible conversions.
    CanSILFunctionType opTI =
        CFI->getOperand()->getType().castTo<SILFunctionType>();
    (void)opTI;
    CanSILFunctionType resTI = CFI->getType().castTo<SILFunctionType>();
    (void)resTI;
    assert(opTI->isABICompatibleWith(resTI, F)
               .isCompatibleUpToNoEscapeConversion() &&
           "Can not convert in between ABI incompatible function types");
  }
  return CFI;
}

bool KeyPathPatternComponent::isComputedSettablePropertyMutating() const {
  switch (getKind()) {
  case Kind::StoredProperty:
  case Kind::GettableProperty:
  case Kind::OptionalChain:
  case Kind::OptionalWrap:
  case Kind::OptionalForce:
  case Kind::TupleElement:
    llvm_unreachable("not a settable computed property");
  case Kind::SettableProperty: {
    auto setter = getComputedPropertySetter();
    return setter->getLoweredFunctionType()->getParameters()[1].getConvention()
       == ParameterConvention::Indirect_Inout;
  }
  }
  llvm_unreachable("unhandled kind");
}

static void
forEachRefcountableReference(const KeyPathPatternComponent &component,
                         llvm::function_ref<void (SILFunction*)> forFunction) {
  switch (component.getKind()) {
  case KeyPathPatternComponent::Kind::StoredProperty:
  case KeyPathPatternComponent::Kind::OptionalChain:
  case KeyPathPatternComponent::Kind::OptionalWrap:
  case KeyPathPatternComponent::Kind::OptionalForce:
  case KeyPathPatternComponent::Kind::TupleElement:
    return;
  case KeyPathPatternComponent::Kind::SettableProperty:
    forFunction(component.getComputedPropertySetter());
    LLVM_FALLTHROUGH;
  case KeyPathPatternComponent::Kind::GettableProperty:
    forFunction(component.getComputedPropertyGetter());
    
    switch (component.getComputedPropertyId().getKind()) {
    case KeyPathPatternComponent::ComputedPropertyId::DeclRef:
      // Mark the vtable entry as used somehow?
      break;
    case KeyPathPatternComponent::ComputedPropertyId::Function:
      forFunction(component.getComputedPropertyId().getFunction());
      break;
    case KeyPathPatternComponent::ComputedPropertyId::Property:
      break;
    }
    
    if (auto equals = component.getSubscriptIndexEquals())
      forFunction(equals);
    if (auto hash = component.getSubscriptIndexHash())
      forFunction(hash);
    return;
  }
}

void KeyPathPatternComponent::incrementRefCounts() const {
  forEachRefcountableReference(*this,
    [&](SILFunction *f) { f->incrementRefCount(); });
}
void KeyPathPatternComponent::decrementRefCounts() const {
  forEachRefcountableReference(*this,
                               [&](SILFunction *f) { f->decrementRefCount(); });
}

KeyPathPattern *
KeyPathPattern::get(SILModule &M, CanGenericSignature signature,
                    CanType rootType, CanType valueType,
                    ArrayRef<KeyPathPatternComponent> components,
                    StringRef objcString) {
  llvm::FoldingSetNodeID id;
  Profile(id, signature, rootType, valueType, components, objcString);
  
  void *insertPos;
  auto existing = M.KeyPathPatterns.FindNodeOrInsertPos(id, insertPos);
  if (existing)
    return existing;
  
  // Determine the number of operands.
  int maxOperandNo = -1;
  for (auto component : components) {
    switch (component.getKind()) {
    case KeyPathPatternComponent::Kind::StoredProperty:
    case KeyPathPatternComponent::Kind::OptionalChain:
    case KeyPathPatternComponent::Kind::OptionalWrap:
    case KeyPathPatternComponent::Kind::OptionalForce:
    case KeyPathPatternComponent::Kind::TupleElement:
      break;
    
    case KeyPathPatternComponent::Kind::GettableProperty:
    case KeyPathPatternComponent::Kind::SettableProperty:
      for (auto &index : component.getSubscriptIndices()) {
        maxOperandNo = std::max(maxOperandNo, (int)index.Operand);
      }
    }
  }
  
  auto newPattern = KeyPathPattern::create(M, signature, rootType, valueType,
                                           components, objcString,
                                           maxOperandNo + 1);
  M.KeyPathPatterns.InsertNode(newPattern, insertPos);
  return newPattern;
}

KeyPathPattern *
KeyPathPattern::create(SILModule &M, CanGenericSignature signature,
                       CanType rootType, CanType valueType,
                       ArrayRef<KeyPathPatternComponent> components,
                       StringRef objcString,
                       unsigned numOperands) {
  auto totalSize = totalSizeToAlloc<KeyPathPatternComponent>(components.size());
  void *mem = M.allocate(totalSize, alignof(KeyPathPatternComponent));
  return ::new (mem) KeyPathPattern(signature, rootType, valueType,
                                    components, objcString, numOperands);
}

KeyPathPattern::KeyPathPattern(CanGenericSignature signature,
                               CanType rootType, CanType valueType,
                               ArrayRef<KeyPathPatternComponent> components,
                               StringRef objcString,
                               unsigned numOperands)
  : NumOperands(numOperands), NumComponents(components.size()),
    Signature(signature), RootType(rootType), ValueType(valueType),
    ObjCString(objcString)
{
  auto *componentsBuf = getTrailingObjects<KeyPathPatternComponent>();
  std::uninitialized_copy(components.begin(), components.end(),
                          componentsBuf);
}

ArrayRef<KeyPathPatternComponent>
KeyPathPattern::getComponents() const {
  return {getTrailingObjects<KeyPathPatternComponent>(), NumComponents};
}

void KeyPathPattern::Profile(llvm::FoldingSetNodeID &ID,
                             CanGenericSignature signature,
                             CanType rootType,
                             CanType valueType,
                             ArrayRef<KeyPathPatternComponent> components,
                             StringRef objcString) {
  ID.AddPointer(signature.getPointer());
  ID.AddPointer(rootType.getPointer());
  ID.AddPointer(valueType.getPointer());
  ID.AddString(objcString);
  
  auto profileIndices = [&](ArrayRef<KeyPathPatternComponent::Index> indices) {
    for (auto &index : indices) {
      ID.AddInteger(index.Operand);
      ID.AddPointer(index.FormalType.getPointer());
      ID.AddPointer(index.LoweredType.getOpaqueValue());
      ID.AddPointer(index.Hashable.getOpaqueValue());
    }
  };
  
  for (auto &component : components) {
    ID.AddInteger((unsigned)component.getKind());
    switch (component.getKind()) {
    case KeyPathPatternComponent::Kind::OptionalForce:
    case KeyPathPatternComponent::Kind::OptionalWrap:
    case KeyPathPatternComponent::Kind::OptionalChain:
      break;
      
    case KeyPathPatternComponent::Kind::StoredProperty:
      ID.AddPointer(component.getStoredPropertyDecl());
      break;
    
    case KeyPathPatternComponent::Kind::TupleElement:
      ID.AddInteger(component.getTupleIndex());
      break;
    
    case KeyPathPatternComponent::Kind::SettableProperty:
      ID.AddPointer(component.getComputedPropertySetter());
      LLVM_FALLTHROUGH;
    case KeyPathPatternComponent::Kind::GettableProperty:
      ID.AddPointer(component.getComputedPropertyGetter());
      auto id = component.getComputedPropertyId();
      ID.AddInteger(id.getKind());
      switch (id.getKind()) {
      case KeyPathPatternComponent::ComputedPropertyId::DeclRef: {
        auto declRef = id.getDeclRef();
        ID.AddPointer(declRef.loc.getOpaqueValue());
        ID.AddInteger((unsigned)declRef.kind);
        ID.AddBoolean(declRef.isForeign);
        ID.AddBoolean(declRef.defaultArgIndex);
        break;
      }
      case KeyPathPatternComponent::ComputedPropertyId::Function: {
        ID.AddPointer(id.getFunction());
        break;
      }
      case KeyPathPatternComponent::ComputedPropertyId::Property: {
        ID.AddPointer(id.getProperty());
        break;
      }
      }
      profileIndices(component.getSubscriptIndices());
      ID.AddPointer(component.getExternalDecl());
      component.getExternalSubstitutions().profile(ID);
      break;
    }
  }
}

KeyPathInst *
KeyPathInst::create(SILDebugLocation Loc,
                    KeyPathPattern *Pattern,
                    SubstitutionMap Subs,
                    ArrayRef<SILValue> Args,
                    SILType Ty,
                    SILFunction &F) {
  assert(Args.size() == Pattern->getNumOperands()
         && "number of key path args doesn't match pattern");

  SmallVector<SILValue, 8> allOperands(Args.begin(), Args.end());
  collectTypeDependentOperands(allOperands, F, Ty);

  auto totalSize = totalSizeToAlloc<Operand>(allOperands.size());
  void *mem = F.getModule().allocateInst(totalSize, alignof(KeyPathInst));
  return ::new (mem) KeyPathInst(Loc, Pattern, Subs, allOperands, Args.size(), Ty);
}

KeyPathInst::KeyPathInst(SILDebugLocation Loc,
                         KeyPathPattern *Pattern,
                         SubstitutionMap Subs,
                         ArrayRef<SILValue> allOperands,
                         unsigned numPatternOperands,
                         SILType Ty)
  : InstructionBase(Loc, Ty),
    Pattern(Pattern),
    numPatternOperands(numPatternOperands),
    numTypeDependentOperands(allOperands.size() - numPatternOperands),
    Substitutions(Subs)
{
  assert(allOperands.size() >= numPatternOperands);
  auto *operandsBuf = getTrailingObjects<Operand>();
  for (unsigned i = 0; i < allOperands.size(); ++i) {
    ::new ((void*)&operandsBuf[i]) Operand(this, allOperands[i]);
  }
  
  // Increment the use of any functions referenced from the keypath pattern.
  for (auto component : Pattern->getComponents()) {
    component.incrementRefCounts();
  }
}

MutableArrayRef<Operand>
KeyPathInst::getAllOperands() {
  return {getTrailingObjects<Operand>(), numPatternOperands + numTypeDependentOperands};
}

KeyPathInst::~KeyPathInst() {
  if (!Pattern)
    return;

  // Decrement the use of any functions referenced from the keypath pattern.
  for (auto component : Pattern->getComponents()) {
    component.decrementRefCounts();
  }
  // Destroy operands.
  for (auto &operand : getAllOperands())
    operand.~Operand();
}

KeyPathPattern *KeyPathInst::getPattern() const {
  assert(Pattern && "pattern was reset!");
  return Pattern;
}

void KeyPathInst::dropReferencedPattern() {
  for (auto component : Pattern->getComponents()) {
    component.decrementRefCounts();
  }
  Pattern = nullptr;
}

void KeyPathPatternComponent::
visitReferencedFunctionsAndMethods(
      std::function<void (SILFunction *)> functionCallBack,
      std::function<void (SILDeclRef)> methodCallBack) const {
  switch (getKind()) {
  case KeyPathPatternComponent::Kind::SettableProperty:
    functionCallBack(getComputedPropertySetter());
    LLVM_FALLTHROUGH;
  case KeyPathPatternComponent::Kind::GettableProperty: {
    functionCallBack(getComputedPropertyGetter());
    auto id = getComputedPropertyId();
    switch (id.getKind()) {
    case KeyPathPatternComponent::ComputedPropertyId::DeclRef: {
      methodCallBack(id.getDeclRef());
      break;
    }
    case KeyPathPatternComponent::ComputedPropertyId::Function:
      functionCallBack(id.getFunction());
      break;
    case KeyPathPatternComponent::ComputedPropertyId::Property:
      break;
    }

    if (auto equals = getSubscriptIndexEquals())
      functionCallBack(equals);
    if (auto hash = getSubscriptIndexHash())
      functionCallBack(hash);

    break;
  }
  case KeyPathPatternComponent::Kind::StoredProperty:
  case KeyPathPatternComponent::Kind::OptionalChain:
  case KeyPathPatternComponent::Kind::OptionalForce:
  case KeyPathPatternComponent::Kind::OptionalWrap:
  case KeyPathPatternComponent::Kind::TupleElement:
    break;
  }
}


GenericSpecializationInformation::GenericSpecializationInformation(
    SILFunction *Caller, SILFunction *Parent, SubstitutionMap Subs)
    : Caller(Caller), Parent(Parent), Subs(Subs) {}

const GenericSpecializationInformation *
GenericSpecializationInformation::create(SILFunction *Caller,
                                         SILFunction *Parent,
                                         SubstitutionMap Subs) {
  auto &M = Parent->getModule();
  void *Buf = M.allocate(sizeof(GenericSpecializationInformation),
                           alignof(GenericSpecializationInformation));
  return new (Buf) GenericSpecializationInformation(Caller, Parent, Subs);
}

const GenericSpecializationInformation *
GenericSpecializationInformation::create(SILInstruction *Inst, SILBuilder &B) {
  auto Apply = ApplySite::isa(Inst);
  // Preserve history only for apply instructions for now.
  // NOTE: We may want to preserve history for all instructions in the future,
  // because it may allow us to track their origins.
  assert(Apply);
  auto *F = Inst->getFunction();
  auto &BuilderF = B.getFunction();

  // If cloning inside the same function, don't change the specialization info.
  if (F == &BuilderF) {
    return Apply.getSpecializationInfo();
  }

  // The following lines are used in case of inlining.

  // If a call-site has a history already, simply preserve it.
  if (Apply.getSpecializationInfo())
    return Apply.getSpecializationInfo();

  // If a call-site has no history, use the history of a containing function.
  if (F->isSpecialization())
    return F->getSpecializationInfo();

  return nullptr;
}

static void computeAggregateFirstLevelSubtypeInfo(
    const SILFunction &F, SILValue Operand,
    llvm::SmallVectorImpl<SILType> &Types,
    llvm::SmallVectorImpl<ValueOwnershipKind> &OwnershipKinds) {
  auto &M = F.getModule();
  SILType OpType = Operand->getType();

  // TODO: Create an iterator for accessing first level projections to eliminate
  // this SmallVector.
  llvm::SmallVector<Projection, 8> Projections;
  Projection::getFirstLevelProjections(OpType, M, F.getTypeExpansionContext(),
                                       Projections);

  auto OpOwnershipKind = Operand->getOwnershipKind();
  for (auto &P : Projections) {
    SILType ProjType = P.getType(OpType, M, F.getTypeExpansionContext());
    Types.emplace_back(ProjType);
    OwnershipKinds.emplace_back(
        OpOwnershipKind.getProjectedOwnershipKind(F, ProjType));
  }
}

DestructureStructInst *
DestructureStructInst::create(const SILFunction &F, SILDebugLocation Loc,
                              SILValue Operand,
                              ValueOwnershipKind forwardingOwnershipKind) {
  auto &M = F.getModule();

  assert(Operand->getType().getStructOrBoundGenericStruct() &&
         "Expected a struct typed operand?!");

  llvm::SmallVector<SILType, 8> Types;
  llvm::SmallVector<ValueOwnershipKind, 8> OwnershipKinds;
  computeAggregateFirstLevelSubtypeInfo(F, Operand, Types, OwnershipKinds);
  assert(Types.size() == OwnershipKinds.size() &&
         "Expected same number of Types and OwnerKinds");

  unsigned NumElts = Types.size();
  unsigned Size =
    totalSizeToAlloc<MultipleValueInstruction *, MultipleValueInstructionResult>(
          1, NumElts);

  void *Buffer = M.allocateInst(Size, alignof(DestructureStructInst));

  return ::new (Buffer) DestructureStructInst(
      M, Loc, Operand, Types, OwnershipKinds, forwardingOwnershipKind);
}

DestructureTupleInst *
DestructureTupleInst::create(const SILFunction &F, SILDebugLocation Loc,
                             SILValue Operand,
                             ValueOwnershipKind forwardingOwnershipKind) {
  auto &M = F.getModule();

  assert(Operand->getType().is<TupleType>() &&
         "Expected a tuple typed operand?!");

  llvm::SmallVector<SILType, 8> Types;
  llvm::SmallVector<ValueOwnershipKind, 8> OwnershipKinds;
  computeAggregateFirstLevelSubtypeInfo(F, Operand, Types, OwnershipKinds);
  assert(Types.size() == OwnershipKinds.size() &&
         "Expected same number of Types and OwnerKinds");

  // We add 1 since we store an offset to our
  unsigned NumElts = Types.size();
  unsigned Size =
    totalSizeToAlloc<MultipleValueInstruction *, MultipleValueInstructionResult>(
          1, NumElts);

  void *Buffer = M.allocateInst(Size, alignof(DestructureTupleInst));

  return ::new (Buffer) DestructureTupleInst(
      M, Loc, Operand, Types, OwnershipKinds, forwardingOwnershipKind);
}

SILType GetAsyncContinuationInstBase::getLoweredResumeType() const {
  // The lowered resume type is the maximally-abstracted lowering of the
  // formal resume type.
  auto formalType = getFormalResumeType();
  auto &M = getFunction()->getModule();
  auto c = getFunction()->getTypeExpansionContext();
  return M.Types.getLoweredType(AbstractionPattern::getOpaque(), formalType, c);
}

ReturnInst::ReturnInst(SILFunction &func, SILDebugLocation debugLoc,
                       SILValue returnValue)
    : UnaryInstructionBase(debugLoc, returnValue),
      ownershipKind(OwnershipKind::None) {
  // If we have a trivial value, leave our ownership kind as none.
  if (returnValue->getType().isTrivial(func))
    return;

  SILFunctionConventions fnConv = func.getConventions();

  // If we do not have any direct SIL results, we should accept a tuple
  // argument, meaning that we should have a none ownership kind.
  auto results = fnConv.getDirectSILResults();
  if (results.empty())
    return;

  auto ownershipKindRange =
      makeTransformRange(results, [&](const SILResultInfo &info) {
        return info.getOwnershipKind(func, func.getLoweredFunctionType());
      });

  // Then merge all of our ownership kinds. Assert if we fail to merge.
  ownershipKind = ValueOwnershipKind::merge(ownershipKindRange);
  assert(ownershipKind &&
         "Conflicting ownership kinds when creating term inst from function "
         "result info?!");
}

bool OwnershipForwardingMixin::hasSameRepresentation(SILInstruction *inst) {
  switch (inst->getKind()) {
  // Explicitly list instructions which definitely involve a representation
  // change.
  case SILInstructionKind::SwitchEnumInst:
  default:
    // Conservatively assume that a conversion changes representation.
    // Operations can be added as needed to participate in SIL opaque values.
    assert(OwnershipForwardingMixin::isa(inst));
    return false;

  case SILInstructionKind::ConvertFunctionInst:
  case SILInstructionKind::DestructureTupleInst:
  case SILInstructionKind::DestructureStructInst:
  case SILInstructionKind::InitExistentialRefInst:
  case SILInstructionKind::ObjectInst:
  case SILInstructionKind::OpenExistentialBoxValueInst:
  case SILInstructionKind::OpenExistentialRefInst:
  case SILInstructionKind::OpenExistentialValueInst:
  case SILInstructionKind::MarkMustCheckInst:
  case SILInstructionKind::MarkUninitializedInst:
  case SILInstructionKind::SelectEnumInst:
  case SILInstructionKind::StructExtractInst:
  case SILInstructionKind::TupleExtractInst:
    return true;
  }
}

bool OwnershipForwardingMixin::isAddressOnly(SILInstruction *inst) {
  if (auto *aggregate =
      dyn_cast<AllArgOwnershipForwardingSingleValueInst>(inst)) {
    // If any of the operands are address-only, then the aggregate must be.
    return aggregate->getType().isAddressOnly(*inst->getFunction());
  }
  // All other forwarding instructions must forward their first operand.
  assert(OwnershipForwardingMixin::isa(inst));
  return inst->getOperand(0)->getType().isAddressOnly(*inst->getFunction());
}

// This may be called in an invalid SIL state. SILCombine creates new
// terminators in non-terminator position and defers deleting the original
// terminator until after all modification.
SILPhiArgument *OwnershipForwardingTermInst::createResult(SILBasicBlock *succ,
                                                          SILType resultTy) {
  // The forwarding instruction declares a forwarding ownership kind that
  // determines the ownership of its results.
  auto resultOwnership = getForwardingOwnershipKind();

  // Trivial results have no ownership. Although it is valid for a trivially
  // typed value to have ownership, it is never necessary and less efficient.
  if (resultTy.isTrivial(*getFunction())) {
    resultOwnership = OwnershipKind::None;

  } else if (resultOwnership == OwnershipKind::None) {
    // switch_enum strangely allows results to acquire ownership out of thin
    // air whenever the operand has no ownership and result is nontrivial:
    //     %e = enum $Optional<AnyObject>, #Optional.none!enumelt
    //     switch_enum %e : $Optional<AnyObject>,
    //                 case #Optional.some!enumelt: bb2...
    //   bb2(%arg : @guaranteed T):
    //
    // We can either use None or Guaranteed. None would correctly propagate
    // ownership and would maintain the invariant that guaranteed values are
    // always within a borrow scope. However it would result in a nontrivial
    // type without ownership. The lifetime verifier does not like that.
    resultOwnership = OwnershipKind::Guaranteed;
  }
  return succ->createPhiArgument(resultTy, resultOwnership);
}

SILPhiArgument *SwitchEnumInst::createDefaultResult() {
  auto *f = getFunction();
  if (!f->hasOwnership())
    return nullptr;

  if (!hasDefault())
    return nullptr;

  assert(getDefaultBB()->getNumArguments() == 0 && "precondition");

  auto enumTy = getOperand()->getType();
  NullablePtr<EnumElementDecl> uniqueCase = getUniqueCaseForDefault();

  // Without a unique default case, the OSSA result simply forwards the
  // switch_enum operand.
  if (!uniqueCase)
    return createResult(getDefaultBB(), enumTy);

  // With a unique default case, the result is materialized exactly the same way
  // as a matched result. It has a value iff the unique case has a payload.
  if (!uniqueCase.get()->hasAssociatedValues())
    return nullptr;

  auto resultTy = enumTy.getEnumElementType(uniqueCase.get(), f->getModule(),
                                            f->getTypeExpansionContext());
  return createResult(getDefaultBB(), resultTy);
}

SILPhiArgument *SwitchEnumInst::createOptionalSomeResult() {
  auto someDecl = getModule().getASTContext().getOptionalSomeDecl();
  auto someBB = getCaseDestination(someDecl);
  return createResult(someBB, getOperand()->getType().unwrapOptionalType());
}

void HasSymbolInst::getReferencedFunctions(
    llvm::SmallVector<SILFunction *, 4> &fns) const {
  auto &M = getModule();
  enumerateFunctionsForHasSymbol(M, getDecl(), [&M, &fns](SILDeclRef declRef) {
    SILFunction *fn = M.lookUpFunction(declRef);
    assert(fn);
    fns.push_back(fn);
  });
}
