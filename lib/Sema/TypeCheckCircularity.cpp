//===--- TypeCheckCircularity.cpp - Type decl circularity checking --------===//
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
// This file implements an infinitely-sized-type check.
//
//===----------------------------------------------------------------------===//

#include "TypeChecker.h"
#include "swift/Basic/Assertions.h"
#include "swift/Basic/Debug.h"

using namespace swift;

#define DEBUG_TYPE "TypeCheckCircularity"

namespace {

/// The information we track for a type.
class TrackingInfo {
  /// The parent type; either null or a key for an entry in TrackingMap.
  CanType Parent;

  /// The member of the parent type that lead to this type, or null
  /// for a tuple element; and whether the type is currently being
  /// expanded.
  llvm::PointerIntPair<ValueDecl *, 1, bool> ParentMemberAndIsBeingExpanded;

public:
  TrackingInfo(CanType parent, ValueDecl *parentMember)
    : Parent(parent), ParentMemberAndIsBeingExpanded(parentMember, false) {}

  CanType getParentType() const {
    return Parent;
  }

  ValueDecl *getParentMember() const {
    return ParentMemberAndIsBeingExpanded.getPointer();
  }

  bool isBeingExpanded() const {
    return ParentMemberAndIsBeingExpanded.getInt();
  }

  void setBeingExpanded(bool isBeingExpanded) {
    ParentMemberAndIsBeingExpanded.setInt(isBeingExpanded);
  }
};

struct WorkItem {
  enum : unsigned {
    /// A special depth we use to say that this work item
    /// is to *finish* expanding the target type.
    FinishExpandingType = ~0U
  };

  unsigned Depth;
  CanType Type;

  WorkItem(unsigned depth, CanType type)
    : Depth(depth), Type(type) {}
};

struct PathElement {
  ValueDecl *Member; // Or nullptr for a tuple element type.
  size_t TupleIndex;
  Type Ty;

  SWIFT_DEBUG_DUMP;
  void print(llvm::raw_ostream &out) const;
};

class Path {
  SmallVector<PathElement, 8> Elements;

public:
  void push_back(const PathElement &elt) { Elements.push_back(elt); }

  bool empty() const { return Elements.empty(); }
  size_t size() const { return Elements.size(); }
  const PathElement &operator[](size_t index) const { return Elements[index]; }
  const PathElement &back() const { return Elements.back(); }

  SWIFT_DEBUG_DUMP;
  void printCycle(llvm::raw_ostream &out, size_t cycleIndex) const;
  void printInfinite(llvm::raw_ostream &out) const;

private:
  void printSegment(llvm::raw_ostream &out, size_t begin, size_t end,
                    size_t maxContext, bool printFirstType = true) const;
};

/// A helper class for performing a circularity check.
class CircularityChecker final {
  /// The original type declaration we're starting with.
  NominalTypeDecl *OriginalDecl;

  /// The maximum circularity depth.
  unsigned MaxDepth;

  llvm::DenseMap<CanType, TrackingInfo> TrackingMap;
  SmallVector<WorkItem, 8> Workstack;

public:
  CircularityChecker(NominalTypeDecl *typeDecl)
      : OriginalDecl(typeDecl),
        MaxDepth(typeDecl->getASTContext().LangOpts.MaxCircularityDepth) {}

  void run();

private:
  Type getOriginalType() const {
    return OriginalDecl->getDeclaredInterfaceType();
  }

  bool expandType(CanType type, unsigned depth);
  bool expandTuple(CanTupleType type, unsigned depth);
  bool expandNominal(CanType type, NominalTypeDecl *D, unsigned depth);
  bool expandStruct(CanType type, StructDecl *S, unsigned depth);
  bool expandEnum(CanType type, EnumDecl *E, unsigned depth);

  bool addMember(CanType parentType, ValueDecl *member, Type memberType,
                 unsigned parentDepth);

  void startExpandingType(CanType type) {
    auto it = TrackingMap.find(type);
    assert(it != TrackingMap.end());

    // Set the IsBeginExpanded flag.
    assert(!it->second.isBeingExpanded() && "already expanding type");
    it->second.setBeingExpanded(true);

    // Push a work item to clear that flag.
    pushFinishExpandingTypeWorkItem(type);
  }

  void finishExpandingType(CanType type) {
    auto it = TrackingMap.find(type);
    assert(it != TrackingMap.end());

    // Clear the IsBeginExpanded flag.
    assert(it->second.isBeingExpanded() && "not already expanding type");
    it->second.setBeingExpanded(false);
  }

  void pushFinishExpandingTypeWorkItem(CanType type) {
    Workstack.emplace_back(WorkItem::FinishExpandingType, type);
  }

  void pushExpandTypeWorkItem(CanType type, unsigned depth) {
    assert(depth != WorkItem::FinishExpandingType);
    Workstack.emplace_back(depth, type);
  }

  bool diagnoseCircularity(CanType parentType, ValueDecl *member,
                           CanType memberType);

  bool diagnoseInfiniteRecursion(CanType parentType, ValueDecl *member,
                                 CanType memberType);

  void diagnoseNonWellFoundedEnum(EnumDecl *E);

  void addPathElementsTo(Path &path, CanType type);
  void addPathElement(Path &path, ValueDecl *member, CanType memberType);

  Path buildPath(CanType parentType, ValueDecl *member, CanType memberType);
};

} // end anonymous namespace

void TypeChecker::checkDeclCircularity(NominalTypeDecl *decl) {
  CircularityChecker(decl).run();
}

/// The main routine for performing circularity checks.
void CircularityChecker::run() {
  auto type = getOriginalType()->getCanonicalType();

  // Prime the tracking map.
  TrackingMap.insert({type, TrackingInfo(CanType(), nullptr)});

  // Recurse into the top-level nominal type.
  expandNominal(type, OriginalDecl, 0);

  // Execute the workstack.
  while (!Workstack.empty()) {
    auto item = Workstack.pop_back_val();
    if (item.Depth == WorkItem::FinishExpandingType) {
      finishExpandingType(item.Type);
    } else if (expandType(item.Type, item.Depth)) {
      return;
    }
  }
}

/// Visit a type and try to expand it one level.
///
/// \return true if a problem was found and all further processing
///   should be aborted.
bool CircularityChecker::expandType(CanType type, unsigned depth) {
  if (auto D = type.getAnyNominal()) {
    return expandNominal(type, D, depth);
  } else if (auto tuple = dyn_cast<TupleType>(type)) {
    return expandTuple(tuple, depth);
  } else {
    return false;
  }
}

/// Visit a tuple type and try to expand it one level.
bool CircularityChecker::expandTuple(CanTupleType tupleType, unsigned depth) {
  LLVM_DEBUG(llvm::dbgs() << std::string(depth, ' ') << "expanding tuple "
                          << tupleType << "\n";);
  startExpandingType(tupleType);

  for (auto eltType : tupleType.getElementTypes()) {
    if (addMember(tupleType, nullptr, eltType, depth))
      return true;
  }

  return false;
}

/// Visit a nominal type and try to expand it one level.
bool CircularityChecker::expandNominal(CanType type, NominalTypeDecl *D,
                                       unsigned depth) {
  LLVM_DEBUG(llvm::dbgs() << std::string(depth, ' ') << "expanding nominal "
                          << type << "\n";);
  if (auto S = dyn_cast<StructDecl>(D)) {
    return expandStruct(type, S, depth);
  } else if (auto E = dyn_cast<EnumDecl>(D)) {
    return expandEnum(type, E, depth);
  } else {
    // Other nominal types are representational leaves.
    return false;
  }
}

/// Visit a struct type and try to expand it one level.
bool CircularityChecker::expandStruct(CanType type, StructDecl *S,
                                      unsigned depth) {
  startExpandingType(type);

  auto subMap = type->getContextSubstitutionMap(
      S->getModuleContext(), S);

  for (auto field: S->getStoredProperties()) {
    auto fieldType =field->getValueInterfaceType().subst(subMap);
    if (addMember(type, field, fieldType, depth))
      return true;
  }

  return false;
}

/// Visit an enum type and try to expand it one level.
bool CircularityChecker::expandEnum(CanType type, EnumDecl *E,
                                   unsigned depth) {
  // Indirect enums are representational leaves.
  if (E->isIndirect()) {
    // Diagnose whether the enum is non-well-founded before bailing
    diagnoseNonWellFoundedEnum(E);
    return false;
  }

  startExpandingType(type);

  auto subMap = type->getContextSubstitutionMap(
      E->getModuleContext(), E);

  for (auto elt: E->getAllElements()) {
    // Indirect elements are representational leaves.
    if (elt->isIndirect())
      continue;

    if (!elt->hasAssociatedValues())
      continue;

    auto eltType = elt->getArgumentInterfaceType().subst(subMap);
    if (addMember(type, elt, eltType, depth))
      return true;
  }
  diagnoseNonWellFoundedEnum(E);

  return false;
}

bool CircularityChecker::addMember(CanType parentType, ValueDecl *member,
                                   Type memberNCType, unsigned parentDepth) {
  auto memberType = memberNCType->getCanonicalType();

  unsigned depth = parentDepth + 1;
  if (depth > MaxDepth) {
    return diagnoseInfiniteRecursion(parentType, member, memberType);
  }

  // Fast path: if the type isn't some sort of interesting type,
  // just ignore it.

  if (isa<TupleType>(memberType)) {
    // Ok, visit tuples.
  } else if (memberType.getStructOrBoundGenericStruct()) {
    // Ok, visit structs.
    // TODO: skip non-generic types in different modules?
  } else if (auto E = memberType.getEnumOrBoundGenericEnum()) {
    // Ok, visit non-indirect enums.
    if (E->isIndirect()) return false;
    // TODO: skip non-generic types in different modules?
  } else {
    // Everything else is a representational leaf.
    return false;
  }

  // Try to start tracking the type.
  auto insertion = TrackingMap.insert({memberType,
                                       TrackingInfo(parentType, member)});

  // If it's not already there, add an item to recurse into it.
  if (insertion.second) {
    pushExpandTypeWorkItem(memberType, depth);
    return false;
  }

  // Otherwise, we've already enqueued it.  If we're not currently
  // expanding it, there's no circularity to worry about.
  auto &info = insertion.first->second;
  if (!info.isBeingExpanded())
    return false;

  return diagnoseCircularity(parentType, member, memberType);
}

static size_t findCycleIndex(const Path &path) {
  for (auto index : IntRange<size_t>(0, path.size() - 1)) {
    if (path[index].Ty->isEqual(path.back().Ty))
      return index;
  }
  llvm_unreachable("didn't find cycle in path");
}

static Type getMemberStorageInterfaceType(ValueDecl *member) {
  if (auto elt = dyn_cast<EnumElementDecl>(member)) {
    return elt->getArgumentInterfaceType();
  } else {
    return member->getInterfaceType();
  }
}

static bool isNonDependentField(const PathElement &elt) {
  if (!elt.Member) return false;
  return !getMemberStorageInterfaceType(elt.Member)->hasTypeParameter();
}

void LLVM_ATTRIBUTE_USED Path::dump() const {
  auto &out = llvm::errs();
  printSegment(out, 0, size(), size());
  out << '\n';
}

/// Prints:
///   TypeA -> (a: TypeB) -> (b: TypeB) -> (c: CycleType)
///         -> (d: TypeD) -> (e: CycleType)
void Path::printCycle(llvm::raw_ostream &out, size_t cycleIndex) const {
  // If the cycle goes to Self or the member type, print the
  // path in one segment starting from the field type.
  if (cycleIndex <= 1) {
    printSegment(out, 1, size(), 3);

  // Otherwise, print the path in two segments.
  } else {
    printSegment(out, 1, cycleIndex + 1, 2);
    printSegment(out, cycleIndex, size(), 2, false);
  }
}

/// Prints:
///   TypeA -> (x: TypeB) -> (y: TypeB) -> (z: TypeC) -> ...
void Path::printInfinite(llvm::raw_ostream &out) const {
  printSegment(out, 1, std::min(size(), size_t(7)), 7);
  out << " -> ...";
}

/// Prints:
///   [TypeA] -> (a: TypeB) -> (b: TypeB) -> (c: TypeC)
/// If the path is too long, elides the middle with '-> ...'.
void Path::printSegment(llvm::raw_ostream &out, size_t begin, size_t end,
                        size_t maxContext, bool printFirstType) const {
  if (printFirstType) {
    out << Elements[begin].Ty;
  }

  size_t numElements = end - begin;
  if (numElements > maxContext * 2) {
    for (size_t i = begin + 1; i != begin + maxContext + 1; ++i)
      Elements[i].print(out);
    out << " -> ... ";
    for (size_t i = end - maxContext; i != end; ++i)
      Elements[i].print(out);
  } else {
    for (size_t i = begin + 1; i != end; ++i) {
      Elements[i].print(out);
    }
  }
}

void LLVM_ATTRIBUTE_USED PathElement::dump() const {
  auto &out = llvm::errs();
  print(out);
  out << '\n';
}

/// Prints:
///   -> (a: TypeA)
void PathElement::print(llvm::raw_ostream &out) const {
  out << " -> (";
  if (Member) {
    auto name = Member->getName();
    if (name) {
      out << name;
    } else {
      out << "<anonymous>";
    }
  } else {
    out << '.' << TupleIndex;
  }
  out << ": " << Ty << ')';
}

/// Recreate a non-canonical type path.
Path CircularityChecker::buildPath(CanType parentType, ValueDecl *member,
                                   CanType memberType) {
  Path path;
  addPathElementsTo(path, parentType);
  addPathElement(path, member, memberType);
  return path;
}

/// Recreate a non-canonical path that leads to the target type.
void CircularityChecker::addPathElementsTo(Path &path, CanType targetType) {
  auto it = TrackingMap.find(targetType);
  assert(it != TrackingMap.end() && "no entry in tracking map?");

  CanType canParentType = it->second.getParentType();
  if (!canParentType) {
    path.push_back({ nullptr, 0, getOriginalType()});
    return;
  }

  addPathElementsTo(path, canParentType);
  addPathElement(path, it->second.getParentMember(), targetType);
}

/// Add a non-canonical path element to a path.
void CircularityChecker::addPathElement(Path &path, ValueDecl *member,
                                        CanType canMemberType) {
  assert(!path.empty());

  Type parentType = path.back().Ty;

  PathElement elt;

  if (member) {
    elt.Member = member;
    elt.TupleIndex = 0;

    Type memberIfaceType = getMemberStorageInterfaceType(member);
    elt.Ty = parentType->getTypeOfMember(member->getModuleContext(), member,
                                         memberIfaceType);

  } else {
    auto tupleType = parentType->castTo<TupleType>();
    for (auto index : indices(tupleType->getElementTypes())) {
      auto eltType = tupleType->getElementType(index);
      if (eltType->getCanonicalType() == canMemberType) {
        elt.Member = nullptr;
        elt.TupleIndex = index;
        elt.Ty = eltType;
        break;
      }
    }
    assert(elt.Ty && "didn't find matching element of tuple");
  }

  // We shouldn't print reference storage types here.
  if (auto ref = elt.Ty->getAs<ReferenceStorageType>()) {
    elt.Ty = ref->getReferentType();
  }

  // Strip outer parens from the type.  (This is especially common with
  // enum elements.)
  if (auto parens = dyn_cast<ParenType>(elt.Ty.getPointer())) {
    elt.Ty = parens->getSinglyDesugaredType();
  }

  path.push_back(elt);
}

/// Diagnose a circularity.
///
/// \returns always true
bool CircularityChecker::diagnoseCircularity(CanType parentType,
                                             ValueDecl *member,
                                             CanType memberType) {
  auto path = buildPath(parentType, member, memberType);

  // Find the index that the cycle leads back to.
  auto cycleIndex = findCycleIndex(path);

  // If the path to the cycle passes through a field (other than the one
  // directly declared on this type) that does not depend on the original
  // declaration in any way, then the type that contains that field should
  // be responsible for reporting the cycle.
  // TODO: we can also suppress this if the cycle would exist independently.
  for (size_t i = 2; i < cycleIndex + 1; ++i) {
    if (isNonDependentField(path[i]))
      return true;
  }

  auto baseType = path[0].Ty;
  if (cycleIndex != 0) {
    OriginalDecl->diagnose(diag::unsupported_infinitely_sized_type, baseType);
  } else if (isa<StructDecl>(OriginalDecl)) {
    path[1].Member->diagnose(diag::unsupported_recursive_struct, baseType);
  } else if (isa<EnumDecl>(OriginalDecl)) {
    OriginalDecl->diagnose(diag::recursive_enum_not_indirect, baseType)
        .fixItInsert(OriginalDecl->getStartLoc(), "indirect ");
  } else {
    llvm_unreachable("what kind of entity was this?");
  }

  // Add a note about the path we found unless it's completely trivial.
  if (path.size() > 2) {
    llvm::SmallString<128> pathString; {
      llvm::raw_svector_ostream out(pathString);
      path.printCycle(out, cycleIndex);
    }
    path[1].Member->diagnose(diag::note_type_cycle_starts_here, pathString);
  } else if (isa<EnumDecl>(OriginalDecl)) {
    path[1].Member->diagnose(diag::note_recursive_enum_case_here);
  }

  return true;
}

bool CircularityChecker::diagnoseInfiniteRecursion(CanType parentType,
                                                   ValueDecl *member,
                                                   CanType memberType) {
  auto path = buildPath(parentType, member, memberType);

  // If the path passes through a field (other than the one directly
  // declared on this type) that does not depend on the original
  // declaration in any way, then the type that contains that field should
  // be responsible for reporting the cycle.
  // Applying this heuristic only makes sense if we're assuming that
  // it really is an infinite expansion.
  for (size_t i = 2, e = path.size(); i < e; ++i) {
    if (isNonDependentField(path[i]))
      return true;
  }

  auto baseType = path[0].Ty;
  OriginalDecl->diagnose(diag::unsupported_infinitely_sized_type, baseType);

  // Add a note about the start of the path.
  llvm::SmallString<128> pathString; {
    llvm::raw_svector_ostream out(pathString);
    path.printInfinite(out);
  }

  path[1].Member->diagnose(diag::note_type_cycle_starts_here, pathString);

  return true;
}

/// Show a warning if all cases of the given enum are recursive,
/// making it impossible to be instantiated. Such an enum is 'non-well-founded'.
/// The outcome of this method is irrelevant.
void CircularityChecker::diagnoseNonWellFoundedEnum(EnumDecl *E) {

  auto containsType = [](TupleType *tuple, Type E) -> bool {
    for (auto type: tuple->getElementTypes()) {
      if (type->isEqual(E))
        return true;
    }
    return false;
  };

  auto isNonWellFounded = [&]() -> bool {
    auto elts = E->getAllElements();
    if (elts.empty())
      return false;

    for (auto elt: elts) {
      if (!elt->isIndirect() && !E->isIndirect())
        return false;

      auto argTy = elt->getArgumentInterfaceType();
      if (!argTy)
        return false;

      if (auto tuple = argTy->getAs<TupleType>()) {
        if (!containsType(tuple, E->getSelfInterfaceType()))
          return false;
      } else if (auto paren = dyn_cast<ParenType>(argTy.getPointer())) {
        if (!E->getSelfInterfaceType()->isEqual(paren->getUnderlyingType()))
          return false;
      }
    }
    return true;
  };

  if (isNonWellFounded())
    E->getASTContext().Diags.diagnose(E, diag::enum_non_well_founded);
}
