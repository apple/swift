//===--- Projection.cpp ---------------------------------------------------===//
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

#define DEBUG_TYPE "sil-projection"
#include "swift/SIL/Projection.h"
#include "swift/Basic/IndexTrie.h"
#include "swift/Basic/NullablePtr.h"
#include "swift/SIL/DebugUtils.h"
#include "swift/SIL/InstructionUtils.h"
#include "swift/SIL/SILBuilder.h"
#include "swift/SIL/SILUndef.h"
#include "llvm/ADT/None.h"
#include "llvm/Support/Debug.h"

using namespace swift;

//===----------------------------------------------------------------------===//
//                         Projection Static Asserts
//===----------------------------------------------------------------------===//

/// These are just for performance and verification. If one needs to make
/// changes that cause the asserts the fire, please update them. The purpose is
/// to prevent these predicates from changing values by mistake.
static_assert(std::is_standard_layout<Projection>::value,
              "Expected projection to be a standard layout type");


//===----------------------------------------------------------------------===//
//                              Utility 
//===----------------------------------------------------------------------===//

/// Extract an integer index from a SILValue.
///
/// Return true if IndexVal is a constant index representable as unsigned
/// int. We do not support symbolic projections yet, only 32-bit unsigned
/// integers.
bool swift::getIntegerIndex(SILValue IndexVal, int &IndexConst) {
  auto *IndexLiteral = dyn_cast<IntegerLiteralInst>(IndexVal);
  if (!IndexLiteral)
    return false;

  APInt ConstInt = IndexLiteral->getValue();
  // Reserve 1 bit for encoding. See AccessPath::Index.
  if (!ConstInt.isSignedIntN(31))
    return false;

  IndexConst = ConstInt.getSExtValue();
  assert(((IndexConst << 1) >> 1) == IndexConst);
  return true;
}

//===----------------------------------------------------------------------===//
//                               Projection
//===----------------------------------------------------------------------===//

constexpr int ProjectionIndex::TailIndex;

Projection::Projection(SingleValueInstruction *I) : Value() {
  if (!I)
    return;
  /// Initialize given the specific instruction type and verify with asserts
  /// that we constructed it correctly.
  switch (I->getKind()) {
  // If we do not support this instruction kind, then just bail. Index will
  // be None so the Projection will be invalid.
  default:
    return;
  case SILInstructionKind::StructElementAddrInst: {
    auto *SEAI = cast<StructElementAddrInst>(I);
    Value = ValueTy(ProjectionKind::Struct, SEAI->getFieldIndex());
    assert(getKind() == ProjectionKind::Struct);
    assert(getIndex() == int(SEAI->getFieldIndex()));
    break;
  }
  case SILInstructionKind::StructExtractInst: {
    auto *SEI = cast<StructExtractInst>(I);
    Value = ValueTy(ProjectionKind::Struct, SEI->getFieldIndex());
    assert(getKind() == ProjectionKind::Struct);
    assert(getIndex() == int(SEI->getFieldIndex()));
    break;
  }
  case SILInstructionKind::RefElementAddrInst: {
    auto *REAI = cast<RefElementAddrInst>(I);
    Value = ValueTy(ProjectionKind::Class, REAI->getFieldIndex());
    assert(getKind() == ProjectionKind::Class);
    assert(getIndex() == int(REAI->getFieldIndex()));
    break;
  }
  case SILInstructionKind::RefTailAddrInst: {
    auto *RTAI = cast<RefTailAddrInst>(I);
    auto *Ty = RTAI->getTailType().getASTType().getPointer();
    Value = ValueTy(ProjectionKind::TailElems, Ty);
    assert(getKind() == ProjectionKind::TailElems);
    break;
  }
  case SILInstructionKind::ProjectBoxInst: {
    auto *PBI = cast<ProjectBoxInst>(I);
    Value = ValueTy(ProjectionKind::Box, static_cast<uintptr_t>(0));
    assert(getKind() == ProjectionKind::Box);
    assert(getIndex() == 0);
    (void) PBI;
    break;
  }
  case SILInstructionKind::TupleExtractInst: {
    auto *TEI = cast<TupleExtractInst>(I);
    Value = ValueTy(ProjectionKind::Tuple, TEI->getFieldIndex());
    assert(getKind() == ProjectionKind::Tuple);
    assert(getIndex() == int(TEI->getFieldIndex()));
    break;
  }
  case SILInstructionKind::TupleElementAddrInst: {
    auto *TEAI = cast<TupleElementAddrInst>(I);
    Value = ValueTy(ProjectionKind::Tuple, TEAI->getFieldIndex());
    assert(getKind() == ProjectionKind::Tuple);
    assert(getIndex() == int(TEAI->getFieldIndex()));
    break;
  }
  case SILInstructionKind::UncheckedEnumDataInst: {
    auto *UEDI = cast<UncheckedEnumDataInst>(I);
    Value = ValueTy(ProjectionKind::Enum, UEDI->getElementNo());
    assert(getKind() == ProjectionKind::Enum);
    assert(getIndex() == int(UEDI->getElementNo()));
    break;
  }
  case SILInstructionKind::UncheckedTakeEnumDataAddrInst: {
    auto *UTEDAI = cast<UncheckedTakeEnumDataAddrInst>(I);
    Value = ValueTy(ProjectionKind::Enum, UTEDAI->getCaseIndex());
    assert(getKind() == ProjectionKind::Enum);
    assert(getIndex() == int(UTEDAI->getCaseIndex()));
    break;
  }
  case SILInstructionKind::IndexAddrInst: {
    // We can represent all integers provided here since getIntegerIndex only
    // returns 32 bit values. When that changes, this code will need to be
    // updated and a MaxLargeIndex will need to be used here. Currently we
    // represent large Indexes using a 64 bit integer, so we don't need to mess
    // with anything.
    int NewIndex = 0;
    auto *IAI = cast<IndexAddrInst>(I);
    // TODO: handle negative indices
    if (getIntegerIndex(IAI->getIndex(), NewIndex) && NewIndex >= 0) {
      Value = ValueTy(ProjectionKind::Index, NewIndex);
      assert(getKind() == ProjectionKind::Index);
      assert(getIndex() == NewIndex);
    }
    break;
  }
  case SILInstructionKind::UpcastInst: {
    auto *Ty = I->getType().getASTType().getPointer();
    assert(Ty->isCanonical());
    Value = ValueTy(ProjectionKind::Upcast, Ty);
    assert(getKind() == ProjectionKind::Upcast);
    break;
  }
  case SILInstructionKind::UncheckedRefCastInst: {
    auto *Ty = I->getType().getASTType().getPointer();
    assert(Ty->isCanonical());
    Value = ValueTy(ProjectionKind::RefCast, Ty);
    assert(getKind() == ProjectionKind::RefCast);
    break;
  }
  case SILInstructionKind::UncheckedBitwiseCastInst:
  case SILInstructionKind::UncheckedAddrCastInst: {
    auto *Ty = I->getType().getASTType().getPointer();
    assert(Ty->isCanonical());
    Value = ValueTy(ProjectionKind::BitwiseCast, Ty);
    assert(getKind() == ProjectionKind::BitwiseCast);
    break;
  }
  }
}

/// Apply this projection to \p BaseType and return the relevant subfield's
/// SILType if BaseField has less subtypes than projection's offset.
///
/// WARNING: This is not a constant time operation because it is implemented
/// in terms of getVarDecl, which requests all BaseType's stored properties.
SILType Projection::getType(SILType BaseType, SILModule &M,
                            TypeExpansionContext context) const {
  assert(isValid());
  switch (getKind()) {
  case ProjectionKind::Struct:
  case ProjectionKind::Class:
    return BaseType.getFieldType(getVarDecl(BaseType), M, context);
  case ProjectionKind::Enum:
    return BaseType.getEnumElementType(getEnumElementDecl(BaseType), M, context);
  case ProjectionKind::Box:
    return getSILBoxFieldType(context, BaseType.castTo<SILBoxType>(), M.Types,
                              getIndex());
  case ProjectionKind::Tuple:
    return BaseType.getTupleElementType(getIndex());
  case ProjectionKind::Upcast:
  case ProjectionKind::RefCast:
  case ProjectionKind::BitwiseCast:
  case ProjectionKind::TailElems:
    return getCastType(BaseType);
  case ProjectionKind::Index:
    // Index types do not change the underlying type.
    return BaseType;
  }

  llvm_unreachable("Unhandled ProjectionKind in switch.");
}

NullablePtr<SingleValueInstruction>
Projection::createObjectProjection(SILBuilder &B, SILLocation Loc,
                                   SILValue Base) const {
  SILType BaseTy = Base->getType();

  // We can only create a value projection from an object.
  if (!BaseTy.isObject())
    return nullptr;

  // Ok, we now know that the type of Base and the type represented by the base
  // of this projection match and that this projection can be represented as
  // value. Create the instruction if we can. Otherwise, return nullptr.
  switch (getKind()) {
  case ProjectionKind::Struct:
    return B.createStructExtract(Loc, Base, getVarDecl(BaseTy));
  case ProjectionKind::Tuple:
    return B.createTupleExtract(Loc, Base, getIndex());
  case ProjectionKind::Index:
    return nullptr;
  case ProjectionKind::Enum:
    return B.createUncheckedEnumData(Loc, Base, getEnumElementDecl(BaseTy));
  case ProjectionKind::Class:
    return nullptr;
  case ProjectionKind::TailElems:
    return nullptr;
  case ProjectionKind::Box:
    return nullptr;
  case ProjectionKind::Upcast:
    return B.createUpcast(Loc, Base, getCastType(BaseTy));
  case ProjectionKind::RefCast:
    return B.createUncheckedRefCast(Loc, Base, getCastType(BaseTy));
  case ProjectionKind::BitwiseCast:
    return B.createUncheckedBitwiseCast(Loc, Base, getCastType(BaseTy));
  }

  llvm_unreachable("Unhandled ProjectionKind in switch.");
}

NullablePtr<SingleValueInstruction>
Projection::createAddressProjection(SILBuilder &B, SILLocation Loc,
                                    SILValue Base) const {
  SILType BaseTy = Base->getType();

  // We can only create an address projection from an object, unless we have a
  // class.
  if (BaseTy.getClassOrBoundGenericClass() || !BaseTy.isAddress())
    return nullptr;

  // Ok, we now know that the type of Base and the type represented by the base
  // of this projection match and that this projection can be represented as
  // value. Create the instruction if we can. Otherwise, return nullptr.
  switch (getKind()) {
  case ProjectionKind::Struct:
    return B.createStructElementAddr(Loc, Base, getVarDecl(BaseTy));
  case ProjectionKind::Tuple:
    return B.createTupleElementAddr(Loc, Base, getIndex());
  case ProjectionKind::Index: {
    auto IntLiteralTy =
        SILType::getBuiltinIntegerType(64, B.getModule().getASTContext());
    auto IntLiteralIndex =
        B.createIntegerLiteral(Loc, IntLiteralTy, getIndex());
    return B.createIndexAddr(Loc, Base, IntLiteralIndex,
        // TODO: do we need to be conservative here?
        /*needsStackProtection=*/ true);
  }
  case ProjectionKind::Enum:
    return B.createUncheckedTakeEnumDataAddr(Loc, Base,
                                             getEnumElementDecl(BaseTy));
  case ProjectionKind::Class:
    return B.createRefElementAddr(Loc, Base, getVarDecl(BaseTy));
  case ProjectionKind::TailElems:
    return B.createRefTailAddr(Loc, Base, getCastType(BaseTy));
  case ProjectionKind::Box:
    return B.createProjectBox(Loc, Base, getIndex());
  case ProjectionKind::Upcast:
    return B.createUpcast(Loc, Base, getCastType(BaseTy));
  case ProjectionKind::RefCast:
  case ProjectionKind::BitwiseCast:
    return B.createUncheckedAddrCast(Loc, Base, getCastType(BaseTy));
  }

  llvm_unreachable("Unhandled ProjectionKind in switch.");
}

void Projection::getFirstLevelProjections(
    SILType Ty, SILModule &Mod, TypeExpansionContext context,
    llvm::SmallVectorImpl<Projection> &Out) {
  if (auto *S = Ty.getStructOrBoundGenericStruct()) {
    unsigned Count = 0;
    for (auto *VDecl : S->getStoredProperties()) {
      (void) VDecl;
      Projection P(ProjectionKind::Struct, Count++);
      LLVM_DEBUG(ProjectionPath X(Ty);
                 assert(X.getMostDerivedType(Mod, context) == Ty);
                 X.append(P);
                 assert(X.getMostDerivedType(Mod, context) ==
                                     Ty.getFieldType(VDecl, Mod, context));
                 X.verify(Mod, context););
      Out.push_back(P);
    }
    return;
  }

  if (auto TT = Ty.getAs<TupleType>()) {
    for (unsigned i = 0, e = TT->getNumElements(); i != e; ++i) {
      Projection P(ProjectionKind::Tuple, i);
      LLVM_DEBUG(ProjectionPath X(Ty);
                 assert(X.getMostDerivedType(Mod, context) == Ty);
                 X.append(P);
                 assert(X.getMostDerivedType(Mod, context) ==
                        Ty.getTupleElementType(i));
                 X.verify(Mod, context););
      Out.push_back(P);
    }
    return;
  }

  if (auto *C = Ty.getClassOrBoundGenericClass()) {
    unsigned Count = 0;
    for (auto *VDecl : C->getStoredProperties()) {
      (void) VDecl;
      Projection P(ProjectionKind::Class, Count++);
      LLVM_DEBUG(ProjectionPath X(Ty);
                 assert(X.getMostDerivedType(Mod, context) == Ty);
                 X.append(P);
                 assert(X.getMostDerivedType(Mod, context) ==
                        Ty.getFieldType(VDecl, Mod, context));
                 X.verify(Mod, context););
      Out.push_back(P);
    }
    return;
  }

  if (auto Box = Ty.getAs<SILBoxType>()) {
    for (unsigned field : indices(Box->getLayout()->getFields())) {
      Projection P(ProjectionKind::Box, field);
      LLVM_DEBUG(ProjectionPath X(Ty);
                 assert(X.getMostDerivedType(Mod, context) == Ty); X.append(P);
                 assert(X.getMostDerivedType(Mod, context) ==
                        getSILBoxFieldType(context, Box, Mod.Types, field));
                 X.verify(Mod, context););
      (void)Box;
      Out.push_back(P);
    }
    return;
  }
}

//===----------------------------------------------------------------------===//
//                            Projection Path
//===----------------------------------------------------------------------===//

llvm::Optional<ProjectionPath> ProjectionPath::getProjectionPath(SILValue Start,
                                                                 SILValue End) {
  ProjectionPath P(Start->getType(), End->getType());

  // If Start == End, there is a "trivial" projection path in between the
  // two. This is represented by returning an empty ProjectionPath.
  if (Start == End)
    return std::move(P);

  // Do not inspect the body of types with unreferenced types such as bitfields
  // and unions. This is currently only associated with structs.
  if (Start->getType().aggregateHasUnreferenceableStorage() ||
      End->getType().aggregateHasUnreferenceableStorage())
    return llvm::None;

  auto Iter = End;
  while (Start != Iter) {

    if (auto *mvr = dyn_cast<MultipleValueInstructionResult>(Iter)) {
      if (auto *bci = dyn_cast<BeginCOWMutationInst>(mvr->getParent())) {
        Iter = bci->getOperand();
        continue;
      }
      break;
    }

    // end_cow_mutation and begin_access are not projections, but we need to be
    // able to form valid ProjectionPaths across them, otherwise optimization
    // passes like RLE/DSE cannot recognize their locations.
    //
    // TODO: migrate users to getProjectionPath to the AccessPath utility to
    // avoid this hack.
    if (!isa<EndCOWMutationInst>(Iter) && !isa<BeginAccessInst>(Iter) &&
        !isa<BeginBorrowInst>(Iter) && !isa<MoveValueInst>(Iter)) {
      Projection AP(Iter);
      if (!AP.isValid())
        break;
      // We _must_ ignore zero-indexing projections so that alias analysis
      // recognizes an "alias" between two addresses where one has such an
      // `index_addr 0` instruction and the other doesn't.
      if (AP.getKind() != ProjectionKind::Index || AP.getIndex() != 0)
        P.Path.push_back(AP);
    }
    Iter = cast<SingleValueInstruction>(*Iter).getOperand(0);
  }

  // Return None if we have an empty projection list or if Start == Iter.
  // We do not worry about th implicit #0 in case of index_addr, as the
  // ProjectionPath never allow paths to be compared as a list of indices.
  // Only the encoded type+index pair will be compared.
  if (P.empty() || Start != Iter)
    return llvm::None;

  // Reverse to get a path from base to most-derived.
  std::reverse(P.Path.begin(), P.Path.end());

  // Otherwise, return P.
  return std::move(P);
}

/// Returns true if the two paths have a non-empty symmetric difference.
///
/// This means that the two objects have the same base but access different
/// fields of the base object.
bool 
ProjectionPath::hasNonEmptySymmetricDifference(const ProjectionPath &RHS) const{
  // First make sure that both of our base types are the same.
  if (BaseType != RHS.BaseType)
    return false;

  // Otherwise, we have a common base and perhaps some common subpath.
  auto LHSIter = Path.begin();
  auto RHSIter = RHS.Path.begin();

  bool FoundDifferingProjections = false;

  // For each index i until min path size...
  unsigned i = 0;
  for (unsigned e = std::min(size(), RHS.size()); i != e; ++i) {
    // Grab the current projections.
    const Projection &LHSProj = *LHSIter;
    const Projection &RHSProj = *RHSIter;

    // If we are accessing different fields of a common object, the two
    // projection paths may have a non-empty symmetric difference. We check if
    if (LHSProj != RHSProj) {
      LLVM_DEBUG(llvm::dbgs() << "        Path different at index: "
                              << i << '\n');
      FoundDifferingProjections = true;
      break;
    }

    // Continue if we are accessing the same field.
    ++LHSIter;
    ++RHSIter;
  }

  // All path elements are the same. The symmetric difference is empty.
  if (!FoundDifferingProjections)
    return false;

  // We found differing projections, but we need to make sure that there are no
  // casts in the symmetric difference. To be conservative, we only wish to
  // allow for casts to appear in the common parts of projections.
  for (unsigned li = i, e = size(); li != e; ++li) {
    if (LHSIter->isAliasingCast())
      return false;
    ++LHSIter;
  }
  for (unsigned ri = i, e = RHS.size(); ri != e; ++ri) {
    if (RHSIter->isAliasingCast())
      return false;
    ++RHSIter;
  }

  // If we don't have any casts in our symmetric difference (i.e. only typed
  // GEPs), then we can say that these actually have a symmetric difference we
  // can understand. The fundamental issue here is that since we do not have any
  // notion of size, we cannot know the effect of a cast + gep on the final
  // location that we are reaching.
  return true;
}

/// TODO: Integrate has empty non-symmetric difference into here.
SubSeqRelation_t
ProjectionPath::computeSubSeqRelation(const ProjectionPath &RHS) const {
  // Make sure that both base types are the same. Otherwise, we can not compare
  // the projections as sequences.
  if (BaseType != RHS.BaseType)
    return SubSeqRelation_t::Unknown;

  // If both paths are empty, return Equal.
  if (empty() && RHS.empty())
    return SubSeqRelation_t::Equal;

  auto LHSIter = begin();
  auto RHSIter = RHS.begin();

  unsigned MinPathSize = std::min(size(), RHS.size());

  // For each index i until min path size...
  for (unsigned i = 0; i != MinPathSize; ++i) {
    // Grab the current projections.
    const Projection &LHSProj = *LHSIter;
    const Projection &RHSProj = *RHSIter;

    // If the two projections do not equal exactly, return Unrelated.
    //
    // TODO: If Index equals zero, then we know that the two lists have nothing
    // in common and should return unrelated. If Index is greater than zero,
    // then we know that the two projection paths have a common base but a
    // non-empty symmetric difference. For now we just return Unrelated since I
    // can not remember why I had the special check in the
    // hasNonEmptySymmetricDifference code.
    if (LHSProj != RHSProj)
      return SubSeqRelation_t::Unknown;

    // Otherwise increment reverse iterators.
    ++LHSIter;
    ++RHSIter;
  }

  // Ok, we now know that one of the paths is a subsequence of the other. If
  // both size() and RHS.size() equal then we know that the entire sequences
  // equal.
  if (size() == RHS.size())
    return SubSeqRelation_t::Equal;

  // If MinPathSize == size(), then we know that LHS is a strict subsequence of
  // RHS.
  if (MinPathSize == size())
    return SubSeqRelation_t::LHSStrictSubSeqOfRHS;

  // Otherwise, we know that MinPathSize must be RHS.size() and RHS must be a
  // strict subsequence of LHS. Assert to check this and return.
  assert(MinPathSize == RHS.size() &&
         "Since LHS and RHS don't equal and size() != MinPathSize, RHS.size() "
         "must equal MinPathSize");
  return SubSeqRelation_t::RHSStrictSubSeqOfLHS;
}

llvm::Optional<ProjectionPath>
ProjectionPath::removePrefix(const ProjectionPath &Path,
                             const ProjectionPath &Prefix) {
  // We can only subtract paths that have the same base.
  if (Path.BaseType != Prefix.BaseType)
    return llvm::None;

  // If Prefix is greater than or equal to Path in size, Prefix can not be a
  // prefix of Path. Return None.
  unsigned PrefixSize = Prefix.size();
  unsigned PathSize = Path.size();

  if (PrefixSize >= PathSize)
    return llvm::None;

  // First make sure that the prefix matches.
  llvm::Optional<ProjectionPath> P = ProjectionPath(Path.BaseType);
  for (unsigned i = 0; i < PrefixSize; ++i) {
    if (Path.Path[i] != Prefix.Path[i]) {
      P.reset();
      return P;
    }
  }

  // Add the rest of Path to P and return P.
  for (unsigned i = PrefixSize, e = PathSize; i != e; ++i) {
    P->Path.push_back(Path.Path[i]);
  }

  return P;
}

void Projection::print(raw_ostream &os, SILType baseType) const {
  switch (getKind()) {
  case ProjectionKind::Struct:
  case ProjectionKind::Class: {
    auto *Decl = getVarDecl(baseType);
    os << "Field: ";
    Decl->print(os);
    break;
  }
  case ProjectionKind::Enum: {
    auto *Decl = getEnumElementDecl(baseType);
    os << "Enum: ";
    Decl->print(os);
    break;
  }
  case ProjectionKind::Index:
  case ProjectionKind::Tuple: {
    os << "Index: " << getIndex();
    break;
  }
  case ProjectionKind::Box: {
    os << " Box over";
    break;
  }
  case ProjectionKind::Upcast: {
    os << "UpCast";
    break;
  }
  case ProjectionKind::RefCast: {
    os << "RefCast";
    break;
  }
  case ProjectionKind::BitwiseCast: {
    os << "BitwiseCast";
    break;
  }
  case ProjectionKind::TailElems: {
    os << " TailElems";
    break;
  }
  }
}

raw_ostream &ProjectionPath::print(raw_ostream &os, SILModule &M,
                                   TypeExpansionContext context) const {
  os << "Projection Path [";
  SILType IterType = getBaseType();
  for (const Projection &IterProj : Path) {
    SILType BaseType = IterType;
    IterType = IterProj.getType(IterType, M, context);

    os << BaseType.getAddressType() << "\n  ";

    IterProj.print(os, BaseType);
    os << " in: ";
  }
  os << IterType.getAddressType() << "]\n";
  return os;
}

void ProjectionPath::dump(SILModule &M, TypeExpansionContext context) const {
  print(llvm::dbgs(), M, context);
}

void ProjectionPath::verify(SILModule &M, TypeExpansionContext context) {
#ifndef NDEBUG
  SILType IterTy = getBaseType();
  assert(IterTy);
  for (auto &Proj : Path) {
    IterTy = Proj.getType(IterTy, M, context);
    assert(IterTy);
  }
#endif
}

void
ProjectionPath::expandTypeIntoLeafProjectionPaths(SILType B, SILModule *Mod,
                                                  TypeExpansionContext context,
                                                  ProjectionPathList &Paths) {
  // Perform a BFS to expand the given type into projectionpath each of
  // which contains 1 field from the type.
  llvm::SmallVector<ProjectionPath, 8> Worklist;
  llvm::SmallVector<Projection, 8> Projections;

  // Push an empty projection path to get started.
  ProjectionPath P(B);
  Worklist.push_back(P);
  do {
    // Get the next level projections based on current projection's type.
    ProjectionPath PP = Worklist.pop_back_val();
    // Get the current type to process.
    SILType Ty = PP.getMostDerivedType(*Mod, context);

    LLVM_DEBUG(llvm::dbgs() << "Visiting type: " << Ty << "\n");

    // If this is a class type, we have reached the end of the type
    // tree for this type.
    //
    // We do not push its next level projection into the worklist,
    // if we do that, we could run into an infinite loop, e.g.
    //
    //   class SelfLoop {
    //     var p : SelfLoop
    //   }
    //
    //   struct XYZ {
    //     var x : Int
    //     var y : SelfLoop
    //   }
    //
    // The worklist would never be empty in this case !.
    //
    if (Ty.getClassOrBoundGenericClass()) {
      LLVM_DEBUG(llvm::dbgs() << "    Found class. Finished projection list\n");
      Paths.push_back(PP);
      continue;
    }

    // Get the first level projection of the current type.
    Projections.clear();
    Projection::getFirstLevelProjections(Ty, *Mod, context, Projections);

    // Reached the end of the projection tree, this field can not be expanded
    // anymore.
    if (Projections.empty()) {
      LLVM_DEBUG(llvm::dbgs() << "    No projections. "
                 "Finished projection list\n");
      Paths.push_back(PP);
      continue;
    }

    // Keep expanding the location.
    for (auto &P : Projections) {
      ProjectionPath X(B);
      X.append(PP);
      ///assert(PP.getMostDerivedType(*Mod) == X.getMostDerivedType(*Mod));
      X.append(P);
      Worklist.push_back(X);
    }
    // Keep iterating if the worklist is not empty.
  } while (!Worklist.empty());
}

bool ProjectionPath::hasUncoveredNonTrivials(SILType B, const SILFunction &F,
                                             ProjectionPathSet &CPaths) {
  auto &Mod = F.getModule();

  llvm::SmallVector<ProjectionPath, 4> Worklist, Paths;
  // Push an empty projection path to get started.
  ProjectionPath P(B);
  Worklist.push_back(P);
  do {
    // Get the next level projections based on current projection's type.
    ProjectionPath PP = Worklist.pop_back_val();
 
    // If this path is part of the covered path, then continue.
    if (CPaths.find(PP) != CPaths.end())
      continue;
      
    // Get the current type to process.
    SILType Ty = PP.getMostDerivedType(Mod, F.getTypeExpansionContext());

    // Get the first level projection of the current type.
    llvm::SmallVector<Projection, 4> Projections;
    Projection::getFirstLevelProjections(Ty, Mod, F.getTypeExpansionContext(),
                                         Projections);

    // Reached the end of the projection tree, this field can not be expanded
    // anymore.
    if (Projections.empty()) {
      Paths.push_back(PP);
      continue;
    }

    // There is at least one projection path that leads to a type with
    // reference semantics.
    if (Ty.getClassOrBoundGenericClass()) {
      Paths.push_back(PP);
      continue;
    }

    // Keep expanding the location.
    for (auto &P : Projections) {
      ProjectionPath X(B);
      X.append(PP);
      assert(PP.getMostDerivedType(Mod, F.getTypeExpansionContext()) ==
             X.getMostDerivedType(Mod, F.getTypeExpansionContext()));
      X.append(P);
      Worklist.push_back(X);
    }
    // Keep iterating if the worklist is not empty.
  } while (!Worklist.empty());

  // Check whether any path leads to a non-trivial type.
  for (auto &X : Paths) {
    if (!X.getMostDerivedType(Mod, F.getTypeExpansionContext()).isTrivial(F))
      return true;
  }   
  return false;
}

SILValue
ProjectionPath::
createExtract(SILValue Base, SILInstruction *Inst, bool IsVal) const {
  // If we found a projection path, but there are no projections, then the two
  // loads must be the same, return PrevLI.
  if (Path.empty())
    return Base;

  // Ok, at this point we know that we can construct our aggregate projections
  // from our list of address projections.
  SILValue LastExtract = Base;
  SILBuilder Builder(Inst);
  Builder.setCurrentDebugScope(Inst->getFunction()->getDebugScope());

  // We use an auto-generated SILLocation for now.
  // TODO: make the sil location more precise.
  SILLocation Loc = RegularLocation::getAutoGeneratedLocation();

  // Construct the path!
  for (auto PI = Path.begin(), PE = Path.end(); PI != PE; ++PI) {
    if (IsVal) {
      LastExtract =
          PI->createObjectProjection(Builder, Loc, LastExtract).get();
      continue;
    }
    LastExtract =
        PI->createAddressProjection(Builder, Loc, LastExtract).get();
  }

  // Return the last extract we created.
  return LastExtract;
}

bool
Projection::operator<(const Projection &Other) const {
  // If we have a nominal kind...
  if (isNominalKind()) {
    // And Other is also nominal...
    if (Other.isNominalKind()) {
      // Just compare the value decl pointers.
      return getIndex() < Other.getIndex();
    }

    // Otherwise if Other is not nominal, return true since we always sort
    // decls before indices.
    return true;
  } else {
    // If this is not a nominal kind and Other is nominal, return
    // false. Nominal kinds are always sorted before non-nominal kinds.
    if (Other.isNominalKind())
      return false;

    // Otherwise, we are both index projections. Compare the indices.
    return getIndex() < Other.getIndex();
  }
}

NullablePtr<SingleValueInstruction>
Projection::createAggFromFirstLevelProjections(
    SILBuilder &B, SILLocation Loc, SILType BaseType,
    ArrayRef<SILValue> Values) {
  if (BaseType.getStructOrBoundGenericStruct()) {
    return B.createStruct(Loc, BaseType, Values);
  }

  if (BaseType.is<TupleType>()) {
    return B.createTuple(Loc, BaseType, Values);
  }

  return nullptr;
}

SILValue Projection::getOperandForAggregate(SILInstruction *I) const {
  switch (getKind()) {
    case ProjectionKind::Struct:
      if (isa<StructInst>(I))
        return I->getOperand(getIndex());
      break;
    case ProjectionKind::Tuple:
      if (isa<TupleInst>(I))
        return I->getOperand(getIndex());
      break;
    case ProjectionKind::Index:
      break;
    case ProjectionKind::Enum:
      if (auto *EI = dyn_cast<EnumInst>(I)) {
        if (EI->getElement() == getEnumElementDecl(EI->getType())) {
          assert(EI->hasOperand() && "expected data operand");
          return EI->getOperand();
        }
      }
      break;
    case ProjectionKind::Class:
    case ProjectionKind::TailElems:
    case ProjectionKind::Box:
    case ProjectionKind::Upcast:
    case ProjectionKind::RefCast:
    case ProjectionKind::BitwiseCast:
      // There is no SIL instruction to create a class or box by aggregating
      // values.
      break;
  }
  return SILValue();
}

//===----------------------------------------------------------------------===//
//                             ProjectionTreeNode
//===----------------------------------------------------------------------===//

ProjectionTreeNode *
ProjectionTreeNode::getChildForProjection(ProjectionTree &Tree,
                                          const Projection &P) {
  for (unsigned Index : ChildProjections) {
    ProjectionTreeNode *N = Tree.getNode(Index);
    if (N->Proj && N->Proj.value() == P) {
      return N;
    }
  }
  return nullptr;
}

ProjectionTreeNode *
ProjectionTreeNode::getParent(ProjectionTree &Tree) {
  if (!Parent)
    return nullptr;
  return Tree.getNode(Parent.value());
}

const ProjectionTreeNode *
ProjectionTreeNode::getParent(const ProjectionTree &Tree) const {
  if (!Parent)
    return nullptr;
  return Tree.getNode(Parent.value());
}

NullablePtr<SingleValueInstruction>
ProjectionTreeNode::
createProjection(SILBuilder &B, SILLocation Loc, SILValue Arg) const {
  if (!Proj)
    return nullptr;

  return Proj->createProjection(B, Loc, Arg);
}

// Projection tree only supports structs and tuples today.
static bool isSupportedProjection(const Projection &p) {
  switch (p.getKind()) {
  case ProjectionKind::Struct:
  case ProjectionKind::Tuple:
    return true;
  case ProjectionKind::Class:
  case ProjectionKind::Enum:
  case ProjectionKind::Box:
  case ProjectionKind::Upcast:
  case ProjectionKind::RefCast:
  case ProjectionKind::BitwiseCast:
  case ProjectionKind::TailElems:
  case ProjectionKind::Index:
    return false;
  }
  llvm_unreachable("unhandled kind");
}

void
ProjectionTreeNode::
processUsersOfValue(ProjectionTree &Tree,
                    llvm::SmallVectorImpl<ValueNodePair> &Worklist,
                    SILValue Value) {
  LLVM_DEBUG(llvm::dbgs() << "    Looking at Users:\n");

  // For all uses of V...
  for (Operand *Op : getNonDebugUses(Value)) {
    // Grab the User of V.
    SILInstruction *User = Op->getUser();

    LLVM_DEBUG(llvm::dbgs() << "        " << *User);

    // The projections we can handle are always single-value instructions.
    auto projectionInst = dyn_cast<SingleValueInstruction>(User);
    if (!projectionInst) {
      LLVM_DEBUG(llvm::dbgs() << "            Failed to create projection. "
                 "Adding to non projection user!\n");
      addNonProjectionUser(Op);
      continue;
    }

    auto P = Projection(projectionInst);

    // If we fail to create a projection or this is a type of projection that we
    // do not support, add User as a user to this node and continue.
    if (!P.isValid() || !isSupportedProjection(P)) {
      LLVM_DEBUG(llvm::dbgs() << "            Failed to create projection. "
                 "Adding to non projection user!\n");
      addNonProjectionUser(Op);
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "            Created projection.\n");

    // we have a projection to the next level children, create the next
    // level children nodes lazily.
    if (!Initialized)
      createNextLevelChildren(
          Tree, TypeExpansionContext(*projectionInst->getFunction()));

    // Look up the Node for this projection add {User, ChildNode} to the
    // worklist.
    //
    // *NOTE* This means that we will process ChildNode multiple times
    // potentially with different projection users.
    if (auto *ChildNode = getChildForProjection(Tree, P)) {
      LLVM_DEBUG(llvm::dbgs() << "            Found child for projection: "
                 << ChildNode->getType() << "\n");

      SILValue V = SILValue(projectionInst);
      Worklist.push_back({V, ChildNode});
    } else {
      LLVM_DEBUG(llvm::dbgs() << "            Did not find a child for "
                 "projection!. Adding to non projection user!\n");

      // The only projection which we do not currently handle are enums since we
      // may not know the correct case. This can be extended in the future.
      // Is the user an epilogue release ?
      addNonProjectionUser(Op);
    }
  }
}

void ProjectionTreeNode::createNextLevelChildrenForStruct(
    ProjectionTree &Tree, TypeExpansionContext context, StructDecl *SD) {
  SILModule &Mod = Tree.getModule();
  unsigned ChildIndex = 0;
  SILType Ty = getType();
  for (VarDecl *VD : SD->getStoredProperties()) {
    assert(Tree.getNode(Index) == this && "Node is not mapped to itself?");
    SILType NodeTy = Ty.getFieldType(VD, Mod, context);
    auto *Node = Tree.createChildForStruct(this, NodeTy, VD, ChildIndex++);
    LLVM_DEBUG(llvm::dbgs() << "        Creating child for: " <<NodeTy << "\n");
    LLVM_DEBUG(llvm::dbgs() << "            Projection: " 
               << Node->getProjection().value().getIndex() << "\n");
    ChildProjections.push_back(Node->getIndex());
    assert(getChildForProjection(Tree, Node->getProjection().value()) == Node &&
           "Child not matched to its projection in parent!");
    assert(Node->getParent(Tree) == this && "Parent of Child is not Parent?!");
  }
}

void
ProjectionTreeNode::
createNextLevelChildrenForTuple(ProjectionTree &Tree, TupleType *TT) {
  SILType Ty = getType();
  for (unsigned i = 0, e = TT->getNumElements(); i != e; ++i) {
    assert(Tree.getNode(Index) == this && "Node is not mapped to itself?");
    SILType NodeTy = Ty.getTupleElementType(i);
    auto *Node = Tree.createChildForTuple(this, NodeTy, i);
    LLVM_DEBUG(llvm::dbgs() << "        Creating child for: " << NodeTy <<"\n");
    LLVM_DEBUG(llvm::dbgs() << "            Projection: "
               << Node->getProjection().value().getIndex() << "\n");
    ChildProjections.push_back(Node->getIndex());
    assert(getChildForProjection(Tree, Node->getProjection().value()) == Node &&
           "Child not matched to its projection in parent!");
    assert(Node->getParent(Tree) == this && "Parent of Child is not Parent?!");
  }
}

void ProjectionTreeNode::createNextLevelChildren(ProjectionTree &Tree,
                                                 TypeExpansionContext context) {
  LLVM_DEBUG(llvm::dbgs() << "    Creating children for: " << getType() <<"\n");
  if (Initialized) {
    LLVM_DEBUG(llvm::dbgs() << "        Already initialized! bailing!\n");
    return;
  }

  Initialized = true;

  SILType Ty = getType();

  if (Ty.aggregateHasUnreferenceableStorage()) {
    LLVM_DEBUG(llvm::dbgs() << "        Has unreferenced storage bailing!\n");
    return;
  }

  if (auto *SD = Ty.getStructOrBoundGenericStruct()) {
    LLVM_DEBUG(llvm::dbgs() << "        Found a struct!\n");
    createNextLevelChildrenForStruct(Tree, context, SD);
    return;
  }

  auto TT = Ty.getAs<TupleType>();
  if (!TT) {
    LLVM_DEBUG(llvm::dbgs() << "        Did not find a tuple or struct, "
               "bailing!\n");
    return;
  }

  LLVM_DEBUG(llvm::dbgs() << "        Found a tuple.");
  createNextLevelChildrenForTuple(Tree, TT);
}

SingleValueInstruction *
ProjectionTreeNode::
createAggregate(SILBuilder &B, SILLocation Loc, ArrayRef<SILValue> Args) const {
  assert(Initialized && "Node must be initialized to create aggregates");

  SILType Ty = getType();

  if (Ty.getStructOrBoundGenericStruct()) {
    return B.createStruct(Loc, Ty, Args);
  }

  if (Ty.is<TupleType>()) {
    return B.createTuple(Loc, Ty, Args);
  }

  llvm_unreachable("Unhandled type");
}

class ProjectionTreeNode::NewAggregateBuilder {
  ProjectionTreeNode *Node;
  SILBuilder &Builder;
  SILLocation Loc;
  llvm::SmallVector<SILValue, 8> Values;

  // Did this aggregate already create an aggregate and thus is "invalidated".
  bool Invalidated;

public:
  NewAggregateBuilder(ProjectionTreeNode *N, SILBuilder &B, SILLocation L)
    : Node(N), Builder(B), Loc(L), Values(), Invalidated(false) {
    assert(N->Initialized && "N must be initialized since we are mapping Node "
           "Children -> SILValues");

    // Initialize the Values array with empty SILValues.
    for (unsigned Child : N->ChildProjections) {
      (void)Child;
      Values.push_back(SILValue());
    }
  }

  bool isInvalidated() const { return Invalidated; }

  /// If all SILValues have been set, we are complete.
  bool isComplete() const {
    return std::all_of(Values.begin(), Values.end(), [](SILValue V) -> bool {
      return V;
    });
  }

  SingleValueInstruction *createInstruction() const {
    assert(isComplete() && "Cannot create instruction until the aggregate is "
           "complete");
    assert(!Invalidated && "Must not be invalidated to create an instruction");
    const_cast<NewAggregateBuilder *>(this)->Invalidated = true;
    return Node->createAggregate(Builder, Loc, Values);
  }

  void setValueForChild(ProjectionTreeNode *Child, SILValue V) {
    assert(!Invalidated && "Must not be invalidated to set value for child");
    Values[Child->Proj.value().getIndex()] = V;
  }
};

namespace {

using NewAggregateBuilder = ProjectionTreeNode::NewAggregateBuilder;

/// A wrapper around a MapVector with generalized operations on the map.
///
/// TODO: Replace this with a simple RPOT and use GraphUtils. Since we do not
/// look through enums or classes, in the current type system it should not be
/// possible to have a cycle implying that a RPOT should be fine.
class NewAggregateBuilderMap {
  SILBuilder &Builder;
  SILLocation Loc;
  llvm::MapVector<ProjectionTreeNode *, NewAggregateBuilder> NodeBuilderMap;

public:

  NewAggregateBuilderMap(SILBuilder &B, SILLocation Loc)
    : Builder(B), Loc(Loc), NodeBuilderMap() {}

  /// Get the NewAggregateBuilder associated with Node or if none is created,
  /// create one for Node.
  NewAggregateBuilder &getBuilder(ProjectionTreeNode *Node) {
    auto I = NodeBuilderMap.find(Node);
    if (I != NodeBuilderMap.end()) {
      return I->second;
    } else {
      auto AggIt = NodeBuilderMap.insert({Node, NewAggregateBuilder(Node, Builder,
                                                                 Loc)});
      return AggIt.first->second;
    }
  }

  /// Get the NewAggregateBuilder associated with Node. Assert on failure.
  NewAggregateBuilder &get(ProjectionTreeNode *Node) {
    auto It = NodeBuilderMap.find(Node);
    assert(It != NodeBuilderMap.end() && "Every item in the worklist should have "
           "an NewAggregateBuilder associated with it");
    return It->second;
  }

  bool isComplete(ProjectionTreeNode *Node) {
    return get(Node).isComplete();
  }

  bool isInvalidated(ProjectionTreeNode *Node) {
    return get(Node).isInvalidated();
  }

  ProjectionTreeNode *
  getNextValidNode(llvm::SmallVectorImpl<ProjectionTreeNode *> &Worklist,
                   bool CheckForDeadLock=false);
};

} // end anonymous namespace

//===----------------------------------------------------------------------===//
//                               ProjectionTree
//===----------------------------------------------------------------------===//

ProjectionTree::ProjectionTree(
    SILModule &Mod, SILType BaseTy,
    llvm::SpecificBumpPtrAllocator<ProjectionTreeNode> &Allocator)
    : Mod(&Mod), Allocator(&Allocator) {
  LLVM_DEBUG(llvm::dbgs() << "Constructing Projection Tree For : " << BaseTy
                          << "\n");

  // Create the root node of the tree with our base type.
  createRoot(BaseTy);

  // Create the rest of the type tree lazily based on uses.
}

ProjectionTree::~ProjectionTree() {
  // Do nothing !. Eventually the all the projection tree nodes will be freed
  // when the BPA allocator is free.
}

SILValue
ProjectionTree::computeExplodedArgumentValueInner(SILBuilder &Builder,
                                                  SILLocation Loc,
                                                  ProjectionTreeNode *Node,
                                                  LeafValueMapTy &LeafValues) {
  // Use the child node value if the child is alive.
  if (Node->ChildProjections.empty()) {
    auto Iter = LeafValues.find(Node->getIndex());
    if (Iter != LeafValues.end())
      return Iter->second;
    // Return undef for dead node.
    return SILUndef::get(Node->getType(), Builder.getFunction());
  }

  // This is an aggregate node, construct its value from its children
  // recursively. 
  //
  // NOTE: We do not expect to have too many levels of nesting, so
  // recursion should be fine.
  llvm::SmallVector<SILValue, 8> ChildValues;
  for (unsigned ChildIdx : Node->ChildProjections) {
    ProjectionTreeNode *Child = getNode(ChildIdx);
    ChildValues.push_back(computeExplodedArgumentValueInner(Builder, Loc, Child,
                                                            LeafValues));
  }

  // Form and return the aggregate.
  NullablePtr<SingleValueInstruction> AI =
      Projection::createAggFromFirstLevelProjections(Builder, Loc,
                                                     Node->getType(),
                                                     ChildValues);

  assert(AI.get() && "Failed to get a part of value");
  return SILValue(AI.get());
}

SILValue
ProjectionTree::computeExplodedArgumentValue(
                            SILBuilder &Builder, SILLocation Loc,
                            llvm::SmallVector<SILValue, 8> &LeafValues) {
  // Construct the leaf index to leaf value map.
  llvm::DenseMap<unsigned, SILValue> LeafIndexToValue;
  for (unsigned i = 0; i < LeafValues.size(); ++i) {
    LeafIndexToValue[LiveLeafIndices[i]] = LeafValues[i];
  }

  // Compute the full root node debug node by walking down the projection tree.
  return computeExplodedArgumentValueInner(Builder, Loc, getRoot(),
                                           LeafIndexToValue);
}

void
ProjectionTree::
computeUsesAndLiveness(SILValue Base) {
  // Propagate liveness and users through the tree.
  llvm::SmallVector<ProjectionTreeNode::ValueNodePair, 32> UseWorklist;
  UseWorklist.push_back({Base, getRoot()});

  // Then until the worklist is empty...
  while (!UseWorklist.empty()) {
    LLVM_DEBUG(llvm::dbgs() << "Current Worklist:\n");
    LLVM_DEBUG(for (auto &T : UseWorklist) {
        llvm::dbgs() << "    Type: " << T.second->getType() << "; Value: ";
        if (T.first) {
          llvm::dbgs() << T.first;
        } else {
          llvm::dbgs() << "<null>\n";
        }
    });

    SILValue Value;
    ProjectionTreeNode *Node;

    // Pop off the top type, value, and node.
    std::tie(Value, Node) = UseWorklist.pop_back_val();

    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << Node->getType() << "\n");

    // If Value is not null, collate all users of Value the appropriate child
    // nodes and add such items to the worklist.
    if (Value) {
      Node->processUsersOfValue(*this, UseWorklist, Value);
    }

    // If this node is live due to a non projection user, propagate down its
    // liveness to its children and its children with an empty value to the
    // worklist so we propagate liveness down to any further descendants.
    if (Node->IsLive) {
      LLVM_DEBUG(llvm::dbgs() << "Node Is Live. Marking Children Live!\n");
      for (unsigned ChildIdx : Node->ChildProjections) {
        ProjectionTreeNode *Child = getNode(ChildIdx);
        Child->IsLive = true;
        LLVM_DEBUG(llvm::dbgs() << "    Marking child live: "
                                << Child->getType() << "\n");
        UseWorklist.push_back({SILValue(), Child});
      }
    }
  }

  // Then setup the leaf list by iterating through our Nodes looking for live
  // leafs. We use a DFS order, always processing the left leafs first so that
  // we match the order in which we will lay out arguments.
  llvm::SmallVector<ProjectionTreeNode *, 8> Worklist;
  Worklist.push_back(getRoot());
  while (!Worklist.empty()) {
    ProjectionTreeNode *Node = Worklist.pop_back_val();

    // If node is not a leaf, add its children to the worklist and continue.
    if (!Node->ChildProjections.empty()) {
      for (unsigned ChildIdx : llvm::reverse(Node->ChildProjections)) {
        Worklist.push_back(getNode(ChildIdx));
      }
      continue;
    }

    // If the node is a leaf and is not a live, continue.
    if (!Node->IsLive)
      continue;

    // Otherwise we have a live leaf, add its index to our LiveLeafIndices list.
    LiveLeafIndices.push_back(Node->getIndex());
  }

#ifndef NDEBUG
  LLVM_DEBUG(llvm::dbgs() << "Final Leafs: \n");
  llvm::SmallVector<SILType, 8> LeafTypes;
  getLiveLeafTypes(LeafTypes);
  for (SILType Leafs : LeafTypes) {
    LLVM_DEBUG(llvm::dbgs() << "    " << Leafs << "\n");
  }
#endif
}

void
ProjectionTree::
createTreeFromValue(SILBuilder &B, SILLocation Loc, SILValue NewBase,
                    llvm::SmallVectorImpl<SILValue> &Leafs) const {
  LLVM_DEBUG(llvm::dbgs() << "Recreating tree from value: " << NewBase);

  using WorklistEntry =
    std::tuple<const ProjectionTreeNode *, SILValue>;
  llvm::SmallVector<WorklistEntry, 32> Worklist;

  // Start our worklist with NewBase and Root.
  Worklist.push_back(std::make_tuple(getRoot(), NewBase));

  // Then until our worklist is clear...
  while (Worklist.size()) {
    // Pop off the top of the list.
    const ProjectionTreeNode *Node = std::get<0>(Worklist.back());
    SILValue V = std::get<1>(Worklist.back());
    Worklist.pop_back();

    LLVM_DEBUG(llvm::dbgs() << "Visiting: " << V->getType() << ": " << V);

    // If we have any children...
    unsigned NumChildren = Node->ChildProjections.size();
    if (NumChildren) {
      LLVM_DEBUG(llvm::dbgs() << "    Not Leaf! Adding children to list.\n");

      // Create projections for each one of them and the child node and
      // projection to the worklist for processing.
      for (unsigned ChildIdx : llvm::reverse(Node->ChildProjections)) {
        const ProjectionTreeNode *ChildNode = getNode(ChildIdx);
        auto I = ChildNode->createProjection(B, Loc, V).get();
        LLVM_DEBUG(llvm::dbgs() << "    Adding Child: " << I->getType() << ": "
                                << *I);
        Worklist.push_back(std::make_tuple(ChildNode, SILValue(I)));
      }
    } else {
      // Otherwise, we have a leaf node. If the leaf node is not alive, do not
      // add it to our leaf list.
      if (!Node->IsLive)
        continue;

      // Otherwise add it to our leaf list.
      LLVM_DEBUG(llvm::dbgs() << "    Is a Leaf! Adding to leaf list\n");
      Leafs.push_back(V);
    }
  }
}

ProjectionTreeNode *
NewAggregateBuilderMap::
getNextValidNode(llvm::SmallVectorImpl<ProjectionTreeNode *> &Worklist,
                 bool CheckForDeadLock) {
  if (Worklist.empty())
    return nullptr;

  ProjectionTreeNode *Node = Worklist.back();

  // If the Node is not complete, then we have reached a dead lock. This should
  // never happen.
  //
  // TODO: Prove this and put the proof here.
  if (CheckForDeadLock && !isComplete(Node)) {
    llvm_unreachable("Algorithm Dead Locked!");
  }

  // This block of code, performs the pop back and also if the Node has been
  // invalidated, skips until we find a non invalidated value.
  while (isInvalidated(Node)) {
    assert(isComplete(Node) && "Invalidated values must be complete");

    // Pop Node off the back of the worklist.
    Worklist.pop_back();

    if (Worklist.empty())
      return nullptr;

    Node = Worklist.back();
  }

  return Node;
}

void
ProjectionTree::
replaceValueUsesWithLeafUses(SILBuilder &Builder, SILLocation Loc,
                             llvm::SmallVectorImpl<SILValue> &Leafs) {
  assert(Leafs.size() == LiveLeafIndices.size() && "Leafs and leaf indices must "
         "equal in size.");

  NewAggregateBuilderMap AggBuilderMap(Builder, Loc);
  llvm::SmallVector<ProjectionTreeNode *, 8> Worklist;

  LLVM_DEBUG(llvm::dbgs() << "Replacing all uses in callee with leafs!\n");

  // For each Leaf we have as input...
  for (unsigned i = 0, e = Leafs.size(); i != e; ++i) {
    SILValue Leaf = Leafs[i];
    ProjectionTreeNode *Node = getNode(LiveLeafIndices[i]);

    LLVM_DEBUG(llvm::dbgs() << "    Visiting leaf: " << Leaf);

    assert(Node->IsLive && "Unexpected dead node in LiveLeafIndices!");

    // Otherwise replace all uses at this level of the tree with uses of the
    // Leaf value.
    LLVM_DEBUG(llvm::dbgs() << "        Replacing operands with leaf!\n");
    for (auto *Op : Node->NonProjUsers) {
      LLVM_DEBUG(llvm::dbgs() << "            User: " << *Op->getUser());
      Op->set(Leaf);
    }

    // Grab the parent of this node.
    ProjectionTreeNode *Parent = Node->getParent(*this);

    // If the parent is dead, continue.
    if (!Parent || !Parent->IsLive) {
      LLVM_DEBUG(llvm::dbgs() << "        Parent is dead... continuing.\n");
      continue;
    }

    LLVM_DEBUG(llvm::dbgs() << "        Parent is alive! Adding to parent "
               "builder\n");

    // Otherwise either create an aggregate builder for the parent or reuse one
    // that has already been created for it.
    AggBuilderMap.getBuilder(Parent).setValueForChild(Node, Leaf);

    LLVM_DEBUG(llvm::dbgs() << "            Is parent complete: "
               << (AggBuilderMap.isComplete(Parent)? "yes" : "no") << "\n");

    // Finally add the parent to the worklist.
    Worklist.push_back(Parent);
  }

  // A utility array to add new Nodes to the list so we maintain while
  // processing the current worklist we maintain only completed items at the end
  // of the list.
  llvm::SmallVector<ProjectionTreeNode *, 8> NewNodes;

  LLVM_DEBUG(llvm::dbgs() << "Processing worklist!\n");

  // Then until we have no work left...
  while (!Worklist.empty()) {
    // Sort the worklist so that complete items are first.
    //
    // TODO: Just change this into a partition method. Should be significantly
    // faster.
    std::sort(Worklist.begin(), Worklist.end(),
              [&AggBuilderMap](ProjectionTreeNode *N1,
                     ProjectionTreeNode *N2) -> bool {
                bool IsComplete1 = AggBuilderMap.isComplete(N1);
                bool IsComplete2 = AggBuilderMap.isComplete(N2);

                // Sort N1 after N2 if N1 is complete and N2 is not. This puts
                // complete items at the end of our list.
                return unsigned(IsComplete1) < unsigned(IsComplete2);
              });

    LLVM_DEBUG(llvm::dbgs() << "    Current Worklist:\n");
#ifndef NDEBUG
    for (auto *_work : Worklist) {
      LLVM_DEBUG(llvm::dbgs() << "        Type: " << _work->getType()
                 << "; Complete: "
                 << (AggBuilderMap.isComplete(_work)? "yes" : "no")
                 << "; Invalidated: "
                 << (AggBuilderMap.isInvalidated(_work)? "yes" : "no") << "\n");
    }
#endif

    // Find the first non invalidated node. If we have all invalidated nodes,
    // this will return nullptr.
    ProjectionTreeNode *Node = AggBuilderMap.getNextValidNode(Worklist, true);

    // Then until we find a node that is not complete...
    while (Node && AggBuilderMap.isComplete(Node)) {
      // Create the aggregate for the current complete Node we are processing...
      SILValue Agg = AggBuilderMap.get(Node).createInstruction();

      // Replace all uses at this level of the tree with uses of the newly
      // constructed aggregate.
      for (auto *Op : Node->NonProjUsers) {
        Op->set(Agg);
      }

      // If this node has a parent and that parent is alive...
      ProjectionTreeNode *Parent = Node->getParentOrNull(*this);
      if (Parent && Parent->IsLive) {
        // Create or lookup the node builder for the parent and associate the
        // newly created aggregate with this node.
        AggBuilderMap.getBuilder(Parent).setValueForChild(Node, SILValue(Agg));
        // Then add the parent to NewNodes to be added to our list.
        NewNodes.push_back(Parent);
      }

      // Grab the next non-invalidated node for the next iteration. If we had
      // all invalidated nodes, this will return nullptr.
      Node = AggBuilderMap.getNextValidNode(Worklist);
    }

    // Copy NewNodes onto the back of our Worklist now that we have finished
    // this iteration.
    std::copy(NewNodes.begin(), NewNodes.end(), std::back_inserter(Worklist));
    NewNodes.clear();
  }
}

void ProjectionTree::getUsers(SmallPtrSetImpl<SILInstruction *> &users) const {
  for (auto *node : ProjectionTreeNodes) {
    for (auto *op : node->getNonProjUsers()) {
      users.insert(op->getUser());
    }
  }
}
