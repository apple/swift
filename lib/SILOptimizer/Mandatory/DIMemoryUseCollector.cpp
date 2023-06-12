//===--- DIMemoryUseCollector.cpp -----------------------------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2018 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "definite-init"

#include "DIMemoryUseCollector.h"
#include "swift/AST/Expr.h"
#include "swift/SIL/ApplySite.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILArgument.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILInstruction.h"
#include "swift/SILOptimizer/Utils/DistributedActor.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/SaveAndRestore.h"

using namespace swift;
using namespace ownership;

//===----------------------------------------------------------------------===//
//                                  Utility
//===----------------------------------------------------------------------===//

static bool isVariableOrResult(MarkUninitializedInst *MUI) {
  return MUI->isVar() || MUI->isOut();
}

static void gatherDestroysOfContainer(const DIMemoryObjectInfo &memoryInfo,
                                      DIElementUseInfo &useInfo) {
  auto *uninitMemory = memoryInfo.getUninitializedValue();

  // The uninitMemory must be used on an alloc_box, alloc_stack, or global_addr.
  // If we have an alloc_stack or a global_addr, there is nothing further to do.
  if (isa<AllocStackInst>(uninitMemory->getOperand(0)) ||
      isa<GlobalAddrInst>(uninitMemory->getOperand(0)) ||
      isa<SILArgument>(uninitMemory->getOperand(0)) ||
      // FIXME: We only support pointer to address here to not break LLDB. It is
      // important that long term we get rid of this since this is a situation
      // where LLDB is breaking SILGen/DI invariants by not creating a new
      // independent stack location for the pointer to address.
      isa<PointerToAddressInst>(uninitMemory->getOperand(0))) {
    return;
  }

  // Otherwise, we assume that we have an alloc_box. Treat destroys of the
  // alloc_box as load+destroys of the value stored in the box.
  //
  // TODO: This should really be tracked separately from other destroys so that
  // we distinguish the lifetime of the container from the value itself.
  assert(isa<ProjectBoxInst>(uninitMemory));
  auto value = uninitMemory->getOperand(0);
  if (auto *bbi = dyn_cast<BeginBorrowInst>(value)) {
    value = bbi->getOperand();
  }
  auto *mui = cast<MarkUninitializedInst>(value);
  for (auto *user : mui->getUsersOfType<DestroyValueInst>()) {
    useInfo.trackDestroy(user);
  }
}

//===----------------------------------------------------------------------===//
//                     DIMemoryObjectInfo Implementation
//===----------------------------------------------------------------------===//

static unsigned getElementCountRec(TypeExpansionContext context,
                                   SILModule &Module, SILType T,
                                   bool IsSelfOfNonDelegatingInitializer) {
  // If this is a tuple, it is always recursively flattened.
  if (CanTupleType TT = T.getAs<TupleType>()) {
    assert(!IsSelfOfNonDelegatingInitializer && "self never has tuple type");
    unsigned NumElements = 0;
    for (unsigned i = 0, e = TT->getNumElements(); i < e; ++i)
      NumElements +=
          getElementCountRec(context, Module, T.getTupleElementType(i), false);
    return NumElements;
  }

  // If this is the top level of a 'self' value, we flatten structs and classes.
  // Stored properties with tuple types are tracked with independent lifetimes
  // for each of the tuple members.
  if (IsSelfOfNonDelegatingInitializer) {
    // Protocols never have a stored properties.
    if (auto *NTD = T.getNominalOrBoundGenericNominal()) {
      unsigned NumElements = 0;
      for (auto *VD : NTD->getStoredProperties())
        NumElements += getElementCountRec(
            context, Module, T.getFieldType(VD, Module, context), false);
      return NumElements;
    }
  }

  // Otherwise, it is a single element.
  return 1;
}

static std::pair<SILType, bool>
computeMemorySILType(MarkUninitializedInst *MUI, SILValue Address) {
  // Compute the type of the memory object.
  SILType MemorySILType = Address->getType().getObjectType();

  // If this is a let variable we're initializing, remember this so we don't
  // allow reassignment.
  if (!isVariableOrResult(MUI))
    return {MemorySILType, false};

  auto *VDecl = MUI->getLoc().getAsASTNode<VarDecl>();
  if (!VDecl)
    return {MemorySILType, false};

  return {MemorySILType, VDecl->isLet()};
}

DIMemoryObjectInfo::DIMemoryObjectInfo(MarkUninitializedInst *MI)
    : MemoryInst(MI) {
  auto &Module = MI->getModule();

  SILValue Address = MemoryInst;
  if (auto BBI = MemoryInst->getSingleUserOfType<BeginBorrowInst>()) {
    if (auto PBI = BBI->getSingleUserOfType<ProjectBoxInst>()) {
      IsBox = true;
      Address = PBI;
    }
  }
  if (auto PBI = MemoryInst->getSingleUserOfType<ProjectBoxInst>()) {
    IsBox = true;
    Address = PBI;
  }

  std::tie(MemorySILType, IsLet) = computeMemorySILType(MI, Address);

  // Compute the number of elements to track in this memory object.
  // If this is a 'self' in a delegating initializer, we only track one bit:
  // whether self.init is called or not.
  if (isDelegatingInit()) {
    NumElements = 1;
    return;
  }

  // If this is a derived class init method for which stored properties are
  // separately initialized, track an element for the super.init call.
  if (isDerivedClassSelfOnly()) {
    NumElements = 1;
    return;
  }

  // Otherwise, we break down the initializer.
  NumElements =
      getElementCountRec(TypeExpansionContext(*MI->getFunction()), Module,
                         MemorySILType, isNonDelegatingInit());

  // If this is a derived class init method, track an extra element to determine
  // whether super.init has been called at each program point.
  NumElements += unsigned(isDerivedClassSelf());

  // Make sure we track /something/ in a cross-module struct initializer.
  if (NumElements == 0 && isCrossModuleStructInitSelf()) {
    NumElements = 1;
    HasDummyElement = true;
  }
}

SILInstruction *DIMemoryObjectInfo::getFunctionEntryPoint() const {
  return &*getFunction().begin()->begin();
}

static SingleValueInstruction *
getUninitializedValue(MarkUninitializedInst *MemoryInst) {
  SILValue inst = MemoryInst;
  if (auto *bbi = MemoryInst->getSingleUserOfType<BeginBorrowInst>()) {
    inst = bbi;
  }

  if (SingleValueInstruction *svi =
          inst->getSingleUserOfType<ProjectBoxInst>()) {
    return svi;
  }

  return MemoryInst;
}

SingleValueInstruction *DIMemoryObjectInfo::getUninitializedValue() const {
  return ::getUninitializedValue(MemoryInst);
}

/// Given a symbolic element number, return the type of the element.
static SILType getElementTypeRec(TypeExpansionContext context,
                                 SILModule &Module, SILType T, unsigned EltNo,
                                 bool IsSelfOfNonDelegatingInitializer) {
  // If this is a tuple type, walk into it.
  if (CanTupleType TT = T.getAs<TupleType>()) {
    assert(!IsSelfOfNonDelegatingInitializer && "self never has tuple type");
    for (unsigned i = 0, e = TT->getNumElements(); i < e; ++i) {
      auto FieldType = T.getTupleElementType(i);
      unsigned NumFieldElements =
          getElementCountRec(context, Module, FieldType, false);
      if (EltNo < NumFieldElements)
        return getElementTypeRec(context, Module, FieldType, EltNo, false);
      EltNo -= NumFieldElements;
    }
    // This can only happen if we look at a symbolic element number of an empty
    // tuple.
    llvm::report_fatal_error("invalid element number");
  }

  // If this is the top level of a 'self' value, we flatten structs and classes.
  // Stored properties with tuple types are tracked with independent lifetimes
  // for each of the tuple members.
  if (IsSelfOfNonDelegatingInitializer) {
    if (auto *NTD = T.getNominalOrBoundGenericNominal()) {
      bool HasStoredProperties = false;
      for (auto *VD : NTD->getStoredProperties()) {
        HasStoredProperties = true;
        auto FieldType = T.getFieldType(VD, Module, context);
        unsigned NumFieldElements =
            getElementCountRec(context, Module, FieldType, false);
        if (EltNo < NumFieldElements)
          return getElementTypeRec(context, Module, FieldType, EltNo, false);
        EltNo -= NumFieldElements;
      }

      // If we do not have any stored properties and were passed an EltNo of 0,
      // just return self.
      if (!HasStoredProperties && EltNo == 0) {
        return T;
      }
      llvm::report_fatal_error("invalid element number");
    }
  }

  // Otherwise, it is a leaf element.
  assert(EltNo == 0);
  return T;
}

/// getElementTypeRec - Return the swift type of the specified element.
SILType DIMemoryObjectInfo::getElementType(unsigned EltNo) const {
  auto &Module = MemoryInst->getModule();
  return getElementTypeRec(TypeExpansionContext(*MemoryInst->getFunction()),
                           Module, MemorySILType, EltNo, isNonDelegatingInit());
}

/// During tear-down of a distributed actor, we must invoke its
/// \p actorSystem.resignID method, passing in the \p id from the
/// instance. Thus, this function inspects the VarDecl about to be destroyed
/// and if it matches the \p id, the \p resignIdentity call is emitted.
///
/// NOTE (id-before-actorSystem): it is crucial that the \p id is
/// deinitialized before the \p actorSystem is deinitialized, because
/// resigning the identity requires a call into the \p actorSystem.
/// Since deinitialization consistently happens in-order, according to the
/// listing returned by \p NominalTypeDecl::getStoredProperties
/// it is important the VarDecl for the \p id is synthesized before
/// the \p actorSystem so that we get the right ordering in DI and deinits.
///
/// \param nomDecl a distributed actor decl
/// \param var a VarDecl that is a member of the \p nomDecl
static void tryToResignIdentity(SILLocation loc, SILBuilder &B,
                                NominalTypeDecl* nomDecl, VarDecl *var,
                                SILValue idRef, SILValue actorInst) {
  assert(nomDecl->isDistributedActor());

  if (var != nomDecl->getDistributedActorIDProperty())
    return;

  emitResignIdentityCall(B, loc, cast<ClassDecl>(nomDecl),
                         actorInst, idRef);
}

/// Given a tuple element number (in the flattened sense) return a pointer to a
/// leaf element of the specified number, so we can insert destroys for it.
SILValue DIMemoryObjectInfo::emitElementAddressForDestroy(
    unsigned EltNo, SILLocation Loc, SILBuilder &B,
    SmallVectorImpl<std::pair<SILValue, EndScopeKind>> &EndScopeList) const {
  SILValue Ptr = getUninitializedValue();
  bool IsSelf = isNonDelegatingInit();
  auto &Module = MemoryInst->getModule();

  auto PointeeType = MemorySILType;

  while (1) {
    // If we have a tuple, flatten it.
    if (CanTupleType TT = PointeeType.getAs<TupleType>()) {
      assert(!IsSelf && "self never has tuple type");

      // Figure out which field we're walking into.
      unsigned FieldNo = 0;
      for (unsigned i = 0, e = TT->getNumElements(); i < e; ++i) {
        auto EltTy = PointeeType.getTupleElementType(i);
        unsigned NumSubElt = getElementCountRec(
            TypeExpansionContext(B.getFunction()), Module, EltTy, false);
        if (EltNo < NumSubElt) {
          Ptr = B.createTupleElementAddr(Loc, Ptr, FieldNo);
          PointeeType = EltTy;
          break;
        }

        EltNo -= NumSubElt;
        ++FieldNo;
      }
      continue;
    }

    // If this is the top level of a 'self' value, we flatten structs and
    // classes.  Stored properties with tuple types are tracked with independent
    // lifetimes for each of the tuple members.
    if (IsSelf) {
      if (auto *NTD = PointeeType.getNominalOrBoundGenericNominal()) {
        const bool IsDistributedActor = NTD->isDistributedActor();
        bool HasStoredProperties = false;
        for (auto *VD : NTD->getStoredProperties()) {
          if (!HasStoredProperties) {
            HasStoredProperties = true;
            // If we have a class, we can use a borrow directly and avoid ref
            // count traffic.
            if (isa<ClassDecl>(NTD) && Ptr->getType().isAddress()) {
              SILValue Borrowed = Ptr = B.createLoadBorrow(Loc, Ptr);
              EndScopeList.emplace_back(Borrowed, EndScopeKind::Borrow);
            }
          }
          auto expansionContext = TypeExpansionContext(B.getFunction());
          auto FieldType =
              PointeeType.getFieldType(VD, Module, expansionContext);
          unsigned NumFieldElements =
              getElementCountRec(expansionContext, Module, FieldType, false);
          if (EltNo < NumFieldElements) {
            if (isa<StructDecl>(NTD)) {
              Ptr = B.createStructElementAddr(Loc, Ptr, VD);
            } else {
              assert(isa<ClassDecl>(NTD));
              SILValue Original, Borrowed;
              if (Ptr->getOwnershipKind() != OwnershipKind::Guaranteed) {
                Original = Ptr;
                Borrowed = Ptr = B.createBeginBorrow(Loc, Ptr);
                EndScopeList.emplace_back(Borrowed, EndScopeKind::Borrow);
              }
              SILValue Self = Ptr;
              Ptr = B.createRefElementAddr(Loc, Ptr, VD);
              Ptr = B.createBeginAccess(
                  Loc, Ptr, SILAccessKind::Deinit, SILAccessEnforcement::Static,
                  false /*noNestedConflict*/, false /*fromBuiltin*/);

              if (IsDistributedActor)
                tryToResignIdentity(Loc, B, NTD, VD, Ptr, Self);

              EndScopeList.emplace_back(Ptr, EndScopeKind::Access);
            }

            PointeeType = FieldType;
            IsSelf = false;
            break;
          }
          EltNo -= NumFieldElements;
        }

        if (!HasStoredProperties) {
          assert(EltNo == 0 && "Element count problem");
          return Ptr;
        }

        continue;
      }
    }

    // Have we gotten to our leaf element?
    assert(EltNo == 0 && "Element count problem");
    return Ptr;
  }
}

/// Push the symbolic path name to the specified element number onto the
/// specified std::string.
static void getPathStringToElementRec(TypeExpansionContext context,
                                      SILModule &Module, SILType T,
                                      unsigned EltNo, std::string &Result) {
  CanTupleType TT = T.getAs<TupleType>();
  if (!TT) {
    // Otherwise, there are no subelements.
    assert(EltNo == 0 && "Element count problem");
    return;
  }

  unsigned FieldNo = 0;
  for (unsigned i = 0, e = TT->getNumElements(); i < e; ++i) {
    auto Field = TT->getElement(i);
    SILType FieldTy = T.getTupleElementType(i);
    unsigned NumFieldElements = getElementCountRec(context, Module, FieldTy, false);

    if (EltNo < NumFieldElements) {
      Result += '.';
      if (Field.hasName())
        Result += Field.getName().str();
      else
        Result += llvm::utostr(FieldNo);
      return getPathStringToElementRec(context, Module, FieldTy, EltNo, Result);
    }

    EltNo -= NumFieldElements;

    ++FieldNo;
  }

  llvm_unreachable("Element number is out of range for this type!");
}

ValueDecl *
DIMemoryObjectInfo::getPathStringToElement(unsigned Element,
                                           std::string &Result) const {
  auto &Module = MemoryInst->getModule();

  if (isAnyInitSelf())
    Result = "self";
  else if (ValueDecl *VD =
               dyn_cast_or_null<ValueDecl>(getLoc().getAsASTNode<Decl>()))
    Result = VD->hasName() ? VD->getBaseIdentifier().str() : "_";
  else
    Result = "<unknown>";

  // If this is indexing into a field of 'self', look it up.
  auto expansionContext = TypeExpansionContext(*MemoryInst->getFunction());
  if (isNonDelegatingInit() && !isDerivedClassSelfOnly()) {
    if (auto *NTD = MemorySILType.getNominalOrBoundGenericNominal()) {
      bool HasStoredProperty = false;
      for (auto *VD : NTD->getStoredProperties()) {
        HasStoredProperty = true;
        auto FieldType =
            MemorySILType.getFieldType(VD, Module, expansionContext);
        unsigned NumFieldElements =
            getElementCountRec(expansionContext, Module, FieldType, false);
        if (Element < NumFieldElements) {
          Result += '.';
          auto originalProperty = VD->getOriginalWrappedProperty();
          if (originalProperty) {
            Result += originalProperty->getName().str();
          } else {
            Result += VD->getName().str();
          }
          getPathStringToElementRec(expansionContext, Module, FieldType,
                                    Element, Result);
          return VD;
        }
        Element -= NumFieldElements;
      }

      // If we do not have any stored properties, we have nothing of interest.
      if (!HasStoredProperty)
        return nullptr;
    }
  }

  // Get the path through a tuple, if relevant.
  getPathStringToElementRec(expansionContext, Module, MemorySILType, Element,
                            Result);

  // If we are analyzing a variable, we can generally get the decl associated
  // with it.
  if (isVariableOrResult(MemoryInst))
    return MemoryInst->getLoc().getAsASTNode<VarDecl>();

  // Otherwise, we can't.
  return nullptr;
}

/// If the specified value is a 'let' property in an initializer, return true.
bool DIMemoryObjectInfo::isElementLetProperty(unsigned Element) const {
  // If we aren't representing 'self' in a non-delegating initializer, then we
  // can't have 'let' properties.
  if (!isNonDelegatingInit())
    return IsLet;

  auto NTD = MemorySILType.getNominalOrBoundGenericNominal();

  if (!NTD) {
    // Otherwise, we miscounted elements?
    assert(Element == 0 && "Element count problem");
    return false;
  }

  auto &Module = MemoryInst->getModule();

  auto expansionContext = TypeExpansionContext(*MemoryInst->getFunction());
  for (auto *VD : NTD->getStoredProperties()) {
    auto FieldType = MemorySILType.getFieldType(VD, Module, expansionContext);
    unsigned NumFieldElements =
        getElementCountRec(expansionContext, Module, FieldType, false);
    if (Element < NumFieldElements)
      return VD->isLet();
    Element -= NumFieldElements;
  }

  // Otherwise, we miscounted elements?
  assert(Element == 0 && "Element count problem");
  return false;
}

SingleValueInstruction *DIMemoryObjectInfo::findUninitializedSelfValue() const {
  // If the object is 'self', return its uninitialized value.
  if (isAnyInitSelf())
    return getUninitializedValue();

  // Otherwise we need to scan entry block to find mark_uninitialized
  // instruction that belongs to `self`.

  auto *BB = getFunction().getEntryBlock();
  if (!BB)
    return nullptr;

  for (auto &I : *BB) {
    SILInstruction *Inst = &I;
    if (auto *MUI = dyn_cast<MarkUninitializedInst>(Inst)) {
      // If instruction is not a local variable, it could only
      // be some kind of `self` (root, delegating, derived etc.)
      // see \c MarkUninitializedInst::Kind for more details.
      if (!isVariableOrResult(MUI))
        return ::getUninitializedValue(MUI);
    }
  }
  return nullptr;
}

ConstructorDecl *DIMemoryObjectInfo::getActorInitSelf() const {
  // is it 'self'?
  if (!isVariableOrResult(MemoryInst)) {
    if (auto decl =
        dyn_cast_or_null<ClassDecl>(getASTType()->getAnyNominal()))
      // is it for an actor?
      if (decl->isAnyActor())
        if (auto *silFn = MemoryInst->getFunction())
          // are we in a constructor?
          if (auto *ctor = dyn_cast_or_null<ConstructorDecl>(
                            silFn->getDeclContext()->getAsDecl()))
            return ctor;
  }

  return nullptr;
}

//===----------------------------------------------------------------------===//
//                        DIMemoryUse Implementation
//===----------------------------------------------------------------------===//

/// onlyTouchesTrivialElements - Return true if all of the accessed elements
/// have trivial type and the access itself is a trivial instruction.
bool DIMemoryUse::onlyTouchesTrivialElements(
    const DIMemoryObjectInfo &MI) const {
  // assign_by_wrapper calls functions to assign a value. This is not
  // considered as trivial.
  if (isa<AssignByWrapperInst>(Inst) || isa<AssignOrInitInst>(Inst))
    return false;

  auto *F = Inst->getFunction();

  for (unsigned i = FirstElement, e = i + NumElements; i != e; ++i) {
    // Skip 'super.init' bit
    if (i == MI.getNumMemoryElements())
      return false;

    auto EltTy = MI.getElementType(i);
    if (!EltTy.isTrivial(*F))
      return false;
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                      DIElementUseInfo Implementation
//===----------------------------------------------------------------------===//

void DIElementUseInfo::trackStoreToSelf(SILInstruction *I) {
  StoresToSelf.push_back(I);
}

//===----------------------------------------------------------------------===//
//                     ElementUseCollector Implementation
//===----------------------------------------------------------------------===//

namespace {

/// Gathers information about a specific address and its uses to determine
/// definite initialization.
class ElementUseCollector {
public:
  typedef SmallPtrSet<SILFunction *, 8> FunctionSet;

private:
  SILModule &Module;
  const DIMemoryObjectInfo &TheMemory;
  DIElementUseInfo &UseInfo;
  FunctionSet &VisitedClosures;

  /// IsSelfOfNonDelegatingInitializer - This is true if we're looking at the
  /// top level of a 'self' variable in a non-delegating init method.
  bool IsSelfOfNonDelegatingInitializer;

  /// When walking the use list, if we index into a struct element, keep track
  /// of this, so that any indexes into tuple subelements don't affect the
  /// element we attribute an access to.
  bool InStructSubElement = false;

  /// When walking the use list, if we index into an enum slice, keep track
  /// of this.
  bool InEnumSubElement = false;

  /// If true, then encountered uses correspond to a VarDecl marked
  /// as \p @_compilerInitialized
  bool InCompilerInitializedField = false;

public:
  ElementUseCollector(const DIMemoryObjectInfo &TheMemory,
                      DIElementUseInfo &UseInfo,
                      FunctionSet &visitedClosures)
      : Module(TheMemory.getModule()), TheMemory(TheMemory), UseInfo(UseInfo),
        VisitedClosures(visitedClosures)
  {}

  /// This is the main entry point for the use walker.  It collects uses from
  /// the address and the refcount result of the allocation.
  void collectFrom(SILValue V, bool collectDestroysOfContainer) {
    IsSelfOfNonDelegatingInitializer = TheMemory.isNonDelegatingInit();

    // If this is a delegating initializer, collect uses specially.
    if (IsSelfOfNonDelegatingInitializer &&
        TheMemory.getASTType()->getClassOrBoundGenericClass() != nullptr) {
      assert(!TheMemory.isDerivedClassSelfOnly() &&
             "Should have been handled outside of here");
      // If this is a class pointer, we need to look through ref_element_addrs.
      collectClassSelfUses(V);
      return;
    }

    collectUses(V, 0);
    if (collectDestroysOfContainer) {
      assert(V == TheMemory.getUninitializedValue() &&
             "can only gather destroys of root value");
      gatherDestroysOfContainer(TheMemory, UseInfo);
    }
  }

  void trackUse(DIMemoryUse Use) { UseInfo.trackUse(Use); }

  void trackDestroy(SILInstruction *Destroy) { UseInfo.trackDestroy(Destroy); }

  /// Return the raw number of elements including the 'super.init' value.
  unsigned getNumMemoryElements() const { return TheMemory.getNumElements(); }

private:
  void collectUses(SILValue Pointer, unsigned BaseEltNo);
  bool addClosureElementUses(PartialApplyInst *pai, Operand *argUse);
  void collectAssignOrInitUses(PartialApplyInst *pai, Operand *argUse,
                               unsigned BaseEltNo = 0);

  void collectClassSelfUses(SILValue ClassPointer);
  void collectClassSelfUses(SILValue ClassPointer, SILType MemorySILType,
                            llvm::SmallDenseMap<VarDecl *, unsigned> &EN);

  void addElementUses(unsigned BaseEltNo, SILType UseTy, SILInstruction *User,
                      DIUseKind Kind, NullablePtr<VarDecl> Field = 0);
  void collectTupleElementUses(TupleElementAddrInst *TEAI, unsigned BaseEltNo);
  void collectStructElementUses(StructElementAddrInst *SEAI,
                                unsigned BaseEltNo);
};
} // end anonymous namespace

/// addElementUses - An operation (e.g. load, store, inout usec etc) on a value
/// acts on all of the aggregate elements in that value.  For example, a load
/// of $*(Int,Int) is a use of both Int elements of the tuple.  This is a helper
/// to keep the Uses data structure up to date for aggregate uses.
void ElementUseCollector::addElementUses(unsigned BaseEltNo, SILType UseTy,
                                         SILInstruction *User, DIUseKind Kind,
                                         NullablePtr<VarDecl> Field) {
  // If we're in a subelement of a struct or enum, just mark the struct, not
  // things that come after it in a parent tuple.
  unsigned NumElements = 1;
  if (TheMemory.getNumElements() != 1 && !InStructSubElement &&
      !InEnumSubElement)
    NumElements =
        getElementCountRec(TypeExpansionContext(*User->getFunction()), Module,
                           UseTy, IsSelfOfNonDelegatingInitializer);

  trackUse(DIMemoryUse(User, Kind, BaseEltNo, NumElements, Field));
}

/// Given a tuple_element_addr or struct_element_addr, compute the new
/// BaseEltNo implicit in the selected member, and recursively add uses of
/// the instruction.
void ElementUseCollector::collectTupleElementUses(TupleElementAddrInst *TEAI,
                                                  unsigned BaseEltNo) {

  // If we're walking into a tuple within a struct or enum, don't adjust the
  // BaseElt.  The uses hanging off the tuple_element_addr are going to be
  // counted as uses of the struct or enum itself.
  if (InStructSubElement || InEnumSubElement)
    return collectUses(TEAI, BaseEltNo);

  assert(!IsSelfOfNonDelegatingInitializer && "self doesn't have tuple type");

  // tuple_element_addr P, 42 indexes into the current tuple element.
  // Recursively process its uses with the adjusted element number.
  unsigned FieldNo = TEAI->getFieldIndex();
  auto T = TEAI->getOperand()->getType();
  if (T.is<TupleType>()) {
    for (unsigned i = 0; i != FieldNo; ++i) {
      SILType EltTy = T.getTupleElementType(i);
      BaseEltNo += getElementCountRec(TypeExpansionContext(*TEAI->getFunction()),
                                      Module, EltTy, false);
    }
  }

  collectUses(TEAI, BaseEltNo);
}

void ElementUseCollector::collectStructElementUses(StructElementAddrInst *SEAI,
                                                   unsigned BaseEltNo) {
  // Generally, we set the "InStructSubElement" flag and recursively process
  // the uses so that we know that we're looking at something within the
  // current element.
  if (!IsSelfOfNonDelegatingInitializer) {
    llvm::SaveAndRestore<bool> X(InStructSubElement, true);
    collectUses(SEAI, BaseEltNo);
    return;
  }

  // If this is the top level of 'self' in an init method, we treat each
  // element of the struct as an element to be analyzed independently.
  llvm::SaveAndRestore<bool> X(IsSelfOfNonDelegatingInitializer, false);

  for (auto *VD : SEAI->getStructDecl()->getStoredProperties()) {
    if (SEAI->getField() == VD)
      break;

    auto expansionContext = TypeExpansionContext(*SEAI->getFunction());
    auto FieldType = SEAI->getOperand()->getType().getFieldType(VD, Module, expansionContext);
    BaseEltNo += getElementCountRec(expansionContext, Module, FieldType, false);
  }

  collectUses(SEAI, BaseEltNo);
}

/// Return the underlying accessed pointer value. This peeks through
/// begin_access patterns such as:
///
/// %mark = mark_uninitialized [rootself] %alloc : $*T
/// %access = begin_access [modify] [unknown] %mark : $*T
/// apply %f(%access) : $(@inout T) -> ()
static SILValue getAccessedPointer(SILValue Pointer) {
  if (auto *Access = dyn_cast<BeginAccessInst>(Pointer))
    return Access->getSource();

  return Pointer;
}

static DIUseKind verifyCompilerInitialized(SILValue pointer,
                                           SILInstruction *write,
                                           DIUseKind origKind) {
  // if the write is _not_ auto-generated, it's a bad store.
  if (!write->getLoc().isAutoGenerated())
    return DIUseKind::BadExplicitStore;

  return origKind;
}

void ElementUseCollector::collectUses(SILValue Pointer, unsigned BaseEltNo) {
  assert(Pointer->getType().isAddress() &&
         "Walked through the pointer to the value?");
  SILType PointeeType = Pointer->getType().getObjectType();

  for (auto *Op : Pointer->getUses()) {
    auto *User = Op->getUser();

    // struct_element_addr P, #field indexes into the current element.
    if (auto *SEAI = dyn_cast<StructElementAddrInst>(User)) {
      collectStructElementUses(SEAI, BaseEltNo);
      continue;
    }

    // Instructions that compute a subelement are handled by a helper.
    if (auto *TEAI = dyn_cast<TupleElementAddrInst>(User)) {
      collectTupleElementUses(TEAI, BaseEltNo);
      continue;
    }

    // Look through begin_access.
    if (isa<BeginAccessInst>(User)) {
      auto begin = cast<SingleValueInstruction>(User);
      collectUses(begin, BaseEltNo);
      continue;
    }

    // Ignore end_access.
    if (isa<EndAccessInst>(User)) {
      continue;
    }

    // Look through mark_must_check. To us, it is not interesting.
    if (auto *mmi = dyn_cast<MarkMustCheckInst>(User)) {
      collectUses(mmi, BaseEltNo);
      continue;
    }

    // Loads are a use of the value.
    if (isa<LoadInst>(User)) {
      addElementUses(BaseEltNo, PointeeType, User, DIUseKind::Load);
      continue;
    }

    // Load borrows are similar to loads except that we do not support
    // scalarizing them now.
    if (isa<LoadBorrowInst>(User)) {
      addElementUses(BaseEltNo, PointeeType, User, DIUseKind::Load);
      continue;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    if (isa<Load##Name##Inst>(User)) { \
      trackUse(DIMemoryUse(User, DIUseKind::Load, BaseEltNo, 1)); \
      continue; \
    }
#include "swift/AST/ReferenceStorage.def"

    // Stores *to* the allocation are writes.
    if ((isa<StoreInst>(User) || isa<AssignInst>(User) ||
         isa<AssignByWrapperInst>(User)) &&
        Op->getOperandNumber() == 1) {
      // Coming out of SILGen, we assume that raw stores are initializations,
      // unless they have trivial type (which we classify as InitOrAssign).
      DIUseKind Kind;
      if (InStructSubElement)
        Kind = DIUseKind::PartialStore;
      else if (isa<AssignInst>(User) || isa<AssignByWrapperInst>(User))
        Kind = DIUseKind::InitOrAssign;
      else if (PointeeType.isTrivial(*User->getFunction()))
        Kind = DIUseKind::InitOrAssign;
      else
        Kind = DIUseKind::Initialization;

      // If it's a non-synthesized write to a @_compilerInitialized field,
      // indicate that in the Kind.
      if (LLVM_UNLIKELY(InCompilerInitializedField))
        Kind = verifyCompilerInitialized(Pointer, User, Kind);

      addElementUses(BaseEltNo, PointeeType, User, Kind);
      continue;
    }

#define NEVER_OR_SOMETIMES_LOADABLE_CHECKED_REF_STORAGE(Name, ...) \
    if (auto SWI = dyn_cast<Store##Name##Inst>(User)) \
      if (Op->getOperandNumber() == 1) { \
        DIUseKind Kind; \
        if (InStructSubElement) \
          Kind = DIUseKind::PartialStore; \
        else if (SWI->isInitializationOfDest()) \
          Kind = DIUseKind::Initialization; \
        else \
          Kind = DIUseKind::InitOrAssign; \
        trackUse(DIMemoryUse(User, Kind, BaseEltNo, 1)); \
        continue; \
      }
#include "swift/AST/ReferenceStorage.def"

    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is an unknown assignment.  Note that we'll
      // revisit this instruction and add it to Uses twice if it is both a load
      // and store to the same aggregate.
      DIUseKind Kind;
      if (Op->getOperandNumber() == 0)
        Kind = DIUseKind::Load;
      else if (InStructSubElement)
        Kind = DIUseKind::PartialStore;
      else if (CAI->isInitializationOfDest())
        Kind = DIUseKind::Initialization;
      else
        Kind = DIUseKind::InitOrAssign;

      addElementUses(BaseEltNo, PointeeType, User, Kind);
      continue;
    }

    if (auto *MAI = dyn_cast<MarkUnresolvedMoveAddrInst>(User)) {
      // If this is the source of the copy_addr, then this is a load.  If it is
      // the destination, then this is an unknown assignment.  Note that we'll
      // revisit this instruction and add it to Uses twice if it is both a load
      // and store to the same aggregate.
      DIUseKind Kind;
      if (Op->getOperandNumber() == 0)
        Kind = DIUseKind::Load;
      else if (InStructSubElement)
        Kind = DIUseKind::PartialStore;
      else
        Kind = DIUseKind::Initialization;

      addElementUses(BaseEltNo, PointeeType, User, Kind);
      continue;
    }

    // The apply instruction does not capture the pointer when it is passed
    // through 'inout' arguments or for indirect returns.  InOut arguments are
    // treated as uses and may-store's, but an indirect return is treated as a
    // full store.
    //
    // Note that partial_apply instructions always close over their argument.
    //
    auto Apply = FullApplySite::isa(User);
    if (Apply) {
      auto substConv = Apply.getSubstCalleeConv();
      unsigned ArgumentNumber = Op->getOperandNumber() - 1;

      // If this is an out-parameter, it is like a store.
      unsigned NumIndirectResults = substConv.getNumIndirectSILResults();
      if (ArgumentNumber < NumIndirectResults) {
        assert(!InStructSubElement && "We're initializing sub-members?");
        addElementUses(BaseEltNo, PointeeType, User, DIUseKind::Initialization);
        continue;

        // Otherwise, adjust the argument index.
      } else {
        ArgumentNumber -= NumIndirectResults;
      }

      auto ParamConvention =
          substConv.getParameters()[ArgumentNumber].getConvention();

      switch (ParamConvention) {
      case ParameterConvention::Direct_Owned:
      case ParameterConvention::Direct_Unowned:
      case ParameterConvention::Direct_Guaranteed:
      case ParameterConvention::Pack_Guaranteed:
      case ParameterConvention::Pack_Owned:
      case ParameterConvention::Pack_Inout:
        llvm_unreachable("address value passed to indirect parameter");

      // If this is an in-parameter, it is like a load.
      case ParameterConvention::Indirect_In:
      case ParameterConvention::Indirect_In_Guaranteed:
        addElementUses(BaseEltNo, PointeeType, User, DIUseKind::IndirectIn);
        continue;

      
      // If this is an @inout parameter, it is like both a load and store.
      case ParameterConvention::Indirect_InoutAliasable: {
        // FIXME: The @inout_aliasable convention is used for indirect captures
        // of both 'let' and 'var' variables. Using a more specific convention
        // for 'let' properties like @in_guaranteed unfortunately exposes bugs
        // elsewhere in the pipeline. A 'let' capture cannot really be mutated
        // by the callee, and this is enforced by sema, so we can consider it
        // a nonmutating use.
        bool isLet = true;

        for (unsigned i = 0; i < TheMemory.getNumElements(); ++i) {
          if (!TheMemory.isElementLetProperty(i)) {
            isLet = false;
            break;
          }
        }

        if (isLet) {
          addElementUses(BaseEltNo, PointeeType, User, DIUseKind::IndirectIn);
          continue;
        }
        
        LLVM_FALLTHROUGH;
      }
      case ParameterConvention::Indirect_Inout: {
        // If we're in the initializer for a struct, and this is a call to a
        // mutating method, we model that as an escape of self.  If an
        // individual sub-member is passed as inout, then we model that as an
        // inout use.
        DIUseKind Kind;
        if (TheMemory.isStructInitSelf() &&
            getAccessedPointer(Pointer) == TheMemory.getUninitializedValue()) {
          Kind = DIUseKind::Escape;
        } else if (Apply.hasSelfArgument() &&
                   Op == &Apply.getSelfArgumentOperand()) {
          Kind = DIUseKind::InOutSelfArgument;
        } else {
          Kind = DIUseKind::InOutArgument;
        }

        addElementUses(BaseEltNo, PointeeType, User, Kind);
        continue;
      }
      }
      llvm_unreachable("bad parameter convention");
    }

    if (isa<AddressToPointerInst>(User)) {
      // address_to_pointer is a mutable escape, which we model as an inout use.
      addElementUses(BaseEltNo, PointeeType, User,
                     DIUseKind::InOutArgument);
      continue;
    }

    // init_enum_data_addr is treated like a tuple_element_addr or other
    // instruction
    // that is looking into the memory object (i.e., the memory object needs to
    // be explicitly initialized by a copy_addr or some other use of the
    // projected address).
    if (auto init = dyn_cast<InitEnumDataAddrInst>(User)) {
      assert(!InStructSubElement &&
             "init_enum_data_addr shouldn't apply to struct subelements");
      // Keep track of the fact that we're inside of an enum.  This informs our
      // recursion that tuple stores are not scalarized outside, and that stores
      // should not be treated as partial stores.
      llvm::SaveAndRestore<bool> X(InEnumSubElement, true);
      collectUses(init, BaseEltNo);
      continue;
    }

    // init_existential_addr is modeled as an initialization store.
    if (isa<InitExistentialAddrInst>(User)) {
      assert(!InStructSubElement &&
             "init_existential_addr should not apply to struct subelements");
      trackUse(DIMemoryUse(User, DIUseKind::Initialization, BaseEltNo, 1));
      continue;
    }

    // inject_enum_addr is modeled as an initialization store.
    if (isa<InjectEnumAddrInst>(User)) {
      assert(!InStructSubElement &&
             "inject_enum_addr the subelement of a struct unless in a ctor");
      trackUse(DIMemoryUse(User, DIUseKind::Initialization, BaseEltNo, 1));
      continue;
    }

    // open_existential_addr is either a load or a modification depending on
    // how it's marked.  Note that the difference is just about immutability
    // checking rather than checking initialization before use.
    if (auto open = dyn_cast<OpenExistentialAddrInst>(User)) {
      // TODO: Is it reasonable to just honor the marking, or should we look
      // at all the uses of the open_existential_addr in case one they're
      // all really just loads?
      switch (open->getAccessKind()) {
      case OpenedExistentialAccess::Immutable:
        trackUse(DIMemoryUse(User, DIUseKind::Load, BaseEltNo, 1));
        continue;

      case OpenedExistentialAccess::Mutable:
        trackUse(DIMemoryUse(User, DIUseKind::InOutArgument, BaseEltNo, 1));
        continue;
      }
      llvm_unreachable("bad access kind");
    }

    // unchecked_take_enum_data_addr takes the address of the payload of an
    // optional, which could be used to update the payload. So, visit the
    // users of this instruction and ensure that there are no overwrites to an
    // immutable optional. Note that this special handling is for checking
    // immutability and is not for checking initialization before use.
    if (auto *enumDataAddr = dyn_cast<UncheckedTakeEnumDataAddrInst>(User)) {
      // Keep track of the fact that we're inside of an enum. This informs our
      // recursion that tuple stores should not be treated as a partial
      // store. This is needed because if the enum has data it would be accessed
      // through tuple_element_addr instruction. The entire enum is expected
      // to be initialized before any such access.
      llvm::SaveAndRestore<bool> X(InEnumSubElement, true);
      collectUses(enumDataAddr, BaseEltNo);
      continue;
    }

    // 'select_enum_addr' selects one of the two operands based on the case
    // of an enum value.
    // 'switch_enum_addr' jumps to one of the two branch labels (provided as
    // operands) based on the case of the enum value.
    if (isa<SelectEnumAddrInst>(User) || isa<SwitchEnumAddrInst>(User)) {
      trackUse(DIMemoryUse(User, DIUseKind::Load, BaseEltNo, 1));
      continue;
    }

    // We model destroy_addr as a release of the entire value.
    if (isa<DestroyAddrInst>(User)) {
      trackDestroy(User);
      continue;
    }

    if (isa<DeallocStackInst>(User)) {
      continue;
    }

    if (User->isDebugInstruction())
      continue;

    if (auto *PAI = dyn_cast<PartialApplyInst>(User)) {
      if (onlyUsedByAssignByWrapper(PAI))
        continue;

      if (onlyUsedByAssignOrInit(PAI)) {
        collectAssignOrInitUses(PAI, Op, BaseEltNo);
        continue;
      }

      if (BaseEltNo == 0 && addClosureElementUses(PAI, Op))
        continue;
    }

    // Sanitizer instrumentation is not user visible, so it should not
    // count as a use and must not affect compile-time diagnostics.
    if (isSanitizerInstrumentation(User))
      continue;

    // Otherwise, the use is something complicated, it escapes.
    addElementUses(BaseEltNo, PointeeType, User, DIUseKind::Escape);
  }
}

/// Add all used elements of an implicit closure, which is capturing 'self'.
///
/// We want to correctly handle implicit closures in initializers, e.g. with
/// boolean operators:
/// \code
///   init() {
///     bool_member1 = false
///     bool_member2 = false || bool_member1 // implicit closure
///   }
/// \endcode
///
/// The implicit closure ('bool_member1' at the RHS of the || operator) captures
/// the whole self, but only uses 'bool_member1'.
/// If we would add the whole captured 'self' as use, we would get a
/// "'self.bool_member2' not initialized" error at the partial_apply.
/// Therefore we look into the body of the closure and only add the actually
/// used members.
bool ElementUseCollector::addClosureElementUses(PartialApplyInst *pai,
                                                Operand *argUse) {
  SILFunction *callee = pai->getReferencedFunctionOrNull();
  if (!callee)
    return false;

  // Implicit closures are "transparent", which means they are always inlined.
  // It would probably also work to handle non-transparent closures (e.g.
  // explicit closures). But if the closure is not inlined we could end up
  // passing a partially initialized self to the closure function. Although it
  // would probably not cause any real problems, an `@in_guaranteed` argument
  // (the captured 'self') is assumed to be fully initialized in SIL.
  if (!callee->isTransparent())
    return false;

  // Implicit closures are only partial-applied once and there cannot be a
  // recursive cycle of implicit closures.
  // Nevertheless such a scenario is theoretically possible in SIL. To be on the
  // safe side, check for cycles.
  if (!VisitedClosures.insert(callee).second)
    return false;

  unsigned argIndex = ApplySite(pai).getCalleeArgIndex(*argUse);
  SILArgument *arg = callee->getArgument(argIndex);
  
  // Bail if arg is not the original 'self' object, but e.g. a projected member.
  assert(TheMemory.getType().isObject());
  if (arg->getType().getObjectType() != TheMemory.getType())
    return false;

  DIElementUseInfo ArgUseInfo;
  ElementUseCollector collector(TheMemory, ArgUseInfo, VisitedClosures);
  collector.collectFrom(arg, /*collectDestroysOfContainer*/ false);

  if (!ArgUseInfo.Releases.empty() || !ArgUseInfo.StoresToSelf.empty())
    return false;
    
  for (const DIMemoryUse &use : ArgUseInfo.Uses) {
    // Only handle loads and escapes. Implicit closures will not have stores or
    // store-like uses, anyway.
    // Also, as we don't do a flow-sensitive analysis of the callee, we cannot
    // handle stores, because we don't know if they are unconditional or not.
    switch (use.Kind) {
      case DIUseKind::Load:
      case DIUseKind::Escape:
      case DIUseKind::InOutArgument:
        break;
      default:
        return false;
    }
  }

  // Track all uses of the closure.
  for (const DIMemoryUse &use : ArgUseInfo.Uses) {
    trackUse(DIMemoryUse(pai, use.Kind, use.FirstElement, use.NumElements));
  }
  return true;
}

void
ElementUseCollector::collectAssignOrInitUses(PartialApplyInst *pai,
                                             Operand *argUse,
                                             unsigned BaseEltNo) {
  for (Operand *Op : pai->getUses()) {
    SILInstruction *User = Op->getUser();
    if (!isa<AssignOrInitInst>(User) || Op->getOperandNumber() != 2) {
      continue;
    }

    /// AssignOrInit doesn't operate on `self` so we need to make sure
    /// that the flag is dropped before calling \c addElementUses.
    llvm::SaveAndRestore<bool> X(IsSelfOfNonDelegatingInitializer, false);

    auto *inst = cast<AssignOrInitInst>(User);
    auto *typeDC = inst->getReferencedInitAccessor()
                       ->getDeclContext()
                       ->getSelfNominalTypeDecl();

    auto selfTy = pai->getOperand(1)->getType();

    auto addUse = [&](VarDecl *property, DIUseKind useKind) {
      auto expansionContext = TypeExpansionContext(*pai->getFunction());
      auto type = selfTy.getFieldType(property, Module, expansionContext);
      addElementUses(Module.getFieldIndex(typeDC, property), type, User,
                     useKind, property);
    };

    auto initializedElts = inst->getInitializedProperties();
    if (initializedElts.empty()) {
      // Add a placeholder use that doesn't touch elements to make sure that
      // the `assign_or_init` instruction gets the kind set when `initializes`
      // list is empty.
      trackUse(DIMemoryUse(User, DIUseKind::InitOrAssign, BaseEltNo, 0));
    } else {
      for (auto *property : initializedElts)
        addUse(property, DIUseKind::InitOrAssign);
    }

    for (auto *property : inst->getAccessedProperties())
      addUse(property, DIUseKind::Load);
  }
}

/// collectClassSelfUses - Collect all the uses of a 'self' pointer in a class
/// constructor.  The memory object has class type.
void ElementUseCollector::collectClassSelfUses(SILValue ClassPointer) {
  assert(IsSelfOfNonDelegatingInitializer &&
         TheMemory.getASTType()->getClassOrBoundGenericClass() != nullptr);

  // For efficiency of lookup below, compute a mapping of the local ivars in the
  // class to their element number.
  llvm::SmallDenseMap<VarDecl *, unsigned> EltNumbering;

  {
    SILType T = TheMemory.getType();
    auto *NTD = T.getNominalOrBoundGenericNominal();
    unsigned NumElements = 0;
    for (auto *VD : NTD->getStoredProperties()) {
      EltNumbering[VD] = NumElements;
      auto expansionContext = TypeExpansionContext(TheMemory.getFunction());
      NumElements += getElementCountRec(
          expansionContext, Module,
          T.getFieldType(VD, Module, expansionContext), false);
    }
  }

  // If we are looking at the init method for a root class, just walk the
  // MUI use-def chain directly to find our uses.
  if (TheMemory.isRootSelf() ||
  
      // Also, just visit all users if ClassPointer is a closure argument,
      // i.e. collectClassSelfUses is called from addClosureElementUses.
      isa<SILFunctionArgument>(ClassPointer)) {
    collectClassSelfUses(ClassPointer, TheMemory.getType(), EltNumbering);
    return;
  }

  // The number of stores of the initial 'self' argument into the self box
  // that we saw.
  unsigned StoresOfArgumentToSelf = 0;

  // Okay, given that we have a proper setup, we walk the use chains of the self
  // box to find any accesses to it. The possible uses are one of:
  //
  //   1) The initialization store.
  //   2) Loads of the box, which have uses of self hanging off of them.
  //   3) An assign to the box, which happens at super.init.
  //   4) Potential escapes after super.init, if self is closed over.
  //
  // Handle each of these in turn.
  SmallVector<Operand *, 8> Uses(TheMemory.getUninitializedValue()->getUses());
  while (!Uses.empty()) {
    Operand *Op = Uses.pop_back_val();
    SILInstruction *User = Op->getUser();

    // Stores to self.
    if (auto *SI = dyn_cast<StoreInst>(User)) {
      if (Op->getOperandNumber() == 1) {
        // The initial store of 'self' into the box at the start of the
        // function. Ignore it.
        if (auto *Arg = dyn_cast<SILArgument>(SI->getSrc())) {
          if (Arg->getParent() == TheMemory.getParentBlock()) {
            ++StoresOfArgumentToSelf;
            continue;
          }
        }

        // A store of a load from the box is ignored.
        // SILGen emits these if delegation to another initializer was
        // interrupted before the initializer was called.
        SILValue src = SI->getSrc();
        // Look through conversions.
        while (auto conversion = dyn_cast<ConversionInst>(src))
          src = conversion->getConverted();
        
        if (auto *LI = dyn_cast<LoadInst>(src))
          if (LI->getOperand() == TheMemory.getUninitializedValue())
            continue;

        // Any other store needs to be recorded.
        UseInfo.trackStoreToSelf(SI);
        continue;
      }
    }

    // Ignore end_borrows. These can only come from us being the source of a
    // load_borrow.
    if (isa<EndBorrowInst>(User))
      continue;

    // Recurse through begin_access.
    if (auto *beginAccess = dyn_cast<BeginAccessInst>(User)) {
      Uses.append(beginAccess->getUses().begin(), beginAccess->getUses().end());
      continue;
    }

    if (isa<EndAccessInst>(User))
      continue;

    // Loads of the box produce self, so collect uses from them.
    if (isa<LoadInst>(User) || isa<LoadBorrowInst>(User)) {
      auto load = cast<SingleValueInstruction>(User);
      collectClassSelfUses(load, TheMemory.getType(), EltNumbering);
      continue;
    }

    // destroy_addr on the box is load+release, which is treated as a release.
    if (isa<DestroyAddrInst>(User) || isa<StrongReleaseInst>(User) ||
        isa<DestroyValueInst>(User)) {
      trackDestroy(User);
      continue;
    }

    // Ignore the deallocation of the stack box.  Its contents will be
    // uninitialized by the point it executes.
    if (isa<DeallocStackInst>(User))
      continue;

    // We can safely handle anything else as an escape.  They should all happen
    // after super.init is invoked.  As such, all elements must be initialized
    // and super.init must be called.
    trackUse(DIMemoryUse(User, DIUseKind::Load, 0, TheMemory.getNumElements()));
  }

  assert(StoresOfArgumentToSelf == 1 &&
         "The 'self' argument should have been stored into the box exactly once");
}

static bool isSuperInitUse(SILInstruction *User) {
  auto *LocExpr = User->getLoc().getAsASTNode<ApplyExpr>();
  if (!LocExpr) {
    // If we're reading a .sil file, treat a call to "superinit" as a
    // super.init call as a hack to allow us to write testcases.
    auto *AI = dyn_cast<ApplyInst>(User);
    if (AI && AI->getLoc().isSILFile())
      if (auto *Fn = AI->getReferencedFunctionOrNull())
        if (Fn->getName() == "superinit")
          return true;
    return false;
  }

  // This is a super.init call if structured like this:
  // (call_expr type='SomeClass'
  //   (dot_syntax_call_expr type='() -> SomeClass' super
  //     (other_constructor_ref_expr implicit decl=SomeClass.init)
  //     (super_ref_expr type='SomeClass'))
  //   (...some argument...)
  LocExpr = dyn_cast<ApplyExpr>(LocExpr->getFn());
  if (!LocExpr || !isa<OtherConstructorDeclRefExpr>(LocExpr->getFn()))
    return false;

  auto *UnaryArg = LocExpr->getArgs()->getUnaryExpr();
  assert(UnaryArg);
  if (UnaryArg->isSuperExpr())
    return true;

  // Instead of super_ref_expr, we can also get this for inherited delegating
  // initializers:

  // (derived_to_base_expr implicit type='C'
  //   (declref_expr type='D' decl='self'))
  if (auto *DTB = dyn_cast<DerivedToBaseExpr>(UnaryArg)) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(DTB->getSubExpr())) {
        ASTContext &Ctx = DRE->getDecl()->getASTContext();
      if (DRE->getDecl()->isImplicit() &&
          DRE->getDecl()->getBaseName() == Ctx.Id_self)
        return true;
    }
  }

  return false;
}

/// Return true if this SILBBArgument is the result of a call to super.init.
static bool isSuperInitUse(SILArgument *Arg) {
  // We only handle a very simple pattern here where there is a single
  // predecessor to the block, and the predecessor instruction is a try_apply
  // of a throwing delegated init.
  auto *BB = Arg->getParent();
  auto *Pred = BB->getSinglePredecessorBlock();

  // The two interesting cases are where self.init throws, in which case
  // the argument came from a try_apply, or if self.init is failable,
  // in which case we have a switch_enum.
  if (!Pred || (!isa<TryApplyInst>(Pred->getTerminator()) &&
                !isa<SwitchEnumInst>(Pred->getTerminator())))
    return false;

  return isSuperInitUse(Pred->getTerminator());
}

static bool isUninitializedMetatypeInst(SILInstruction *I) {
  // A simple reference to "type(of:)" is always fine,
  // even if self is uninitialized.
  if (isa<ValueMetatypeInst>(I))
    return true;

  // Sometimes we get an upcast whose sole usage is a value_metatype_inst,
  // for example when calling a convenience initializer from a superclass.
  if (auto *UCI = dyn_cast<UpcastInst>(I)) {
    for (auto *Op : UCI->getUses()) {
      auto *User = Op->getUser();
      if (isa<ValueMetatypeInst>(User))
        continue;
      return false;
    }

    return true;
  }

  return false;
}

/// isSelfInitUse - Return true if this apply_inst is a call to self.init.
static bool isSelfInitUse(SILInstruction *I) {
  // If we're reading a .sil file, treat a call to "selfinit" as a
  // self.init call as a hack to allow us to write testcases.
  if (I->getLoc().isSILFile()) {
    if (auto *AI = dyn_cast<ApplyInst>(I))
      if (auto *Fn = AI->getReferencedFunctionOrNull())
        if (Fn->getName().startswith("selfinit"))
          return true;

    return false;
  }

  // Otherwise, a self.init call must have location info, and must be an expr
  // to be considered.
  auto *LocExpr = I->getLoc().getAsASTNode<Expr>();
  if (!LocExpr)
    return false;

  // If this is a force_value_expr, it might be a self.init()! call, look
  // through it.
  if (auto *FVE = dyn_cast<ForceValueExpr>(LocExpr))
    LocExpr = FVE->getSubExpr();

  // If we have the rebind_self_in_constructor_expr, then the call is the
  // sub-expression.
  if (auto *RB = dyn_cast<RebindSelfInConstructorExpr>(LocExpr)) {
    LocExpr = RB->getSubExpr();
    // Look through TryExpr or ForceValueExpr, but not both.
    if (auto *TE = dyn_cast<AnyTryExpr>(LocExpr))
      LocExpr = TE->getSubExpr();
    else if (auto *FVE = dyn_cast<ForceValueExpr>(LocExpr))
      LocExpr = FVE->getSubExpr();
  }

  // Look through covariant return, if any.
  if (auto CRE = dyn_cast<CovariantReturnConversionExpr>(LocExpr))
    LocExpr = CRE->getSubExpr();

  // This is a self.init call if structured like this:
  //
  // (call_expr type='SomeClass'
  //   (dot_syntax_call_expr type='() -> SomeClass' self
  //     (other_constructor_ref_expr implicit decl=SomeClass.init)
  //     (decl_ref_expr type='SomeClass', "self"))
  //   (...some argument...)
  //
  // Or like this:
  //
  // (call_expr type='SomeClass'
  //   (dot_syntax_call_expr type='() -> SomeClass' self
  //     (decr_ref_expr implicit decl=SomeClass.init)
  //     (decl_ref_expr type='SomeClass', "self"))
  //   (...some argument...)
  //
  if (auto *AE = dyn_cast<ApplyExpr>(LocExpr)) {
    if ((AE = dyn_cast<ApplyExpr>(AE->getFn()))) {
      if (isa<OtherConstructorDeclRefExpr>(AE->getFn()))
        return true;
      if (auto *DRE = dyn_cast<DeclRefExpr>(AE->getFn()))
        if (auto *CD = dyn_cast<ConstructorDecl>(DRE->getDecl()))
          if (CD->isFactoryInit())
            return true;
    }
  }
  return false;
}

/// Return true if this SILBBArgument is the result of a call to self.init.
static bool isSelfInitUse(SILArgument *Arg) {
  // We only handle a very simple pattern here where there is a single
  // predecessor to the block, and the predecessor instruction is a try_apply
  // of a throwing delegated init.
  auto *BB = Arg->getParent();
  auto *Pred = BB->getSinglePredecessorBlock();

  // The two interesting cases are where self.init throws, in which case
  // the argument came from a try_apply, or if self.init is failable,
  // in which case we have a switch_enum.
  if (!Pred || (!isa<TryApplyInst>(Pred->getTerminator()) &&
                !isa<SwitchEnumInst>(Pred->getTerminator())))
    return false;

  return isSelfInitUse(Pred->getTerminator());
}

static bool isSelfOperand(const Operand *Op, const SILInstruction *User) {
  unsigned operandNum = Op->getOperandNumber();
  unsigned numOperands;

  // FIXME: This should just be cast<FullApplySite>(User) but that doesn't
  // work
  if (auto *AI = dyn_cast<ApplyInst>(User))
    numOperands = AI->getNumOperands();
  else
    numOperands = cast<TryApplyInst>(User)->getNumOperands();

  return (operandNum == numOperands - 1);
}

void ElementUseCollector::collectClassSelfUses(
    SILValue ClassPointer, SILType MemorySILType,
    llvm::SmallDenseMap<VarDecl *, unsigned> &EltNumbering) {
  llvm::SmallVector<Operand *, 16> Worklist(ClassPointer->use_begin(),
                                            ClassPointer->use_end());
  while (!Worklist.empty()) {
    auto *Op = Worklist.pop_back_val();
    auto *User = Op->getUser();

    // Ignore any method lookup use.
    if (isa<SuperMethodInst>(User) ||
        isa<ObjCSuperMethodInst>(User) ||
        isa<ClassMethodInst>(User) ||
        isa<ObjCMethodInst>(User)) {
      continue;
    }

    // Skip end_borrow and end_access.
    if (isa<EndBorrowInst>(User) || isa<EndAccessInst>(User))
      continue;

    // ref_element_addr P, #field lookups up a field.
    if (auto *REAI = dyn_cast<RefElementAddrInst>(User)) {
      // FIXME: This is a Sema bug and breaks resilience, we should not
      // emit ref_element_addr in such cases at all.
      if (EltNumbering.count(REAI->getField()) != 0) {
        assert(EltNumbering.count(REAI->getField()) &&
               "ref_element_addr not a local field?");

        // Remember whether the field is marked '@_compilerInitialized'
        const bool hasCompInit =
          REAI->getField()->getAttrs().hasAttribute<CompilerInitializedAttr>();
        llvm::SaveAndRestore<bool> X1(InCompilerInitializedField, hasCompInit);

        // Recursively collect uses of the fields.  Note that fields of the class
        // could be tuples, so they may be tracked as independent elements.
        llvm::SaveAndRestore<bool> X2(IsSelfOfNonDelegatingInitializer, false);
        collectUses(REAI, EltNumbering[REAI->getField()]);
        continue;
      }
    }

    // retains of self in class constructors can be ignored since we do not care
    // about the retain that we are producing, but rather the consumer of the
    // retain. This /should/ be true today and will be verified as true in
    // Semantic SIL.
    if (isa<StrongRetainInst>(User)) {
      continue;
    }

    // Destroys of self are tracked as a release.
    //
    // *NOTE* In the case of a failing initializer, the release on the exit path
    // needs to cleanup the partially initialized elements.
    if (isa<StrongReleaseInst>(User) || isa<DestroyValueInst>(User)) {
      trackDestroy(User);
      continue;
    }

    // Look through begin_borrow, upcast, unchecked_ref_cast
    // and copy_value.
    if (isa<BeginBorrowInst>(User) || isa<BeginAccessInst>(User) ||
        isa<UpcastInst>(User) || isa<UncheckedRefCastInst>(User) ||
        isa<CopyValueInst>(User) || isa<MarkMustCheckInst>(User)) {
      auto value = cast<SingleValueInstruction>(User);
      std::copy(value->use_begin(), value->use_end(),
                std::back_inserter(Worklist));
      continue;
    }

    // If this is an ApplyInst, check to see if this is part of a self.init
    // call in a delegating initializer.
    DIUseKind Kind = DIUseKind::Load;
    if (isa<FullApplySite>(User) &&
        (isSelfInitUse(User) || isSuperInitUse(User))) {
      if (isSelfOperand(Op, User)) {
        Kind = DIUseKind::SelfInit;
      }
    }
    
    if (isUninitializedMetatypeInst(User))
      continue;

    if (User->isDebugInstruction())
      continue;
 
    // If this is a partial application of self, then this is an escape point
    // for it.
    if (auto *PAI = dyn_cast<PartialApplyInst>(User)) {
      if (onlyUsedByAssignByWrapper(PAI))
        continue;

      if (onlyUsedByAssignOrInit(PAI)) {
        collectAssignOrInitUses(PAI, Op);
        continue;
      }

      if (addClosureElementUses(PAI, Op))
        continue;

      Kind = DIUseKind::Escape;
    }

    trackUse(DIMemoryUse(User, Kind, 0, TheMemory.getNumElements()));
  }
}

//===----------------------------------------------------------------------===//
//                         DelegatingInitUseCollector
//===----------------------------------------------------------------------===//

static void
collectDelegatingInitUses(const DIMemoryObjectInfo &TheMemory,
                          DIElementUseInfo &UseInfo,
                          SingleValueInstruction *I) {
  for (auto *Op : I->getUses()) {
    SILInstruction *User = Op->getUser();

    // destroy_addr is a release of the entire value. This can result from an
    // early release due to a conditional initializer.
    if (isa<DestroyAddrInst>(User)) {
      UseInfo.trackDestroy(User);
      continue;
    }

    // For delegating initializers, we only track calls to self.init with
    // specialized code. All other uses are modeled as escapes.
    //
    // *NOTE* This intentionally ignores all stores, which (if they got emitted
    // as copyaddr or assigns) will eventually get rewritten as assignments (not
    // initializations), which is the right thing to do.
    DIUseKind Kind = DIUseKind::Escape;

    // Stores *to* the allocation are writes.  If the value being stored is a
    // call to self.init()... then we have a self.init call.
    if (auto *AI = dyn_cast<AssignInst>(User)) {
      if (AI->getDest() == I) {
        UseInfo.trackStoreToSelf(AI);
        Kind = DIUseKind::InitOrAssign;
      }
    }

    if (auto *CAI = dyn_cast<CopyAddrInst>(User)) {
      if (CAI->getDest() == I) {
        UseInfo.trackStoreToSelf(CAI);
        Kind = DIUseKind::InitOrAssign;
      }
    }

    if (auto *MAI = dyn_cast<MarkUnresolvedMoveAddrInst>(User)) {
      if (MAI->getDest() == I) {
        UseInfo.trackStoreToSelf(MAI);
        Kind = DIUseKind::Initialization;
      }
    }

    // Look through begin_access
    if (auto *BAI = dyn_cast<BeginAccessInst>(User)) {
      collectDelegatingInitUses(TheMemory, UseInfo, BAI);
      continue;
    }

    // Look through mark_must_check.
    if (auto *MMCI = dyn_cast<MarkMustCheckInst>(User)) {
      collectDelegatingInitUses(TheMemory, UseInfo, MMCI);
      continue;
    }

    // Ignore end_access
    if (isa<EndAccessInst>(User))
      continue;
    
    // A load of the value that's only used to handle a type(of:) query before
    // self has been initialized can just use the initializer's metatype
    // argument. For value types, there's no metatype subtyping to worry about,
    // and for class convenience initializers, `self` notionally has the
    // original Self type as its dynamic type before theoretically being
    // rebound.
    //
    // This is necessary for source compatibility; previously, convenience
    // initializers behaved like in Objective-C where the initializer received
    // an uninitialized object to fill in, and type(of: self) worked by asking
    // for the dynamic type of that uninitialized object.
    if (isa<LoadInst>(User)) {
      auto UserVal = cast<SingleValueInstruction>(User);
      if (UserVal->hasOneUse()
          && isa<ValueMetatypeInst>(UserVal->getSingleUse()->get())) {
        Kind = DIUseKind::LoadForTypeOfSelf;
      }
    }
    // value_metatype may appear on a borrowed load, in which case there'll
    // be an end_borrow use in addition to the value_metatype.
    if (isa<LoadBorrowInst>(User)) {
      auto UserVal = cast<SingleValueInstruction>(User);
      bool onlyUseIsValueMetatype = false;
      for (auto use : UserVal->getUses()) {
        auto *user = use->getUser();
        if (isa<EndBorrowInst>(user))
          continue;
        if (isa<ValueMetatypeInst>(user)) {
          onlyUseIsValueMetatype = true;
          continue;
        }
        onlyUseIsValueMetatype = false;
        break;
      }
      if (onlyUseIsValueMetatype) {
        Kind = DIUseKind::LoadForTypeOfSelf;
      }
    }
    // value_metatype may also use the 'self' value directly, if it has an
    // address-only type.
    if (isa<ValueMetatypeInst>(User))
      Kind = DIUseKind::TypeOfSelf;

    // We can safely handle anything else as an escape.  They should all happen
    // after self.init is invoked.
    UseInfo.trackUse(DIMemoryUse(User, Kind, 0, 1));
  }
}

//===----------------------------------------------------------------------===//
//                        ClassInitElementUseCollector
//===----------------------------------------------------------------------===//

namespace {

class ClassInitElementUseCollector {
  const DIMemoryObjectInfo &TheMemory;
  DIElementUseInfo &UseInfo;

public:
  ClassInitElementUseCollector(const DIMemoryObjectInfo &TheMemory,
                                         DIElementUseInfo &UseInfo)
      : TheMemory(TheMemory), UseInfo(UseInfo) {}

  void collectClassInitSelfUses();

  // *NOTE* Even though this takes a SILInstruction it actually only accepts
  // load_borrow and load instructions. This is enforced via an assert.
  void collectClassInitSelfLoadUses(SingleValueInstruction *MUI,
                                    SingleValueInstruction *LI);
};

} // end anonymous namespace

/// collectDelegatingClassInitSelfUses - Collect uses of the self argument in a
/// delegating-constructor-for-a-class case.
void ClassInitElementUseCollector::collectClassInitSelfUses() {
  // When we're analyzing a delegating constructor, we aren't field sensitive at
  // all.  Just treat all members of self as uses of the single
  // non-field-sensitive value.
  assert(TheMemory.getNumElements() == 1 && "delegating inits only have 1 bit");
  auto *uninitMemory = TheMemory.getUninitializedValue();

  // The number of stores of the initial 'self' argument into the self box
  // that we saw.
  unsigned StoresOfArgumentToSelf = 0;

  // We walk the use chains of the self uninitMemory to find any accesses to it.
  // The possible uses are:
  //   1) The initialization store.
  //   2) Loads of the box, which have uses of self hanging off of them.
  //   3) An assign to the box, which happens at super.init.
  //   4) Potential escapes after super.init, if self is closed over.
  // Handle each of these in turn.
  //
  SmallVector<Operand *, 8> Uses(uninitMemory->getUses());
  while (!Uses.empty()) {
    Operand *Op = Uses.pop_back_val();
    SILInstruction *User = Op->getUser();

    // Ignore end_borrow. If we see an end_borrow it can only come from a
    // load_borrow from ourselves.
    if (isa<EndBorrowInst>(User))
      continue;

    // Recurse through begin_access.
    if (auto *beginAccess = dyn_cast<BeginAccessInst>(User)) {
      Uses.append(beginAccess->getUses().begin(), beginAccess->getUses().end());
      continue;
    }
    if (isa<EndAccessInst>(User))
      continue;

    // Stores to self.
    if (auto *SI = dyn_cast<StoreInst>(User)) {
      if (Op->getOperandNumber() == 1) {
        // A store of 'self' into the box at the start of the
        // function. Ignore it.
        if (auto *Arg = dyn_cast<SILArgument>(SI->getSrc())) {
          if (Arg->getParent() == uninitMemory->getParent()) {
            ++StoresOfArgumentToSelf;
            continue;
          }
        }

        // A store of a load from the box is ignored.
        //
        // SILGen emits these if delegation to another initializer was
        // interrupted before the initializer was called.
        SILValue src = SI->getSrc();
        // Look through conversions.
        while (auto conversion = dyn_cast<ConversionInst>(src))
          src = conversion->getConverted();
        
        if (auto *LI = dyn_cast<LoadInst>(src))
          if (LI->getOperand() == uninitMemory)
            continue;

        // Any other store needs to be recorded.
        UseInfo.trackStoreToSelf(SI);
        continue;
      }
    }

    // For class initializers, the assign into the self box may be
    // captured as SelfInit or SuperInit elsewhere.
    if (isa<AssignInst>(User) &&
        Op->getOperandNumber() == 1) {
      // If the source of the assignment is an application of a C
      // function, there is no metatype argument, so treat the
      // assignment to the self box as the initialization.
      if (auto *AI = dyn_cast<ApplyInst>(cast<AssignInst>(User)->getSrc())) {
        if (auto *Fn = AI->getCalleeFunction()) {
          if (Fn->getRepresentation() ==
              SILFunctionTypeRepresentation::CFunctionPointer) {
            UseInfo.trackStoreToSelf(User);
            UseInfo.trackUse(DIMemoryUse(User, DIUseKind::SelfInit, 0, 1));
            continue;
          }
        }
      }
    }

    // Stores *to* the allocation are writes.  If the value being stored is a
    // call to self.init()... then we have a self.init call.
    if (auto *AI = dyn_cast<AssignInst>(User)) {
      if (auto *AssignSource = AI->getOperand(0)->getDefiningInstruction()) {
        if (isSelfInitUse(AssignSource) || isSuperInitUse(AssignSource)) {
          UseInfo.trackStoreToSelf(User);
          UseInfo.trackUse(DIMemoryUse(User, DIUseKind::SelfInit, 0, 1));
          continue;
        }
      }
      if (auto *AssignSource = dyn_cast<SILArgument>(AI->getOperand(0))) {
        if (AssignSource->getParent() == AI->getParent() &&
            (isSelfInitUse(AssignSource) || isSuperInitUse(AssignSource))) {
          UseInfo.trackStoreToSelf(User);
          UseInfo.trackUse(DIMemoryUse(User, DIUseKind::SelfInit, 0, 1));
          continue;
        }
      }
    }

    // Loads of the box produce self, so collect uses from them.
    if (isa<LoadInst>(User) || isa<LoadBorrowInst>(User)) {
      collectClassInitSelfLoadUses(uninitMemory,
                                   cast<SingleValueInstruction>(User));
      continue;
    }

    // destroy_addr on the box is load+release, which is treated as a release.
    if (isa<DestroyAddrInst>(User)) {
      UseInfo.trackDestroy(User);
      continue;
    }

    // We can safely handle anything else as an escape.  They should all happen
    // after self.init is invoked.
    UseInfo.trackUse(DIMemoryUse(User, DIUseKind::Escape, 0, 1));
  }

  assert(StoresOfArgumentToSelf == 1 &&
         "The 'self' argument should have been stored into the box exactly once");
}

void ClassInitElementUseCollector::collectClassInitSelfLoadUses(
    SingleValueInstruction *MUI, SingleValueInstruction *LI) {
  assert(isa<ProjectBoxInst>(MUI) || isa<MarkUninitializedInst>(MUI));
  assert(isa<LoadBorrowInst>(LI) || isa<LoadInst>(LI));

  // If we have a load, then this is a use of the box.  Look at the uses of
  // the load to find out more information.
  llvm::SmallVector<Operand *, 8> Worklist(LI->use_begin(), LI->use_end());
  while (!Worklist.empty()) {
    auto *Op = Worklist.pop_back_val();
    auto *User = Op->getUser();

    // Ignore any method lookup use.
    if (isa<SuperMethodInst>(User) ||
        isa<ObjCSuperMethodInst>(User) ||
        isa<ClassMethodInst>(User) ||
        isa<ObjCMethodInst>(User)) {
      continue;
    }

    // We ignore retains of self.
    if (isa<StrongRetainInst>(User))
      continue;

    // Ignore end_borrow.
    if (isa<EndBorrowInst>(User))
      continue;

    // A release of a load from the self box in a class delegating
    // initializer might be releasing an uninitialized self, which requires
    // special processing.
    if (isa<StrongReleaseInst>(User) || isa<DestroyValueInst>(User)) {
      UseInfo.trackDestroy(User);
      continue;
    }

    // Look through begin_borrow, upcast and unchecked_ref_cast.
    if (isa<BeginBorrowInst>(User) ||
        isa<UpcastInst>(User) ||
        isa<UncheckedRefCastInst>(User)) {
      auto I = cast<SingleValueInstruction>(User);
      llvm::copy(I->getUses(), std::back_inserter(Worklist));
      continue;
    }

    // We only track two kinds of uses for delegating initializers:
    // calls to self.init, and "other", which we choose to model as escapes.
    // This intentionally ignores all stores, which (if they got emitted as
    // copyaddr or assigns) will eventually get rewritten as assignments
    // (not initializations), which is the right thing to do.
    DIUseKind Kind = DIUseKind::Escape;

    // If this is an ApplyInst, check to see if this is part of a self.init
    // call in a delegating initializer.
    if (isa<FullApplySite>(User) &&
        (isSelfInitUse(User) || isSuperInitUse(User))) {
      if (isSelfOperand(Op, User)) {
        Kind = DIUseKind::SelfInit;
      }
    }

    // If this load's value is being stored immediately back into the delegating
    // mark_uninitialized buffer, skip the use.
    //
    // This is to handle situations where we do not actually consume self as a
    // result of situations such as:
    //
    // 1. The usage of a metatype to allocate the object.
    //
    // 2. If our self init call has a throwing function as an argument that
    //    actually throws.
    if (auto *SI = dyn_cast<StoreInst>(User)) {
      if (SI->getDest() == MUI) {
        SILValue src = SI->getSrc();

        // Look through conversions.
        while (auto *conversion = dyn_cast<ConversionInst>(src)) {
          src = conversion->getConverted();
        }

        if (auto *li = dyn_cast<LoadInst>(src)) {
          if (li->getOperand() == MUI) {
            continue;
          }
        }
      }
    }

    if (isUninitializedMetatypeInst(User))
      continue;

    UseInfo.trackUse(DIMemoryUse(User, Kind, 0, 1));
  }
}

//===----------------------------------------------------------------------===//
//                            Top Level Entrypoint
//===----------------------------------------------------------------------===//

static bool shouldPerformClassInitSelf(const DIMemoryObjectInfo &MemoryInfo) {
  if (MemoryInfo.isDelegatingSelfAllocated())
    return true;

  return MemoryInfo.isNonDelegatingInit() &&
         MemoryInfo.getASTType()->getClassOrBoundGenericClass() != nullptr &&
         MemoryInfo.isDerivedClassSelfOnly();
}

/// Analyze all uses of the specified allocation instruction (alloc_box,
/// alloc_stack or mark_uninitialized), classifying them and storing the
/// information found into the Uses and Releases lists.
void swift::ownership::collectDIElementUsesFrom(
    const DIMemoryObjectInfo &MemoryInfo, DIElementUseInfo &UseInfo) {

  if (shouldPerformClassInitSelf(MemoryInfo)) {
    ClassInitElementUseCollector UseCollector(MemoryInfo, UseInfo);
    UseCollector.collectClassInitSelfUses();
    gatherDestroysOfContainer(MemoryInfo, UseInfo);
    return;
  }

  if (MemoryInfo.isDelegatingInit()) {
    // When we're analyzing a delegating constructor, we aren't field sensitive
    // at all. Just treat all members of self as uses of the single
    // non-field-sensitive value.
    assert(MemoryInfo.getNumElements() == 1 &&
           "delegating inits only have 1 bit");
    collectDelegatingInitUses(MemoryInfo, UseInfo,
                              MemoryInfo.getUninitializedValue());
    gatherDestroysOfContainer(MemoryInfo, UseInfo);
    return;
  }

  ElementUseCollector::FunctionSet VisitedClosures;
  ElementUseCollector collector(MemoryInfo, UseInfo, VisitedClosures);
  collector.collectFrom(MemoryInfo.getUninitializedValue(),
                        /*collectDestroysOfContainer*/ true);
}
